.. SPDX-License-Identifier: GPL-2.0

======================
Out Of Memory Handling
======================

When the kernel cannot satisfy a memory allocation after exhausting reclaim,
compaction, and memory reserves, it invokes the OOM killer to terminate a
process and free memory.  The implementation is in ``mm/oom_kill.c``.

Victim Selection
================

The OOM killer scores every eligible process and kills the one with the
highest score.  The score is the sum of the process's resident pages, swap
entries, and page table pages.  This sum is then adjusted by the per-process
``oom_score_adj`` tunable (range -1000 to 1000, default 0), which biases
the score by ``oom_score_adj * totalpages / 1000``.  Setting
``oom_score_adj`` to -1000 disables OOM killing for that process entirely.

The ``totalpages`` baseline depends on the allocation constraint:

- **Unconstrained**: all RAM plus swap.
- **Cpuset**: memory on nodes in the current cpuset.
- **Memory policy**: memory on nodes in the current mempolicy.
- **Memory cgroup**: the cgroup's memory limit.

Only processes that can use memory within the constraint are considered.
Kernel threads and init are never eligible.

OOM Reaper
==========

Sending SIGKILL does not immediately free memory — the victim must be
scheduled, unwind its stack, and tear down its address space.  To speed
this up, the OOM reaper kernel thread (available on MMU systems) proactively
unmaps the victim's anonymous and private pages without waiting for the
victim to exit.

The reaper gives the victim a short window to exit naturally before
intervening.  It walks the victim's VMAs in reverse and calls
``unmap_page_range()`` to release physical pages.  Once reaping completes
(or is no longer possible), the mm is marked ``MMF_OOM_SKIP`` so the OOM
killer skips it in future invocations.

Before reaping, the mm is marked ``MMF_UNSTABLE`` to signal page fault
handlers that private mappings may have been zeroed and are no longer
reliable.

process_mrelease
================

The ``process_mrelease(pidfd, flags)`` system call lets userspace OOM
managers (such as systemd-oomd or Android's lmkd) trigger the same reaping
mechanism on a dying process without waiting for the kernel OOM killer.
It operates on a process that is already exiting and performs the same
address space teardown that the OOM reaper would.

Sysctl Knobs
============

``vm.panic_on_oom``
  0 (default): kill a process.  1: panic on unconstrained OOM only.
  2: always panic.

``vm.oom_kill_allocating_task``
  When non-zero, kill the task that triggered the OOM rather than scanning
  for the largest process.

``vm.oom_dump_tasks``
  When non-zero (default), dump a table of all eligible tasks and their
  memory usage to the kernel log before killing.
