/*
 * Copyright (c) 2019, 2021, Oracle and/or its affiliates. All rights reserved.
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
 */

#include "precompiled.hpp"
#include "asm/macroAssembler.inline.hpp"
#include "code/codeBlob.hpp"
#include "code/vmreg.inline.hpp"
#include "gc/z/zBarrier.inline.hpp"
#include "gc/z/zBarrierSet.hpp"
#include "gc/z/zBarrierSetAssembler.hpp"
#include "gc/z/zBarrierSetRuntime.hpp"
#include "gc/z/zThreadLocalData.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/jniHandles.hpp"
#include "runtime/sharedRuntime.hpp"
#include "utilities/macros.hpp"
#ifdef COMPILER1
#include "c1/c1_LIRAssembler.hpp"
#include "c1/c1_MacroAssembler.hpp"
#include "gc/z/c1/zBarrierSetC1.hpp"
#endif // COMPILER1
#ifdef COMPILER2
#include "gc/z/c2/zBarrierSetC2.hpp"
#include "opto/output.hpp"
#endif // COMPILER2

#ifdef PRODUCT
#define BLOCK_COMMENT(str) /* nothing */
#else
#define BLOCK_COMMENT(str) __ block_comment(str)
#endif

#undef __
#define __ masm->

// Helper for saving and restoring registers across a runtime call that does
// not have any live vector registers.
class ZRuntimeCallSpill {
private:
  MacroAssembler* _masm;
  Register _result;

  void save() {
    MacroAssembler* masm = _masm;

    __ enter();
    if (_result != noreg) {
      __ push_call_clobbered_registers_except(RegSet::of(_result));
    } else {
      __ push_call_clobbered_registers();
    }
  }

  void restore() {
    MacroAssembler* masm = _masm;

    if (_result != noreg) {
      // Make sure _result has the return value.
      if (_result != V0) {
        __ move(_result, V0);
      }

      __ pop_call_clobbered_registers_except(RegSet::of(_result));
    } else {
      __ pop_call_clobbered_registers();
    }
    __ leave();
  }

public:
  ZRuntimeCallSpill(MacroAssembler* masm, Register result)
    : _masm(masm),
      _result(result) {
    save();
  }

  ~ZRuntimeCallSpill() {
    restore();
  }
};

void ZBarrierSetAssembler::check_oop(MacroAssembler* masm, Register obj, Register tmp1, Register tmp2, Label& error) {
  // C1 calls verify_oop in the middle of barriers, before they have been uncolored
  // and after being colored. Therefore, we must deal with colored oops as well.
  Label done;
  Label check_oop;
  Label check_zaddress;
  int color_bits = ZPointerRemappedShift + ZPointerRemappedBits;

  uintptr_t shifted_base_start_mask = (UCONST64(1) << (ZAddressHeapBaseShift + color_bits + 1)) - 1;
  uintptr_t shifted_base_end_mask = (UCONST64(1) << (ZAddressHeapBaseShift + 1)) - 1;
  uintptr_t shifted_base_mask = shifted_base_start_mask ^ shifted_base_end_mask;

  uintptr_t shifted_address_end_mask = (UCONST64(1) << (color_bits + 1)) - 1;
  uintptr_t shifted_address_mask = shifted_base_end_mask ^ (uintptr_t)CONST64(-1);

  // Check colored null
  __ li(tmp1, shifted_address_mask);
  __ andr(tmp1, tmp1, obj);
  __ beqz(tmp1, done);

  // Check for zpointer
  __ li(tmp1, shifted_base_mask);
  __ andr(tmp1, tmp1, obj);
  __ beqz(tmp1, check_oop);

  // Uncolor presumed zpointer
  __ z_uncolor(obj);

  __ b(check_zaddress);

  __ bind(check_oop);

  // make sure klass is 'reasonable', which is not zero.
  __ load_klass(tmp1, obj);  // get klass
  __ beqz(tmp1, error); // if klass is null it is broken

  __ bind(check_zaddress);
  // Check if the oop is in the right area of memory
  __ li(tmp1, (intptr_t) Universe::verify_oop_mask());
  __ andr(tmp1, tmp1, obj);
  __ li(obj, (intptr_t) Universe::verify_oop_bits());
  __ bne(tmp1, obj, error);

  __ bind(done);
}

void ZBarrierSetAssembler::load_at(MacroAssembler* masm,
                                   DecoratorSet decorators,
                                   BasicType type,
                                   Register dst,
                                   Address src,
                                   Register tmp1,
                                   Register tmp2) {
  if (!ZBarrierSet::barrier_needed(decorators, type)) {
    // Barrier not needed
    BarrierSetAssembler::load_at(masm, decorators, type, dst, src, tmp1, tmp2);
    return;
  }

  BLOCK_COMMENT("ZBarrierSetAssembler::load_at {");

  assert_different_registers(tmp1, tmp2, src.base(), noreg);
  assert_different_registers(tmp1, tmp2, src.index());
  assert_different_registers(tmp1, tmp2, dst, noreg);

  Label done;
  Label uncolor;

  // Load bad mask into scratch register.
  const bool on_non_strong =
    (decorators & ON_WEAK_OOP_REF) != 0 ||
    (decorators & ON_PHANTOM_OOP_REF) != 0;

  // Test address bad mask
  if (on_non_strong) {
    __ ld_d(tmp1, mark_bad_mask_from_thread(TREG));
  } else {
    __ ld_d(tmp1, load_bad_mask_from_thread(TREG));
  }

  __ lea(tmp2, src);
  __ ld_d(dst, tmp2, 0);

  // Test reference against bad mask. If mask bad, then we need to fix it up.
  __ andr(tmp1, dst, tmp1);
  __ beqz(tmp1, uncolor);

  {
    // Call VM
    ZRuntimeCallSpill rcs(masm, dst);

    if (A0 != dst) {
      __ move(A0, dst);
    }
    __ move(A1, tmp2);
    __ MacroAssembler::call_VM_leaf_base(ZBarrierSetRuntime::load_barrier_on_oop_field_preloaded_addr(decorators), 2);
  }

  // Slow-path has already uncolored
  __ b(done);

  __ bind(uncolor);

  // Remove the color bits
  __ z_uncolor(dst);

  __ bind(done);

  BLOCK_COMMENT("} ZBarrierSetAssembler::load_at");
}

void ZBarrierSetAssembler::store_barrier_fast(MacroAssembler* masm,
                                              Address ref_addr,
                                              Register rnew_zaddress,
                                              Register rnew_zpointer,
                                              Register rtmp,
                                              bool in_nmethod,
                                              bool is_atomic,
                                              Label& medium_path,
                                              Label& medium_path_continuation) const {
  assert_different_registers(ref_addr.base(), rnew_zpointer, rtmp);
  assert_different_registers(ref_addr.index(), rnew_zpointer, rtmp);
  assert_different_registers(rnew_zaddress, rnew_zpointer, rtmp);

  if (in_nmethod) {
    if (is_atomic) {
      __ ld_hu(rtmp, ref_addr);
      // Atomic operations must ensure that the contents of memory are store-good before
      // an atomic operation can execute.
      // A not relocatable object could have spurious raw null pointers in its fields after
      // getting promoted to the old generation.
      __ relocate(barrier_Relocation::spec(), ZBarrierRelocationFormatStoreGoodBits);
      __ patchable_li16(rnew_zpointer, barrier_Relocation::unpatched);
      __ bne(rtmp, rnew_zpointer, medium_path);
    } else {
      __ ld_d(rtmp, ref_addr);
      // Stores on relocatable objects never need to deal with raw null pointers in fields.
      // Raw null pointers may only exist in the young generation, as they get pruned when
      // the object is relocated to old. And no pre-write barrier needs to perform any action
      // in the young generation.
      __ relocate(barrier_Relocation::spec(), ZBarrierRelocationFormatStoreBadMask);
      __ patchable_li16(rnew_zpointer, barrier_Relocation::unpatched);
      __ andr(rtmp, rtmp, rnew_zpointer);
      __ bnez(rtmp, medium_path);
    }
    __ bind(medium_path_continuation);
    __ z_color(rnew_zpointer, rnew_zaddress, rtmp);
  } else {
    assert(!is_atomic, "atomics outside of nmethods not supported");
    __ lea(rtmp, ref_addr);
    __ ld_d(rtmp, rtmp, 0);
    __ ld_d(rnew_zpointer, Address(TREG, ZThreadLocalData::store_bad_mask_offset()));
    __ andr(rtmp, rtmp, rnew_zpointer);
    __ bnez(rtmp, medium_path);
    __ bind(medium_path_continuation);
    if (rnew_zaddress == noreg) {
      __ move(rnew_zpointer, R0);
    } else {
      __ move(rnew_zpointer, rnew_zaddress);
    }

    // Load the current good shift, and add the color bits
    __ slli_d(rnew_zpointer, rnew_zpointer, ZPointerLoadShift);
    __ ld_d(rtmp, Address(TREG, ZThreadLocalData::store_good_mask_offset()));
    __ orr(rnew_zpointer, rnew_zpointer, rtmp);
  }
}

static void store_barrier_buffer_add(MacroAssembler* masm,
                                     Address ref_addr,
                                     Register tmp1,
                                     Register tmp2,
                                     Label& slow_path) {
  Address buffer(TREG, ZThreadLocalData::store_barrier_buffer_offset());
  assert_different_registers(ref_addr.base(), ref_addr.index(), tmp1, tmp2);

  __ ld_d(tmp1, buffer);

  // Combined pointer bump and check if the buffer is disabled or full
  // Tune ZStoreBarrierBuffer length to decrease the opportunity goto
  // copy_store_at slow-path.
  __ ld_d(tmp2, Address(tmp1, ZStoreBarrierBuffer::current_offset()));
  __ beqz(tmp2, slow_path);

  // Bump the pointer
  __ addi_d(tmp2, tmp2, - (int) sizeof(ZStoreBarrierEntry));
  __ st_d(tmp2, Address(tmp1, ZStoreBarrierBuffer::current_offset()));

  // Compute the buffer entry address
  __ lea(tmp2, Address(tmp2, ZStoreBarrierBuffer::buffer_offset()));
  __ add_d(tmp2, tmp2, tmp1);

  // Compute and log the store address
  __ lea(tmp1, ref_addr);
  __ st_d(tmp1, Address(tmp2, in_bytes(ZStoreBarrierEntry::p_offset())));

  // Load and log the prev value
  __ ld_d(tmp1, tmp1, 0);
  __ st_d(tmp1, Address(tmp2, in_bytes(ZStoreBarrierEntry::prev_offset())));
}

void ZBarrierSetAssembler::store_barrier_medium(MacroAssembler* masm,
                                                Address ref_addr,
                                                Register rtmp1,
                                                Register rtmp2,
                                                Register rtmp3,
                                                bool is_native,
                                                bool is_atomic,
                                                Label& medium_path_continuation,
                                                Label& slow_path,
                                                Label& slow_path_continuation) const {
  assert_different_registers(ref_addr.base(), ref_addr.index(), rtmp1, rtmp2);

  // The reason to end up in the medium path is that the pre-value was not 'good'.
  if (is_native) {
    __ b(slow_path);
    __ bind(slow_path_continuation);
    __ b(medium_path_continuation);
  } else if (is_atomic) {
    // Atomic accesses can get to the medium fast path because the value was a
    // raw null value. If it was not null, then there is no doubt we need to take a slow path.

    __ lea(rtmp2, ref_addr);
    __ ld_d(rtmp1, rtmp2, 0);
    __ bnez(rtmp1, slow_path);

    // If we get this far, we know there is a young raw null value in the field.
    __ relocate(barrier_Relocation::spec(), ZBarrierRelocationFormatStoreGoodBits);
    __ patchable_li16(rtmp1, barrier_Relocation::unpatched);
    __ cmpxchg(Address(rtmp2, 0), R0, rtmp1, SCR1,
               false /* retold */, false /* barrier */, true /* weak */, false /* exchange */);
    __ beqz(SCR1, slow_path);

    __ bind(slow_path_continuation);
    __ b(medium_path_continuation);
  } else {
    // A non-atomic relocatable object won't get to the medium fast path due to a
    // raw null in the young generation. We only get here because the field is bad.
    // In this path we don't need any self healing, so we can avoid a runtime call
    // most of the time by buffering the store barrier to be applied lazily.
    store_barrier_buffer_add(masm,
                             ref_addr,
                             rtmp1,
                             rtmp2,
                             slow_path);
    __ bind(slow_path_continuation);
    __ b(medium_path_continuation);
  }
}

void ZBarrierSetAssembler::store_at(MacroAssembler* masm,
                                    DecoratorSet decorators,
                                    BasicType type,
                                    Address dst,
                                    Register val,
                                    Register tmp1,
                                    Register tmp2,
                                    Register tmp3) {
  if (!ZBarrierSet::barrier_needed(decorators, type)) {
    BarrierSetAssembler::store_at(masm, decorators, type, dst, val, tmp1, tmp2, tmp3);
    return;
  }

  bool dest_uninitialized = (decorators & IS_DEST_UNINITIALIZED) != 0;

  assert_different_registers(val, tmp1, dst.base());

  if (dest_uninitialized) {
    if (val == noreg) {
      __ move(tmp1, R0);
    } else {
      __ move(tmp1, val);
    }
    // Add the color bits
    __ slli_d(tmp1, tmp1, ZPointerLoadShift);
    __ ld_d(tmp2, Address(TREG, ZThreadLocalData::store_good_mask_offset()));
    __ orr(tmp1, tmp2, tmp1);
  } else {
    Label done;
    Label medium;
    Label medium_continuation;
    Label slow;
    Label slow_continuation;
    store_barrier_fast(masm, dst, val, tmp1, tmp2, false, false, medium, medium_continuation);

    __ b(done);
    __ bind(medium);
    store_barrier_medium(masm,
                         dst,
                         tmp1,
                         tmp2,
                         noreg /* tmp3 */,
                         false /* is_native */,
                         false /* is_atomic */,
                         medium_continuation,
                         slow,
                         slow_continuation);

    __ bind(slow);
    {
      // Call VM
      ZRuntimeCallSpill rcs(masm, noreg);
      __ lea(A0, dst);
      __ MacroAssembler::call_VM_leaf_base(ZBarrierSetRuntime::store_barrier_on_oop_field_without_healing_addr(), 1);
    }

    __ b(slow_continuation);
    __ bind(done);
  }

  // Store value
  BarrierSetAssembler::store_at(masm, decorators, type, dst, tmp1, tmp2, tmp3, noreg);
}

// Reference to stub generate_disjoint|conjoint_large_copy_lsx|lasx and generate_long_small_copy
static FloatRegister z_copy_load_bad_vreg    = FT10;
static FloatRegister z_copy_store_good_vreg  = FT11;
static FloatRegister z_copy_store_bad_vreg   = FT12;
static FloatRegSet   z_arraycopy_saved_vregs = FloatRegSet::of(F0, F1) +
                                               FloatRegSet::range(FT0, FT7) +
                                               FloatRegSet::of(z_copy_load_bad_vreg,
                                                               z_copy_store_good_vreg,
                                                               z_copy_store_bad_vreg);

static void load_wide_arraycopy_masks(MacroAssembler* masm) {
  __ lea_long(SCR1, ExternalAddress((address)&ZPointerVectorLoadBadMask));
  if (UseLASX) {
    __ xvld(z_copy_load_bad_vreg, SCR1, 0);
  } else if (UseLSX) {
    __ vld(z_copy_load_bad_vreg, SCR1, 0);
  }

  __ lea_long(SCR1, ExternalAddress((address)&ZPointerVectorStoreBadMask));
  if (UseLASX) {
    __ xvld(z_copy_store_bad_vreg, SCR1, 0);
  } else if (UseLSX) {
    __ vld(z_copy_store_bad_vreg, SCR1, 0);
  }

  __ lea_long(SCR1, ExternalAddress((address)&ZPointerVectorStoreGoodMask));
  if (UseLASX) {
    __ xvld(z_copy_store_good_vreg, SCR1, 0);
  } else if (UseLSX) {
    __ vld(z_copy_store_good_vreg, SCR1, 0);
  }
}

void ZBarrierSetAssembler::arraycopy_prologue(MacroAssembler* masm,
                                              DecoratorSet decorators,
                                              bool is_oop,
                                              Register src,
                                              Register dst,
                                              Register count,
                                              RegSet saved_regs) {
  if (!is_oop) {
    // Barrier not needed
    return;
  }

  BLOCK_COMMENT("ZBarrierSetAssembler::arraycopy_prologue {");

  load_wide_arraycopy_masks(masm);

  BLOCK_COMMENT("} ZBarrierSetAssembler::arraycopy_prologue");
}

void ZBarrierSetAssembler::copy_load_at(MacroAssembler* masm,
                                        DecoratorSet decorators,
                                        BasicType type,
                                        size_t bytes,
                                        Register dst,
                                        Address src,
                                        Register tmp) {
  if (!is_reference_type(type)) {
    BarrierSetAssembler::copy_load_at(masm, decorators, type, bytes, dst, src, noreg);
    return;
  }

  Label load_done;

  // Load oop at address
  BarrierSetAssembler::copy_load_at(masm, decorators, type, bytes, dst, src, noreg);

  assert_different_registers(dst, tmp);

  // Test address bad mask
  __ ld_d(tmp, Address(TREG, ZThreadLocalData::load_bad_mask_offset()));
  __ andr(tmp, dst, tmp);
  __ beqz(tmp, load_done);

  {
    // Call VM
    ZRuntimeCallSpill rsc(masm, dst);

    __ lea(A1, src);

    if (A0 != dst) {
      __ move(A0, dst);
    }

    __ MacroAssembler::call_VM_leaf_base(ZBarrierSetRuntime::load_barrier_on_oop_field_preloaded_store_good_addr(), 2);
  }

  __ bind(load_done);

  // Remove metadata bits so that the store side (vectorized or non-vectorized) can
  // inject the store-good color with an or instruction.
  __ bstrins_d(dst, R0, 15, 0);

  if ((decorators & ARRAYCOPY_CHECKCAST) != 0) {
    __ z_uncolor(dst);
  }
}

void ZBarrierSetAssembler::copy_store_at(MacroAssembler* masm,
                                         DecoratorSet decorators,
                                         BasicType type,
                                         size_t bytes,
                                         Address dst,
                                         Register src,
                                         Register tmp1,
                                         Register tmp2,
                                         Register tmp3) {
  if (!is_reference_type(type)) {
    BarrierSetAssembler::copy_store_at(masm, decorators, type, bytes, dst, src, noreg, noreg, noreg);
    return;
  }

  bool is_dest_uninitialized = (decorators & IS_DEST_UNINITIALIZED) != 0;

  assert_different_registers(src, tmp1, tmp2, tmp3);

  if (!is_dest_uninitialized) {
    Label store, store_bad;
    __ ld_d(tmp3, dst);
    // Test reference against bad mask. If mask bad, then we need to fix it up.
    __ ld_d(tmp1, Address(TREG, ZThreadLocalData::store_bad_mask_offset()));
    __ andr(tmp1, tmp3, tmp1);
    __ beqz(tmp1, store);

    store_barrier_buffer_add(masm, dst, tmp1, tmp2, store_bad);
    __ b(store);

    __ bind(store_bad);
    {
      // Call VM
      ZRuntimeCallSpill rcs(masm, noreg);

      __ lea(A0, dst);

      __ MacroAssembler::call_VM_leaf_base(ZBarrierSetRuntime::store_barrier_on_oop_field_without_healing_addr(), 1);
    }

    __ bind(store);
  }

  if ((decorators & ARRAYCOPY_CHECKCAST) != 0) {
    __ slli_d(src, src, ZPointerLoadShift);
  }

  // Set store-good color, replacing whatever color was there before
  __ ld_d(tmp1, Address(TREG, ZThreadLocalData::store_good_mask_offset()));
  __ bstrins_d(src, tmp1, 15, 0);

  // Store value
  BarrierSetAssembler::copy_store_at(masm, decorators, type, bytes, dst, src, noreg, noreg, noreg);
}

void ZBarrierSetAssembler::copy_load_at(MacroAssembler* masm,
                                        DecoratorSet decorators,
                                        BasicType type,
                                        size_t bytes,
                                        FloatRegister dst,
                                        Address src,
                                        Register tmp1,
                                        Register tmp2,
                                        FloatRegister vec_tmp,
                                        bool need_save_restore) {
  if (!is_reference_type(type)) {
    BarrierSetAssembler::copy_load_at(masm, decorators, type, bytes, dst, src, noreg, noreg, fnoreg);
    return;
  }

  // Load source vector
  BarrierSetAssembler::copy_load_at(masm, decorators, type, bytes, dst, src, noreg, noreg, fnoreg);

  assert_different_registers(dst, vec_tmp);

  Label done, fallback;

  // Test reference against bad mask. If mask bad, then we need to fix it up.
  if (UseLASX) {
    __ xvand_v(vec_tmp, dst, z_copy_load_bad_vreg);
    __ xvsetnez_v(FCC0, vec_tmp);
  } else if (UseLSX) {
    __ vand_v(vec_tmp, dst, z_copy_load_bad_vreg);
    __ vsetnez_v(FCC0, vec_tmp);
  }
  __ movcf2gr(SCR1, FCC0);
  __ bnez(SCR1, fallback);  // vec_tmp not equal 0.0, then goto fallback

  // Remove bad metadata bits so that the store can colour the pointers with an or instruction.
  // This makes the fast path and slow path formats look the same, in the sense that they don't
  // have any of the store bad bits.
  if (UseLASX) {
    __ xvandn_v(dst, z_copy_store_bad_vreg, dst);
  } else if (UseLSX) {
    __ vandn_v(dst, z_copy_store_bad_vreg, dst);
  }
  __ b(done);

  __ bind(fallback);

  Address src0(src.base(), src.disp() + 0);
  Address src1(src.base(), src.disp() + 8);
  Address src2(src.base(), src.disp() + 16);
  Address src3(src.base(), src.disp() + 24);

  if (need_save_restore) {
    __ push_vp(z_arraycopy_saved_vregs - FloatRegSet::of(dst));
  }

  assert_different_registers(tmp1, tmp2);

  if (UseLASX) {
    __ addi_d(SP, SP, - wordSize * 4);

    // The lower 64 bits.
    ZBarrierSetAssembler::copy_load_at(masm, decorators, type, 8, tmp2, src0, tmp1);
    __ st_d(tmp2, SP, 0);

    // The mid-lower 64 bits.
    ZBarrierSetAssembler::copy_load_at(masm, decorators, type, 8, tmp2, src1, tmp1);
    __ st_d(tmp2, SP, 8);

    // The mid-higher 64 bits.
    ZBarrierSetAssembler::copy_load_at(masm, decorators, type, 8, tmp2, src2, tmp1);
    __ st_d(tmp2, SP, 16);

    // The higher 64 bits.
    ZBarrierSetAssembler::copy_load_at(masm, decorators, type, 8, tmp2, src3, tmp1);
    __ st_d(tmp2, SP, 24);

    __ xvld(dst, SP, 0);
    __ addi_d(SP, SP, wordSize * 4);
  } else if (UseLSX) {
    __ addi_d(SP, SP, - wordSize * 2);

    // The lower 64 bits.
    ZBarrierSetAssembler::copy_load_at(masm, decorators, type, 8, tmp2, src0, tmp1);
    __ st_d(tmp2, SP, 0);

    // The higher 64 bits.
    ZBarrierSetAssembler::copy_load_at(masm, decorators, type, 8, tmp2, src1, tmp1);
    __ st_d(tmp2, SP, 8);

    __ vld(dst, SP, 0);
    __ addi_d(SP, SP, wordSize * 2);
  }

  if (need_save_restore) {
    __ pop_vp(z_arraycopy_saved_vregs - FloatRegSet::of(dst));
  }

  __ bind(done);
}

void ZBarrierSetAssembler::copy_store_at(MacroAssembler* masm,
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
  if (!is_reference_type(type)) {
    BarrierSetAssembler::copy_store_at(masm, decorators, type, bytes, dst, src, noreg, noreg, noreg, noreg, fnoreg, fnoreg);
    return;
  }

  bool is_dest_uninitialized = (decorators & IS_DEST_UNINITIALIZED) != 0;

  Label done, fallback;

  if (!is_dest_uninitialized) {
    // Load pre values
    BarrierSetAssembler::copy_load_at(masm, decorators, type, bytes, vec_tmp1, dst, noreg, noreg, fnoreg);

    assert_different_registers(vec_tmp1, vec_tmp2);

    // Test reference against bad mask. If mask bad, then we need to fix it up.
    if (UseLASX) {
      __ xvand_v(vec_tmp2, vec_tmp1, z_copy_store_bad_vreg);
      __ xvsetnez_v(FCC0, vec_tmp2);
    } else if (UseLSX) {
      __ vand_v(vec_tmp2, vec_tmp1, z_copy_store_bad_vreg);
      __ vsetnez_v(FCC0, vec_tmp2);
    }
    __ movcf2gr(SCR1, FCC0);
    __ bnez(SCR1, fallback); // vec_tmp1 not equal 0.0, then goto fallback
  }

  // Color source
  if (UseLASX) {
    __ xvor_v(src, src, z_copy_store_good_vreg);
  } else if (UseLSX) {
    __ vor_v(src, src, z_copy_store_good_vreg);
  }
  // Store colored source in destination
  BarrierSetAssembler::copy_store_at(masm, decorators, type, bytes, dst, src, noreg, noreg, noreg, noreg, fnoreg, fnoreg);
  __ b(done);

  __ bind(fallback);

  Address dst0(dst.base(), dst.disp() + 0);
  Address dst1(dst.base(), dst.disp() + 8);
  Address dst2(dst.base(), dst.disp() + 16);
  Address dst3(dst.base(), dst.disp() + 24);

  if (need_save_restore) {
    __ push_vp(z_arraycopy_saved_vregs - FloatRegSet::of(src));
  }

  assert_different_registers(tmp4, tmp1, tmp2, tmp3);

  if (UseLASX) {
    __ addi_d(SP, SP, - wordSize * 4);
    __ xvst(src, SP, 0);

    // The lower 64 bits.
    __ ld_d(tmp4, SP, 0);
    ZBarrierSetAssembler::copy_store_at(masm, decorators, type, 8, dst0, tmp4, tmp1, tmp2, tmp3);

    // The mid-lower 64 bits.
    __ ld_d(tmp4, SP, 8);
    ZBarrierSetAssembler::copy_store_at(masm, decorators, type, 8, dst1, tmp4, tmp1, tmp2, tmp3);

    // The mid-higher 64 bits.
    __ ld_d(tmp4, SP, 16);
    ZBarrierSetAssembler::copy_store_at(masm, decorators, type, 8, dst2, tmp4, tmp1, tmp2, tmp3);

    // The higher 64 bits.
    __ ld_d(tmp4, SP, 24);
    ZBarrierSetAssembler::copy_store_at(masm, decorators, type, 8, dst3, tmp4, tmp1, tmp2, tmp3);

    __ addi_d(SP, SP, wordSize * 4);
  } else if (UseLSX) {
    // Extract the 2 oops from the src vector register
    __ addi_d(SP, SP, - wordSize * 2);
    __ vst(src, SP, 0);

    // The lower 64 bits.
    __ ld_d(tmp4, SP, 0);
    ZBarrierSetAssembler::copy_store_at(masm, decorators, type, 8, dst0, tmp4, tmp1, tmp2, tmp3);

    // The higher 64 bits.
    __ ld_d(tmp4, SP, 8);
    ZBarrierSetAssembler::copy_store_at(masm, decorators, type, 8, dst1, tmp4, tmp1, tmp2, tmp3);

    __ addi_d(SP, SP, wordSize * 2);
  }

  if (need_save_restore) {
    __ pop_vp(z_arraycopy_saved_vregs - FloatRegSet::of(src));
  }

  __ bind(done);
}

void ZBarrierSetAssembler::try_resolve_jobject_in_native(MacroAssembler* masm,
                                                         Register jni_env,
                                                         Register robj,
                                                         Register tmp,
                                                         Label& slowpath) {
  BLOCK_COMMENT("ZBarrierSetAssembler::try_resolve_jobject_in_native {");

  Label done, tagged, weak_tagged, uncolor;

  // Test for tag
  __ li(tmp, JNIHandles::tag_mask);
  __ andr(tmp, robj, tmp);
  __ bnez(tmp, tagged);

  // Resolve local handle
  __ ld_d(robj, robj, 0);
  __ b(done);

  __ bind(tagged);

  // Test for weak tag
  __ li(tmp, JNIHandles::TypeTag::weak_global);
  __ andr(tmp, robj, tmp);
  __ bnez(tmp, weak_tagged);

  // Resolve global handle
  __ ld_d(robj, Address(robj, -JNIHandles::TypeTag::global));
  __ lea(tmp, load_bad_mask_from_jni_env(jni_env));
  __ ld_d(tmp, tmp, 0);
  __ andr(tmp, robj, tmp);
  __ bnez(tmp, slowpath);
  __ b(uncolor);

  __ bind(weak_tagged);

  // Resolve weak handle
  __ ld_d(robj, Address(robj, -JNIHandles::TypeTag::weak_global));
  __ lea(tmp, mark_bad_mask_from_jni_env(jni_env));
  __ ld_d(tmp, tmp, 0);
  __ andr(tmp, robj, tmp);
  __ bnez(tmp, slowpath);

  __ bind(uncolor);

  // Uncolor
  __ z_uncolor(robj);

  __ bind(done);

  BLOCK_COMMENT("} ZBarrierSetAssembler::try_resolve_jobject_in_native");
}

static uint16_t patch_barrier_relocation_value(int format) {
  switch (format) {
  case ZBarrierRelocationFormatLoadBadMask:
    return (uint16_t)ZPointerLoadBadMask;
  case ZBarrierRelocationFormatMarkBadMask:
    return (uint16_t)ZPointerMarkBadMask;
  case ZBarrierRelocationFormatStoreGoodBits:
    return (uint16_t)ZPointerStoreGoodMask;
  case ZBarrierRelocationFormatStoreBadMask:
    return (uint16_t)ZPointerStoreBadMask;
  default:
    ShouldNotReachHere();
    return 0;
  }
}

void ZBarrierSetAssembler::patch_barrier_relocation(address addr, int format) {
  int inst = *(int*)addr;
  int size = 2 * BytesPerInstWord;
  CodeBuffer cb(addr, size);
  MacroAssembler masm(&cb);
  masm.patchable_li16(as_Register(inst & 0x1f), patch_barrier_relocation_value(format));
  ICache::invalidate_range(addr, size);
}

#ifdef COMPILER1

#undef __
#define __ ce->masm()->

void ZBarrierSetAssembler::generate_c1_uncolor(LIR_Assembler* ce, LIR_Opr ref) const {
  __ z_uncolor(ref->as_register());
}

void ZBarrierSetAssembler::generate_c1_color(LIR_Assembler* ce, LIR_Opr ref) const {
  __ z_color(ref->as_register(), ref->as_register(), SCR1);
}

void ZBarrierSetAssembler::generate_c1_load_barrier(LIR_Assembler* ce,
                                                    LIR_Opr ref,
                                                    ZLoadBarrierStubC1* stub,
                                                    bool on_non_strong) const {
  Label good;
  __ check_color(ref->as_register(), SCR1, on_non_strong);
  __ beqz(SCR1, good);
  __ b(*stub->entry());

  __ bind(good);
  __ z_uncolor(ref->as_register());
  __ bind(*stub->continuation());
}

void ZBarrierSetAssembler::generate_c1_load_barrier_stub(LIR_Assembler* ce,
                                                         ZLoadBarrierStubC1* stub) const {
  // Stub entry
  __ bind(*stub->entry());

  Register ref = stub->ref()->as_register();
  Register ref_addr = noreg;
  Register tmp = noreg;

  if (stub->tmp()->is_valid()) {
    // Load address into tmp register
    ce->leal(stub->ref_addr(), stub->tmp());
    ref_addr = tmp = stub->tmp()->as_pointer_register();
  } else {
    // Address already in register
    ref_addr = stub->ref_addr()->as_address_ptr()->base()->as_pointer_register();
  }

  assert_different_registers(ref, ref_addr, noreg);

  // Save V0 unless it is the result or tmp register
  // Set up SP to accommodate parameters and maybe V0.
  if (ref != V0 && tmp != V0) {
    __ addi_d(SP, SP, -32);
    __ st_d(V0, SP, 16);
  } else {
    __ addi_d(SP, SP, -16);
  }

  // Setup arguments and call runtime stub
  ce->store_parameter(ref_addr, 1);
  ce->store_parameter(ref, 0);

  __ call(stub->runtime_stub(), relocInfo::runtime_call_type);

  // Verify result
  __ verify_oop(V0);

  // Move result into place
  if (ref != V0) {
    __ move(ref, V0);
  }

  // Restore V0 unless it is the result or tmp register
  if (ref != V0 && tmp != V0) {
    __ ld_d(V0, SP, 16);
    __ addi_d(SP, SP, 32);
  } else {
    __ addi_d(SP, SP, 16);
  }

  // Stub exit
  __ b(*stub->continuation());
}

void ZBarrierSetAssembler::generate_c1_store_barrier(LIR_Assembler* ce,
                                                     LIR_Address* addr,
                                                     LIR_Opr new_zaddress,
                                                     LIR_Opr new_zpointer,
                                                     ZStoreBarrierStubC1* stub) const {
  Register rnew_zaddress = new_zaddress->as_register();
  Register rnew_zpointer = new_zpointer->as_register();

  store_barrier_fast(ce->masm(),
                     ce->as_Address(addr),
                     rnew_zaddress,
                     rnew_zpointer,
                     SCR2,
                     true,
                     stub->is_atomic(),
                     *stub->entry(),
                     *stub->continuation());
}

void ZBarrierSetAssembler::generate_c1_store_barrier_stub(LIR_Assembler* ce,
                                                          ZStoreBarrierStubC1* stub) const {
  // Stub entry
  __ bind(*stub->entry());
  Label slow;
  Label slow_continuation;
  store_barrier_medium(ce->masm(),
                       ce->as_Address(stub->ref_addr()->as_address_ptr()),
                       SCR2,
                       stub->new_zpointer()->as_register(),
                       stub->tmp()->as_pointer_register(),
                       false /* is_native */,
                       stub->is_atomic(),
                       *stub->continuation(),
                       slow,
                       slow_continuation);

  __ bind(slow);

  __ lea(stub->new_zpointer()->as_register(), ce->as_Address(stub->ref_addr()->as_address_ptr()));

  __ addi_d(SP, SP, -16);
  // Setup arguments and call runtime stub
  assert(stub->new_zpointer()->is_valid(), "invariant");
  ce->store_parameter(stub->new_zpointer()->as_register(), 0);
  __ call(stub->runtime_stub(), relocInfo::runtime_call_type);
  __ addi_d(SP, SP, 16);

  // Stub exit
  __ b(slow_continuation);
}

#undef __
#define __ sasm->

void ZBarrierSetAssembler::generate_c1_load_barrier_runtime_stub(StubAssembler* sasm,
                                                                 DecoratorSet decorators) const {
  __ prologue("zgc_load_barrier stub", false);

  __ push_call_clobbered_registers_except(RegSet::of(V0));

  // Setup arguments
  __ load_parameter(0, A0);
  __ load_parameter(1, A1);

  __ call_VM_leaf(ZBarrierSetRuntime::load_barrier_on_oop_field_preloaded_addr(decorators), 2);

  __ pop_call_clobbered_registers_except(RegSet::of(V0));

  __ epilogue();
}

void ZBarrierSetAssembler::generate_c1_store_barrier_runtime_stub(StubAssembler* sasm,
                                                                  bool self_healing) const {
  __ prologue("zgc_store_barrier stub", false);

  __ push_call_clobbered_registers();

  // Setup arguments
  __ load_parameter(0, c_rarg0);

  if (self_healing) {
    __ call_VM_leaf(ZBarrierSetRuntime::store_barrier_on_oop_field_with_healing_addr(), 1);
  } else {
    __ call_VM_leaf(ZBarrierSetRuntime::store_barrier_on_oop_field_without_healing_addr(), 1);
  }

  __ pop_call_clobbered_registers();

  __ epilogue();
}

#endif // COMPILER1

#ifdef COMPILER2

OptoReg::Name ZBarrierSetAssembler::refine_register(const Node* node, OptoReg::Name opto_reg) {
  if (!OptoReg::is_reg(opto_reg)) {
    return OptoReg::Bad;
  }

  const VMReg vm_reg = OptoReg::as_VMReg(opto_reg);
  if (vm_reg->is_FloatRegister()) {
    return opto_reg & ~1;
  }

  return opto_reg;
}

#undef __
#define __ _masm->

class ZSaveLiveRegisters {
private:
  MacroAssembler* const _masm;
  RegSet                _gp_regs;
  FloatRegSet           _fp_regs;
  FloatRegSet           _lsx_vp_regs;
  FloatRegSet           _lasx_vp_regs;

public:
  void initialize(ZBarrierStubC2* stub) {
    // Record registers that needs to be saved/restored
    RegMaskIterator rmi(stub->live());
    while (rmi.has_next()) {
      const OptoReg::Name opto_reg = rmi.next();
      if (OptoReg::is_reg(opto_reg)) {
        const VMReg vm_reg = OptoReg::as_VMReg(opto_reg);
        if (vm_reg->is_Register()) {
          _gp_regs += RegSet::of(vm_reg->as_Register());
        } else if (vm_reg->is_FloatRegister()) {
          if (UseLASX && vm_reg->next(7))
            _lasx_vp_regs += FloatRegSet::of(vm_reg->as_FloatRegister());
          else if (UseLSX && vm_reg->next(3))
            _lsx_vp_regs += FloatRegSet::of(vm_reg->as_FloatRegister());
          else
            _fp_regs += FloatRegSet::of(vm_reg->as_FloatRegister());
        } else {
          fatal("Unknown register type");
        }
      }
    }

    // Remove C-ABI SOE registers, scratch regs and _ref register that will be updated
    if (stub->result() != noreg) {
      _gp_regs -= RegSet::range(S0, S7) + RegSet::of(SP, SCR1, SCR2, stub->result());
    } else {
      _gp_regs -= RegSet::range(S0, S7) + RegSet::of(SP, SCR1, SCR2);
    }
  }

  ZSaveLiveRegisters(MacroAssembler* masm, ZBarrierStubC2* stub) :
      _masm(masm),
      _gp_regs(),
      _fp_regs(),
      _lsx_vp_regs(),
      _lasx_vp_regs() {

    // Figure out what registers to save/restore
    initialize(stub);

    // Save registers
    __ push(_gp_regs);
    __ push_fpu(_fp_regs);
    __ push_vp(_lsx_vp_regs  /* UseLSX  */);
    __ push_vp(_lasx_vp_regs /* UseLASX */);
  }

  ~ZSaveLiveRegisters() {
    // Restore registers
    __ pop_vp(_lasx_vp_regs /* UseLASX */);
    __ pop_vp(_lsx_vp_regs  /* UseLSX  */);
    __ pop_fpu(_fp_regs);
    __ pop(_gp_regs);
  }
};

#undef __
#define __ _masm->

class ZSetupArguments {
private:
  MacroAssembler* const _masm;
  const Register        _ref;
  const Address         _ref_addr;

public:
  ZSetupArguments(MacroAssembler* masm, ZLoadBarrierStubC2* stub) :
      _masm(masm),
      _ref(stub->ref()),
      _ref_addr(stub->ref_addr()) {

    // Setup arguments
    if (_ref_addr.base() == noreg) {
      // No self healing
      if (_ref != A0) {
        __ move(A0, _ref);
      }
      __ move(A1, R0);
    } else {
      // Self healing
      if (_ref == A0) {
        // _ref is already at correct place
        __ lea(A1, _ref_addr);
      } else if (_ref != A1) {
        // _ref is in wrong place, but not in A1, so fix it first
        __ lea(A1, _ref_addr);
        __ move(A0, _ref);
      } else if (_ref_addr.base() != A0 && _ref_addr.index() != A0) {
        assert(_ref == A1, "Mov ref first, vacating A0");
        __ move(A0, _ref);
        __ lea(A1, _ref_addr);
      } else {
        assert(_ref == A1, "Need to vacate A1 and _ref_addr is using A0");
        if (_ref_addr.base() == A0 || _ref_addr.index() == A0) {
          __ move(SCR2, A1);
          __ lea(A1, _ref_addr);
          __ move(A0, SCR2);
        } else {
          ShouldNotReachHere();
        }
      }
    }
  }

  ~ZSetupArguments() {
    // Transfer result
    if (_ref != V0) {
      __ move(_ref, V0);
    }
  }
};

#undef __
#define __ masm->

void ZBarrierSetAssembler::generate_c2_load_barrier_stub(MacroAssembler* masm, ZLoadBarrierStubC2* stub) const {
  BLOCK_COMMENT("ZLoadBarrierStubC2");

  // Stub entry
  if (!Compile::current()->output()->in_scratch_emit_size()) {
    __ bind(*stub->entry());
  }

  {
    ZSaveLiveRegisters save_live_registers(masm, stub);
    ZSetupArguments setup_arguments(masm, stub);
    __ MacroAssembler::call_VM_leaf_base(stub->slow_path(), 2);
  }
  // Stub exit
  __ b(*stub->continuation());
}

void ZBarrierSetAssembler::generate_c2_store_barrier_stub(MacroAssembler* masm, ZStoreBarrierStubC2* stub) const {
  BLOCK_COMMENT("ZStoreBarrierStubC2");

  // Stub entry
  __ bind(*stub->entry());

  Label slow;
  Label slow_continuation;
  store_barrier_medium(masm,
                       stub->ref_addr(),
                       stub->new_zpointer(),
                       SCR2,
                       SCR1,
                       stub->is_native(),
                       stub->is_atomic(),
                       *stub->continuation(),
                       slow,
                       slow_continuation);

  __ bind(slow);

  {
    ZSaveLiveRegisters save_live_registers(masm, stub);
    __ lea(A0, stub->ref_addr());

    if (stub->is_native()) {
      __ MacroAssembler::call_VM_leaf_base(ZBarrierSetRuntime::store_barrier_on_native_oop_field_without_healing_addr(), 1);
    } else if (stub->is_atomic()) {
      __ MacroAssembler::call_VM_leaf_base(ZBarrierSetRuntime::store_barrier_on_oop_field_with_healing_addr(), 1);
    } else {
      __ MacroAssembler::call_VM_leaf_base(ZBarrierSetRuntime::store_barrier_on_oop_field_without_healing_addr(), 1);
    }
  }

  // Stub exit
  __ b(slow_continuation);
}

#endif // COMPILER2
