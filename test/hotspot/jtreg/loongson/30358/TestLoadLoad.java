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
 * @test TestLoadLoad
 * @summary Checks LoadLoad membar
 *
 * @library /test/lib
 *
 * @requires os.arch=="loongarch64"
 *
 * @run driver TestLoadLoad
 */

import java.util.ArrayList;
import java.util.Iterator;
import java.util.ListIterator;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

public class TestLoadLoad {

    public static void main(String[] args) throws Exception {
        ArrayList<String> command = new ArrayList<String>();
        command.add("-XX:+UnlockDiagnosticVMOptions");
        command.add("-XX:+PrintInterpreter");
        command.add("-version");

        ProcessBuilder pb = ProcessTools.createLimitedTestJavaProcessBuilder(command);
        OutputAnalyzer analyzer = new OutputAnalyzer(pb.start());

        analyzer.shouldHaveExitValue(0);
        System.out.println(analyzer.getOutput());
        checkMembarLoadLoad(analyzer);
    }

    private static void addInstrs(String line, ArrayList<String> instrs) {
        for (String instr : line.split("\\|")) {
            instrs.add(instr.trim());
        }
    }

    // The output with hsdis library is:
    // ---------------------------------------------------------------------
    // fast_agetfield  203 fast_agetfield  [0x000000ffe8436f00, 0x000000ffe8436f68]  104 bytes
    //
    // --------------------------------------------------------------------------------
    //   0x000000ffe8436f00:   ld.d    $a0,$sp,0                   ;;@FILE: /home/sunguoyun/jdk-ls/src/hotspot/share/interpreter/templateInterpreterGenerator.cpp
    //                                                             ;;  357:     case atos: vep = __ pc(); __ pop(atos); aep = __ pc(); generate_and_dispatch(t); break;
    //   0x000000ffe8436f04:   addi.d  $sp,$sp,8(0x8)
    //   0x000000ffe8436f08:   ld.hu   $t2,$s0,1(0x1)              ;;  357:     case atos: vep = __ pc(); __ pop(atos); aep = __ pc(); generate_and_dispatch(t); break;
    //                                                             ;;  378:   __ verify_FPU(1, t->tos_in());
    //                                                             ;;  391:     __ dispatch_prolog(tos_out, step);
    //   0x000000ffe8436f0c:   ld.d    $t3,$fp,-72(0xfb8)
    //   0x000000ffe8436f10:   slli.d  $t2,$t2,0x2
    //   0x000000ffe8436f14:   dbar    0x15
    //
    // The output no hsdis library is:
    // 0x000000ffe7b58e80: 6400 c028 | 6320 c002 | ee06 402a | cfe2 fe28 | ce09 4100 | 1500 7238 | d33d 2d00 | 6e22 c128
    // 0x000000ffe7b58ea0: 7342 c128 | 1440 0014 | 94ce 1400 | 800a 0058 | 1000 7238 | 9300 8028 | 84b8 1000 | 8400 802a

    private static void checkMembarLoadLoad(OutputAnalyzer output) {
        Iterator<String> iter = output.asLines().listIterator();

        String match = skipTo(iter, "fast_agetfield  203 fast_agetfield");
        if (match == null) {
            throw new RuntimeException("Missing interpreter output");
        }

        ArrayList<String> instrs = new ArrayList<String>();
        String line = null;
        while (iter.hasNext()) {
            line = iter.next();
            if (line.contains("fast_bgetfield")) {
                break;
            }
            if (line.contains("0x")) {
                addInstrs(line, instrs);
            }
        }

        ListIterator<String> instrReverseIter = instrs.listIterator(instrs.size());
        boolean foundMembarInst = false;

        while (instrReverseIter.hasPrevious()) {
            String inst = instrReverseIter.previous();
            if (inst.endsWith(MEMBARType.LoadLoad + MEMBARType.DBARINSCODE) || inst.contains(MEMBARType.DBARSTR + MEMBARType.LoadLoad)) {
                foundMembarInst = true;
                break;
            }
        }

        if (foundMembarInst == false) {
            throw new RuntimeException("No founed MembarRelease instruction (0x" + MEMBARType.LoadLoad + ")!\n");
        }
    }

    private static String skipTo(Iterator<String> iter, String substring) {
        while (iter.hasNext()) {
            String nextLine = iter.next();
            if (nextLine.contains(substring)) {
                return nextLine;
            }
        }
        return null;
    }

}
