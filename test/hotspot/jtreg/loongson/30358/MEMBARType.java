/*
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

public final class MEMBARType {
    public static final String DBARINSCODE   = "00 7238"; // dbar hint
    public static final String DBARSTR   = "dbar	0x";

    public static final String LoadLoad   = "15";
    public static final String LoadStore  = "16";
    public static final String StoreLoad  = "19";
    public static final String StoreStore = "1a";
    public static final String AnyAny     = "10";

    public static final String Acquire    = "14"; // LoadStore & LoadLoad
    public static final String Release    = "12"; // LoadStore & StoreStore
    public static final String Volatile   = StoreLoad;
}
