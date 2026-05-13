/*
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

/**
 * @test
 * @summary test c2 or2s
 *
 * @run main/othervm -Xcomp -XX:-TieredCompilation Test25443
 */
public class Test25443 {
    static short test_ori2s(int v1) {
        short t = (short)(v1 | 0x14);
        return t;
    }

    static short test_or2s(int v1, int v2) {
        short t = (short)(v1 | v2);
        return t;
    }

    static short ret;
    public static void main(String[] args) {
        for (int i = 0; i < 12000; i++) { //warmup
            test_ori2s(0x333300);
            test_or2s(0x333300, 0x14);
        }

        if ( (test_ori2s(0x333300) == 0x3314)
            && (test_or2s(0x333300, 0x14) == 0x3314)
            && (test_or2s(0x333300, 0x1000) == 0x3300)
            && (test_or2s(0x333300, 0x8000) == 0xffffb300)) {
            System.out.println("TEST PASSED");
        } else {
            throw new AssertionError("Not be expected results");
        }
    }
}
