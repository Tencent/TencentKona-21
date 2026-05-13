/*
 * Copyright (c) 2000, 2012, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2015, 2023, Loongson Technology. All rights reserved.
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

#ifndef CPU_LOONGARCH_REGISTER_LOONGARCH_HPP
#define CPU_LOONGARCH_REGISTER_LOONGARCH_HPP

#include "asm/register.hpp"
#include "utilities/powerOfTwo.hpp"
#include "logging/log.hpp"
#include "utilities/bitMap.hpp"
#include "utilities/formatBuffer.hpp"
#include "utilities/ticks.hpp"

class VMRegImpl;
typedef VMRegImpl* VMReg;

class Register {
 private:
  int _encoding;

  constexpr explicit Register(int encoding) : _encoding(encoding) {}

 public:
  enum {
    number_of_registers      = 32,
    max_slots_per_register   = 2,
  };

  class RegisterImpl: public AbstractRegisterImpl {
    friend class Register;

    static constexpr const RegisterImpl* first();

   public:
    // accessors
    constexpr int raw_encoding() const { return checked_cast<int>(this - first()); }
    constexpr int     encoding() const { assert(is_valid(), "invalid register"); return raw_encoding(); }
    constexpr bool    is_valid() const { return 0 <= raw_encoding() && raw_encoding() < number_of_registers; }

    // derived registers, offsets, and addresses
    inline Register successor() const;

    VMReg as_VMReg() const;

    const char* name() const;
  };

  inline friend constexpr Register as_Register(int encoding);

  constexpr Register() : _encoding(-1) {} // noreg

  int operator==(const Register r) const { return _encoding == r._encoding; }
  int operator!=(const Register r) const { return _encoding != r._encoding; }

  constexpr const RegisterImpl* operator->() const { return RegisterImpl::first() + _encoding; }
};

extern Register::RegisterImpl all_RegisterImpls[Register::number_of_registers + 1] INTERNAL_VISIBILITY;

inline constexpr const Register::RegisterImpl* Register::RegisterImpl::first() {
  return all_RegisterImpls + 1;
}

constexpr Register noreg = Register();

inline constexpr Register as_Register(int encoding) {
  if (0 <= encoding && encoding < Register::number_of_registers) {
    return Register(encoding);
  }
  return noreg;
}

inline Register Register::RegisterImpl::successor() const {
  assert(is_valid(), "sanity");
  return as_Register(encoding() + 1);
}

// The integer registers of the LoongArch architecture
constexpr Register r0     = as_Register( 0);
constexpr Register r1     = as_Register( 1);
constexpr Register r2     = as_Register( 2);
constexpr Register r3     = as_Register( 3);
constexpr Register r4     = as_Register( 4);
constexpr Register r5     = as_Register( 5);
constexpr Register r6     = as_Register( 6);
constexpr Register r7     = as_Register( 7);
constexpr Register r8     = as_Register( 8);
constexpr Register r9     = as_Register( 9);
constexpr Register r10    = as_Register(10);
constexpr Register r11    = as_Register(11);
constexpr Register r12    = as_Register(12);
constexpr Register r13    = as_Register(13);
constexpr Register r14    = as_Register(14);
constexpr Register r15    = as_Register(15);
constexpr Register r16    = as_Register(16);
constexpr Register r17    = as_Register(17);
constexpr Register r18    = as_Register(18);
constexpr Register r19    = as_Register(19);
constexpr Register r20    = as_Register(20);
constexpr Register r21    = as_Register(21);
constexpr Register r22    = as_Register(22);
constexpr Register r23    = as_Register(23);
constexpr Register r24    = as_Register(24);
constexpr Register r25    = as_Register(25);
constexpr Register r26    = as_Register(26);
constexpr Register r27    = as_Register(27);
constexpr Register r28    = as_Register(28);
constexpr Register r29    = as_Register(29);
constexpr Register r30    = as_Register(30);
constexpr Register r31    = as_Register(31);


constexpr Register NOREG  = noreg;
constexpr Register R0     = r0;
constexpr Register R1     = r1;
constexpr Register R2     = r2;
constexpr Register R3     = r3;
constexpr Register R4     = r4;
constexpr Register R5     = r5;
constexpr Register R6     = r6;
constexpr Register R7     = r7;
constexpr Register R8     = r8;
constexpr Register R9     = r9;
constexpr Register R10    = r10;
constexpr Register R11    = r11;
constexpr Register R12    = r12;
constexpr Register R13    = r13;
constexpr Register R14    = r14;
constexpr Register R15    = r15;
constexpr Register R16    = r16;
constexpr Register R17    = r17;
constexpr Register R18    = r18;
constexpr Register R19    = r19;
constexpr Register R20    = r20;
constexpr Register R21    = r21;
constexpr Register R22    = r22;
constexpr Register R23    = r23;
constexpr Register R24    = r24;
constexpr Register R25    = r25;
constexpr Register R26    = r26;
constexpr Register R27    = r27;
constexpr Register R28    = r28;
constexpr Register R29    = r29;
constexpr Register R30    = r30;
constexpr Register R31    = r31;


constexpr Register RA     = R1;
constexpr Register TP     = R2;
constexpr Register SP     = R3;
constexpr Register A0     = R4;
constexpr Register A1     = R5;
constexpr Register A2     = R6;
constexpr Register A3     = R7;
constexpr Register A4     = R8;
constexpr Register A5     = R9;
constexpr Register A6     = R10;
constexpr Register A7     = R11;
constexpr Register T0     = R12;
constexpr Register T1     = R13;
constexpr Register T2     = R14;
constexpr Register T3     = R15;
constexpr Register T4     = R16;
constexpr Register T5     = R17;
constexpr Register T6     = R18;
constexpr Register T7     = R19;
constexpr Register T8     = R20;
constexpr Register RX     = R21;
constexpr Register FP     = R22;
constexpr Register S0     = R23;
constexpr Register S1     = R24;
constexpr Register S2     = R25;
constexpr Register S3     = R26;
constexpr Register S4     = R27;
constexpr Register S5     = R28;
constexpr Register S6     = R29;
constexpr Register S7     = R30;
constexpr Register S8     = R31;


// Use FloatRegister as shortcut
class FloatRegister {
 private:
  int _encoding;

  constexpr explicit FloatRegister(int encoding) : _encoding(encoding) {}

 public:
  inline friend constexpr FloatRegister as_FloatRegister(int encoding);

  enum {
    number_of_registers     = 32,
    save_slots_per_register = 2,
    slots_per_lsx_register  = 4,
    slots_per_lasx_register = 8,
    max_slots_per_register  = 8
  };

  class FloatRegisterImpl: public AbstractRegisterImpl {
    friend class FloatRegister;

    static constexpr const FloatRegisterImpl* first();

   public:
    // accessors
    constexpr int raw_encoding() const { return checked_cast<int>(this - first()); }
    constexpr int     encoding() const { assert(is_valid(), "invalid register"); return raw_encoding(); }
    constexpr bool    is_valid() const { return 0 <= raw_encoding() && raw_encoding() < number_of_registers; }

    // derived registers, offsets, and addresses
    inline FloatRegister successor() const;

    VMReg as_VMReg() const;

    const char* name() const;
  };

  constexpr FloatRegister() : _encoding(-1) {} // fnoreg

  int operator==(const FloatRegister r) const { return _encoding == r._encoding; }
  int operator!=(const FloatRegister r) const { return _encoding != r._encoding; }

  constexpr const FloatRegisterImpl* operator->() const { return FloatRegisterImpl::first() + _encoding; }
};

extern FloatRegister::FloatRegisterImpl all_FloatRegisterImpls[FloatRegister::number_of_registers + 1] INTERNAL_VISIBILITY;

inline constexpr const FloatRegister::FloatRegisterImpl* FloatRegister::FloatRegisterImpl::first() {
  return all_FloatRegisterImpls + 1;
}

constexpr FloatRegister fnoreg = FloatRegister();

inline constexpr FloatRegister as_FloatRegister(int encoding) {
  if (0 <= encoding && encoding < FloatRegister::number_of_registers) {
    return FloatRegister(encoding);
  }
  return fnoreg;
}

inline FloatRegister FloatRegister::FloatRegisterImpl::successor() const {
  assert(is_valid(), "sanity");
  return as_FloatRegister(encoding() + 1);
}

constexpr FloatRegister f0     = as_FloatRegister( 0);
constexpr FloatRegister f1     = as_FloatRegister( 1);
constexpr FloatRegister f2     = as_FloatRegister( 2);
constexpr FloatRegister f3     = as_FloatRegister( 3);
constexpr FloatRegister f4     = as_FloatRegister( 4);
constexpr FloatRegister f5     = as_FloatRegister( 5);
constexpr FloatRegister f6     = as_FloatRegister( 6);
constexpr FloatRegister f7     = as_FloatRegister( 7);
constexpr FloatRegister f8     = as_FloatRegister( 8);
constexpr FloatRegister f9     = as_FloatRegister( 9);
constexpr FloatRegister f10    = as_FloatRegister(10);
constexpr FloatRegister f11    = as_FloatRegister(11);
constexpr FloatRegister f12    = as_FloatRegister(12);
constexpr FloatRegister f13    = as_FloatRegister(13);
constexpr FloatRegister f14    = as_FloatRegister(14);
constexpr FloatRegister f15    = as_FloatRegister(15);
constexpr FloatRegister f16    = as_FloatRegister(16);
constexpr FloatRegister f17    = as_FloatRegister(17);
constexpr FloatRegister f18    = as_FloatRegister(18);
constexpr FloatRegister f19    = as_FloatRegister(19);
constexpr FloatRegister f20    = as_FloatRegister(20);
constexpr FloatRegister f21    = as_FloatRegister(21);
constexpr FloatRegister f22    = as_FloatRegister(22);
constexpr FloatRegister f23    = as_FloatRegister(23);
constexpr FloatRegister f24    = as_FloatRegister(24);
constexpr FloatRegister f25    = as_FloatRegister(25);
constexpr FloatRegister f26    = as_FloatRegister(26);
constexpr FloatRegister f27    = as_FloatRegister(27);
constexpr FloatRegister f28    = as_FloatRegister(28);
constexpr FloatRegister f29    = as_FloatRegister(29);
constexpr FloatRegister f30    = as_FloatRegister(30);
constexpr FloatRegister f31    = as_FloatRegister(31);


constexpr FloatRegister FNOREG = fnoreg;
constexpr FloatRegister F0     = f0;
constexpr FloatRegister F1     = f1;
constexpr FloatRegister F2     = f2;
constexpr FloatRegister F3     = f3;
constexpr FloatRegister F4     = f4;
constexpr FloatRegister F5     = f5;
constexpr FloatRegister F6     = f6;
constexpr FloatRegister F7     = f7;
constexpr FloatRegister F8     = f8;
constexpr FloatRegister F9     = f9;
constexpr FloatRegister F10    = f10;
constexpr FloatRegister F11    = f11;
constexpr FloatRegister F12    = f12;
constexpr FloatRegister F13    = f13;
constexpr FloatRegister F14    = f14;
constexpr FloatRegister F15    = f15;
constexpr FloatRegister F16    = f16;
constexpr FloatRegister F17    = f17;
constexpr FloatRegister F18    = f18;
constexpr FloatRegister F19    = f19;
constexpr FloatRegister F20    = f20;
constexpr FloatRegister F21    = f21;
constexpr FloatRegister F22    = f22;
constexpr FloatRegister F23    = f23;
constexpr FloatRegister F24    = f24;
constexpr FloatRegister F25    = f25;
constexpr FloatRegister F26    = f26;
constexpr FloatRegister F27    = f27;
constexpr FloatRegister F28    = f28;
constexpr FloatRegister F29    = f29;
constexpr FloatRegister F30    = f30;
constexpr FloatRegister F31    = f31;

constexpr FloatRegister FA0    = F0;
constexpr FloatRegister FA1    = F1;
constexpr FloatRegister FA2    = F2;
constexpr FloatRegister FA3    = F3;
constexpr FloatRegister FA4    = F4;
constexpr FloatRegister FA5    = F5;
constexpr FloatRegister FA6    = F6;
constexpr FloatRegister FA7    = F7;
constexpr FloatRegister FT0    = F8;
constexpr FloatRegister FT1    = F9;
constexpr FloatRegister FT2    = F10;
constexpr FloatRegister FT3    = F11;
constexpr FloatRegister FT4    = F12;
constexpr FloatRegister FT5    = F13;
constexpr FloatRegister FT6    = F14;
constexpr FloatRegister FT7    = F15;
constexpr FloatRegister FT8    = F16;
constexpr FloatRegister FT9    = F17;
constexpr FloatRegister FT10   = F18;
constexpr FloatRegister FT11   = F19;
constexpr FloatRegister FT12   = F20;
constexpr FloatRegister FT13   = F21;
constexpr FloatRegister FT14   = F22;
constexpr FloatRegister FT15   = F23;
constexpr FloatRegister FS0    = F24;
constexpr FloatRegister FS1    = F25;
constexpr FloatRegister FS2    = F26;
constexpr FloatRegister FS3    = F27;
constexpr FloatRegister FS4    = F28;
constexpr FloatRegister FS5    = F29;
constexpr FloatRegister FS6    = F30;
constexpr FloatRegister FS7    = F31;


class ConditionalFlagRegister {
  int _encoding;

  constexpr explicit ConditionalFlagRegister(int encoding) : _encoding(encoding) {}

 public:
  inline friend constexpr ConditionalFlagRegister as_ConditionalFlagRegister(int encoding);

  enum {
    number_of_registers = 8
  };

  class ConditionalFlagRegisterImpl: public AbstractRegisterImpl {
    friend class ConditionalFlagRegister;

    static constexpr const ConditionalFlagRegisterImpl* first();

   public:
    // accessors
    int raw_encoding() const { return checked_cast<int>(this - first()); }
    int encoding() const     { assert(is_valid(), "invalid register"); return raw_encoding(); }
    bool is_valid() const    { return 0 <= raw_encoding() && raw_encoding() < number_of_registers; }

    // derived registers, offsets, and addresses
    inline ConditionalFlagRegister successor() const;

    VMReg as_VMReg() const;

    const char* name() const;
  };

  constexpr ConditionalFlagRegister() : _encoding(-1) {} // vnoreg

  int operator==(const ConditionalFlagRegister r) const { return _encoding == r._encoding; }
  int operator!=(const ConditionalFlagRegister r) const { return _encoding != r._encoding; }

  const ConditionalFlagRegisterImpl* operator->() const { return ConditionalFlagRegisterImpl::first() + _encoding; }
};

extern ConditionalFlagRegister::ConditionalFlagRegisterImpl all_ConditionalFlagRegisterImpls[ConditionalFlagRegister::number_of_registers + 1] INTERNAL_VISIBILITY;

inline constexpr const ConditionalFlagRegister::ConditionalFlagRegisterImpl* ConditionalFlagRegister::ConditionalFlagRegisterImpl::first() {
  return all_ConditionalFlagRegisterImpls + 1;
}

constexpr ConditionalFlagRegister cfnoreg = ConditionalFlagRegister();

inline constexpr ConditionalFlagRegister as_ConditionalFlagRegister(int encoding) {
  if (0 <= encoding && encoding < ConditionalFlagRegister::number_of_registers) {
    return ConditionalFlagRegister(encoding);
  }
  return cfnoreg;
}

inline ConditionalFlagRegister ConditionalFlagRegister::ConditionalFlagRegisterImpl::successor() const {
  assert(is_valid(), "sanity");
  return as_ConditionalFlagRegister(encoding() + 1);
}

constexpr ConditionalFlagRegister fcc0 = as_ConditionalFlagRegister(0);
constexpr ConditionalFlagRegister fcc1 = as_ConditionalFlagRegister(1);
constexpr ConditionalFlagRegister fcc2 = as_ConditionalFlagRegister(2);
constexpr ConditionalFlagRegister fcc3 = as_ConditionalFlagRegister(3);
constexpr ConditionalFlagRegister fcc4 = as_ConditionalFlagRegister(4);
constexpr ConditionalFlagRegister fcc5 = as_ConditionalFlagRegister(5);
constexpr ConditionalFlagRegister fcc6 = as_ConditionalFlagRegister(6);
constexpr ConditionalFlagRegister fcc7 = as_ConditionalFlagRegister(7);

constexpr ConditionalFlagRegister FCC0 = fcc0;
constexpr ConditionalFlagRegister FCC1 = fcc1;
constexpr ConditionalFlagRegister FCC2 = fcc2;
constexpr ConditionalFlagRegister FCC3 = fcc3;
constexpr ConditionalFlagRegister FCC4 = fcc4;
constexpr ConditionalFlagRegister FCC5 = fcc5;
constexpr ConditionalFlagRegister FCC6 = fcc6;
constexpr ConditionalFlagRegister FCC7 = fcc7;

// Need to know the total number of registers of all sorts for SharedInfo.
// Define a class that exports it.
class ConcreteRegisterImpl : public AbstractRegisterImpl {
 public:
  enum {
    max_gpr = Register::number_of_registers * Register::max_slots_per_register,
    max_fpr = max_gpr + FloatRegister::number_of_registers * FloatRegister::max_slots_per_register,

    // A big enough number for C2: all the registers plus flags
    // This number must be large enough to cover REG_COUNT (defined by c2) registers.
    // There is no requirement that any ordering here matches any ordering c2 gives
    // it's optoregs.
    number_of_registers = max_fpr // gpr/fpr/vpr
  };
};

typedef AbstractRegSet<Register> RegSet;
typedef AbstractRegSet<FloatRegister> FloatRegSet;


template <>
inline Register AbstractRegSet<Register>::first() {
  uint32_t first = _bitset & -_bitset;
  return first ? as_Register(exact_log2(first)) : noreg;
}

template <>
inline FloatRegister AbstractRegSet<FloatRegister>::first() {
  uint32_t first = _bitset & -_bitset;
  return first ? as_FloatRegister(exact_log2(first)) : fnoreg;
}

#endif //CPU_LOONGARCH_REGISTER_LOONGARCH_HPP
