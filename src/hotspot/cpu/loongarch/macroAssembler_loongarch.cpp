/*
 * Copyright (c) 1997, 2014, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2017, 2023, Loongson Technology. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "asm/assembler.hpp"
#include "asm/assembler.inline.hpp"
#include "asm/macroAssembler.inline.hpp"
#include "compiler/disassembler.hpp"
#include "compiler/oopMap.hpp"
#include "gc/shared/barrierSet.hpp"
#include "gc/shared/barrierSetAssembler.hpp"
#include "gc/shared/collectedHeap.inline.hpp"
#include "interpreter/bytecodeHistogram.hpp"
#include "interpreter/interpreter.hpp"
#include "jvm.h"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "nativeInst_loongarch.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/klass.inline.hpp"
#include "prims/methodHandles.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/jniHandles.inline.hpp"
#include "runtime/objectMonitor.hpp"
#include "runtime/os.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/safepointMechanism.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubRoutines.hpp"
#include "utilities/macros.hpp"

#ifdef COMPILER2
#include "opto/compile.hpp"
#include "opto/output.hpp"
#endif

#if INCLUDE_ZGC
#include "gc/z/zThreadLocalData.hpp"
#endif

// Implementation of MacroAssembler

void MacroAssembler::pd_patch_instruction(address branch, address target, const char* file, int line) {
  jint& stub_inst = *(jint*)branch;
  jint *pc = (jint *)branch;

  if (high(stub_inst, 7) == pcaddu18i_op) {
    // far:
    //   pcaddu18i reg, si20
    //   jirl  r0, reg, si18

    assert(high(pc[1], 6) == jirl_op, "Not a branch label patch");
    jlong offs = target - branch;
    CodeBuffer cb(branch, 2 * BytesPerInstWord);
    MacroAssembler masm(&cb);
    if (reachable_from_branch_short(offs)) {
      // convert far to short
#define __ masm.
      __ b(target);
      __ nop();
#undef __
    } else {
      masm.patchable_jump_far(R0, offs);
    }
    return;
  } else if (high(stub_inst, 7) == pcaddi_op) {
    // see MacroAssembler::set_last_Java_frame:
    //   pcaddi reg, si20

    jint offs = (target - branch) >> 2;
    guarantee(is_simm(offs, 20), "Not signed 20-bit offset");
    CodeBuffer cb(branch, 1 * BytesPerInstWord);
    MacroAssembler masm(&cb);
    masm.pcaddi(as_Register(low(stub_inst, 5)), offs);
    return;
  } else if (high(stub_inst, 7) == pcaddu12i_op) {
    // pc-relative
    jlong offs = target - branch;
    guarantee(is_simm(offs, 32), "Not signed 32-bit offset");
    jint si12, si20;
    jint& stub_instNext = *(jint*)(branch+4);
    split_simm32(offs, si12, si20);
    CodeBuffer cb(branch, 2 * BytesPerInstWord);
    MacroAssembler masm(&cb);
    masm.pcaddu12i(as_Register(low(stub_inst, 5)), si20);
    masm.addi_d(as_Register(low((stub_instNext), 5)), as_Register(low((stub_instNext) >> 5, 5)), si12);
    return;
  } else if (high(stub_inst, 7) == lu12i_w_op) {
    // long call (absolute)
    CodeBuffer cb(branch, 3 * BytesPerInstWord);
    MacroAssembler masm(&cb);
    masm.call_long(target);
    return;
  }

  stub_inst = patched_branch(target - branch, stub_inst, 0);
}

bool MacroAssembler::reachable_from_branch_short(jlong offs) {
  if (ForceUnreachable) {
    return false;
  }
  return is_simm(offs >> 2, 26);
}

void MacroAssembler::patchable_jump_far(Register ra, jlong offs) {
  jint si18, si20;
  guarantee(is_simm(offs, 38), "Not signed 38-bit offset");
  split_simm38(offs, si18, si20);
  pcaddu18i(AT, si20);
  jirl(ra, AT, si18);
}

void MacroAssembler::patchable_jump(address target, bool force_patchable) {
  assert(ReservedCodeCacheSize < 4*G, "branch out of range");
  assert(CodeCache::find_blob(target) != nullptr,
         "destination of jump not found in code cache");
  if (force_patchable || patchable_branches()) {
    jlong offs = target - pc();
    if (reachable_from_branch_short(offs)) { // Short jump
      b(offset26(target));
      nop();
    } else {                                 // Far jump
      patchable_jump_far(R0, offs);
    }
  } else {                                   // Real short jump
    b(offset26(target));
  }
}

void MacroAssembler::patchable_call(address target, address call_site) {
  jlong offs = target - (call_site ? call_site : pc());
  if (reachable_from_branch_short(offs - BytesPerInstWord)) { // Short call
    nop();
    bl((offs - BytesPerInstWord) >> 2);
  } else {                                                    // Far call
    patchable_jump_far(RA, offs);
  }
}

// Maybe emit a call via a trampoline. If the code cache is small
// trampolines won't be emitted.
address MacroAssembler::trampoline_call(AddressLiteral entry, CodeBuffer* cbuf) {
  assert(entry.rspec().type() == relocInfo::runtime_call_type ||
         entry.rspec().type() == relocInfo::opt_virtual_call_type ||
         entry.rspec().type() == relocInfo::static_call_type ||
         entry.rspec().type() == relocInfo::virtual_call_type, "wrong reloc type");

  address target = entry.target();

  // We need a trampoline if branches are far.
  if (far_branches()) {
    if (!in_scratch_emit_size()) {
      address stub = emit_trampoline_stub(offset(), target);
      if (stub == nullptr) {
        postcond(pc() == badAddress);
        return nullptr; // CodeCache is full
      }
    }
    target = pc();
  }

  if (cbuf != nullptr) { cbuf->set_insts_mark(); }
  relocate(entry.rspec());
  bl(target);

  // just need to return a non-null address
  postcond(pc() != badAddress);
  return pc();
}

// Emit a trampoline stub for a call to a target which is too far away.
//
// code sequences:
//
// call-site:
//   branch-and-link to <destination> or <trampoline stub>
//
// Related trampoline stub for this call site in the stub section:
//   load the call target from the constant pool
//   branch (RA still points to the call site above)

address MacroAssembler::emit_trampoline_stub(int insts_call_instruction_offset,
                                             address dest) {
  // Start the stub
  address stub = start_a_stub(NativeInstruction::nop_instruction_size
                   + NativeCallTrampolineStub::instruction_size);
  if (stub == nullptr) {
    return nullptr;  // CodeBuffer::expand failed
  }

  // Create a trampoline stub relocation which relates this trampoline stub
  // with the call instruction at insts_call_instruction_offset in the
  // instructions code-section.
  align(wordSize);
  relocate(trampoline_stub_Relocation::spec(code()->insts()->start()
                                            + insts_call_instruction_offset));
  const int stub_start_offset = offset();

  // Now, create the trampoline stub's code:
  // - load the call
  // - call
  pcaddi(AT, 0);
  ld_d(AT, AT, 16);
  jr(AT);
  nop();  //align
  assert(offset() - stub_start_offset == NativeCallTrampolineStub::data_offset,
         "should be");
  emit_int64((int64_t)dest);

  const address stub_start_addr = addr_at(stub_start_offset);

  NativeInstruction* ni = nativeInstruction_at(stub_start_addr);
  assert(ni->is_NativeCallTrampolineStub_at(), "doesn't look like a trampoline");

  end_a_stub();
  return stub_start_addr;
}

void MacroAssembler::beq_far(Register rs, Register rt, address entry) {
  if (is_simm16((entry - pc()) >> 2)) { // Short jump
    beq(rs, rt, offset16(entry));
  } else {                              // Far jump
    Label not_jump;
    bne(rs, rt, not_jump);
    b_far(entry);
    bind(not_jump);
  }
}

void MacroAssembler::beq_far(Register rs, Register rt, Label& L) {
  if (L.is_bound()) {
    beq_far(rs, rt, target(L));
  } else {
    Label not_jump;
    bne(rs, rt, not_jump);
    b_far(L);
    bind(not_jump);
  }
}

void MacroAssembler::bne_far(Register rs, Register rt, address entry) {
  if (is_simm16((entry - pc()) >> 2)) { // Short jump
    bne(rs, rt, offset16(entry));
  } else {                              // Far jump
    Label not_jump;
    beq(rs, rt, not_jump);
    b_far(entry);
    bind(not_jump);
  }
}

void MacroAssembler::bne_far(Register rs, Register rt, Label& L) {
  if (L.is_bound()) {
    bne_far(rs, rt, target(L));
  } else {
    Label not_jump;
    beq(rs, rt, not_jump);
    b_far(L);
    bind(not_jump);
  }
}

void MacroAssembler::blt_far(Register rs, Register rt, address entry, bool is_signed) {
  if (is_simm16((entry - pc()) >> 2)) { // Short jump
    if (is_signed) {
      blt(rs, rt, offset16(entry));
    } else {
      bltu(rs, rt, offset16(entry));
    }
  } else {                              // Far jump
    Label not_jump;
    if (is_signed) {
      bge(rs, rt, not_jump);
    } else {
      bgeu(rs, rt, not_jump);
    }
    b_far(entry);
    bind(not_jump);
  }
}

void MacroAssembler::blt_far(Register rs, Register rt, Label& L, bool is_signed) {
  if (L.is_bound()) {
    blt_far(rs, rt, target(L), is_signed);
  } else {
    Label not_jump;
    if (is_signed) {
      bge(rs, rt, not_jump);
    } else {
      bgeu(rs, rt, not_jump);
    }
    b_far(L);
    bind(not_jump);
  }
}

void MacroAssembler::bge_far(Register rs, Register rt, address entry, bool is_signed) {
  if (is_simm16((entry - pc()) >> 2)) { // Short jump
    if (is_signed) {
      bge(rs, rt, offset16(entry));
    } else {
      bgeu(rs, rt, offset16(entry));
    }
  } else {                              // Far jump
    Label not_jump;
    if (is_signed) {
      blt(rs, rt, not_jump);
    } else {
      bltu(rs, rt, not_jump);
    }
    b_far(entry);
    bind(not_jump);
  }
}

void MacroAssembler::bge_far(Register rs, Register rt, Label& L, bool is_signed) {
  if (L.is_bound()) {
    bge_far(rs, rt, target(L), is_signed);
  } else {
    Label not_jump;
    if (is_signed) {
      blt(rs, rt, not_jump);
    } else {
      bltu(rs, rt, not_jump);
    }
    b_far(L);
    bind(not_jump);
  }
}

void MacroAssembler::b_far(Label& L) {
  if (L.is_bound()) {
    b_far(target(L));
  } else {
    L.add_patch_at(code(), locator());
    if (ForceUnreachable) {
      patchable_jump_far(R0, 0);
    } else {
      b(0);
    }
  }
}

void MacroAssembler::b_far(address entry) {
  jlong offs = entry - pc();
  if (reachable_from_branch_short(offs)) { // Short jump
    b(offset26(entry));
  } else {                                 // Far jump
    patchable_jump_far(R0, offs);
  }
}

// tmp_reg1 and tmp_reg2 should be saved outside of atomic_inc32 (caller saved).
void MacroAssembler::atomic_inc32(address counter_addr, int inc, Register tmp_reg1, Register tmp_reg2) {
  li(tmp_reg1, inc);
  li(tmp_reg2, counter_addr);
  amadd_w(R0, tmp_reg1, tmp_reg2);
}

// Writes to stack successive pages until offset reached to check for
// stack overflow + shadow pages.  This clobbers tmp.
void MacroAssembler::bang_stack_size(Register size, Register tmp) {
  assert_different_registers(tmp, size, AT);
  move(tmp, SP);
  // Bang stack for total size given plus shadow page size.
  // Bang one page at a time because large size can bang beyond yellow and
  // red zones.
  Label loop;
  li(AT, (int)os::vm_page_size());
  bind(loop);
  sub_d(tmp, tmp, AT);
  sub_d(size, size, AT);
  st_d(size, tmp, 0);
  blt(R0, size, loop);

  // Bang down shadow pages too.
  // At this point, (tmp-0) is the last address touched, so don't
  // touch it again.  (It was touched as (tmp-pagesize) but then tmp
  // was post-decremented.)  Skip this address by starting at i=1, and
  // touch a few more pages below.  N.B.  It is important to touch all
  // the way down to and including i=StackShadowPages.
  for (int i = 0; i < (int)(StackOverflow::stack_shadow_zone_size() / (int)os::vm_page_size()) - 1; i++) {
    // this could be any sized move but this is can be a debugging crumb
    // so the bigger the better.
    sub_d(tmp, tmp, AT);
    st_d(size, tmp, 0);
  }
}

void MacroAssembler::reserved_stack_check() {
  // testing if reserved zone needs to be enabled
  Label no_reserved_zone_enabling;

  ld_d(AT, Address(TREG, JavaThread::reserved_stack_activation_offset()));
  sub_d(AT, SP, AT);
  blt(AT, R0,  no_reserved_zone_enabling);

  enter();   // RA and FP are live.
  call_VM_leaf(CAST_FROM_FN_PTR(address, SharedRuntime::enable_stack_reserved_zone), TREG);
  leave();

  // We have already removed our own frame.
  // throw_delayed_StackOverflowError will think that it's been
  // called by our caller.
  li(AT, (long)StubRoutines::throw_delayed_StackOverflowError_entry());
  jr(AT);
  should_not_reach_here();

  bind(no_reserved_zone_enabling);
}

// the stack pointer adjustment is needed. see InterpreterMacroAssembler::super_call_VM_leaf
// this method will handle the stack problem, you need not to preserve the stack space for the argument now
void MacroAssembler::call_VM_leaf_base(address entry_point, int number_of_arguments) {
  assert(number_of_arguments <= 4, "just check");
  assert(StackAlignmentInBytes == 16, "must be");
  move(AT, SP);
  bstrins_d(SP, R0, 3, 0);
  addi_d(SP, SP, -(StackAlignmentInBytes));
  st_d(AT, SP, 0);
  call(entry_point, relocInfo::runtime_call_type);
  ld_d(SP, SP, 0);
}


void MacroAssembler::jmp(address entry) {
  jlong offs = entry - pc();
  if (reachable_from_branch_short(offs)) { // Short jump
    b(offset26(entry));
  } else {                                 // Far jump
    patchable_jump_far(R0, offs);
  }
}

void MacroAssembler::jmp(address entry, relocInfo::relocType rtype) {
  switch (rtype) {
    case relocInfo::none:
      jmp(entry);
      break;
    default:
      {
        InstructionMark im(this);
        relocate(rtype);
        patchable_jump(entry);
      }
      break;
  }
}

void MacroAssembler::jmp_far(Label& L) {
  if (L.is_bound()) {
    assert(target(L) != nullptr, "jmp most probably wrong");
    patchable_jump(target(L), true /* force patchable */);
  } else {
    L.add_patch_at(code(), locator());
    patchable_jump_far(R0, 0);
  }
}

// Move an oop into a register.
void MacroAssembler::movoop(Register dst, jobject obj) {
  int oop_index;
  if (obj == nullptr) {
    oop_index = oop_recorder()->allocate_oop_index(obj);
  } else {
#ifdef ASSERT
    {
      ThreadInVMfromUnknown tiv;
      assert(Universe::heap()->is_in(JNIHandles::resolve(obj)), "should be real oop");
    }
#endif
    oop_index = oop_recorder()->find_index(obj);
  }
  RelocationHolder rspec = oop_Relocation::spec(oop_index);

  if (BarrierSet::barrier_set()->barrier_set_assembler()->supports_instruction_patching()) {
    relocate(rspec);
    patchable_li52(dst, (long)obj);
  } else {
    address dummy = address(uintptr_t(pc()) & -wordSize); // A nearby aligned address
    relocate(rspec);
    patchable_li52(dst, (long)dummy);
  }
}

void MacroAssembler::mov_metadata(Address dst, Metadata* obj) {
  int oop_index;
  if (obj) {
    oop_index = oop_recorder()->find_index(obj);
  } else {
    oop_index = oop_recorder()->allocate_metadata_index(obj);
  }
  relocate(metadata_Relocation::spec(oop_index));
  patchable_li52(AT, (long)obj);
  st_d(AT, dst);
}

void MacroAssembler::mov_metadata(Register dst, Metadata* obj) {
  int oop_index;
  if (obj) {
    oop_index = oop_recorder()->find_index(obj);
  } else {
    oop_index = oop_recorder()->allocate_metadata_index(obj);
  }
  relocate(metadata_Relocation::spec(oop_index));
  patchable_li52(dst, (long)obj);
}

void MacroAssembler::call(address entry) {
  jlong offs = entry - pc();
  if (reachable_from_branch_short(offs)) { // Short call (pc-rel)
    bl(offset26(entry));
  } else if (is_simm(offs, 38)) {          // Far call (pc-rel)
    patchable_jump_far(RA, offs);
  } else {                                 // Long call (absolute)
    call_long(entry);
  }
}

void MacroAssembler::call(address entry, relocInfo::relocType rtype) {
  switch (rtype) {
    case relocInfo::none:
      call(entry);
      break;
    case relocInfo::runtime_call_type:
      if (!is_simm(entry - pc(), 38)) {
        call_long(entry);
        break;
      }
      // fallthrough
    default:
      {
        InstructionMark im(this);
        relocate(rtype);
        patchable_call(entry);
      }
      break;
  }
}

void MacroAssembler::call(address entry, RelocationHolder& rh){
  switch (rh.type()) {
    case relocInfo::none:
      call(entry);
      break;
    case relocInfo::runtime_call_type:
      if (!is_simm(entry - pc(), 38)) {
        call_long(entry);
        break;
      }
      // fallthrough
    default:
      {
        InstructionMark im(this);
        relocate(rh);
        patchable_call(entry);
      }
      break;
  }
}

void MacroAssembler::call_long(address entry) {
  jlong value = (jlong)entry;
  lu12i_w(AT, split_low20(value >> 12));
  lu32i_d(AT, split_low20(value >> 32));
  jirl(RA, AT, split_low12(value));
}

address MacroAssembler::ic_call(address entry, jint method_index) {
  RelocationHolder rh = virtual_call_Relocation::spec(pc(), method_index);
  patchable_li52(IC_Klass, (long)Universe::non_oop_word());
  assert(entry != nullptr, "call most probably wrong");
  InstructionMark im(this);
  return trampoline_call(AddressLiteral(entry, rh));
}

void MacroAssembler::emit_static_call_stub() {
  // Code stream for loading method may be changed.
  ibar(0);

  // static stub relocation also tags the Method* in the code-stream.
  mov_metadata(Rmethod, nullptr);
  // This is recognized as unresolved by relocs/nativeInst/ic code

  patchable_jump(pc());
}

void MacroAssembler::c2bool(Register r) {
  sltu(r, R0, r);
}

#ifndef PRODUCT
extern "C" void findpc(intptr_t x);
#endif

void MacroAssembler::debug(char* msg/*, RegistersForDebugging* regs*/) {
  if ( ShowMessageBoxOnError ) {
    JavaThreadState saved_state = JavaThread::current()->thread_state();
    JavaThread::current()->set_thread_state(_thread_in_vm);
    {
      // In order to get locks work, we need to fake a in_VM state
      ttyLocker ttyl;
      ::tty->print_cr("EXECUTION STOPPED: %s\n", msg);
      if (CountBytecodes || TraceBytecodes || StopInterpreterAt) {
        BytecodeCounter::print();
      }
    }
  }
  fatal("DEBUG MESSAGE: %s", msg);
}

void MacroAssembler::stop(const char* msg) {
#ifndef PRODUCT
  block_comment(msg);
#endif
  csrrd(R0, 0);
  emit_int64((uintptr_t)msg);
}

void MacroAssembler::increment(Register reg, int imm) {
  if (!imm) return;
  if (is_simm(imm, 12)) {
    addi_d(reg, reg, imm);
  } else {
    li(AT, imm);
    add_d(reg, reg, AT);
  }
}

void MacroAssembler::decrement(Register reg, int imm) {
  increment(reg, -imm);
}

void MacroAssembler::increment(Address addr, int imm) {
  if (!imm) return;
  assert(is_simm(imm, 12), "must be");
  ld_d(AT, addr);
  addi_d(AT, AT, imm);
  st_d(AT, addr);
}

void MacroAssembler::decrement(Address addr, int imm) {
  increment(addr, -imm);
}

void MacroAssembler::call_VM(Register oop_result,
                             address entry_point,
                             bool check_exceptions) {
  call_VM_helper(oop_result, entry_point, 0, check_exceptions);
}

void MacroAssembler::call_VM(Register oop_result,
                             address entry_point,
                             Register arg_1,
                             bool check_exceptions) {
  if (arg_1!=A1) move(A1, arg_1);
  call_VM_helper(oop_result, entry_point, 1, check_exceptions);
}

void MacroAssembler::call_VM(Register oop_result,
                             address entry_point,
                             Register arg_1,
                             Register arg_2,
                             bool check_exceptions) {
  if (arg_1 != A1) move(A1, arg_1);
  assert(arg_2 != A1, "smashed argument");
  if (arg_2 != A2) move(A2, arg_2);
  call_VM_helper(oop_result, entry_point, 2, check_exceptions);
}

void MacroAssembler::call_VM(Register oop_result,
                             address entry_point,
                             Register arg_1,
                             Register arg_2,
                             Register arg_3,
                             bool check_exceptions) {
  if (arg_1 != A1) move(A1, arg_1);
  assert(arg_2 != A1, "smashed argument");
  if (arg_2 != A2) move(A2, arg_2);
  assert(arg_3 != A1 && arg_3 != A2, "smashed argument");
  if (arg_3 != A3) move(A3, arg_3);
  call_VM_helper(oop_result, entry_point, 3, check_exceptions);
}

void MacroAssembler::call_VM(Register oop_result,
                             Register last_java_sp,
                             address entry_point,
                             int number_of_arguments,
                             bool check_exceptions) {
  call_VM_base(oop_result, NOREG, last_java_sp, entry_point, number_of_arguments, check_exceptions);
}

void MacroAssembler::call_VM(Register oop_result,
                             Register last_java_sp,
                             address entry_point,
                             Register arg_1,
                             bool check_exceptions) {
  if (arg_1 != A1) move(A1, arg_1);
  call_VM(oop_result, last_java_sp, entry_point, 1, check_exceptions);
}

void MacroAssembler::call_VM(Register oop_result,
                             Register last_java_sp,
                             address entry_point,
                             Register arg_1,
                             Register arg_2,
                             bool check_exceptions) {
  if (arg_1 != A1) move(A1, arg_1);
  assert(arg_2 != A1, "smashed argument");
  if (arg_2 != A2) move(A2, arg_2);
  call_VM(oop_result, last_java_sp, entry_point, 2, check_exceptions);
}

void MacroAssembler::call_VM(Register oop_result,
                             Register last_java_sp,
                             address entry_point,
                             Register arg_1,
                             Register arg_2,
                             Register arg_3,
                             bool check_exceptions) {
  if (arg_1 != A1) move(A1, arg_1);
  assert(arg_2 != A1, "smashed argument");
  if (arg_2 != A2) move(A2, arg_2);
  assert(arg_3 != A1 && arg_3 != A2, "smashed argument");
  if (arg_3 != A3) move(A3, arg_3);
  call_VM(oop_result, last_java_sp, entry_point, 3, check_exceptions);
}

void MacroAssembler::call_VM_base(Register oop_result,
                                  Register java_thread,
                                  Register last_java_sp,
                                  address  entry_point,
                                  int      number_of_arguments,
                                  bool     check_exceptions) {
  // determine java_thread register
  if (!java_thread->is_valid()) {
    java_thread = TREG;
  }
  // determine last_java_sp register
  if (!last_java_sp->is_valid()) {
    last_java_sp = SP;
  }
  // debugging support
  assert(number_of_arguments >= 0   , "cannot have negative number of arguments");
  assert(number_of_arguments <= 4   , "cannot have negative number of arguments");
  assert(java_thread != oop_result  , "cannot use the same register for java_thread & oop_result");
  assert(java_thread != last_java_sp, "cannot use the same register for java_thread & last_java_sp");

  assert(last_java_sp != FP, "this code doesn't work for last_java_sp == fp, which currently can't portably work anyway since C2 doesn't save fp");

  // set last Java frame before call
  Label before_call;
  bind(before_call);
  set_last_Java_frame(java_thread, last_java_sp, FP, before_call);

  // do the call
  move(A0, java_thread);
  call(entry_point, relocInfo::runtime_call_type);

  // restore the thread (cannot use the pushed argument since arguments
  // may be overwritten by C code generated by an optimizing compiler);
  // however can use the register value directly if it is callee saved.

#ifdef ASSERT
  {
    Label L;
    get_thread(AT);
    beq(java_thread, AT, L);
    stop("MacroAssembler::call_VM_base: TREG not callee saved?");
    bind(L);
  }
#endif

  // discard thread and arguments
  ld_d(SP, Address(java_thread, JavaThread::last_Java_sp_offset()));
  // reset last Java frame
  reset_last_Java_frame(java_thread, false);

  check_and_handle_popframe(java_thread);
  check_and_handle_earlyret(java_thread);
  if (check_exceptions) {
    // check for pending exceptions (java_thread is set upon return)
    Label L;
    ld_d(AT, java_thread, in_bytes(Thread::pending_exception_offset()));
    beq(AT, R0, L);
    // reload RA that may have been modified by the entry_point
    lipc(RA, before_call);
    jmp(StubRoutines::forward_exception_entry(), relocInfo::runtime_call_type);
    bind(L);
  }

  // get oop result if there is one and reset the value in the thread
  if (oop_result->is_valid()) {
    ld_d(oop_result, java_thread, in_bytes(JavaThread::vm_result_offset()));
    st_d(R0, java_thread, in_bytes(JavaThread::vm_result_offset()));
    verify_oop(oop_result);
  }
}

void MacroAssembler::call_VM_helper(Register oop_result, address entry_point, int number_of_arguments, bool check_exceptions) {
  move(V0, SP);
  //we also reserve space for java_thread here
  assert(StackAlignmentInBytes == 16, "must be");
  bstrins_d(SP, R0, 3, 0);
  call_VM_base(oop_result, NOREG, V0, entry_point, number_of_arguments, check_exceptions);
}

void MacroAssembler::call_VM_leaf(address entry_point, int number_of_arguments) {
  call_VM_leaf_base(entry_point, number_of_arguments);
}

void MacroAssembler::call_VM_leaf(address entry_point, Register arg_0) {
  if (arg_0 != A0) move(A0, arg_0);
  call_VM_leaf(entry_point, 1);
}

void MacroAssembler::call_VM_leaf(address entry_point, Register arg_0, Register arg_1) {
  if (arg_0 != A0) move(A0, arg_0);
  assert(arg_1 != A0, "smashed argument");
  if (arg_1 != A1) move(A1, arg_1);
  call_VM_leaf(entry_point, 2);
}

void MacroAssembler::call_VM_leaf(address entry_point, Register arg_0, Register arg_1, Register arg_2) {
  if (arg_0 != A0) move(A0, arg_0);
  assert(arg_1 != A0, "smashed argument");
  if (arg_1 != A1) move(A1, arg_1);
  assert(arg_2 != A0 && arg_2 != A1, "smashed argument");
  if (arg_2 != A2) move(A2, arg_2);
  call_VM_leaf(entry_point, 3);
}

void MacroAssembler::super_call_VM_leaf(address entry_point) {
  MacroAssembler::call_VM_leaf_base(entry_point, 0);
}

void MacroAssembler::super_call_VM_leaf(address entry_point,
                                                   Register arg_1) {
  if (arg_1 != A0) move(A0, arg_1);
  MacroAssembler::call_VM_leaf_base(entry_point, 1);
}

void MacroAssembler::super_call_VM_leaf(address entry_point,
                                                   Register arg_1,
                                                   Register arg_2) {
  if (arg_1 != A0) move(A0, arg_1);
  assert(arg_2 != A0, "smashed argument");
  if (arg_2 != A1) move(A1, arg_2);
  MacroAssembler::call_VM_leaf_base(entry_point, 2);
}

void MacroAssembler::super_call_VM_leaf(address entry_point,
                                                   Register arg_1,
                                                   Register arg_2,
                                                   Register arg_3) {
  if (arg_1 != A0) move(A0, arg_1);
  assert(arg_2 != A0, "smashed argument");
  if (arg_2 != A1) move(A1, arg_2);
  assert(arg_3 != A0 && arg_3 != A1, "smashed argument");
  if (arg_3 != A2) move(A2, arg_3);
  MacroAssembler::call_VM_leaf_base(entry_point, 3);
}

// these are no-ops overridden by InterpreterMacroAssembler
void MacroAssembler::check_and_handle_earlyret(Register java_thread) {}

void MacroAssembler::check_and_handle_popframe(Register java_thread) {}

void MacroAssembler::null_check(Register reg, int offset) {
  if (needs_explicit_null_check(offset)) {
    // provoke OS null exception if reg is null by
    // accessing M[reg] w/o changing any (non-CC) registers
    // NOTE: cmpl is plenty here to provoke a segv
    ld_w(AT, reg, 0);
  } else {
    // nothing to do, (later) access of M[reg + offset]
    // will provoke OS null exception if reg is null
  }
}

void MacroAssembler::enter() {
  push2(RA, FP);
  addi_d(FP, SP, 2 * wordSize);
}

void MacroAssembler::leave() {
  addi_d(SP, FP, -2 * wordSize);
  pop2(RA, FP);
}

void MacroAssembler::build_frame(int framesize) {
  assert(framesize >= 2 * wordSize, "framesize must include space for FP/RA");
  assert(framesize % (2 * wordSize) == 0, "must preserve 2 * wordSize alignment");
  if (Assembler::is_simm(-framesize, 12)) {
    addi_d(SP, SP, -framesize);
    st_d(FP, Address(SP, framesize - 2 * wordSize));
    st_d(RA, Address(SP, framesize - 1 * wordSize));
    if (PreserveFramePointer)
      addi_d(FP, SP, framesize);
  } else {
    addi_d(SP, SP, -2 * wordSize);
    st_d(FP, Address(SP, 0 * wordSize));
    st_d(RA, Address(SP, 1 * wordSize));
    if (PreserveFramePointer)
      addi_d(FP, SP, 2 * wordSize);
    li(SCR1, framesize - 2 * wordSize);
    sub_d(SP, SP, SCR1);
  }
  verify_cross_modify_fence_not_required();
}

void MacroAssembler::remove_frame(int framesize) {
  assert(framesize >= 2 * wordSize, "framesize must include space for FP/RA");
  assert(framesize % (2*wordSize) == 0, "must preserve 2*wordSize alignment");
  if (Assembler::is_simm(framesize, 12)) {
    ld_d(FP, Address(SP, framesize - 2 * wordSize));
    ld_d(RA, Address(SP, framesize - 1 * wordSize));
    addi_d(SP, SP, framesize);
  } else {
    li(SCR1, framesize - 2 * wordSize);
    add_d(SP, SP, SCR1);
    ld_d(FP, Address(SP, 0 * wordSize));
    ld_d(RA, Address(SP, 1 * wordSize));
    addi_d(SP, SP, 2 * wordSize);
  }
}

void MacroAssembler::unimplemented(const char* what) {
  const char* buf = nullptr;
  {
    ResourceMark rm;
    stringStream ss;
    ss.print("unimplemented: %s", what);
    buf = code_string(ss.as_string());
  }
  stop(buf);
}

// get_thread() can be called anywhere inside generated code so we
// need to save whatever non-callee save context might get clobbered
// by the call to Thread::current() or, indeed, the call setup code.
void MacroAssembler::get_thread(Register thread) {
  // save all call-clobbered int regs except thread
  RegSet caller_saved_gpr = RegSet::range(A0, T8) + FP + RA - thread;

  push(caller_saved_gpr);

  call(CAST_FROM_FN_PTR(address, Thread::current), relocInfo::runtime_call_type);

  if (thread != A0) {
    move(thread, A0);
  }

  pop(caller_saved_gpr);
}

void MacroAssembler::reset_last_Java_frame(Register java_thread, bool clear_fp) {
  // determine java_thread register
  if (!java_thread->is_valid()) {
    java_thread = TREG;
  }
  // we must set sp to zero to clear frame
  st_d(R0, Address(java_thread, JavaThread::last_Java_sp_offset()));
  // must clear fp, so that compiled frames are not confused; it is possible
  // that we need it only for debugging
  if(clear_fp) {
    st_d(R0, Address(java_thread, JavaThread::last_Java_fp_offset()));
  }

  // Always clear the pc because it could have been set by make_walkable()
  st_d(R0, Address(java_thread, JavaThread::last_Java_pc_offset()));
}

void MacroAssembler::reset_last_Java_frame(bool clear_fp) {
  // we must set sp to zero to clear frame
  st_d(R0, TREG, in_bytes(JavaThread::last_Java_sp_offset()));
  // must clear fp, so that compiled frames are not confused; it is
  // possible that we need it only for debugging
  if (clear_fp) {
    st_d(R0, TREG, in_bytes(JavaThread::last_Java_fp_offset()));
  }

  // Always clear the pc because it could have been set by make_walkable()
  st_d(R0, TREG, in_bytes(JavaThread::last_Java_pc_offset()));
}

void MacroAssembler::safepoint_poll(Label& slow_path, Register thread_reg, bool at_return, bool acquire, bool in_nmethod) {
  if (acquire) {
    ld_d(AT, thread_reg, in_bytes(JavaThread::polling_word_offset()));
    membar(Assembler::Membar_mask_bits(LoadLoad|LoadStore));
  } else {
    ld_d(AT, thread_reg, in_bytes(JavaThread::polling_word_offset()));
  }
  if (at_return) {
    // Note that when in_nmethod is set, the stack pointer is incremented before the poll. Therefore,
    // we may safely use the sp instead to perform the stack watermark check.
    blt_far(AT, in_nmethod ? SP : FP, slow_path, false /* signed */);
  } else {
    andi(AT, AT, SafepointMechanism::poll_bit());
    bnez(AT, slow_path);
  }
}

// Calls to C land
//
// When entering C land, the fp, & sp of the last Java frame have to be recorded
// in the (thread-local) JavaThread object. When leaving C land, the last Java fp
// has to be reset to 0. This is required to allow proper stack traversal.
void MacroAssembler::set_last_Java_frame(Register java_thread,
                                         Register last_java_sp,
                                         Register last_java_fp,
                                         Label& last_java_pc) {
  // determine java_thread register
  if (!java_thread->is_valid()) {
    java_thread = TREG;
  }

  // determine last_java_sp register
  if (!last_java_sp->is_valid()) {
    last_java_sp = SP;
  }

  // last_java_fp is optional
  if (last_java_fp->is_valid()) {
    st_d(last_java_fp, Address(java_thread, JavaThread::last_Java_fp_offset()));
  }

  // last_java_pc
  lipc(AT, last_java_pc);
  st_d(AT, Address(java_thread, JavaThread::frame_anchor_offset() +
                                JavaFrameAnchor::last_Java_pc_offset()));

  st_d(last_java_sp, Address(java_thread, JavaThread::last_Java_sp_offset()));
}

void MacroAssembler::set_last_Java_frame(Register last_java_sp,
                                         Register last_java_fp,
                                         Label& last_java_pc) {
  set_last_Java_frame(NOREG, last_java_sp, last_java_fp, last_java_pc);
}

void MacroAssembler::set_last_Java_frame(Register last_java_sp,
                                         Register last_java_fp,
                                         Register last_java_pc) {
  // determine last_java_sp register
  if (!last_java_sp->is_valid()) {
    last_java_sp = SP;
  }

  // last_java_fp is optional
  if (last_java_fp->is_valid()) {
    st_d(last_java_fp, Address(TREG, JavaThread::last_Java_fp_offset()));
  }

  // last_java_pc is optional
  if (last_java_pc->is_valid()) {
    st_d(last_java_pc, Address(TREG, JavaThread::frame_anchor_offset() +
                                     JavaFrameAnchor::last_Java_pc_offset()));
  }

  st_d(last_java_sp, Address(TREG, JavaThread::last_Java_sp_offset()));
}

// Defines obj, preserves var_size_in_bytes, okay for t2 == var_size_in_bytes.
void MacroAssembler::tlab_allocate(Register obj,
                                   Register var_size_in_bytes,
                                   int con_size_in_bytes,
                                   Register t1,
                                   Register t2,
                                   Label& slow_case) {
  BarrierSetAssembler *bs = BarrierSet::barrier_set()->barrier_set_assembler();
  bs->tlab_allocate(this, obj, var_size_in_bytes, con_size_in_bytes, t1, t2, slow_case);
}

void MacroAssembler::incr_allocated_bytes(Register thread,
                                          Register var_size_in_bytes,
                                          int con_size_in_bytes,
                                          Register t1) {
  if (!thread->is_valid()) {
    thread = TREG;
  }

  ld_d(AT, Address(thread, JavaThread::allocated_bytes_offset()));
  if (var_size_in_bytes->is_valid()) {
    add_d(AT, AT, var_size_in_bytes);
  } else {
    addi_d(AT, AT, con_size_in_bytes);
  }
  st_d(AT, Address(thread, JavaThread::allocated_bytes_offset()));
}

void MacroAssembler::li(Register rd, jlong value) {
  jlong hi12 = bitfield(value, 52, 12);
  jlong lo52 = bitfield(value,  0, 52);

  if (hi12 != 0 && lo52 == 0) {
    lu52i_d(rd, R0, hi12);
  } else {
    jlong hi20 = bitfield(value, 32, 20);
    jlong lo20 = bitfield(value, 12, 20);
    jlong lo12 = bitfield(value,  0, 12);

    if (lo20 == 0) {
      ori(rd, R0, lo12);
    } else if (bitfield(simm12(lo12), 12, 20) == lo20) {
      addi_w(rd, R0, simm12(lo12));
    } else {
      lu12i_w(rd, lo20);
      if (lo12 != 0)
        ori(rd, rd, lo12);
    }
    if (hi20 != bitfield(simm20(lo20), 20, 20))
      lu32i_d(rd, hi20);
    if (hi12 != bitfield(simm20(hi20), 20, 12))
      lu52i_d(rd, rd, hi12);
  }
}

void MacroAssembler::patchable_li52(Register rd, jlong value) {
  int count = 0;

  if (value <= max_jint && value >= min_jint) {
    if (is_simm(value, 12)) {
      addi_d(rd, R0, value);
      count++;
    } else if (is_uimm(value, 12)) {
      ori(rd, R0, value);
      count++;
    } else {
      lu12i_w(rd, split_low20(value >> 12));
      count++;
      if (split_low12(value)) {
        ori(rd, rd, split_low12(value));
        count++;
      }
    }
  } else if (is_simm(value, 52)) {
    lu12i_w(rd, split_low20(value >> 12));
    count++;
    if (split_low12(value)) {
      ori(rd, rd, split_low12(value));
      count++;
    }
    lu32i_d(rd, split_low20(value >> 32));
    count++;
  } else {
    tty->print_cr("value = 0x%lx", value);
    guarantee(false, "Not supported yet !");
  }

  while (count < 3) {
    nop();
    count++;
  }
}

void MacroAssembler::lipc(Register rd, Label& L) {
  if (L.is_bound()) {
    jint offs = (target(L) - pc()) >> 2;
    guarantee(is_simm(offs, 20), "Not signed 20-bit offset");
    pcaddi(rd, offs);
  } else {
    InstructionMark im(this);
    L.add_patch_at(code(), locator());
    pcaddi(rd, 0);
  }
}

void MacroAssembler::set_narrow_klass(Register dst, Klass* k) {
  assert(UseCompressedClassPointers, "should only be used for compressed header");
  assert(oop_recorder() != nullptr, "this assembler needs an OopRecorder");

  int klass_index = oop_recorder()->find_index(k);
  RelocationHolder rspec = metadata_Relocation::spec(klass_index);
  long narrowKlass = (long)CompressedKlassPointers::encode(k);

  relocate(rspec, Assembler::narrow_oop_operand);
  patchable_li52(dst, narrowKlass);
}

void MacroAssembler::set_narrow_oop(Register dst, jobject obj) {
  assert(UseCompressedOops, "should only be used for compressed header");
  assert(oop_recorder() != nullptr, "this assembler needs an OopRecorder");

  int oop_index = oop_recorder()->find_index(obj);
  RelocationHolder rspec = oop_Relocation::spec(oop_index);

  relocate(rspec, Assembler::narrow_oop_operand);
  patchable_li52(dst, oop_index);
}

// ((OopHandle)result).resolve();
void MacroAssembler::resolve_oop_handle(Register result, Register tmp1, Register tmp2) {
  // OopHandle::resolve is an indirection.
  access_load_at(T_OBJECT, IN_NATIVE, result, Address(result, 0), tmp1, tmp2);
}

// ((WeakHandle)result).resolve();
void MacroAssembler::resolve_weak_handle(Register result, Register tmp1, Register tmp2) {
  assert_different_registers(result, tmp1, tmp2);
  Label resolved;

  // A null weak handle resolves to null.
  beqz(result, resolved);

  // Only 64 bit platforms support GCs that require a tmp register
  // WeakHandle::resolve is an indirection like jweak.
  access_load_at(T_OBJECT, IN_NATIVE | ON_PHANTOM_OOP_REF,
                 result, Address(result), tmp1, tmp2);
  bind(resolved);
}

void MacroAssembler::load_mirror(Register mirror, Register method, Register tmp1, Register tmp2) {
  ld_d(mirror, Address(method, Method::const_offset()));
  ld_d(mirror, Address(mirror, ConstMethod::constants_offset()));
  ld_d(mirror, Address(mirror, ConstantPool::pool_holder_offset()));
  ld_d(mirror, Address(mirror, Klass::java_mirror_offset()));
  resolve_oop_handle(mirror, tmp1, tmp2);
}

void MacroAssembler::_verify_oop(Register reg, const char* s, const char* file, int line) {
  if (!VerifyOops) return;

  const char* bx = nullptr;
  {
    ResourceMark rm;
    stringStream ss;
    ss.print("verify_oop: %s: %s (%s:%d)", reg->name(), s, file, line);
    bx = code_string(ss.as_string());
  }

  push(RegSet::of(RA, SCR1, c_rarg0, c_rarg1));

  move(c_rarg1, reg);
  // The length of the instruction sequence emitted should be independent
  // of the value of the local char buffer address so that the size of mach
  // nodes for scratch emit and normal emit matches.
  patchable_li52(c_rarg0, (long)bx);

  // call indirectly to solve generation ordering problem
  li(SCR1, StubRoutines::verify_oop_subroutine_entry_address());
  ld_d(SCR1, SCR1, 0);
  jalr(SCR1);

  pop(RegSet::of(RA, SCR1, c_rarg0, c_rarg1));
}

void MacroAssembler::_verify_oop_addr(Address addr, const char* s, const char* file, int line) {
  if (!VerifyOops) return;

  const char* bx = nullptr;
  {
    ResourceMark rm;
    stringStream ss;
    ss.print("verify_oop_addr: %s (%s:%d)", s, file, line);
    bx = code_string(ss.as_string());
  }

  push(RegSet::of(RA, SCR1, c_rarg0, c_rarg1));

  // addr may contain sp so we will have to adjust it based on the
  // pushes that we just did.
  if (addr.uses(SP)) {
    lea(c_rarg1, addr);
    ld_d(c_rarg1, Address(c_rarg1, 4 * wordSize));
  } else {
    ld_d(c_rarg1, addr);
  }

  // The length of the instruction sequence emitted should be independent
  // of the value of the local char buffer address so that the size of mach
  // nodes for scratch emit and normal emit matches.
  patchable_li52(c_rarg0, (long)bx);

  // call indirectly to solve generation ordering problem
  li(SCR1, StubRoutines::verify_oop_subroutine_entry_address());
  ld_d(SCR1, SCR1, 0);
  jalr(SCR1);

  pop(RegSet::of(RA, SCR1, c_rarg0, c_rarg1));
}

void MacroAssembler::verify_tlab(Register t1, Register t2) {
#ifdef ASSERT
  assert_different_registers(t1, t2, AT);
  if (UseTLAB && VerifyOops) {
    Label next, ok;

    get_thread(t1);

    ld_d(t2, Address(t1, JavaThread::tlab_top_offset()));
    ld_d(AT, Address(t1, JavaThread::tlab_start_offset()));
    bgeu(t2, AT, next);

    stop("assert(top >= start)");

    bind(next);
    ld_d(AT, Address(t1, JavaThread::tlab_end_offset()));
    bgeu(AT, t2, ok);

    stop("assert(top <= end)");

    bind(ok);

  }
#endif
}

void MacroAssembler::bswap_h(Register dst, Register src) {
  revb_2h(dst, src);
  ext_w_h(dst, dst);  // sign extension of the lower 16 bits
}

void MacroAssembler::bswap_hu(Register dst, Register src) {
  revb_2h(dst, src);
  bstrpick_d(dst, dst, 15, 0);  // zero extension of the lower 16 bits
}

void MacroAssembler::bswap_w(Register dst, Register src) {
  revb_2w(dst, src);
  slli_w(dst, dst, 0);  // keep sign, clear upper bits
}

void MacroAssembler::cmpxchg(Address addr, Register oldval, Register newval,
                             Register resflag, bool retold, bool acquire,
                             bool weak, bool exchange) {
  assert(oldval != resflag, "oldval != resflag");
  assert(newval != resflag, "newval != resflag");
  assert(addr.base() != resflag, "addr.base() != resflag");
  Label again, succ, fail;

  if (UseAMCAS) {
    move(resflag, oldval/* compare_value */);
    if (addr.disp() != 0) {
      assert_different_registers(AT, oldval);
      assert_different_registers(AT, newval);
      assert_different_registers(AT, resflag);

      if (Assembler::is_simm(addr.disp(), 12)) {
        addi_d(AT, addr.base(), addr.disp());
      } else {
        li(AT, addr.disp());
        add_d(AT, addr.base(), AT);
      }
      amcas_db_d(resflag, newval, AT);
    } else {
      amcas_db_d(resflag, newval, addr.base());
    }
    bne(resflag, oldval, fail);
    if (!exchange) {
      ori(resflag, R0, 1);
    }
    b(succ);
    bind(fail);
    if (retold && oldval != R0) {
      move(oldval, resflag);
    }
    if (!exchange) {
      move(resflag, R0);
    }
    bind(succ);

  } else {
    bind(again);
    ll_d(resflag, addr);
    bne(resflag, oldval, fail);
    move(resflag, newval);
    sc_d(resflag, addr);
    if (weak) {
      b(succ);
    } else {
      beqz(resflag, again);
    }
    if (exchange) {
      move(resflag, oldval);
    }
    b(succ);

    bind(fail);
    if (acquire) {
      membar(Assembler::Membar_mask_bits(LoadLoad|LoadStore));
    } else {
      dbar(0x700);
    }
    if (retold && oldval != R0)
      move(oldval, resflag);
    if (!exchange) {
      move(resflag, R0);
    }
    bind(succ);
  }
}

void MacroAssembler::cmpxchg(Address addr, Register oldval, Register newval,
                             Register tmp, bool retold, bool acquire, Label& succ, Label* fail) {
  assert(oldval != tmp, "oldval != tmp");
  assert(newval != tmp, "newval != tmp");
  Label again, neq;

  if (UseAMCAS) {
    move(tmp, oldval);
    if (addr.disp() != 0) {
      assert_different_registers(AT, oldval);
      assert_different_registers(AT, newval);
      assert_different_registers(AT, tmp);

      if (Assembler::is_simm(addr.disp(), 12)) {
        addi_d(AT, addr.base(), addr.disp());
      } else {
        li(AT, addr.disp());
        add_d(AT, addr.base(), AT);
      }
      amcas_db_d(tmp, newval, AT);
    } else {
      amcas_db_d(tmp, newval, addr.base());
    }
    bne(tmp, oldval, neq);
    b(succ);
    bind(neq);
    if (fail) {
      b(*fail);
    }
  } else {
    bind(again);
    ll_d(tmp, addr);
    bne(tmp, oldval, neq);
    move(tmp, newval);
    sc_d(tmp, addr);
    beqz(tmp, again);
    b(succ);
    bind(neq);
    if (acquire) {
      membar(Assembler::Membar_mask_bits(LoadLoad|LoadStore));
    } else {
      dbar(0x700);
    }
    if (retold && oldval != R0)
      move(oldval, tmp);
    if (fail)
      b(*fail);
  }
}

void MacroAssembler::cmpxchg32(Address addr, Register oldval, Register newval,
                               Register resflag, bool sign, bool retold, bool acquire,
                               bool weak, bool exchange) {
  assert(oldval != resflag, "oldval != resflag");
  assert(newval != resflag, "newval != resflag");
  assert(addr.base() != resflag, "addr.base() != resflag");
  Label again, succ, fail;

  if (UseAMCAS) {
    move(resflag, oldval/* compare_value */);
    if (addr.disp() != 0) {
      assert_different_registers(AT, oldval);
      assert_different_registers(AT, newval);
      assert_different_registers(AT, resflag);

      if (Assembler::is_simm(addr.disp(), 12)) {
        addi_d(AT, addr.base(), addr.disp());
      } else {
        li(AT, addr.disp());
        add_d(AT, addr.base(), AT);
      }
      amcas_db_w(resflag, newval, AT);
    } else {
      amcas_db_w(resflag, newval, addr.base());
    }
    if (!sign) {
      lu32i_d(resflag, 0);
    }
    bne(resflag, oldval, fail);
    if (!exchange) {
      ori(resflag, R0, 1);
    }
    b(succ);
    bind(fail);
    if (retold && oldval != R0) {
      move(oldval, resflag);
    }
    if (!exchange) {
      move(resflag, R0);
    }
    bind(succ);
  } else {
    bind(again);
    ll_w(resflag, addr);
    if (!sign)
      lu32i_d(resflag, 0);
    bne(resflag, oldval, fail);
    move(resflag, newval);
    sc_w(resflag, addr);
    if (weak) {
      b(succ);
    } else {
      beqz(resflag, again);
    }
    if (exchange) {
      move(resflag, oldval);
    }
    b(succ);

    bind(fail);
    if (acquire) {
      membar(Assembler::Membar_mask_bits(LoadLoad|LoadStore));
    } else {
      dbar(0x700);
    }
    if (retold && oldval != R0)
      move(oldval, resflag);
    if (!exchange) {
      move(resflag, R0);
    }
    bind(succ);
  }
}

void MacroAssembler::cmpxchg32(Address addr, Register oldval, Register newval, Register tmp,
                               bool sign, bool retold, bool acquire, Label& succ, Label* fail) {
  assert(oldval != tmp, "oldval != tmp");
  assert(newval != tmp, "newval != tmp");
  Label again, neq;

  if (UseAMCAS) {
    move(tmp, oldval);
    if (addr.disp() != 0) {
      assert_different_registers(AT, oldval);
      assert_different_registers(AT, newval);
      assert_different_registers(AT, tmp);

      if (Assembler::is_simm(addr.disp(), 12)) {
        addi_d(AT, addr.base(), addr.disp());
      } else {
        li(AT, addr.disp());
        add_d(AT, addr.base(), AT);
      }
      amcas_db_w(tmp, newval, AT);
    } else {
      amcas_db_w(tmp, newval, addr.base());
    }
    if (!sign) {
      lu32i_d(tmp, 0);
    }
    bne(tmp, oldval, neq);
    b(succ);
    bind(neq);
    if (fail) {
      b(*fail);
    }
  } else {
    bind(again);
    ll_w(tmp, addr);
    if (!sign)
      lu32i_d(tmp, 0);
    bne(tmp, oldval, neq);
    move(tmp, newval);
    sc_w(tmp, addr);
    beqz(tmp, again);
    b(succ);

    bind(neq);
    if (acquire) {
      membar(Assembler::Membar_mask_bits(LoadLoad|LoadStore));
    } else {
      dbar(0x700);
    }
    if (retold && oldval != R0)
      move(oldval, tmp);
    if (fail)
      b(*fail);
  }
}

void MacroAssembler::cmpxchg16(Address addr, Register oldval, Register newval,
                               Register resflag, bool sign, bool retold, bool acquire,
                               bool weak, bool exchange) {
  assert(oldval != resflag, "oldval != resflag");
  assert(newval != resflag, "newval != resflag");
  assert(addr.base() != resflag, "addr.base() != resflag");
  assert(UseAMCAS == true, "UseAMCAS == true");
  Label again, succ, fail;

  move(resflag, oldval/* compare_value */);
  if (addr.disp() != 0) {
    assert_different_registers(AT, oldval);
    assert_different_registers(AT, newval);
    assert_different_registers(AT, resflag);

    if (Assembler::is_simm(addr.disp(), 12)) {
      addi_d(AT, addr.base(), addr.disp());
    } else {
      li(AT, addr.disp());
      add_d(AT, addr.base(), AT);
    }
    amcas_db_h(resflag, newval, AT);
  } else {
    amcas_db_h(resflag, newval, addr.base());
  }
  if (!sign) {
    bstrpick_w(resflag, resflag, 15, 0);
  }
  bne(resflag, oldval, fail);
  if (!exchange) {
    ori(resflag, R0, 1);
  }
  b(succ);
  bind(fail);
  if (retold && oldval != R0) {
    move(oldval, resflag);
  }
  if (!exchange) {
    move(resflag, R0);
  }
  bind(succ);
}

void MacroAssembler::cmpxchg16(Address addr, Register oldval, Register newval, Register tmp,
                               bool sign, bool retold, bool acquire, Label& succ, Label* fail) {
  assert(oldval != tmp, "oldval != tmp");
  assert(newval != tmp, "newval != tmp");
  assert(UseAMCAS == true, "UseAMCAS == true");
  Label again, neq;

  move(tmp, oldval);
  if (addr.disp() != 0) {
    assert_different_registers(AT, oldval);
    assert_different_registers(AT, newval);
    assert_different_registers(AT, tmp);

    if (Assembler::is_simm(addr.disp(), 12)) {
      addi_d(AT, addr.base(), addr.disp());
    } else {
      li(AT, addr.disp());
      add_d(AT, addr.base(), AT);
    }
    amcas_db_h(tmp, newval, AT);
  } else {
    amcas_db_h(tmp, newval, addr.base());
  }
  if (!sign) {
    bstrpick_w(tmp, tmp, 15, 0);
  }
  bne(tmp, oldval, neq);
  b(succ);
  bind(neq);
  if (retold && oldval != R0) {
    move(oldval, tmp);
  }
  if (fail) {
    b(*fail);
  }
}

void MacroAssembler::cmpxchg8(Address addr, Register oldval, Register newval,
                               Register resflag, bool sign, bool retold, bool acquire,
                               bool weak, bool exchange) {
  assert(oldval != resflag, "oldval != resflag");
  assert(newval != resflag, "newval != resflag");
  assert(addr.base() != resflag, "addr.base() != resflag");
  assert(UseAMCAS == true, "UseAMCAS == true");
  Label again, succ, fail;

  move(resflag, oldval/* compare_value */);
  if (addr.disp() != 0) {
    assert_different_registers(AT, oldval);
    assert_different_registers(AT, newval);
    assert_different_registers(AT, resflag);

    if (Assembler::is_simm(addr.disp(), 12)) {
      addi_d(AT, addr.base(), addr.disp());
    } else {
      li(AT, addr.disp());
      add_d(AT, addr.base(), AT);
    }
    amcas_db_b(resflag, newval, AT);
  } else {
    amcas_db_b(resflag, newval, addr.base());
  }
  if (!sign) {
    andi(resflag, resflag, 0xFF);
  }
  bne(resflag, oldval, fail);
  if (!exchange) {
    ori(resflag, R0, 1);
  }
  b(succ);
  bind(fail);
  if (retold && oldval != R0) {
    move(oldval, resflag);
  }
  if (!exchange) {
    move(resflag, R0);
  }
  bind(succ);
}

void MacroAssembler::cmpxchg8(Address addr, Register oldval, Register newval, Register tmp,
                               bool sign, bool retold, bool acquire, Label& succ, Label* fail) {
  assert(oldval != tmp, "oldval != tmp");
  assert(newval != tmp, "newval != tmp");
  assert(UseAMCAS == true, "UseAMCAS == true");
  Label again, neq;

  move(tmp, oldval);
  if (addr.disp() != 0) {
    assert_different_registers(AT, oldval);
    assert_different_registers(AT, newval);
    assert_different_registers(AT, tmp);

    if (Assembler::is_simm(addr.disp(), 12)) {
      addi_d(AT, addr.base(), addr.disp());
    } else {
      li(AT, addr.disp());
      add_d(AT, addr.base(), AT);
    }
    amcas_db_b(tmp, newval, AT);
  } else {
    amcas_db_b(tmp, newval, addr.base());
  }
  if (!sign) {
    andi(tmp, tmp, 0xFF);
  }
  bne(tmp, oldval, neq);
  b(succ);
  bind(neq);
  if (retold && oldval != R0) {
    move(oldval, tmp);
  }
  if (fail) {
    b(*fail);
  }
}

void MacroAssembler::push_cont_fastpath(Register java_thread) {
  if (!Continuations::enabled()) return;
  Label done;
  ld_d(AT, Address(java_thread, JavaThread::cont_fastpath_offset()));
  bgeu(AT, SP, done);
  st_d(SP, Address(java_thread, JavaThread::cont_fastpath_offset()));
  bind(done);
}

void MacroAssembler::pop_cont_fastpath(Register java_thread) {
  if (!Continuations::enabled()) return;
  Label done;
  ld_d(AT, Address(java_thread, JavaThread::cont_fastpath_offset()));
  bltu(SP, AT, done);
  st_d(R0, Address(java_thread, JavaThread::cont_fastpath_offset()));
  bind(done);
}

void MacroAssembler::align(int modulus) {
  while (offset() % modulus != 0) nop();
}

void MacroAssembler::post_call_nop() {
  if (!Continuations::enabled()) return;
  InstructionMark im(this);
  relocate(post_call_nop_Relocation::spec());
  // pick 2 instructions to save oopmap(8 bits) and offset(24 bits)
  nop();
  ori(R0, R0, 0);
  ori(R0, R0, 0);
}

// SCR2 is allocable in C2 Compiler
static RegSet caller_saved_regset = RegSet::range(A0, A7) + RegSet::range(T0, T8) + RegSet::of(FP, RA) - RegSet::of(SCR1);
static FloatRegSet caller_saved_fpu_regset = FloatRegSet::range(F0, F23);

void MacroAssembler::push_call_clobbered_registers_except(RegSet exclude) {
  push(caller_saved_regset - exclude);
  push_fpu(caller_saved_fpu_regset);
}

void MacroAssembler::pop_call_clobbered_registers_except(RegSet exclude) {
  pop_fpu(caller_saved_fpu_regset);
  pop(caller_saved_regset - exclude);
}

void MacroAssembler::push2(Register reg1, Register reg2) {
  addi_d(SP, SP, -16);
  st_d(reg1, SP, 8);
  st_d(reg2, SP, 0);
}

void MacroAssembler::pop2(Register reg1, Register reg2) {
  ld_d(reg1, SP, 8);
  ld_d(reg2, SP, 0);
  addi_d(SP, SP, 16);
}

void MacroAssembler::push(unsigned int bitset) {
  unsigned char regs[31];
  int count = 0;

  bitset >>= 1;
  for (int reg = 1; reg < 31; reg++) {
    if (1 & bitset)
      regs[count++] = reg;
    bitset >>= 1;
  }

  addi_d(SP, SP, -align_up(count, 2) * wordSize);
  for (int i = 0; i < count; i ++)
    st_d(as_Register(regs[i]), SP, i * wordSize);
}

void MacroAssembler::pop(unsigned int bitset) {
  unsigned char regs[31];
  int count = 0;

  bitset >>= 1;
  for (int reg = 1; reg < 31; reg++) {
    if (1 & bitset)
      regs[count++] = reg;
    bitset >>= 1;
  }

  for (int i = 0; i < count; i ++)
    ld_d(as_Register(regs[i]), SP, i * wordSize);
  addi_d(SP, SP, align_up(count, 2) * wordSize);
}

void MacroAssembler::push_fpu(unsigned int bitset) {
  unsigned char regs[32];
  int count = 0;

  if (bitset == 0)
    return;

  for (int reg = 0; reg <= 31; reg++) {
    if (1 & bitset)
      regs[count++] = reg;
    bitset >>= 1;
  }

  addi_d(SP, SP, -align_up(count, 2) * wordSize);
  for (int i = 0; i < count; i++)
    fst_d(as_FloatRegister(regs[i]), SP, i * wordSize);
}

void MacroAssembler::pop_fpu(unsigned int bitset) {
  unsigned char regs[32];
  int count = 0;

  if (bitset == 0)
    return;

  for (int reg = 0; reg <= 31; reg++) {
    if (1 & bitset)
      regs[count++] = reg;
    bitset >>= 1;
  }

  for (int i = 0; i < count; i++)
    fld_d(as_FloatRegister(regs[i]), SP, i * wordSize);
  addi_d(SP, SP, align_up(count, 2) * wordSize);
}

static int vpr_offset(int off) {
  int slots_per_vpr = 0;

  if (UseLASX)
    slots_per_vpr = FloatRegister::slots_per_lasx_register;
  else if (UseLSX)
    slots_per_vpr = FloatRegister::slots_per_lsx_register;

  return off * slots_per_vpr * VMRegImpl::stack_slot_size;
}

void MacroAssembler::push_vp(unsigned int bitset) {
  unsigned char regs[32];
  int count = 0;

  if (bitset == 0)
    return;

  for (int reg = 0; reg <= 31; reg++) {
    if (1 & bitset)
      regs[count++] = reg;
    bitset >>= 1;
  }

  addi_d(SP, SP, vpr_offset(-align_up(count, 2)));

  for (int i = 0; i < count; i++) {
    int off = vpr_offset(i);
    if (UseLASX)
      xvst(as_FloatRegister(regs[i]), SP, off);
    else if (UseLSX)
      vst(as_FloatRegister(regs[i]), SP, off);
  }
}

void MacroAssembler::pop_vp(unsigned int bitset) {
  unsigned char regs[32];
  int count = 0;

  if (bitset == 0)
    return;

  for (int reg = 0; reg <= 31; reg++) {
    if (1 & bitset)
      regs[count++] = reg;
    bitset >>= 1;
  }

  for (int i = 0; i < count; i++) {
    int off = vpr_offset(i);
    if (UseLASX)
      xvld(as_FloatRegister(regs[i]), SP, off);
    else if (UseLSX)
      vld(as_FloatRegister(regs[i]), SP, off);
  }

  addi_d(SP, SP, vpr_offset(align_up(count, 2)));
}

void MacroAssembler::load_method_holder(Register holder, Register method) {
  ld_d(holder, Address(method, Method::const_offset()));                      // ConstMethod*
  ld_d(holder, Address(holder, ConstMethod::constants_offset()));             // ConstantPool*
  ld_d(holder, Address(holder, ConstantPool::pool_holder_offset())); // InstanceKlass*
}

void MacroAssembler::load_method_holder_cld(Register rresult, Register rmethod) {
  load_method_holder(rresult, rmethod);
  ld_d(rresult, Address(rresult, InstanceKlass::class_loader_data_offset()));
}

// for UseCompressedOops Option
void MacroAssembler::load_klass(Register dst, Register src) {
  if(UseCompressedClassPointers){
    ld_wu(dst, Address(src, oopDesc::klass_offset_in_bytes()));
    decode_klass_not_null(dst);
  } else {
    ld_d(dst, src, oopDesc::klass_offset_in_bytes());
  }
}

void MacroAssembler::store_klass(Register dst, Register src) {
  if(UseCompressedClassPointers){
    encode_klass_not_null(src);
    st_w(src, dst, oopDesc::klass_offset_in_bytes());
  } else {
    st_d(src, dst, oopDesc::klass_offset_in_bytes());
  }
}

void MacroAssembler::store_klass_gap(Register dst, Register src) {
  if (UseCompressedClassPointers) {
    st_w(src, dst, oopDesc::klass_gap_offset_in_bytes());
  }
}

void MacroAssembler::access_load_at(BasicType type, DecoratorSet decorators, Register dst, Address src,
                                    Register tmp1, Register tmp2) {
  BarrierSetAssembler* bs = BarrierSet::barrier_set()->barrier_set_assembler();
  decorators = AccessInternal::decorator_fixup(decorators, type);
  bool as_raw = (decorators & AS_RAW) != 0;
  if (as_raw) {
    bs->BarrierSetAssembler::load_at(this, decorators, type, dst, src, tmp1, tmp2);
  } else {
    bs->load_at(this, decorators, type, dst, src, tmp1, tmp2);
  }
}

void MacroAssembler::access_store_at(BasicType type, DecoratorSet decorators, Address dst, Register val,
                                     Register tmp1, Register tmp2, Register tmp3) {
  BarrierSetAssembler* bs = BarrierSet::barrier_set()->barrier_set_assembler();
  decorators = AccessInternal::decorator_fixup(decorators, type);
  bool as_raw = (decorators & AS_RAW) != 0;
  if (as_raw) {
    bs->BarrierSetAssembler::store_at(this, decorators, type, dst, val, tmp1, tmp2, tmp3);
  } else {
    bs->store_at(this, decorators, type, dst, val, tmp1, tmp2, tmp3);
  }
}

void MacroAssembler::load_heap_oop(Register dst, Address src, Register tmp1,
                                   Register tmp2, DecoratorSet decorators) {
  access_load_at(T_OBJECT, IN_HEAP | decorators, dst, src, tmp1, tmp2);
}

// Doesn't do verification, generates fixed size code
void MacroAssembler::load_heap_oop_not_null(Register dst, Address src, Register tmp1,
                                            Register tmp2, DecoratorSet decorators) {
  access_load_at(T_OBJECT, IN_HEAP | IS_NOT_NULL | decorators, dst, src, tmp1, tmp2);
}

void MacroAssembler::store_heap_oop(Address dst, Register val, Register tmp1,
                                    Register tmp2, Register tmp3, DecoratorSet decorators) {
  access_store_at(T_OBJECT, IN_HEAP | decorators, dst, val, tmp1, tmp2, tmp3);
}

// Used for storing NULLs.
void MacroAssembler::store_heap_oop_null(Address dst) {
  access_store_at(T_OBJECT, IN_HEAP, dst, noreg, noreg, noreg, noreg);
}

#ifdef ASSERT
void MacroAssembler::verify_heapbase(const char* msg) {
  assert (UseCompressedOops || UseCompressedClassPointers, "should be compressed");
  assert (Universe::heap() != nullptr, "java heap should be initialized");
}
#endif

// Algorithm must match oop.inline.hpp encode_heap_oop.
void MacroAssembler::encode_heap_oop(Register r) {
#ifdef ASSERT
  verify_heapbase("MacroAssembler::encode_heap_oop:heap base corrupted?");
#endif
  verify_oop_msg(r, "broken oop in encode_heap_oop");
  if (CompressedOops::base() == nullptr) {
    if (CompressedOops::shift() != 0) {
      assert(LogMinObjAlignmentInBytes == CompressedOops::shift(), "decode alg wrong");
      srli_d(r, r, LogMinObjAlignmentInBytes);
    }
    return;
  }

  sub_d(AT, r, S5_heapbase);
  maskeqz(r, AT, r);
  if (CompressedOops::shift() != 0) {
    assert(LogMinObjAlignmentInBytes == CompressedOops::shift(), "decode alg wrong");
    srli_d(r, r, LogMinObjAlignmentInBytes);
  }
}

void MacroAssembler::encode_heap_oop(Register dst, Register src) {
#ifdef ASSERT
  verify_heapbase("MacroAssembler::encode_heap_oop:heap base corrupted?");
#endif
  verify_oop_msg(src, "broken oop in encode_heap_oop");
  if (CompressedOops::base() == nullptr) {
    if (CompressedOops::shift() != 0) {
      assert(LogMinObjAlignmentInBytes == CompressedOops::shift(), "decode alg wrong");
      srli_d(dst, src, LogMinObjAlignmentInBytes);
    } else {
      if (dst != src) {
        move(dst, src);
      }
    }
    return;
  }

  sub_d(AT, src, S5_heapbase);
  maskeqz(dst, AT, src);
  if (CompressedOops::shift() != 0) {
    assert(LogMinObjAlignmentInBytes == CompressedOops::shift(), "decode alg wrong");
    srli_d(dst, dst, LogMinObjAlignmentInBytes);
  }
}

void MacroAssembler::encode_heap_oop_not_null(Register r) {
  assert (UseCompressedOops, "should be compressed");
#ifdef ASSERT
  if (CheckCompressedOops) {
    Label ok;
    bne(r, R0, ok);
    stop("null oop passed to encode_heap_oop_not_null");
    bind(ok);
  }
#endif
  verify_oop_msg(r, "broken oop in encode_heap_oop_not_null");
  if (CompressedOops::base() != nullptr) {
    sub_d(r, r, S5_heapbase);
  }
  if (CompressedOops::shift() != 0) {
    assert (LogMinObjAlignmentInBytes == CompressedOops::shift(), "decode alg wrong");
    srli_d(r, r, LogMinObjAlignmentInBytes);
  }

}

void MacroAssembler::encode_heap_oop_not_null(Register dst, Register src) {
  assert (UseCompressedOops, "should be compressed");
#ifdef ASSERT
  if (CheckCompressedOops) {
    Label ok;
    bne(src, R0, ok);
    stop("null oop passed to encode_heap_oop_not_null2");
    bind(ok);
  }
#endif
  verify_oop_msg(src, "broken oop in encode_heap_oop_not_null2");
  if (CompressedOops::base() == nullptr) {
    if (CompressedOops::shift() != 0) {
      assert(LogMinObjAlignmentInBytes == CompressedOops::shift(), "decode alg wrong");
      srli_d(dst, src, LogMinObjAlignmentInBytes);
    } else {
      if (dst != src) {
        move(dst, src);
      }
    }
    return;
  }

  sub_d(dst, src, S5_heapbase);
  if (CompressedOops::shift() != 0) {
    assert(LogMinObjAlignmentInBytes == CompressedOops::shift(), "decode alg wrong");
    srli_d(dst, dst, LogMinObjAlignmentInBytes);
  }
}

void MacroAssembler::decode_heap_oop(Register r) {
#ifdef ASSERT
  verify_heapbase("MacroAssembler::decode_heap_oop corrupted?");
#endif
  if (CompressedOops::base() == nullptr) {
    if (CompressedOops::shift() != 0) {
      assert(LogMinObjAlignmentInBytes == CompressedOops::shift(), "decode alg wrong");
      slli_d(r, r, LogMinObjAlignmentInBytes);
    }
    return;
  }

  move(AT, r);
  if (CompressedOops::shift() != 0) {
    assert(LogMinObjAlignmentInBytes == CompressedOops::shift(), "decode alg wrong");
    if (LogMinObjAlignmentInBytes <= 4) {
      alsl_d(r, r, S5_heapbase, LogMinObjAlignmentInBytes - 1);
    } else {
      slli_d(r, r, LogMinObjAlignmentInBytes);
      add_d(r, r, S5_heapbase);
    }
  } else {
    add_d(r, r, S5_heapbase);
  }
  maskeqz(r, r, AT);
  verify_oop_msg(r, "broken oop in decode_heap_oop");
}

void MacroAssembler::decode_heap_oop(Register dst, Register src) {
#ifdef ASSERT
  verify_heapbase("MacroAssembler::decode_heap_oop corrupted?");
#endif
  if (CompressedOops::base() == nullptr) {
    if (CompressedOops::shift() != 0) {
      assert(LogMinObjAlignmentInBytes == CompressedOops::shift(), "decode alg wrong");
      slli_d(dst, src, LogMinObjAlignmentInBytes);
    } else {
      if (dst != src) {
        move(dst, src);
      }
    }
    return;
  }

  Register cond;
  if (dst == src) {
    cond = AT;
    move(cond, src);
  } else {
    cond = src;
  }
  if (CompressedOops::shift() != 0) {
    assert(LogMinObjAlignmentInBytes == CompressedOops::shift(), "decode alg wrong");
    if (LogMinObjAlignmentInBytes <= 4) {
      alsl_d(dst, src, S5_heapbase, LogMinObjAlignmentInBytes - 1);
    } else {
      slli_d(dst, src, LogMinObjAlignmentInBytes);
      add_d(dst, dst, S5_heapbase);
    }
  } else {
    add_d(dst, src, S5_heapbase);
  }
  maskeqz(dst, dst, cond);
  verify_oop_msg(dst, "broken oop in decode_heap_oop");
}

void MacroAssembler::decode_heap_oop_not_null(Register r) {
  // Note: it will change flags
  assert(UseCompressedOops, "should only be used for compressed headers");
  assert(Universe::heap() != nullptr, "java heap should be initialized");
  // Cannot assert, unverified entry point counts instructions (see .ad file)
  // vtableStubs also counts instructions in pd_code_size_limit.
  // Also do not verify_oop as this is called by verify_oop.
  if (CompressedOops::shift() != 0) {
    assert(LogMinObjAlignmentInBytes == CompressedOops::shift(), "decode alg wrong");
    if (CompressedOops::base() != nullptr) {
      if (LogMinObjAlignmentInBytes <= 4) {
        alsl_d(r, r, S5_heapbase, LogMinObjAlignmentInBytes - 1);
      } else {
        slli_d(r, r, LogMinObjAlignmentInBytes);
        add_d(r, r, S5_heapbase);
      }
    } else {
      slli_d(r, r, LogMinObjAlignmentInBytes);
    }
  } else {
    assert(CompressedOops::base() == nullptr, "sanity");
  }
}

void MacroAssembler::decode_heap_oop_not_null(Register dst, Register src) {
  assert(UseCompressedOops, "should only be used for compressed headers");
  assert(Universe::heap() != nullptr, "java heap should be initialized");
  // Cannot assert, unverified entry point counts instructions (see .ad file)
  // vtableStubs also counts instructions in pd_code_size_limit.
  // Also do not verify_oop as this is called by verify_oop.
  if (CompressedOops::shift() != 0) {
    assert(LogMinObjAlignmentInBytes == CompressedOops::shift(), "decode alg wrong");
    if (CompressedOops::base() != nullptr) {
      if (LogMinObjAlignmentInBytes <= 4) {
        alsl_d(dst, src, S5_heapbase, LogMinObjAlignmentInBytes - 1);
      } else {
        slli_d(dst, src, LogMinObjAlignmentInBytes);
        add_d(dst, dst, S5_heapbase);
      }
    } else {
      slli_d(dst, src, LogMinObjAlignmentInBytes);
    }
  } else {
    assert (CompressedOops::base() == nullptr, "sanity");
    if (dst != src) {
      move(dst, src);
    }
  }
}

void MacroAssembler::encode_klass_not_null(Register r) {
  if (CompressedKlassPointers::base() != nullptr) {
    if (((uint64_t)CompressedKlassPointers::base() & 0xffffffff) == 0
        && CompressedKlassPointers::shift() == 0) {
      bstrpick_d(r, r, 31, 0);
      return;
    }
    assert(r != AT, "Encoding a klass in AT");
    li(AT, (int64_t)CompressedKlassPointers::base());
    sub_d(r, r, AT);
  }
  if (CompressedKlassPointers::shift() != 0) {
    assert (LogKlassAlignmentInBytes == CompressedKlassPointers::shift(), "decode alg wrong");
    srli_d(r, r, LogKlassAlignmentInBytes);
  }
}

void MacroAssembler::encode_klass_not_null(Register dst, Register src) {
  if (dst == src) {
    encode_klass_not_null(src);
  } else {
    if (CompressedKlassPointers::base() != nullptr) {
      if (((uint64_t)CompressedKlassPointers::base() & 0xffffffff) == 0
          && CompressedKlassPointers::shift() == 0) {
        bstrpick_d(dst, src, 31, 0);
        return;
      }
      li(dst, (int64_t)CompressedKlassPointers::base());
      sub_d(dst, src, dst);
      if (CompressedKlassPointers::shift() != 0) {
        assert (LogKlassAlignmentInBytes == CompressedKlassPointers::shift(), "decode alg wrong");
        srli_d(dst, dst, LogKlassAlignmentInBytes);
      }
    } else {
      if (CompressedKlassPointers::shift() != 0) {
        assert (LogKlassAlignmentInBytes == CompressedKlassPointers::shift(), "decode alg wrong");
        srli_d(dst, src, LogKlassAlignmentInBytes);
      } else {
        move(dst, src);
      }
    }
  }
}

void MacroAssembler::decode_klass_not_null(Register r) {
  assert(UseCompressedClassPointers, "should only be used for compressed headers");
  assert(r != AT, "Decoding a klass in AT");
  // Cannot assert, unverified entry point counts instructions (see .ad file)
  // vtableStubs also counts instructions in pd_code_size_limit.
  // Also do not verify_oop as this is called by verify_oop.
  if (CompressedKlassPointers::base() != nullptr) {
    if (CompressedKlassPointers::shift() == 0) {
      if (((uint64_t)CompressedKlassPointers::base() & 0xffffffff) == 0) {
        lu32i_d(r, (uint64_t)CompressedKlassPointers::base() >> 32);
      } else {
        li(AT, (int64_t)CompressedKlassPointers::base());
        add_d(r, r, AT);
      }
    } else {
      assert(LogKlassAlignmentInBytes == CompressedKlassPointers::shift(), "decode alg wrong");
      assert(LogKlassAlignmentInBytes == Address::times_8, "klass not aligned on 64bits?");
      li(AT, (int64_t)CompressedKlassPointers::base());
      alsl_d(r, r, AT, Address::times_8 - 1);
    }
  } else {
    if (CompressedKlassPointers::shift() != 0) {
      assert(LogKlassAlignmentInBytes == CompressedKlassPointers::shift(), "decode alg wrong");
      slli_d(r, r, LogKlassAlignmentInBytes);
    }
  }
}

void MacroAssembler::decode_klass_not_null(Register dst, Register src) {
  assert(UseCompressedClassPointers, "should only be used for compressed headers");
  if (dst == src) {
    decode_klass_not_null(dst);
  } else {
    // Cannot assert, unverified entry point counts instructions (see .ad file)
    // vtableStubs also counts instructions in pd_code_size_limit.
    // Also do not verify_oop as this is called by verify_oop.
    if (CompressedKlassPointers::base() != nullptr) {
      if (CompressedKlassPointers::shift() == 0) {
        if (((uint64_t)CompressedKlassPointers::base() & 0xffffffff) == 0) {
          move(dst, src);
          lu32i_d(dst, (uint64_t)CompressedKlassPointers::base() >> 32);
        } else {
          li(dst, (int64_t)CompressedKlassPointers::base());
          add_d(dst, dst, src);
        }
      } else {
        assert(LogKlassAlignmentInBytes == CompressedKlassPointers::shift(), "decode alg wrong");
        assert(LogKlassAlignmentInBytes == Address::times_8, "klass not aligned on 64bits?");
        li(dst, (int64_t)CompressedKlassPointers::base());
        alsl_d(dst, src, dst, Address::times_8 - 1);
      }
    } else {
      if (CompressedKlassPointers::shift() != 0) {
        assert(LogKlassAlignmentInBytes == CompressedKlassPointers::shift(), "decode alg wrong");
        slli_d(dst, src, LogKlassAlignmentInBytes);
      } else {
        move(dst, src);
      }
    }
  }
}

void MacroAssembler::reinit_heapbase() {
  if (UseCompressedOops) {
    if (Universe::heap() != nullptr) {
      if (CompressedOops::base() == nullptr) {
        move(S5_heapbase, R0);
      } else {
        li(S5_heapbase, (int64_t)CompressedOops::ptrs_base());
      }
    } else {
      li(S5_heapbase, (intptr_t)CompressedOops::ptrs_base_addr());
      ld_d(S5_heapbase, S5_heapbase, 0);
    }
  }
}

void MacroAssembler::check_klass_subtype(Register sub_klass,
                           Register super_klass,
                           Register temp_reg,
                           Label& L_success) {
//implement ind   gen_subtype_check
  Label L_failure;
  check_klass_subtype_fast_path(sub_klass, super_klass, temp_reg,        &L_success, &L_failure, nullptr);
  check_klass_subtype_slow_path<false>(sub_klass, super_klass, temp_reg, noreg, &L_success, nullptr);
  bind(L_failure);
}

void MacroAssembler::check_klass_subtype_fast_path(Register sub_klass,
                                                   Register super_klass,
                                                   Register temp_reg,
                                                   Label* L_success,
                                                   Label* L_failure,
                                                   Label* L_slow_path,
                                        RegisterOrConstant super_check_offset) {
  assert_different_registers(sub_klass, super_klass, temp_reg);
  bool must_load_sco = (super_check_offset.constant_or_zero() == -1);
  if (super_check_offset.is_register()) {
    assert_different_registers(sub_klass, super_klass,
                               super_check_offset.as_register());
  } else if (must_load_sco) {
    assert(temp_reg != noreg, "supply either a temp or a register offset");
  }

  Label L_fallthrough;
  int label_nulls = 0;
  if (L_success == nullptr)   { L_success   = &L_fallthrough; label_nulls++; }
  if (L_failure == nullptr)   { L_failure   = &L_fallthrough; label_nulls++; }
  if (L_slow_path == nullptr) { L_slow_path = &L_fallthrough; label_nulls++; }
  assert(label_nulls <= 1, "at most one null in the batch");

  int sc_offset = in_bytes(Klass::secondary_super_cache_offset());
  int sco_offset = in_bytes(Klass::super_check_offset_offset());
  // If the pointers are equal, we are done (e.g., String[] elements).
  // This self-check enables sharing of secondary supertype arrays among
  // non-primary types such as array-of-interface.  Otherwise, each such
  // type would need its own customized SSA.
  // We move this check to the front of the fast path because many
  // type checks are in fact trivially successful in this manner,
  // so we get a nicely predicted branch right at the start of the check.
  beq(sub_klass, super_klass, *L_success);
  // Check the supertype display:
  if (must_load_sco) {
    ld_wu(temp_reg, super_klass, sco_offset);
    super_check_offset = RegisterOrConstant(temp_reg);
  }
  add_d(AT, sub_klass, super_check_offset.register_or_noreg());
  ld_d(AT, AT, super_check_offset.constant_or_zero());

  // This check has worked decisively for primary supers.
  // Secondary supers are sought in the super_cache ('super_cache_addr').
  // (Secondary supers are interfaces and very deeply nested subtypes.)
  // This works in the same check above because of a tricky aliasing
  // between the super_cache and the primary super display elements.
  // (The 'super_check_addr' can address either, as the case requires.)
  // Note that the cache is updated below if it does not help us find
  // what we need immediately.
  // So if it was a primary super, we can just fail immediately.
  // Otherwise, it's the slow path for us (no success at this point).

  if (super_check_offset.is_register()) {
    beq(super_klass, AT, *L_success);
    addi_d(AT, super_check_offset.as_register(), -sc_offset);
    if (L_failure == &L_fallthrough) {
      beq(AT, R0, *L_slow_path);
    } else {
      bne_far(AT, R0, *L_failure);
      b(*L_slow_path);
    }
  } else if (super_check_offset.as_constant() == sc_offset) {
    // Need a slow path; fast failure is impossible.
    if (L_slow_path == &L_fallthrough) {
      beq(super_klass, AT, *L_success);
    } else {
      bne(super_klass, AT, *L_slow_path);
      b(*L_success);
    }
  } else {
    // No slow path; it's a fast decision.
    if (L_failure == &L_fallthrough) {
      beq(super_klass, AT, *L_success);
    } else {
      bne_far(super_klass, AT, *L_failure);
      b(*L_success);
    }
  }

  bind(L_fallthrough);
}

template
void MacroAssembler::check_klass_subtype_slow_path<false>(Register sub_klass,
                                                   Register super_klass,
                                                   Register temp_reg,
                                                   Register temp2_reg,
                                                   Label* L_success,
                                                   Label* L_failure,
                                                   bool set_cond_codes);
template
void MacroAssembler::check_klass_subtype_slow_path<true>(Register sub_klass,
                                                   Register super_klass,
                                                   Register temp_reg,
                                                   Register temp2_reg,
                                                   Label* L_success,
                                                   Label* L_failure,
                                                   bool set_cond_codes);
template <bool LONG_JMP>
void MacroAssembler::check_klass_subtype_slow_path(Register sub_klass,
                                                   Register super_klass,
                                                   Register temp_reg,
                                                   Register temp2_reg,
                                                   Label* L_success,
                                                   Label* L_failure,
                                                   bool set_cond_codes) {
  if (!LONG_JMP) {
    if (temp2_reg == noreg)
      temp2_reg = TSR;
  }
  assert_different_registers(sub_klass, super_klass, temp_reg, temp2_reg);
#define IS_A_TEMP(reg) ((reg) == temp_reg || (reg) == temp2_reg)

  Label L_fallthrough;
  int label_nulls = 0;
  if (L_success == nullptr)   { L_success   = &L_fallthrough; label_nulls++; }
  if (L_failure == nullptr)   { L_failure   = &L_fallthrough; label_nulls++; }
  assert(label_nulls <= 1, "at most one null in the batch");

  // a couple of useful fields in sub_klass:
  int ss_offset = in_bytes(Klass::secondary_supers_offset());
  int sc_offset = in_bytes(Klass::secondary_super_cache_offset());
  Address secondary_supers_addr(sub_klass, ss_offset);
  Address super_cache_addr(     sub_klass, sc_offset);

  // Do a linear scan of the secondary super-klass chain.
  // This code is rarely used, so simplicity is a virtue here.
  // The repne_scan instruction uses fixed registers, which we must spill.
  // Don't worry too much about pre-existing connections with the input regs.

#ifndef PRODUCT
  int* pst_counter = &SharedRuntime::_partial_subtype_ctr;
  ExternalAddress pst_counter_addr((address) pst_counter);
#endif //PRODUCT

  // We will consult the secondary-super array.
  ld_d(temp_reg, secondary_supers_addr);
  // Load the array length.
  ld_w(temp2_reg, Address(temp_reg, Array<Klass*>::length_offset_in_bytes()));
  // Skip to start of data.
  addi_d(temp_reg, temp_reg, Array<Klass*>::base_offset_in_bytes());

  Label Loop, subtype;
  bind(Loop);

  if (LONG_JMP) {
    Label not_taken;
    bne(temp2_reg, R0, not_taken);
    jmp_far(*L_failure);
    bind(not_taken);
  } else {
    beqz(temp2_reg, *L_failure);
  }

  ld_d(AT, temp_reg, 0);
  addi_d(temp_reg, temp_reg, 1 * wordSize);
  beq(AT, super_klass, subtype);
  addi_d(temp2_reg, temp2_reg, -1);
  b(Loop);

  bind(subtype);
  st_d(super_klass, super_cache_addr);
  if (L_success != &L_fallthrough) {
    if (LONG_JMP)
      jmp_far(*L_success);
    else
      b(*L_success);
  }

  // Success.  Cache the super we found and proceed in triumph.
#undef IS_A_TEMP

  bind(L_fallthrough);
}

void MacroAssembler::clinit_barrier(Register klass, Register scratch, Label* L_fast_path, Label* L_slow_path) {

  assert(L_fast_path != nullptr || L_slow_path != nullptr, "at least one is required");
  assert_different_registers(klass, TREG, scratch);

  Label L_fallthrough;
  if (L_fast_path == nullptr) {
    L_fast_path = &L_fallthrough;
  } else if (L_slow_path == nullptr) {
    L_slow_path = &L_fallthrough;
  }

  // Fast path check: class is fully initialized
  ld_b(scratch, Address(klass, InstanceKlass::init_state_offset()));
  addi_d(scratch, scratch, -InstanceKlass::fully_initialized);
  beqz(scratch, *L_fast_path);

  // Fast path check: current thread is initializer thread
  ld_d(scratch, Address(klass, InstanceKlass::init_thread_offset()));
  if (L_slow_path == &L_fallthrough) {
    beq(TREG, scratch, *L_fast_path);
    bind(*L_slow_path);
  } else if (L_fast_path == &L_fallthrough) {
    bne(TREG, scratch, *L_slow_path);
    bind(*L_fast_path);
  } else {
    Unimplemented();
  }
}

void MacroAssembler::get_vm_result(Register oop_result, Register java_thread) {
  ld_d(oop_result, Address(java_thread, JavaThread::vm_result_offset()));
  st_d(R0, Address(java_thread, JavaThread::vm_result_offset()));
  verify_oop_msg(oop_result, "broken oop in call_VM_base");
}

void MacroAssembler::get_vm_result_2(Register metadata_result, Register java_thread) {
  ld_d(metadata_result, Address(java_thread, JavaThread::vm_result_2_offset()));
  st_d(R0, Address(java_thread, JavaThread::vm_result_2_offset()));
}

Address MacroAssembler::argument_address(RegisterOrConstant arg_slot,
                                         int extra_slot_offset) {
  // cf. TemplateTable::prepare_invoke(), if (load_receiver).
  int stackElementSize = Interpreter::stackElementSize;
  int offset = Interpreter::expr_offset_in_bytes(extra_slot_offset+0);
#ifdef ASSERT
  int offset1 = Interpreter::expr_offset_in_bytes(extra_slot_offset+1);
  assert(offset1 - offset == stackElementSize, "correct arithmetic");
#endif
  Register             scale_reg    = noreg;
  Address::ScaleFactor scale_factor = Address::no_scale;
  if (arg_slot.is_constant()) {
    offset += arg_slot.as_constant() * stackElementSize;
  } else {
    scale_reg    = arg_slot.as_register();
    scale_factor = Address::times(stackElementSize);
  }
  return Address(SP, scale_reg, scale_factor, offset);
}

SkipIfEqual::~SkipIfEqual() {
  _masm->bind(_label);
}

void MacroAssembler::load_sized_value(Register dst, Address src, size_t size_in_bytes, bool is_signed, Register dst2) {
  switch (size_in_bytes) {
  case  8:  ld_d(dst, src); break;
  case  4:  ld_w(dst, src); break;
  case  2:  is_signed ? ld_h(dst, src) : ld_hu(dst, src); break;
  case  1:  is_signed ? ld_b( dst, src) : ld_bu( dst, src); break;
  default:  ShouldNotReachHere();
  }
}

void MacroAssembler::store_sized_value(Address dst, Register src, size_t size_in_bytes, Register src2) {
  switch (size_in_bytes) {
  case  8:  st_d(src, dst); break;
  case  4:  st_w(src, dst); break;
  case  2:  st_h(src, dst); break;
  case  1:  st_b(src, dst); break;
  default:  ShouldNotReachHere();
  }
}

// Look up the method for a megamorphic invokeinterface call.
// The target method is determined by <intf_klass, itable_index>.
// The receiver klass is in recv_klass.
// On success, the result will be in method_result, and execution falls through.
// On failure, execution transfers to the given label.
void MacroAssembler::lookup_interface_method(Register recv_klass,
                                             Register intf_klass,
                                             RegisterOrConstant itable_index,
                                             Register method_result,
                                             Register scan_temp,
                                             Label& L_no_such_interface,
                                             bool return_method) {
  assert_different_registers(recv_klass, intf_klass, scan_temp, AT);
  assert_different_registers(method_result, intf_klass, scan_temp, AT);
  assert(recv_klass != method_result || !return_method,
         "recv_klass can be destroyed when method isn't needed");

  assert(itable_index.is_constant() || itable_index.as_register() == method_result,
         "caller must use same register for non-constant itable index as for method");

  // Compute start of first itableOffsetEntry (which is at the end of the vtable)
  int vtable_base = in_bytes(Klass::vtable_start_offset());
  int itentry_off = in_bytes(itableMethodEntry::method_offset());
  int scan_step   = itableOffsetEntry::size() * wordSize;
  int vte_size    = vtableEntry::size() * wordSize;
  Address::ScaleFactor times_vte_scale = Address::times_ptr;
  assert(vte_size == wordSize, "else adjust times_vte_scale");

  ld_w(scan_temp, Address(recv_klass, Klass::vtable_length_offset()));

  // %%% Could store the aligned, prescaled offset in the klassoop.
  alsl_d(scan_temp, scan_temp, recv_klass, times_vte_scale - 1);
  addi_d(scan_temp, scan_temp, vtable_base);

  if (return_method) {
    // Adjust recv_klass by scaled itable_index, so we can free itable_index.
    if (itable_index.is_constant()) {
      li(AT, (itable_index.as_constant() * itableMethodEntry::size() * wordSize) + itentry_off);
      add_d(recv_klass, recv_klass, AT);
    } else {
      assert(itableMethodEntry::size() * wordSize == wordSize, "adjust the scaling in the code below");
      alsl_d(AT, itable_index.as_register(), recv_klass, (int)Address::times_ptr - 1);
      addi_d(recv_klass, AT, itentry_off);
    }
  }

  Label search, found_method;

  ld_d(method_result, Address(scan_temp, itableOffsetEntry::interface_offset()));
  beq(intf_klass, method_result, found_method);

  bind(search);
  // Check that the previous entry is non-null.  A null entry means that
  // the receiver class doesn't implement the interface, and wasn't the
  // same as when the caller was compiled.
  beqz(method_result, L_no_such_interface);
  addi_d(scan_temp, scan_temp, scan_step);
  ld_d(method_result, Address(scan_temp, itableOffsetEntry::interface_offset()));
  bne(intf_klass, method_result, search);

  bind(found_method);
  if (return_method) {
    // Got a hit.
    ld_wu(scan_temp, Address(scan_temp, itableOffsetEntry::offset_offset()));
    ldx_d(method_result, recv_klass, scan_temp);
  }
}

// virtual method calling
void MacroAssembler::lookup_virtual_method(Register recv_klass,
                                           RegisterOrConstant vtable_index,
                                           Register method_result) {
  assert(vtableEntry::size() * wordSize == wordSize, "else adjust the scaling in the code below");

  if (vtable_index.is_constant()) {
    li(AT, vtable_index.as_constant());
    alsl_d(AT, AT, recv_klass, Address::times_ptr - 1);
  } else {
    alsl_d(AT, vtable_index.as_register(), recv_klass, Address::times_ptr - 1);
  }

  ld_d(method_result, AT, in_bytes(Klass::vtable_start_offset() + vtableEntry::method_offset()));
}

void MacroAssembler::load_byte_map_base(Register reg) {
  CardTable::CardValue* byte_map_base =
    ((CardTableBarrierSet*)(BarrierSet::barrier_set()))->card_table()->byte_map_base();

  // Strictly speaking the byte_map_base isn't an address at all, and it might
  // even be negative. It is thus materialised as a constant.
  li(reg, (uint64_t)byte_map_base);
}

void MacroAssembler::resolve_jobject(Register value, Register tmp1, Register tmp2) {
  assert_different_registers(value, tmp1, tmp2);
  Label done, tagged, weak_tagged;

  beqz(value, done);                // Use null as-is.
  // Test for tag.
  andi(AT, value, JNIHandles::tag_mask);
  bnez(AT, tagged);

  // Resolve local handle
  access_load_at(T_OBJECT, IN_NATIVE | AS_RAW, value, Address(value, 0), tmp1, tmp2);
  verify_oop(value);
  b(done);

  bind(tagged);
  // Test for jweak tag.
  andi(AT, value, JNIHandles::TypeTag::weak_global);
  bnez(AT, weak_tagged);

  // Resolve global handle
  access_load_at(T_OBJECT, IN_NATIVE, value,
                 Address(value, -JNIHandles::TypeTag::global), tmp1, tmp2);
  verify_oop(value);
  b(done);

  bind(weak_tagged);
  // Resolve jweak.
  access_load_at(T_OBJECT, IN_NATIVE | ON_PHANTOM_OOP_REF,
                 value, Address(value, -JNIHandles::TypeTag::weak_global), tmp1, tmp2);
  verify_oop(value);
  bind(done);
}

void MacroAssembler::resolve_global_jobject(Register value, Register tmp1, Register tmp2) {
  assert_different_registers(value, tmp1, tmp2);
  Label done;

  beqz(value, done);           // Use null as-is.

#ifdef ASSERT
  {
    Label valid_global_tag;
    andi(AT, value, JNIHandles::TypeTag::global); // Test for global tag.
    bnez(AT, valid_global_tag);
    stop("non global jobject using resolve_global_jobject");
    bind(valid_global_tag);
  }
#endif

  // Resolve global handle
  access_load_at(T_OBJECT, IN_NATIVE, value,
                 Address(value, -JNIHandles::TypeTag::global), tmp1, tmp2);
  verify_oop(value);

  bind(done);
}

void MacroAssembler::lea(Register rd, Address src) {
  Register dst   = rd;
  Register base  = src.base();
  Register index = src.index();

  int scale = src.scale();
  int disp  = src.disp();

  if (index == noreg) {
    if (is_simm(disp, 12)) {
      addi_d(dst, base, disp);
    } else {
      lu12i_w(AT, split_low20(disp >> 12));
      if (split_low12(disp))
        ori(AT, AT, split_low12(disp));
      add_d(dst, base, AT);
    }
  } else {
    if (scale == 0) {
      if (disp == 0) {
        add_d(dst, base, index);
      } else if (is_simm(disp, 12)) {
        add_d(AT, base, index);
        addi_d(dst, AT, disp);
      } else {
        lu12i_w(AT, split_low20(disp >> 12));
        if (split_low12(disp))
          ori(AT, AT, split_low12(disp));
        add_d(AT, base, AT);
        add_d(dst, AT, index);
      }
    } else {
      if (disp == 0) {
        alsl_d(dst, index, base, scale - 1);
      } else if (is_simm(disp, 12)) {
        alsl_d(AT, index, base, scale - 1);
        addi_d(dst, AT, disp);
      } else {
        lu12i_w(AT, split_low20(disp >> 12));
        if (split_low12(disp))
          ori(AT, AT, split_low12(disp));
        add_d(AT, AT, base);
        alsl_d(dst, index, AT, scale - 1);
      }
    }
  }
}

void MacroAssembler::lea(Register dst, AddressLiteral adr) {
  code_section()->relocate(pc(), adr.rspec());
  pcaddi(dst, (adr.target() - pc()) >> 2);
}

void MacroAssembler::lea_long(Register dst, AddressLiteral adr) {
  code_section()->relocate(pc(), adr.rspec());
  jint si12, si20;
  split_simm32((adr.target() - pc()), si12, si20);
  pcaddu12i(dst, si20);
  addi_d(dst, dst, si12);
}

int MacroAssembler::patched_branch(int dest_pos, int inst, int inst_pos) {
  int v = (dest_pos - inst_pos) >> 2;
  switch(high(inst, 6)) {
  case beq_op:
  case bne_op:
  case blt_op:
  case bge_op:
  case bltu_op:
  case bgeu_op:
#ifndef PRODUCT
    if(!is_simm16(v))
    {
      tty->print_cr("must be simm16");
      tty->print_cr("Inst: %x", inst);
      tty->print_cr("Op:   %x", high(inst, 6));
    }
#endif
    assert(is_simm16(v), "must be simm16");

    inst &= 0xfc0003ff;
    inst |= ((v & 0xffff) << 10);
    break;
  case beqz_op:
  case bnez_op:
  case bccondz_op:
    assert(is_simm(v, 21), "must be simm21");
#ifndef PRODUCT
    if(!is_simm(v, 21))
    {
      tty->print_cr("must be simm21");
      tty->print_cr("Inst: %x", inst);
    }
#endif

    inst &= 0xfc0003e0;
    inst |= ( ((v & 0xffff) << 10) | ((v >> 16) & 0x1f) );
    break;
  case b_op:
  case bl_op:
    assert(is_simm(v, 26), "must be simm26");
#ifndef PRODUCT
    if(!is_simm(v, 26))
    {
      tty->print_cr("must be simm26");
      tty->print_cr("Inst: %x", inst);
    }
#endif

    inst &= 0xfc000000;
    inst |= ( ((v & 0xffff) << 10) | ((v >> 16) & 0x3ff) );
    break;
  default:
    ShouldNotReachHere();
    break;
  }
  return inst;
}

void MacroAssembler::cmp_cmov_zero(Register  op1,
                              Register  op2,
                              Register  dst,
                              Register  src,
                              CMCompare cmp,
                              bool      is_signed) {
  switch (cmp) {
    case EQ:
      sub_d(AT, op1, op2);
      maskeqz(dst, src, AT);
      break;

    case NE:
      sub_d(AT, op1, op2);
      masknez(dst, src, AT);
      break;

    case GT:
      if (is_signed) {
        slt(AT, op2, op1);
      } else {
        sltu(AT, op2, op1);
      }
      masknez(dst, src, AT);
      break;

    case GE:
      if (is_signed) {
        slt(AT, op1, op2);
      } else {
        sltu(AT, op1, op2);
      }
      maskeqz(dst, src, AT);
      break;

    case LT:
      if (is_signed) {
        slt(AT, op1, op2);
      } else {
        sltu(AT, op1, op2);
      }
      masknez(dst, src, AT);
      break;

    case LE:
      if (is_signed) {
        slt(AT, op2, op1);
      } else {
        sltu(AT, op2, op1);
      }
      maskeqz(dst, src, AT);
      break;

    default:
      Unimplemented();
  }
}

void MacroAssembler::cmp_cmov(Register  op1,
                              Register  op2,
                              Register  dst,
                              Register  src1,
                              Register  src2,
                              CMCompare cmp,
                              bool      is_signed) {
  switch (cmp) {
    case EQ:
      sub_d(AT, op1, op2);
      if (dst == src2) {
        masknez(dst, src2, AT);
        maskeqz(AT, src1, AT);
      } else {
        maskeqz(dst, src1, AT);
        masknez(AT, src2, AT);
      }
      break;

    case NE:
      sub_d(AT, op1, op2);
      if (dst == src2) {
        maskeqz(dst, src2, AT);
        masknez(AT, src1, AT);
      } else {
        masknez(dst, src1, AT);
        maskeqz(AT, src2, AT);
      }
      break;

    case GT:
      if (is_signed) {
        slt(AT, op2, op1);
      } else {
        sltu(AT, op2, op1);
      }
      if(dst == src2) {
        maskeqz(dst, src2, AT);
        masknez(AT, src1, AT);
      } else {
        masknez(dst, src1, AT);
        maskeqz(AT, src2, AT);
      }
      break;
    case GE:
      if (is_signed) {
        slt(AT, op1, op2);
      } else {
        sltu(AT, op1, op2);
      }
      if(dst == src2) {
        masknez(dst, src2, AT);
        maskeqz(AT, src1, AT);
      } else {
        maskeqz(dst, src1, AT);
        masknez(AT, src2, AT);
      }
      break;

    case LT:
      if (is_signed) {
        slt(AT, op1, op2);
      } else {
        sltu(AT, op1, op2);
      }
      if(dst == src2) {
        maskeqz(dst, src2, AT);
        masknez(AT, src1, AT);
      } else {
        masknez(dst, src1, AT);
        maskeqz(AT, src2, AT);
      }
      break;
    case LE:
      if (is_signed) {
        slt(AT, op2, op1);
      } else {
        sltu(AT, op2, op1);
      }
      if(dst == src2) {
        masknez(dst, src2, AT);
        maskeqz(AT, src1, AT);
      } else {
        maskeqz(dst, src1, AT);
        masknez(AT, src2, AT);
      }
      break;
    default:
      Unimplemented();
  }
  OR(dst, dst, AT);
}

void MacroAssembler::cmp_cmov(Register  op1,
                              Register  op2,
                              Register  dst,
                              Register  src,
                              CMCompare cmp,
                              bool      is_signed) {
  switch (cmp) {
    case EQ:
      sub_d(AT, op1, op2);
      maskeqz(dst, dst, AT);
      masknez(AT, src, AT);
      break;

    case NE:
      sub_d(AT, op1, op2);
      masknez(dst, dst, AT);
      maskeqz(AT, src, AT);
      break;

    case GT:
      if (is_signed) {
        slt(AT, op2, op1);
      } else {
        sltu(AT, op2, op1);
      }
      masknez(dst, dst, AT);
      maskeqz(AT, src, AT);
      break;

    case GE:
      if (is_signed) {
        slt(AT, op1, op2);
      } else {
        sltu(AT, op1, op2);
      }
      maskeqz(dst, dst, AT);
      masknez(AT, src, AT);
      break;

    case LT:
      if (is_signed) {
        slt(AT, op1, op2);
      } else {
        sltu(AT, op1, op2);
      }
      masknez(dst, dst, AT);
      maskeqz(AT, src, AT);
      break;

    case LE:
      if (is_signed) {
        slt(AT, op2, op1);
      } else {
        sltu(AT, op2, op1);
      }
      maskeqz(dst, dst, AT);
      masknez(AT, src, AT);
      break;

    default:
      Unimplemented();
  }
  OR(dst, dst, AT);
}


void MacroAssembler::cmp_cmov(FloatRegister op1,
                              FloatRegister op2,
                              Register      dst,
                              Register      src,
                              FloatRegister tmp1,
                              FloatRegister tmp2,
                              CMCompare     cmp,
                              bool          is_float) {
  movgr2fr_d(tmp1, dst);
  movgr2fr_d(tmp2, src);

  switch(cmp) {
    case EQ:
      if (is_float) {
        fcmp_ceq_s(FCC0, op1, op2);
      } else {
        fcmp_ceq_d(FCC0, op1, op2);
      }
      fsel(tmp1, tmp1, tmp2, FCC0);
      break;

    case NE:
      if (is_float) {
        fcmp_ceq_s(FCC0, op1, op2);
      } else {
        fcmp_ceq_d(FCC0, op1, op2);
      }
      fsel(tmp1, tmp2, tmp1, FCC0);
      break;

    case GT:
      if (is_float) {
        fcmp_cule_s(FCC0, op1, op2);
      } else {
        fcmp_cule_d(FCC0, op1, op2);
      }
      fsel(tmp1, tmp2, tmp1, FCC0);
      break;

    case GE:
      if (is_float) {
        fcmp_cult_s(FCC0, op1, op2);
      } else {
        fcmp_cult_d(FCC0, op1, op2);
      }
      fsel(tmp1, tmp2, tmp1, FCC0);
      break;

    case LT:
      if (is_float) {
        fcmp_cult_s(FCC0, op1, op2);
      } else {
        fcmp_cult_d(FCC0, op1, op2);
      }
      fsel(tmp1, tmp1, tmp2, FCC0);
      break;

    case LE:
      if (is_float) {
        fcmp_cule_s(FCC0, op1, op2);
      } else {
        fcmp_cule_d(FCC0, op1, op2);
      }
      fsel(tmp1, tmp1, tmp2, FCC0);
      break;

    default:
      Unimplemented();
  }

  movfr2gr_d(dst, tmp1);
}

void MacroAssembler::cmp_cmov(FloatRegister op1,
                              FloatRegister op2,
                              FloatRegister dst,
                              FloatRegister src,
                              CMCompare     cmp,
                              bool          is_float) {
  switch(cmp) {
    case EQ:
      if (!is_float) {
        fcmp_ceq_d(FCC0, op1, op2);
      } else {
        fcmp_ceq_s(FCC0, op1, op2);
      }
      fsel(dst, dst, src, FCC0);
      break;

    case NE:
      if (!is_float) {
        fcmp_ceq_d(FCC0, op1, op2);
      } else {
        fcmp_ceq_s(FCC0, op1, op2);
      }
      fsel(dst, src, dst, FCC0);
      break;

    case GT:
      if (!is_float) {
        fcmp_cule_d(FCC0, op1, op2);
      } else {
        fcmp_cule_s(FCC0, op1, op2);
      }
      fsel(dst, src, dst, FCC0);
      break;

    case GE:
      if (!is_float) {
        fcmp_cult_d(FCC0, op1, op2);
      } else {
        fcmp_cult_s(FCC0, op1, op2);
      }
      fsel(dst, src, dst, FCC0);
      break;

    case LT:
      if (!is_float) {
        fcmp_cult_d(FCC0, op1, op2);
      } else {
        fcmp_cult_s(FCC0, op1, op2);
      }
      fsel(dst, dst, src, FCC0);
      break;

    case LE:
      if (!is_float) {
        fcmp_cule_d(FCC0, op1, op2);
      } else {
        fcmp_cule_s(FCC0, op1, op2);
      }
      fsel(dst, dst, src, FCC0);
      break;

    default:
      Unimplemented();
  }
}

void MacroAssembler::cmp_cmov(Register      op1,
                              Register      op2,
                              FloatRegister dst,
                              FloatRegister src,
                              CMCompare     cmp,
                              bool          is_signed) {
  switch (cmp) {
    case EQ:
    case NE:
      sub_d(AT, op1, op2);
      sltu(AT, R0, AT);
      break;

    case GT:
    case LE:
      if (is_signed) {
        slt(AT, op2, op1);
      } else {
        sltu(AT, op2, op1);
      }
      break;

    case GE:
    case LT:
      if (is_signed) {
        slt(AT, op1, op2);
      } else {
        sltu(AT, op1, op2);
      }
      break;

    default:
      Unimplemented();
  }

  if (UseGR2CF) {
    movgr2cf(FCC0, AT);
  } else {
    movgr2fr_w(fscratch, AT);
    movfr2cf(FCC0, fscratch);
  }

  switch (cmp) {
    case EQ:
    case GE:
    case LE:
      fsel(dst, src, dst, FCC0);
      break;

    case NE:
    case GT:
    case LT:
      fsel(dst, dst, src, FCC0);
      break;

    default:
      Unimplemented();
  }
}

void MacroAssembler::membar(Membar_mask_bits hint){
  address prev = pc() - NativeInstruction::sync_instruction_size;
  address last = code()->last_insn();
  if (last != nullptr && ((NativeInstruction*)last)->is_sync() && prev == last) {
    NativeMembar *membar = (NativeMembar*)prev;
#ifndef PRODUCT
    char buf[50];
    snprintf(buf, sizeof(buf), "merged membar 0x%x 0x%x => 0x%x",
      (Ordering | membar->get_hint()), (Ordering | (~hint & 0xF)), (Ordering | (membar->get_hint() & (~hint & 0xF))));
    block_comment(buf);
#endif
    // merged membar
    // e.g. LoadLoad and LoadLoad|LoadStore to LoadLoad|LoadStore
    membar->set_hint(membar->get_hint() & (~hint & 0xF));
  } else {
    code()->set_last_insn(pc());
    Assembler::membar(hint);
  }
}

/**
 * Emits code to update CRC-32 with a byte value according to constants in table
 *
 * @param [in,out]crc   Register containing the crc.
 * @param [in]val       Register containing the byte to fold into the CRC.
 * @param [in]table     Register containing the table of crc constants.
 *
 * uint32_t crc;
 * val = crc_table[(val ^ crc) & 0xFF];
 * crc = val ^ (crc >> 8);
**/
void MacroAssembler::update_byte_crc32(Register crc, Register val, Register table) {
  xorr(val, val, crc);
  andi(val, val, 0xff);
  ld_w(val, Address(table, val, Address::times_4, 0));
  srli_w(crc, crc, 8);
  xorr(crc, val, crc);
}

/**
 * @param crc   register containing existing CRC (32-bit)
 * @param buf   register pointing to input byte buffer (byte*)
 * @param len   register containing number of bytes
 * @param tmp   scratch register
**/
void MacroAssembler::kernel_crc32(Register crc, Register buf, Register len, Register tmp) {
  Label CRC_by64_loop, CRC_by4_loop, CRC_by1_loop, CRC_less64, CRC_by64_pre, CRC_by32_loop, CRC_less32, L_exit;
  assert_different_registers(crc, buf, len, tmp);

    nor(crc, crc, R0);

    addi_d(len, len, -64);
    bge(len, R0, CRC_by64_loop);
    addi_d(len, len, 64-4);
    bge(len, R0, CRC_by4_loop);
    addi_d(len, len, 4);
    blt(R0, len, CRC_by1_loop);
    b(L_exit);

  bind(CRC_by64_loop);
    ld_d(tmp, buf, 0);
    crc_w_d_w(crc, tmp, crc);
    ld_d(tmp, buf, 8);
    crc_w_d_w(crc, tmp, crc);
    ld_d(tmp, buf, 16);
    crc_w_d_w(crc, tmp, crc);
    ld_d(tmp, buf, 24);
    crc_w_d_w(crc, tmp, crc);
    ld_d(tmp, buf, 32);
    crc_w_d_w(crc, tmp, crc);
    ld_d(tmp, buf, 40);
    crc_w_d_w(crc, tmp, crc);
    ld_d(tmp, buf, 48);
    crc_w_d_w(crc, tmp, crc);
    ld_d(tmp, buf, 56);
    crc_w_d_w(crc, tmp, crc);
    addi_d(buf, buf, 64);
    addi_d(len, len, -64);
    bge(len, R0, CRC_by64_loop);
    addi_d(len, len, 64-4);
    bge(len, R0, CRC_by4_loop);
    addi_d(len, len, 4);
    blt(R0, len, CRC_by1_loop);
    b(L_exit);

  bind(CRC_by4_loop);
    ld_w(tmp, buf, 0);
    crc_w_w_w(crc, tmp, crc);
    addi_d(buf, buf, 4);
    addi_d(len, len, -4);
    bge(len, R0, CRC_by4_loop);
    addi_d(len, len, 4);
    bge(R0, len, L_exit);

  bind(CRC_by1_loop);
    ld_b(tmp, buf, 0);
    crc_w_b_w(crc, tmp, crc);
    addi_d(buf, buf, 1);
    addi_d(len, len, -1);
    blt(R0, len, CRC_by1_loop);

  bind(L_exit);
    nor(crc, crc, R0);
}

/**
 * @param crc   register containing existing CRC (32-bit)
 * @param buf   register pointing to input byte buffer (byte*)
 * @param len   register containing number of bytes
 * @param tmp   scratch register
**/
void MacroAssembler::kernel_crc32c(Register crc, Register buf, Register len, Register tmp) {
  Label CRC_by64_loop, CRC_by4_loop, CRC_by1_loop, CRC_less64, CRC_by64_pre, CRC_by32_loop, CRC_less32, L_exit;
  assert_different_registers(crc, buf, len, tmp);

    addi_d(len, len, -64);
    bge(len, R0, CRC_by64_loop);
    addi_d(len, len, 64-4);
    bge(len, R0, CRC_by4_loop);
    addi_d(len, len, 4);
    blt(R0, len, CRC_by1_loop);
    b(L_exit);

  bind(CRC_by64_loop);
    ld_d(tmp, buf, 0);
    crcc_w_d_w(crc, tmp, crc);
    ld_d(tmp, buf, 8);
    crcc_w_d_w(crc, tmp, crc);
    ld_d(tmp, buf, 16);
    crcc_w_d_w(crc, tmp, crc);
    ld_d(tmp, buf, 24);
    crcc_w_d_w(crc, tmp, crc);
    ld_d(tmp, buf, 32);
    crcc_w_d_w(crc, tmp, crc);
    ld_d(tmp, buf, 40);
    crcc_w_d_w(crc, tmp, crc);
    ld_d(tmp, buf, 48);
    crcc_w_d_w(crc, tmp, crc);
    ld_d(tmp, buf, 56);
    crcc_w_d_w(crc, tmp, crc);
    addi_d(buf, buf, 64);
    addi_d(len, len, -64);
    bge(len, R0, CRC_by64_loop);
    addi_d(len, len, 64-4);
    bge(len, R0, CRC_by4_loop);
    addi_d(len, len, 4);
    blt(R0, len, CRC_by1_loop);
    b(L_exit);

  bind(CRC_by4_loop);
    ld_w(tmp, buf, 0);
    crcc_w_w_w(crc, tmp, crc);
    addi_d(buf, buf, 4);
    addi_d(len, len, -4);
    bge(len, R0, CRC_by4_loop);
    addi_d(len, len, 4);
    bge(R0, len, L_exit);

  bind(CRC_by1_loop);
    ld_b(tmp, buf, 0);
    crcc_w_b_w(crc, tmp, crc);
    addi_d(buf, buf, 1);
    addi_d(len, len, -1);
    blt(R0, len, CRC_by1_loop);

  bind(L_exit);
}

// Search for Non-ASCII character (Negative byte value) in a byte array,
// return the index of the first such character, otherwise the length
// of the array segment searched.
//   ..\jdk\src\java.base\share\classes\java\lang\StringCoding.java
//   @IntrinsicCandidate
//   public static int countPositives(byte[] ba, int off, int len) {
//     for (int i = off; i < off + len; i++) {
//       if (ba[i] < 0) {
//         return i - off;
//       }
//     }
//     return len;
//   }
void MacroAssembler::count_positives(Register src, Register len, Register result,
                                     Register tmp1, Register tmp2) {
  Label Loop, Negative, Once, Done;

  move(result, R0);
  beqz(len, Done);

  addi_w(tmp2, len, -8);
  blt(tmp2, R0, Once);

  li(tmp1, 0x8080808080808080);

  bind(Loop);
    ldx_d(AT, src, result);
    andr(AT, AT, tmp1);
    bnez(AT, Negative);
    addi_w(result, result, 8);
    bge(tmp2, result, Loop);

  beq(result, len, Done);
  ldx_d(AT, src, tmp2);
  andr(AT, AT, tmp1);
  move(result, tmp2);

  bind(Negative);
    ctz_d(AT, AT);
    srai_w(AT, AT, 3);
    add_w(result, result, AT);
    b(Done);

  bind(Once);
    ldx_b(tmp1, src, result);
    blt(tmp1, R0, Done);
    addi_w(result, result, 1);
    blt(result, len, Once);

  bind(Done);
}

// Intrinsic for java.lang.StringUTF16.compress(char[] src, int srcOff, byte[] dst, int dstOff, int len)
// Return the array length if every element in array can be encoded,
// otherwise, the index of first non-latin1 (> 0xff) character.
// jtreg: TestStringIntrinsicRangeChecks.java
void MacroAssembler::char_array_compress(Register src, Register dst,
                                         Register len, Register result,
                                         Register tmp1, Register tmp2, Register tmp3,
                                         FloatRegister vtemp1, FloatRegister vtemp2,
                                         FloatRegister vtemp3, FloatRegister vtemp4) {
  encode_iso_array(src, dst, len, result, tmp1, tmp2, tmp3, false, vtemp1, vtemp2, vtemp3, vtemp4);
}

// Inflate byte[] to char[]. len must be positive int.
// jtreg:test/jdk/sun/nio/cs/FindDecoderBugs.java
void MacroAssembler::byte_array_inflate(Register src, Register dst, Register len,
                                        Register tmp1, Register tmp2,
                                        FloatRegister vtemp1, FloatRegister vtemp2) {
  Label L_loop, L_small, L_small_loop, L_last, L_done;

  bge(R0, len, L_done);

  addi_w(tmp2, len, -16);
  blt(tmp2, R0, L_small);

  move(tmp1, R0);
  alsl_d(AT, len, dst, 0);  // AT = dst + len * 2
  vxor_v(fscratch, fscratch, fscratch);

  // load and inflate 16 chars per loop
  bind(L_loop);
    vldx(vtemp1, src, tmp1);
    addi_w(tmp1, tmp1, 16);

    // 0x0000000000000000a1b2c3d4e5f6g7h8 -> 0x00a100b200c300d4.....
    vilvl_b(vtemp2, fscratch, vtemp1);
    vst(vtemp2, dst, 0);

    // 0xa1b2c3d4e5f6g7h80000000000000000 -> 0x00a100b200c300d4.....
    vilvh_b(vtemp1, fscratch, vtemp1);
    vst(vtemp1, dst, 16);

    addi_d(dst, dst, 32);
    bge(tmp2, tmp1, L_loop);

  // inflate the last 16 chars
  beq(len, tmp1, L_done);
  addi_d(AT, AT, -32);
  vldx(vtemp1, src, tmp2);
  vilvl_b(vtemp2, fscratch, vtemp1);
  vst(vtemp2, AT, 0);
  vilvh_b(vtemp1, fscratch, vtemp1);
  vst(vtemp1, AT, 16);
  b(L_done);

  bind(L_small);
    li(AT, 4);
    blt(len, AT, L_last);

  bind(L_small_loop);
    ld_wu(tmp1, src, 0);
    addi_d(src, src, 4);
    addi_w(len, len, -4);

    // 0x00000000a1b2c3d4 -> 0x00a100b200c300d4
    bstrpick_d(tmp2, tmp1, 7, 0);
    srli_d(tmp1, tmp1, 8);
    bstrins_d(tmp2, tmp1, 23, 16);
    srli_d(tmp1, tmp1, 8);
    bstrins_d(tmp2, tmp1, 39, 32);
    srli_d(tmp1, tmp1, 8);
    bstrins_d(tmp2, tmp1, 55, 48);

    st_d(tmp2, dst, 0);
    addi_d(dst, dst, 8);
    bge(len, AT, L_small_loop);

  bind(L_last);
    beqz(len, L_done);
    ld_bu(AT, src, 0);
    st_h(AT, dst, 0);
    addi_w(len, len, -1);

    beqz(len, L_done);
    ld_bu(AT, src, 1);
    st_h(AT, dst, 2);
    addi_w(len, len, -1);

    beqz(len, L_done);
    ld_bu(AT, src, 2);
    st_h(AT, dst, 4);

  bind(L_done);
}

// Intrinsic for
//
// - java.lang.StringCoding::implEncodeISOArray
// - java.lang.StringCoding::implEncodeAsciiArray
//
// This version always returns the number of characters copied.
void MacroAssembler::encode_iso_array(Register src, Register dst,
                                      Register len, Register result,
                                      Register tmp1, Register tmp2,
                                      Register tmp3, bool ascii,
                                      FloatRegister vtemp1, FloatRegister vtemp2,
                                      FloatRegister vtemp3, FloatRegister vtemp4) {
  const FloatRegister shuf_index = vtemp3;
  const FloatRegister latin_mask = vtemp4;

  Label Deal8, Loop8, Loop32, Done, Once;

  move(result, R0);  // init in case of bad value
  bge(R0, len, Done);

  li(tmp3, ascii ? 0xff80ff80ff80ff80 : 0xff00ff00ff00ff00);
  srai_w(AT, len, 4);
  beqz(AT, Deal8);

  li(tmp1, StubRoutines::la::string_compress_index());
  vld(shuf_index, tmp1, 0);
  vreplgr2vr_d(latin_mask, tmp3);

  bind(Loop32);
    beqz(AT, Deal8);

    vld(vtemp1, src, 0);
    vld(vtemp2, src, 16);
    addi_w(AT, AT, -1);

    vor_v(fscratch, vtemp1, vtemp2);
    vand_v(fscratch, fscratch, latin_mask);
    vseteqz_v(FCC0, fscratch);  // not latin-1, apply slow path
    bceqz(FCC0, Once);

    vshuf_b(fscratch, vtemp2, vtemp1, shuf_index);

    vstx(fscratch, dst, result);
    addi_d(src, src, 32);
    addi_w(result, result, 16);
    b(Loop32);

  bind(Deal8);
    bstrpick_w(AT, len, 3, 2);

  bind(Loop8);
    beqz(AT, Once);
    ld_d(tmp1, src, 0);
    andr(tmp2, tmp3, tmp1);  // not latin-1, apply slow path
    bnez(tmp2, Once);

    // 0x00a100b200c300d4 -> 0x00000000a1b2c3d4
    srli_d(tmp2, tmp1, 8);
    orr(tmp2, tmp2, tmp1);           // 0x00a1a1b2b2c3c3d4
    bstrpick_d(tmp1, tmp2, 47, 32);  // 0x0000a1b2
    slli_d(tmp1, tmp1, 16);          // 0xa1b20000
    bstrins_d(tmp1, tmp2, 15, 0);    // 0xa1b2c3d4

    stx_w(tmp1, dst, result);
    addi_w(AT, AT, -1);
    addi_d(src, src, 8);
    addi_w(result, result, 4);
    b(Loop8);

  bind(Once);
    beq(len, result, Done);
    ld_hu(tmp1, src, 0);
    andr(tmp2, tmp3, tmp1);  // not latin-1, stop here
    bnez(tmp2, Done);
    stx_b(tmp1, dst, result);
    addi_d(src, src, 2);
    addi_w(result, result, 1);
    b(Once);

  bind(Done);
}

// Math.round employs the ties-to-positive round mode,
// which is not a typically conversion method defined
// in the IEEE-754-2008. For single-precision floatings,
// the following algorithm can be used to effectively
// implement rounding via standard operations.
void MacroAssembler::java_round_float(Register dst,
                                      FloatRegister src,
                                      FloatRegister vtemp1) {
  block_comment("java_round_float: { ");

  Label L_abnormal, L_done;

  li(AT, StubRoutines::la::round_float_imm());

  // if src is -0.5f, return 0 as result
  fld_s(vtemp1, AT, 0);
  fcmp_ceq_s(FCC0, vtemp1, src);
  bceqz(FCC0, L_abnormal);
  move(dst, R0);
  b(L_done);

  // else, floor src with the magic number
  bind(L_abnormal);
  fld_s(vtemp1, AT, 4);
  fadd_s(fscratch, vtemp1, src);
  ftintrm_w_s(fscratch, fscratch);
  movfr2gr_s(dst, fscratch);

  bind(L_done);

  block_comment("} java_round_float");
}

void MacroAssembler::java_round_float_lsx(FloatRegister dst,
                                          FloatRegister src,
                                          FloatRegister vtemp1,
                                          FloatRegister vtemp2) {
  block_comment("java_round_float_lsx: { ");
  li(AT, StubRoutines::la::round_float_imm());
  vldrepl_w(vtemp1, AT, 0);  // repl -0.5f
  vldrepl_w(vtemp2, AT, 1);  // repl 0.49999997f

  vfcmp_cne_s(fscratch, src, vtemp1);  // generate the mask
  vand_v(fscratch, fscratch, src);     // clear the special
  vfadd_s(dst, fscratch, vtemp2);      // plus the magic
  vftintrm_w_s(dst, dst);              // floor the result
  block_comment("} java_round_float_lsx");
}

void MacroAssembler::java_round_float_lasx(FloatRegister dst,
                                           FloatRegister src,
                                           FloatRegister vtemp1,
                                           FloatRegister vtemp2) {
  block_comment("java_round_float_lasx: { ");
  li(AT, StubRoutines::la::round_float_imm());
  xvldrepl_w(vtemp1, AT, 0);  // repl -0.5f
  xvldrepl_w(vtemp2, AT, 1);  // repl 0.49999997f

  xvfcmp_cne_s(fscratch, src, vtemp1);  // generate the mask
  xvand_v(fscratch, fscratch, src);     // clear the special
  xvfadd_s(dst, fscratch, vtemp2);      // plus the magic
  xvftintrm_w_s(dst, dst);              // floor the result
  block_comment("} java_round_float_lasx");
}

// Math.round employs the ties-to-positive round mode,
// which is not a typically conversion method defined
// in the IEEE-754-2008. For double-precision floatings,
// the following algorithm can be used to effectively
// implement rounding via standard operations.
void MacroAssembler::java_round_double(Register dst,
                                       FloatRegister src,
                                       FloatRegister vtemp1) {
  block_comment("java_round_double: { ");

  Label L_abnormal, L_done;

  li(AT, StubRoutines::la::round_double_imm());

  // if src is -0.5d, return 0 as result
  fld_d(vtemp1, AT, 0);
  fcmp_ceq_d(FCC0, vtemp1, src);
  bceqz(FCC0, L_abnormal);
  move(dst, R0);
  b(L_done);

  // else, floor src with the magic number
  bind(L_abnormal);
  fld_d(vtemp1, AT, 8);
  fadd_d(fscratch, vtemp1, src);
  ftintrm_l_d(fscratch, fscratch);
  movfr2gr_d(dst, fscratch);

  bind(L_done);

  block_comment("} java_round_double");
}

void MacroAssembler::java_round_double_lsx(FloatRegister dst,
                                           FloatRegister src,
                                           FloatRegister vtemp1,
                                           FloatRegister vtemp2) {
  block_comment("java_round_double_lsx: { ");
  li(AT, StubRoutines::la::round_double_imm());
  vldrepl_d(vtemp1, AT, 0);  // repl -0.5d
  vldrepl_d(vtemp2, AT, 1);  // repl 0.49999999999999994d

  vfcmp_cne_d(fscratch, src, vtemp1);  // generate the mask
  vand_v(fscratch, fscratch, src);     // clear the special
  vfadd_d(dst, fscratch, vtemp2);      // plus the magic
  vftintrm_l_d(dst, dst);              // floor the result
  block_comment("} java_round_double_lsx");
}

void MacroAssembler::java_round_double_lasx(FloatRegister dst,
                                            FloatRegister src,
                                            FloatRegister vtemp1,
                                            FloatRegister vtemp2) {
  block_comment("java_round_double_lasx: { ");
  li(AT, StubRoutines::la::round_double_imm());
  xvldrepl_d(vtemp1, AT, 0);  // repl -0.5d
  xvldrepl_d(vtemp2, AT, 1);  // repl 0.49999999999999994d

  xvfcmp_cne_d(fscratch, src, vtemp1);  // generate the mask
  xvand_v(fscratch, fscratch, src);     // clear the special
  xvfadd_d(dst, fscratch, vtemp2);      // plus the magic
  xvftintrm_l_d(dst, dst);              // floor the result
  block_comment("} java_round_double_lasx");
}

// Code for BigInteger::mulAdd intrinsic
// out     = c_rarg0
// in      = c_rarg1
// offset  = c_rarg2  (already out.length-offset)
// len     = c_rarg3
// k       = c_rarg4
//
// pseudo code from java implementation:
// long kLong = k & LONG_MASK;
// carry = 0;
// offset = out.length-offset - 1;
// for (int j = len - 1; j >= 0; j--) {
//     product = (in[j] & LONG_MASK) * kLong + (out[offset] & LONG_MASK) + carry;
//     out[offset--] = (int)product;
//     carry = product >>> 32;
// }
// return (int)carry;
void MacroAssembler::mul_add(Register out, Register in, Register offset,
                             Register len, Register k) {
  Label L_tail_loop, L_unroll, L_end;

  move(SCR2, out);
  move(out, R0); // should clear out
  bge(R0, len, L_end);

  alsl_d(offset, offset, SCR2, LogBytesPerInt - 1);
  alsl_d(in, len, in, LogBytesPerInt - 1);

  const int unroll = 16;
  li(SCR2, unroll);
  blt(len, SCR2, L_tail_loop);

  bind(L_unroll);

    addi_d(in, in, -unroll * BytesPerInt);
    addi_d(offset, offset, -unroll * BytesPerInt);

    for (int i = unroll - 1; i >= 0; i--) {
      ld_wu(SCR1, in, i * BytesPerInt);
      mulw_d_wu(SCR1, SCR1, k);
      add_d(out, out, SCR1); // out as scratch
      ld_wu(SCR1, offset, i * BytesPerInt);
      add_d(SCR1, SCR1, out);
      st_w(SCR1, offset, i * BytesPerInt);
      srli_d(out, SCR1, 32); // keep carry
    }

    sub_w(len, len, SCR2);
    bge(len, SCR2, L_unroll);

  bge(R0, len, L_end); // check tail

  bind(L_tail_loop);

    addi_d(in, in, -BytesPerInt);
    ld_wu(SCR1, in, 0);
    mulw_d_wu(SCR1, SCR1, k);
    add_d(out, out, SCR1); // out as scratch

    addi_d(offset, offset, -BytesPerInt);
    ld_wu(SCR1, offset, 0);
    add_d(SCR1, SCR1, out);
    st_w(SCR1, offset, 0);

    srli_d(out, SCR1, 32); // keep carry

    addi_w(len, len, -1);
    blt(R0, len, L_tail_loop);

  bind(L_end);
}

#ifndef PRODUCT
void MacroAssembler::verify_cross_modify_fence_not_required() {
  if (VerifyCrossModifyFence) {
    // Check if thread needs a cross modify fence.
    ld_bu(SCR1, Address(TREG, in_bytes(JavaThread::requires_cross_modify_fence_offset())));
    Label fence_not_required;
    beqz(SCR1, fence_not_required);
    // If it does then fail.
    move(A0, TREG);
    call(CAST_FROM_FN_PTR(address, JavaThread::verify_cross_modify_fence_failure));
    bind(fence_not_required);
  }
}
#endif

// The java_calling_convention describes stack locations as ideal slots on
// a frame with no abi restrictions. Since we must observe abi restrictions
// (like the placement of the register window) the slots must be biased by
// the following value.
static int reg2offset_in(VMReg r) {
  // Account for saved rfp and lr
  // This should really be in_preserve_stack_slots
  return r->reg2stack() * VMRegImpl::stack_slot_size;
}

static int reg2offset_out(VMReg r) {
  return (r->reg2stack() + SharedRuntime::out_preserve_stack_slots()) * VMRegImpl::stack_slot_size;
}

// A simple move of integer like type
void MacroAssembler::simple_move32(VMRegPair src, VMRegPair dst, Register tmp) {
  if (src.first()->is_stack()) {
    if (dst.first()->is_stack()) {
      // stack to stack
      ld_w(tmp, FP, reg2offset_in(src.first()));
      st_d(tmp, SP, reg2offset_out(dst.first()));
    } else {
      // stack to reg
      ld_w(dst.first()->as_Register(), FP, reg2offset_in(src.first()));
    }
  } else if (dst.first()->is_stack()) {
    // reg to stack
    st_d(src.first()->as_Register(), SP, reg2offset_out(dst.first()));
  } else {
    if (dst.first() != src.first()) {
      // 32bits extend sign
      add_w(dst.first()->as_Register(), src.first()->as_Register(), R0);
    }
  }
}

// An oop arg. Must pass a handle not the oop itself
void MacroAssembler::object_move(
                        OopMap* map,
                        int oop_handle_offset,
                        int framesize_in_slots,
                        VMRegPair src,
                        VMRegPair dst,
                        bool is_receiver,
                        int* receiver_offset) {

  // must pass a handle. First figure out the location we use as a handle
  Register rHandle = dst.first()->is_stack() ? T5 : dst.first()->as_Register();

  if (src.first()->is_stack()) {
    // Oop is already on the stack as an argument
    Label nil;
    move(rHandle, R0);
    ld_d(AT, FP, reg2offset_in(src.first()));
    beqz(AT, nil);
    lea(rHandle, Address(FP, reg2offset_in(src.first())));
    bind(nil);

    int offset_in_older_frame = src.first()->reg2stack()
      + SharedRuntime::out_preserve_stack_slots();
    map->set_oop(VMRegImpl::stack2reg(offset_in_older_frame + framesize_in_slots));
    if (is_receiver) {
      *receiver_offset = (offset_in_older_frame + framesize_in_slots) * VMRegImpl::stack_slot_size;
    }
  } else {
    // Oop is in an a register we must store it to the space we reserve
    // on the stack for oop_handles and pass a handle if oop is non-null
    const Register rOop = src.first()->as_Register();
    assert((rOop->encoding() >= A0->encoding()) && (rOop->encoding() <= T0->encoding()),"wrong register");
    //Important: refer to java_calling_convention
    int oop_slot = (rOop->encoding() - j_rarg1->encoding()) * VMRegImpl::slots_per_word + oop_handle_offset;
    int offset = oop_slot*VMRegImpl::stack_slot_size;

    Label skip;
    st_d(rOop, SP, offset);
    map->set_oop(VMRegImpl::stack2reg(oop_slot));
    move(rHandle, R0);
    beqz(rOop, skip);
    lea(rHandle, Address(SP, offset));
    bind(skip);

    if (is_receiver) {
      *receiver_offset = offset;
    }
  }

  // If arg is on the stack then place it otherwise it is already in correct reg.
  if (dst.first()->is_stack()) {
    st_d(rHandle, Address(SP, reg2offset_out(dst.first())));
  }
}

// Referring to c_calling_convention, float and/or double argument shuffling may
// adopt int register for spilling. So we need to capture and deal with these
// kinds of situations in the float_move and double_move methods.

// A float move
void MacroAssembler::float_move(VMRegPair src, VMRegPair dst, Register tmp) {
  assert(!src.second()->is_valid() && !dst.second()->is_valid(), "bad float_move");
  if (src.first()->is_stack()) {
    // stack to stack/reg
    if (dst.first()->is_stack()) {
      ld_w(tmp, FP, reg2offset_in(src.first()));
      st_w(tmp, SP, reg2offset_out(dst.first()));
    } else if (dst.first()->is_FloatRegister()) {
      fld_s(dst.first()->as_FloatRegister(), FP, reg2offset_in(src.first()));
    } else {
      ld_w(dst.first()->as_Register(), FP, reg2offset_in(src.first()));
    }
  } else {
    // reg to stack/reg
    if (dst.first()->is_stack()) {
      fst_s(src.first()->as_FloatRegister(), SP, reg2offset_out(dst.first()));
    } else if (dst.first()->is_FloatRegister()) {
      fmov_s(dst.first()->as_FloatRegister(), src.first()->as_FloatRegister());
    } else {
      movfr2gr_s(dst.first()->as_Register(), src.first()->as_FloatRegister());
    }
  }
}

// A long move
void MacroAssembler::long_move(VMRegPair src, VMRegPair dst, Register tmp) {
  if (src.first()->is_stack()) {
    if (dst.first()->is_stack()) {
      ld_d(tmp, FP, reg2offset_in(src.first()));
      st_d(tmp, SP, reg2offset_out(dst.first()));
    } else {
      ld_d(dst.first()->as_Register(), FP, reg2offset_in(src.first()));
    }
  } else {
    if (dst.first()->is_stack()) {
      st_d(src.first()->as_Register(), SP, reg2offset_out(dst.first()));
    } else {
      move(dst.first()->as_Register(), src.first()->as_Register());
    }
  }
}

// A double move
void MacroAssembler::double_move(VMRegPair src, VMRegPair dst, Register tmp) {
  if (src.first()->is_stack()) {
    // source is all stack
    if (dst.first()->is_stack()) {
      ld_d(tmp, FP, reg2offset_in(src.first()));
      st_d(tmp, SP, reg2offset_out(dst.first()));
    } else if (dst.first()->is_FloatRegister()) {
      fld_d(dst.first()->as_FloatRegister(), FP, reg2offset_in(src.first()));
    } else {
      ld_d(dst.first()->as_Register(), FP, reg2offset_in(src.first()));
    }
  } else {
    // reg to stack/reg
    if (dst.first()->is_stack()) {
      fst_d(src.first()->as_FloatRegister(), SP, reg2offset_out(dst.first()));
    } else if (dst.first()->is_FloatRegister()) {
      fmov_d(dst.first()->as_FloatRegister(), src.first()->as_FloatRegister());
    } else {
      movfr2gr_d(dst.first()->as_Register(), src.first()->as_FloatRegister());
    }
  }
}

// Implements lightweight-locking.
// Branches to slow upon failure to lock the object.
// Falls through upon success.
//
//  - obj: the object to be locked
//  - hdr: the header, already loaded from obj, will be destroyed
//  - flag: as cr for c2, but only as temporary regisgter for c1/interpreter
//  - tmp: temporary registers, will be destroyed
void MacroAssembler::lightweight_lock(Register obj, Register hdr, Register flag, Register tmp, Label& slow) {
  assert(LockingMode == LM_LIGHTWEIGHT, "only used with new lightweight locking");
  assert_different_registers(obj, hdr, flag, tmp);

  // Check if we would have space on lock-stack for the object.
  ld_wu(flag, Address(TREG, JavaThread::lock_stack_top_offset()));
  li(tmp, (unsigned)LockStack::end_offset());
  sltu(flag, flag, tmp);
  beqz(flag, slow);

  // Load (object->mark() | 1) into hdr
  ori(hdr, hdr, markWord::unlocked_value);
  // Clear lock-bits, into tmp
  xori(tmp, hdr, markWord::unlocked_value);
  // Try to swing header from unlocked to locked
  cmpxchg(/*addr*/ Address(obj, 0), /*old*/ hdr, /*new*/ tmp, /*flag*/ flag, /*retold*/ true, /*barrier*/true);
  beqz(flag, slow);

  // After successful lock, push object on lock-stack
  ld_wu(tmp, Address(TREG, JavaThread::lock_stack_top_offset()));
  stx_d(obj, TREG, tmp);
  addi_w(tmp, tmp, oopSize);
  st_w(tmp, Address(TREG, JavaThread::lock_stack_top_offset()));
}

// Implements lightweight-unlocking.
// Branches to slow upon failure.
// Falls through upon success.
//
// - obj: the object to be unlocked
// - hdr: the (pre-loaded) header of the object
// - flag: as cr for c2, but only as temporary regisgter for c1/interpreter
// - tmp: temporary registers
void MacroAssembler::lightweight_unlock(Register obj, Register hdr, Register flag, Register tmp, Label& slow) {
  assert(LockingMode == LM_LIGHTWEIGHT, "only used with new lightweight locking");
  assert_different_registers(obj, hdr, tmp, flag);

#ifdef ASSERT
  {
    // The following checks rely on the fact that LockStack is only ever modified by
    // its owning thread, even if the lock got inflated concurrently; removal of LockStack
    // entries after inflation will happen delayed in that case.

    // Check for lock-stack underflow.
    Label stack_ok;
    ld_wu(tmp, Address(TREG, JavaThread::lock_stack_top_offset()));
    li(flag, (unsigned)LockStack::start_offset());
    bltu(flag, tmp, stack_ok);
    stop("Lock-stack underflow");
    bind(stack_ok);
  }
  {
    // Check if the top of the lock-stack matches the unlocked object.
    Label tos_ok;
    addi_w(tmp, tmp, -oopSize);
    ldx_d(tmp, TREG, tmp);
    beq(tmp, obj, tos_ok);
    stop("Top of lock-stack does not match the unlocked object");
    bind(tos_ok);
  }
  {
    // Check that hdr is fast-locked.
    Label hdr_ok;
    andi(tmp, hdr, markWord::lock_mask_in_place);
    beqz(tmp, hdr_ok);
    stop("Header is not fast-locked");
    bind(hdr_ok);
  }
#endif

  // Load the new header (unlocked) into tmp
  ori(tmp, hdr, markWord::unlocked_value);

  // Try to swing header from locked to unlocked
  cmpxchg(/*addr*/ Address(obj, 0), /*old*/ hdr, /*new*/ tmp, /*flag*/ flag, /**/true, /*barrier*/ true);
  beqz(flag, slow);

  // After successful unlock, pop object from lock-stack
  ld_wu(tmp, Address(TREG, JavaThread::lock_stack_top_offset()));
  addi_w(tmp, tmp, -oopSize);
#ifdef ASSERT
  stx_d(R0, TREG, tmp);
#endif
  st_w(tmp, Address(TREG, JavaThread::lock_stack_top_offset()));
}

#if INCLUDE_ZGC
void MacroAssembler::patchable_li16(Register rd, uint16_t value) {
  int count = 0;

  if (is_simm(value, 12)) {
    addi_d(rd, R0, value);
    count++;
  } else if (is_uimm(value, 12)) {
    ori(rd, R0, value);
    count++;
  } else {
    lu12i_w(rd, split_low20(value >> 12));
    count++;
    if (split_low12(value)) {
      ori(rd, rd, split_low12(value));
      count++;
    }
  }

  while (count < 2) {
    nop();
    count++;
  }
}

void MacroAssembler::z_color(Register dst, Register src, Register tmp) {
  assert_different_registers(dst, tmp);
  assert_different_registers(src, tmp);
  relocate(barrier_Relocation::spec(), ZBarrierRelocationFormatStoreGoodBits);
  if (src != noreg) {
    patchable_li16(tmp, barrier_Relocation::unpatched);
    slli_d(dst, src, ZPointerLoadShift);
    orr(dst, dst, tmp);
  } else {
    patchable_li16(dst, barrier_Relocation::unpatched);
  }
}

void MacroAssembler::z_uncolor(Register ref) {
  srli_d(ref, ref, ZPointerLoadShift);
}

void MacroAssembler::check_color(Register ref, Register tmp, bool on_non_strong) {
  assert_different_registers(ref, tmp);
  int relocFormat = on_non_strong ? ZBarrierRelocationFormatMarkBadMask
                                  : ZBarrierRelocationFormatLoadBadMask;
  relocate(barrier_Relocation::spec(), relocFormat);
  patchable_li16(tmp, barrier_Relocation::unpatched);
  andr(tmp, ref, tmp);
}
#endif
