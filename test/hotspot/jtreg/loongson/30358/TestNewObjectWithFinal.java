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
 * @test TestNewObjectWithFinal
 * @summary Checks membars for a object with final val create
 *
 * for c1 new object with final, two StoreStore membar will be insert
 * store final val
 * membar_storestore
 * store obj
 * membar_storestore
 *
 * for c2 new object with final, one Release membar will be insert
 * store final val
 * store obj
 * membar_release
 *
 * @library /test/lib
 *
 * @requires os.arch=="loongarch64"
 *
 * @run driver TestNewObjectWithFinal c1
 * @run driver TestNewObjectWithFinal c2
 */

import java.util.ArrayList;
import java.util.Iterator;
import java.util.ListIterator;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

public class TestNewObjectWithFinal {

    public static void main(String[] args) throws Exception {
        String compiler = args[0];
        ArrayList<String> command = new ArrayList<String>();
        command.add("-XX:-BackgroundCompilation");
        command.add("-XX:+UnlockDiagnosticVMOptions");
        command.add("-XX:+PrintAssembly");

        if (compiler.equals("c2")) {
            command.add("-XX:-TieredCompilation");
        } else if (compiler.equals("c1")) {
            command.add("-XX:TieredStopAtLevel=1");
        } else {
            throw new RuntimeException("Unknown compiler: " + compiler);
        }
        command.add("-XX:CompileCommand=compileonly," + Launcher.class.getName() + "::" + "test");
        command.add(Launcher.class.getName());

        ProcessBuilder pb = ProcessTools.createLimitedTestJavaProcessBuilder(command);
        OutputAnalyzer analyzer = new OutputAnalyzer(pb.start());

        analyzer.shouldHaveExitValue(0);

        System.out.println(analyzer.getOutput());

        if (compiler.equals("c1")) {
            checkMembarStoreStore(analyzer);
        } else if (compiler.equals("c2")) {
            checkMembarRelease(analyzer);
        }
    }

    private static void addInstrs(String line, ArrayList<String> instrs) {
        for (String instr : line.split("\\|")) {
            instrs.add(instr.trim());
        }
    }

    // ----------------------------------- Assembly -----------------------------------
    //
    // Compiled method (c2)     950   24             TestNewObjectWithFinal$Launcher::test (8 bytes)
    //
    // [Constant Pool (empty)]
    //
    // [MachCode]
    // [Verified Entry Point]
    //   # {method} {0x000000ffd06033f0} 'test' '()LTestNewObjectWithFinal$Launcher;' in 'TestNewObjectWithFinal$Launcher'
    //   0x000000ffed0be59c: 0c24 8003
    //
    //   0x000000ffed0be5a0: ;*invokespecial <init> {reexecute=0 rethrow=0 return_oop=0}
    //                       ; - TestNewObjectWithFinal$Launcher::test@4 (line 187)
    //   0x000000ffed0be5a0: 8c30 8029
    //
    //   0x000000ffed0be5a4: ;*synchronization entry
    //                       ; - TestNewObjectWithFinal$Launcher::<init>@-1 (line 176)
    //                       ; - TestNewObjectWithFinal$Launcher::test@4 (line 187)
    //   0x000000ffed0be5a4: 1200 7238 | 7640 c028 | 6160 c028 | 6380 c002
    //
    //   0x000000ffed0be5b4: ;   {poll_return}
    //   0x000000ffed0be5b4: b323 cf28 | 630a 006c | 0040 0050 | 2000 004c
    //   ...
    // [Stub Code]
    //
    //
    // The output with hsdis library is:
    //
    // 0x000000ffed0be5a4:   dbar	0x12                        ;*synchronization entry
    //                                                           ; - TestNewObjectWithFinal$Launcher::<init>@-1 (line 227)
    //
    private static void checkMembarRelease(OutputAnalyzer output) {
        Iterator<String> iter = output.asLines().listIterator();

        String match = skipTo(iter, "'test' '()LTestNewObjectWithFinal$Launcher");
        if (match == null) {
            throw new RuntimeException("Missing compiler c2 output");
        }

        ArrayList<String> instrs = new ArrayList<String>();
        String line = null;
        while (iter.hasNext()) {
            line = iter.next();
            if (line.contains("[Stub Code]")) {
                break;
            }
            if (line.contains("0x")/* && !line.contains(";")*/) {
                addInstrs(line, instrs);
            }
        }

        ListIterator<String> instrReverseIter = instrs.listIterator(instrs.size());
        boolean foundMembarInst = false;

        while (instrReverseIter.hasPrevious()) {
            String inst = instrReverseIter.previous();
            if (inst.endsWith(MEMBARType.Release + MEMBARType.DBARINSCODE) || inst.contains(MEMBARType.DBARSTR + MEMBARType.Release)) {
                foundMembarInst = true;
                break;
            }
        }

        if (foundMembarInst == false) {
            throw new RuntimeException("No founed MembarRelease instruction (0x" + MEMBARType.Release + ")!\n");
        }
    }

    // ============================= C1-compiled nmethod ==============================
    // ----------------------------------- Assembly -----------------------------------
    //
    // Compiled method (c1)     948   24       1       TestNewObjectWithFinal$Launcher::test (8 bytes)
    //   0x000000ffe903feb8: ;*new {reexecute=0 rethrow=0 return_oop=0}
    //                       ; - TestNewObjectWithFinal$Launcher::test@0 (line 190)
    //   0x000000ffe903feb8: 1a00 7238 | 0524 8003
    //
    //   0x000000ffe903fec0: ;*putfield val_i {reexecute=0 rethrow=0 return_oop=0}
    //                       ; - TestNewObjectWithFinal$Launcher::<init>@7 (line 180)
    //                       ; - TestNewObjectWithFinal$Launcher::test@4 (line 190)
    //   0x000000ffe903fec0: 8530 8029 | 1a00 7238 | 7600 c128 | 6120 c128 | 6340 c102
    //
    //   0x000000ffe903fed4: ;   {poll_return}
    // [Exception Handler]
    //
    //
    // The output with hsdis library is:
    //
    // 0x000000ffed03feb8:   dbar	0x1a                        ;*new {reexecute=0 rethrow=0 return_oop=0}
    //                                                           ; - TestNewObjectWithFinal$Launcher::test@0 (line 225)
    // 0x000000ffed03febc:   ori	$a1,$zero,0x9
    // 0x000000ffed03fec0:   st.w	$a1,$a0,12(0xc)             ;*putfield val_i {reexecute=0 rethrow=0 return_oop=0}
    //                                                           ; - TestNewObjectWithFinal$Launcher::<init>@7 (line 215)
    //                                                           ; - TestNewObjectWithFinal$Launcher::test@4 (line 225)
    // 0x000000ffed03fec4:   dbar	0x1a
    //
    private static void checkMembarStoreStore(OutputAnalyzer output) {
        Iterator<String> iter = output.asLines().listIterator();

        String match = skipTo(iter, "TestNewObjectWithFinal$Launcher::test (8 bytes)");
        if (match == null) {
            throw new RuntimeException("Missing compiler c1 output");
        }

        ArrayList<String> instrs = new ArrayList<String>();
        String line = null;
        boolean hasHexInstInOutput = false;
        while (iter.hasNext()) {
            line = iter.next();
            if (line.contains("[Exception Handler]")) {
                break;
            }
            if (line.contains("0x")/* && !line.contains(";")*/) {
                addInstrs(line, instrs);
            }
        }

        ListIterator<String> instrReverseIter = instrs.listIterator(instrs.size());
        int foundMembarInst = 0;

        while (instrReverseIter.hasPrevious()) {
            String inst = instrReverseIter.previous();
            if (inst.endsWith(MEMBARType.StoreStore + MEMBARType.DBARINSCODE) || inst.contains(MEMBARType.DBARSTR + MEMBARType.StoreStore)) {
                ++foundMembarInst;
            }
        }

        if (foundMembarInst < 2) {
            throw new RuntimeException("No founed MembarStoreStore instruction (" + MEMBARType.StoreStore + ")! foundMembarInst=" + foundMembarInst + "\n");
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

    static class Launcher {
        final int val_i = 0x9;
        static Launcher l;
        public static void main(final String[] args) throws Exception {
            int end = 20_000;

            for (int i=0; i < end; i++) {
                l = test();
            }
        }
        static Launcher test() {
            return new Launcher();
        }
    }
}
