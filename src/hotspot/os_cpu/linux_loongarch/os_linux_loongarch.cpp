/*
 * Copyright (c) 1999, 2014, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2015, 2025, Loongson Technology. All rights reserved.
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

// no precompiled headers
#include "asm/macroAssembler.hpp"
#include "classfile/classLoader.hpp"
#include "classfile/systemDictionary.hpp"
#include "classfile/vmSymbols.hpp"
#include "code/icBuffer.hpp"
#include "code/vtableStubs.hpp"
#include "interpreter/interpreter.hpp"
#include "memory/allocation.inline.hpp"
#include "os_linux.hpp"
#include "os_posix.hpp"
#include "prims/jniFastGetField.hpp"
#include "prims/jvm_misc.hpp"
#include "runtime/arguments.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/osThread.hpp"
#include "runtime/safepointMechanism.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/timer.hpp"
#include "signals_posix.hpp"
#include "utilities/events.hpp"
#include "utilities/vmError.hpp"
#include "compiler/disassembler.hpp"

// put OS-includes here
# include <sys/types.h>
# include <sys/mman.h>
# include <pthread.h>
# include <signal.h>
# include <errno.h>
# include <dlfcn.h>
# include <stdlib.h>
# include <stdio.h>
# include <unistd.h>
# include <sys/resource.h>
# include <pthread.h>
# include <sys/stat.h>
# include <sys/time.h>
# include <sys/utsname.h>
# include <sys/socket.h>
# include <sys/wait.h>
# include <pwd.h>
# include <poll.h>
# include <ucontext.h>
# include <fpu_control.h>

#define REG_SP 3
#define REG_FP 22

NOINLINE address os::current_stack_pointer() {
  register void *sp __asm__ ("$r3");
  return (address) sp;
}

char* os::non_memory_address_word() {
  // Must never look like an address returned by reserve_memory,
  // even in its subfields (as defined by the CPU immediate fields,
  // if the CPU splits constants across multiple instructions).

  return (char*) -1;
}

address os::Posix::ucontext_get_pc(const ucontext_t * uc) {
  return (address)uc->uc_mcontext.__pc;
}

void os::Posix::ucontext_set_pc(ucontext_t * uc, address pc) {
  uc->uc_mcontext.__pc = (intptr_t)pc;
}

intptr_t* os::Linux::ucontext_get_sp(const ucontext_t * uc) {
  return (intptr_t*)uc->uc_mcontext.__gregs[REG_SP];
}

intptr_t* os::Linux::ucontext_get_fp(const ucontext_t * uc) {
  return (intptr_t*)uc->uc_mcontext.__gregs[REG_FP];
}

address os::fetch_frame_from_context(const void* ucVoid,
                    intptr_t** ret_sp, intptr_t** ret_fp) {

  address  epc;
  ucontext_t* uc = (ucontext_t*)ucVoid;

  if (uc != nullptr) {
    epc = os::Posix::ucontext_get_pc(uc);
    if (ret_sp) *ret_sp = os::Linux::ucontext_get_sp(uc);
    if (ret_fp) *ret_fp = os::Linux::ucontext_get_fp(uc);
  } else {
    epc = nullptr;
    if (ret_sp) *ret_sp = (intptr_t *)nullptr;
    if (ret_fp) *ret_fp = (intptr_t *)nullptr;
  }

  return epc;
}

frame os::fetch_frame_from_context(const void* ucVoid) {
  intptr_t* sp;
  intptr_t* fp;
  address epc = fetch_frame_from_context(ucVoid, &sp, &fp);
  if (!is_readable_pointer(epc)) {
    // Try to recover from calling into bad memory
    // Assume new frame has not been set up, the same as
    // compiled frame stack bang
    return fetch_compiled_frame_from_context(ucVoid);
  }
  return frame(sp, fp, epc);
}

frame os::fetch_compiled_frame_from_context(const void* ucVoid) {
  const ucontext_t* uc = (const ucontext_t*)ucVoid;
  // In compiled code, the stack banging is performed before RA
  // has been saved in the frame.  RA is live, and SP and FP
  // belong to the caller.
  intptr_t* fp = os::Linux::ucontext_get_fp(uc);
  intptr_t* sp = os::Linux::ucontext_get_sp(uc);
  address pc = (address)(uc->uc_mcontext.__gregs[1]);
  return frame(sp, fp, pc);
}

// By default, gcc always save frame pointer on stack. It may get
// turned off by -fomit-frame-pointer,
frame os::get_sender_for_C_frame(frame* fr) {
  return frame(fr->sender_sp(), fr->link(), fr->sender_pc());
}

frame os::current_frame() {
  intptr_t *fp = ((intptr_t **)__builtin_frame_address(0))[frame::link_offset];
  frame myframe((intptr_t*)os::current_stack_pointer(),
                (intptr_t*)fp,
                CAST_FROM_FN_PTR(address, os::current_frame));
  if (os::is_first_C_frame(&myframe)) {
    // stack is not walkable
    return frame();
  } else {
    return os::get_sender_for_C_frame(&myframe);
  }
}

bool PosixSignals::pd_hotspot_signal_handler(int sig, siginfo_t* info,
                                             ucontext_t* uc, JavaThread* thread) {
#ifdef PRINT_SIGNAL_HANDLE
  tty->print_cr("Signal: signo=%d, sicode=%d, sierrno=%d, siaddr=%lx",
      info->si_signo,
      info->si_code,
      info->si_errno,
      info->si_addr);
#endif

  // decide if this trap can be handled by a stub
  address stub = nullptr;
  address pc   = nullptr;

  pc = (address) os::Posix::ucontext_get_pc(uc);
#ifdef PRINT_SIGNAL_HANDLE
  tty->print_cr("pc=%lx", pc);
  os::print_context(tty, uc);
#endif
  //%note os_trap_1
  if (info != nullptr && uc != nullptr && thread != nullptr) {
    pc = (address) os::Posix::ucontext_get_pc(uc);

    // Handle ALL stack overflow variations here
    if (sig == SIGSEGV) {
      address addr = (address) info->si_addr;
#ifdef PRINT_SIGNAL_HANDLE
      tty->print("handle all stack overflow variations: ");
      /*tty->print("addr = %lx, stack base = %lx, stack top = %lx\n",
        addr,
        thread->stack_base(),
        thread->stack_base() - thread->stack_size());
        */
#endif

      // check if fault address is within thread stack
      if (thread->is_in_full_stack(addr)) {
        // stack overflow
#ifdef PRINT_SIGNAL_HANDLE
        tty->print("stack exception check \n");
#endif
        if (os::Posix::handle_stack_overflow(thread, addr, pc, uc, &stub)) {
          return true; // continue
        }
      }
    } // sig == SIGSEGV

    if (thread->thread_state() == _thread_in_Java) {
      // Java thread running in Java code => find exception handler if any
      // a fault inside compiled code, the interpreter, or a stub
#ifdef PRINT_SIGNAL_HANDLE
      tty->print("java thread running in java code\n");
#endif

      // Handle signal from NativeJump::patch_verified_entry().
      if (sig == SIGILL && nativeInstruction_at(pc)->is_sigill_not_entrant()) {
#ifdef PRINT_SIGNAL_HANDLE
        tty->print_cr("verified entry = %lx, sig=%d", nativeInstruction_at(pc), sig);
#endif
        stub = SharedRuntime::get_handle_wrong_method_stub();
      } else if (sig == SIGSEGV && SafepointMechanism::is_poll_address((address)info->si_addr)) {
#ifdef PRINT_SIGNAL_HANDLE
        tty->print_cr("polling address = %lx, sig=%d", os::get_polling_page(), sig);
#endif
        stub = SharedRuntime::get_poll_stub(pc);
      } else if (sig == SIGBUS /* && info->si_code == BUS_OBJERR */) {
        // BugId 4454115: A read from a MappedByteBuffer can fault
        // here if the underlying file has been truncated.
        // Do not crash the VM in such a case.
        CodeBlob* cb = CodeCache::find_blob(pc);
        CompiledMethod* nm = (cb != nullptr) ? cb->as_compiled_method_or_null() : nullptr;
#ifdef PRINT_SIGNAL_HANDLE
        tty->print("cb = %lx, nm = %lx\n", cb, nm);
#endif
        bool is_unsafe_arraycopy = (thread->doing_unsafe_access() && UnsafeCopyMemory::contains_pc(pc));
        if ((nm != nullptr && nm->has_unsafe_access()) || is_unsafe_arraycopy) {
          address next_pc = pc + NativeInstruction::nop_instruction_size;
          if (is_unsafe_arraycopy) {
            next_pc = UnsafeCopyMemory::page_error_continue_pc(pc);
          }
          stub = SharedRuntime::handle_unsafe_access(thread, next_pc);
        }
      } else if (sig == SIGFPE &&
                 (info->si_code == FPE_INTDIV || info->si_code == FPE_FLTDIV)) {
        stub = SharedRuntime::continuation_for_implicit_exception(thread,
                                    pc,
                                    SharedRuntime::IMPLICIT_DIVIDE_BY_ZERO);
      } else if (sig == SIGSEGV &&
                 MacroAssembler::uses_implicit_null_check(info->si_addr)) {
#ifdef PRINT_SIGNAL_HANDLE
        tty->print("continuation for implicit exception\n");
#endif
        // Determination of interpreter/vtable stub/compiled code null exception
        stub = SharedRuntime::continuation_for_implicit_exception(thread, pc, SharedRuntime::IMPLICIT_NULL);
#ifdef PRINT_SIGNAL_HANDLE
        tty->print_cr("continuation_for_implicit_exception stub: %lx", stub);
#endif
      } else if (sig == SIGILL && nativeInstruction_at(pc)->is_stop()) {
        // Pull a pointer to the error message out of the instruction
        // stream.
        const uint64_t *detail_msg_ptr
          = (uint64_t*)(pc + 4/*NativeInstruction::instruction_size*/);
        const char *detail_msg = (const char *)*detail_msg_ptr;
        const char *msg = "stop";
        if (TraceTraps) {
          tty->print_cr("trap: %s: (SIGILL)", msg);
        }

        // End life with a fatal error, message and detail message and the context.
        // Note: no need to do any post-processing here (e.g. signal chaining)
        va_list va_dummy;
        VMError::report_and_die(thread, uc, nullptr, 0, msg, detail_msg, va_dummy);
        va_end(va_dummy);

        ShouldNotReachHere();
      }
    } else if ((thread->thread_state() == _thread_in_vm ||
                 thread->thread_state() == _thread_in_native) &&
               sig == SIGBUS && /* info->si_code == BUS_OBJERR && */
               thread->doing_unsafe_access()) {
#ifdef PRINT_SIGNAL_HANDLE
      tty->print_cr("SIGBUS in vm thread \n");
#endif
      address next_pc = pc + NativeInstruction::nop_instruction_size;
      if (UnsafeCopyMemory::contains_pc(pc)) {
        next_pc = UnsafeCopyMemory::page_error_continue_pc(pc);
      }
      stub = SharedRuntime::handle_unsafe_access(thread, next_pc);
    }

    // jni_fast_Get<Primitive>Field can trap at certain pc's if a GC kicks in
    // and the heap gets shrunk before the field access.
    if ((sig == SIGSEGV) || (sig == SIGBUS)) {
#ifdef PRINT_SIGNAL_HANDLE
      tty->print("jni fast get trap: ");
#endif
      address addr = JNI_FastGetField::find_slowcase_pc(pc);
      if (addr != (address)-1) {
        stub = addr;
      }
#ifdef PRINT_SIGNAL_HANDLE
      tty->print_cr("addr = %d, stub = %lx", addr, stub);
#endif
    }
  }

  if (stub != nullptr) {
#ifdef PRINT_SIGNAL_HANDLE
    tty->print_cr("resolved stub=%lx\n",stub);
#endif
    // save all thread context in case we need to restore it
    if (thread != nullptr) thread->set_saved_exception_pc(pc);

    os::Posix::ucontext_set_pc(uc, stub);
    return true;
  }

  return false;
}

void os::Linux::init_thread_fpu_state(void) {
}

int os::Linux::get_fpu_control_word(void) {
  return 0; // mute compiler
}

void os::Linux::set_fpu_control_word(int fpu_control) {
}

////////////////////////////////////////////////////////////////////////////////
// thread stack

// Minimum usable stack sizes required to get to user code. Space for
// HotSpot guard pages is added later.
size_t os::_compiler_thread_min_stack_allowed = 48 * K;
size_t os::_java_thread_min_stack_allowed = 40 * K;
size_t os::_vm_internal_thread_min_stack_allowed = 64 * K;

// Return default stack size for thr_type
size_t os::Posix::default_stack_size(os::ThreadType thr_type) {
  // Default stack size (compiler thread needs larger stack)
  size_t s = (thr_type == os::compiler_thread ? 2 * M : 512 * K);
  return s;
}

/////////////////////////////////////////////////////////////////////////////
// helper functions for fatal error handler
void os::print_register_info(outputStream *st, const void *context, int& continuation) {
  const int register_count = 32;
  int n = continuation;
  assert(n >= 0 && n <= register_count, "Invalid continuation value");
  if (context == nullptr || n == register_count) {
    return;
  }

  const ucontext_t *uc = (const ucontext_t*)context;
  while (n < register_count) {
    // Update continuation with next index before printing location
    continuation = n + 1;
# define CASE_PRINT_REG(n, str) case n: st->print(str); print_location(st, (intptr_t)uc->uc_mcontext.__gregs[n]);
    switch (n) {
      CASE_PRINT_REG( 0, "ZERO="); break;
      CASE_PRINT_REG( 1, "RA="); break;
      CASE_PRINT_REG( 2, "TP="); break;
      CASE_PRINT_REG( 3, "SP="); break;
      CASE_PRINT_REG( 4, "A0="); break;
      CASE_PRINT_REG( 5, "A1="); break;
      CASE_PRINT_REG( 6, "A2="); break;
      CASE_PRINT_REG( 7, "A3="); break;
      CASE_PRINT_REG( 8, "A4="); break;
      CASE_PRINT_REG( 9, "A5="); break;
      CASE_PRINT_REG(10, "A6="); break;
      CASE_PRINT_REG(11, "A7="); break;
      CASE_PRINT_REG(12, "T0="); break;
      CASE_PRINT_REG(13, "T1="); break;
      CASE_PRINT_REG(14, "T2="); break;
      CASE_PRINT_REG(15, "T3="); break;
      CASE_PRINT_REG(16, "T4="); break;
      CASE_PRINT_REG(17, "T5="); break;
      CASE_PRINT_REG(18, "T6="); break;
      CASE_PRINT_REG(19, "T7="); break;
      CASE_PRINT_REG(20, "T8="); break;
      CASE_PRINT_REG(21, "RX="); break;
      CASE_PRINT_REG(22, "FP="); break;
      CASE_PRINT_REG(23, "S0="); break;
      CASE_PRINT_REG(24, "S1="); break;
      CASE_PRINT_REG(25, "S2="); break;
      CASE_PRINT_REG(26, "S3="); break;
      CASE_PRINT_REG(27, "S4="); break;
      CASE_PRINT_REG(28, "S5="); break;
      CASE_PRINT_REG(29, "S6="); break;
      CASE_PRINT_REG(30, "S7="); break;
      CASE_PRINT_REG(31, "S8="); break;
    }
# undef CASE_PRINT_REG
    ++n;
  }
}

void os::print_context(outputStream *st, const void *context) {
  if (context == nullptr) return;

  const ucontext_t *uc = (const ucontext_t*)context;

  st->print_cr("Registers:");
  st->print(  "ZERO=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[0]);
  st->print(", RA=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[1]);
  st->print(", TP=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[2]);
  st->print(", SP=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[3]);
  st->cr();
  st->print(  "A0=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[4]);
  st->print(", A1=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[5]);
  st->print(", A2=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[6]);
  st->print(", A3=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[7]);
  st->cr();
  st->print(  "A4=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[8]);
  st->print(", A5=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[9]);
  st->print(", A6=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[10]);
  st->print(", A7=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[11]);
  st->cr();
  st->print(  "T0=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[12]);
  st->print(", T1=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[13]);
  st->print(", T2=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[14]);
  st->print(", T3=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[15]);
  st->cr();
  st->print(  "T4=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[16]);
  st->print(", T5=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[17]);
  st->print(", T6=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[18]);
  st->print(", T7=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[19]);
  st->cr();
  st->print(  "T8=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[20]);
  st->print(", RX=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[21]);
  st->print(", FP=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[22]);
  st->print(", S0=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[23]);
  st->cr();
  st->print(  "S1=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[24]);
  st->print(", S2=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[25]);
  st->print(", S3=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[26]);
  st->print(", S4=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[27]);
  st->cr();
  st->print(  "S5=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[28]);
  st->print(", S6=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[29]);
  st->print(", S7=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[30]);
  st->print(", S8=" INTPTR_FORMAT, (intptr_t)uc->uc_mcontext.__gregs[31]);
  st->cr();
  st->cr();
}

void os::setup_fpu() {
  // no use for LA
}

#ifndef PRODUCT
void os::verify_stack_alignment() {
  assert(((intptr_t)os::current_stack_pointer() & (StackAlignmentInBytes-1)) == 0, "incorrect stack alignment");
}
#endif

int os::extra_bang_size_in_bytes() {
  // LA does not require the additional stack bang.
  return 0;
}
