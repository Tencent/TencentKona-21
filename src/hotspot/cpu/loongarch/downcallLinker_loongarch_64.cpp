/*
 * Copyright (c) 2020, Red Hat, Inc. All rights reserved.
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
#include "asm/macroAssembler.hpp"
#include "code/codeBlob.hpp"
#include "code/codeCache.hpp"
#include "code/vmreg.inline.hpp"
#include "compiler/oopMap.hpp"
#include "logging/logStream.hpp"
#include "memory/resourceArea.hpp"
#include "prims/downcallLinker.hpp"
#include "runtime/globals.hpp"
#include "runtime/stubCodeGenerator.hpp"

#define __ _masm->

class DowncallStubGenerator : public StubCodeGenerator {
  BasicType* _signature;
  int _num_args;
  BasicType _ret_bt;
  const ABIDescriptor& _abi;

  const GrowableArray<VMStorage>& _input_registers;
  const GrowableArray<VMStorage>& _output_registers;

  bool _needs_return_buffer;
  int _captured_state_mask;
  bool _needs_transition;

  int _frame_complete;
  int _frame_size_slots;
  OopMapSet* _oop_maps;
public:
  DowncallStubGenerator(CodeBuffer* buffer,
                         BasicType* signature,
                         int num_args,
                         BasicType ret_bt,
                         const ABIDescriptor& abi,
                         const GrowableArray<VMStorage>& input_registers,
                         const GrowableArray<VMStorage>& output_registers,
                         bool needs_return_buffer,
                         int captured_state_mask,
                         bool needs_transition)
   : StubCodeGenerator(buffer, PrintMethodHandleStubs),
     _signature(signature),
     _num_args(num_args),
     _ret_bt(ret_bt),
     _abi(abi),
     _input_registers(input_registers),
     _output_registers(output_registers),
     _needs_return_buffer(needs_return_buffer),
     _captured_state_mask(captured_state_mask),
     _needs_transition(needs_transition),
     _frame_complete(0),
     _frame_size_slots(0),
     _oop_maps(nullptr) {
  }

  void generate();

  int frame_complete() const {
    return _frame_complete;
  }

  int framesize() const {
    return (_frame_size_slots >> (LogBytesPerWord - LogBytesPerInt));
  }

  OopMapSet* oop_maps() const {
    return _oop_maps;
  }
};

static const int native_invoker_code_base_size = 256;
static const int native_invoker_size_per_arg = 8;

RuntimeStub* DowncallLinker::make_downcall_stub(BasicType* signature,
                                                int num_args,
                                                BasicType ret_bt,
                                                const ABIDescriptor& abi,
                                                const GrowableArray<VMStorage>& input_registers,
                                                const GrowableArray<VMStorage>& output_registers,
                                                bool needs_return_buffer,
                                                int captured_state_mask,
                                                bool needs_transition) {
  int code_size = native_invoker_code_base_size + (num_args * native_invoker_size_per_arg);
  int locs_size = 1; // must be non-zero
  CodeBuffer code("nep_invoker_blob", code_size, locs_size);
  DowncallStubGenerator g(&code, signature, num_args, ret_bt, abi,
                          input_registers, output_registers,
                          needs_return_buffer, captured_state_mask,
                          needs_transition);
  g.generate();
  code.log_section_sizes("nep_invoker_blob");

  RuntimeStub* stub =
    RuntimeStub::new_runtime_stub("nep_invoker_blob",
                                  &code,
                                  g.frame_complete(),
                                  g.framesize(),
                                  g.oop_maps(), false);

#ifndef PRODUCT
  LogTarget(Trace, foreign, downcall) lt;
  if (lt.is_enabled()) {
    ResourceMark rm;
    LogStream ls(lt);
    stub->print_on(&ls);
  }
#endif

  return stub;
}

void DowncallStubGenerator::generate() {
  enum layout {
    fp_off,
    fp_off2,
    return_off,
    return_off2,
    framesize // inclusive of return address
    // The following are also computed dynamically:
    // spill area for return value
    // out arg area (e.g. for stack args)
  };

  Register tmp1 = SCR1;
  Register tmp2 = SCR2;

  VMStorage shuffle_reg = as_VMStorage(S0);
  JavaCallingConvention in_conv;
  NativeCallingConvention out_conv(_input_registers);
  ArgumentShuffle arg_shuffle(_signature, _num_args, _signature, _num_args, &in_conv, &out_conv, shuffle_reg);

#ifndef PRODUCT
  LogTarget(Trace, foreign, downcall) lt;
  if (lt.is_enabled()) {
    ResourceMark rm;
    LogStream ls(lt);
    arg_shuffle.print_on(&ls);
  }
#endif

  int allocated_frame_size = 0;
  assert(_abi._shadow_space_bytes == 0, "not expecting shadow space on LoongArch64");
  allocated_frame_size += arg_shuffle.out_arg_bytes();

  bool should_save_return_value = !_needs_return_buffer;
  RegSpiller out_reg_spiller(_output_registers);
  int spill_offset = -1;

  if (should_save_return_value) {
    spill_offset = 0;
    // spill area can be shared with shadow space and out args,
    // since they are only used before the call,
    // and spill area is only used after.
    allocated_frame_size = out_reg_spiller.spill_size_bytes() > allocated_frame_size
      ? out_reg_spiller.spill_size_bytes()
      : allocated_frame_size;
  }

  StubLocations locs;
  locs.set(StubLocations::TARGET_ADDRESS, _abi._scratch1);
  if (_needs_return_buffer) {
    locs.set_frame_data(StubLocations::RETURN_BUFFER, allocated_frame_size);
    allocated_frame_size += BytesPerWord; // for address spill
  }
  if (_captured_state_mask != 0) {
    locs.set_frame_data(StubLocations::CAPTURED_STATE_BUFFER, allocated_frame_size);
    allocated_frame_size += BytesPerWord;
  }

  _frame_size_slots = align_up(framesize + (allocated_frame_size >> LogBytesPerInt), 4);
  assert(is_even(_frame_size_slots/2), "sp not 16-byte aligned");

  _oop_maps  = _needs_transition ? new OopMapSet() : nullptr;
  address start = __ pc();

  __ enter();

  // RA and FP are already in place
  __ addi_d(SP, SP, -((unsigned)_frame_size_slots-4) << LogBytesPerInt); // prolog

  _frame_complete = __ pc() - start;

  if (_needs_transition) {
    Label L;
    address the_pc = __ pc();
    __ bind(L);
    __ set_last_Java_frame(TREG, SP, FP, L);
    OopMap* map = new OopMap(_frame_size_slots, 0);
    _oop_maps->add_gc_map(the_pc - start, map);

    // State transition
    __ li(tmp1, _thread_in_native);
    if (os::is_MP()) {
      __ addi_d(tmp2, TREG, in_bytes(JavaThread::thread_state_offset()));
      __ amswap_db_w(R0, tmp1, tmp2);
    } else {
      __ st_w(tmp1, TREG, in_bytes(JavaThread::thread_state_offset()));
    }
  }

  __ block_comment("{ argument shuffle");
  arg_shuffle.generate(_masm, shuffle_reg, 0, _abi._shadow_space_bytes, locs);
  __ block_comment("} argument shuffle");

  __ jalr(as_Register(locs.get(StubLocations::TARGET_ADDRESS)));
  // this call is assumed not to have killed rthread

  if (_needs_return_buffer) {
    __ ld_d(tmp1, SP, locs.data_offset(StubLocations::RETURN_BUFFER));
    int offset = 0;
    for (int i = 0; i < _output_registers.length(); i++) {
      VMStorage reg = _output_registers.at(i);
      if (reg.type() == StorageType::INTEGER) {
        __ st_d(as_Register(reg), tmp1, offset);
        offset += 8;
      } else if (reg.type() == StorageType::FLOAT) {
        __ fst_d(as_FloatRegister(reg), tmp1, offset);
        offset += 8;
      } else {
        ShouldNotReachHere();
      }
    }
  }

  //////////////////////////////////////////////////////////////////////////////

  if (_captured_state_mask != 0) {
    __ block_comment("{ save thread local");

    if (should_save_return_value) {
      out_reg_spiller.generate_spill(_masm, spill_offset);
    }

    __ ld_d(c_rarg0, Address(SP, locs.data_offset(StubLocations::CAPTURED_STATE_BUFFER)));
    __ li(c_rarg1, _captured_state_mask);
    __ call(CAST_FROM_FN_PTR(address, DowncallLinker::capture_state), relocInfo::runtime_call_type);

    if (should_save_return_value) {
      out_reg_spiller.generate_fill(_masm, spill_offset);
    }

    __ block_comment("} save thread local");
  }

  //////////////////////////////////////////////////////////////////////////////

  Label L_after_safepoint_poll;
  Label L_safepoint_poll_slow_path;
  Label L_reguard;
  Label L_after_reguard;
  if (_needs_transition) {
    __ li(tmp1, _thread_in_native_trans);

    // Force this write out before the read below
    if (os::is_MP() && !UseSystemMemoryBarrier) {
      __ addi_d(tmp2, TREG, in_bytes(JavaThread::thread_state_offset()));
      __ amswap_db_w(R0, tmp1, tmp2); // AnyAny
    } else {
      __ st_w(tmp1, TREG, in_bytes(JavaThread::thread_state_offset()));
    }

    __ safepoint_poll(L_safepoint_poll_slow_path, TREG, true /* at_return */, true /* acquire */, false /* in_nmethod */);

    __ ld_w(tmp1, TREG, in_bytes(JavaThread::suspend_flags_offset()));
    __ bnez(tmp1, L_safepoint_poll_slow_path);

    __ bind(L_after_safepoint_poll);

    // change thread state
    __ li(tmp1, _thread_in_Java);
    if (os::is_MP()) {
      __ addi_d(tmp2, TREG, in_bytes(JavaThread::thread_state_offset()));
      __ amswap_db_w(R0, tmp1, tmp2);
    } else {
      __ st_w(tmp1, TREG, in_bytes(JavaThread::thread_state_offset()));
    }

    __ block_comment("reguard stack check");
    __ ld_w(tmp1, TREG, in_bytes(JavaThread::stack_guard_state_offset()));
    __ addi_d(tmp1, tmp1, -StackOverflow::stack_guard_yellow_reserved_disabled);
    __ beqz(tmp1, L_reguard);
    __ bind(L_after_reguard);

    __ reset_last_Java_frame(true);
  }

  __ leave(); // required for proper stackwalking of RuntimeStub frame
  __ jr(RA);

  //////////////////////////////////////////////////////////////////////////////

  if (_needs_transition) {
    __ block_comment("{ L_safepoint_poll_slow_path");
    __ bind(L_safepoint_poll_slow_path);

    if (should_save_return_value) {
      // Need to save the native result registers around any runtime calls.
      out_reg_spiller.generate_spill(_masm, spill_offset);
    }

    __ move(c_rarg0, TREG);
    assert(frame::arg_reg_save_area_bytes == 0, "not expecting frame reg save area");
    __ call(CAST_FROM_FN_PTR(address, JavaThread::check_special_condition_for_native_trans), relocInfo::runtime_call_type);

    if (should_save_return_value) {
      out_reg_spiller.generate_fill(_masm, spill_offset);
    }

    __ b(L_after_safepoint_poll);
    __ block_comment("} L_safepoint_poll_slow_path");

  //////////////////////////////////////////////////////////////////////////////

    __ block_comment("{ L_reguard");
    __ bind(L_reguard);

    if (should_save_return_value) {
      out_reg_spiller.generate_spill(_masm, spill_offset);
    }

    __ call(CAST_FROM_FN_PTR(address, SharedRuntime::reguard_yellow_pages),relocInfo::runtime_call_type);

    if (should_save_return_value) {
      out_reg_spiller.generate_fill(_masm, spill_offset);
    }

    __ b(L_after_reguard);

    __ block_comment("} L_reguard");
  }

  //////////////////////////////////////////////////////////////////////////////

  __ flush();
}
