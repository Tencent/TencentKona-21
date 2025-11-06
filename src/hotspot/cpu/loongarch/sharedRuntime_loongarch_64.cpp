/*
 * Copyright (c) 2003, 2013, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2015, 2024, Loongson Technology. All rights reserved.
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
#include "asm/macroAssembler.hpp"
#include "asm/macroAssembler.inline.hpp"
#include "code/compiledIC.hpp"
#include "code/debugInfoRec.hpp"
#include "code/icBuffer.hpp"
#include "code/nativeInst.hpp"
#include "code/vtableStubs.hpp"
#include "compiler/oopMap.hpp"
#include "gc/shared/barrierSetAssembler.hpp"
#include "interpreter/interpreter.hpp"
#include "oops/compiledICHolder.hpp"
#include "oops/klass.inline.hpp"
#include "oops/method.inline.hpp"
#include "prims/methodHandles.hpp"
#include "runtime/continuation.hpp"
#include "runtime/continuationEntry.inline.hpp"
#include "runtime/globals.hpp"
#include "runtime/jniHandles.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/signature.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/vframeArray.hpp"
#include "vmreg_loongarch.inline.hpp"
#ifdef COMPILER2
#include "opto/runtime.hpp"
#endif
#if INCLUDE_JVMCI
#include "jvmci/jvmciJavaClasses.hpp"
#endif

#define __ masm->

const int StackAlignmentInSlots = StackAlignmentInBytes / VMRegImpl::stack_slot_size;

class RegisterSaver {
  // Capture info about frame layout
  enum layout {
    fpr0_off = 0,
    fpr1_off,
    fpr2_off,
    fpr3_off,
    fpr4_off,
    fpr5_off,
    fpr6_off,
    fpr7_off,
    fpr8_off,
    fpr9_off,
    fpr10_off,
    fpr11_off,
    fpr12_off,
    fpr13_off,
    fpr14_off,
    fpr15_off,
    fpr16_off,
    fpr17_off,
    fpr18_off,
    fpr19_off,
    fpr20_off,
    fpr21_off,
    fpr22_off,
    fpr23_off,
    fpr24_off,
    fpr25_off,
    fpr26_off,
    fpr27_off,
    fpr28_off,
    fpr29_off,
    fpr30_off,
    fpr31_off,
    a0_off,
    a1_off,
    a2_off,
    a3_off,
    a4_off,
    a5_off,
    a6_off,
    a7_off,
    t0_off,
    t1_off,
    t2_off,
    t3_off,
    t4_off,
    t5_off,
    t6_off,
    t7_off,
    t8_off,
    s0_off,
    s1_off,
    s2_off,
    s3_off,
    s4_off,
    s5_off,
    s6_off,
    s7_off,
    s8_off,
    fp_off,
    ra_off,
    fpr_size = fpr31_off - fpr0_off + 1,
    gpr_size = ra_off - a0_off + 1,
  };

  const bool _save_vectors;
  public:
  RegisterSaver(bool save_vectors) : _save_vectors(save_vectors) {}

  OopMap* save_live_registers(MacroAssembler* masm, int additional_frame_words, int* total_frame_words);
  void restore_live_registers(MacroAssembler* masm);

  int slots_save() {
    int slots = gpr_size * VMRegImpl::slots_per_word;

    if (_save_vectors && UseLASX)
      slots += FloatRegister::slots_per_lasx_register * fpr_size;
    else if (_save_vectors && UseLSX)
      slots += FloatRegister::slots_per_lsx_register * fpr_size;
    else
      slots += FloatRegister::save_slots_per_register * fpr_size;

    return slots;
  }

  int gpr_offset(int off) {
      int slots_per_fpr = FloatRegister::save_slots_per_register;
      int slots_per_gpr = VMRegImpl::slots_per_word;

      if (_save_vectors && UseLASX)
        slots_per_fpr = FloatRegister::slots_per_lasx_register;
      else if (_save_vectors && UseLSX)
        slots_per_fpr = FloatRegister::slots_per_lsx_register;

      return (fpr_size * slots_per_fpr + (off - a0_off) * slots_per_gpr) * VMRegImpl::stack_slot_size;
  }

  int fpr_offset(int off) {
      int slots_per_fpr = FloatRegister::save_slots_per_register;

      if (_save_vectors && UseLASX)
        slots_per_fpr = FloatRegister::slots_per_lasx_register;
      else if (_save_vectors && UseLSX)
        slots_per_fpr = FloatRegister::slots_per_lsx_register;

      return off * slots_per_fpr * VMRegImpl::stack_slot_size;
  }

  int ra_offset() { return gpr_offset(ra_off); }
  int t5_offset() { return gpr_offset(t5_off); }
  int s3_offset() { return gpr_offset(s3_off); }
  int v0_offset() { return gpr_offset(a0_off); }
  int v1_offset() { return gpr_offset(a1_off); }

  int fpr0_offset() { return fpr_offset(fpr0_off); }
  int fpr1_offset() { return fpr_offset(fpr1_off); }

  // During deoptimization only the result register need to be restored
  // all the other values have already been extracted.
  void restore_result_registers(MacroAssembler* masm);
};

OopMap* RegisterSaver::save_live_registers(MacroAssembler* masm, int additional_frame_words, int* total_frame_words) {
  // Always make the frame size 16-byte aligned
  int frame_size_in_bytes = align_up(additional_frame_words * wordSize + slots_save() * VMRegImpl::stack_slot_size, StackAlignmentInBytes);
  // OopMap frame size is in compiler stack slots (jint's) not bytes or words
  int frame_size_in_slots = frame_size_in_bytes / VMRegImpl::stack_slot_size;
  // The caller will allocate additional_frame_words
  int additional_frame_slots = additional_frame_words * wordSize / VMRegImpl::stack_slot_size;
  // CodeBlob frame size is in words.
  int frame_size_in_words = frame_size_in_bytes / wordSize;

  *total_frame_words = frame_size_in_words;

  OopMapSet *oop_maps = new OopMapSet();
  OopMap* map =  new OopMap(frame_size_in_slots, 0);

  // save registers
  __ addi_d(SP, SP, -slots_save() * VMRegImpl::stack_slot_size);

  for (int i = 0; i < fpr_size; i++) {
    FloatRegister fpr = as_FloatRegister(i);
    int off = fpr_offset(i);

    if (_save_vectors && UseLASX)
      __ xvst(fpr, SP, off);
    else if (_save_vectors && UseLSX)
      __ vst(fpr, SP, off);
    else
      __ fst_d(fpr, SP, off);
    map->set_callee_saved(VMRegImpl::stack2reg(off / VMRegImpl::stack_slot_size + additional_frame_slots), fpr->as_VMReg());
  }

  for (int i = a0_off; i <= a7_off; i++) {
    Register gpr = as_Register(A0->encoding() + (i - a0_off));
    int off = gpr_offset(i);

    __ st_d(gpr, SP, gpr_offset(i));
    map->set_callee_saved(VMRegImpl::stack2reg(off / VMRegImpl::stack_slot_size + additional_frame_slots), gpr->as_VMReg());
  }

  for (int i = t0_off; i <= t6_off; i++) {
    Register gpr = as_Register(T0->encoding() + (i - t0_off));
    int off = gpr_offset(i);

    __ st_d(gpr, SP, gpr_offset(i));
    map->set_callee_saved(VMRegImpl::stack2reg(off / VMRegImpl::stack_slot_size + additional_frame_slots), gpr->as_VMReg());
  }
  __ st_d(T8, SP, gpr_offset(t8_off));
  map->set_callee_saved(VMRegImpl::stack2reg(gpr_offset(t8_off) / VMRegImpl::stack_slot_size + additional_frame_slots), T8->as_VMReg());

  for (int i = s0_off; i <= s8_off; i++) {
    Register gpr = as_Register(S0->encoding() + (i - s0_off));
    int off = gpr_offset(i);

    __ st_d(gpr, SP, gpr_offset(i));
    map->set_callee_saved(VMRegImpl::stack2reg(off / VMRegImpl::stack_slot_size + additional_frame_slots), gpr->as_VMReg());
  }

  __ st_d(FP, SP, gpr_offset(fp_off));
  map->set_callee_saved(VMRegImpl::stack2reg(gpr_offset(fp_off) / VMRegImpl::stack_slot_size + additional_frame_slots), FP->as_VMReg());
  __ st_d(RA, SP, gpr_offset(ra_off));
  map->set_callee_saved(VMRegImpl::stack2reg(gpr_offset(ra_off) / VMRegImpl::stack_slot_size + additional_frame_slots), RA->as_VMReg());

  __ addi_d(FP, SP, slots_save() * VMRegImpl::stack_slot_size);

  return map;
}


// Pop the current frame and restore all the registers that we
// saved.
void RegisterSaver::restore_live_registers(MacroAssembler* masm) {
  for (int i = 0; i < fpr_size; i++) {
    FloatRegister fpr = as_FloatRegister(i);
    int off = fpr_offset(i);

    if (_save_vectors && UseLASX)
      __ xvld(fpr, SP, off);
    else if (_save_vectors && UseLSX)
      __ vld(fpr, SP, off);
    else
      __ fld_d(fpr, SP, off);
  }

  for (int i = a0_off; i <= a7_off; i++) {
    Register gpr = as_Register(A0->encoding() + (i - a0_off));
    int off = gpr_offset(i);

    __ ld_d(gpr, SP, gpr_offset(i));
  }

  for (int i = t0_off; i <= t6_off; i++) {
    Register gpr = as_Register(T0->encoding() + (i - t0_off));
    int off = gpr_offset(i);

    __ ld_d(gpr, SP, gpr_offset(i));
  }
  __ ld_d(T8, SP, gpr_offset(t8_off));

  for (int i = s0_off; i <= s8_off; i++) {
    Register gpr = as_Register(S0->encoding() + (i - s0_off));
    int off = gpr_offset(i);

    __ ld_d(gpr, SP, gpr_offset(i));
  }

  __ ld_d(FP, SP, gpr_offset(fp_off));
  __ ld_d(RA, SP, gpr_offset(ra_off));

  __ addi_d(SP, SP, slots_save() * VMRegImpl::stack_slot_size);
}

// Pop the current frame and restore the registers that might be holding
// a result.
void RegisterSaver::restore_result_registers(MacroAssembler* masm) {
  // Just restore result register. Only used by deoptimization. By
  // now any callee save register that needs to be restore to a c2
  // caller of the deoptee has been extracted into the vframeArray
  // and will be stuffed into the c2i adapter we create for later
  // restoration so only result registers need to be restored here.

  __ ld_d(V0, SP, gpr_offset(a0_off));
  __ ld_d(V1, SP, gpr_offset(a1_off));

  __ fld_d(F0, SP, fpr_offset(fpr0_off));
  __ fld_d(F1, SP, fpr_offset(fpr1_off));

  __ addi_d(SP, SP, gpr_offset(ra_off));
}

// Is vector's size (in bytes) bigger than a size saved by default?
// 8 bytes registers are saved by default using fld/fst instructions.
bool SharedRuntime::is_wide_vector(int size) {
  return size > 8;
}

// ---------------------------------------------------------------------------
// Read the array of BasicTypes from a signature, and compute where the
// arguments should go.  Values in the VMRegPair regs array refer to 4-byte
// quantities.  Values less than SharedInfo::stack0 are registers, those above
// refer to 4-byte stack slots.  All stack slots are based off of the stack pointer
// as framesizes are fixed.
// VMRegImpl::stack0 refers to the first slot 0(sp).
// and VMRegImpl::stack0+1 refers to the memory word 4-byes higher.  Register
// up to Register::number_of_registers) are the 32-bit
// integer registers.

// Note: the INPUTS in sig_bt are in units of Java argument words, which are
// either 32-bit or 64-bit depending on the build.  The OUTPUTS are in 32-bit
// units regardless of build.

int SharedRuntime::java_calling_convention(const BasicType *sig_bt,
                                           VMRegPair *regs,
                                           int total_args_passed) {

  // Create the mapping between argument positions and registers.
  static const Register INT_ArgReg[Argument::n_int_register_parameters_j] = {
    j_rarg0, j_rarg1, j_rarg2, j_rarg3,
    j_rarg4, j_rarg5, j_rarg6, j_rarg7, j_rarg8
  };
  static const FloatRegister FP_ArgReg[Argument::n_float_register_parameters_j] = {
    j_farg0, j_farg1, j_farg2, j_farg3,
    j_farg4, j_farg5, j_farg6, j_farg7
  };

  uint int_args = 0;
  uint fp_args = 0;
  uint stk_args = 0;

  for (int i = 0; i < total_args_passed; i++) {
    switch (sig_bt[i]) {
    case T_VOID:
      // halves of T_LONG or T_DOUBLE
      assert(i != 0 && (sig_bt[i - 1] == T_LONG || sig_bt[i - 1] == T_DOUBLE), "expecting half");
      regs[i].set_bad();
      break;
    case T_BOOLEAN:
    case T_CHAR:
    case T_BYTE:
    case T_SHORT:
    case T_INT:
      if (int_args < Argument::n_int_register_parameters_j) {
        regs[i].set1(INT_ArgReg[int_args++]->as_VMReg());
      } else {
        stk_args = align_up(stk_args, 2);
        regs[i].set1(VMRegImpl::stack2reg(stk_args));
        stk_args += 1;
      }
      break;
    case T_LONG:
      assert(sig_bt[i + 1] == T_VOID, "expecting half");
      // fall through
    case T_OBJECT:
    case T_ARRAY:
    case T_ADDRESS:
      if (int_args < Argument::n_int_register_parameters_j) {
        regs[i].set2(INT_ArgReg[int_args++]->as_VMReg());
      } else {
        stk_args = align_up(stk_args, 2);
        regs[i].set2(VMRegImpl::stack2reg(stk_args));
        stk_args += 2;
      }
      break;
    case T_FLOAT:
      if (fp_args < Argument::n_float_register_parameters_j) {
        regs[i].set1(FP_ArgReg[fp_args++]->as_VMReg());
      } else {
        stk_args = align_up(stk_args, 2);
        regs[i].set1(VMRegImpl::stack2reg(stk_args));
        stk_args += 1;
      }
      break;
    case T_DOUBLE:
      assert(sig_bt[i + 1] == T_VOID, "expecting half");
      if (fp_args < Argument::n_float_register_parameters_j) {
        regs[i].set2(FP_ArgReg[fp_args++]->as_VMReg());
      } else {
        stk_args = align_up(stk_args, 2);
        regs[i].set2(VMRegImpl::stack2reg(stk_args));
        stk_args += 2;
      }
      break;
    default:
      ShouldNotReachHere();
      break;
    }
  }

  return stk_args;
}

// Patch the callers callsite with entry to compiled code if it exists.
static void patch_callers_callsite(MacroAssembler *masm) {
  Label L;
  __ ld_d(AT, Address(Rmethod, Method::code_offset()));
  __ beqz(AT, L);

  __ enter();
  __ bstrins_d(SP, R0, 3, 0);  // align the stack
  __ push_call_clobbered_registers();

  // VM needs caller's callsite
  // VM needs target method
  // This needs to be a long call since we will relocate this adapter to
  // the codeBuffer and it may not reach

#ifndef PRODUCT
  assert(frame::arg_reg_save_area_bytes == 0, "not expecting frame reg save area");
#endif

  __ move(c_rarg0, Rmethod);
  __ move(c_rarg1, RA);
  __ call(CAST_FROM_FN_PTR(address, SharedRuntime::fixup_callers_callsite),
          relocInfo::runtime_call_type);

  __ pop_call_clobbered_registers();
  __ leave();
  __ bind(L);
}

static void gen_c2i_adapter(MacroAssembler *masm,
                            int total_args_passed,
                            int comp_args_on_stack,
                            const BasicType *sig_bt,
                            const VMRegPair *regs,
                            Label& skip_fixup) {
  // Before we get into the guts of the C2I adapter, see if we should be here
  // at all.  We've come from compiled code and are attempting to jump to the
  // interpreter, which means the caller made a static call to get here
  // (vcalls always get a compiled target if there is one).  Check for a
  // compiled target.  If there is one, we need to patch the caller's call.
  patch_callers_callsite(masm);

  __ bind(skip_fixup);

  // Since all args are passed on the stack, total_args_passed *
  // Interpreter::stackElementSize is the space we need.
  int extraspace = total_args_passed * Interpreter::stackElementSize;

  __ move(Rsender, SP);

  // stack is aligned, keep it that way
  extraspace = align_up(extraspace, 2 * wordSize);

  __ addi_d(SP, SP, -extraspace);

  // Now write the args into the outgoing interpreter space
  for (int i = 0; i < total_args_passed; i++) {
    if (sig_bt[i] == T_VOID) {
      assert(i > 0 && (sig_bt[i-1] == T_LONG || sig_bt[i-1] == T_DOUBLE), "missing half");
      continue;
    }

    // offset to start parameters
    int st_off = (total_args_passed - i - 1) * Interpreter::stackElementSize;
    int next_off = st_off - Interpreter::stackElementSize;

    // Say 4 args:
    // i   st_off
    // 0   32 T_LONG
    // 1   24 T_VOID
    // 2   16 T_OBJECT
    // 3    8 T_BOOL
    // -    0 return address
    //
    // However to make thing extra confusing. Because we can fit a Java long/double in
    // a single slot on a 64 bt vm and it would be silly to break them up, the interpreter
    // leaves one slot empty and only stores to a single slot. In this case the
    // slot that is occupied is the T_VOID slot. See I said it was confusing.

    VMReg r_1 = regs[i].first();
    VMReg r_2 = regs[i].second();
    if (!r_1->is_valid()) {
      assert(!r_2->is_valid(), "");
      continue;
    }
    if (r_1->is_stack()) {
      // memory to memory
      int ld_off = r_1->reg2stack() * VMRegImpl::stack_slot_size + extraspace;
      if (!r_2->is_valid()) {
        __ ld_wu(AT, Address(SP, ld_off));
        __ st_d(AT, Address(SP, st_off));
      } else {
        __ ld_d(AT, Address(SP, ld_off));

        // Two VMREgs|OptoRegs can be T_OBJECT, T_ADDRESS, T_DOUBLE, T_LONG
        // T_DOUBLE and T_LONG use two slots in the interpreter
        if (sig_bt[i] == T_LONG || sig_bt[i] == T_DOUBLE) {
          __ st_d(AT, Address(SP, next_off));
        } else {
          __ st_d(AT, Address(SP, st_off));
        }
      }
    } else if (r_1->is_Register()) {
      Register r = r_1->as_Register();
      if (!r_2->is_valid()) {
        // must be only an int (or less ) so move only 32bits to slot
        __ st_d(r, Address(SP, st_off));
      } else {
        // Two VMREgs|OptoRegs can be T_OBJECT, T_ADDRESS, T_DOUBLE, T_LONG
        // T_DOUBLE and T_LONG use two slots in the interpreter
        if (sig_bt[i] == T_LONG || sig_bt[i] == T_DOUBLE) {
          __ st_d(r, Address(SP, next_off));
        } else {
          __ st_d(r, Address(SP, st_off));
        }
      }
    } else {
      assert(r_1->is_FloatRegister(), "");
      FloatRegister fr = r_1->as_FloatRegister();
      if (!r_2->is_valid()) {
        // only a float use just part of the slot
        __ fst_s(fr, Address(SP, st_off));
      } else {
        __ fst_d(fr, Address(SP, next_off));
      }
    }
  }

  __ ld_d(AT, Address(Rmethod, Method::interpreter_entry_offset()));
  __ jr(AT);
}

void SharedRuntime::gen_i2c_adapter(MacroAssembler *masm,
                                    int total_args_passed,
                                    int comp_args_on_stack,
                                    const BasicType *sig_bt,
                                    const VMRegPair *regs) {
  // Note: Rsender contains the senderSP on entry. We must preserve
  // it since we may do a i2c -> c2i transition if we lose a race
  // where compiled code goes non-entrant while we get args ready.
  const Register saved_sp = T5;
  __ move(saved_sp, SP);

  // Cut-out for having no stack args.
  int comp_words_on_stack = align_up(comp_args_on_stack * VMRegImpl::stack_slot_size, wordSize) >> LogBytesPerWord;
  if (comp_args_on_stack != 0) {
    __ addi_d(SP, SP, -1 * comp_words_on_stack * wordSize);
  }

  // Align the outgoing SP
  assert(StackAlignmentInBytes == 16, "must be");
  __ bstrins_d(SP, R0, 3, 0);

  // Will jump to the compiled code just as if compiled code was doing it.
  // Pre-load the register-jump target early, to schedule it better.
  const Register comp_code_target = TSR;
  __ ld_d(comp_code_target, Rmethod, in_bytes(Method::from_compiled_offset()));

#if INCLUDE_JVMCI
  if (EnableJVMCI) {
    // check if this call should be routed towards a specific entry point
    __ ld_d(AT, Address(TREG, in_bytes(JavaThread::jvmci_alternate_call_target_offset())));
    Label no_alternative_target;
    __ beqz(AT, no_alternative_target);
    __ move(comp_code_target, AT);
    __ st_d(R0, Address(TREG, in_bytes(JavaThread::jvmci_alternate_call_target_offset())));
    __ bind(no_alternative_target);
  }
#endif // INCLUDE_JVMCI

  // Now generate the shuffle code.
  for (int i = 0; i < total_args_passed; i++) {
    if (sig_bt[i] == T_VOID) {
      assert(i > 0 && (sig_bt[i - 1] == T_LONG || sig_bt[i - 1] == T_DOUBLE), "missing half");
      continue;
    }

    // Pick up 0, 1 or 2 words from SP+offset.

    assert(!regs[i].second()->is_valid() || regs[i].first()->next() == regs[i].second(), "scrambled load targets?");
    // Load in argument order going down.
    int ld_off = (total_args_passed - i - 1) * Interpreter::stackElementSize;
    // Point to interpreter value (vs. tag)
    int next_off = ld_off - Interpreter::stackElementSize;

    VMReg r_1 = regs[i].first();
    VMReg r_2 = regs[i].second();
    if (!r_1->is_valid()) {
      assert(!r_2->is_valid(), "");
      continue;
    }
    if (r_1->is_stack()) {
      // Convert stack slot to an SP offset (+ wordSize to account for return address )
      int st_off = regs[i].first()->reg2stack() * VMRegImpl::stack_slot_size;
      if (!r_2->is_valid()) {
        __ ld_w(AT, Address(saved_sp, ld_off));
        __ st_d(AT, Address(SP, st_off));
      } else {
        // We are using two optoregs. This can be either T_OBJECT,
        // T_ADDRESS, T_LONG, or T_DOUBLE the interpreter allocates
        // two slots but only uses one for thr T_LONG or T_DOUBLE case
        // So we must adjust where to pick up the data to match the
        // interpreter.
        if (sig_bt[i] == T_LONG || sig_bt[i] == T_DOUBLE) {
          __ ld_d(AT, Address(saved_sp, next_off));
        } else {
          __ ld_d(AT, Address(saved_sp, ld_off));
        }
        __ st_d(AT, Address(SP, st_off));
      }
    } else if (r_1->is_Register()) {  // Register argument
      Register r = r_1->as_Register();
      if (r_2->is_valid()) {
        // We are using two VMRegs. This can be either T_OBJECT,
        // T_ADDRESS, T_LONG, or T_DOUBLE the interpreter allocates
        // two slots but only uses one for thr T_LONG or T_DOUBLE case
        // So we must adjust where to pick up the data to match the
        // interpreter.
        if (sig_bt[i] == T_LONG) {
          __ ld_d(r, Address(saved_sp, next_off));
        } else {
          __ ld_d(r, Address(saved_sp, ld_off));
        }
      } else {
        __ ld_w(r, Address(saved_sp, ld_off));
      }
    } else {
      assert(sig_bt[i] == T_FLOAT || sig_bt[i] == T_DOUBLE, "Must be float regs");
      FloatRegister fr = r_1->as_FloatRegister();
      if (!r_2->is_valid()) {
        __ fld_s(fr, Address(saved_sp, ld_off));
      } else {
        __ fld_d(fr, Address(saved_sp, next_off));
      }
    }
  }

  __ push_cont_fastpath(TREG); // Set JavaThread::_cont_fastpath to the sp of the oldest interpreted frame we know about

  // 6243940 We might end up in handle_wrong_method if
  // the callee is deoptimized as we race thru here. If that
  // happens we don't want to take a safepoint because the
  // caller frame will look interpreted and arguments are now
  // "compiled" so it is much better to make this transition
  // invisible to the stack walking code. Unfortunately if
  // we try and find the callee by normal means a safepoint
  // is possible. So we stash the desired callee in the thread
  // and the vm will find there should this case occur.

  __ st_d(Rmethod, Address(TREG, JavaThread::callee_target_offset()));

  // Jump to the compiled code just as if compiled code was doing it.
  __ jr(comp_code_target);
}

// ---------------------------------------------------------------
AdapterHandlerEntry* SharedRuntime::generate_i2c2i_adapters(MacroAssembler *masm,
                                                            int total_args_passed,
                                                            int comp_args_on_stack,
                                                            const BasicType *sig_bt,
                                                            const VMRegPair *regs,
                                                            AdapterFingerPrint* fingerprint) {
  address i2c_entry = __ pc();

  __ block_comment("gen_i2c_adapter");
  gen_i2c_adapter(masm, total_args_passed, comp_args_on_stack, sig_bt, regs);

  // -------------------------------------------------------------------------
  // Generate a C2I adapter.  On entry we know G5 holds the Method*.  The
  // args start out packed in the compiled layout.  They need to be unpacked
  // into the interpreter layout.  This will almost always require some stack
  // space.  We grow the current (compiled) stack, then repack the args.  We
  // finally end in a jump to the generic interpreter entry point.  On exit
  // from the interpreter, the interpreter will restore our SP (lest the
  // compiled code, which relies solely on SP and not FP, get sick).

  address c2i_unverified_entry = __ pc();
  Label skip_fixup;
  {
    __ block_comment("c2i_unverified_entry {");
    Register holder = IC_Klass;
    Register receiver = T0;
    Register temp = T8;
    address ic_miss = SharedRuntime::get_ic_miss_stub();

    Label missed;

    //add for compressedoops
    __ load_klass(temp, receiver);

    __ ld_d(AT, Address(holder, CompiledICHolder::holder_klass_offset()));
    __ ld_d(Rmethod, Address(holder, CompiledICHolder::holder_metadata_offset()));
    __ bne(AT, temp, missed);
    // Method might have been compiled since the call site was patched to
    // interpreted if that is the case treat it as a miss so we can get
    // the call site corrected.
    __ ld_d(AT, Address(Rmethod, Method::code_offset()));
    __ beq(AT, R0, skip_fixup);
    __ bind(missed);

    __ jmp(ic_miss, relocInfo::runtime_call_type);
    __ block_comment("} c2i_unverified_entry");
  }
  address c2i_entry = __ pc();

  // Class initialization barrier for static methods
  address c2i_no_clinit_check_entry = nullptr;
  if (VM_Version::supports_fast_class_init_checks()) {
    Label L_skip_barrier;
    address handle_wrong_method = SharedRuntime::get_handle_wrong_method_stub();

    { // Bypass the barrier for non-static methods
      __ ld_w(AT, Address(Rmethod, Method::access_flags_offset()));
      __ andi(AT, AT, JVM_ACC_STATIC);
      __ beqz(AT, L_skip_barrier); // non-static
    }

    __ load_method_holder(T4, Rmethod);
    __ clinit_barrier(T4, AT, &L_skip_barrier);
    __ jmp(handle_wrong_method, relocInfo::runtime_call_type);

    __ bind(L_skip_barrier);
    c2i_no_clinit_check_entry = __ pc();
  }

  BarrierSetAssembler* bs = BarrierSet::barrier_set()->barrier_set_assembler();
  __ block_comment("c2i_entry_barrier");
  bs->c2i_entry_barrier(masm);

  __ block_comment("gen_c2i_adapter");
  gen_c2i_adapter(masm, total_args_passed, comp_args_on_stack, sig_bt, regs, skip_fixup);

  return AdapterHandlerLibrary::new_entry(fingerprint, i2c_entry, c2i_entry, c2i_unverified_entry, c2i_no_clinit_check_entry);
}

int SharedRuntime::vector_calling_convention(VMRegPair *regs,
                                             uint num_bits,
                                             uint total_args_passed) {
  Unimplemented();
  return 0;
}

int SharedRuntime::c_calling_convention(const BasicType *sig_bt,
                                         VMRegPair *regs,
                                         VMRegPair *regs2,
                                         int total_args_passed) {
  assert(regs2 == nullptr, "not needed on LA");

  // We return the amount of VMRegImpl stack slots we need to reserve for all
  // the arguments NOT counting out_preserve_stack_slots.

  static const Register INT_ArgReg[Argument::n_int_register_parameters_c] = {
    c_rarg0, c_rarg1, c_rarg2, c_rarg3,
    c_rarg4, c_rarg5, c_rarg6, c_rarg7
  };
  static const FloatRegister FP_ArgReg[Argument::n_float_register_parameters_c] = {
    c_farg0, c_farg1, c_farg2, c_farg3,
    c_farg4, c_farg5, c_farg6, c_farg7
  };
  uint int_args = 0;
  uint fp_args = 0;
  uint stk_args = 0; // inc by 2 each time

// Example:
//    n   java.lang.UNIXProcess::forkAndExec
//     private native int forkAndExec(byte[] prog,
//                                    byte[] argBlock, int argc,
//                                    byte[] envBlock, int envc,
//                                    byte[] dir,
//                                    boolean redirectErrorStream,
//                                    FileDescriptor stdin_fd,
//                                    FileDescriptor stdout_fd,
//                                    FileDescriptor stderr_fd)
// JNIEXPORT jint JNICALL
// Java_java_lang_UNIXProcess_forkAndExec(JNIEnv *env,
//                                        jobject process,
//                                        jbyteArray prog,
//                                        jbyteArray argBlock, jint argc,
//                                        jbyteArray envBlock, jint envc,
//                                        jbyteArray dir,
//                                        jboolean redirectErrorStream,
//                                        jobject stdin_fd,
//                                        jobject stdout_fd,
//                                        jobject stderr_fd)
//
// ::c_calling_convention
//  0:      // env                 <--       a0
//  1: L    // klass/obj           <-- t0 => a1
//  2: [    // prog[]              <-- a0 => a2
//  3: [    // argBlock[]          <-- a1 => a3
//  4: I    // argc                <-- a2 => a4
//  5: [    // envBlock[]          <-- a3 => a5
//  6: I    // envc                <-- a4 => a5
//  7: [    // dir[]               <-- a5 => a7
//  8: Z    // redirectErrorStream <-- a6 => sp[0]
//  9: L    // stdin               <-- a7 => sp[8]
// 10: L    // stdout              fp[16] => sp[16]
// 11: L    // stderr              fp[24] => sp[24]
//
  for (int i = 0; i < total_args_passed; i++) {
    switch (sig_bt[i]) {
    case T_VOID: // Halves of longs and doubles
      assert(i != 0 && (sig_bt[i - 1] == T_LONG || sig_bt[i - 1] == T_DOUBLE), "expecting half");
      regs[i].set_bad();
      break;
    case T_BOOLEAN:
    case T_CHAR:
    case T_BYTE:
    case T_SHORT:
    case T_INT:
      if (int_args < Argument::n_int_register_parameters_c) {
        regs[i].set1(INT_ArgReg[int_args++]->as_VMReg());
      } else {
        regs[i].set1(VMRegImpl::stack2reg(stk_args));
        stk_args += 2;
      }
      break;
    case T_LONG:
      assert(sig_bt[i + 1] == T_VOID, "expecting half");
      // fall through
    case T_OBJECT:
    case T_ARRAY:
    case T_ADDRESS:
    case T_METADATA:
      if (int_args < Argument::n_int_register_parameters_c) {
        regs[i].set2(INT_ArgReg[int_args++]->as_VMReg());
      } else {
        regs[i].set2(VMRegImpl::stack2reg(stk_args));
        stk_args += 2;
      }
      break;
    case T_FLOAT:
      if (fp_args < Argument::n_float_register_parameters_c) {
        regs[i].set1(FP_ArgReg[fp_args++]->as_VMReg());
      } else if (int_args < Argument::n_int_register_parameters_c) {
        regs[i].set1(INT_ArgReg[int_args++]->as_VMReg());
      } else {
        regs[i].set1(VMRegImpl::stack2reg(stk_args));
        stk_args += 2;
      }
      break;
    case T_DOUBLE:
      assert(sig_bt[i + 1] == T_VOID, "expecting half");
      if (fp_args < Argument::n_float_register_parameters_c) {
        regs[i].set2(FP_ArgReg[fp_args++]->as_VMReg());
      } else if (int_args < Argument::n_int_register_parameters_c) {
        regs[i].set2(INT_ArgReg[int_args++]->as_VMReg());
      } else {
        regs[i].set2(VMRegImpl::stack2reg(stk_args));
        stk_args += 2;
      }
      break;
    default:
      ShouldNotReachHere();
      break;
    }
  }

  return align_up(stk_args, 2);
}

// ---------------------------------------------------------------------------
void SharedRuntime::save_native_result(MacroAssembler *masm, BasicType ret_type, int frame_slots) {
  // We always ignore the frame_slots arg and just use the space just below frame pointer
  // which by this time is free to use
  switch (ret_type) {
    case T_FLOAT:
      __ fst_s(FSF, FP, -3 * wordSize);
      break;
    case T_DOUBLE:
      __ fst_d(FSF, FP, -3 * wordSize);
      break;
    case T_VOID:  break;
    default: {
      __ st_d(V0, FP, -3 * wordSize);
    }
  }
}

void SharedRuntime::restore_native_result(MacroAssembler *masm, BasicType ret_type, int frame_slots) {
  // We always ignore the frame_slots arg and just use the space just below frame pointer
  // which by this time is free to use
  switch (ret_type) {
    case T_FLOAT:
      __ fld_s(FSF, FP, -3 * wordSize);
      break;
    case T_DOUBLE:
      __ fld_d(FSF, FP, -3 * wordSize);
      break;
    case T_VOID:  break;
    default: {
      __ ld_d(V0, FP, -3 * wordSize);
    }
  }
}

static void save_args(MacroAssembler *masm, int arg_count, int first_arg, VMRegPair *args) {
  for ( int i = first_arg ; i < arg_count ; i++ ) {
    if (args[i].first()->is_Register()) {
      __ push(args[i].first()->as_Register());
    } else if (args[i].first()->is_FloatRegister()) {
      __ push(args[i].first()->as_FloatRegister());
    }
  }
}

static void restore_args(MacroAssembler *masm, int arg_count, int first_arg, VMRegPair *args) {
  for ( int i = arg_count - 1 ; i >= first_arg ; i-- ) {
    if (args[i].first()->is_Register()) {
      __ pop(args[i].first()->as_Register());
    } else if (args[i].first()->is_FloatRegister()) {
      __ pop(args[i].first()->as_FloatRegister());
    }
  }
}

static void verify_oop_args(MacroAssembler* masm,
                            const methodHandle& method,
                            const BasicType* sig_bt,
                            const VMRegPair* regs) {
  if (VerifyOops) {
    // verify too many args may overflow the code buffer
    int arg_size = MIN2(64, (int)(method->size_of_parameters()));

    for (int i = 0; i < arg_size; i++) {
      if (is_reference_type(sig_bt[i])) {
        VMReg r = regs[i].first();
        assert(r->is_valid(), "bad oop arg");
        if (r->is_stack()) {
          __ verify_oop_addr(Address(SP, r->reg2stack() * VMRegImpl::stack_slot_size));
        } else {
          __ verify_oop(r->as_Register());
        }
      }
    }
  }
}

// on exit, sp points to the ContinuationEntry
OopMap* continuation_enter_setup(MacroAssembler* masm, int& stack_slots) {
  assert(ContinuationEntry::size() % VMRegImpl::stack_slot_size == 0, "");
  assert(in_bytes(ContinuationEntry::cont_offset())  % VMRegImpl::stack_slot_size == 0, "");
  assert(in_bytes(ContinuationEntry::chunk_offset()) % VMRegImpl::stack_slot_size == 0, "");

  stack_slots += checked_cast<int>(ContinuationEntry::size()) / wordSize;
  __ li(AT, checked_cast<int>(ContinuationEntry::size()));
  __ sub_d(SP, SP, AT);

  OopMap* map = new OopMap(((int)ContinuationEntry::size() + wordSize) / VMRegImpl::stack_slot_size, 0 /* arg_slots*/);

  __ ld_d(AT, Address(TREG, JavaThread::cont_entry_offset()));
  __ st_d(AT, Address(SP, ContinuationEntry::parent_offset()));
  __ st_d(SP, Address(TREG, JavaThread::cont_entry_offset()));

  return map;
}

// on entry j_rarg0 points to the continuation
//          SP points to ContinuationEntry
//          j_rarg2 -- isVirtualThread
void fill_continuation_entry(MacroAssembler* masm) {
#ifdef ASSERT
  __ li(AT, ContinuationEntry::cookie_value());
  __ st_w(AT, Address(SP, ContinuationEntry::cookie_offset()));
#endif

  __ st_d(j_rarg0, Address(SP, ContinuationEntry::cont_offset()));
  __ st_w(j_rarg2, Address(SP, ContinuationEntry::flags_offset()));
  __ st_d(R0, Address(SP, ContinuationEntry::chunk_offset()));
  __ st_w(R0, Address(SP, ContinuationEntry::argsize_offset()));
  __ st_w(R0, Address(SP, ContinuationEntry::pin_count_offset()));

  __ ld_d(AT, Address(TREG, JavaThread::cont_fastpath_offset()));
  __ st_d(AT, Address(SP, ContinuationEntry::parent_cont_fastpath_offset()));
  __ ld_d(AT, Address(TREG, JavaThread::held_monitor_count_offset()));
  __ st_d(AT, Address(SP, ContinuationEntry::parent_held_monitor_count_offset()));

  __ st_d(R0, Address(TREG, JavaThread::cont_fastpath_offset()));
  __ st_d(R0, Address(TREG, JavaThread::held_monitor_count_offset()));
}

// on entry, sp points to the ContinuationEntry
// on exit, fp points to the spilled fp + 2 * wordSize in the entry frame
void continuation_enter_cleanup(MacroAssembler* masm) {
#ifndef PRODUCT
  Label OK;
  __ ld_d(AT, Address(TREG, JavaThread::cont_entry_offset()));
  __ beq(SP, AT, OK);
  __ stop("incorrect sp for cleanup");
  __ bind(OK);
#endif

  __ ld_d(AT, Address(SP, ContinuationEntry::parent_cont_fastpath_offset()));
  __ st_d(AT, Address(TREG, JavaThread::cont_fastpath_offset()));
  __ ld_d(AT, Address(SP, ContinuationEntry::parent_held_monitor_count_offset()));
  __ st_d(AT, Address(TREG, JavaThread::held_monitor_count_offset()));

  __ ld_d(AT, Address(SP, ContinuationEntry::parent_offset()));
  __ st_d(AT, Address(TREG, JavaThread::cont_entry_offset()));

  // add 2 extra words to match up with leave()
  __ li(AT, (int)ContinuationEntry::size() + 2 * wordSize);
  __ add_d(FP, SP, AT);
}

// enterSpecial(Continuation c, boolean isContinue, boolean isVirtualThread)
// On entry: j_rarg0 (T0) -- the continuation object
//           j_rarg1 (A0) -- isContinue
//           j_rarg2 (A1) -- isVirtualThread
static void gen_continuation_enter(MacroAssembler* masm,
                                   const methodHandle& method,
                                   const BasicType* sig_bt,
                                   const VMRegPair* regs,
                                   int& exception_offset,
                                   OopMapSet*oop_maps,
                                   int& frame_complete,
                                   int& stack_slots,
                                   int& interpreted_entry_offset,
                                   int& compiled_entry_offset) {
  AddressLiteral resolve(SharedRuntime::get_resolve_static_call_stub(),
                         relocInfo::static_call_type);

  address start = __ pc();

  Label call_thaw, exit;

  // i2i entry used at interp_only_mode only
  interpreted_entry_offset = __ pc() - start;
  {
#ifdef ASSERT
    Label is_interp_only;
    __ ld_w(AT, Address(TREG, JavaThread::interp_only_mode_offset()));
    __ bnez(AT, is_interp_only);
    __ stop("enterSpecial interpreter entry called when not in interp_only_mode");
    __ bind(is_interp_only);
#endif

    // Read interpreter arguments into registers (this is an ad-hoc i2c adapter)
    __ ld_d(j_rarg0, Address(SP, Interpreter::stackElementSize * 2));
    __ ld_d(j_rarg1, Address(SP, Interpreter::stackElementSize * 1));
    __ ld_d(j_rarg2, Address(SP, Interpreter::stackElementSize * 0));
    __ push_cont_fastpath(TREG);

    __ enter();
    stack_slots = 2; // will be adjusted in setup
    OopMap* map = continuation_enter_setup(masm, stack_slots);
    // The frame is complete here, but we only record it for the compiled entry, so the frame would appear unsafe,
    // but that's okay because at the very worst we'll miss an async sample, but we're in interp_only_mode anyway.

    fill_continuation_entry(masm);

    __ bnez(j_rarg1, call_thaw);

    address mark = __ pc();
    __ trampoline_call(resolve);

    oop_maps->add_gc_map(__ pc() - start, map);
    __ post_call_nop();

    __ b(exit);

    CodeBuffer* cbuf = masm->code_section()->outer();
    CompiledStaticCall::emit_to_interp_stub(*cbuf, mark);
  }

  // compiled entry
  __ align(CodeEntryAlignment);
  compiled_entry_offset = __ pc() - start;

  __ enter();
  stack_slots = 2; // will be adjusted in setup
  OopMap* map = continuation_enter_setup(masm, stack_slots);
  frame_complete = __ pc() - start;

  fill_continuation_entry(masm);

  __ bnez(j_rarg1, call_thaw);

  address mark = __ pc();
  __ trampoline_call(resolve);

  oop_maps->add_gc_map(__ pc() - start, map);
  __ post_call_nop();

  __ b(exit);

  __ bind(call_thaw);

  __ call(CAST_FROM_FN_PTR(address, StubRoutines::cont_thaw()), relocInfo::runtime_call_type);
  oop_maps->add_gc_map(__ pc() - start, map->deep_copy());
  ContinuationEntry::_return_pc_offset = __ pc() - start;
  __ post_call_nop();

  __ bind(exit);

  // We've succeeded, set sp to the ContinuationEntry
  __ ld_d(SP, Address(TREG, JavaThread::cont_entry_offset()));
  continuation_enter_cleanup(masm);
  __ leave();
  __ jr(RA);

  // exception handling
  exception_offset = __ pc() - start;
  {
    __ move(TSR, A0); // save return value contaning the exception oop in callee-saved TSR

    // We've succeeded, set sp to the ContinuationEntry
    __ ld_d(SP, Address(TREG, JavaThread::cont_entry_offset()));
    continuation_enter_cleanup(masm);

    __ ld_d(c_rarg1, Address(FP, -1 * wordSize)); // return address
    __ call_VM_leaf(CAST_FROM_FN_PTR(address, SharedRuntime::exception_handler_for_return_address), TREG, c_rarg1);

    // Continue at exception handler:
    //   A0: exception oop
    //   T4: exception handler
    //   A1: exception pc
    __ move(T4, A0);
    __ move(A0, TSR);
    __ verify_oop(A0);

    __ leave();
    __ move(A1, RA);
    __ jr(T4);
  }

  CodeBuffer* cbuf = masm->code_section()->outer();
  CompiledStaticCall::emit_to_interp_stub(*cbuf, mark);
}

static void gen_continuation_yield(MacroAssembler* masm,
                                   const methodHandle& method,
                                   const BasicType* sig_bt,
                                   const VMRegPair* regs,
                                   int& exception_offset,
                                   OopMapSet* oop_maps,
                                   int& frame_complete,
                                   int& stack_slots,
                                   int& interpreted_entry_offset,
                                   int& compiled_entry_offset) {
  enum layout {
    fp_off,
    fp_off2,
    return_off,
    return_off2,
    framesize // inclusive of return address
  };

  stack_slots = framesize / VMRegImpl::slots_per_word;
  assert(stack_slots == 2, "recheck layout");

  address start = __ pc();

  compiled_entry_offset = __ pc() - start;
  __ enter();

  __ move(c_rarg1, SP);

  frame_complete = __ pc() - start;
  address the_pc = __ pc();

  Label L;
  __ bind(L);

  __ post_call_nop(); // this must be exactly after the pc value that is pushed into the frame info, we use this nop for fast CodeBlob lookup

  __ move(c_rarg0, TREG);
  __ set_last_Java_frame(TREG, SP, FP, L);
  __ call_VM_leaf(Continuation::freeze_entry(), 2);
  __ reset_last_Java_frame(true);

  Label pinned;

  __ bnez(A0, pinned);

  // We've succeeded, set sp to the ContinuationEntry
  __ ld_d(SP, Address(TREG, JavaThread::cont_entry_offset()));
  continuation_enter_cleanup(masm);

  __ bind(pinned); // pinned -- return to caller

  // handle pending exception thrown by freeze
  __ ld_d(AT, Address(TREG, in_bytes(Thread::pending_exception_offset())));
  Label ok;
  __ beqz(AT, ok);
  __ leave();
  __ jmp(StubRoutines::forward_exception_entry(), relocInfo::runtime_call_type);
  __ bind(ok);

  __ leave();
  __ jr(RA);

  OopMap* map = new OopMap(framesize, 1);
  oop_maps->add_gc_map(the_pc - start, map);
}

static void gen_special_dispatch(MacroAssembler* masm,
                                 const methodHandle& method,
                                 const BasicType* sig_bt,
                                 const VMRegPair* regs) {
  verify_oop_args(masm, method, sig_bt, regs);
  vmIntrinsics::ID iid = method->intrinsic_id();

  // Now write the args into the outgoing interpreter space
  bool     has_receiver   = false;
  Register receiver_reg   = noreg;
  int      member_arg_pos = -1;
  Register member_reg     = noreg;
  int      ref_kind       = MethodHandles::signature_polymorphic_intrinsic_ref_kind(iid);
  if (ref_kind != 0) {
    member_arg_pos = method->size_of_parameters() - 1;  // trailing MemberName argument
    member_reg = S3;  // known to be free at this point
    has_receiver = MethodHandles::ref_kind_has_receiver(ref_kind);
  } else if (iid == vmIntrinsics::_invokeBasic) {
    has_receiver = true;
  } else if (iid == vmIntrinsics::_linkToNative) {
    member_arg_pos = method->size_of_parameters() - 1;  // trailing NativeEntryPoint argument
    member_reg = S3;  // known to be free at this point
  } else {
    fatal("unexpected intrinsic id %d", vmIntrinsics::as_int(iid));
  }

  if (member_reg != noreg) {
    // Load the member_arg into register, if necessary.
    SharedRuntime::check_member_name_argument_is_last_argument(method, sig_bt, regs);
    VMReg r = regs[member_arg_pos].first();
    if (r->is_stack()) {
      __ ld_d(member_reg, Address(SP, r->reg2stack() * VMRegImpl::stack_slot_size));
    } else {
      // no data motion is needed
      member_reg = r->as_Register();
    }
  }

  if (has_receiver) {
    // Make sure the receiver is loaded into a register.
    assert(method->size_of_parameters() > 0, "oob");
    assert(sig_bt[0] == T_OBJECT, "receiver argument must be an object");
    VMReg r = regs[0].first();
    assert(r->is_valid(), "bad receiver arg");
    if (r->is_stack()) {
      // Porting note:  This assumes that compiled calling conventions always
      // pass the receiver oop in a register.  If this is not true on some
      // platform, pick a temp and load the receiver from stack.
      fatal("receiver always in a register");
      receiver_reg = T6;  // known to be free at this point
      __ ld_d(receiver_reg, Address(SP, r->reg2stack() * VMRegImpl::stack_slot_size));
    } else {
      // no data motion is needed
      receiver_reg = r->as_Register();
    }
  }

  // Figure out which address we are really jumping to:
  MethodHandles::generate_method_handle_dispatch(masm, iid,
                                                 receiver_reg, member_reg, /*for_compiler_entry:*/ true);
}

// ---------------------------------------------------------------------------
// Generate a native wrapper for a given method.  The method takes arguments
// in the Java compiled code convention, marshals them to the native
// convention (handlizes oops, etc), transitions to native, makes the call,
// returns to java state (possibly blocking), unhandlizes any result and
// returns.
nmethod *SharedRuntime::generate_native_wrapper(MacroAssembler* masm,
                                                const methodHandle& method,
                                                int compile_id,
                                                BasicType* in_sig_bt,
                                                VMRegPair* in_regs,
                                                BasicType ret_type) {
  if (method->is_continuation_native_intrinsic()) {
    int vep_offset = 0;
    int exception_offset = 0;
    int frame_complete = 0;
    int stack_slots = 0;
    OopMapSet* oop_maps =  new OopMapSet();
    int interpreted_entry_offset = -1;
    if (method->is_continuation_enter_intrinsic()) {
      gen_continuation_enter(masm,
                             method,
                             in_sig_bt,
                             in_regs,
                             exception_offset,
                             oop_maps,
                             frame_complete,
                             stack_slots,
                             interpreted_entry_offset,
                             vep_offset);
    } else if (method->is_continuation_yield_intrinsic()) {
      gen_continuation_yield(masm,
                             method,
                             in_sig_bt,
                             in_regs,
                             exception_offset,
                             oop_maps,
                             frame_complete,
                             stack_slots,
                             interpreted_entry_offset,
                             vep_offset);
    } else {
      guarantee(false, "Unknown Continuation native intrinsic");
    }

    __ flush();
    nmethod* nm = nmethod::new_native_nmethod(method,
                                              compile_id,
                                              masm->code(),
                                              vep_offset,
                                              frame_complete,
                                              stack_slots,
                                              in_ByteSize(-1),
                                              in_ByteSize(-1),
                                              oop_maps,
                                              exception_offset);
    if (method->is_continuation_enter_intrinsic()) {
      ContinuationEntry::set_enter_code(nm, interpreted_entry_offset);
    } else if (method->is_continuation_yield_intrinsic()) {
      _cont_doYield_stub = nm;
    } else {
      guarantee(false, "Unknown Continuation native intrinsic");
    }
    return nm;
  }

  if (method->is_method_handle_intrinsic()) {
    vmIntrinsics::ID iid = method->intrinsic_id();
    intptr_t start = (intptr_t)__ pc();
    int vep_offset = ((intptr_t)__ pc()) - start;
    gen_special_dispatch(masm,
                         method,
                         in_sig_bt,
                         in_regs);
    assert(((intptr_t)__ pc() - start - vep_offset) >= 1 * BytesPerInstWord,
           "valid size for make_non_entrant");
    int frame_complete = ((intptr_t)__ pc()) - start;  // not complete, period
    __ flush();
    int stack_slots = SharedRuntime::out_preserve_stack_slots();  // no out slots at all, actually
    return nmethod::new_native_nmethod(method,
                                       compile_id,
                                       masm->code(),
                                       vep_offset,
                                       frame_complete,
                                       stack_slots / VMRegImpl::slots_per_word,
                                       in_ByteSize(-1),
                                       in_ByteSize(-1),
                                       nullptr);
  }

  address native_func = method->native_function();
  assert(native_func != nullptr, "must have function");

  // Native nmethod wrappers never take possession of the oop arguments.
  // So the caller will gc the arguments. The only thing we need an
  // oopMap for is if the call is static
  //
  // An OopMap for lock (and class if static), and one for the VM call itself
  OopMapSet *oop_maps = new OopMapSet();

  // We have received a description of where all the java arg are located
  // on entry to the wrapper. We need to convert these args to where
  // the jni function will expect them. To figure out where they go
  // we convert the java signature to a C signature by inserting
  // the hidden arguments as arg[0] and possibly arg[1] (static method)

  const int total_in_args = method->size_of_parameters();
  int total_c_args = total_in_args + (method->is_static() ? 2 : 1);

  BasicType* out_sig_bt = NEW_RESOURCE_ARRAY(BasicType, total_c_args);
  VMRegPair* out_regs   = NEW_RESOURCE_ARRAY(VMRegPair, total_c_args);
  BasicType* in_elem_bt = nullptr;

  int argc = 0;
  out_sig_bt[argc++] = T_ADDRESS;
  if (method->is_static()) {
    out_sig_bt[argc++] = T_OBJECT;
  }

  for (int i = 0; i < total_in_args ; i++ ) {
    out_sig_bt[argc++] = in_sig_bt[i];
  }

  // Now figure out where the args must be stored and how much stack space
  // they require (neglecting out_preserve_stack_slots but space for storing
  // the 1st six register arguments). It's weird see int_stk_helper.
  //
  int out_arg_slots;
  out_arg_slots = c_calling_convention(out_sig_bt, out_regs, nullptr, total_c_args);

  // Compute framesize for the wrapper.  We need to handlize all oops in
  // registers. We must create space for them here that is disjoint from
  // the windowed save area because we have no control over when we might
  // flush the window again and overwrite values that gc has since modified.
  // (The live window race)
  //
  // We always just allocate 6 word for storing down these object. This allow
  // us to simply record the base and use the Ireg number to decide which
  // slot to use. (Note that the reg number is the inbound number not the
  // outbound number).
  // We must shuffle args to match the native convention, and include var-args space.

  // Calculate the total number of stack slots we will need.

  // First count the abi requirement plus all of the outgoing args
  int stack_slots = SharedRuntime::out_preserve_stack_slots() + out_arg_slots;

  // Now the space for the inbound oop handle area
  int total_save_slots = Argument::n_int_register_parameters_j * VMRegImpl::slots_per_word;

  int oop_handle_offset = stack_slots;
  stack_slots += total_save_slots;

  // Now any space we need for handlizing a klass if static method

  int klass_slot_offset = 0;
  int klass_offset = -1;
  int lock_slot_offset = 0;
  bool is_static = false;

  if (method->is_static()) {
    klass_slot_offset = stack_slots;
    stack_slots += VMRegImpl::slots_per_word;
    klass_offset = klass_slot_offset * VMRegImpl::stack_slot_size;
    is_static = true;
  }

  // Plus a lock if needed

  if (method->is_synchronized()) {
    lock_slot_offset = stack_slots;
    stack_slots += VMRegImpl::slots_per_word;
  }

  // Now a place (+2) to save return value or as a temporary for any gpr -> fpr moves
  // + 4 for return address (which we own) and saved fp
  stack_slots += 6;

  // Ok The space we have allocated will look like:
  //
  //
  // FP-> |                     |
  //      | 2 slots (ra)        |
  //      | 2 slots (fp)        |
  //      |---------------------|
  //      | 2 slots for moves   |
  //      |---------------------|
  //      | lock box (if sync)  |
  //      |---------------------| <- lock_slot_offset
  //      | klass (if static)   |
  //      |---------------------| <- klass_slot_offset
  //      | oopHandle area      |
  //      |---------------------| <- oop_handle_offset
  //      | outbound memory     |
  //      | based arguments     |
  //      |                     |
  //      |---------------------|
  //      | vararg area         |
  //      |---------------------|
  //      |                     |
  // SP-> | out_preserved_slots |
  //
  //


  // Now compute actual number of stack words we need rounding to make
  // stack properly aligned.
  stack_slots = align_up(stack_slots, StackAlignmentInSlots);

  int stack_size = stack_slots * VMRegImpl::stack_slot_size;

  intptr_t start = (intptr_t)__ pc();



  // First thing make an ic check to see if we should even be here
  address ic_miss = SharedRuntime::get_ic_miss_stub();

  // We are free to use all registers as temps without saving them and
  // restoring them except fp. fp is the only callee save register
  // as far as the interpreter and the compiler(s) are concerned.

  const Register ic_reg = IC_Klass;
  const Register receiver = T0;

  Label hit;
  Label exception_pending;

  __ verify_oop(receiver);
  //add for compressedoops
  __ load_klass(T4, receiver);
  __ beq(T4, ic_reg, hit);
  __ jmp(ic_miss, relocInfo::runtime_call_type);
  __ bind(hit);

  int vep_offset = ((intptr_t)__ pc()) - start;

  if (VM_Version::supports_fast_class_init_checks() && method->needs_clinit_barrier()) {
    Label L_skip_barrier;
    address handle_wrong_method = SharedRuntime::get_handle_wrong_method_stub();
    __ mov_metadata(T4, method->method_holder()); // InstanceKlass*
    __ clinit_barrier(T4, AT, &L_skip_barrier);
    __ jmp(handle_wrong_method, relocInfo::runtime_call_type);

    __ bind(L_skip_barrier);
  }

  // Generate stack overflow check
  __ bang_stack_with_offset((int)StackOverflow::stack_shadow_zone_size());

  // The instruction at the verified entry point must be 4 bytes or longer
  // because it can be patched on the fly by make_non_entrant.
  if (((intptr_t)__ pc() - start - vep_offset) < 1 * BytesPerInstWord) {
    __ nop();
  }

  // Generate a new frame for the wrapper.
  // do LA need this ?
  __ st_d(SP, Address(TREG, JavaThread::last_Java_sp_offset()));
  assert(StackAlignmentInBytes == 16, "must be");
  __ bstrins_d(SP, R0, 3, 0);

  __ enter();
  // -2 because return address is already present and so is saved fp
  __ addi_d(SP, SP, -1 * (stack_size - 2*wordSize));

  BarrierSetAssembler* bs = BarrierSet::barrier_set()->barrier_set_assembler();
  bs->nmethod_entry_barrier(masm, nullptr /* slow_path */, nullptr /* continuation */, nullptr /* guard */);

  // Frame is now completed as far a size and linkage.

  int frame_complete = ((intptr_t)__ pc()) - start;

  // Calculate the difference between sp and fp. We need to know it
  // after the native call because on windows Java Natives will pop
  // the arguments and it is painful to do sp relative addressing
  // in a platform independent way. So after the call we switch to
  // fp relative addressing.
  //FIXME actually , the fp_adjustment may not be the right, because andr(sp, sp, at) may change
  //the SP
  int fp_adjustment = stack_size;

  // Compute the fp offset for any slots used after the jni call

  int lock_slot_fp_offset = (lock_slot_offset*VMRegImpl::stack_slot_size) - fp_adjustment;

  // We use S4 as the oop handle for the receiver/klass
  // It is callee save so it survives the call to native

  const Register oop_handle_reg = S4;

  // Move arguments from register/stack to register/stack.
  // --------------------------------------------------------------------------
  //
  // We immediately shuffle the arguments so that for any vm call we have
  // to make from here on out (sync slow path, jvmti, etc.) we will have
  // captured the oops from our caller and have a valid oopMap for them.

  // -----------------
  // The Grand Shuffle
  //
  // Natives require 1 or 2 extra arguments over the normal ones: the JNIEnv*
  // and, if static, the class mirror instead of a receiver.  This pretty much
  // guarantees that register layout will not match (and LA doesn't use reg
  // parms though amd does).  Since the native abi doesn't use register args
  // and the java conventions does we don't have to worry about collisions.
  // All of our moved are reg->stack or stack->stack.
  // We ignore the extra arguments during the shuffle and handle them at the
  // last moment. The shuffle is described by the two calling convention
  // vectors we have in our possession. We simply walk the java vector to
  // get the source locations and the c vector to get the destinations.

  // Record sp-based slot for receiver on stack for non-static methods
  int receiver_offset = -1;

  // This is a trick. We double the stack slots so we can claim
  // the oops in the caller's frame. Since we are sure to have
  // more args than the caller doubling is enough to make sure
  // we can capture all the incoming oop args from the caller.
  OopMap* map = new OopMap(stack_slots * 2, 0 /* arg_slots*/);

#ifdef ASSERT
  bool reg_destroyed[Register::number_of_registers];
  bool freg_destroyed[FloatRegister::number_of_registers];
  for ( int r = 0 ; r < Register::number_of_registers ; r++ ) {
    reg_destroyed[r] = false;
  }
  for ( int f = 0 ; f < FloatRegister::number_of_registers ; f++ ) {
    freg_destroyed[f] = false;
  }

#endif /* ASSERT */

  // We move the arguments backward because the floating point registers
  // destination will always be to a register with a greater or equal
  // register number or the stack.
  //   in  is the index of the incoming Java arguments
  //   out is the index of the outgoing C arguments

  for (int in = total_in_args - 1, out = total_c_args - 1; in >= 0; in--, out--) {
    __ block_comment(err_msg("move %d -> %d", in, out));
#ifdef ASSERT
    if (in_regs[in].first()->is_Register()) {
      assert(!reg_destroyed[in_regs[in].first()->as_Register()->encoding()], "destroyed reg!");
    } else if (in_regs[in].first()->is_FloatRegister()) {
      assert(!freg_destroyed[in_regs[in].first()->as_FloatRegister()->encoding()], "destroyed reg!");
    }
    if (out_regs[out].first()->is_Register()) {
      reg_destroyed[out_regs[out].first()->as_Register()->encoding()] = true;
    } else if (out_regs[out].first()->is_FloatRegister()) {
      freg_destroyed[out_regs[out].first()->as_FloatRegister()->encoding()] = true;
    }
#endif /* ASSERT */
    switch (in_sig_bt[in]) {
      case T_BOOLEAN:
      case T_CHAR:
      case T_BYTE:
      case T_SHORT:
      case T_INT:
        __ simple_move32(in_regs[in], out_regs[out]);
        break;
      case T_ARRAY:
      case T_OBJECT:
        __ object_move(map, oop_handle_offset, stack_slots,
                    in_regs[in], out_regs[out],
                    ((in == 0) && (!is_static)), &receiver_offset);
        break;
      case T_VOID:
        break;
      case T_FLOAT:
        __ float_move(in_regs[in], out_regs[out]);
        break;
      case T_DOUBLE:
        assert(in + 1 < total_in_args &&
               in_sig_bt[in + 1] == T_VOID &&
               out_sig_bt[out + 1] == T_VOID, "bad arg list");
        __ double_move(in_regs[in], out_regs[out]);
        break;
      case T_LONG :
        __ long_move(in_regs[in], out_regs[out]);
        break;
      case T_ADDRESS:
        fatal("found T_ADDRESS in java args");
        break;
      default:
        ShouldNotReachHere();
        break;
    }
  }

  // point c_arg at the first arg that is already loaded in case we
  // need to spill before we call out
  int c_arg = total_c_args - total_in_args;

  // Pre-load a static method's oop into c_rarg1.
  // Used both by locking code and the normal JNI call code.
  if (method->is_static()) {

    // load oop into a register
    __ movoop(c_rarg1,
              JNIHandles::make_local(method->method_holder()->java_mirror()));

    // Now handlize the static class mirror it's known not-null.
    __ st_d(c_rarg1, SP, klass_offset);
    map->set_oop(VMRegImpl::stack2reg(klass_slot_offset));

    // Now get the handle
    __ lea(c_rarg1, Address(SP, klass_offset));
    // and protect the arg if we must spill
    c_arg--;
  }

  // Change state to native (we save the return address in the thread, since it might not
  // be pushed on the stack when we do a a stack traversal). It is enough that the pc()
  // points into the right code segment. It does not have to be the correct return pc.
  // We use the same pc/oopMap repeatedly when we call out

  Label native_return;
  __ set_last_Java_frame(SP, noreg, native_return);

  // We have all of the arguments setup at this point. We must not touch any register
  // argument registers at this point (what if we save/restore them there are no oop?
  {
    SkipIfEqual skip_if(masm, &DTraceMethodProbes, 0);
    save_args(masm, total_c_args, c_arg, out_regs);
    __ mov_metadata(c_rarg1, method());
    __ call_VM_leaf(
         CAST_FROM_FN_PTR(address, SharedRuntime::dtrace_method_entry),
         TREG, c_rarg1);
    restore_args(masm, total_c_args, c_arg, out_regs);
  }

  // RedefineClasses() tracing support for obsolete method entry
  if (log_is_enabled(Trace, redefine, class, obsolete)) {
    // protect the args we've loaded
    save_args(masm, total_c_args, c_arg, out_regs);
    __ mov_metadata(c_rarg1, method());
    __ call_VM_leaf(
         CAST_FROM_FN_PTR(address, SharedRuntime::rc_trace_method_entry),
         TREG, c_rarg1);
    restore_args(masm, total_c_args, c_arg, out_regs);
  }

  // These are register definitions we need for locking/unlocking
  const Register swap_reg = T8;  // Must use T8 for cmpxchg instruction
  const Register obj_reg  = T4;  // Will contain the oop
  const Register lock_reg = T0;  // Address of compiler lock object (BasicLock)

  Label slow_path_lock;
  Label lock_done;

  // Lock a synchronized method
  if (method->is_synchronized()) {
    Label count;
    const int mark_word_offset = BasicLock::displaced_header_offset_in_bytes();

    // Get the handle (the 2nd argument)
    __ move(oop_handle_reg, A1);

    // Get address of the box
    __ lea(lock_reg, Address(FP, lock_slot_fp_offset));

    // Load the oop from the handle
    __ ld_d(obj_reg, oop_handle_reg, 0);

    if (LockingMode == LM_MONITOR) {
      __ b(slow_path_lock);
    } else if (LockingMode == LM_LEGACY) {
      // Load immediate 1 into swap_reg %T8
      __ li(swap_reg, 1);

      __ ld_d(AT, obj_reg, 0);
      __ orr(swap_reg, swap_reg, AT);

      __ st_d(swap_reg, lock_reg, mark_word_offset);
      __ cmpxchg(Address(obj_reg, 0), swap_reg, lock_reg, AT, true, true /* acquire */, count);
      // Test if the oopMark is an obvious stack pointer, i.e.,
      //  1) (mark & 3) == 0, and
      //  2) sp <= mark < mark + os::pagesize()
      // These 3 tests can be done by evaluating the following
      // expression: ((mark - sp) & (3 - os::vm_page_size())),
      // assuming both stack pointer and pagesize have their
      // least significant 2 bits clear.
      // NOTE: the oopMark is in swap_reg %T8 as the result of cmpxchg

      __ sub_d(swap_reg, swap_reg, SP);
      __ li(AT, 3 - (int)os::vm_page_size());
      __ andr(swap_reg , swap_reg, AT);
      // Save the test result, for recursive case, the result is zero
      __ st_d(swap_reg, lock_reg, mark_word_offset);
      __ bne(swap_reg, R0, slow_path_lock);
    } else {
      assert(LockingMode == LM_LIGHTWEIGHT, "must be");
      __ ld_d(swap_reg, Address(obj_reg, oopDesc::mark_offset_in_bytes()));
      // FIXME
      Register tmp = T1;
      __ lightweight_lock(obj_reg, swap_reg, tmp, SCR1, slow_path_lock);
    }

    __ bind(count);
    __ increment(Address(TREG, JavaThread::held_monitor_count_offset()), 1);

    // Slow path will re-enter here
    __ bind(lock_done);
  }


  // Finally just about ready to make the JNI call


  // get JNIEnv* which is first argument to native
  __ addi_d(A0, TREG, in_bytes(JavaThread::jni_environment_offset()));

  // Now set thread in native
  __ addi_d(AT, R0, _thread_in_native);
  if (os::is_MP()) {
    __ addi_d(T4, TREG, in_bytes(JavaThread::thread_state_offset()));
    __ amswap_db_w(R0, AT, T4);
  } else {
    __ st_w(AT, TREG, in_bytes(JavaThread::thread_state_offset()));
  }

  // do the call
  __ call(native_func, relocInfo::runtime_call_type);
  __ bind(native_return);

  oop_maps->add_gc_map(((intptr_t)__ pc()) - start, map);

  // WARNING - on Windows Java Natives use pascal calling convention and pop the
  // arguments off of the stack. We could just re-adjust the stack pointer here
  // and continue to do SP relative addressing but we instead switch to FP
  // relative addressing.

  // Unpack native results.
  if (ret_type != T_OBJECT && ret_type != T_ARRAY) {
    __ cast_primitive_type(ret_type, V0);
  }

  Label after_transition;

  // Switch thread to "native transition" state before reading the synchronization state.
  // This additional state is necessary because reading and testing the synchronization
  // state is not atomic w.r.t. GC, as this scenario demonstrates:
  //     Java thread A, in _thread_in_native state, loads _not_synchronized and is preempted.
  //     VM thread changes sync state to synchronizing and suspends threads for GC.
  //     Thread A is resumed to finish this native method, but doesn't block here since it
  //     didn't see any synchronization is progress, and escapes.
  __ addi_d(AT, R0, _thread_in_native_trans);

  // Force this write out before the read below
  if (os::is_MP() && UseSystemMemoryBarrier) {
    __ addi_d(T4, TREG, in_bytes(JavaThread::thread_state_offset()));
    __ amswap_db_w(R0, AT, T4); // AnyAny
  } else {
    __ st_w(AT, TREG, in_bytes(JavaThread::thread_state_offset()));
  }

  // check for safepoint operation in progress and/or pending suspend requests
  {
    Label Continue;
    Label slow_path;

    // We need an acquire here to ensure that any subsequent load of the
    // global SafepointSynchronize::_state flag is ordered after this load
    // of the thread-local polling word.  We don't want this poll to
    // return false (i.e. not safepointing) and a later poll of the global
    // SafepointSynchronize::_state spuriously to return true.
    //
    // This is to avoid a race when we're in a native->Java transition
    // racing the code which wakes up from a safepoint.

    __ safepoint_poll(slow_path, TREG, true /* at_return */, true /* acquire */, false /* in_nmethod */);
    __ ld_w(AT, TREG, in_bytes(JavaThread::suspend_flags_offset()));
    __ beq(AT, R0, Continue);
    __ bind(slow_path);

    // Don't use call_VM as it will see a possible pending exception and forward it
    // and never return here preventing us from clearing _last_native_pc down below.
    //
    save_native_result(masm, ret_type, stack_slots);
    __ move(A0, TREG);
    __ addi_d(SP, SP, -wordSize);
    __ push(S2);
    __ move(S2, SP);     // use S2 as a sender SP holder
    assert(StackAlignmentInBytes == 16, "must be");
    __ bstrins_d(SP, R0, 3, 0); // align stack as required by ABI
    __ call(CAST_FROM_FN_PTR(address, JavaThread::check_special_condition_for_native_trans), relocInfo::runtime_call_type);
    __ move(SP, S2);     // use S2 as a sender SP holder
    __ pop(S2);
    __ addi_d(SP, SP, wordSize);
    // Restore any method result value
    restore_native_result(masm, ret_type, stack_slots);

    __ bind(Continue);
  }

  // change thread state
  __ addi_d(AT, R0, _thread_in_Java);
  if (os::is_MP()) {
    __ addi_d(T4, TREG, in_bytes(JavaThread::thread_state_offset()));
    __ amswap_db_w(R0, AT, T4);
  } else {
    __ st_w(AT, TREG, in_bytes(JavaThread::thread_state_offset()));
  }
  __ bind(after_transition);
  Label reguard;
  Label reguard_done;
  __ ld_w(AT, TREG, in_bytes(JavaThread::stack_guard_state_offset()));
  __ addi_d(AT, AT, -StackOverflow::stack_guard_yellow_reserved_disabled);
  __ beq(AT, R0, reguard);
  // slow path reguard  re-enters here
  __ bind(reguard_done);

  // Handle possible exception (will unlock if necessary)

  // native result if any is live

  // Unlock
  Label slow_path_unlock;
  Label unlock_done;
  if (method->is_synchronized()) {

    // Get locked oop from the handle we passed to jni
    __ ld_d( obj_reg, oop_handle_reg, 0);

    Label done, not_recursive;

    if (LockingMode == LM_LEGACY) {
      // Simple recursive lock?
      __ ld_d(AT, FP, lock_slot_fp_offset);
      __ bnez(AT, not_recursive);
      __ decrement(Address(TREG, JavaThread::held_monitor_count_offset()), 1);
      __ b(done);
    }

    __ bind(not_recursive);

    // Must save FSF if if it is live now because cmpxchg must use it
    if (ret_type != T_FLOAT && ret_type != T_DOUBLE && ret_type != T_VOID) {
      save_native_result(masm, ret_type, stack_slots);
    }

    if (LockingMode == LM_MONITOR) {
      __ b(slow_path_unlock);
    } else if (LockingMode == LM_LEGACY) {
      //  get old displaced header
      __ ld_d(T8, FP, lock_slot_fp_offset);
      // get address of the stack lock
      __ addi_d(lock_reg, FP, lock_slot_fp_offset);
      // Atomic swap old header if oop still contains the stack lock
      Label count;
      __ cmpxchg(Address(obj_reg, 0), lock_reg, T8, AT, false, true /* acquire */, count, &slow_path_unlock);
      __ bind(count);
      __ decrement(Address(TREG, JavaThread::held_monitor_count_offset()), 1);
    } else {
      assert(LockingMode == LM_LIGHTWEIGHT, "");
      __ ld_d(lock_reg, Address(obj_reg, oopDesc::mark_offset_in_bytes()));
      __ andi(AT, lock_reg, markWord::monitor_value);
      __ bnez(AT, slow_path_unlock);
      __ lightweight_unlock(obj_reg, lock_reg, swap_reg, SCR1, slow_path_unlock);
      __ decrement(Address(TREG, JavaThread::held_monitor_count_offset()));
    }

    // slow path re-enters here
    __ bind(unlock_done);
    if (ret_type != T_FLOAT && ret_type != T_DOUBLE && ret_type != T_VOID) {
      restore_native_result(masm, ret_type, stack_slots);
    }

    __ bind(done);
  }
  {
    SkipIfEqual skip_if(masm, &DTraceMethodProbes, 0);
    // Tell dtrace about this method exit
    save_native_result(masm, ret_type, stack_slots);
    int metadata_index = __ oop_recorder()->find_index( (method()));
    RelocationHolder rspec = metadata_Relocation::spec(metadata_index);
    __ relocate(rspec);
    __ patchable_li52(AT, (long)(method()));

    __ call_VM_leaf(
         CAST_FROM_FN_PTR(address, SharedRuntime::dtrace_method_exit),
         TREG, AT);
    restore_native_result(masm, ret_type, stack_slots);
  }

  // We can finally stop using that last_Java_frame we setup ages ago

  __ reset_last_Java_frame(false);

  // Unpack oop result, e.g. JNIHandles::resolve value.
  if (is_reference_type(ret_type)) {
    __ resolve_jobject(V0, SCR2, SCR1);
  }

  if (CheckJNICalls) {
    // clear_pending_jni_exception_check
    __ st_d(R0, TREG, in_bytes(JavaThread::pending_jni_exception_check_fn_offset()));
  }

  // reset handle block
  __ ld_d(AT, TREG, in_bytes(JavaThread::active_handles_offset()));
  __ st_w(R0, AT, in_bytes(JNIHandleBlock::top_offset()));

  __ leave();

  // Any exception pending?
  __ ld_d(AT, TREG, in_bytes(Thread::pending_exception_offset()));
  __ bne(AT, R0, exception_pending);

  // We're done
  __ jr(RA);

  // Unexpected paths are out of line and go here

  // forward the exception
  __ bind(exception_pending);

  __ jmp(StubRoutines::forward_exception_entry(), relocInfo::runtime_call_type);

  // Slow path locking & unlocking
  if (method->is_synchronized()) {

    // BEGIN Slow path lock
    __ bind(slow_path_lock);

    // protect the args we've loaded
    save_args(masm, total_c_args, c_arg, out_regs);

    // has last_Java_frame setup. No exceptions so do vanilla call not call_VM
    // args are (oop obj, BasicLock* lock, JavaThread* thread)

    __ move(A0, obj_reg);
    __ move(A1, lock_reg);
    __ move(A2, TREG);
    __ addi_d(SP, SP, - 3*wordSize);

    __ move(S2, SP);     // use S2 as a sender SP holder
    assert(StackAlignmentInBytes == 16, "must be");
    __ bstrins_d(SP, R0, 3, 0); // align stack as required by ABI

    __ call(CAST_FROM_FN_PTR(address, SharedRuntime::complete_monitor_locking_C), relocInfo::runtime_call_type);
    __ move(SP, S2);
    __ addi_d(SP, SP, 3*wordSize);

    restore_args(masm, total_c_args, c_arg, out_regs);

#ifdef ASSERT
    { Label L;
      __ ld_d(AT, TREG, in_bytes(Thread::pending_exception_offset()));
      __ beq(AT, R0, L);
      __ stop("no pending exception allowed on exit from monitorenter");
      __ bind(L);
    }
#endif
    __ b(lock_done);
    // END Slow path lock

    // BEGIN Slow path unlock
    __ bind(slow_path_unlock);

    // Slow path unlock

    if (ret_type == T_FLOAT || ret_type == T_DOUBLE ) {
      save_native_result(masm, ret_type, stack_slots);
    }
    // Save pending exception around call to VM (which contains an EXCEPTION_MARK)

    __ ld_d(AT, TREG, in_bytes(Thread::pending_exception_offset()));
    __ push(AT);
    __ st_d(R0, TREG, in_bytes(Thread::pending_exception_offset()));

    __ move(S2, SP);     // use S2 as a sender SP holder
    assert(StackAlignmentInBytes == 16, "must be");
    __ bstrins_d(SP, R0, 3, 0); // align stack as required by ABI

    // should be a peal
    // +wordSize because of the push above
    __ addi_d(A1, FP, lock_slot_fp_offset);

    __ move(A0, obj_reg);
    __ move(A2, TREG);
    __ addi_d(SP, SP, -2*wordSize);
    __ call(CAST_FROM_FN_PTR(address, SharedRuntime::complete_monitor_unlocking_C),
        relocInfo::runtime_call_type);
    __ addi_d(SP, SP, 2*wordSize);
    __ move(SP, S2);
#ifdef ASSERT
    {
      Label L;
      __ ld_d( AT, TREG, in_bytes(Thread::pending_exception_offset()));
      __ beq(AT, R0, L);
      __ stop("no pending exception allowed on exit complete_monitor_unlocking_C");
      __ bind(L);
    }
#endif /* ASSERT */

    __ pop(AT);
    __ st_d(AT, TREG, in_bytes(Thread::pending_exception_offset()));
    if (ret_type == T_FLOAT || ret_type == T_DOUBLE ) {
      restore_native_result(masm, ret_type, stack_slots);
    }
    __ b(unlock_done);
    // END Slow path unlock

  }

  // SLOW PATH Reguard the stack if needed

  __ bind(reguard);
  save_native_result(masm, ret_type, stack_slots);
  __ call(CAST_FROM_FN_PTR(address, SharedRuntime::reguard_yellow_pages),
      relocInfo::runtime_call_type);
  restore_native_result(masm, ret_type, stack_slots);
  __ b(reguard_done);

  __ flush();

  nmethod *nm = nmethod::new_native_nmethod(method,
                                            compile_id,
                                            masm->code(),
                                            vep_offset,
                                            frame_complete,
                                            stack_slots / VMRegImpl::slots_per_word,
                                            (is_static ? in_ByteSize(klass_offset) : in_ByteSize(receiver_offset)),
                                            in_ByteSize(lock_slot_offset*VMRegImpl::stack_slot_size),
                                            oop_maps);

  return nm;
}

// this function returns the adjust size (in number of words) to a c2i adapter
// activation for use during deoptimization
int Deoptimization::last_frame_adjust(int callee_parameters, int callee_locals) {
  return (callee_locals - callee_parameters) * Interpreter::stackElementWords;
}

// Number of stack slots between incoming argument block and the start of
// a new frame.  The PROLOG must add this many slots to the stack.  The
// EPILOG must remove this many slots. LA needs two slots for
// return address and fp.
// TODO think this is correct but check
uint SharedRuntime::in_preserve_stack_slots() {
  return 4;
}

// "Top of Stack" slots that may be unused by the calling convention but must
// otherwise be preserved.
// On Intel these are not necessary and the value can be zero.
// On Sparc this describes the words reserved for storing a register window
// when an interrupt occurs.
uint SharedRuntime::out_preserve_stack_slots() {
   return 0;
}

//------------------------------generate_deopt_blob----------------------------
// Ought to generate an ideal graph & compile, but here's some SPARC ASM
// instead.
void SharedRuntime::generate_deopt_blob() {
  // allocate space for the code
  ResourceMark rm;
  // setup code generation tools
  int pad = 0;
#if INCLUDE_JVMCI
  if (EnableJVMCI) {
    pad += 512; // Increase the buffer size when compiling for JVMCI
  }
#endif
  CodeBuffer     buffer ("deopt_blob", 2048+pad, 1024);
  MacroAssembler* masm  = new MacroAssembler( & buffer);
  int frame_size_in_words;
  OopMap* map = nullptr;
  // Account for the extra args we place on the stack
  // by the time we call fetch_unroll_info
  const int additional_words = 2; // deopt kind, thread

  OopMapSet *oop_maps = new OopMapSet();
  RegisterSaver reg_save(COMPILER2_OR_JVMCI != 0);

  address start = __ pc();
  Label cont;
  // we use S3 for DeOpt reason register
  Register reason = S3;
  // use S7 for fetch_unroll_info returned UnrollBlock
  Register unroll = S7;
  // Prolog for non exception case!

  // We have been called from the deopt handler of the deoptee.
  //
  // deoptee:
  //                      ...
  //                      call X
  //                      ...
  //  deopt_handler:      call_deopt_stub
  //  cur. return pc  --> ...
  //
  // So currently RA points behind the call in the deopt handler.
  // We adjust it such that it points to the start of the deopt handler.
  // The return_pc has been stored in the frame of the deoptee and
  // will replace the address of the deopt_handler in the call
  // to Deoptimization::fetch_unroll_info below.

  // HandlerImpl::size_deopt_handler()
  __ addi_d(RA, RA, - NativeFarCall::instruction_size);
  // Save everything in sight.
  map = reg_save.save_live_registers(masm, additional_words, &frame_size_in_words);
  // Normal deoptimization
  __ li(reason, Deoptimization::Unpack_deopt);
  __ b(cont);

  int reexecute_offset = __ pc() - start;
#if INCLUDE_JVMCI && !defined(COMPILER1)
  if (EnableJVMCI && UseJVMCICompiler) {
    // JVMCI does not use this kind of deoptimization
    __ should_not_reach_here();
  }
#endif

  // Reexecute case
  // return address is the pc describes what bci to do re-execute at

  // No need to update map as each call to save_live_registers will produce identical oopmap
  (void) reg_save.save_live_registers(masm, additional_words, &frame_size_in_words);
  __ li(reason, Deoptimization::Unpack_reexecute);
  __ b(cont);

#if INCLUDE_JVMCI
  Label after_fetch_unroll_info_call;
  int implicit_exception_uncommon_trap_offset = 0;
  int uncommon_trap_offset = 0;

  if (EnableJVMCI) {
    implicit_exception_uncommon_trap_offset = __ pc() - start;

    __ ld_d(RA, Address(TREG, in_bytes(JavaThread::jvmci_implicit_exception_pc_offset())));
    __ st_d(R0, Address(TREG, in_bytes(JavaThread::jvmci_implicit_exception_pc_offset())));

    uncommon_trap_offset = __ pc() - start;

    // Save everything in sight.
    (void) reg_save.save_live_registers(masm, additional_words, &frame_size_in_words);
    __ addi_d(SP, SP, -additional_words * wordSize);
    // fetch_unroll_info needs to call last_java_frame()
    Label retaddr;
    __ set_last_Java_frame(NOREG, NOREG, retaddr);

    __ ld_w(c_rarg1, Address(TREG, in_bytes(JavaThread::pending_deoptimization_offset())));
    __ li(AT, -1);
    __ st_w(AT, Address(TREG, in_bytes(JavaThread::pending_deoptimization_offset())));

    __ li(reason, (int32_t)Deoptimization::Unpack_reexecute);
    __ move(c_rarg0, TREG);
    __ move(c_rarg2, reason); // exec mode
    __ call((address)Deoptimization::uncommon_trap, relocInfo::runtime_call_type);
    __ bind(retaddr);
    oop_maps->add_gc_map( __ pc()-start, map->deep_copy());
    __ addi_d(SP, SP, additional_words * wordSize);

    __ reset_last_Java_frame(false);

    __ b(after_fetch_unroll_info_call);
  } // EnableJVMCI
#endif // INCLUDE_JVMCI

  int   exception_offset = __ pc() - start;
  // Prolog for exception case

  // all registers are dead at this entry point, except for V0 and
  // V1 which contain the exception oop and exception pc
  // respectively.  Set them in TLS and fall thru to the
  // unpack_with_exception_in_tls entry point.

  __ st_d(V1, Address(TREG, JavaThread::exception_pc_offset()));
  __ st_d(V0, Address(TREG, JavaThread::exception_oop_offset()));
  int exception_in_tls_offset = __ pc() - start;
  // new implementation because exception oop is now passed in JavaThread

  // Prolog for exception case
  // All registers must be preserved because they might be used by LinearScan
  // Exceptiop oop and throwing PC are passed in JavaThread
  // tos: stack at point of call to method that threw the exception (i.e. only
  // args are on the stack, no return address)

  // Return address will be patched later with the throwing pc. The correct value is not
  // available now because loading it from memory would destroy registers.
  // Save everything in sight.
  // No need to update map as each call to save_live_registers will produce identical oopmap
  (void) reg_save.save_live_registers(masm, additional_words, &frame_size_in_words);

  // Now it is safe to overwrite any register
  // store the correct deoptimization type
  __ li(reason, Deoptimization::Unpack_exception);
  // load throwing pc from JavaThread and patch it as the return address
  // of the current frame. Then clear the field in JavaThread

  __ ld_d(V1, Address(TREG, JavaThread::exception_pc_offset()));
  __ st_d(V1, SP, reg_save.ra_offset()); //save ra
  __ st_d(R0, Address(TREG, JavaThread::exception_pc_offset()));

#ifdef ASSERT
  // verify that there is really an exception oop in JavaThread
  __ ld_d(AT, TREG, in_bytes(JavaThread::exception_oop_offset()));
  __ verify_oop(AT);
  // verify that there is no pending exception
  Label no_pending_exception;
  __ ld_d(AT, TREG, in_bytes(Thread::pending_exception_offset()));
  __ beq(AT, R0, no_pending_exception);
  __ stop("must not have pending exception here");
  __ bind(no_pending_exception);
#endif
  __ bind(cont);

  // Call C code.  Need thread and this frame, but NOT official VM entry
  // crud.  We cannot block on this call, no GC can happen.

  __ move(c_rarg0, TREG);
  __ move(c_rarg1, reason); // exec_mode
  __ addi_d(SP, SP, -additional_words * wordSize);

  Label retaddr;
  __ set_last_Java_frame(NOREG, NOREG, retaddr);

  // Call fetch_unroll_info().  Need thread and this frame, but NOT official VM entry - cannot block on
  // this call, no GC can happen.  Call should capture return values.

  // TODO: confirm reloc
  __ call(CAST_FROM_FN_PTR(address, Deoptimization::fetch_unroll_info), relocInfo::runtime_call_type);
  __ bind(retaddr);
  oop_maps->add_gc_map(__ pc() - start, map);
  __ addi_d(SP, SP, additional_words * wordSize);

  __ reset_last_Java_frame(false);

#if INCLUDE_JVMCI
  if (EnableJVMCI) {
    __ bind(after_fetch_unroll_info_call);
  }
#endif

  // Load UnrollBlock into S7
  __ move(unroll, V0);


  // Move the unpack kind to a safe place in the UnrollBlock because
  // we are very short of registers

  Address unpack_kind(unroll, Deoptimization::UnrollBlock::unpack_kind_offset());
  __ ld_w(reason, unpack_kind);

  Label noException;
  __ li(AT, Deoptimization::Unpack_exception);
  __ bne(AT, reason, noException);// Was exception pending?
  __ ld_d(V0, Address(TREG, JavaThread::exception_oop_offset()));
  __ ld_d(V1, Address(TREG, JavaThread::exception_pc_offset()));
  __ st_d(R0, Address(TREG, JavaThread::exception_pc_offset()));
  __ st_d(R0, Address(TREG, JavaThread::exception_oop_offset()));

  __ verify_oop(V0);

  // Overwrite the result registers with the exception results.
  __ st_d(V0, SP, reg_save.v0_offset());
  __ st_d(V1, SP, reg_save.v1_offset());

  __ bind(noException);


  // Stack is back to only having register save data on the stack.
  // Now restore the result registers. Everything else is either dead or captured
  // in the vframeArray.

  reg_save.restore_result_registers(masm);
  // All of the register save area has been popped of the stack. Only the
  // return address remains.
  // Pop all the frames we must move/replace.
  // Frame picture (youngest to oldest)
  // 1: self-frame (no frame link)
  // 2: deopting frame  (no frame link)
  // 3: caller of deopting frame (could be compiled/interpreted).
  //
  // Note: by leaving the return address of self-frame on the stack
  // and using the size of frame 2 to adjust the stack
  // when we are done the return to frame 3 will still be on the stack.

  // register for the sender's sp
  Register sender_sp = Rsender;
  // register for frame pcs
  Register pcs = T0;
  // register for frame sizes
  Register sizes = T1;
  // register for frame count
  Register count = T3;

  // Pop deoptimized frame
  __ ld_w(T8, Address(unroll, Deoptimization::UnrollBlock::size_of_deoptimized_frame_offset()));
  __ add_d(SP, SP, T8);
  // sp should be pointing at the return address to the caller (3)

  // Load array of frame pcs into pcs
  __ ld_d(pcs, Address(unroll, Deoptimization::UnrollBlock::frame_pcs_offset()));
  __ addi_d(SP, SP, wordSize);  // trash the old pc
  // Load array of frame sizes into T6
  __ ld_d(sizes, Address(unroll, Deoptimization::UnrollBlock::frame_sizes_offset()));

#ifdef ASSERT
  // Compilers generate code that bang the stack by as much as the
  // interpreter would need. So this stack banging should never
  // trigger a fault. Verify that it does not on non product builds.
  __ ld_w(TSR, Address(unroll, Deoptimization::UnrollBlock::total_frame_sizes_offset()));
  __ bang_stack_size(TSR, T8);
#endif

  // Load count of frams into T3
  __ ld_w(count, Address(unroll, Deoptimization::UnrollBlock::number_of_frames_offset()));
  // Pick up the initial fp we should save
  __ ld_d(FP, Address(unroll, Deoptimization::UnrollBlock::initial_info_offset()));
   // Now adjust the caller's stack to make up for the extra locals
  // but record the original sp so that we can save it in the skeletal interpreter
  // frame and the stack walking of interpreter_sender will get the unextended sp
  // value and not the "real" sp value.
  __ move(sender_sp, SP);
  __ ld_w(AT, Address(unroll, Deoptimization::UnrollBlock::caller_adjustment_offset()));
  __ sub_d(SP, SP, AT);

  Label loop;
  __ bind(loop);
  __ ld_d(T2, sizes, 0);    // Load frame size
  __ ld_d(AT, pcs, 0);           // save return address
  __ addi_d(T2, T2, -2 * wordSize);           // we'll push pc and fp, by hand
  __ push2(AT, FP);
  __ addi_d(FP, SP, 2 * wordSize);
  __ sub_d(SP, SP, T2);       // Prolog!
  // This value is corrected by layout_activation_impl
  __ st_d(R0, FP, frame::interpreter_frame_last_sp_offset * wordSize);
  __ st_d(sender_sp, FP, frame::interpreter_frame_sender_sp_offset * wordSize);// Make it walkable
  __ move(sender_sp, SP);  // pass to next frame
  __ addi_d(count, count, -1);   // decrement counter
  __ addi_d(sizes, sizes, wordSize);   // Bump array pointer (sizes)
  __ addi_d(pcs, pcs, wordSize);   // Bump array pointer (pcs)
  __ bne(count, R0, loop);

  // Re-push self-frame
  __ ld_d(AT, pcs, 0);      // frame_pcs[number_of_frames] = Interpreter::deopt_entry(vtos, 0);
  __ push2(AT, FP);
  __ addi_d(FP, SP, 2 * wordSize);
  __ addi_d(SP, SP, -(frame_size_in_words - 2 - additional_words) * wordSize);

  // Restore frame locals after moving the frame
  __ st_d(V0, SP, reg_save.v0_offset());
  __ st_d(V1, SP, reg_save.v1_offset());
  __ fst_d(F0, SP, reg_save.fpr0_offset());
  __ fst_d(F1, SP, reg_save.fpr1_offset());

  // Call unpack_frames().  Need thread and this frame, but NOT official VM entry - cannot block on
  // this call, no GC can happen.
  __ move(A1, reason);  // exec_mode
  __ move(A0, TREG);  // thread
  __ addi_d(SP, SP, (-additional_words) *wordSize);

  // set last_Java_sp, last_Java_fp
  Label L;
  address the_pc = __ pc();
  __ bind(L);
  __ set_last_Java_frame(NOREG, FP, L);

  assert(StackAlignmentInBytes == 16, "must be");
  __ bstrins_d(SP, R0, 3, 0);   // Fix stack alignment as required by ABI

  __ call(CAST_FROM_FN_PTR(address, Deoptimization::unpack_frames), relocInfo::runtime_call_type);
  // Revert SP alignment after call since we're going to do some SP relative addressing below
  __ ld_d(SP, TREG, in_bytes(JavaThread::last_Java_sp_offset()));
  // Set an oopmap for the call site
  oop_maps->add_gc_map(the_pc - start, new OopMap(frame_size_in_words, 0));

  __ push(V0);

  __ reset_last_Java_frame(true);

  // Collect return values
  __ ld_d(V0, SP, reg_save.v0_offset() + (additional_words + 1) * wordSize);
  __ ld_d(V1, SP, reg_save.v1_offset() + (additional_words + 1) * wordSize);
  // Pop float stack and store in local
  __ fld_d(F0, SP, reg_save.fpr0_offset() + (additional_words + 1) * wordSize);
  __ fld_d(F1, SP, reg_save.fpr1_offset() + (additional_words + 1) * wordSize);

  // Push a float or double return value if necessary.
  __ leave();

  // Jump to interpreter
  __ jr(RA);

  masm->flush();
  _deopt_blob = DeoptimizationBlob::create(&buffer, oop_maps, 0, exception_offset, reexecute_offset, frame_size_in_words);
  _deopt_blob->set_unpack_with_exception_in_tls_offset(exception_in_tls_offset);
#if INCLUDE_JVMCI
  if (EnableJVMCI) {
    _deopt_blob->set_uncommon_trap_offset(uncommon_trap_offset);
    _deopt_blob->set_implicit_exception_uncommon_trap_offset(implicit_exception_uncommon_trap_offset);
  }
#endif
}

#ifdef COMPILER2

//------------------------------generate_uncommon_trap_blob--------------------
// Ought to generate an ideal graph & compile, but here's some SPARC ASM
// instead.
void SharedRuntime::generate_uncommon_trap_blob() {
  // allocate space for the code
  ResourceMark rm;
  // setup code generation tools
  CodeBuffer  buffer ("uncommon_trap_blob", 512*80 , 512*40 );
  MacroAssembler* masm = new MacroAssembler(&buffer);

  enum frame_layout {
    fp_off, fp_off2,
    return_off, return_off2,
    framesize
  };
  assert(framesize % 4 == 0, "sp not 16-byte aligned");
  address start = __ pc();

  // Push self-frame.
  __ addi_d(SP, SP, -framesize * BytesPerInt);

  __ st_d(RA, SP, return_off * BytesPerInt);
  __ st_d(FP, SP, fp_off * BytesPerInt);

  __ addi_d(FP, SP, framesize * BytesPerInt);

  // set last_Java_sp
  Label retaddr;
  __ set_last_Java_frame(NOREG, FP, retaddr);
  // Call C code.  Need thread but NOT official VM entry
  // crud.  We cannot block on this call, no GC can happen.  Call should
  // capture callee-saved registers as well as return values.
  __ move(A0, TREG);
  // argument already in T0
  __ move(A1, T0);
  __ addi_d(A2, R0, Deoptimization::Unpack_uncommon_trap);
  __ call((address)Deoptimization::uncommon_trap, relocInfo::runtime_call_type);
  __ bind(retaddr);

  // Set an oopmap for the call site
  OopMapSet *oop_maps = new OopMapSet();
  OopMap* map =  new OopMap( framesize, 0 );

  oop_maps->add_gc_map(__ pc() - start, map);

  __ reset_last_Java_frame(false);

  // Load UnrollBlock into S7
  Register unroll = S7;
  __ move(unroll, V0);

#ifdef ASSERT
  { Label L;
    __ ld_d(AT, Address(unroll, Deoptimization::UnrollBlock::unpack_kind_offset()));
    __ li(T4, Deoptimization::Unpack_uncommon_trap);
    __ beq(AT, T4, L);
    __ stop("SharedRuntime::generate_uncommon_trap_blob: expected Unpack_uncommon_trap");
    __ bind(L);
  }
#endif

  // Pop all the frames we must move/replace.
  //
  // Frame picture (youngest to oldest)
  // 1: self-frame (no frame link)
  // 2: deopting frame  (no frame link)
  // 3: possible-i2c-adapter-frame
  // 4: caller of deopting frame (could be compiled/interpreted. If interpreted we will create an
  //    and c2i here)

  __ addi_d(SP, SP, framesize * BytesPerInt);

  // Pop deoptimized frame
  __ ld_w(T8, Address(unroll, Deoptimization::UnrollBlock::size_of_deoptimized_frame_offset()));
  __ add_d(SP, SP, T8);

#ifdef ASSERT
  // Compilers generate code that bang the stack by as much as the
  // interpreter would need. So this stack banging should never
  // trigger a fault. Verify that it does not on non product builds.
  __ ld_w(TSR, Address(unroll, Deoptimization::UnrollBlock::total_frame_sizes_offset()));
  __ bang_stack_size(TSR, T8);
#endif

  // register for frame pcs
  Register pcs = T8;
  // register for frame sizes
  Register sizes = T4;
  // register for frame count
  Register count = T3;
  // register for the sender's sp
  Register sender_sp = T1;

  // sp should be pointing at the return address to the caller (4)
  // Load array of frame pcs
  __ ld_d(pcs, Address(unroll, Deoptimization::UnrollBlock::frame_pcs_offset()));

  // Load array of frame sizes
  __ ld_d(sizes, Address(unroll, Deoptimization::UnrollBlock::frame_sizes_offset()));
  __ ld_wu(count, Address(unroll, Deoptimization::UnrollBlock::number_of_frames_offset()));

  // Pick up the initial fp we should save
  __ ld_d(FP, Address(unroll, Deoptimization::UnrollBlock::initial_info_offset()));

  // Now adjust the caller's stack to make up for the extra locals
  // but record the original sp so that we can save it in the skeletal interpreter
  // frame and the stack walking of interpreter_sender will get the unextended sp
  // value and not the "real" sp value.
  __ move(sender_sp, SP);
  __ ld_w(AT, Address(unroll, Deoptimization::UnrollBlock::caller_adjustment_offset()));
  __ sub_d(SP, SP, AT);

  // Push interpreter frames in a loop
  Label loop;
  __ bind(loop);
  __ ld_d(T2, sizes, 0);          // Load frame size
  __ ld_d(RA, pcs, 0);           // save return address
  __ addi_d(T2, T2, -2*wordSize);           // we'll push pc and fp, by hand
  __ enter();
  __ sub_d(SP, SP, T2);                   // Prolog!
  // This value is corrected by layout_activation_impl
  __ st_d(R0, FP, frame::interpreter_frame_last_sp_offset * wordSize);
  __ st_d(sender_sp, FP, frame::interpreter_frame_sender_sp_offset * wordSize);// Make it walkable
  __ move(sender_sp, SP);       // pass to next frame
  __ addi_d(count, count, -1);    // decrement counter
  __ addi_d(sizes, sizes, wordSize);     // Bump array pointer (sizes)
  __ addi_d(pcs, pcs, wordSize);      // Bump array pointer (pcs)
  __ bne(count, R0, loop);

  __ ld_d(RA, pcs, 0);

  // Re-push self-frame
  // save old & set new FP
  // save final return address
  __ enter();

  // Use FP because the frames look interpreted now
  // Save "the_pc" since it cannot easily be retrieved using the last_java_SP after we aligned SP.
  // Don't need the precise return PC here, just precise enough to point into this code blob.
  Label L;
  address the_pc = __ pc();
  __ bind(L);
  __ set_last_Java_frame(NOREG, FP, L);

  assert(StackAlignmentInBytes == 16, "must be");
  __ bstrins_d(SP, R0, 3, 0);   // Fix stack alignment as required by ABI

  // Call C code.  Need thread but NOT official VM entry
  // crud.  We cannot block on this call, no GC can happen.  Call should
  // restore return values to their stack-slots with the new SP.
  __ move(A0, TREG);
  __ li(A1, Deoptimization::Unpack_uncommon_trap);
  __ call((address)Deoptimization::unpack_frames, relocInfo::runtime_call_type);
  // Set an oopmap for the call site
  oop_maps->add_gc_map(the_pc - start, new OopMap(framesize, 0));

  __ reset_last_Java_frame(true);

  // Pop self-frame.
  __ leave();     // Epilog!

  // Jump to interpreter
  __ jr(RA);
  // -------------
  // make sure all code is generated
  masm->flush();
  _uncommon_trap_blob = UncommonTrapBlob::create(&buffer, oop_maps, framesize / 2);
}

#endif // COMPILER2

//------------------------------generate_handler_blob-------------------
//
// Generate a special Compile2Runtime blob that saves all registers, and sets
// up an OopMap and calls safepoint code to stop the compiled code for
// a safepoint.
//
// This blob is jumped to (via a breakpoint and the signal handler) from a
// safepoint in compiled code.

SafepointBlob* SharedRuntime::generate_handler_blob(address call_ptr, int poll_type) {

  // Account for thread arg in our frame
  const int additional_words = 0;
  int frame_size_in_words;

  assert (StubRoutines::forward_exception_entry() != nullptr, "must be generated before");

  ResourceMark rm;
  OopMapSet *oop_maps = new OopMapSet();
  OopMap* map;

  // allocate space for the code
  // setup code generation tools
  CodeBuffer  buffer ("handler_blob", 2048, 512);
  MacroAssembler* masm = new MacroAssembler( &buffer);

  address start   = __ pc();
  bool cause_return = (poll_type == POLL_AT_RETURN);
  RegisterSaver reg_save(poll_type == POLL_AT_VECTOR_LOOP /* save_vectors */);

  map = reg_save.save_live_registers(masm, additional_words, &frame_size_in_words);

  // The following is basically a call_VM. However, we need the precise
  // address of the call in order to generate an oopmap. Hence, we do all the
  // work outselvs.

  Label retaddr;
  __ set_last_Java_frame(NOREG, NOREG, retaddr);

  if (!cause_return) {
    // overwrite the return address pushed by save_live_registers
    // Additionally, TSR is a callee-saved register so we can look at
    // it later to determine if someone changed the return address for
    // us!
    __ ld_d(TSR, Address(TREG, JavaThread::saved_exception_pc_offset()));
    __ st_d(TSR, SP, reg_save.ra_offset());
  }

  // Do the call
  __ move(A0, TREG);
  // TODO: confirm reloc
  __ call(call_ptr, relocInfo::runtime_call_type);
  __ bind(retaddr);

  // Set an oopmap for the call site.  This oopmap will map all
  // oop-registers and debug-info registers as callee-saved.  This
  // will allow deoptimization at this safepoint to find all possible
  // debug-info recordings, as well as let GC find all oops.
  oop_maps->add_gc_map(__ pc() - start, map);

  Label noException;

  // Clear last_Java_sp again
  __ reset_last_Java_frame(false);

  __ ld_d(AT, Address(TREG, Thread::pending_exception_offset()));
  __ beq(AT, R0, noException);

  // Exception pending

  reg_save.restore_live_registers(masm);

  __ jmp(StubRoutines::forward_exception_entry(), relocInfo::runtime_call_type);

  // No exception case
  __ bind(noException);

  Label no_adjust, bail;
  if (!cause_return) {
    // If our stashed return pc was modified by the runtime we avoid touching it
    __ ld_d(AT, SP, reg_save.ra_offset());
    __ bne(AT, TSR, no_adjust);

#ifdef ASSERT
    // Verify the correct encoding of the poll we're about to skip.
    // See NativeInstruction::is_safepoint_poll()
    __ ld_wu(AT, TSR, 0);
    __ push(T5);
    __ li(T5, 0xffc0001f);
    __ andr(AT, AT, T5);
    __ li(T5, 0x28800013);
    __ xorr(AT, AT, T5);
    __ pop(T5);
    __ bne(AT, R0, bail);
#endif
    // Adjust return pc forward to step over the safepoint poll instruction
     __ addi_d(RA, TSR, 4);    // NativeInstruction::instruction_size=4
     __ st_d(RA, SP, reg_save.ra_offset());
  }

  __ bind(no_adjust);
  // Normal exit, register restoring and exit
  reg_save.restore_live_registers(masm);
  __ jr(RA);

#ifdef ASSERT
  __ bind(bail);
  __ stop("Attempting to adjust pc to skip safepoint poll but the return point is not what we expected");
#endif

  // Make sure all code is generated
  masm->flush();
  // Fill-out other meta info
  return SafepointBlob::create(&buffer, oop_maps, frame_size_in_words);
}

//
// generate_resolve_blob - call resolution (static/virtual/opt-virtual/ic-miss
//
// Generate a stub that calls into vm to find out the proper destination
// of a java call. All the argument registers are live at this point
// but since this is generic code we don't know what they are and the caller
// must do any gc of the args.
//
RuntimeStub* SharedRuntime::generate_resolve_blob(address destination, const char* name) {
  assert (StubRoutines::forward_exception_entry() != nullptr, "must be generated before");

  // allocate space for the code
  ResourceMark rm;

  CodeBuffer buffer(name, 1000, 512);
  MacroAssembler* masm  = new MacroAssembler(&buffer);

  int frame_size_words;
  RegisterSaver reg_save(false /* save_vectors */);
  //we put the thread in A0

  OopMapSet *oop_maps = new OopMapSet();
  OopMap* map = nullptr;

  address start = __ pc();
  map = reg_save.save_live_registers(masm, 0, &frame_size_words);

  int frame_complete = __ offset();

  __ move(A0, TREG);
  Label retaddr;
  __ set_last_Java_frame(noreg, FP, retaddr);
  // align the stack before invoke native
  assert(StackAlignmentInBytes == 16, "must be");
  __ bstrins_d(SP, R0, 3, 0);

  // TODO: confirm reloc
  __ call(destination, relocInfo::runtime_call_type);
  __ bind(retaddr);

  // Set an oopmap for the call site.
  // We need this not only for callee-saved registers, but also for volatile
  // registers that the compiler might be keeping live across a safepoint.
  oop_maps->add_gc_map(__ pc() - start, map);
  // V0 contains the address we are going to jump to assuming no exception got installed
  __ ld_d(SP, Address(TREG, JavaThread::last_Java_sp_offset()));
  // clear last_Java_sp
  __ reset_last_Java_frame(true);
  // check for pending exceptions
  Label pending;
  __ ld_d(AT, Address(TREG, Thread::pending_exception_offset()));
  __ bne(AT, R0, pending);
  // get the returned Method*
  __ get_vm_result_2(Rmethod, TREG);
  __ st_d(Rmethod, SP, reg_save.s3_offset());
  __ st_d(V0, SP, reg_save.t5_offset());
  reg_save.restore_live_registers(masm);

  // We are back the original state on entry and ready to go the callee method.
  __ jr(T5);
  // Pending exception after the safepoint

  __ bind(pending);

  reg_save.restore_live_registers(masm);

  // exception pending => remove activation and forward to exception handler

  __ st_d(R0, Address(TREG, JavaThread::vm_result_offset()));
  __ ld_d(V0, Address(TREG, Thread::pending_exception_offset()));
  __ jmp(StubRoutines::forward_exception_entry(), relocInfo::runtime_call_type);
  //
  // make sure all code is generated
  masm->flush();
  RuntimeStub* tmp= RuntimeStub::new_runtime_stub(name, &buffer, frame_complete, frame_size_words, oop_maps, true);
  return tmp;
}

#ifdef COMPILER2
//-------------- generate_exception_blob -----------
// creates exception blob at the end
// Using exception blob, this code is jumped from a compiled method.
// (see emit_exception_handler in loongarch.ad file)
//
// Given an exception pc at a call we call into the runtime for the
// handler in this method. This handler might merely restore state
// (i.e. callee save registers) unwind the frame and jump to the
// exception handler for the nmethod if there is no Java level handler
// for the nmethod.
//
// This code is entered with a jump, and left with a jump.
//
// Arguments:
//   A0: exception oop
//   A1: exception pc
//
// Results:
//   A0: exception oop
//   A1: exception pc in caller
//   destination: exception handler of caller
//
// Note: the exception pc MUST be at a call (precise debug information)
//
//  [stubGenerator_loongarch_64.cpp] generate_forward_exception()
//      |- A0, A1 are created
//      |- T4 <= SharedRuntime::exception_handler_for_return_address
//      `- jr T4
//           `- the caller's exception_handler
//                 `- jr OptoRuntime::exception_blob
//                        `- here
//
void OptoRuntime::generate_exception_blob() {
  enum frame_layout {
    fp_off, fp_off2,
    return_off, return_off2,
    framesize
  };
  assert(framesize % 4 == 0, "sp not 16-byte aligned");

  // Allocate space for the code
  ResourceMark rm;
  // Setup code generation tools
  CodeBuffer buffer("exception_blob", 2048, 1024);
  MacroAssembler* masm = new MacroAssembler(&buffer);

  address start = __ pc();

  // Exception pc is 'return address' for stack walker
  __ push2(A1 /* return address */, FP);

  // there are no callee save registers and we don't expect an
  // arg reg save area
#ifndef PRODUCT
  assert(frame::arg_reg_save_area_bytes == 0, "not expecting frame reg save area");
#endif
  // Store exception in Thread object. We cannot pass any arguments to the
  // handle_exception call, since we do not want to make any assumption
  // about the size of the frame where the exception happened in.
  __ st_d(A0, Address(TREG, JavaThread::exception_oop_offset()));
  __ st_d(A1, Address(TREG, JavaThread::exception_pc_offset()));

  // This call does all the hard work.  It checks if an exception handler
  // exists in the method.
  // If so, it returns the handler address.
  // If not, it prepares for stack-unwinding, restoring the callee-save
  // registers of the frame being removed.
  //
  // address OptoRuntime::handle_exception_C(JavaThread* thread)
  //
  Label L;
  address the_pc = __ pc();
  __ bind(L);
  __ set_last_Java_frame(TREG, SP, NOREG, L);

  assert(StackAlignmentInBytes == 16, "must be");
  __ bstrins_d(SP, R0, 3, 0);   // Fix stack alignment as required by ABI

  __ move(A0, TREG);
  __ call(CAST_FROM_FN_PTR(address, OptoRuntime::handle_exception_C),
          relocInfo::runtime_call_type);

  // handle_exception_C is a special VM call which does not require an explicit
  // instruction sync afterwards.

  // Set an oopmap for the call site.  This oopmap will only be used if we
  // are unwinding the stack.  Hence, all locations will be dead.
  // Callee-saved registers will be the same as the frame above (i.e.,
  // handle_exception_stub), since they were restored when we got the
  // exception.

  OopMapSet *oop_maps = new OopMapSet();

  oop_maps->add_gc_map(the_pc - start, new OopMap(framesize, 0));

  __ reset_last_Java_frame(TREG, false);

  // Restore callee-saved registers

  // FP is an implicitly saved callee saved register (i.e. the calling
  // convention will save restore it in prolog/epilog) Other than that
  // there are no callee save registers now that adapter frames are gone.
  // and we dont' expect an arg reg save area
  __ pop2(RA, FP);

  const Register exception_handler = T4;

  // We have a handler in A0, (could be deopt blob)
  __ move(exception_handler, A0);

  // Get the exception
  __ ld_d(A0, Address(TREG, JavaThread::exception_oop_offset()));
  // Get the exception pc in case we are deoptimized
  __ ld_d(A1, Address(TREG, JavaThread::exception_pc_offset()));
#ifdef ASSERT
  __ st_d(R0, Address(TREG, JavaThread::exception_handler_pc_offset()));
  __ st_d(R0, Address(TREG, JavaThread::exception_pc_offset()));
#endif
  // Clear the exception oop so GC no longer processes it as a root.
  __ st_d(R0, Address(TREG, JavaThread::exception_oop_offset()));

  // A0: exception oop
  // A1: exception pc
  __ jr(exception_handler);

  // make sure all code is generated
  masm->flush();

  // Set exception blob
  _exception_blob = ExceptionBlob::create(&buffer, oop_maps, framesize >> 1);
}
#endif // COMPILER2

extern "C" int SpinPause() {return 0;}
