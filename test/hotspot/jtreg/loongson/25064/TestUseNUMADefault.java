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
 * @test TestUseNUMADefault
 * @summary
 *     Tests that UseNUMA should be enabled by default for all collectors
 *     on machines with multiple NUMA nodes, unless NUMA init fails or
 *     the InitialHeapSize is too small.
 * @library /test/lib
 * @library /
 * @requires os.family == "linux"
 * @requires os.arch == "loongarch64"
 * @requires vm.gc.G1
 * @run main/othervm TestUseNUMADefault -XX:+UseG1GC
 */

/**
 * @test TestUseNUMADefault
 * @summary
 *     Tests that UseNUMA should be enabled by default for all collectors
 *     on machines with multiple NUMA nodes, unless NUMA init fails or
 *     the InitialHeapSize is too small.
 * @library /test/lib
 * @library /
 * @requires os.family == "linux"
 * @requires os.arch == "loongarch64"
 * @requires vm.gc.Parallel
 * @run main/othervm TestUseNUMADefault -XX:+UseParallelGC
 */

/**
 * @test TestUseNUMADefault
 * @summary
 *     Tests that UseNUMA should be enabled by default for all collectors
 *     on machines with multiple NUMA nodes, unless NUMA init fails or
 *     the InitialHeapSize is too small.
 * @library /test/lib
 * @library /
 * @requires os.family == "linux"
 * @requires os.arch == "loongarch64"
 * @requires vm.gc.Z
 * @run main/othervm TestUseNUMADefault -XX:+UseZGC
 */

/**
 * @test TestUseNUMADefault
 * @summary
 *     Tests that UseNUMA should be enabled by default for all collectors
 *     on machines with multiple NUMA nodes, unless NUMA init fails or
 *     the InitialHeapSize is too small.
 * @library /test/lib
 * @library /
 * @requires os.family == "linux"
 * @requires os.arch == "loongarch64"
 * @requires vm.gc.Shenandoah
 * @run main/othervm TestUseNUMADefault -XX:+UseShenandoahGC
 */

public class TestUseNUMADefault {

    public static void main(String[] args) throws Exception {
        String gcFlag = args[0];
        int nodes = NUMAHelper.getNUMANodes();

        NUMAHelper.judge(NUMAHelper.invokeJvm(
            gcFlag,
            "-XX:+PrintFlagsFinal",
            "-version"), nodes, null, false);

        NUMAHelper.judge(NUMAHelper.invokeJvm(
            gcFlag,
            "-XX:NUMAMinHeapSizePerNode=127m",
            "-XX:+PrintFlagsFinal",
            "-version"), nodes, null, false);

        NUMAHelper.judge(NUMAHelper.invokeJvm(
            gcFlag,
            "-XX:NUMAMinHeapSizePerNode=128m",
            "-XX:+PrintFlagsFinal",
            "-version"), nodes, null, false);

        NUMAHelper.judge(NUMAHelper.invokeJvm(
            gcFlag,
            "-XX:NUMAMinHeapSizePerNode=129m",
            "-XX:+PrintFlagsFinal",
            "-version"), nodes, null, false);

        NUMAHelper.judge(NUMAHelper.invokeJvm(
            gcFlag,
            "-XX:NUMAMinHeapSizePerNode=128m",
            "-XX:InitialHeapSize=" + 127 * nodes + "m",
            "-XX:+PrintFlagsFinal",
            "-version"), nodes, 133169152L * nodes, false);

        NUMAHelper.judge(NUMAHelper.invokeJvm(
            gcFlag,
            "-XX:NUMAMinHeapSizePerNode=128m",
            "-XX:InitialHeapSize=" + 128 * nodes + "m",
            "-XX:+PrintFlagsFinal",
            "-version"), nodes, 134217728L * nodes, false);

        NUMAHelper.judge(NUMAHelper.invokeJvm(
            gcFlag,
            "-XX:NewSize=" + 1 * nodes + "m",
            "-XX:+PrintFlagsFinal",
            "-version"), nodes, null, true);

        NUMAHelper.judge(NUMAHelper.invokeJvm(
            gcFlag,
            "-XX:NewSize=" + 2 * nodes + "m",
            "-XX:+PrintFlagsFinal",
            "-version"), nodes, null, true);

        NUMAHelper.judge(NUMAHelper.invokeJvm(
            gcFlag,
            "-XX:NewSize=" + 2 * nodes + "m",
            "-XX:NUMAMinHeapSizePerNode=128m",
            "-XX:InitialHeapSize=" + 127 * nodes + "m",
            "-XX:+PrintFlagsFinal",
            "-version"), nodes, 133169152L * nodes, true);

        NUMAHelper.judge(NUMAHelper.invokeJvm(
            gcFlag,
            "-XX:NewSize=" + 2 * nodes + "m",
            "-XX:NUMAMinHeapSizePerNode=128m",
            "-XX:InitialHeapSize=" + 128 * nodes + "m",
            "-XX:+PrintFlagsFinal",
            "-version"), nodes, 134217728L * nodes, true);
    }
}
