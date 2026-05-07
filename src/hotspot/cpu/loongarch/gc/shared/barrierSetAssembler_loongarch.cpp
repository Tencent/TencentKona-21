/*
 * Copyright (c) 2018, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2018, 2023, Loongson Technology. All rights reserved.
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
#include "classfile/classLoaderData.hpp"
#include "gc/shared/barrierSet.hpp"
#include "gc/shared/barrierSetAssembler.hpp"
#include "gc/shared/barrierSetNMethod.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "interpreter/interp_masm.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/jniHandles.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubRoutines.hpp"

#define __ masm->

void BarrierSetAssembler::load_at(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
                                  Register dst, Address src, Register tmp1, Register tmp2) {
  // RA is live. It must be saved around calls.

  bool in_heap = (decorators & IN_HEAP) != 0;
  bool in_native = (decorators & IN_NATIVE) != 0;
  bool is_not_null = (decorators & IS_NOT_NULL) != 0;

  switch (type) {
  case T_OBJECT:
  case T_ARRAY: {
    if (in_heap) {
      if (UseCompressedOops) {
        __ ld_wu(dst, src);
        if (is_not_null) {
          __ decode_heap_oop_not_null(dst);
        } else {
          __ decode_heap_oop(dst);
        }
      } else {
        __ ld_d(dst, src);
      }
    } else {
      assert(in_native, "why else?");
      __ ld_d(dst, src);
    }
    break;
  }
  case T_BOOLEAN: __ ld_bu   (dst, src);    break;
  case T_BYTE:    __ ld_b    (dst, src);    break;
  case T_CHAR:    __ ld_hu   (dst, src);    break;
  case T_SHORT:   __ ld_h    (dst, src);    break;
  case T_INT:     __ ld_w    (dst, src);    break;
  case T_LONG:    __ ld_d    (dst, src);    break;
  case T_ADDRESS: __ ld_d    (dst, src);    break;
  case T_FLOAT:
    assert(dst == noreg, "only to ftos");
    __ fld_s(FSF, src);
    break;
  case T_DOUBLE:
    assert(dst == noreg, "only to dtos");
    __ fld_d(FSF, src);
    break;
  default: Unimplemented();
  }
}

void BarrierSetAssembler::store_at(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
                                   Address dst, Register val, Register tmp1, Register tmp2, Register tmp3) {
  bool in_heap = (decorators & IN_HEAP) != 0;
  bool in_native = (decorators & IN_NATIVE) != 0;
  bool is_not_null = (decorators & IS_NOT_NULL) != 0;

  switch (type) {
  case T_OBJECT:
  case T_ARRAY: {
    if (in_heap) {
      if (val == noreg) {
        assert(!is_not_null, "inconsistent access");
        if (UseCompressedOops) {
          __ st_w(R0, dst);
        } else {
          __ st_d(R0, dst);
        }
      } else {
        if (UseCompressedOops) {
          assert(!dst.uses(val), "not enough registers");
          if (is_not_null) {
            __ encode_heap_oop_not_null(val);
          } else {
            __ encode_heap_oop(val);
          }
          __ st_w(val, dst);
        } else {
          __ st_d(val, dst);
        }
      }
    } else {
      assert(in_native, "why else?");
      assert(val != noreg, "not supported");
      __ st_d(val, dst);
    }
    break;
  }
  case T_BOOLEAN:
    __ andi(val, val, 0x1);  // boolean is true if LSB is 1
    __ st_b(val, dst);
    break;
  case T_BYTE:
    __ st_b(val, dst);
    break;
  case T_SHORT:
    __ st_h(val, dst);
    break;
  case T_CHAR:
    __ st_h(val, dst);
    break;
  case T_INT:
    __ st_w(val, dst);
    break;
  case T_LONG:
    __ st_d(val, dst);
    break;
  case T_FLOAT:
    assert(val == noreg, "only tos");
    __ fst_s(FSF, dst);
    break;
  case T_DOUBLE:
    assert(val == noreg, "only tos");
    __ fst_d(FSF, dst);
    break;
  case T_ADDRESS:
    __ st_d(val, dst);
    break;
  default: Unimplemented();
  }
}

void BarrierSetAssembler::copy_load_at(MacroAssembler* masm,
                                       DecoratorSet decorators,
                                       BasicType type,
                                       size_t bytes,
                                       Register dst,
                                       Address src,
                                       Register tmp) {
  if (bytes == 1) {
    __ ld_bu(dst, src);
  } else if (bytes == 2) {
    __ ld_hu(dst, src);
  } else if (bytes == 4) {
    __ ld_wu(dst, src);
  } else if (bytes == 8) {
    __ ld_d(dst, src);
  } else {
    // Not the right size
    ShouldNotReachHere();
  }
  if ((decorators & ARRAYCOPY_CHECKCAST) != 0 && UseCompressedOops) {
    __ decode_heap_oop(dst);
  }
}

void BarrierSetAssembler::copy_store_at(MacroAssembler* masm,
                                        DecoratorSet decorators,
                                        BasicType type,
                                        size_t bytes,
                                        Address dst,
                                        Register src,
                                        Register tmp1,
                                        Register tmp2,
                                        Register tmp3) {
  if ((decorators & ARRAYCOPY_CHECKCAST) != 0 && UseCompressedOops) {
    __ encode_heap_oop(src);
  }

  if (bytes == 1) {
    __ st_b(src, dst);
  } else if (bytes == 2) {
    __ st_h(src, dst);
  } else if (bytes == 4) {
    __ st_w(src, dst);
  } else if (bytes == 8) {
    __ st_d(src, dst);
  } else {
    // Not the right size
    ShouldNotReachHere();
  }
}

void BarrierSetAssembler::copy_load_at(MacroAssembler* masm,
                                       DecoratorSet decorators,
                                       BasicType type,
                                       size_t bytes,
                                       FloatRegister dst,
                                       Address src,
                                       Register tmp1,
                                       Register tmp2,
                                       FloatRegister vec_tmp,
                                       bool need_save_restore) {
  assert(bytes > 8, "can only deal with vector registers");
  if (UseLSX && bytes == 16) {
    __ vld(dst, src.base(), src.disp());
  } else if (UseLASX && bytes == 32) {
    __ xvld(dst, src.base(), src.disp());
  } else {
    ShouldNotReachHere();
  }
}

void BarrierSetAssembler::copy_store_at(MacroAssembler* masm,
                                        DecoratorSet decorators,
                                        BasicType type,
                                        size_t bytes,
                                        Address dst,
                                        FloatRegister src,
                                        Register tmp1,
                                        Register tmp2,
                                        Register tmp3,
                                        Register tmp4,
                                        FloatRegister vec_tmp1,
                                        FloatRegister vec_tmp2,
                                        bool need_save_restore) {
  assert(bytes > 8, "can only deal with vector registers");
  if (UseLSX && bytes == 16) {
    __ vst(src, dst.base(), dst.disp());
  } else if (UseLASX && bytes == 32) {
    __ xvst(src, dst.base(), dst.disp());
  } else {
    ShouldNotReachHere();
  }
}

void BarrierSetAssembler::obj_equals(MacroAssembler* masm,
                                     Register obj1, Address obj2) {
  Unimplemented();
}

void BarrierSetAssembler::obj_equals(MacroAssembler* masm,
                                     Register obj1, Register obj2) {
  Unimplemented();
}

void BarrierSetAssembler::try_resolve_jobject_in_native(MacroAssembler* masm, Register jni_env,
                                                        Register obj, Register tmp, Label& slowpath) {
  STATIC_ASSERT(JNIHandles::tag_mask == 3);
  __ addi_d(AT, R0, ~(int)JNIHandles::tag_mask);
  __ andr(obj, obj, AT);
  __ ld_d(obj, Address(obj, 0));
}

// Defines obj, preserves var_size_in_bytes, okay for t2 == var_size_in_bytes.
void BarrierSetAssembler::tlab_allocate(MacroAssembler* masm, Register obj,
                                        Register var_size_in_bytes,
                                        int con_size_in_bytes,
                                        Register t1,
                                        Register t2,
                                        Label& slow_case) {
  assert_different_registers(obj, t2);
  assert_different_registers(obj, var_size_in_bytes);
  Register end = t2;

  // verify_tlab();

  __ ld_d(obj, Address(TREG, JavaThread::tlab_top_offset()));
  if (var_size_in_bytes == noreg) {
    __ lea(end, Address(obj, con_size_in_bytes));
  } else {
    __ lea(end, Address(obj, var_size_in_bytes, Address::no_scale, 0));
  }
  __ ld_d(SCR1, Address(TREG, JavaThread::tlab_end_offset()));
  __ blt_far(SCR1, end, slow_case, false);

  // update the tlab top pointer
  __ st_d(end, Address(TREG, JavaThread::tlab_top_offset()));

  // recover var_size_in_bytes if necessary
  if (var_size_in_bytes == end) {
    __ sub_d(var_size_in_bytes, var_size_in_bytes, obj);
  }
  // verify_tlab();
}

void BarrierSetAssembler::incr_allocated_bytes(MacroAssembler* masm,
                                               Register var_size_in_bytes,
                                               int con_size_in_bytes,
                                               Register t1) {
  assert(t1->is_valid(), "need temp reg");

  __ ld_d(t1, Address(TREG, JavaThread::allocated_bytes_offset()));
  if (var_size_in_bytes->is_valid())
    __ add_d(t1, t1, var_size_in_bytes);
  else
    __ addi_d(t1, t1, con_size_in_bytes);
  __ st_d(t1, Address(TREG, JavaThread::allocated_bytes_offset()));
}

static volatile uint32_t _patching_epoch = 0;

address BarrierSetAssembler::patching_epoch_addr() {
  return (address)&_patching_epoch;
}

void BarrierSetAssembler::increment_patching_epoch() {
  Atomic::inc(&_patching_epoch);
}

void BarrierSetAssembler::clear_patching_epoch() {
  _patching_epoch = 0;
}

void BarrierSetAssembler::nmethod_entry_barrier(MacroAssembler* masm, Label* slow_path, Label* continuation, Label* guard) {
  BarrierSetNMethod* bs_nm = BarrierSet::barrier_set()->barrier_set_nmethod();

  if (bs_nm == nullptr) {
    return;
  }

  Label local_guard;
  NMethodPatchingType patching_type = nmethod_patching_type();

  if (slow_path == nullptr) {
    guard = &local_guard;
  }

  __ lipc(SCR1, *guard);
  __ ld_wu(SCR1, SCR1, 0);

  switch (patching_type) {
    case NMethodPatchingType::conc_data_patch:
      // Subsequent loads of oops must occur after load of guard value.
      // BarrierSetNMethod::disarm sets guard with release semantics.
      __ membar(__ LoadLoad); // fall through to stw_instruction_and_data_patch
    case NMethodPatchingType::stw_instruction_and_data_patch:
      {
        // With STW patching, no data or instructions are updated concurrently,
        // which means there isn't really any need for any fencing for neither
        // data nor instruction modification happening concurrently. The
        // instruction patching is synchronized with global icache_flush() by
        // the write hart on riscv. So here we can do a plain conditional
        // branch with no fencing.
        Address thread_disarmed_addr(TREG, in_bytes(bs_nm->thread_disarmed_guard_value_offset()));
        __ ld_wu(SCR2, thread_disarmed_addr);
        break;
      }
    case NMethodPatchingType::conc_instruction_and_data_patch:
      {
        // If we patch code we need both a code patching and a loadload
        // fence. It's not super cheap, so we use a global epoch mechanism
        // to hide them in a slow path.
        // The high level idea of the global epoch mechanism is to detect
        // when any thread has performed the required fencing, after the
        // last nmethod was disarmed. This implies that the required
        // fencing has been performed for all preceding nmethod disarms
        // as well. Therefore, we do not need any further fencing.
        __ lea_long(SCR2, ExternalAddress((address)&_patching_epoch));
        // Embed an artificial data dependency to order the guard load
        // before the epoch load.
        __ srli_d(RA, SCR1, 32);
        __ orr(SCR2, SCR2, RA);
        // Read the global epoch value.
        __ ld_wu(SCR2, SCR2, 0);
        // Combine the guard value (low order) with the epoch value (high order).
        __ slli_d(SCR2, SCR2, 32);
        __ orr(SCR1, SCR1, SCR2);
        // Compare the global values with the thread-local values
        Address thread_disarmed_and_epoch_addr(TREG, in_bytes(bs_nm->thread_disarmed_guard_value_offset()));
        __ ld_d(SCR2, thread_disarmed_and_epoch_addr);
        break;
      }
    default:
      ShouldNotReachHere();
  }

  if (slow_path == nullptr) {
    Label skip_barrier;
    __ beq(SCR1, SCR2, skip_barrier);

    __ call_long(StubRoutines::la::method_entry_barrier());
    __ b(skip_barrier);

    __ bind(local_guard);
    __ emit_int32(0);   // nmethod guard value. Skipped over in common case.
    __ bind(skip_barrier);
  } else {
    __ xorr(SCR1, SCR1, SCR2);
    __ bnez(SCR1, *slow_path);
    __ bind(*continuation);
  }
}

void BarrierSetAssembler::c2i_entry_barrier(MacroAssembler* masm) {
  BarrierSetNMethod* bs = BarrierSet::barrier_set()->barrier_set_nmethod();
  if (bs == nullptr) {
    return;
  }

  Label bad_call;
  __ beqz(Rmethod, bad_call);

  // Pointer chase to the method holder to find out if the method is concurrently unloading.
  Label method_live;
  __ load_method_holder_cld(SCR2, Rmethod);

  // Is it a strong CLD?
  __ ld_w(SCR1, Address(SCR2, ClassLoaderData::keep_alive_offset()));
  __ bnez(SCR1, method_live);

  // Is it a weak but alive CLD?
  __ push2(T2, T8);
  __ ld_d(T8, Address(SCR2, ClassLoaderData::holder_offset()));
  __ resolve_weak_handle(T8, T2, SCR2); // Assembler occupies SCR1.
  __ move(SCR1, T8);
  __ pop2(T2, T8);
  __ bnez(SCR1, method_live);

  __ bind(bad_call);

  __ jmp(SharedRuntime::get_handle_wrong_method_stub(), relocInfo::runtime_call_type);
  __ bind(method_live);
}

void BarrierSetAssembler::check_oop(MacroAssembler* masm, Register obj, Register tmp1, Register tmp2, Label& error) {
  // Check if the oop is in the right area of memory
  __ li(tmp2, (intptr_t) Universe::verify_oop_mask());
  __ andr(tmp1, obj, tmp2);
  __ li(tmp2, (intptr_t) Universe::verify_oop_bits());

  // Compare tmp1 and tmp2.
  __ bne(tmp1, tmp2, error);

  // make sure klass is 'reasonable', which is not zero.
  __ load_klass(obj, obj); // get klass
  __ beqz(obj, error);     // if klass is null it is broken
}
