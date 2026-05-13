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

/**
 * @test TestUseNUMADisabled
 * @summary
 *     If -XX:-UseNUMA is specified at startup, then UseNUMA should be
 *     disabled for all collectors on machines with any number of NUMA
 *     nodes ergonomically.
 * @library /test/lib
 * @library /
 * @requires os.family == "linux"
 * @requires os.arch == "loongarch64"
 * @requires vm.gc.G1
 * @run main/othervm TestUseNUMADisabled -XX:+UseG1GC
 */

/**
 * @test TestUseNUMADisabled
 * @summary
 *     If -XX:-UseNUMA is specified at startup, then UseNUMA should be
 *     disabled for all collectors on machines with any number of NUMA
 *     nodes ergonomically.
 * @library /test/lib
 * @library /
 * @requires os.family == "linux"
 * @requires os.arch == "loongarch64"
 * @requires vm.gc.Parallel
 * @run main/othervm TestUseNUMADisabled -XX:+UseParallelGC
 */

/**
 * @test TestUseNUMADisabled
 * @summary
 *     If -XX:-UseNUMA is specified at startup, then UseNUMA should be
 *     disabled for all collectors on machines with any number of NUMA
 *     nodes ergonomically.
 * @library /test/lib
 * @library /
 * @requires os.family == "linux"
 * @requires os.arch == "loongarch64"
 * @requires vm.gc.Z
 * @run main/othervm TestUseNUMADisabled -XX:+UseZGC
 */

/**
 * @test TestUseNUMADisabled
 * @summary
 *     If -XX:-UseNUMA is specified at startup, then UseNUMA should be
 *     disabled for all collectors on machines with any number of NUMA
 *     nodes ergonomically.
 * @library /test/lib
 * @library /
 * @requires os.family == "linux"
 * @requires os.arch == "loongarch64"
 * @requires vm.gc.Shenandoah
 * @run main/othervm TestUseNUMADisabled -XX:+UseShenandoahGC
 */

import jdk.test.lib.process.OutputAnalyzer;

public class TestUseNUMADisabled {
    public static void main(String[] args) throws Exception {
        String gcFlag = args[0];
        OutputAnalyzer o = NUMAHelper.invokeJvm(
            gcFlag,
            "-XX:-UseNUMA",
            "-XX:+PrintFlagsFinal",
            "-version");

        o.shouldMatch("bool UseNUMA[ ]+= false");
    }
}
