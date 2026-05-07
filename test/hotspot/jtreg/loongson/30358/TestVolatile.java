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
 * @test TestVolatile
 * @summary Checks membars for a volatile field load and store
 *
 * for a volatile wirte ideal:
 * MemBarRelease
 * store
 * MemBarVolatile (NOTE: c1 used AnyAny instead MembarVolatile)
 *
 * for a volatile read ideal:
 * load
 * MemBarAcquire
 *
 * @library /test/lib
 *
 * @requires os.arch=="loongarch64"
 *
 * @run driver TestVolatile c1
 * @run driver TestVolatile c2
 */
import java.util.ArrayList;
import java.util.Iterator;
import java.util.ListIterator;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

public class TestVolatile {

    public static void main(String[] args) throws Exception {
        String compiler = args[0];
        ArrayList<String> command = new ArrayList<String>();
        command.add("-XX:-BackgroundCompilation");
        command.add("-XX:+UnlockDiagnosticVMOptions");
        command.add("-XX:+PrintAssembly");
        command.add("-Xcomp");

        if (compiler.equals("c2")) {
            command.add("-XX:-TieredCompilation");
            command.add("-XX:+UseBarriersForVolatile");
        } else if (compiler.equals("c1")) {
            command.add("-XX:TieredStopAtLevel=1");
        } else if (compiler.equals("int")) {
            command.add("-Xint");
            command.add("-XX:+PrintInterpreter");
        } else {
            throw new RuntimeException("Unknown compiler: " + compiler);
        }

        command.add("-XX:CompileCommand=compileonly," + Launcher.class.getName() + "::" + "*");
        command.add("-XX:CompileCommand=dontinline," + Launcher.class.getName() + "::" + "*");
        command.add(Launcher.class.getName());

        ProcessBuilder pb = ProcessTools.createLimitedTestJavaProcessBuilder(command);

        OutputAnalyzer analyzer = new OutputAnalyzer(pb.start());

        analyzer.shouldHaveExitValue(0);

        System.out.println(analyzer.getOutput());

        if (compiler.equals("c1")) {
            checkC1VolatileRead(analyzer);
            checkC1VolatileWrite(analyzer);
        } else if (compiler.equals("c2")) {
            checkC2VolatileRead(analyzer);
            checkC2VolatileWrite(analyzer);
        }
    }

    private static void addInstrs(String line, ArrayList<String> instrs) {
        for (String instr : line.split("\\|")) {
            instrs.add(instr.trim());
        }
    }

    // ----------------------------------- Assembly -----------------------------------
    //
    // Compiled method (c1)     976   24       1       TestVolatile$Launcher::main (13 bytes)
    //   # {method} {0x000000ffc8700308} 'main' '([Ljava/lang/String;)V' in 'TestVolatile$Launcher'
    //   0x000000fff0147e28: ;*getfield flags {reexecute=0 rethrow=0 return_oop=0}
    //                       ; - TestVolatile$Launcher::main@6 (line 296)
    //   0x000000fff0147e28: 1400 7238
    //
    // The output with hsdis library is:
    //
    // 0x000000ffe903fe28:   dbar	0x14                        ;*getfield flags {reexecute=0 rethrow=0 return_oop=0}
    //                                                          ; - TestVolatile$Launcher::main@6 (line 322)
    // 0x000000ffe903fe2c:   st.w	$a1,$a0,116(0x74)           ;*putstatic val {reexecute=0 rethrow=0 return_oop=0}
    //
    private static void checkC1VolatileRead(OutputAnalyzer output) {
        Iterator<String> iter = output.asLines().listIterator();

        String match = skipTo(iter, "'main' '([Ljava/lang/String;)V' in 'TestVolatile$Launcher'");
        if (match == null) {
            throw new RuntimeException("Missing compiler c1 output");
        }

       /* match = skipTo(iter, "*getfield flags");
        if (match == null) {
            throw new RuntimeException("Missing read volatile flags");
        }*/
        ArrayList<String> instrs = new ArrayList<String>();
        String line = null;
        while (iter.hasNext()) {
            line = iter.next();
            if (line.contains("0x")/* && !line.contains(";")*/) {
                addInstrs(line, instrs);
                if (line.contains("[Stub Code]")) {
                    break;
                }
            }
        }

        ListIterator<String> instrReverseIter = instrs.listIterator(instrs.size());
        boolean foundMembarInst = false;

       while (instrReverseIter.hasPrevious()) {
            String inst = instrReverseIter.previous();
            if (inst.endsWith(MEMBARType.Acquire + MEMBARType.DBARINSCODE) || inst.contains(MEMBARType.DBARSTR + MEMBARType.Acquire)) {
                foundMembarInst = true;
                break;
            }
        }

        if (foundMembarInst == false) {
            throw new RuntimeException("No founed valid acquire instruction (0x" + MEMBARType.Acquire + ")!\n");
        }
    }

    // ----------------------------------- Assembly -----------------------------------
    //
    // Compiled method (c1)     988   26       1       TestVolatile$Launcher::<init> (11 bytes)
    //   # {method} {0x000000ffc8700248} '<init>' '()V' in 'TestVolatile$Launcher'
    //   0x000000fff0147640: 0424 8003 | 1200 7238 | 8431 8029
    //  ;; membar
    //   0x000000fff014764c: ;*putfield flags {reexecute=0 rethrow=0 return_oop=0}
    //                       ; - TestVolatile$Launcher::<init>@7 (line 286)
    //   0x000000fff014764c: 1000 7238 | 76c0 c028 | 61e0 c028 | 6300 c102
    //   0x000000fff014765c: ;   {poll_return}
    //
    // The output with hsdis library is:
    //
    // 0x000000ffe903f644:   dbar	0x12
    // 0x000000ffe903f648:   st.w	$a0,$t0,12(0xc)
    // ;; membar
    // 0x000000ffe903f64c:   dbar	0x10                        ;*putfield flags {reexecute=0 rethrow=0 return_oop=0}
    //                                                              ; - TestVolatile$Launcher::<init>@7 (line 315)
    //
    private static void checkC1VolatileWrite(OutputAnalyzer output) {
        Iterator<String> iter = output.asLines().listIterator();

        String match = skipTo(iter, "'<init>' '()V' in 'TestVolatile$Launcher'");
        if (match == null) {
            throw new RuntimeException("Missing compiler c1 output");
        }

        ArrayList<String> instrs = new ArrayList<String>();
        String line = null;
        while (iter.hasNext()) {
            line = iter.next();
            if (line.contains("{poll_return}")) {
                break;
            }
            if (line.contains("0x")/* && !line.contains(";")*/) {
                addInstrs(line, instrs);
            }
        }

        ListIterator<String> instrReverseIter = instrs.listIterator(instrs.size());
        boolean foundMembarRelease = false;
        boolean foundMembarAnyAny = false;

        while (instrReverseIter.hasPrevious()) {
            String inst = instrReverseIter.previous();
            if (inst.endsWith(MEMBARType.Release + MEMBARType.DBARINSCODE) || inst.contains(MEMBARType.DBARSTR + MEMBARType.Release)) {
                foundMembarRelease = true;
            } else if (inst.endsWith(MEMBARType.AnyAny + MEMBARType.DBARINSCODE) || inst.contains(MEMBARType.DBARSTR + MEMBARType.AnyAny)) {
                foundMembarAnyAny = true;
            }
            if (foundMembarRelease && foundMembarAnyAny)
                break;
        }

        if (foundMembarRelease == false) {
            throw new RuntimeException("No founed valid release instruction (0x" + MEMBARType.Release + ")!\n");
        }
        if (foundMembarAnyAny == false) {
            throw new RuntimeException("No founed valid volatile instruction (0x" + MEMBARType.AnyAny + ")!\n");
        }
    }

    // ----------------------------------- Assembly -----------------------------------
    //
    // Compiled method (c2)    1038   26             TestVolatile$Launcher::<init> (11 bytes)
    //   # {method} {0x000000ffcc603248} '<init>' '()V' in 'TestVolatile$Launcher'
    //   0x000000ffed0bfd20: 1200 7238 | 0d24 8003 | cd32 8029
    //
    //   0x000000ffed0bfd2c: ;*putfield flags {reexecute=0 rethrow=0 return_oop=0}
    //                       ; - TestVolatile$Launcher::<init>@7 (line 309)
    //   0x000000ffed0bfd2c: 1900 7238 | 7640 c028 | 6160 c028 | 6380 c002
    //
    //   0x000000ffed0bfd3c: ;   {poll_return}
    //
    // The output with hsdis library is:
    //
    // 0x000000ffed0bfca0:   dbar	0x12
    // 0x000000ffed0bfca4:   ori	$t1,$zero,0x9
    // 0x000000ffed0bfca8:   st.w	$t1,$fp,12(0xc)
    // 0x000000ffed0bfcac:   dbar	0x19                        ;*putfield flags {reexecute=0 rethrow=0 return_oop=0}
    //                                                          ; - TestVolatile$Launcher::<init>@7 (line 333)
    //
    private static void checkC2VolatileWrite(OutputAnalyzer output) {
        Iterator<String> iter = output.asLines().listIterator();

        String match = skipTo(iter, "'<init>' '()V' in 'TestVolatile$Launcher'");
        if (match == null) {
            throw new RuntimeException("Missing compiler c2 output");
        }

        ArrayList<String> instrs = new ArrayList<String>();
        String line = null;
        while (iter.hasNext()) {
            line = iter.next();
            if (line.contains("{poll_return}")) {
                break;
            }
            if (line.contains("0x")/* && !line.contains(";")*/) {
                addInstrs(line, instrs);
            }
        }

        ListIterator<String> instrReverseIter = instrs.listIterator(instrs.size());
        boolean foundMembarRelease = false;
        boolean foundMembarVolatile = false;

        while (instrReverseIter.hasPrevious()) {
            String inst = instrReverseIter.previous();
            if (inst.endsWith(MEMBARType.Release + MEMBARType.DBARINSCODE) || inst.contains(MEMBARType.DBARSTR + MEMBARType.Release)) {
                foundMembarRelease = true;
            } else if (inst.endsWith(MEMBARType.Volatile + MEMBARType.DBARINSCODE) || inst.contains(MEMBARType.DBARSTR + MEMBARType.Volatile)) {
                foundMembarVolatile = true;
            }
        }

        if (foundMembarRelease == false) {
            throw new RuntimeException("No founed valid release instruction (0x" + MEMBARType.Release + ")!\n");
        }
        if (foundMembarVolatile == false) {
            throw new RuntimeException("No founed valid volatile instruction (0x" + MEMBARType.Volatile + ")!\n");
        }
    }

    //----------------------------------- Assembly -----------------------------------
    //
    //Compiled method (c2)     846   24             TestVolatile$Launcher::main (13 bytes)
    //[Verified Entry Point]
    //  # {method} {0x000000ffcc603308} 'main' '([Ljava/lang/String;)V' in 'TestVolatile$Launcher'
    //  0x000000fff0ff6394: ;*getfield flags {reexecute=0 rethrow=0 return_oop=0}
    //                      ; - TestVolatile$Launcher::main@6 (line 127)
    //  0x000000fff0ff6394: 1400 7238
    //
    //  0x000000fff0ff6398: ;*synchronization entry
    //                      ; - TestVolatile$Launcher::main@-1 (line 123)
    //  0x000000fff0ff6398: 8ed1 8129 | 7640 c028 | 6160 c028 | 6380 c002
    //
    //  0x000000fff0ff63a8: ;   {poll_return}
    //
    // The output with hsdis library is:
    // 0x000000ffed0be514:   dbar	0x14                        ;*getfield flags {reexecute=0 rethrow=0 return_oop=0}
    //                                                        ; - TestVolatile$Launcher::main@6 (line 340)
    //
    private static void checkC2VolatileRead(OutputAnalyzer output) {
        Iterator<String> iter = output.asLines().listIterator();

        String match = skipTo(iter, "'main' '([Ljava/lang/String;)V' in 'TestVolatile$Launcher'");
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
            if (inst.endsWith(MEMBARType.Acquire + MEMBARType.DBARINSCODE) || inst.contains(MEMBARType.DBARSTR + MEMBARType.Acquire)) {
                foundMembarInst = true;
                break;
            }
        }

        if (foundMembarInst == false) {
            throw new RuntimeException("No founed valid acquire instruction (0x" + MEMBARType.Acquire + ")!\n");
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

        public volatile int flags = 0x9;

        static Launcher l;
        static int val;

        public static void main(final String[] args) throws Exception {
            test();
            val = l.flags;
        }

        static void test() {
            l = new Launcher();
        }
    }
}
