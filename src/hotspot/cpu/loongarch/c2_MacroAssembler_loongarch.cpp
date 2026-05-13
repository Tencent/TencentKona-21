/*
 * Copyright (c) 2020, Oracle and/or its affiliates. All rights reserved.
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
#include "asm/assembler.hpp"
#include "asm/assembler.inline.hpp"
#include "opto/c2_MacroAssembler.hpp"
#include "opto/compile.hpp"
#include "opto/intrinsicnode.hpp"
#include "opto/output.hpp"
#include "opto/subnode.hpp"
#include "runtime/objectMonitor.hpp"
#include "runtime/stubRoutines.hpp"


// using the cr register as the bool result: 0 for failed; others success.
void C2_MacroAssembler::fast_lock_c2(Register oop, Register box, Register flag,
                                  Register disp_hdr, Register tmp) {
  Label cont;
  Label object_has_monitor;
  Label count, no_count;

  assert_different_registers(oop, box, tmp, disp_hdr, flag);

  // Load markWord from object into displaced_header.
  assert(oopDesc::mark_offset_in_bytes() == 0, "offset of _mark is not 0");
  ld_d(disp_hdr, oop, oopDesc::mark_offset_in_bytes());

  if (DiagnoseSyncOnValueBasedClasses != 0) {
    load_klass(flag, oop);
    ld_wu(flag, Address(flag, Klass::access_flags_offset()));
    li(AT, JVM_ACC_IS_VALUE_BASED_CLASS);
    andr(AT, flag, AT);
    move(flag, R0);
    bnez(AT, cont);
  }

  // Check for existing monitor
  andi(AT, disp_hdr, markWord::monitor_value);
  bnez(AT, object_has_monitor); // inflated vs stack-locked|neutral|bias

  if (LockingMode == LM_MONITOR) {
    move(flag, R0); // Set zero flag to indicate 'failure'
    b(cont);
  } else if (LockingMode == LM_LEGACY) {
    // Set tmp to be (markWord of object | UNLOCK_VALUE).
    ori(tmp, disp_hdr, markWord::unlocked_value);

    // Initialize the box. (Must happen before we update the object mark!)
    st_d(tmp, box, BasicLock::displaced_header_offset_in_bytes());

    // If cmpxchg is succ, then flag = 1
    cmpxchg(Address(oop, 0), tmp, box, flag, true, true /* acquire */);
    bnez(flag, cont);

    // If the compare-and-exchange succeeded, then we found an unlocked
    // object, will have now locked it will continue at label cont
    // We did not see an unlocked object so try the fast recursive case.

    // Check if the owner is self by comparing the value in the
    // markWord of object (disp_hdr) with the stack pointer.
    sub_d(disp_hdr, tmp, SP);
    li(tmp, (intptr_t) (~(os::vm_page_size()-1) | (uintptr_t)markWord::lock_mask_in_place));
    // If (mark & lock_mask) == 0 and mark - sp < page_size,
    // we are stack-locking and goto cont,
    // hence we can store 0 as the displaced header in the box,
    // which indicates that it is a recursive lock.
    andr(tmp, disp_hdr, tmp);
    st_d(tmp, box, BasicLock::displaced_header_offset_in_bytes());
    sltui(flag, tmp, 1); // flag = (tmp == 0) ? 1 : 0
    b(cont);
  } else {
    assert(LockingMode == LM_LIGHTWEIGHT, "must be");
    lightweight_lock(oop, disp_hdr, flag, SCR1, no_count);
    b(count);
  }

  // Handle existing monitor.
  bind(object_has_monitor);

  // The object's monitor m is unlocked if m->owner is null,
  // otherwise m->owner may contain a thread or a stack address.
  //
  // Try to CAS m->owner from null to current thread.
  move(AT, R0);
  addi_d(tmp, disp_hdr, in_bytes(ObjectMonitor::owner_offset()) - markWord::monitor_value);
  cmpxchg(Address(tmp, 0), AT, TREG, flag, true, true /* acquire */);
  if (LockingMode != LM_LIGHTWEIGHT) {
    // Store a non-null value into the box to avoid looking like a re-entrant
    // lock. The fast-path monitor unlock code checks for
    // markWord::monitor_value so use markWord::unused_mark which has the
    // relevant bit set, and also matches ObjectSynchronizer::enter.
    li(tmp, (address)markWord::unused_mark().value());
    st_d(tmp, Address(box, BasicLock::displaced_header_offset_in_bytes()));
  }
  bnez(flag, cont); // CAS success means locking succeeded

  bne(AT, TREG, cont); // Check for recursive locking

  // Recursive lock case
  li(flag, 1);
  increment(Address(disp_hdr, in_bytes(ObjectMonitor::recursions_offset()) - markWord::monitor_value), 1);

  bind(cont);
  // flag == 1 indicates success
  // flag == 0 indicates failure
  beqz(flag, no_count);

  bind(count);
  increment(Address(TREG, JavaThread::held_monitor_count_offset()), 1);

  bind(no_count);
}

// using cr flag to indicate the fast_unlock result: 0 for failed; others success.
void C2_MacroAssembler::fast_unlock_c2(Register oop, Register box, Register flag,
                                    Register disp_hdr, Register tmp) {
  Label cont;
  Label object_has_monitor;
  Label count, no_count;

  assert_different_registers(oop, box, tmp, disp_hdr, flag);

  // Find the lock address and load the displaced header from the stack.
  ld_d(disp_hdr, Address(box, BasicLock::displaced_header_offset_in_bytes()));

  if (LockingMode == LM_LEGACY) {
    // If the displaced header is 0, we have a recursive unlock.
    sltui(flag, disp_hdr, 1); // flag = (disp_hdr == 0) ? 1 : 0
    beqz(disp_hdr, cont);
  }

  assert(oopDesc::mark_offset_in_bytes() == 0, "offset of _mark is not 0");

  // Handle existing monitor.
  ld_d(tmp, oop, oopDesc::mark_offset_in_bytes());
  andi(AT, tmp, markWord::monitor_value);
  bnez(AT, object_has_monitor);

  if (LockingMode == LM_MONITOR) {
    move(flag, R0); // Set zero flag to indicate 'failure'
    b(cont);
  } else if (LockingMode == LM_LEGACY) {
    // Check if it is still a light weight lock, this is true if we
    // see the stack address of the basicLock in the markWord of the
    // object.
    cmpxchg(Address(oop, 0), box, disp_hdr, flag, false, false /* acquire */);
    b(cont);
  } else {
    assert(LockingMode == LM_LIGHTWEIGHT, "must be");
    lightweight_unlock(oop, tmp, flag, box, no_count);
    b(count);
  }

  // Handle existing monitor.
  bind(object_has_monitor);

  addi_d(tmp, tmp, -(int)markWord::monitor_value); // monitor

  if (LockingMode == LM_LIGHTWEIGHT) {
    // If the owner is anonymous, we need to fix it -- in an outline stub.
    Register tmp2 = disp_hdr;
    ld_d(tmp2, Address(tmp, ObjectMonitor::owner_offset()));
    // We cannot use tbnz here, the target might be too far away and cannot
    // be encoded.
    assert_different_registers(tmp2, AT);
    li(AT, (uint64_t)ObjectMonitor::ANONYMOUS_OWNER);
    andr(AT, tmp2, AT);
    C2HandleAnonOMOwnerStub* stub = new (Compile::current()->comp_arena()) C2HandleAnonOMOwnerStub(tmp, tmp2);
    Compile::current()->output()->add_stub(stub);
    bnez(AT, stub->entry());
    bind(stub->continuation());
  }

  ld_d(disp_hdr, Address(tmp, ObjectMonitor::recursions_offset()));

  Label notRecursive;
  beqz(disp_hdr, notRecursive);

  // Recursive lock
  addi_d(disp_hdr, disp_hdr, -1);
  st_d(disp_hdr, Address(tmp, ObjectMonitor::recursions_offset()));
  li(flag, 1);
  b(cont);

  bind(notRecursive);
  ld_d(flag, Address(tmp, ObjectMonitor::EntryList_offset()));
  ld_d(disp_hdr, Address(tmp, ObjectMonitor::cxq_offset()));
  orr(AT, flag, disp_hdr);

  move(flag, R0);
  bnez(AT, cont);

  addi_d(AT, tmp, in_bytes(ObjectMonitor::owner_offset()));
  amswap_db_d(tmp, R0, AT);
  li(flag, 1);

  bind(cont);
  // flag == 1 indicates success
  // flag == 0 indicates failure
  beqz(flag, no_count);

  bind(count);
  decrement(Address(TREG, JavaThread::held_monitor_count_offset()), 1);

  bind(no_count);
}

typedef void (MacroAssembler::* load_chr_insn)(Register rd, const Address &adr);

void C2_MacroAssembler::string_indexof(Register haystack, Register needle,
                                       Register haystack_len, Register needle_len,
                                       Register result, int ae)
{
  assert(ae != StrIntrinsicNode::LU, "Invalid encoding");

  Label LINEARSEARCH, LINEARSTUB, DONE, NOMATCH;

  bool isLL = ae == StrIntrinsicNode::LL;

  bool needle_isL = ae == StrIntrinsicNode::LL || ae == StrIntrinsicNode::UL;
  bool haystack_isL = ae == StrIntrinsicNode::LL || ae == StrIntrinsicNode::LU;

  int needle_chr_size = needle_isL ? 1 : 2;
  int haystack_chr_size = haystack_isL ? 1 : 2;

  Address::ScaleFactor needle_chr_shift = needle_isL ? Address::no_scale
                                                     : Address::times_2;
  Address::ScaleFactor haystack_chr_shift = haystack_isL ? Address::no_scale
                                                         : Address::times_2;

  load_chr_insn needle_load_1chr = needle_isL ? (load_chr_insn)&MacroAssembler::ld_bu
                                              : (load_chr_insn)&MacroAssembler::ld_hu;
  load_chr_insn haystack_load_1chr = haystack_isL ? (load_chr_insn)&MacroAssembler::ld_bu
                                                  : (load_chr_insn)&MacroAssembler::ld_hu;

  // Note, inline_string_indexOf() generates checks:
  // if (pattern.count > src.count) return -1;
  // if (pattern.count == 0) return 0;

  // We have two strings, a source string in haystack, haystack_len and a pattern string
  // in needle, needle_len. Find the first occurrence of pattern in source or return -1.

  // For larger pattern and source we use a simplified Boyer Moore algorithm.
  // With a small pattern and source we use linear scan.

  // needle_len >= 8 && needle_len < 256 && needle_len < haystack_len/4, use bmh algorithm.

  // needle_len < 8, use linear scan
  li(AT, 8);
  blt(needle_len, AT, LINEARSEARCH);

  // needle_len >= 256, use linear scan
  li(AT, 256);
  bge(needle_len, AT, LINEARSTUB);

  // needle_len >= haystack_len/4, use linear scan
  srli_d(AT, haystack_len, 2);
  bge(needle_len, AT, LINEARSTUB);

  // Boyer-Moore-Horspool introduction:
  // The Boyer Moore alogorithm is based on the description here:-
  //
  // http://en.wikipedia.org/wiki/Boyer%E2%80%93Moore_string_search_algorithm
  //
  // This describes and algorithm with 2 shift rules. The 'Bad Character' rule
  // and the 'Good Suffix' rule.
  //
  // These rules are essentially heuristics for how far we can shift the
  // pattern along the search string.
  //
  // The implementation here uses the 'Bad Character' rule only because of the
  // complexity of initialisation for the 'Good Suffix' rule.
  //
  // This is also known as the Boyer-Moore-Horspool algorithm:
  //
  // http://en.wikipedia.org/wiki/Boyer-Moore-Horspool_algorithm
  //
  // #define ASIZE 256
  //
  //    int bm(unsigned char *pattern, int m, unsigned char *src, int n) {
  //      int i, j;
  //      unsigned c;
  //      unsigned char bc[ASIZE];
  //
  //      /* Preprocessing */
  //      for (i = 0; i < ASIZE; ++i)
  //        bc[i] = m;
  //      for (i = 0; i < m - 1; ) {
  //        c = pattern[i];
  //        ++i;
  //        // c < 256 for Latin1 string, so, no need for branch
  //        #ifdef PATTERN_STRING_IS_LATIN1
  //        bc[c] = m - i;
  //        #else
  //        if (c < ASIZE) bc[c] = m - i;
  //        #endif
  //      }
  //
  //      /* Searching */
  //      j = 0;
  //      while (j <= n - m) {
  //        c = src[i+j];
  //        if (pattern[m-1] == c)
  //          int k;
  //          for (k = m - 2; k >= 0 && pattern[k] == src[k + j]; --k);
  //          if (k < 0) return j;
  //          // c < 256 for Latin1 string, so, no need for branch
  //          #ifdef SOURCE_STRING_IS_LATIN1_AND_PATTERN_STRING_IS_LATIN1
  //          // LL case: (c< 256) always true. Remove branch
  //          j += bc[pattern[j+m-1]];
  //          #endif
  //          #ifdef SOURCE_STRING_IS_UTF_AND_PATTERN_STRING_IS_UTF
  //          // UU case: need if (c<ASIZE) check. Skip 1 character if not.
  //          if (c < ASIZE)
  //            j += bc[pattern[j+m-1]];
  //          else
  //            j += 1
  //          #endif
  //          #ifdef SOURCE_IS_UTF_AND_PATTERN_IS_LATIN1
  //          // UL case: need if (c<ASIZE) check. Skip <pattern length> if not.
  //          if (c < ASIZE)
  //            j += bc[pattern[j+m-1]];
  //          else
  //            j += m
  //          #endif
  //      }
  //      return -1;
  //    }

  Label BCLOOP, BCSKIP, BMLOOPSTR2, BMLOOPSTR1, BMSKIP, BMADV, BMMATCH,
        BMLOOPSTR1_LASTCMP, BMLOOPSTR1_CMP, BMLOOPSTR1_AFTER_LOAD;

  Register haystack_end = haystack_len;
  Register result_tmp = result;

  Register nlen_tmp = T0; // needle len tmp
  Register skipch = T1;
  Register last_byte = T2;
  Register last_dword = T3;
  Register orig_haystack = T4;
  Register ch1 = T5;
  Register ch2 = T6;

  RegSet spilled_regs = RegSet::range(T0, T6);

  push(spilled_regs);

  // pattern length is >=8, so, we can read at least 1 register for cases when
  // UTF->Latin1 conversion is not needed(8 LL or 4UU) and half register for
  // UL case. We'll re-read last character in inner pre-loop code to have
  // single outer pre-loop load
  const int first_step = isLL ? 7 : 3;

  const int ASIZE = 256;

  addi_d(SP, SP, -ASIZE);

  // init BC offset table with default value: needle_len
  //
  // for (i = 0; i < ASIZE; ++i)
  //   bc[i] = m;
  if (UseLASX) {
    xvreplgr2vr_b(fscratch, needle_len);

    for (int i = 0; i < ASIZE; i += 32) {
      xvst(fscratch, SP, i);
    }
  } else if (UseLSX) {
    vreplgr2vr_b(fscratch, needle_len);

    for (int i = 0; i < ASIZE; i += 16) {
      vst(fscratch, SP, i);
    }
  } else {
    move(AT, needle_len);
    bstrins_d(AT, AT, 15, 8);
    bstrins_d(AT, AT, 31, 16);
    bstrins_d(AT, AT, 63, 32);

    for (int i = 0; i < ASIZE; i += 8) {
      st_d(AT, SP, i);
    }
  }

  sub_d(nlen_tmp, haystack_len, needle_len);
  lea(haystack_end, Address(haystack, nlen_tmp, haystack_chr_shift, 0));
  addi_d(ch2, needle_len, -1); // bc offset init value
  move(nlen_tmp, needle);

  //  for (i = 0; i < m - 1; ) {
  //    c = pattern[i];
  //    ++i;
  //    // c < 256 for Latin1 string, so, no need for branch
  //    #ifdef PATTERN_STRING_IS_LATIN1
  //    bc[c] = m - i;
  //    #else
  //    if (c < ASIZE) bc[c] = m - i;
  //    #endif
  //  }
  bind(BCLOOP);
  (this->*needle_load_1chr)(ch1, Address(nlen_tmp));
  addi_d(nlen_tmp, nlen_tmp, needle_chr_size);
  if (!needle_isL) {
    // ae == StrIntrinsicNode::UU
    li(AT, 256u);
    bgeu(ch1, AT, BCSKIP); // GE for UTF
  }
  stx_b(ch2, SP, ch1); // store skip offset to BC offset table

  bind(BCSKIP);
  addi_d(ch2, ch2, -1); // for next pattern element, skip distance -1
  blt(R0, ch2, BCLOOP);

  if (needle_isL == haystack_isL) {
    // load last 8 pattern bytes (8LL/4UU symbols)
    ld_d(last_dword, Address(needle, needle_len, needle_chr_shift, -wordSize));
    addi_d(nlen_tmp, needle_len, -1); // m - 1, index of the last element in pattern
    move(orig_haystack, haystack);
    bstrpick_d(last_byte, last_dword, 63, 64 - 8 * needle_chr_size); // UU/LL: pattern[m-1]
  } else {
    // UL: from UTF-16(source) search Latin1(pattern)
    // load last 4 bytes(4 symbols)
    ld_wu(last_byte, Address(needle, needle_len, Address::no_scale, -wordSize / 2));
    addi_d(nlen_tmp, needle_len, -1); // m - 1, index of the last element in pattern
    move(orig_haystack, haystack);
    // convert Latin1 to UTF. eg: 0x0000abcd -> 0x0a0b0c0d
    bstrpick_d(last_dword, last_byte, 7, 0);
    srli_d(last_byte, last_byte, 8);
    bstrins_d(last_dword, last_byte, 23, 16);
    srli_d(last_byte, last_byte, 8);
    bstrins_d(last_dword, last_byte, 39, 32);
    srli_d(last_byte, last_byte, 8); // last_byte: 0x0000000a
    bstrins_d(last_dword, last_byte, 55, 48); // last_dword: 0x0a0b0c0d
  }

  // i = m - 1;
  // skipch = j + i;
  // if (skipch == pattern[m - 1]
  //   for (k = m - 2; k >= 0 && pattern[k] == src[k + j]; --k);
  // else
  //   move j with bad char offset table
  bind(BMLOOPSTR2);
  // compare pattern to source string backward
  (this->*haystack_load_1chr)(skipch, Address(haystack, nlen_tmp, haystack_chr_shift, 0));
  addi_d(nlen_tmp, nlen_tmp, -first_step); // nlen_tmp is positive here, because needle_len >= 8
  bne(last_byte, skipch, BMSKIP); // if not equal, skipch is bad char
  ld_d(ch2, Address(haystack, nlen_tmp, haystack_chr_shift, 0)); // load 8 bytes from source string
  move(ch1, last_dword);
  if (isLL) {
    b(BMLOOPSTR1_AFTER_LOAD);
  } else {
    addi_d(nlen_tmp, nlen_tmp, -1); // no need to branch for UU/UL case. cnt1 >= 8
    b(BMLOOPSTR1_CMP);
  }

  bind(BMLOOPSTR1);
  (this->*needle_load_1chr)(ch1, Address(needle, nlen_tmp, needle_chr_shift, 0));
  (this->*haystack_load_1chr)(ch2, Address(haystack, nlen_tmp, haystack_chr_shift, 0));

  bind(BMLOOPSTR1_AFTER_LOAD);
  addi_d(nlen_tmp, nlen_tmp, -1);
  blt(nlen_tmp, R0, BMLOOPSTR1_LASTCMP);

  bind(BMLOOPSTR1_CMP);
  beq(ch1, ch2, BMLOOPSTR1);

  bind(BMSKIP);
  if (!isLL) {
    // if we've met UTF symbol while searching Latin1 pattern, then we can
    // skip needle_len symbols
    if (needle_isL != haystack_isL) {
      move(result_tmp, needle_len);
    } else {
      li(result_tmp, 1);
    }
    li(AT, 256u);
    bgeu(skipch, AT, BMADV); // GE for UTF
  }
  ldx_bu(result_tmp, SP, skipch); // load skip offset

  bind(BMADV);
  addi_d(nlen_tmp, needle_len, -1);
  // move haystack after bad char skip offset
  lea(haystack, Address(haystack, result_tmp, haystack_chr_shift, 0));
  bge(haystack_end, haystack, BMLOOPSTR2);
  addi_d(SP, SP, ASIZE);
  b(NOMATCH);

  bind(BMLOOPSTR1_LASTCMP);
  bne(ch1, ch2, BMSKIP);

  bind(BMMATCH);
  sub_d(result, haystack, orig_haystack);
  if (!haystack_isL) {
    srli_d(result, result, 1);
  }
  addi_d(SP, SP, ASIZE);
  pop(spilled_regs);
  b(DONE);

  bind(LINEARSTUB);
  li(AT, 16); // small patterns still should be handled by simple algorithm
  blt(needle_len, AT, LINEARSEARCH);
  move(result, R0);
  address stub;
  if (isLL) {
    stub = StubRoutines::la::string_indexof_linear_ll();
    assert(stub != nullptr, "string_indexof_linear_ll stub has not been generated");
  } else if (needle_isL) {
    stub = StubRoutines::la::string_indexof_linear_ul();
    assert(stub != nullptr, "string_indexof_linear_ul stub has not been generated");
  } else {
    stub = StubRoutines::la::string_indexof_linear_uu();
    assert(stub != nullptr, "string_indexof_linear_uu stub has not been generated");
  }
  trampoline_call(RuntimeAddress(stub));
  b(DONE);

  bind(NOMATCH);
  li(result, -1);
  pop(spilled_regs);
  b(DONE);

  bind(LINEARSEARCH);
  string_indexof_linearscan(haystack, needle, haystack_len, needle_len, -1, result, ae);

  bind(DONE);
}

void C2_MacroAssembler::string_indexof_linearscan(Register haystack, Register needle,
                                                  Register haystack_len, Register needle_len,
                                                  int needle_con_cnt, Register result, int ae)
{
  // Note:
  // needle_con_cnt > 0 means needle_len register is invalid, needle length is constant
  // for UU/LL: needle_con_cnt[1, 4], UL: needle_con_cnt = 1
  assert(needle_con_cnt <= 4, "Invalid needle constant count");
  assert(ae != StrIntrinsicNode::LU, "Invalid encoding");

  Register hlen_neg = haystack_len;
  Register nlen_neg = needle_len;
  Register result_tmp = result;

  Register nlen_tmp = A0, hlen_tmp = A1;
  Register first = A2, ch1 = A3, ch2 = AT;

  RegSet spilled_regs = RegSet::range(A0, A3);

  push(spilled_regs);

  bool isLL = ae == StrIntrinsicNode::LL;

  bool needle_isL = ae == StrIntrinsicNode::LL || ae == StrIntrinsicNode::UL;
  bool haystack_isL = ae == StrIntrinsicNode::LL || ae == StrIntrinsicNode::LU;
  int needle_chr_shift = needle_isL ? 0 : 1;
  int haystack_chr_shift = haystack_isL ? 0 : 1;
  int needle_chr_size = needle_isL ? 1 : 2;
  int haystack_chr_size = haystack_isL ? 1 : 2;

  load_chr_insn needle_load_1chr = needle_isL ? (load_chr_insn)&MacroAssembler::ld_bu
                                              : (load_chr_insn)&MacroAssembler::ld_hu;
  load_chr_insn haystack_load_1chr = haystack_isL ? (load_chr_insn)&MacroAssembler::ld_bu
                                                  : (load_chr_insn)&MacroAssembler::ld_hu;
  load_chr_insn load_2chr = isLL ? (load_chr_insn)&MacroAssembler::ld_hu
                                 : (load_chr_insn)&MacroAssembler::ld_wu;
  load_chr_insn load_4chr = isLL ? (load_chr_insn)&MacroAssembler::ld_wu
                                 : (load_chr_insn)&MacroAssembler::ld_d;

  Label DO1, DO2, DO3, MATCH, NOMATCH, DONE;

  if (needle_con_cnt == -1) {
    Label DOSHORT, FIRST_LOOP, STR2_NEXT, STR1_LOOP, STR1_NEXT;

    li(AT, needle_isL == haystack_isL ? 4 : 2); // UU/LL:4, UL:2
    blt(needle_len, AT, DOSHORT);

    sub_d(result_tmp, haystack_len, needle_len);

    (this->*needle_load_1chr)(first, Address(needle));
    if (!haystack_isL) slli_d(result_tmp, result_tmp, haystack_chr_shift);
    add_d(haystack, haystack, result_tmp);
    sub_d(hlen_neg, R0, result_tmp);
    if (!needle_isL) slli_d(needle_len, needle_len, needle_chr_shift);
    add_d(needle, needle, needle_len);
    sub_d(nlen_neg, R0, needle_len);

    bind(FIRST_LOOP);
    (this->*haystack_load_1chr)(ch2, Address(haystack, hlen_neg, Address::no_scale, 0));
    beq(first, ch2, STR1_LOOP);

    bind(STR2_NEXT);
    addi_d(hlen_neg, hlen_neg, haystack_chr_size);
    bge(R0, hlen_neg, FIRST_LOOP);
    b(NOMATCH);

    bind(STR1_LOOP);
    addi_d(nlen_tmp, nlen_neg, needle_chr_size);
    addi_d(hlen_tmp, hlen_neg, haystack_chr_size);
    bge(nlen_tmp, R0, MATCH);

    bind(STR1_NEXT);
    (this->*needle_load_1chr)(ch1, Address(needle, nlen_tmp, Address::no_scale, 0));
    (this->*haystack_load_1chr)(ch2, Address(haystack, hlen_tmp, Address::no_scale, 0));
    bne(ch1, ch2, STR2_NEXT);
    addi_d(nlen_tmp, nlen_tmp, needle_chr_size);
    addi_d(hlen_tmp, hlen_tmp, haystack_chr_size);
    blt(nlen_tmp, R0, STR1_NEXT);
    b(MATCH);

    bind(DOSHORT);
    if (needle_isL == haystack_isL) {
      li(AT, 2);
      blt(needle_len, AT, DO1); // needle_len == 1
      blt(AT, needle_len, DO3); // needle_len == 3
      // if needle_len == 2 then goto DO2
    }
  }

  if (needle_con_cnt == 4) {
    Label CH1_LOOP;
    (this->*load_4chr)(ch1, Address(needle));
    addi_d(result_tmp, haystack_len, -4);
    if (!haystack_isL) slli_d(result_tmp, result_tmp, haystack_chr_shift);
    add_d(haystack, haystack, result_tmp);
    sub_d(hlen_neg, R0, result_tmp);

    bind(CH1_LOOP);
    (this->*load_4chr)(ch2, Address(haystack, hlen_neg, Address::no_scale, 0));
    beq(ch1, ch2, MATCH);
    addi_d(hlen_neg, hlen_neg, haystack_chr_size);
    bge(R0, hlen_neg, CH1_LOOP);
    b(NOMATCH);
  }

  if ((needle_con_cnt == -1 && needle_isL == haystack_isL) || needle_con_cnt == 2) {
    Label CH1_LOOP;
    bind(DO2);
    (this->*load_2chr)(ch1, Address(needle));
    addi_d(result_tmp, haystack_len, -2);
    if (!haystack_isL) slli_d(result_tmp, result_tmp, haystack_chr_shift);
    add_d(haystack, haystack, result_tmp);
    sub_d(hlen_neg, R0, result_tmp);

    bind(CH1_LOOP);
    (this->*load_2chr)(ch2, Address(haystack, hlen_neg, Address::no_scale, 0));
    beq(ch1, ch2, MATCH);
    addi_d(hlen_neg, hlen_neg, haystack_chr_size);
    bge(R0, hlen_neg, CH1_LOOP);
    b(NOMATCH);
  }

  if ((needle_con_cnt == -1 && needle_isL == haystack_isL) || needle_con_cnt == 3) {
    Label FIRST_LOOP, STR2_NEXT, STR1_LOOP;

    bind(DO3);
    (this->*load_2chr)(first, Address(needle));
    (this->*needle_load_1chr)(ch1, Address(needle, 2 * needle_chr_size));
    addi_d(result_tmp, haystack_len, -3);
    if (!haystack_isL) slli_d(result_tmp, result_tmp, haystack_chr_shift);
    add_d(haystack, haystack, result_tmp);
    sub_d(hlen_neg, R0, result_tmp);

    bind(FIRST_LOOP);
    (this->*load_2chr)(ch2, Address(haystack, hlen_neg, Address::no_scale, 0));
    beq(first, ch2, STR1_LOOP);

    bind(STR2_NEXT);
    addi_d(hlen_neg, hlen_neg, haystack_chr_size);
    bge(R0, hlen_neg, FIRST_LOOP);
    b(NOMATCH);

    bind(STR1_LOOP);
    (this->*haystack_load_1chr)(ch2, Address(haystack, hlen_neg, Address::no_scale, 2 * haystack_chr_size));
    bne(ch1, ch2, STR2_NEXT);
    b(MATCH);
  }

  if (needle_con_cnt == -1 || needle_con_cnt == 1) {
    Label CH1_LOOP, HAS_ZERO, DO1_SHORT, DO1_LOOP;
    Register mask01 = nlen_tmp;
    Register mask7f = hlen_tmp;
    Register masked = first;

    bind(DO1);
    (this->*needle_load_1chr)(ch1, Address(needle));
    li(AT, 8);
    blt(haystack_len, AT, DO1_SHORT);

    addi_d(result_tmp, haystack_len, -8 / haystack_chr_size);
    if (!haystack_isL) slli_d(result_tmp, result_tmp, haystack_chr_shift);
    add_d(haystack, haystack, result_tmp);
    sub_d(hlen_neg, R0, result_tmp);

    if (haystack_isL) bstrins_d(ch1, ch1, 15, 8);
    bstrins_d(ch1, ch1, 31, 16);
    bstrins_d(ch1, ch1, 63, 32);

    li(mask01, haystack_isL ? 0x0101010101010101 : 0x0001000100010001);
    li(mask7f, haystack_isL ? 0x7f7f7f7f7f7f7f7f : 0x7fff7fff7fff7fff);

    bind(CH1_LOOP);
    ldx_d(ch2, haystack, hlen_neg);
    xorr(ch2, ch1, ch2);
    sub_d(masked, ch2, mask01);
    orr(ch2, ch2, mask7f);
    andn(masked, masked, ch2);
    bnez(masked, HAS_ZERO);
    addi_d(hlen_neg, hlen_neg, 8);
    blt(hlen_neg, R0, CH1_LOOP);

    li(AT, 8);
    bge(hlen_neg, AT, NOMATCH);
    move(hlen_neg, R0);
    b(CH1_LOOP);

    bind(HAS_ZERO);
    ctz_d(masked, masked);
    srli_d(masked, masked, 3);
    add_d(hlen_neg, hlen_neg, masked);
    b(MATCH);

    bind(DO1_SHORT);
    addi_d(result_tmp, haystack_len, -1);
    if (!haystack_isL) slli_d(result_tmp, result_tmp, haystack_chr_shift);
    add_d(haystack, haystack, result_tmp);
    sub_d(hlen_neg, R0, result_tmp);

    bind(DO1_LOOP);
    (this->*haystack_load_1chr)(ch2, Address(haystack, hlen_neg, Address::no_scale, 0));
    beq(ch1, ch2, MATCH);
    addi_d(hlen_neg, hlen_neg, haystack_chr_size);
    bge(R0, hlen_neg, DO1_LOOP);
  }

  bind(NOMATCH);
  li(result, -1);
  b(DONE);

  bind(MATCH);
  add_d(result, result_tmp, hlen_neg);
  if (!haystack_isL) srai_d(result, result, haystack_chr_shift);

  bind(DONE);
  pop(spilled_regs);
}

void C2_MacroAssembler::string_indexof_char(Register str1, Register cnt1,
                                            Register ch, Register result,
                                            Register tmp1, Register tmp2,
                                            Register tmp3)
{
  Label CH1_LOOP, HAS_ZERO, DO1_SHORT, DO1_LOOP, NOMATCH, DONE;

  beqz(cnt1, NOMATCH);

  move(result, R0);
  ori(tmp1, R0, 4);
  blt(cnt1, tmp1, DO1_LOOP);

  // UTF-16 char occupies 16 bits
  // ch -> chchchch
  bstrins_d(ch, ch, 31, 16);
  bstrins_d(ch, ch, 63, 32);

  li(tmp2, 0x0001000100010001);
  li(tmp3, 0x7fff7fff7fff7fff);

  bind(CH1_LOOP);
    ld_d(AT, str1, 0);
    xorr(AT, ch, AT);
    sub_d(tmp1, AT, tmp2);
    orr(AT, AT, tmp3);
    andn(tmp1, tmp1, AT);
    bnez(tmp1, HAS_ZERO);
    addi_d(str1, str1, 8);
    addi_d(result, result, 4);

    // meet the end of string
    beq(cnt1, result, NOMATCH);

    addi_d(tmp1, result, 4);
    bge(tmp1, cnt1, DO1_SHORT);
    b(CH1_LOOP);

  bind(HAS_ZERO);
    ctz_d(tmp1, tmp1);
    srli_d(tmp1, tmp1, 4);
    add_d(result, result, tmp1);
    b(DONE);

  // restore ch
  bind(DO1_SHORT);
    bstrpick_d(ch, ch, 15, 0);

  bind(DO1_LOOP);
    ld_hu(tmp1, str1, 0);
    beq(ch, tmp1, DONE);
    addi_d(str1, str1, 2);
    addi_d(result, result, 1);
    blt(result, cnt1, DO1_LOOP);

  bind(NOMATCH);
    addi_d(result, R0, -1);

  bind(DONE);
}

void C2_MacroAssembler::stringL_indexof_char(Register str1, Register cnt1,
                                             Register ch, Register result,
                                             Register tmp1, Register tmp2,
                                             Register tmp3)
{
  Label CH1_LOOP, HAS_ZERO, DO1_SHORT, DO1_LOOP, NOMATCH, DONE;

  beqz(cnt1, NOMATCH);

  move(result, R0);
  ori(tmp1, R0, 8);
  blt(cnt1, tmp1, DO1_LOOP);

  // Latin-1 char occupies 8 bits
  // ch -> chchchchchchchch
  bstrins_d(ch, ch, 15, 8);
  bstrins_d(ch, ch, 31, 16);
  bstrins_d(ch, ch, 63, 32);

  li(tmp2, 0x0101010101010101);
  li(tmp3, 0x7f7f7f7f7f7f7f7f);

  bind(CH1_LOOP);
    ld_d(AT, str1, 0);
    xorr(AT, ch, AT);
    sub_d(tmp1, AT, tmp2);
    orr(AT, AT, tmp3);
    andn(tmp1, tmp1, AT);
    bnez(tmp1, HAS_ZERO);
    addi_d(str1, str1, 8);
    addi_d(result, result, 8);

    // meet the end of string
    beq(cnt1, result, NOMATCH);

    addi_d(tmp1, result, 8);
    bge(tmp1, cnt1, DO1_SHORT);
    b(CH1_LOOP);

  bind(HAS_ZERO);
    ctz_d(tmp1, tmp1);
    srli_d(tmp1, tmp1, 3);
    add_d(result, result, tmp1);
    b(DONE);

  // restore ch
  bind(DO1_SHORT);
    bstrpick_d(ch, ch, 7, 0);

  bind(DO1_LOOP);
    ld_bu(tmp1, str1, 0);
    beq(ch, tmp1, DONE);
    addi_d(str1, str1, 1);
    addi_d(result, result, 1);
    blt(result, cnt1, DO1_LOOP);

  bind(NOMATCH);
    addi_d(result, R0, -1);

  bind(DONE);
}

// Compare strings, used for char[] and byte[].
void C2_MacroAssembler::string_compare(Register str1, Register str2,
                                    Register cnt1, Register cnt2, Register result,
                                    int ae, Register tmp1, Register tmp2,
                                    FloatRegister vtmp1, FloatRegister vtmp2) {
  Label L, Loop, LoopEnd, HaveResult, Done, Loop_Start,
        V_L, V_Loop, V_Result, V_Start;

  bool isLL = ae == StrIntrinsicNode::LL;
  bool isLU = ae == StrIntrinsicNode::LU;
  bool isUL = ae == StrIntrinsicNode::UL;
  bool isUU = ae == StrIntrinsicNode::UU;

  bool str1_isL = isLL || isLU;
  bool str2_isL = isLL || isUL;

  int charsInWord = isLL ? wordSize : wordSize/2;
  int charsInFloatRegister = (UseLASX && (isLL||isUU))?(isLL? 32 : 16):(isLL? 16 : 8);

  if (!str1_isL) srli_w(cnt1, cnt1, 1);
  if (!str2_isL) srli_w(cnt2, cnt2, 1);

  // compute the difference of lengths (in result)
  sub_d(result, cnt1, cnt2); // result holds the difference of two lengths

  // compute the shorter length (in cnt1)
  bge(cnt2, cnt1, V_Start);
  move(cnt1, cnt2);

  bind(V_Start);
  // it is hard to apply the xvilvl to flate 16 bytes into 32 bytes,
  // so we employ the LASX only for the LL or UU StrIntrinsicNode.
  if (UseLASX && (isLL || isUU)) {
    ori(AT, R0, charsInFloatRegister);
    addi_d(tmp1, R0, 16);
    xvinsgr2vr_d(fscratch, R0, 0);
    xvinsgr2vr_d(fscratch, tmp1, 2);
    bind(V_Loop);
    blt(cnt1, AT, Loop_Start);
    if (isLL) {
      xvld(vtmp1, str1, 0);
      xvld(vtmp2, str2, 0);
      xvxor_v(vtmp1, vtmp1, vtmp2);
      xvseteqz_v(FCC0, vtmp1);
      bceqz(FCC0, V_L);

      addi_d(str1, str1, 32);
      addi_d(str2, str2, 32);
      addi_d(cnt1, cnt1, -charsInFloatRegister);
      b(V_Loop);

      bind(V_L);
      xvxor_v(vtmp2, vtmp2, vtmp2);
      xvabsd_b(vtmp1, vtmp1, vtmp2);
      xvneg_b(vtmp1, vtmp1);
      xvfrstp_b(vtmp2, vtmp1, fscratch);
      xvpickve2gr_du(tmp1, vtmp2, 0);
      addi_d(cnt2, R0, 16);
      bne(tmp1, cnt2, V_Result);

      xvpickve2gr_du(tmp1, vtmp2, 2);
      addi_d(tmp1, tmp1, 16);

      // the index value was stored in tmp1
      bind(V_Result);
      ldx_bu(result, str1, tmp1);
      ldx_bu(tmp2, str2, tmp1);
      sub_d(result, result, tmp2);
      b(Done);
    } else if (isUU) {
      xvld(vtmp1, str1, 0);
      xvld(vtmp2, str2, 0);
      xvxor_v(vtmp1, vtmp1, vtmp2);
      xvseteqz_v(FCC0, vtmp1);
      bceqz(FCC0, V_L);

      addi_d(str1, str1, 32);
      addi_d(str2, str2, 32);
      addi_d(cnt1, cnt1, -charsInFloatRegister);
      b(V_Loop);

      bind(V_L);
      xvxor_v(vtmp2, vtmp2, vtmp2);
      xvabsd_h(vtmp1, vtmp1, vtmp2);
      xvneg_h(vtmp1, vtmp1);
      xvfrstp_h(vtmp2, vtmp1, fscratch);
      xvpickve2gr_du(tmp1, vtmp2, 0);
      addi_d(cnt2, R0, 8);
      bne(tmp1, cnt2, V_Result);

      xvpickve2gr_du(tmp1, vtmp2, 2);
      addi_d(tmp1, tmp1, 8);

      // the index value was stored in tmp1
      bind(V_Result);
      slli_d(tmp1, tmp1, 1);
      ldx_hu(result, str1, tmp1);
      ldx_hu(tmp2, str2, tmp1);
      sub_d(result, result, tmp2);
      b(Done);
    }
  } else if (UseLSX) {
    ori(AT, R0, charsInFloatRegister);
    vxor_v(fscratch, fscratch, fscratch);
    bind(V_Loop);
    blt(cnt1, AT, Loop_Start);
    if (isLL) {
      vld(vtmp1, str1, 0);
      vld(vtmp2, str2, 0);
      vxor_v(vtmp1, vtmp1, vtmp2);
      vseteqz_v(FCC0, vtmp1);
      bceqz(FCC0, V_L);

      addi_d(str1, str1, 16);
      addi_d(str2, str2, 16);
      addi_d(cnt1, cnt1, -charsInFloatRegister);
      b(V_Loop);

      bind(V_L);
      vxor_v(vtmp2, vtmp2, vtmp2);
      vabsd_b(vtmp1, vtmp1, vtmp2);
      vneg_b(vtmp1, vtmp1);
      vfrstpi_b(vtmp2, vtmp1, 0);
      vpickve2gr_bu(tmp1, vtmp2, 0);

      // the index value was stored in tmp1
      ldx_bu(result, str1, tmp1);
      ldx_bu(tmp2, str2, tmp1);
      sub_d(result, result, tmp2);
      b(Done);
    } else if (isLU) {
      vld(vtmp1, str1, 0);
      vld(vtmp2, str2, 0);
      vilvl_b(vtmp1, fscratch, vtmp1);
      vxor_v(vtmp1, vtmp1, vtmp2);
      vseteqz_v(FCC0, vtmp1);
      bceqz(FCC0, V_L);

      addi_d(str1, str1, 8);
      addi_d(str2, str2, 16);
      addi_d(cnt1, cnt1, -charsInFloatRegister);
      b(V_Loop);

      bind(V_L);
      vxor_v(vtmp2, vtmp2, vtmp2);
      vabsd_h(vtmp1, vtmp1, vtmp2);
      vneg_h(vtmp1, vtmp1);
      vfrstpi_h(vtmp2, vtmp1, 0);
      vpickve2gr_bu(tmp1, vtmp2, 0);

      // the index value was stored in tmp1
      ldx_bu(result, str1, tmp1);
      slli_d(tmp1, tmp1, 1);
      ldx_hu(tmp2, str2, tmp1);
      sub_d(result, result, tmp2);
      b(Done);
    } else if (isUL) {
      vld(vtmp1, str1, 0);
      vld(vtmp2, str2, 0);
      vilvl_b(vtmp2, fscratch, vtmp2);
      vxor_v(vtmp1, vtmp1, vtmp2);
      vseteqz_v(FCC0, vtmp1);
      bceqz(FCC0, V_L);

      addi_d(str1, str1, 16);
      addi_d(str2, str2, 8);
      addi_d(cnt1, cnt1, -charsInFloatRegister);
      b(V_Loop);

      bind(V_L);
      vxor_v(vtmp2, vtmp2, vtmp2);
      vabsd_h(vtmp1, vtmp1, vtmp2);
      vneg_h(vtmp1, vtmp1);
      vfrstpi_h(vtmp2, vtmp1, 0);
      vpickve2gr_bu(tmp1, vtmp2, 0);

      // the index value was stored in tmp1
      ldx_bu(tmp2, str2, tmp1);
      slli_d(tmp1, tmp1, 1);
      ldx_hu(result, str1, tmp1);
      sub_d(result, result, tmp2);
      b(Done);
    } else if (isUU) {
      vld(vtmp1, str1, 0);
      vld(vtmp2, str2, 0);
      vxor_v(vtmp1, vtmp1, vtmp2);
      vseteqz_v(FCC0, vtmp1);
      bceqz(FCC0, V_L);

      addi_d(str1, str1, 16);
      addi_d(str2, str2, 16);
      addi_d(cnt1, cnt1, -charsInFloatRegister);
      b(V_Loop);

      bind(V_L);
      vxor_v(vtmp2, vtmp2, vtmp2);
      vabsd_h(vtmp1, vtmp1, vtmp2);
      vneg_h(vtmp1, vtmp1);
      vfrstpi_h(vtmp2, vtmp1, 0);
      vpickve2gr_bu(tmp1, vtmp2, 0);

      // the index value was stored in tmp1
      slli_d(tmp1, tmp1, 1);
      ldx_hu(result, str1, tmp1);
      ldx_hu(tmp2, str2, tmp1);
      sub_d(result, result, tmp2);
      b(Done);
    }
  }

  // Now the shorter length is in cnt1 and cnt2 can be used as a tmp register
  //
  // For example:
  //  If isLL == true and cnt1 > 8, we load 8 bytes from str1 and str2. (Suppose A1 and B1 are different)
  //    tmp1: A7 A6 A5 A4 A3 A2 A1 A0
  //    tmp2: B7 B6 B5 B4 B3 B2 B1 B0
  //
  //  Then Use xor to find the difference between tmp1 and tmp2, right shift.
  //    tmp1: 00 A7 A6 A5 A4 A3 A2 A1
  //    tmp2: 00 B7 B6 B5 B4 B3 B2 B1
  //
  //  Fetch 0 to 7 bits of tmp1 and tmp2, subtract to get the result.
  //  Other types are similar to isLL.

  bind(Loop_Start);
  ori(AT, R0, charsInWord);
  bind(Loop);
  blt(cnt1, AT, LoopEnd);
  if (isLL) {
    ld_d(tmp1, str1, 0);
    ld_d(tmp2, str2, 0);
    beq(tmp1, tmp2, L);
    xorr(cnt2, tmp1, tmp2);
    ctz_d(cnt2, cnt2);
    andi(cnt2, cnt2, 0x38);
    srl_d(tmp1, tmp1, cnt2);
    srl_d(tmp2, tmp2, cnt2);
    bstrpick_d(tmp1, tmp1, 7, 0);
    bstrpick_d(tmp2, tmp2, 7, 0);
    sub_d(result, tmp1, tmp2);
    b(Done);
    bind(L);
    addi_d(str1, str1, 8);
    addi_d(str2, str2, 8);
    addi_d(cnt1, cnt1, -charsInWord);
    b(Loop);
  } else if (isLU) {
    ld_wu(cnt2, str1, 0);
    andr(tmp1, R0, R0);
    bstrins_d(tmp1, cnt2, 7, 0);
    srli_d(cnt2, cnt2, 8);
    bstrins_d(tmp1, cnt2, 23, 16);
    srli_d(cnt2, cnt2, 8);
    bstrins_d(tmp1, cnt2, 39, 32);
    srli_d(cnt2, cnt2, 8);
    bstrins_d(tmp1, cnt2, 55, 48);
    ld_d(tmp2, str2, 0);
    beq(tmp1, tmp2, L);
    xorr(cnt2, tmp1, tmp2);
    ctz_d(cnt2, cnt2);
    andi(cnt2, cnt2, 0x30);
    srl_d(tmp1, tmp1, cnt2);
    srl_d(tmp2, tmp2, cnt2);
    bstrpick_d(tmp1, tmp1, 15, 0);
    bstrpick_d(tmp2, tmp2, 15, 0);
    sub_d(result, tmp1, tmp2);
    b(Done);
    bind(L);
    addi_d(str1, str1, 4);
    addi_d(str2, str2, 8);
    addi_d(cnt1, cnt1, -charsInWord);
    b(Loop);
  } else if (isUL) {
    ld_wu(cnt2, str2, 0);
    andr(tmp2, R0, R0);
    bstrins_d(tmp2, cnt2, 7, 0);
    srli_d(cnt2, cnt2, 8);
    bstrins_d(tmp2, cnt2, 23, 16);
    srli_d(cnt2, cnt2, 8);
    bstrins_d(tmp2, cnt2, 39, 32);
    srli_d(cnt2, cnt2, 8);
    bstrins_d(tmp2, cnt2, 55, 48);
    ld_d(tmp1, str1, 0);
    beq(tmp1, tmp2, L);
    xorr(cnt2, tmp1, tmp2);
    ctz_d(cnt2, cnt2);
    andi(cnt2, cnt2, 0x30);
    srl_d(tmp1, tmp1, cnt2);
    srl_d(tmp2, tmp2, cnt2);
    bstrpick_d(tmp1, tmp1, 15, 0);
    bstrpick_d(tmp2, tmp2, 15, 0);
    sub_d(result, tmp1, tmp2);
    b(Done);
    bind(L);
    addi_d(str1, str1, 8);
    addi_d(str2, str2, 4);
    addi_d(cnt1, cnt1, -charsInWord);
    b(Loop);
  } else { // isUU
    ld_d(tmp1, str1, 0);
    ld_d(tmp2, str2, 0);
    beq(tmp1, tmp2, L);
    xorr(cnt2, tmp1, tmp2);
    ctz_d(cnt2, cnt2);
    andi(cnt2, cnt2, 0x30);
    srl_d(tmp1, tmp1, cnt2);
    srl_d(tmp2, tmp2, cnt2);
    bstrpick_d(tmp1, tmp1, 15, 0);
    bstrpick_d(tmp2, tmp2, 15, 0);
    sub_d(result, tmp1, tmp2);
    b(Done);
    bind(L);
    addi_d(str1, str1, 8);
    addi_d(str2, str2, 8);
    addi_d(cnt1, cnt1, -charsInWord);
    b(Loop);
  }

  bind(LoopEnd);
  beqz(cnt1, Done);
  if (str1_isL) {
    ld_bu(tmp1, str1, 0);
  } else {
    ld_hu(tmp1, str1, 0);
  }

  // compare current character
  if (str2_isL) {
    ld_bu(tmp2, str2, 0);
  } else {
    ld_hu(tmp2, str2, 0);
  }
  bne(tmp1, tmp2, HaveResult);
  addi_d(str1, str1, str1_isL ? 1 : 2);
  addi_d(str2, str2, str2_isL ? 1 : 2);
  addi_d(cnt1, cnt1, -1);
  b(LoopEnd);

  bind(HaveResult);
  sub_d(result, tmp1, tmp2);

  bind(Done);
}

// Compare char[] or byte[] arrays or substrings.
void C2_MacroAssembler::arrays_equals(Register str1, Register str2,
                                   Register cnt, Register tmp1, Register tmp2, Register result,
                                   bool is_char, bool is_array) {
  assert_different_registers(str1, str2, result, cnt, tmp1, tmp2);
  Label Loop, LoopEnd, ShortLoop, True, False;
  Label A_IS_NOT_NULL, A_MIGHT_BE_NULL;

  int length_offset  = arrayOopDesc::length_offset_in_bytes();
  int base_offset    = arrayOopDesc::base_offset_in_bytes(is_char ? T_CHAR : T_BYTE);

  addi_d(result, R0, 1);
  // Check the input args
  beq(str1, str2, True); // May have read barriers for str1 and str2 if is_array is true.
  if (is_array) {
    // Need additional checks for arrays_equals.
    andr(tmp1, str1, str2);
    beqz(tmp1, A_MIGHT_BE_NULL);
    bind(A_IS_NOT_NULL);

    // Check the lengths
    ld_w(cnt, str1, length_offset);
    ld_w(tmp1, str2, length_offset);
    bne(cnt, tmp1, False);
  }
  beqz(cnt, True);

  if (is_array) {
    addi_d(str1, str1, base_offset);
    addi_d(str2, str2, base_offset);
  }

  if (is_char && is_array) {
    slli_w(cnt, cnt, 1);
  }
  move(AT, R0);
  addi_w(cnt, cnt, -8);
  blt(cnt, R0, LoopEnd);
  bind(Loop);
  ldx_d(tmp1, str1, AT);
  ldx_d(tmp2, str2, AT);
  bne(tmp1, tmp2, False);
  addi_w(AT, AT, 8);
  addi_w(cnt, cnt, -8);
  bge(cnt, R0, Loop);
  li(tmp1, -8);
  beq(cnt, tmp1, True);

  bind(LoopEnd);
  addi_d(cnt, cnt, 8);

  bind(ShortLoop);
  ldx_bu(tmp1, str1, AT);
  ldx_bu(tmp2, str2, AT);
  bne(tmp1, tmp2, False);
  addi_w(AT, AT, 1);
  addi_w(cnt, cnt, -1);
  bnez(cnt, ShortLoop);
  b(True);

  if (is_array) {
    bind(A_MIGHT_BE_NULL);
    beqz(str1, False);
    beqz(str2, False);
    b(A_IS_NOT_NULL);
  }

  bind(False);
  move(result, R0);

  bind(True);
}

void C2_MacroAssembler::loadstore(Register reg, Register base, int disp, int type) {
  switch (type) {
    case STORE_BYTE:   st_b (reg, base, disp); break;
    case STORE_CHAR:
    case STORE_SHORT:  st_h (reg, base, disp); break;
    case STORE_INT:    st_w (reg, base, disp); break;
    case STORE_LONG:   st_d (reg, base, disp); break;
    case LOAD_BYTE:    ld_b (reg, base, disp); break;
    case LOAD_U_BYTE:  ld_bu(reg, base, disp); break;
    case LOAD_SHORT:   ld_h (reg, base, disp); break;
    case LOAD_U_SHORT: ld_hu(reg, base, disp); break;
    case LOAD_INT:     ld_w (reg, base, disp); break;
    case LOAD_U_INT:   ld_wu(reg, base, disp); break;
    case LOAD_LONG:    ld_d (reg, base, disp); break;
    case LOAD_LINKED_LONG:
      ll_d(reg, base, disp);
      break;
    default:
      ShouldNotReachHere();
    }
}

void C2_MacroAssembler::loadstore(Register reg, Register base, Register disp, int type) {
  switch (type) {
    case STORE_BYTE:   stx_b (reg, base, disp); break;
    case STORE_CHAR:
    case STORE_SHORT:  stx_h (reg, base, disp); break;
    case STORE_INT:    stx_w (reg, base, disp); break;
    case STORE_LONG:   stx_d (reg, base, disp); break;
    case LOAD_BYTE:    ldx_b (reg, base, disp); break;
    case LOAD_U_BYTE:  ldx_bu(reg, base, disp); break;
    case LOAD_SHORT:   ldx_h (reg, base, disp); break;
    case LOAD_U_SHORT: ldx_hu(reg, base, disp); break;
    case LOAD_INT:     ldx_w (reg, base, disp); break;
    case LOAD_U_INT:   ldx_wu(reg, base, disp); break;
    case LOAD_LONG:    ldx_d (reg, base, disp); break;
    case LOAD_LINKED_LONG:
      add_d(AT, base, disp);
      ll_d(reg, AT, 0);
      break;
    default:
      ShouldNotReachHere();
    }
}

void C2_MacroAssembler::loadstore(FloatRegister reg, Register base, int disp, int type) {
  switch (type) {
    case STORE_FLOAT:    fst_s(reg, base, disp); break;
    case STORE_DOUBLE:   fst_d(reg, base, disp); break;
    case STORE_VECTORX:  vst  (reg, base, disp); break;
    case STORE_VECTORY: xvst  (reg, base, disp); break;
    case LOAD_FLOAT:     fld_s(reg, base, disp); break;
    case LOAD_DOUBLE:    fld_d(reg, base, disp); break;
    case LOAD_VECTORX:   vld  (reg, base, disp); break;
    case LOAD_VECTORY:  xvld  (reg, base, disp); break;
    default:
      ShouldNotReachHere();
    }
}

void C2_MacroAssembler::loadstore(FloatRegister reg, Register base, Register disp, int type) {
  switch (type) {
    case STORE_FLOAT:    fstx_s(reg, base, disp); break;
    case STORE_DOUBLE:   fstx_d(reg, base, disp); break;
    case STORE_VECTORX:  vstx  (reg, base, disp); break;
    case STORE_VECTORY: xvstx  (reg, base, disp); break;
    case LOAD_FLOAT:     fldx_s(reg, base, disp); break;
    case LOAD_DOUBLE:    fldx_d(reg, base, disp); break;
    case LOAD_VECTORX:   vldx  (reg, base, disp); break;
    case LOAD_VECTORY:  xvldx  (reg, base, disp); break;
    default:
      ShouldNotReachHere();
    }
}

void C2_MacroAssembler::reduce_ins_v(FloatRegister vec1, FloatRegister vec2, FloatRegister vec3, BasicType type, int opcode) {
  switch (type) {
    case T_BYTE:
      switch (opcode) {
        case Op_AddReductionVI: vadd_b(vec1, vec2, vec3); break;
        case Op_MulReductionVI: vmul_b(vec1, vec2, vec3); break;
        case Op_MaxReductionV:  vmax_b(vec1, vec2, vec3); break;
        case Op_MinReductionV:  vmin_b(vec1, vec2, vec3); break;
        case Op_AndReductionV:  vand_v(vec1, vec2, vec3); break;
        case Op_OrReductionV:    vor_v(vec1, vec2, vec3); break;
        case Op_XorReductionV:  vxor_v(vec1, vec2, vec3); break;
        default:
          ShouldNotReachHere();
      }
      break;
    case T_SHORT:
      switch (opcode) {
        case Op_AddReductionVI: vadd_h(vec1, vec2, vec3); break;
        case Op_MulReductionVI: vmul_h(vec1, vec2, vec3); break;
        case Op_MaxReductionV:  vmax_h(vec1, vec2, vec3); break;
        case Op_MinReductionV:  vmin_h(vec1, vec2, vec3); break;
        case Op_AndReductionV:  vand_v(vec1, vec2, vec3); break;
        case Op_OrReductionV:    vor_v(vec1, vec2, vec3); break;
        case Op_XorReductionV:  vxor_v(vec1, vec2, vec3); break;
        default:
          ShouldNotReachHere();
      }
      break;
    case T_INT:
      switch (opcode) {
        case Op_AddReductionVI: vadd_w(vec1, vec2, vec3); break;
        case Op_MulReductionVI: vmul_w(vec1, vec2, vec3); break;
        case Op_MaxReductionV:  vmax_w(vec1, vec2, vec3); break;
        case Op_MinReductionV:  vmin_w(vec1, vec2, vec3); break;
        case Op_AndReductionV:  vand_v(vec1, vec2, vec3); break;
        case Op_OrReductionV:    vor_v(vec1, vec2, vec3); break;
        case Op_XorReductionV:  vxor_v(vec1, vec2, vec3); break;
        default:
          ShouldNotReachHere();
      }
      break;
    case T_LONG:
      switch (opcode) {
        case Op_AddReductionVL: vadd_d(vec1, vec2, vec3); break;
        case Op_MulReductionVL: vmul_d(vec1, vec2, vec3); break;
        case Op_MaxReductionV:  vmax_d(vec1, vec2, vec3); break;
        case Op_MinReductionV:  vmin_d(vec1, vec2, vec3); break;
        case Op_AndReductionV:  vand_v(vec1, vec2, vec3); break;
        case Op_OrReductionV:    vor_v(vec1, vec2, vec3); break;
        case Op_XorReductionV:  vxor_v(vec1, vec2, vec3); break;
        default:
          ShouldNotReachHere();
      }
      break;
    default:
      ShouldNotReachHere();
  }
}

void C2_MacroAssembler::reduce_ins_r(Register reg1, Register reg2, Register reg3, BasicType type, int opcode) {
  switch (type) {
    case T_BYTE:
    case T_SHORT:
    case T_INT:
      switch (opcode) {
        case Op_AddReductionVI: add_w(reg1, reg2, reg3); break;
        case Op_MulReductionVI: mul_w(reg1, reg2, reg3); break;
        case Op_AndReductionV:   andr(reg1, reg2, reg3); break;
        case Op_OrReductionV:     orr(reg1, reg2, reg3); break;
        case Op_XorReductionV:   xorr(reg1, reg2, reg3); break;
        default:
          ShouldNotReachHere();
      }
      break;
    case T_LONG:
      switch (opcode) {
        case Op_AddReductionVL: add_d(reg1, reg2, reg3); break;
        case Op_MulReductionVL: mul_d(reg1, reg2, reg3); break;
        case Op_AndReductionV:   andr(reg1, reg2, reg3); break;
        case Op_OrReductionV:     orr(reg1, reg2, reg3); break;
        case Op_XorReductionV:   xorr(reg1, reg2, reg3); break;
        default:
          ShouldNotReachHere();
      }
      break;
    default:
      ShouldNotReachHere();
  }
}

void C2_MacroAssembler::reduce_ins_f(FloatRegister reg1, FloatRegister reg2, FloatRegister reg3, BasicType type, int opcode) {
  switch (type) {
    case T_FLOAT:
      switch (opcode) {
        case Op_AddReductionVF: fadd_s(reg1, reg2, reg3); break;
        case Op_MulReductionVF: fmul_s(reg1, reg2, reg3); break;
        default:
          ShouldNotReachHere();
      }
      break;
    case T_DOUBLE:
      switch (opcode) {
        case Op_AddReductionVD: fadd_d(reg1, reg2, reg3); break;
        case Op_MulReductionVD: fmul_d(reg1, reg2, reg3); break;
        default:
          ShouldNotReachHere();
      }
      break;
    default:
      ShouldNotReachHere();
  }
}

void C2_MacroAssembler::reduce(Register dst, Register src, FloatRegister vsrc, FloatRegister tmp1, FloatRegister tmp2, BasicType type, int opcode, int vector_size) {
  if (vector_size == 32) {
    xvpermi_d(tmp1, vsrc, 0b00001110);
    reduce_ins_v(tmp1, vsrc, tmp1, type, opcode);
    vpermi_w(tmp2, tmp1, 0b00001110);
    reduce_ins_v(tmp1, tmp2, tmp1, type, opcode);
  } else if (vector_size == 16) {
    vpermi_w(tmp1, vsrc, 0b00001110);
    reduce_ins_v(tmp1, vsrc, tmp1, type, opcode);
  } else if (vector_size == 8) {
    vshuf4i_w(tmp1, vsrc, 0b00000001);
    reduce_ins_v(tmp1, vsrc, tmp1, type, opcode);
  } else if (vector_size == 4) {
    vshuf4i_h(tmp1, vsrc, 0b00000001);
    reduce_ins_v(tmp1, vsrc, tmp1, type, opcode);
  } else {
    ShouldNotReachHere();
  }

  if (type != T_LONG) {
    if (vector_size > 8) {
      vshuf4i_w(tmp2, tmp1, 0b00000001);
      reduce_ins_v(tmp1, tmp2, tmp1, type, opcode);
    }
    if (type != T_INT) {
      if (vector_size > 4) {
        vshuf4i_h(tmp2, tmp1, 0b00000001);
        reduce_ins_v(tmp1, tmp2, tmp1, type, opcode);
      }
      if (type != T_SHORT) {
        vshuf4i_b(tmp2, tmp1, 0b00000001);
        reduce_ins_v(tmp1, tmp2, tmp1, type, opcode);
      }
    }
  }

  switch (type) {
    case T_BYTE:  vpickve2gr_b(dst, tmp1, 0); break;
    case T_SHORT: vpickve2gr_h(dst, tmp1, 0); break;
    case T_INT:   vpickve2gr_w(dst, tmp1, 0); break;
    case T_LONG:  vpickve2gr_d(dst, tmp1, 0); break;
    default:
      ShouldNotReachHere();
  }
  if (opcode == Op_MaxReductionV) {
    slt(AT, dst, src);
    masknez(dst, dst, AT);
    maskeqz(AT, src, AT);
    orr(dst, dst, AT);
  } else if (opcode == Op_MinReductionV) {
    slt(AT, src, dst);
    masknez(dst, dst, AT);
    maskeqz(AT, src, AT);
    orr(dst, dst, AT);
  } else {
    reduce_ins_r(dst, dst, src, type, opcode);
  }
  switch (type) {
    case T_BYTE:  ext_w_b(dst, dst); break;
    case T_SHORT: ext_w_h(dst, dst); break;
    default:
      break;
  }
}

void C2_MacroAssembler::reduce(FloatRegister dst, FloatRegister src, FloatRegister vsrc, FloatRegister tmp, BasicType type, int opcode, int vector_size) {
  if (vector_size == 32) {
    switch (type) {
      case T_FLOAT:
        reduce_ins_f(dst, vsrc, src, type, opcode);
        xvpickve_w(tmp, vsrc, 1);
        reduce_ins_f(dst, tmp, dst, type, opcode);
        xvpickve_w(tmp, vsrc, 2);
        reduce_ins_f(dst, tmp, dst, type, opcode);
        xvpickve_w(tmp, vsrc, 3);
        reduce_ins_f(dst, tmp, dst, type, opcode);
        xvpickve_w(tmp, vsrc, 4);
        reduce_ins_f(dst, tmp, dst, type, opcode);
        xvpickve_w(tmp, vsrc, 5);
        reduce_ins_f(dst, tmp, dst, type, opcode);
        xvpickve_w(tmp, vsrc, 6);
        reduce_ins_f(dst, tmp, dst, type, opcode);
        xvpickve_w(tmp, vsrc, 7);
        reduce_ins_f(dst, tmp, dst, type, opcode);
        break;
      case T_DOUBLE:
        reduce_ins_f(dst, vsrc, src, type, opcode);
        xvpickve_d(tmp, vsrc, 1);
        reduce_ins_f(dst, tmp, dst, type, opcode);
        xvpickve_d(tmp, vsrc, 2);
        reduce_ins_f(dst, tmp, dst, type, opcode);
        xvpickve_d(tmp, vsrc, 3);
        reduce_ins_f(dst, tmp, dst, type, opcode);
        break;
      default:
        ShouldNotReachHere();
    }
  } else if (vector_size == 16) {
    switch (type) {
      case T_FLOAT:
        reduce_ins_f(dst, vsrc, src, type, opcode);
        vpermi_w(tmp, vsrc, 0b00000001);
        reduce_ins_f(dst, tmp, dst, type, opcode);
        vpermi_w(tmp, vsrc, 0b00000010);
        reduce_ins_f(dst, tmp, dst, type, opcode);
        vpermi_w(tmp, vsrc, 0b00000011);
        reduce_ins_f(dst, tmp, dst, type, opcode);
        break;
      case T_DOUBLE:
        reduce_ins_f(dst, vsrc, src, type, opcode);
        vpermi_w(tmp, vsrc, 0b00001110);
        reduce_ins_f(dst, tmp, dst, type, opcode);
        break;
      default:
        ShouldNotReachHere();
    }
  } else if (vector_size == 8) {
    assert(type == T_FLOAT, "must be");
    vpermi_w(tmp, vsrc, 0b00000001);
    reduce_ins_f(dst, vsrc, src, type, opcode);
    reduce_ins_f(dst, tmp, dst, type, opcode);
  } else {
    ShouldNotReachHere();
  }
}

void C2_MacroAssembler::vector_compare(FloatRegister dst, FloatRegister src1, FloatRegister src2, BasicType bt, int cond, int vector_size) {
  if (vector_size == 32) {
    if (bt == T_BYTE) {
      switch (cond) {
        case BoolTest::ne:  xvseq_b (dst, src1, src2); xvxori_b(dst, dst, 0xff); break;
        case BoolTest::eq:  xvseq_b (dst, src1, src2); break;
        case BoolTest::ge:  xvsle_b (dst, src2, src1); break;
        case BoolTest::gt:  xvslt_b (dst, src2, src1); break;
        case BoolTest::le:  xvsle_b (dst, src1, src2); break;
        case BoolTest::lt:  xvslt_b (dst, src1, src2); break;
        case BoolTest::uge: xvsle_bu(dst, src2, src1); break;
        case BoolTest::ugt: xvslt_bu(dst, src2, src1); break;
        case BoolTest::ule: xvsle_bu(dst, src1, src2); break;
        case BoolTest::ult: xvslt_bu(dst, src1, src2); break;
        default:
          ShouldNotReachHere();
      }
    } else if (bt == T_SHORT) {
      switch (cond) {
        case BoolTest::ne:  xvseq_h (dst, src1, src2); xvxori_b(dst, dst, 0xff); break;
        case BoolTest::eq:  xvseq_h (dst, src1, src2); break;
        case BoolTest::ge:  xvsle_h (dst, src2, src1); break;
        case BoolTest::gt:  xvslt_h (dst, src2, src1); break;
        case BoolTest::le:  xvsle_h (dst, src1, src2); break;
        case BoolTest::lt:  xvslt_h (dst, src1, src2); break;
        case BoolTest::uge: xvsle_hu(dst, src2, src1); break;
        case BoolTest::ugt: xvslt_hu(dst, src2, src1); break;
        case BoolTest::ule: xvsle_hu(dst, src1, src2); break;
        case BoolTest::ult: xvslt_hu(dst, src1, src2); break;
        default:
          ShouldNotReachHere();
      }
    } else if (bt == T_INT) {
      switch (cond) {
        case BoolTest::ne:  xvseq_w (dst, src1, src2); xvxori_b(dst, dst, 0xff); break;
        case BoolTest::eq:  xvseq_w (dst, src1, src2); break;
        case BoolTest::ge:  xvsle_w (dst, src2, src1); break;
        case BoolTest::gt:  xvslt_w (dst, src2, src1); break;
        case BoolTest::le:  xvsle_w (dst, src1, src2); break;
        case BoolTest::lt:  xvslt_w (dst, src1, src2); break;
        case BoolTest::uge: xvsle_wu(dst, src2, src1); break;
        case BoolTest::ugt: xvslt_wu(dst, src2, src1); break;
        case BoolTest::ule: xvsle_wu(dst, src1, src2); break;
        case BoolTest::ult: xvslt_wu(dst, src1, src2); break;
        default:
          ShouldNotReachHere();
      }
    } else if (bt == T_LONG) {
      switch (cond) {
        case BoolTest::ne:  xvseq_d (dst, src1, src2); xvxori_b(dst, dst, 0xff); break;
        case BoolTest::eq:  xvseq_d (dst, src1, src2); break;
        case BoolTest::ge:  xvsle_d (dst, src2, src1); break;
        case BoolTest::gt:  xvslt_d (dst, src2, src1); break;
        case BoolTest::le:  xvsle_d (dst, src1, src2); break;
        case BoolTest::lt:  xvslt_d (dst, src1, src2); break;
        case BoolTest::uge: xvsle_du(dst, src2, src1); break;
        case BoolTest::ugt: xvslt_du(dst, src2, src1); break;
        case BoolTest::ule: xvsle_du(dst, src1, src2); break;
        case BoolTest::ult: xvslt_du(dst, src1, src2); break;
        default:
          ShouldNotReachHere();
      }
    } else if (bt == T_FLOAT) {
      switch (cond) {
        case BoolTest::ne: xvfcmp_cune_s(dst, src1, src2); break;
        case BoolTest::eq: xvfcmp_ceq_s (dst, src1, src2); break;
        case BoolTest::ge: xvfcmp_cle_s (dst, src2, src1); break;
        case BoolTest::gt: xvfcmp_clt_s (dst, src2, src1); break;
        case BoolTest::le: xvfcmp_cle_s (dst, src1, src2); break;
        case BoolTest::lt: xvfcmp_clt_s (dst, src1, src2); break;
        default:
          ShouldNotReachHere();
      }
    } else if (bt == T_DOUBLE) {
      switch (cond) {
        case BoolTest::ne: xvfcmp_cune_d(dst, src1, src2); break;
        case BoolTest::eq: xvfcmp_ceq_d (dst, src1, src2); break;
        case BoolTest::ge: xvfcmp_cle_d (dst, src2, src1); break;
        case BoolTest::gt: xvfcmp_clt_d (dst, src2, src1); break;
        case BoolTest::le: xvfcmp_cle_d (dst, src1, src2); break;
        case BoolTest::lt: xvfcmp_clt_d (dst, src1, src2); break;
        default:
          ShouldNotReachHere();
      }
    } else {
      ShouldNotReachHere();
    }
  } else if (vector_size == 16 || vector_size == 8 || vector_size == 4) {
    if (bt == T_BYTE) {
      switch (cond) {
        case BoolTest::ne:  vseq_b (dst, src1, src2); vxori_b(dst, dst, 0xff); break;
        case BoolTest::eq:  vseq_b (dst, src1, src2); break;
        case BoolTest::ge:  vsle_b (dst, src2, src1); break;
        case BoolTest::gt:  vslt_b (dst, src2, src1); break;
        case BoolTest::le:  vsle_b (dst, src1, src2); break;
        case BoolTest::lt:  vslt_b (dst, src1, src2); break;
        case BoolTest::uge: vsle_bu(dst, src2, src1); break;
        case BoolTest::ugt: vslt_bu(dst, src2, src1); break;
        case BoolTest::ule: vsle_bu(dst, src1, src2); break;
        case BoolTest::ult: vslt_bu(dst, src1, src2); break;
        default:
          ShouldNotReachHere();
      }
    } else if (bt == T_SHORT) {
      switch (cond) {
        case BoolTest::ne:  vseq_h (dst, src1, src2); vxori_b(dst, dst, 0xff); break;
        case BoolTest::eq:  vseq_h (dst, src1, src2); break;
        case BoolTest::ge:  vsle_h (dst, src2, src1); break;
        case BoolTest::gt:  vslt_h (dst, src2, src1); break;
        case BoolTest::le:  vsle_h (dst, src1, src2); break;
        case BoolTest::lt:  vslt_h (dst, src1, src2); break;
        case BoolTest::uge: vsle_hu(dst, src2, src1); break;
        case BoolTest::ugt: vslt_hu(dst, src2, src1); break;
        case BoolTest::ule: vsle_hu(dst, src1, src2); break;
        case BoolTest::ult: vslt_hu(dst, src1, src2); break;
        default:
          ShouldNotReachHere();
      }
    } else if (bt == T_INT) {
      switch (cond) {
        case BoolTest::ne:  vseq_w (dst, src1, src2); vxori_b(dst, dst, 0xff); break;
        case BoolTest::eq:  vseq_w (dst, src1, src2); break;
        case BoolTest::ge:  vsle_w (dst, src2, src1); break;
        case BoolTest::gt:  vslt_w (dst, src2, src1); break;
        case BoolTest::le:  vsle_w (dst, src1, src2); break;
        case BoolTest::lt:  vslt_w (dst, src1, src2); break;
        case BoolTest::uge: vsle_wu(dst, src2, src1); break;
        case BoolTest::ugt: vslt_wu(dst, src2, src1); break;
        case BoolTest::ule: vsle_wu(dst, src1, src2); break;
        case BoolTest::ult: vslt_wu(dst, src1, src2); break;
        default:
          ShouldNotReachHere();
      }
    } else if (bt == T_LONG) {
      switch (cond) {
        case BoolTest::ne:  vseq_d (dst, src1, src2); vxori_b(dst, dst, 0xff); break;
        case BoolTest::eq:  vseq_d (dst, src1, src2); break;
        case BoolTest::ge:  vsle_d (dst, src2, src1); break;
        case BoolTest::gt:  vslt_d (dst, src2, src1); break;
        case BoolTest::le:  vsle_d (dst, src1, src2); break;
        case BoolTest::lt:  vslt_d (dst, src1, src2); break;
        case BoolTest::uge: vsle_du(dst, src2, src1); break;
        case BoolTest::ugt: vslt_du(dst, src2, src1); break;
        case BoolTest::ule: vsle_du(dst, src1, src2); break;
        case BoolTest::ult: vslt_du(dst, src1, src2); break;
        default:
          ShouldNotReachHere();
      }
    } else if (bt == T_FLOAT) {
      switch (cond) {
        case BoolTest::ne: vfcmp_cune_s(dst, src1, src2); break;
        case BoolTest::eq: vfcmp_ceq_s (dst, src1, src2); break;
        case BoolTest::ge: vfcmp_cle_s (dst, src2, src1); break;
        case BoolTest::gt: vfcmp_clt_s (dst, src2, src1); break;
        case BoolTest::le: vfcmp_cle_s (dst, src1, src2); break;
        case BoolTest::lt: vfcmp_clt_s (dst, src1, src2); break;
        default:
          ShouldNotReachHere();
      }
    } else if (bt == T_DOUBLE) {
      switch (cond) {
        case BoolTest::ne: vfcmp_cune_d(dst, src1, src2); break;
        case BoolTest::eq: vfcmp_ceq_d (dst, src1, src2); break;
        case BoolTest::ge: vfcmp_cle_d (dst, src2, src1); break;
        case BoolTest::gt: vfcmp_clt_d (dst, src2, src1); break;
        case BoolTest::le: vfcmp_cle_d (dst, src1, src2); break;
        case BoolTest::lt: vfcmp_clt_d (dst, src1, src2); break;
        default:
          ShouldNotReachHere();
      }
    } else {
      ShouldNotReachHere();
    }
  } else {
    ShouldNotReachHere();
  }
}

void C2_MacroAssembler::cmp_branch_short(int flag, Register op1, Register op2, Label& L, bool is_signed) {

    switch(flag) {
      case 0x01: //equal
          beq(op1, op2, L);
        break;
      case 0x02: //not_equal
          bne(op1, op2, L);
        break;
      case 0x03: //above
        if (is_signed)
          blt(op2, op1, L);
        else
          bltu(op2, op1, L);
        break;
      case 0x04: //above_equal
        if (is_signed)
          bge(op1, op2, L);
        else
          bgeu(op1, op2, L);
        break;
      case 0x05: //below
        if (is_signed)
          blt(op1, op2, L);
        else
          bltu(op1, op2, L);
        break;
      case 0x06: //below_equal
        if (is_signed)
          bge(op2, op1, L);
        else
          bgeu(op2, op1, L);
        break;
      default:
        Unimplemented();
    }
}

void C2_MacroAssembler::cmp_branch_long(int flag, Register op1, Register op2, Label* L, bool is_signed) {
    Label not_taken;

    switch(flag) {
      case 0x01: //equal
        bne(op1, op2, not_taken);
        break;
      case 0x02: //not_equal
        beq(op1, op2, not_taken);
        break;
      case 0x03: //above
        if (is_signed)
          bge(op2, op1, not_taken);
        else
          bgeu(op2, op1, not_taken);
        break;
      case 0x04: //above_equal
        if (is_signed)
          blt(op1, op2, not_taken);
        else
          bltu(op1, op2, not_taken);
        break;
      case 0x05: //below
        if (is_signed)
          bge(op1, op2, not_taken);
        else
          bgeu(op1, op2, not_taken);
        break;
      case 0x06: //below_equal
        if (is_signed)
          blt(op2, op1, not_taken);
        else
          bltu(op2, op1, not_taken);
        break;
      default:
        Unimplemented();
    }

    jmp_far(*L);
    bind(not_taken);
}

void C2_MacroAssembler::cmp_branchEqNe_off21(int flag, Register op1, Label& L) {
    switch(flag) {
      case 0x01: //equal
        beqz(op1, L);
        break;
      case 0x02: //not_equal
        bnez(op1, L);
        break;
      default:
        Unimplemented();
    }
}

bool C2_MacroAssembler::in_scratch_emit_size() {
  if (ciEnv::current()->task() != nullptr) {
    PhaseOutput* phase_output = Compile::current()->output();
    if (phase_output != nullptr && phase_output->in_scratch_emit_size()) {
      return true;
    }
  }
  return MacroAssembler::in_scratch_emit_size();
}
