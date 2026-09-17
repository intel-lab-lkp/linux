.. SPDX-License-Identifier: GPL-2.0

=========================
WF_SYNC Wakeup Semantics
=========================

WF_SYNC is a wakeup flag supplied by callers that expect the waking task
to schedule away soon after waking another task. It is a scheduler hint,
not a CPU-placement request.

The synchronous waitqueue helpers pass WF_SYNC to their wake functions.
The default wake function forwards it to the scheduler. WF_SYNC itself
does not block, yield, or otherwise change the state of the waker.

This document describes the current WF_SYNC semantics for the fair
scheduler. Other scheduler classes may ignore WF_SYNC or apply their own
policy.

Custom waitqueue wake functions may ignore WF_SYNC. The following
describes fair-scheduler behavior when a wake function forwards the flag
to the scheduler.

Placement
=========

For fair-class tasks, WF_SYNC can influence CPU selection for a wakee.
It expresses that placing the wakee so that it can run after the waker
schedules away may be beneficial.

WF_SYNC is advisory. CPU selection remains subject to the wakee's
affinity and migration constraints, as well as the scheduler's other
placement policy. The scheduler can therefore select an eligible CPU
other than the waker's CPU or the wakee's previous CPU.

In particular, WF_SYNC does not guarantee that the wakee:

* runs on the waker CPU;
* remains on its previous CPU;
* avoids migration; or
* shares a core with the waker.

Preemption
==========

For fair-class tasks, WF_SYNC can also influence wakeup preemption. It
does not guarantee either immediate preemption or non-preemption. The
scheduler can preempt the current task immediately or defer the wakee's
execution according to fair scheduling policy.

The scheduler does not verify that the waker subsequently schedules
away. A caller can continue to execute after a WF_SYNC wakeup or issue
several WF_SYNC wakeups before scheduling away. WF_SYNC consequently
does not imply that the wakee will run immediately after the wakeup.

On uniprocessor systems, WF_SYNC may avoid a preemption when the waker
is expected to schedule away soon.

Policy
======

WF_SYNC is a non-binding hint. It describes the caller's expectation,
but does not establish a placement or execution-order guarantee for the
wakee.

The hint leaves placement and preemption decisions to the scheduler.
Any future policy that strengthens WF_SYNC semantics must define its
placement and preemption guarantees explicitly.
