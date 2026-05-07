/*
 * Copyright (c) 2021, Loongson Technology. All rights reserved.
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

package org.openjdk.bench.loongarch;

import org.openjdk.jmh.annotations.Benchmark;

public class C2Memory {
    public static int sum;
    public static int array1[] = new int[0x8000];
    public static int array2[] = new int[0x8000];

    @Benchmark
    public void testMethod() {
       for (int i = 0; i<10000;i++) {
         sum = array1[0x7fff] + array2[0x1f0];
         array1[0x7fff] += array2[0x1f0];
       }
    }

    @Benchmark
    public void testBasePosIndexOffset() {
        int xstart = 30000;
        long carry = 63;

        for (int j=xstart; j >= 0; j--) {
            array2[j] = array1[xstart];
        }

        array2[xstart] = (int)carry;
    }

    public static byte b_array1[] = new byte[0x8000];
    public static byte b_array2[] = new byte[0x8000];

    @Benchmark
    public void testBaseIndexOffset() {
        int xstart = 10000;
        byte carry = 63;

        for (int j=xstart; j >= 0; j--) {
            b_array2[j] = b_array1[xstart];
        }

        b_array2[xstart] = carry;
    }
}
