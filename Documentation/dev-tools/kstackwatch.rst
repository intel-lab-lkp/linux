.. SPDX-License-Identifier: GPL-2.0

=================================
Kernel Stack Watch (KStackWatch)
=================================

Overview
========

KStackWatch is a lightweight debugging tool designed to detect kernel stack
corruption in real time. It installs a hardware breakpoint (watchpoint) at a
function's specified offset using *kprobe.post_handler* and removes it in
*fprobe.exit_handler*. This covers the full execution window and reports
corruption immediately with time, location, and call stack.

Main features:

* Immediate and precise stack corruption detection
* Support for multiple concurrent watchpoints with configurable limits
* Lockless design, usable in any context
* Depth filter for recursive calls
* Low overhead of memory and CPU
* Flexible debugfs configuration with key=val syntax
* Architecture support: x86_64 and arm64
* Auto-canary detection to simplify configuration

Performance Impact
==================

Runtime overhead was measured on Intel Core Ultra 5 125H @ 3 GHz running
kernel 6.17, using test4:

+------------------------+-------------+---------+
| Type                   | Time (ns)   | Cycles  |
+========================+=============+=========+
| entry with watch       | 10892       | 32620   |
+------------------------+-------------+---------+
| entry without watch    | 159         | 466     |
+------------------------+-------------+---------+
| exit with watch        | 12541       | 37556   |
+------------------------+-------------+---------+
| exit without watch     | 124         | 369     |
+------------------------+-------------+---------+

From a broader perspective, the overall comparison is as follows:

+----------------------------+----------------------+-------------------------+
| Mode                       | CPU Overhead (add)   | Memory Overhead (add)   |
+============================+======================+=========================+
| Compiled but not enabled   | None                 | ~20 B per task          |
+----------------------------+----------------------+-------------------------+
| Enabled, no function hit   | None                 | ~few hundred B          |
+----------------------------+----------------------+-------------------------+
| Func hit, HWBP not toggled | ~140 ns per call     | None                    |
+----------------------------+----------------------+-------------------------+
| Func hit, HWBP toggled     | ~11–12 µs per call   | None                    |
+----------------------------+----------------------+-------------------------+

The overhead is minimal, making KStackWatch suitable for production
environments where stack corruption is suspected but kernel rebuilds are not
feasible.

Kconfig Options
===============

The following configuration options control KStackWatch builds:

- CONFIG_KSTACKWATCH

  Builds the kernel with KStackWatch enabled.

- CONFIG_KSTACKWATCH_PROFILING

  Measures probe runtime overhead for performance analysis and tuning.

- CONFIG_KSTACKWATCH_TEST

  Builds a test module to validate KStackWatch functionality.

Usage
=====

KStackWatch provides optional configurations for different use cases.
CONFIG_KSTACKWATCH enables real-time stack corruption detection using hardware breakpoints and probes.
CONFIG_KSTACKWATCH_PROFILING allows measurement of probe latency and overhead for performance analysis.
CONFIG_KSTACKWATCH_TEST builds a test module for validating KStackWatch functionality under controlled conditions.

KStackWatch is configured through */sys/kernel/debug/kstackwatch/config* using a
key=value format. Both long and short forms are supported. Writing an empty
string disables the watch.

.. code-block:: bash

	# long form
	echo func_name=? func_offset=? ... > /sys/kernel/debug/kstackwatch/config

	# short form
	echo fn=? fo=? ... > /sys/kernel/debug/kstackwatch/config

	# disable
	echo > /sys/kernel/debug/kstackwatch/config

The func_name and the func_offset where the watchpoint should be placed must be
known. This information can be obtained from *objdump* or other tools.

Required parameters
--------------------

+--------------+--------+-----------------------------------------+
| Parameter    | Short  | Description                             |
+==============+========+=========================================+
| func_name    | fn     | Name of the target function             |
+--------------+--------+-----------------------------------------+
| func_offset  | fo     | Instruction pointer offset              |
+--------------+--------+-----------------------------------------+

Optional parameters
--------------------

Default 0 and can be omitted.
Both decimal and hexadecimal are supported.

+--------------+--------+------------------------------------------------+
| Parameter    | Short  | Description                                    |
+==============+========+================================================+
| auto_canary  | ac     | Automatically calculated canary sp_offset      |
+--------------+--------+------------------------------------------------+
| depth        | dp     | Recursion depth filter                         |
+--------------+--------+------------------------------------------------+
|              |        | Maximum number of concurrent watchpoints       |
| max_watch    | mw     | (default 0, capped by available hardware       |
|              |        | breakpoints)                                   |
+--------------+--------+------------------------------------------------+
| panic_hit    | ph     | Panic system on watchpoint hit (default 0)     |
+--------------+--------+------------------------------------------------+
| sp_offset    | so     | Watching addr offset from stack pointer        |
+--------------+--------+------------------------------------------------+
| watch_len    | wl     | Watch length in bytes (1, 2, 4, 8 onX86_64)    |
+--------------+--------+------------------------------------------------+


Workflow Example
================

Silent corruption
-----------------

Consider *test3* in *kstackwatch_test.sh*. Run it directly:

.. code-block:: bash

	echo test3 >/sys/kernel/debug/kstackwatch/test

Sometimes, *test_mthread_victim()* may report as unhappy:

.. code-block:: bash

	[    7.807082] kstackwatch_test: victim[0][11]: unhappy buf[8]=0xabcdabcd

Its source code is:

.. code-block:: c

	static void test_mthread_victim(int thread_id, int seq_id, u64 start_ns)
	{
		ulong buf[BUFFER_SIZE];

		for (int j = 0; j < BUFFER_SIZE; j++)
			buf[j] = 0xdeadbeef + seq_id;

		if (start_ns)
			silent_wait_us(start_ns, VICTIM_MINIOR_WAIT_NS);

		for (int j = 0; j < BUFFER_SIZE; j++) {
			if (buf[j] != (0xdeadbeef + seq_id)) {
				pr_warn("victim[%d][%d]: unhappy buf[%d]=0x%lx\n",
					thread_id, seq_id, j, buf[j]);
				return;
			}
		}

		pr_info("victim[%d][%d]: happy\n", thread_id, seq_id);
	}

From the source code, the report indicates buf[8] was unexpectedly modified,
a case of silent corruption.

Configuration
-------------

Since buf[8] is the corrupted variable, the following configuration shows
how to use KStackWatch to detect its corruption.

func_name
~~~~~~~~~~~

As seen, buf[8] is initialized and modified in *test_mthread_victim*\(\) ,
which sets *func_name*.

func_offset & sp_offset
~~~~~~~~~~~~~~~~~~~~~~~~~
The watchpoint should be set after the assignment and as close as
possible, which sets *func_offset*.

The watchpoint should be set to watch buf[8], which sets *sp_offset*.

Use the objdump output to disassemble the function:

.. code-block:: bash

	objdump -S --disassemble=test_mthread_victim vmlinux

A shortened output is:

.. code-block:: text

	static void test_mthread_victim(int thread_id, int seq_id, u64 start_ns)
	{
	ffffffff815cb4e0:       e8 5b 9b ca ff          call   ffffffff81275040 <__fentry__>
	ffffffff815cb4e5:       55                      push   %rbp
	ffffffff815cb4e6:       53                      push   %rbx
	ffffffff815cb4e7:       48 81 ec 08 01 00 00    sub    $0x108,%rsp
	ffffffff815cb4ee:       89 fd                   mov    %edi,%ebp
	ffffffff815cb4f0:       89 f3                   mov    %esi,%ebx
	ffffffff815cb4f2:       49 89 d0                mov    %rdx,%r8
	ffffffff815cb4f5:       65 48 8b 05 0b cb 80    mov    %gs:0x280cb0b(%rip),%rax        # ffffffff83dd8008 <__stack_chk_guard>
	ffffffff815cb4fc:       02
	ffffffff815cb4fd:       48 89 84 24 00 01 00    mov    %rax,0x100(%rsp)
	ffffffff815cb504:       00
	ffffffff815cb505:       31 c0                   xor    %eax,%eax
		ulong buf[BUFFER_SIZE];
	ffffffff815cb507:       48 89 e2                mov    %rsp,%rdx
	ffffffff815cb50a:       b9 20 00 00 00          mov    $0x20,%ecx
	ffffffff815cb50f:       48 89 d7                mov    %rdx,%rdi
	ffffffff815cb512:       f3 48 ab                rep stos %rax,%es:(%rdi)

		for (int j = 0; j < BUFFER_SIZE; j++)
	ffffffff815cb515:       eb 10                   jmp    ffffffff815cb527 <test_mthread_victim+0x47>
			buf[j] = 0xdeadbeef + seq_id;
	ffffffff815cb517:       8d 93 ef be ad de       lea    -0x21524111(%rbx),%edx
	ffffffff815cb51d:       48 63 c8                movslq %eax,%rcx
	ffffffff815cb520:       48 89 14 cc             mov    %rdx,(%rsp,%rcx,8)
	ffffffff815cb524:       83 c0 01                add    $0x1,%eax
	ffffffff815cb527:       83 f8 1f                cmp    $0x1f,%eax
	ffffffff815cb52a:       7e eb                   jle    ffffffff815cb517 <test_mthread_victim+0x37>
		if (start_ns)
	ffffffff815cb52c:       4d 85 c0                test   %r8,%r8
	ffffffff815cb52f:       75 21                   jne    ffffffff815cb552 <test_mthread_victim+0x72>
			silent_wait_us(start_ns, VICTIM_MINIOR_WAIT_NS);
	...
	ffffffff815cb571:       48 8b 84 24 00 01 00    mov    0x100(%rsp),%rax
	ffffffff815cb579:       65 48 2b 05 87 ca 80    sub    %gs:0x280ca87(%rip),%rax        # ffffffff83dd8008 <__stack_chk_guard>
	...
	ffffffff815cb5a1:       eb ce                   jmp    ffffffff815cb571 <test_mthread_victim+0x91>
	}
	ffffffff815cb5a3:       e8 d8 86 f1 00          call   ffffffff824e3c80 <__stack_chk_fail>


func_offset
^^^^^^^^^^^

The function begins at ffffffff815cb4e0. The *buf* array is initialized in a loop.
The instruction storing values into the array is at ffffffff815cb520, and the
first instruction after the loop is at ffffffff815cb52c.

Because KStackWatch uses *kprobe.post_handler*, the watchpoint can be
set right after ffffffff815cb520. However, this will cause false positive
because the watchpoint is active before buf[8] is assigned.

An alternative is to place the watchpoint at ffffffff815cb52c, right
after the loop. This avoids false positives but leaves a small window
for false negatives.

In this document, ffffffff815cb52c is chosen for cleaner logs. If false
negatives are suspected, repeat the test to catch the corruption.

The required offset is calculated from the function start:

*func_offset* is 0x4c (ffffffff815cb52c - ffffffff815cb4e0).

sp_offset
^^^^^^^^^^^

From the disassembly, the buf array is at the top of the stack,
meaning buf == rsp. Therefore, buf[8] sits at rsp + 8 * sizeof(ulong) =
rsp + 64. Thus, *sp_offset* is 64.

Other parameters
~~~~~~~~~~~~~~~~~~

* *depth* is 0, as test_mthread_victim is not recursive
* *max_watch* is 0 to use all available hwbps
* *watch_len* is 8, the size of a ulong on x86_64

Parameters with a value of 0 can be omitted as defaults.

Configure the watch:

.. code-block:: bash

	echo "fn=test_mthread_victim fo=0x4c so=64 wl=8" > /sys/kernel/debug/kstackwatch/config

Now rerun the test:

.. code-block:: bash

	echo test3 >/sys/kernel/debug/kstackwatch/test

The dmesg log shows:

.. code-block:: text

	[    7.607074] kstackwatch: ========== KStackWatch: Caught stack corruption =======
	[    7.607077] kstackwatch: config fn=test_mthread_victim fo=0x4c so=64 wl=8
	[    7.607080] CPU: 2 UID: 0 PID: 347 Comm: corrupting Not tainted 6.17.0-rc7-00022-g90270f3db80a-dirty #509 PREEMPT(voluntary)
	[    7.607083] Call Trace:
	[    7.607084]  <#DB>
	[    7.607085]  dump_stack_lvl+0x66/0xa0
	[    7.607091]  ksw_watch_handler.part.0+0x2b/0x60
	[    7.607094]  ksw_watch_handler+0xba/0xd0
	[    7.607095]  ? test_mthread_corrupting+0x48/0xd0
	[    7.607097]  ? kthread+0x10d/0x210
	[    7.607099]  ? ret_from_fork+0x187/0x1e0
	[    7.607102]  ? ret_from_fork_asm+0x1a/0x30
	[    7.607105]  __perf_event_overflow+0x154/0x570
	[    7.607108]  perf_bp_event+0xb4/0xc0
	[    7.607112]  ? look_up_lock_class+0x59/0x150
	[    7.607115]  hw_breakpoint_exceptions_notify+0xf7/0x110
	[    7.607117]  notifier_call_chain+0x44/0x110
	[    7.607119]  atomic_notifier_call_chain+0x5f/0x110
	[    7.607121]  notify_die+0x4c/0xb0
	[    7.607123]  exc_debug_kernel+0xaf/0x170
	[    7.607126]  asm_exc_debug+0x1e/0x40
	[    7.607127] RIP: 0010:test_mthread_corrupting+0x48/0xd0
	[    7.607129] Code: c7 80 0a 24 83 e8 48 f1 f1 00 48 85 c0 74 dd eb 30 bb 00 00 00 00 eb 59 48 63 c2 48 c1 e0 03 48 03 03 be cd ab cd ab 48 89 30 <83> c2 01 b8 20 00 00 00 29 c8 39 d0 7f e0 48 8d 7b 10 e8 d1 86 d4
	[    7.607130] RSP: 0018:ffffc90000acfee0 EFLAGS: 00000286
	[    7.607132] RAX: ffffc90000a13de8 RBX: ffff888102d57580 RCX: 0000000000000008
	[    7.607132] RDX: 0000000000000008 RSI: 00000000abcdabcd RDI: ffffc90000acfe00
	[    7.607133] RBP: ffff8881085bc800 R08: 0000000000000001 R09: 0000000000000000
	[    7.607133] R10: 0000000000000001 R11: 0000000000000000 R12: ffff888105398000
	[    7.607134] R13: ffff8881085bc800 R14: ffffffff815cb660 R15: 0000000000000000
	[    7.607134]  ? __pfx_test_mthread_corrupting+0x10/0x10
	[    7.607137]  </#DB>
	[    7.607138]  <TASK>
	[    7.607138]  kthread+0x10d/0x210
	[    7.607140]  ? __pfx_kthread+0x10/0x10
	[    7.607141]  ret_from_fork+0x187/0x1e0
	[    7.607143]  ? __pfx_kthread+0x10/0x10
	[    7.607144]  ret_from_fork_asm+0x1a/0x30
	[    7.607147]  </TASK>
	[    7.607147] kstackwatch: =================== KStackWatch End ===================
	[    7.807082] kstackwatch_test: victim[0][11]: unhappy buf[8]=0xabcdabcd

The line ``RIP: 0010:test_mthread_corrupting+0x48/0xd0`` shows the exact
location where the corruption occurred. Now that the ``corrupting()`` function has
been identified, it is straightforward to trace back to ``buggy()`` and fix the bug.


More usage examples and corruption scenarios are provided in
``kstackwatch_test.sh`` and ``mm/kstackwatch/test.c``.

Limitations
===========

* Limited by available hardware breakpoints
* Only one function can be watched at a time
* Canary search limited to 128 * sizeof(ulong) from the current stack
  pointer. This is sufficient for most cases, but has three limitations:

  - If the stack frame is larger, the search may fail.
  - If the function does not have a canary, the search may fail.
  - If stack memory occasionally contains the same value as the canary,
    it may be incorrectly matched.

  In these cases, the user can provide the canary location using
  ``sp_offset``, or treat any memory in the function prologue
  as the canary.
