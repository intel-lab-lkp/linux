.. SPDX-License-Identifier: GPL-2.0

======================================
KWatch - Kernel Memory Watchpoint Tool
======================================

Overview
========

KWatch is a runtime-configurable debugging tool for locating kernel memory
corruption. It arms hardware breakpoints (watchpoints) on a target address
while a chosen function is executing, and reports the exact instruction that
touches the watched memory, together with a stack trace, through a
tracepoint.

Unlike shadow-memory sanitizers, KWatch does not detect invalid accesses in
general; it answers a narrower but common question during corruption hunts:
"who writes to this address?". This includes in-bounds logical overwrites
that KASAN cannot see, because the rogue writer modifies valid memory
through a valid pointer, just at the wrong time or with the wrong data.

Comparison with other tools:

* KASAN detects out-of-bounds and use-after-free accesses, but reports the
  symptom (the invalid access), not the writer that corrupted the data
  earlier. It requires a rebuild and has significant CPU and memory
  overhead, and its redzones perturb memory layout, which can hide
  timing-sensitive bugs.
* KFENCE is a low-overhead sampling detector for slab objects; it cannot be
  pointed at one specific address.
* Hardware breakpoints via kgdb or perf can watch an address, but only a
  fixed one, system-wide, for the whole run. KWatch resolves the address
  dynamically at function entry (for example "argument 2 of this function,
  plus offset 8, dereferenced once") and disarms it again at function exit,
  so short-lived and per-invocation objects can be watched too.

KWatch has near-zero overhead while armed: the watched function pays for
one kprobe/kretprobe pair plus programming of the debug registers; the rest
of the system runs at full speed.

Requirements
============

* ``CONFIG_KWATCH=y`` or ``m``. The Kconfig symbol depends on
  ``CONFIG_PERF_EVENTS``, ``CONFIG_DEBUG_FS`` and an architecture that
  provides ``HAVE_REINSTALL_HW_BREAKPOINT`` (currently x86 only).
* Resolving symbol names in watch expressions requires ``CONFIG_KWATCH=y``
  (built-in); a module can only watch absolute hexadecimal addresses.

Usage
=====

KWatch is configured through a single debugfs file::

    /sys/kernel/debug/kwatch/config

Writing a configuration string starts a watch session (stopping any previous
one); reading the file shows the active configuration and hit-rejection
counters. The configuration is a whitespace-separated list of ``key=value``
tokens:

=================== ==========================================================
Key                 Meaning
=================== ==========================================================
``func_name``       Function whose execution opens the watch window.
``func_offset``     Instruction offset inside ``func_name`` at which the
                    watchpoint is armed (default 0 = function entry).
``watch_expr``      Expression describing the address to watch (see below).
``watch_len``       Watched length in bytes: 1, 2, 4 or 8 (default 8).
``depth``           Recursion depth at which the window opens (default 0).
``max_watch``       Number of hardware watchpoints to preallocate
                    (default 4).
``max_concurrency`` Maximum number of tasks concurrently inside the watch
                    window (default 256).
``duration``        For global watches: seconds until automatic stop.
=================== ==========================================================

Watch expressions
-----------------

The address to watch is computed at function entry from::

    watch_expr={base}[+-offset][->[+-]offset]...

* ``base`` is one of:

  - ``arg1`` ... ``arg6``: a function argument (register calling
    convention). These are only meaningful at function entry, i.e. with
    ``func_offset`` unset; combining ``argN`` with ``func_offset`` reads
    the argument registers mid-function, where they no longer hold the
    original arguments,
  - ``stack``: the kernel stack pointer at the probe point,
  - an absolute hexadecimal address, e.g. ``0xffffffff81234567``,
  - a global symbol name (built-in KWatch only).

* ``+offset`` / ``-offset`` adjusts the current address.
* ``->offset`` loads the pointer stored at the current address (via
  ``get_kernel_nofault()``) and then applies the offset. Up to four chain
  elements are supported; offsets must be explicit (``->`` alone is
  rejected).

Given::

    struct some_struct {
        struct some_struct *ptr;    /* offset 0 */
        int num;                    /* offset 8 */
    };

    void target_function(struct some_struct *arg1);

typical expressions are:

=========================== ==============================================
Expression                  Watches
=========================== ==============================================
``watch_expr=arg1``         ``&arg1->ptr`` (the pointer field itself)
``watch_expr=arg1+8``       ``&arg1->num``
``watch_expr=arg1->0``      ``&arg1->ptr->ptr`` (one dereference)
``watch_expr=arg1->8``      ``&arg1->ptr->num``
``watch_expr=0xffff...+8``  absolute address plus 8
=========================== ==============================================

Example: catch whoever overwrites ``arg1->num`` of a function while that
function runs::

    echo "func_name=target_function watch_expr=arg1+8 watch_len=4" \
        > /sys/kernel/debug/kwatch/config

Watching global variables
-------------------------

A global variable has no natural function window. When ``duration`` is
given without ``func_name``, KWatch starts an internal anchor kernel thread
that sleeps inside a dummy function, and uses that function as the window::

    echo "watch_expr=jiffies_wobble duration=60 watch_len=8" \
        > /sys/kernel/debug/kwatch/config

The session tears itself down when the duration expires.

Reading hits
------------

Hits are emitted as the ``kwatch:kwatch_hit`` tracepoint, which is safe in
NMI-like contexts where printk is not. Each event carries the timestamp,
the instruction pointer, the watched address and a short stack trace::

    echo 1 > /sys/kernel/debug/tracing/events/kwatch/kwatch_hit/enable
    cat /sys/kernel/debug/tracing/trace_pipe

If the corruption crashes the machine, the ring buffer can still be
recovered:

* ``echo 1 > /proc/sys/kernel/ftrace_dump_on_oops`` (or the
  ``ftrace_dump_on_oops`` boot parameter) dumps the buffer to the console
  on an oops.
* With kdump, the buffer is present in the vmcore and can be read with
  ``crash> trace``.
* ``CONFIG_PSTORE_FTRACE`` persists it across reboots on supported
  platforms.

Limitations
===========

* Functions that run in a genuine NMI(-like) context are rejected at
  function entry; rejected invocations never open a watch window and are
  counted in the ``nmi_rejected`` field of the config file. Watching
  functions reachable from NMI handlers is out of scope.
* The number of concurrent watchpoints is bounded by the CPU's debug
  registers (typically 4).
* Cross-CPU re-arming of a watchpoint is rate-limited per CPU: the local
  CPU is always re-pointed, but the broadcast to other CPUs is throttled
  so a very hot watched function cannot storm the system with IPIs. While
  a broadcast is suppressed the other CPUs keep watching the previous
  address, so a writer that runs on another CPU during that window can be
  missed; the number of suppressed broadcasts is reported in the
  ``arm_ipi_suppressed`` field of the config file. KWatch therefore
  targets functions that are entered at a moderate rate.
* If the target address cannot be resolved at arming time (for example a
  ``get_kernel_nofault()`` failure on a swapped or unmapped page), the
  watchpoint is not armed for that invocation.
* Offsets in watch expressions are static; dynamic indexing such as
  ``arg1->ptr[arg2]`` is not supported.
* If a task is torn down while still inside the watched function without
  the function returning (an oops or BUG in the window, which abandons the
  stack), its watch window is not closed until the session stops. This is
  the same best-effort cleanup that applies to every resource a task holds
  when it dies abnormally. Do not target the task-exit path itself.
* arm64 is not yet supported: stepping over a hit that has a custom
  overflow handler needs a generic mechanism in the arch code, which is
  planned as a follow-up series.

Implementation notes
====================

The implementation lives in ``mm/kwatch/`` and is split into a control
plane (``core.c``, the debugfs interface), an execution plane (``probe.c``
and ``deref.c``: kprobe/kretprobe window management and address
resolution), and a resource plane (``hwbp.c`` and ``task_ctx.c``).

Hardware watchpoints are preallocated as perf events on every CPU and
re-pointed at hit time with ``modify_wide_hw_breakpoint_local()``, a new
hw_breakpoint API that updates the breakpoint on the local CPU without
releasing its slot; other CPUs are updated by asynchronous IPIs. Per-task
window state is kept in a fixed-size, lockless open-addressing array
claimed with ``cmpxchg()``, so the hit path performs no allocation and
takes no locks, which keeps it safe in atomic and NMI-like contexts.
