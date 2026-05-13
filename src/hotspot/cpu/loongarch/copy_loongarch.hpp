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

#ifndef CPU_LOONGARCH_COPY_LOONGARCH_HPP
#define CPU_LOONGARCH_COPY_LOONGARCH_HPP

friend class StubGenerator;

typedef void (*CopyByte)(const void*, void*, size_t);
typedef void (*CopyShort)(const jshort*, jshort*, size_t);
typedef void (*CopyInt)(const jint*, jint*, size_t);
typedef void (*CopyLong)(const jlong*, jlong*, size_t);
typedef void (*CopyOop)(const oop*, oop*, size_t);
typedef void (*CopyHeapWord)(const HeapWord*, HeapWord*, size_t);
typedef void (*FillByte)(void*, jubyte, size_t);
typedef void (*FillHeapWord)(HeapWord*, julong, size_t);

static CopyHeapWord _conjoint_words;
static CopyHeapWord _disjoint_words;
static CopyHeapWord _disjoint_words_atomic;
static CopyHeapWord _aligned_conjoint_words;
static CopyHeapWord _aligned_disjoint_words;
static CopyByte _conjoint_bytes;
static CopyByte _conjoint_bytes_atomic;
static CopyShort _conjoint_jshorts_atomic;
static CopyInt _conjoint_jints_atomic;
static CopyLong _conjoint_jlongs_atomic;
static CopyOop _conjoint_oops_atomic;
static CopyHeapWord _arrayof_conjoint_bytes;
static CopyHeapWord _arrayof_conjoint_jshorts;
static CopyHeapWord _arrayof_conjoint_jints;
static CopyHeapWord _arrayof_conjoint_jlongs;
static CopyHeapWord _arrayof_conjoint_oops;
static FillHeapWord _fill_to_words;
static FillHeapWord _fill_to_aligned_words;
static FillByte _fill_to_bytes;

// Inline functions for memory copy and fill.

// Contains inline asm implementations
#include OS_CPU_HEADER_INLINE(copy)

#endif //CPU_LOONGARCH_COPY_LOONGARCH_HPP
