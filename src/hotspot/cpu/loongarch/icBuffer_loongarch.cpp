/*
 * Copyright (c) 1997, 2012, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2015, 2022, Loongson Technology. All rights reserved.
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
#include "code/icBuffer.hpp"
#include "gc/shared/collectedHeap.inline.hpp"
#include "interpreter/bytecodes.hpp"
#include "memory/resourceArea.hpp"
#include "nativeInst_loongarch.hpp"
#include "oops/oop.inline.hpp"


int InlineCacheBuffer::ic_stub_code_size() {
  return NativeMovConstReg::instruction_size +  // patchable_li52() == 3 ins
         NativeGeneralJump::instruction_size;   // patchable_jump() == 2 ins
}


// The use IC_Klass refer to SharedRuntime::gen_i2c2i_adapters
void InlineCacheBuffer::assemble_ic_buffer_code(address code_begin,
                                                void* cached_value,
                                                address entry_point) {
  ResourceMark rm;
  CodeBuffer code(code_begin, ic_stub_code_size());
  MacroAssembler* masm = new MacroAssembler(&code);
  // Note: even though the code contains an embedded value, we do not need reloc info
  // because
  // (1) the value is old (i.e., doesn't matter for scavenges)
  // (2) these ICStubs are removed *before* a GC happens, so the roots disappear

#define __ masm->
  address start = __ pc();
  __ patchable_li52(IC_Klass, (long)cached_value);
  __ jmp(entry_point, relocInfo::runtime_call_type);

  ICache::invalidate_range(code_begin, InlineCacheBuffer::ic_stub_code_size());
  assert(__ pc() - start == ic_stub_code_size(), "must be");
#undef __
}


address InlineCacheBuffer::ic_buffer_entry_point(address code_begin) {
  // move -> jump -> entry
  NativeMovConstReg* move = nativeMovConstReg_at(code_begin);
  NativeGeneralJump* jump = nativeGeneralJump_at(move->next_instruction_address());
  return jump->jump_destination();
}


void* InlineCacheBuffer::ic_buffer_cached_value(address code_begin) {
  // double check the instructions flow
  NativeMovConstReg* move = nativeMovConstReg_at(code_begin);
  NativeGeneralJump* jump = nativeGeneralJump_at(move->next_instruction_address());

  // cached value is the data arg of NativeMovConstReg
  void* cached_value = (void*)move->data();
  return cached_value;
}
