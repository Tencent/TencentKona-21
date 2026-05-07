/*
 * Copyright (c) 2015, 2018, Loongson Technology. All rights reserved.
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
 * @summary Divide by zero
 *
 * @run main/othervm -Xint Test7423
 * @run main/othervm -Xcomp Test7423
 */
public class Test7423 {

  private static int divInt(int n) {
    int a = 1 / n;
    return a;
  }

  private static long divLong(long n) {
    long a = (long)1 / n;
    return a;
  }

  public static void main(String[] args) throws Exception {

    try {
      for (int i = 0; i < 20000; i++) {
        if (i == 18000) {
          divInt(0);
          divLong((long)0);
        } else {
          divInt(1);
          divLong((long)1);
        }
      }
    } catch (java.lang.ArithmeticException exc) {
      System.out.println("expected-exception " + exc);
    }
  }

}
