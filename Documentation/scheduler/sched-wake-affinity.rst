.. SPDX-License-Identifier: GPL-2.0

==============================
WF_SYNC Wakeup Placement Hints
==============================

WF_SYNC is a wakeup flag supplied by callers that expect the waking task
to schedule away soon after waking another task. It is a scheduler hint,
not a CPU-placement request.

The synchronous waitqueue helpers pass WF_SYNC to try_to_wake_up(). The
wakeup path adds WF_TTWU before invoking the scheduler. WF_SYNC itself
does not block, yield, or otherwise change the state of the waker.

This document describes the current behavior for the fair scheduler.
Other scheduler classes may ignore WF_SYNC or apply their own policy.

Wakeup paths
============

A successful wakeup does not always select a CPU. If the wakee is already
queued, try_to_wake_up() can complete the wakeup through ttwu_runnable().
That path retains the wakee's current runqueue, although it can still
invoke wakeup_preempt().

For a wakee that is not queued, try_to_wake_up() calls
select_task_rq(). If the wakee has one allowed CPU or migration is
disabled, select_task_rq() bypasses the scheduler-class CPU-selection
method and selects an allowed CPU directly.

Fair-class CPU selection
========================

For a fair-class wakee, select_task_rq_fair() derives its local sync
state as::

        sync = (wake_flags & WF_SYNC) &&
               !(current->flags & PF_EXITING);

Thus, WF_SYNC does not influence wake-affine selection when the current
task is exiting.

For WF_TTWU wakeups, select_task_rq_fair() first calls record_wakee().
It can then return before wake-affine selection in either of these cases:

* WF_CURRENT_CPU is set and the waking CPU is allowed; or
* find_energy_efficient_cpu() selects a CPU while the root domain is not
  overutilized.

Otherwise, the fair scheduler computes::

        want_affine = !wake_wide(p) &&
                      cpumask_test_cpu(cpu, p->cpus_ptr);

wake_wide() uses the wakee-flip state maintained by record_wakee() to
identify broad wakeup relationships. WF_SYNC does not override this
classification.

Wake affinity is considered only when want_affine is true, the domain has
SD_WAKE_AFFINE set, and the wakee's previous CPU belongs to that domain.
wake_affine() considers only two CPUs: the waking CPU and the wakee's
previous CPU.

With WF_SYNC, wake_affine_idle() can prefer the waking CPU when::

        rq->nr_running - cfs_h_nr_delayed(rq) == 1

wake_affine_weight() also adjusts the effective load comparison by
removing the current task's load from the waking CPU and biasing the
previous-CPU effective load.

The result of wake_affine() is only a candidate. For WF_TTWU wakeups,
select_task_rq_fair() passes that candidate to select_idle_sibling().

Idle CPU selection
==================

select_idle_sibling() first tests whether the candidate CPU is idle and
can run the wakee. If not, it can select:

* the previous CPU when it is cache-affine and idle;
* a recently used CPU when it is cache-affine and idle;
* an idle SMT sibling; or
* another idle CPU in the relevant search domain.

On asymmetric-capacity systems, the search uses sd_asym_cpucapacity when
available. Otherwise, it uses sd_llc for the candidate CPU.

Consequently, WF_SYNC does not guarantee that the wakee runs on the
waker CPU, remains on its previous CPU, avoids migration, or shares a
core with the waker.

Fair-class wakeup preemption
============================

WF_SYNC can also affect wakeup_preempt_fair(). The normal fair-class
preemption checks run first. In particular, a non-idle wakee can preempt
an idle entity, and PREEMPT_SHORT can select the wakee before the
WF_SYNC-specific path is reached.

If the wakee becomes the next buddy after those checks, preempt_sync()
uses WF_SYNC to decide whether to request rescheduling. The wakee must
be earlier than the current entity, and the current entity must have run
for at least the applicable threshold. The threshold is
sysctl_sched_migration_cost, divided by four when WF_RQ_SELECTED is set.

If those conditions are not met, preempt_sync() returns
PREEMPT_WAKEUP_NONE. Consequently, WF_SYNC neither guarantees nor
prevents immediate wakee preemption. On UP, it can avoid an unnecessary
preemption when the waker is expected to schedule away.

Semantics and policy
====================

WF_SYNC is a non-binding hint. It does not guarantee that the wakee:

* runs on the waker CPU;
* remains on its previous CPU;
* avoids migration;
* shares a core with the waker; or
* immediately preempts the current task.

The scheduler does not verify that the waker subsequently blocks.
Callers may therefore use WF_SYNC where the waker continues to execute,
or where several wakeups are issued before it schedules away.

The current policy leaves the locality, parallelism, topology, load, and
capacity tradeoffs to the scheduler. It does not require the wakee to
remain on the waker CPU when that CPU has no other runnable task.

Any future policy that strengthens WF_SYNC placement semantics must
consider the different call sites, workload patterns, and hardware
topologies that use the flag.
