/*
 * Copyright (c) 2022, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2023, Loongson Technology. All rights reserved.
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
#include "macroAssembler_loongarch.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/stubRoutines.hpp"

/**
 * Perform the quarter round calculations on values contained within
 * four SIMD registers.
 *
 * @param aVec the SIMD register containing only the "a" values
 * @param bVec the SIMD register containing only the "b" values
 * @param cVec the SIMD register containing only the "c" values
 * @param dVec the SIMD register containing only the "d" values
 */
void MacroAssembler::cc20_quarter_round(FloatRegister aVec, FloatRegister bVec,
    FloatRegister cVec, FloatRegister dVec) {

  // a += b, d ^= a, d <<<= 16
  vadd_w(aVec, aVec, bVec);
  vxor_v(dVec, dVec, aVec);
  vrotri_w(dVec, dVec, 16);

  // c += d, b ^= c, b <<<= 12
  vadd_w(cVec, cVec, dVec);
  vxor_v(bVec, bVec, cVec);
  vrotri_w(bVec, bVec, 20);

  // a += b, d ^= a, d <<<= 8
  vadd_w(aVec, aVec, bVec);
  vxor_v(dVec, dVec, aVec);
  vrotri_w(dVec, dVec, 24);

  // c += d, b ^= c, b <<<= 7
  vadd_w(cVec, cVec, dVec);
  vxor_v(bVec, bVec, cVec);
  vrotri_w(bVec, bVec, 25);
}

/**
 * Shift the b, c, and d vectors between columnar and diagonal representations.
 * Note that the "a" vector does not shift.
 *
 * @param bVec the SIMD register containing only the "b" values
 * @param cVec the SIMD register containing only the "c" values
 * @param dVec the SIMD register containing only the "d" values
 * @param colToDiag true if moving columnar to diagonal, false if
 *                  moving diagonal back to columnar.
 */
void MacroAssembler::cc20_shift_lane_org(FloatRegister bVec, FloatRegister cVec,
    FloatRegister dVec, bool colToDiag) {
  int bShift = colToDiag ? 0b00111001 : 0b10010011;
  int cShift = 0b01001110;
  int dShift = colToDiag ? 0b10010011 : 0b00111001;

  vshuf4i_w(bVec, bVec, bShift);
  vshuf4i_w(cVec, cVec, cShift);
  vshuf4i_w(dVec, dVec, dShift);
}
