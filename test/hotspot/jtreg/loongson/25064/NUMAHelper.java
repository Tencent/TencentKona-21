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

import java.io.File;
import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

public class NUMAHelper {

    private static final String INITIAL_HEAP_SIZE_PATTERN
        = "\\bInitialHeapSize\\b.*?=.*?([0-9]+)";

    private static final String MIN_HEAP_SIZE_PER_NODE_PATTERN
        = "\\bNUMAMinHeapSizePerNode\\b.*?=.*?([0-9]+)";

    private static final String NEW_SIZE_PATTERN
        = "\\bNewSize\\b.*?=.*?([0-9]+)";

    static long getInitialHeapSize(OutputAnalyzer output) {
        String matched = output.firstMatch(INITIAL_HEAP_SIZE_PATTERN, 1);
        return Long.parseLong(matched);
    }

    static long getMinHeapSizePerNode(OutputAnalyzer output) {
        String matched = output.firstMatch(MIN_HEAP_SIZE_PER_NODE_PATTERN, 1);
        return Long.parseLong(matched);
    }

    static long getNewSize(OutputAnalyzer output) {
        String matched = output.firstMatch(NEW_SIZE_PATTERN, 1);
        return Long.parseLong(matched);
    }

    static OutputAnalyzer invokeJvm(String... args) throws Exception {
        ProcessBuilder pb = ProcessTools.createTestJavaProcessBuilder(args);
        return new OutputAnalyzer(pb.start());
    }

    static int getNUMANodes() throws Exception {
        String command = "ls /sys/devices/system/node | grep '^node' | wc -l";
        ProcessBuilder processBuilder = new ProcessBuilder("sh", "-c", command);

        Process process = processBuilder.start();
        BufferedReader reader = new BufferedReader(
                new InputStreamReader(process.getInputStream()));
        String line = reader.readLine();
        process.destroy();

        int nodes = Integer.parseInt(line);
        System.out.println("Number of NUMA nodes: " + nodes);
        return nodes;
    }

    static void judge(OutputAnalyzer o, int nodes,
                      Long manualInitialHeapSize,
                      boolean manualNewSize) {
        long initialHeapSize;
        if (manualInitialHeapSize != null) {
            initialHeapSize = (long) manualInitialHeapSize;
        } else { // InitialHeapSize may be aligned up via GC
            initialHeapSize = NUMAHelper.getInitialHeapSize(o);
        }
        long minHeapSizePerNode = NUMAHelper.getMinHeapSizePerNode(o);
        long newSize = NUMAHelper.getNewSize(o);

        if (nodes <= 1) { // not supported numa or only one numa node
            o.shouldMatch("bool UseNUMA[ ]+= false");
        } else if (initialHeapSize < minHeapSizePerNode * nodes) {
            o.shouldMatch("bool UseNUMA[ ]+= false");
        } else if (manualNewSize && newSize < 1363144 * nodes) {
            o.shouldMatch("bool UseNUMA[ ]+= false");
        } else {
            o.shouldMatch("bool UseNUMA[ ]+= true");
        }
    }
}
