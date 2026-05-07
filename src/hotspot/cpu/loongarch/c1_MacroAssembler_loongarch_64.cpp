/*
 * Copyright (c) 1999, 2021, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2021, 2023, Loongson Technology. All rights reserved.
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
#include "c1/c1_MacroAssembler.hpp"
#include "c1/c1_Runtime1.hpp"
#include "gc/shared/barrierSetAssembler.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "gc/shared/tlab_globals.hpp"
#include "interpreter/interpreter.hpp"
#include "oops/arrayOop.hpp"
#include "oops/markWord.hpp"
#include "runtime/basicLock.hpp"
#include "runtime/os.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubRoutines.hpp"

int C1_MacroAssembler::lock_object(Register hdr, Register obj, Register disp_hdr, Label& slow_case) {
  const int aligned_mask = BytesPerWord -1;
  const int hdr_offset = oopDesc::mark_offset_in_bytes();
  assert_different_registers(hdr, obj, disp_hdr);
  int null_check_offset = -1;

  verify_oop(obj);

  // save object being locked into the BasicObjectLock
  st_d(obj, Address(disp_hdr, BasicObjectLock::obj_offset()));

  null_check_offset = offset();

  if (DiagnoseSyncOnValueBasedClasses != 0) {
    load_klass(hdr, obj);
    ld_w(hdr, Address(hdr, Klass::access_flags_offset()));
    li(SCR1, JVM_ACC_IS_VALUE_BASED_CLASS);
    andr(SCR1, hdr, SCR1);
    bnez(SCR1, slow_case);
  }

  // Load object header
  ld_d(hdr, Address(obj, hdr_offset));
  if (LockingMode == LM_LIGHTWEIGHT) {
    lightweight_lock(obj, hdr, SCR1, SCR2, slow_case);
  } else if (LockingMode == LM_LEGACY) {
    Label done;
    // and mark it as unlocked
    ori(hdr, hdr, markWord::unlocked_value);
    // save unlocked object header into the displaced header location on the stack
    st_d(hdr, Address(disp_hdr, 0));
    // test if object header is still the same (i.e. unlocked), and if so, store the
    // displaced header address in the object header - if it is not the same, get the
    // object header instead
    lea(SCR2, Address(obj, hdr_offset));
    cmpxchg(Address(SCR2, 0), hdr, disp_hdr, SCR1, true, true /* acquire */, done);
    // if the object header was the same, we're done
    // if the object header was not the same, it is now in the hdr register
    // => test if it is a stack pointer into the same stack (recursive locking), i.e.:
    //
    // 1) (hdr & aligned_mask) == 0
    // 2) sp <= hdr
    // 3) hdr <= sp + page_size
    //
    // these 3 tests can be done by evaluating the following expression:
    //
    // (hdr - sp) & (aligned_mask - page_size)
    //
    // assuming both the stack pointer and page_size have their least
    // significant 2 bits cleared and page_size is a power of 2
    sub_d(hdr, hdr, SP);
    li(SCR1, aligned_mask - os::vm_page_size());
    andr(hdr, hdr, SCR1);
    // for recursive locking, the result is zero => save it in the displaced header
    // location (null in the displaced hdr location indicates recursive locking)
    st_d(hdr, Address(disp_hdr, 0));
    // otherwise we don't care about the result and handle locking via runtime call
    bnez(hdr, slow_case);
    // done
    bind(done);
  }
  increment(Address(TREG, JavaThread::held_monitor_count_offset()), 1);
  return null_check_offset;
}

void C1_MacroAssembler::unlock_object(Register hdr, Register obj, Register disp_hdr, Label& slow_case) {
  const int aligned_mask = BytesPerWord -1;
  const int hdr_offset = oopDesc::mark_offset_in_bytes();
  assert(hdr != obj && hdr != disp_hdr && obj != disp_hdr, "registers must be different");
  Label done;

  if (LockingMode != LM_LIGHTWEIGHT) {
    // load displaced header
    ld_d(hdr, Address(disp_hdr, 0));
    // if the loaded hdr is null we had recursive locking
    // if we had recursive locking, we are done
    beqz(hdr, done);
  }

  // load object
  ld_d(obj, Address(disp_hdr, BasicObjectLock::obj_offset()));
  verify_oop(obj);
  if (LockingMode == LM_LIGHTWEIGHT) {
    ld_d(hdr, Address(obj, oopDesc::mark_offset_in_bytes()));
    // We cannot use tbnz here, the target might be too far away and cannot
    // be encoded.
    andi(AT, hdr, markWord::monitor_value);
    bnez(AT, slow_case);
    lightweight_unlock(obj, hdr, SCR1, SCR2, slow_case);
  } else if (LockingMode == LM_LEGACY) {
    // test if object header is pointing to the displaced header, and if so, restore
    // the displaced header in the object - if the object header is not pointing to
    // the displaced header, get the object header instead
    // if the object header was not pointing to the displaced header,
    // we do unlocking via runtime call
    if (hdr_offset) {
      lea(SCR1, Address(obj, hdr_offset));
      cmpxchg(Address(SCR1, 0), disp_hdr, hdr, SCR2, false, true /* acquire */, done, &slow_case);
    } else {
      cmpxchg(Address(obj, 0), disp_hdr, hdr, SCR2, false, true /* acquire */, done, &slow_case);
    }
    // done
    bind(done);
  }
  decrement(Address(TREG, JavaThread::held_monitor_count_offset()), 1);
}

// Defines obj, preserves var_size_in_bytes
void C1_MacroAssembler::try_allocate(Register obj, Register var_size_in_bytes,
                                     int con_size_in_bytes, Register t1, Register t2,
                                     Label& slow_case) {
  if (UseTLAB) {
    tlab_allocate(obj, var_size_in_bytes, con_size_in_bytes, t1, t2, slow_case);
  } else {
    b_far(slow_case);
  }
}

void C1_MacroAssembler::initialize_header(Register obj, Register klass, Register len,
                                          Register t1, Register t2) {
  assert_different_registers(obj, klass, len);
  // This assumes that all prototype bits fit in an int32_t
  li(t1, (int32_t)(intptr_t)markWord::prototype().value());
  st_d(t1, Address(obj, oopDesc::mark_offset_in_bytes()));

  if (UseCompressedClassPointers) { // Take care not to kill klass
    encode_klass_not_null(t1, klass);
    st_w(t1, Address(obj, oopDesc::klass_offset_in_bytes()));
  } else {
    st_d(klass, Address(obj, oopDesc::klass_offset_in_bytes()));
  }

  if (len->is_valid()) {
    st_w(len, Address(obj, arrayOopDesc::length_offset_in_bytes()));
  } else if (UseCompressedClassPointers) {
    store_klass_gap(obj, R0);
  }
}

// preserves obj, destroys len_in_bytes
//
// Scratch registers: t1 = T0, t2 = T1
//
void C1_MacroAssembler::initialize_body(Register obj, Register len_in_bytes,
                                        int hdr_size_in_bytes, Register t1, Register t2) {
  assert(hdr_size_in_bytes >= 0, "header size must be positive or 0");
  assert(t1 == T0 && t2 == T1, "must be");
  Label done;

  // len_in_bytes is positive and ptr sized
  addi_d(len_in_bytes, len_in_bytes, -hdr_size_in_bytes);
  beqz(len_in_bytes, done);

  // zero_words() takes ptr in t1 and count in bytes in t2
  lea(t1, Address(obj, hdr_size_in_bytes));
  addi_d(t2, len_in_bytes, -BytesPerWord);

  Label loop;
  bind(loop);
  stx_d(R0, t1, t2);
  addi_d(t2, t2, -BytesPerWord);
  bge(t2, R0, loop);

  bind(done);
}

void C1_MacroAssembler::allocate_object(Register obj, Register t1, Register t2, int header_size,
                                        int object_size, Register klass, Label& slow_case) {
  assert_different_registers(obj, t1, t2);
  assert(header_size >= 0 && object_size >= header_size, "illegal sizes");

  try_allocate(obj, noreg, object_size * BytesPerWord, t1, t2, slow_case);

  initialize_object(obj, klass, noreg, object_size * HeapWordSize, t1, t2, UseTLAB);
}

// Scratch registers: t1 = T0, t2 = T1
void C1_MacroAssembler::initialize_object(Register obj, Register klass, Register var_size_in_bytes,
                                          int con_size_in_bytes, Register t1, Register t2,
                                          bool is_tlab_allocated) {
  assert((con_size_in_bytes & MinObjAlignmentInBytesMask) == 0,
         "con_size_in_bytes is not multiple of alignment");
  const int hdr_size_in_bytes = instanceOopDesc::header_size() * HeapWordSize;

  initialize_header(obj, klass, noreg, t1, t2);

  if (!(UseTLAB && ZeroTLAB && is_tlab_allocated)) {
     // clear rest of allocated space
     const Register index = t2;
     if (var_size_in_bytes != noreg) {
       move(index, var_size_in_bytes);
       initialize_body(obj, index, hdr_size_in_bytes, t1, t2);
     } else if (con_size_in_bytes > hdr_size_in_bytes) {
       con_size_in_bytes -= hdr_size_in_bytes;
       lea(t1, Address(obj, hdr_size_in_bytes));
       Label loop;
       li(SCR1, con_size_in_bytes - BytesPerWord);
       bind(loop);
       stx_d(R0, t1, SCR1);
       addi_d(SCR1, SCR1, -BytesPerWord);
       bge(SCR1, R0, loop);
     }
  }

  membar(StoreStore);

  if (CURRENT_ENV->dtrace_alloc_probes()) {
    assert(obj == A0, "must be");
    call(Runtime1::entry_for(Runtime1::dtrace_object_alloc_id), relocInfo::runtime_call_type);
  }

  verify_oop(obj);
}

void C1_MacroAssembler::allocate_array(Register obj, Register len, Register t1, Register t2,
                                       int header_size, int f, Register klass, Label& slow_case) {
  assert_different_registers(obj, len, t1, t2, klass);

  // determine alignment mask
  assert(!(BytesPerWord & 1), "must be a multiple of 2 for masking code to work");

  // check for negative or excessive length
  li(SCR1, (int32_t)max_array_allocation_length);
  bge_far(len, SCR1, slow_case, false);

  const Register arr_size = t2; // okay to be the same
  // align object end
  li(arr_size, (int32_t)header_size * BytesPerWord + MinObjAlignmentInBytesMask);
  slli_w(SCR1, len, f);
  add_d(arr_size, arr_size, SCR1);
  bstrins_d(arr_size, R0, exact_log2(MinObjAlignmentInBytesMask + 1) - 1, 0);

  try_allocate(obj, arr_size, 0, t1, t2, slow_case);

  initialize_header(obj, klass, len, t1, t2);

  // clear rest of allocated space
  initialize_body(obj, arr_size, header_size * BytesPerWord, t1, t2);

  membar(StoreStore);

  if (CURRENT_ENV->dtrace_alloc_probes()) {
    assert(obj == A0, "must be");
    call(Runtime1::entry_for(Runtime1::dtrace_object_alloc_id), relocInfo::runtime_call_type);
  }

  verify_oop(obj);
}

void C1_MacroAssembler::build_frame(int framesize, int bang_size_in_bytes) {
  assert(bang_size_in_bytes >= framesize, "stack bang size incorrect");
  // Make sure there is enough stack space for this method's activation.
  // Note that we do this before creating a frame.
  generate_stack_overflow_check(bang_size_in_bytes);
  MacroAssembler::build_frame(framesize);

  // Insert nmethod entry barrier into frame.
  BarrierSetAssembler* bs = BarrierSet::barrier_set()->barrier_set_assembler();
  bs->nmethod_entry_barrier(this, nullptr /* slow_path */, nullptr /* continuation */, nullptr /* guard */);
}

void C1_MacroAssembler::remove_frame(int framesize) {
  MacroAssembler::remove_frame(framesize);
}

void C1_MacroAssembler::verified_entry(bool breakAtEntry) {
  // If we have to make this method not-entrant we'll overwrite its
  // first instruction with a jump.  For this action to be legal we
  // must ensure that this first instruction is a b, bl, nop, break.
  // Make it a NOP.
  nop();
}

void C1_MacroAssembler::load_parameter(int offset_in_words, Register reg) {
  //  FP + -2: link
  //     + -1: return address
  //     +  0: argument with offset 0
  //     +  1: argument with offset 1
  //     +  2: ...

  ld_d(reg, Address(FP, offset_in_words * BytesPerWord));
}

#ifndef PRODUCT
void C1_MacroAssembler::verify_stack_oop(int stack_offset) {
  if (!VerifyOops) return;
  verify_oop_addr(Address(SP, stack_offset));
}

void C1_MacroAssembler::verify_not_null_oop(Register r) {
  if (!VerifyOops) return;
  Label not_null;
  bnez(r, not_null);
  stop("non-null oop required");
  bind(not_null);
  verify_oop(r);
}

void C1_MacroAssembler::invalidate_registers(bool inv_a0, bool inv_s0, bool inv_a2,
                                             bool inv_a3, bool inv_a4, bool inv_a5) {
#ifdef ASSERT
  static int nn;
  if (inv_a0) li(A0, 0xDEAD);
  if (inv_s0) li(S0, 0xDEAD);
  if (inv_a2) li(A2, nn++);
  if (inv_a3) li(A3, 0xDEAD);
  if (inv_a4) li(A4, 0xDEAD);
  if (inv_a5) li(A5, 0xDEAD);
#endif
}
#endif // ifndef PRODUCT
