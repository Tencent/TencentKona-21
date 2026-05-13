/*
 * Copyright (c) 1997, 2013, Oracle and/or its affiliates. All rights reserved.
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

#ifndef CPU_LOONGARCH_MACROASSEMBLER_LOONGARCH_INLINE_HPP
#define CPU_LOONGARCH_MACROASSEMBLER_LOONGARCH_INLINE_HPP

#include "asm/assembler.inline.hpp"
#include "asm/macroAssembler.hpp"
#include "asm/codeBuffer.hpp"
#include "code/codeCache.hpp"

inline void MacroAssembler::tiny_fill_0_24(Register to, Register value) {
    // 0:
    jr(RA);
    nop();
    nop();
    nop();

    // 1:
    st_b(value, to, 0);
    jr(RA);
    nop();
    nop();

    // 2:
    st_h(value, to, 0);
    jr(RA);
    nop();
    nop();

    // 3:
    st_h(value, to, 0);
    st_b(value, to, 2);
    jr(RA);
    nop();

    // 4:
    st_w(value, to, 0);
    jr(RA);
    nop();
    nop();

    // 5:
    st_w(value, to, 0);
    st_b(value, to, 4);
    jr(RA);
    nop();

    // 6:
    st_w(value, to, 0);
    st_h(value, to, 4);
    jr(RA);
    nop();

    // 7:
    st_w(value, to, 0);
    st_w(value, to, 3);
    jr(RA);
    nop();

    // 8:
    st_d(value, to, 0);
    jr(RA);
    nop();
    nop();

    // 9:
    st_d(value, to, 0);
    st_b(value, to, 8);
    jr(RA);
    nop();

    // 10:
    st_d(value, to, 0);
    st_h(value, to, 8);
    jr(RA);
    nop();

    // 11:
    st_d(value, to, 0);
    st_w(value, to, 7);
    jr(RA);
    nop();

    // 12:
    st_d(value, to, 0);
    st_w(value, to, 8);
    jr(RA);
    nop();

    // 13:
    st_d(value, to, 0);
    st_d(value, to, 5);
    jr(RA);
    nop();

    // 14:
    st_d(value, to, 0);
    st_d(value, to, 6);
    jr(RA);
    nop();

    // 15:
    st_d(value, to, 0);
    st_d(value, to, 7);
    jr(RA);
    nop();

    // 16:
    st_d(value, to, 0);
    st_d(value, to, 8);
    jr(RA);
    nop();

    // 17:
    st_d(value, to, 0);
    st_d(value, to, 8);
    st_b(value, to, 16);
    jr(RA);

    // 18:
    st_d(value, to, 0);
    st_d(value, to, 8);
    st_h(value, to, 16);
    jr(RA);

    // 19:
    st_d(value, to, 0);
    st_d(value, to, 8);
    st_w(value, to, 15);
    jr(RA);

    // 20:
    st_d(value, to, 0);
    st_d(value, to, 8);
    st_w(value, to, 16);
    jr(RA);

    // 21:
    st_d(value, to, 0);
    st_d(value, to, 8);
    st_d(value, to, 13);
    jr(RA);

    // 22:
    st_d(value, to, 0);
    st_d(value, to, 8);
    st_d(value, to, 14);
    jr(RA);

    // 23:
    st_d(value, to, 0);
    st_d(value, to, 8);
    st_d(value, to, 15);
    jr(RA);

    // 24:
    st_d(value, to, 0);
    st_d(value, to, 8);
    st_d(value, to, 16);
    jr(RA);
}

inline void MacroAssembler::array_fill(BasicType t, Register to,
                                       Register value, Register count,
                                       bool aligned) {
    assert_different_registers(to, value, count, SCR1);

    Label L_small;

    int shift = -1;
    switch (t) {
      case T_BYTE:
        shift = 0;
        slti(SCR1, count, 25);
        bstrins_d(value, value, 15, 8);  //  8 bit -> 16 bit
        bstrins_d(value, value, 31, 16); // 16 bit -> 32 bit
        bstrins_d(value, value, 63, 32); // 32 bit -> 64 bit
        bnez(SCR1, L_small);
        // count denotes the end, in bytes
        add_d(count, to, count);
        break;
      case T_SHORT:
        shift = 1;
        slti(SCR1, count, 13);
        bstrins_d(value, value, 31, 16); // 16 bit -> 32 bit
        bstrins_d(value, value, 63, 32); // 32 bit -> 64 bit
        bnez(SCR1, L_small);
        // count denotes the end, in bytes
        alsl_d(count, count, to, shift - 1);
        break;
      case T_INT:
        shift = 2;
        slti(SCR1, count, 7);
        bstrins_d(value, value, 63, 32); // 32 bit -> 64 bit
        bnez(SCR1, L_small);
        // count denotes the end, in bytes
        alsl_d(count, count, to, shift - 1);
        break;
      case T_LONG:
        shift = 3;
        slti(SCR1, count, 4);
        bnez(SCR1, L_small);
        // count denotes the end, in bytes
        alsl_d(count, count, to, shift - 1);
        break;
      default: ShouldNotReachHere();
    }

    // nature aligned for store
    if (!aligned) {
      st_d(value, to,  0);
      bstrins_d(to, R0, 2, 0);
      addi_d(to, to, 8);
    }

    // fill large chunks
    Label L_loop64, L_lt64, L_lt32, L_lt16, L_lt8;

    addi_d(SCR1, count, -64);
    blt(SCR1, to, L_lt64);

    bind(L_loop64);
    st_d(value, to, 0);
    st_d(value, to, 8);
    st_d(value, to, 16);
    st_d(value, to, 24);
    st_d(value, to, 32);
    st_d(value, to, 40);
    st_d(value, to, 48);
    st_d(value, to, 56);
    addi_d(to, to, 64);
    bge(SCR1, to, L_loop64);

    bind(L_lt64);
    addi_d(SCR1, count, -32);
    blt(SCR1, to, L_lt32);
    st_d(value, to, 0);
    st_d(value, to, 8);
    st_d(value, to, 16);
    st_d(value, to, 24);
    addi_d(to, to, 32);

    bind(L_lt32);
    addi_d(SCR1, count, -16);
    blt(SCR1, to, L_lt16);
    st_d(value, to, 0);
    st_d(value, to, 8);
    addi_d(to, to, 16);

    bind(L_lt16);
    addi_d(SCR1, count, -8);
    blt(SCR1, to, L_lt8);
    st_d(value, to, 0);

    bind(L_lt8);
    st_d(value, count, -8);

    jr(RA);

    // Short arrays (<= 24 bytes)
    bind(L_small);
    pcaddi(SCR1, 4);
    slli_d(count, count, 4 + shift);
    add_d(SCR1, SCR1, count);
    jr(SCR1);

    tiny_fill_0_24(to, value);
}

inline void MacroAssembler::array_fill_lsx(BasicType t, Register to,
                                           Register value, Register count) {
    assert(UseLSX, "should be");
    assert_different_registers(to, value, count, SCR1);

    Label L_small;

    int shift = -1;
    switch (t) {
      case T_BYTE:
        shift = 0;
        slti(SCR1, count, 49);
        vreplgr2vr_b(fscratch, value); // 8 bit -> 128 bit
        movfr2gr_d(value, fscratch);
        bnez(SCR1, L_small);
        // count denotes the end, in bytes
        add_d(count, to, count);
        break;
      case T_SHORT:
        shift = 1;
        slti(SCR1, count, 25);
        vreplgr2vr_h(fscratch, value); // 16 bit -> 128 bit
        movfr2gr_d(value, fscratch);
        bnez(SCR1, L_small);
        // count denotes the end, in bytes
        alsl_d(count, count, to, shift - 1);
        break;
      case T_INT:
        shift = 2;
        slti(SCR1, count, 13);
        vreplgr2vr_w(fscratch, value); // 32 bit -> 128 bit
        movfr2gr_d(value, fscratch);
        bnez(SCR1, L_small);
        // count denotes the end, in bytes
        alsl_d(count, count, to, shift - 1);
        break;
      case T_LONG:
        shift = 3;
        slti(SCR1, count, 7);
        vreplgr2vr_d(fscratch, value); // 64 bit -> 128 bit
        bnez(SCR1, L_small);
        // count denotes the end, in bytes
        alsl_d(count, count, to, shift - 1);
        break;
      default: ShouldNotReachHere();
    }

    // nature aligned for store
    vst(fscratch, to, 0);
    bstrins_d(to, R0, 3, 0);
    addi_d(to, to, 16);

    // fill large chunks
    Label L_loop128, L_lt128, L_lt64, L_lt32, L_lt16;

    addi_d(SCR1, count, -128);
    blt(SCR1, to, L_lt128);

    bind(L_loop128);
    vst(fscratch, to, 0);
    vst(fscratch, to, 16);
    vst(fscratch, to, 32);
    vst(fscratch, to, 48);
    vst(fscratch, to, 64);
    vst(fscratch, to, 80);
    vst(fscratch, to, 96);
    vst(fscratch, to, 112);
    addi_d(to, to, 128);
    bge(SCR1, to, L_loop128);

    bind(L_lt128);
    addi_d(SCR1, count, -64);
    blt(SCR1, to, L_lt64);
    vst(fscratch, to, 0);
    vst(fscratch, to, 16);
    vst(fscratch, to, 32);
    vst(fscratch, to, 48);
    addi_d(to, to, 64);

    bind(L_lt64);
    addi_d(SCR1, count, -32);
    blt(SCR1, to, L_lt32);
    vst(fscratch, to, 0);
    vst(fscratch, to, 16);
    addi_d(to, to, 32);

    bind(L_lt32);
    addi_d(SCR1, count, -16);
    blt(SCR1, to, L_lt16);
    vst(fscratch, to, 0);

    bind(L_lt16);
    vst(fscratch, count, -16);

    jr(RA);

    // Short arrays (<= 48 bytes)
    bind(L_small);
    pcaddi(SCR1, 4);
    slli_d(count, count, 4 + shift);
    add_d(SCR1, SCR1, count);
    jr(SCR1);

    tiny_fill_0_24(to, value);

    // 25:
    vst(fscratch, to, 0);
    vst(fscratch, to, 9);
    jr(RA);
    nop();

    // 26:
    vst(fscratch, to, 0);
    vst(fscratch, to, 10);
    jr(RA);
    nop();

    // 27:
    vst(fscratch, to, 0);
    vst(fscratch, to, 11);
    jr(RA);
    nop();

    // 28:
    vst(fscratch, to, 0);
    vst(fscratch, to, 12);
    jr(RA);
    nop();

    // 29:
    vst(fscratch, to, 0);
    vst(fscratch, to, 13);
    jr(RA);
    nop();

    // 30:
    vst(fscratch, to, 0);
    vst(fscratch, to, 14);
    jr(RA);
    nop();

    // 31:
    vst(fscratch, to, 0);
    vst(fscratch, to, 15);
    jr(RA);
    nop();

    // 32:
    vst(fscratch, to, 0);
    vst(fscratch, to, 16);
    jr(RA);
    nop();

    // 33:
    vst(fscratch, to, 0);
    vst(fscratch, to, 16);
    st_b(value, to, 32);
    jr(RA);

    // 34:
    vst(fscratch, to, 0);
    vst(fscratch, to, 16);
    st_h(value, to, 32);
    jr(RA);

    // 35:
    vst(fscratch, to, 0);
    vst(fscratch, to, 16);
    st_w(value, to, 31);
    jr(RA);

    // 36:
    vst(fscratch, to, 0);
    vst(fscratch, to, 16);
    st_w(value, to, 32);
    jr(RA);

    // 37:
    vst(fscratch, to, 0);
    vst(fscratch, to, 16);
    st_d(value, to, 29);
    jr(RA);

    // 38:
    vst(fscratch, to, 0);
    vst(fscratch, to, 16);
    st_d(value, to, 30);
    jr(RA);

    // 39:
    vst(fscratch, to, 0);
    vst(fscratch, to, 16);
    st_d(value, to, 31);
    jr(RA);

    // 40:
    vst(fscratch, to, 0);
    vst(fscratch, to, 16);
    st_d(value, to, 32);
    jr(RA);

    // 41:
    vst(fscratch, to, 0);
    vst(fscratch, to, 16);
    vst(fscratch, to, 25);
    jr(RA);

    // 42:
    vst(fscratch, to, 0);
    vst(fscratch, to, 16);
    vst(fscratch, to, 26);
    jr(RA);

    // 43:
    vst(fscratch, to, 0);
    vst(fscratch, to, 16);
    vst(fscratch, to, 27);
    jr(RA);

    // 44:
    vst(fscratch, to, 0);
    vst(fscratch, to, 16);
    vst(fscratch, to, 28);
    jr(RA);

    // 45:
    vst(fscratch, to, 0);
    vst(fscratch, to, 16);
    vst(fscratch, to, 29);
    jr(RA);

    // 46:
    vst(fscratch, to, 0);
    vst(fscratch, to, 16);
    vst(fscratch, to, 30);
    jr(RA);

    // 47:
    vst(fscratch, to, 0);
    vst(fscratch, to, 16);
    vst(fscratch, to, 31);
    jr(RA);

    // 48:
    vst(fscratch, to, 0);
    vst(fscratch, to, 16);
    vst(fscratch, to, 32);
    jr(RA);
}

inline void MacroAssembler::array_fill_lasx(BasicType t, Register to,
                                            Register value, Register count) {
    assert(UseLASX, "should be");
    assert_different_registers(to, value, count, SCR1);

    Label L_small;

    int shift = -1;
    switch (t) {
      case T_BYTE:
        shift = 0;
        slti(SCR1, count, 73);
        xvreplgr2vr_b(fscratch, value); // 8 bit -> 256 bit
        movfr2gr_d(value, fscratch);
        bnez(SCR1, L_small);
        // count denotes the end, in bytes
        add_d(count, to, count);
        break;
      case T_SHORT:
        shift = 1;
        slti(SCR1, count, 37);
        xvreplgr2vr_h(fscratch, value); // 16 bit -> 256 bit
        movfr2gr_d(value, fscratch);
        bnez(SCR1, L_small);
        // count denotes the end, in bytes
        alsl_d(count, count, to, shift - 1);
        break;
      case T_INT:
        shift = 2;
        slti(SCR1, count, 19);
        xvreplgr2vr_w(fscratch, value); // 32 bit -> 256 bit
        movfr2gr_d(value, fscratch);
        bnez(SCR1, L_small);
        // count denotes the end, in bytes
        alsl_d(count, count, to, shift - 1);
        break;
      case T_LONG:
        shift = 3;
        slti(SCR1, count, 10);
        xvreplgr2vr_d(fscratch, value); // 64 bit -> 256 bit
        bnez(SCR1, L_small);
        // count denotes the end, in bytes
        alsl_d(count, count, to, shift - 1);
        break;
      default: ShouldNotReachHere();
    }

    // nature aligned for store
    xvst(fscratch, to, 0);
    bstrins_d(to, R0, 4, 0);
    addi_d(to, to, 32);

    // fill large chunks
    Label L_loop256, L_lt256, L_lt128, L_lt64, L_lt32;

    addi_d(SCR1, count, -256);
    blt(SCR1, to, L_lt256);

    bind(L_loop256);
    xvst(fscratch, to, 0);
    xvst(fscratch, to, 32);
    xvst(fscratch, to, 64);
    xvst(fscratch, to, 96);
    xvst(fscratch, to, 128);
    xvst(fscratch, to, 160);
    xvst(fscratch, to, 192);
    xvst(fscratch, to, 224);
    addi_d(to, to, 256);
    bge(SCR1, to, L_loop256);

    bind(L_lt256);
    addi_d(SCR1, count, -128);
    blt(SCR1, to, L_lt128);
    xvst(fscratch, to, 0);
    xvst(fscratch, to, 32);
    xvst(fscratch, to, 64);
    xvst(fscratch, to, 96);
    addi_d(to, to, 128);

    bind(L_lt128);
    addi_d(SCR1, count, -64);
    blt(SCR1, to, L_lt64);
    xvst(fscratch, to, 0);
    xvst(fscratch, to, 32);
    addi_d(to, to, 64);

    bind(L_lt64);
    addi_d(SCR1, count, -32);
    blt(SCR1, to, L_lt32);
    xvst(fscratch, to, 0);

    bind(L_lt32);
    xvst(fscratch, count, -32);

    jr(RA);

    // Short arrays (<= 72 bytes)
    bind(L_small);
    pcaddi(SCR1, 4);
    slli_d(count, count, 4 + shift);
    add_d(SCR1, SCR1, count);
    jr(SCR1);

    tiny_fill_0_24(to, value);

    // 25:
    vst(fscratch, to, 0);
    vst(fscratch, to, 9);
    jr(RA);
    nop();

    // 26:
    vst(fscratch, to, 0);
    vst(fscratch, to, 10);
    jr(RA);
    nop();

    // 27:
    vst(fscratch, to, 0);
    vst(fscratch, to, 11);
    jr(RA);
    nop();

    // 28:
    vst(fscratch, to, 0);
    vst(fscratch, to, 12);
    jr(RA);
    nop();

    // 29:
    vst(fscratch, to, 0);
    vst(fscratch, to, 13);
    jr(RA);
    nop();

    // 30:
    vst(fscratch, to, 0);
    vst(fscratch, to, 14);
    jr(RA);
    nop();

    // 31:
    vst(fscratch, to, 0);
    vst(fscratch, to, 15);
    jr(RA);
    nop();

    // 32:
    xvst(fscratch, to, 0);
    jr(RA);
    nop();
    nop();

    // 33:
    xvst(fscratch, to, 0);
    st_b(value, to, 32);
    jr(RA);
    nop();

    // 34:
    xvst(fscratch, to, 0);
    st_h(value, to, 32);
    jr(RA);
    nop();

    // 35:
    xvst(fscratch, to, 0);
    st_w(value, to, 31);
    jr(RA);
    nop();

    // 36:
    xvst(fscratch, to, 0);
    st_w(value, to, 32);
    jr(RA);
    nop();

    // 37:
    xvst(fscratch, to, 0);
    st_d(value, to, 29);
    jr(RA);
    nop();

    // 38:
    xvst(fscratch, to, 0);
    st_d(value, to, 30);
    jr(RA);
    nop();

    // 39:
    xvst(fscratch, to, 0);
    st_d(value, to, 31);
    jr(RA);
    nop();

    // 40:
    xvst(fscratch, to, 0);
    st_d(value, to, 32);
    jr(RA);
    nop();

    // 41:
    xvst(fscratch, to, 0);
    vst(fscratch, to, 25);
    jr(RA);
    nop();

    // 42:
    xvst(fscratch, to, 0);
    vst(fscratch, to, 26);
    jr(RA);
    nop();

    // 43:
    xvst(fscratch, to, 0);
    vst(fscratch, to, 27);
    jr(RA);
    nop();

    // 44:
    xvst(fscratch, to, 0);
    vst(fscratch, to, 28);
    jr(RA);
    nop();

    // 45:
    xvst(fscratch, to, 0);
    vst(fscratch, to, 29);
    jr(RA);
    nop();

    // 46:
    xvst(fscratch, to, 0);
    vst(fscratch, to, 30);
    jr(RA);
    nop();

    // 47:
    xvst(fscratch, to, 0);
    vst(fscratch, to, 31);
    jr(RA);
    nop();

    // 48:
    xvst(fscratch, to, 0);
    vst(fscratch, to, 32);
    jr(RA);
    nop();

    // 49:
    xvst(fscratch, to, 0);
    xvst(fscratch, to, 17);
    jr(RA);
    nop();

    // 50:
    xvst(fscratch, to, 0);
    xvst(fscratch, to, 18);
    jr(RA);
    nop();

    // 51:
    xvst(fscratch, to, 0);
    xvst(fscratch, to, 19);
    jr(RA);
    nop();

    // 52:
    xvst(fscratch, to, 0);
    xvst(fscratch, to, 20);
    jr(RA);
    nop();

    // 53:
    xvst(fscratch, to, 0);
    xvst(fscratch, to, 21);
    jr(RA);
    nop();

    // 54:
    xvst(fscratch, to, 0);
    xvst(fscratch, to, 22);
    jr(RA);
    nop();

    // 55:
    xvst(fscratch, to, 0);
    xvst(fscratch, to, 23);
    jr(RA);
    nop();

    // 56:
    xvst(fscratch, to, 0);
    xvst(fscratch, to, 24);
    jr(RA);
    nop();

    // 57:
    xvst(fscratch, to, 0);
    xvst(fscratch, to, 25);
    jr(RA);
    nop();

    // 58:
    xvst(fscratch, to, 0);
    xvst(fscratch, to, 26);
    jr(RA);
    nop();

    // 59:
    xvst(fscratch, to, 0);
    xvst(fscratch, to, 27);
    jr(RA);
    nop();

    // 60:
    xvst(fscratch, to, 0);
    xvst(fscratch, to, 28);
    jr(RA);
    nop();

    // 61:
    xvst(fscratch, to, 0);
    xvst(fscratch, to, 29);
    jr(RA);
    nop();

    // 62:
    xvst(fscratch, to, 0);
    xvst(fscratch, to, 30);
    jr(RA);
    nop();

    // 63:
    xvst(fscratch, to, 0);
    xvst(fscratch, to, 31);
    jr(RA);
    nop();

    // 64:
    xvst(fscratch, to, 0);
    xvst(fscratch, to, 32);
    jr(RA);
    nop();

    // 65:
    xvst(fscratch, to, 0);
    xvst(fscratch, to, 32);
    st_b(value, to, 64);
    jr(RA);

    // 66:
    xvst(fscratch, to, 0);
    xvst(fscratch, to, 32);
    st_h(value, to, 64);
    jr(RA);

    // 67:
    xvst(fscratch, to, 0);
    xvst(fscratch, to, 32);
    st_w(value, to, 63);
    jr(RA);

    // 68:
    xvst(fscratch, to, 0);
    xvst(fscratch, to, 32);
    st_w(value, to, 64);
    jr(RA);

    // 69:
    xvst(fscratch, to, 0);
    xvst(fscratch, to, 32);
    st_d(value, to, 61);
    jr(RA);

    // 70:
    xvst(fscratch, to, 0);
    xvst(fscratch, to, 32);
    st_d(value, to, 62);
    jr(RA);

    // 71:
    xvst(fscratch, to, 0);
    xvst(fscratch, to, 32);
    st_d(value, to, 63);
    jr(RA);

    // 72:
    xvst(fscratch, to, 0);
    xvst(fscratch, to, 32);
    st_d(value, to, 64);
    jr(RA);
}

#endif // CPU_LOONGARCH_MACROASSEMBLER_LOONGARCH_INLINE_HPP
