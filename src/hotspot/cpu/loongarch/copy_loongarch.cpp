/*
 * Copyright (c) 2003, 2010, Oracle and/or its affiliates. All rights reserved.
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

#include "utilities/copy.hpp"

// Template for atomic, element-wise copy.
template <class T>
static void copy_conjoint_atomic(const T* from, T* to, size_t count) {
  if (from > to) {
    while (count-- > 0) {
      // Copy forwards
      *to++ = *from++;
    }
  } else {
    from += count - 1;
    to   += count - 1;
    while (count-- > 0) {
      // Copy backwards
      *to-- = *from--;
    }
  }
}

static void c_conjoint_words(const HeapWord* from, HeapWord* to, size_t count) {
  (void)memmove(to, from, count * HeapWordSize);
}

static void c_disjoint_words(const HeapWord* from, HeapWord* to, size_t count) {
  (void)memcpy(to, from, count * HeapWordSize);
}

static void c_disjoint_words_atomic(const HeapWord* from, HeapWord* to, size_t count) {
  while (count-- > 0) {
    *to++ = *from++;
  }
}

static void c_aligned_conjoint_words(const HeapWord* from, HeapWord* to, size_t count) {
  c_conjoint_words(from, to, count);
}

static void c_aligned_disjoint_words(const HeapWord* from, HeapWord* to, size_t count) {
  c_disjoint_words(from, to, count);
}

static void c_conjoint_bytes(const void* from, void* to, size_t count) {
  (void)memmove(to, from, count);
}

static void c_conjoint_bytes_atomic(const void* from, void* to, size_t count) {
  c_conjoint_bytes(from, to, count);
}

static void c_conjoint_jshorts_atomic(const jshort* from, jshort* to, size_t count) {
  copy_conjoint_atomic<jshort>(from, to, count);
}

static void c_conjoint_jints_atomic(const jint* from, jint* to, size_t count) {
  copy_conjoint_atomic<jint>(from, to, count);
}

static void c_conjoint_jlongs_atomic(const jlong* from, jlong* to, size_t count) {
  copy_conjoint_atomic<jlong>(from, to, count);
}

static void c_conjoint_oops_atomic(const oop* from, oop* to, size_t count) {
  assert(HeapWordSize == BytesPerOop, "heapwords and oops must be the same size");
  copy_conjoint_atomic<oop>(from, to, count);
}

static void c_arrayof_conjoint_bytes(const HeapWord* from, HeapWord* to, size_t count) {
  c_conjoint_bytes_atomic(from, to, count);
}

static void c_arrayof_conjoint_jshorts(const HeapWord* from, HeapWord* to, size_t count) {
  c_conjoint_jshorts_atomic((jshort*)from, (jshort*)to, count);
}

static void c_arrayof_conjoint_jints(const HeapWord* from, HeapWord* to, size_t count) {
  c_conjoint_jints_atomic((jint*)from, (jint*)to, count);
}

static void c_arrayof_conjoint_jlongs(const HeapWord* from, HeapWord* to, size_t count) {
  c_conjoint_jlongs_atomic((jlong*)from, (jlong*)to, count);
}

static void c_arrayof_conjoint_oops(const HeapWord* from, HeapWord* to, size_t count) {
  assert(BytesPerLong == BytesPerOop, "jlongs and oops must be the same size");
  c_conjoint_oops_atomic((oop*)from, (oop*)to, count);
}

static void c_fill_to_words(HeapWord* tohw, julong value, size_t count) {
  julong* to = (julong*) tohw;
  while (count-- > 0) {
    *to++ = value;
  }
}

static void c_fill_to_aligned_words(HeapWord* tohw, julong value, size_t count) {
  c_fill_to_words(tohw, value, count);
}

static void c_fill_to_bytes(void* to, jubyte value, size_t count) {
  (void)memset(to, value, count);
}

Copy::CopyHeapWord Copy::_conjoint_words = c_conjoint_words;
Copy::CopyHeapWord Copy::_disjoint_words = c_disjoint_words;
Copy::CopyHeapWord Copy::_disjoint_words_atomic = c_disjoint_words_atomic;
Copy::CopyHeapWord Copy::_aligned_conjoint_words = c_aligned_conjoint_words;
Copy::CopyHeapWord Copy::_aligned_disjoint_words = c_aligned_disjoint_words;
Copy::CopyByte Copy::_conjoint_bytes = c_conjoint_bytes;
Copy::CopyByte Copy::_conjoint_bytes_atomic = c_conjoint_bytes_atomic;
Copy::CopyShort Copy::_conjoint_jshorts_atomic = c_conjoint_jshorts_atomic;
Copy::CopyInt Copy::_conjoint_jints_atomic = c_conjoint_jints_atomic;
Copy::CopyLong Copy::_conjoint_jlongs_atomic = c_conjoint_jlongs_atomic;
Copy::CopyOop Copy::_conjoint_oops_atomic = c_conjoint_oops_atomic;
Copy::CopyHeapWord Copy::_arrayof_conjoint_bytes = c_arrayof_conjoint_bytes;
Copy::CopyHeapWord Copy::_arrayof_conjoint_jshorts = c_arrayof_conjoint_jshorts;
Copy::CopyHeapWord Copy::_arrayof_conjoint_jints = c_arrayof_conjoint_jints;
Copy::CopyHeapWord Copy::_arrayof_conjoint_jlongs = c_arrayof_conjoint_jlongs;
Copy::CopyHeapWord Copy::_arrayof_conjoint_oops = c_arrayof_conjoint_oops;
Copy::FillHeapWord Copy::_fill_to_words = c_fill_to_words;
Copy::FillHeapWord Copy::_fill_to_aligned_words = c_fill_to_aligned_words;
Copy::FillByte Copy::_fill_to_bytes = c_fill_to_bytes;
