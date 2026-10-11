.. SPDX-License-Identifier: GPL-2.0

Monitor tlob
============

- Name: tlob - task latency over budget
- Type: per-object hybrid automaton (RV_MON_PER_OBJ)
- Author: Wen Yang <wen.yang@linux.dev>

Description
-----------

The tlob (task latency over budget) monitor is a per-object hybrid
automaton that checks a task's elapsed time (CLOCK_MONOTONIC) against a
budget: from an instrumented entry point to its matching return, the
task must not spend more than the budget in any combination of running,
waiting and sleeping.  The clock ``clk_elapsed`` must satisfy
``clk_elapsed <= BUDGET_NS()`` in the ``running``, ``waiting`` and
``sleeping`` states; when it does not, the HA timer fires, the framework
emits ``error_env_tlob`` and resets the monitor::

                                           | (initial)
                                           v
        +--------------+   stop    +----------+
     +->|   running    | --------> |  stopped |
     |  +--------------+ <-start-- +----------+
   switch_in  preempt  sleep
     |        |       |
     |        |       |
     |        v       v
     +---------+  +---------+
     | waiting |  | sleeping|
     +---------+  +---------+
          ^            v
          |   wakeup   |
          |            |
          +------------+

Not drawn above, ``running`` and ``waiting`` each take ``wakeup`` as a
self-loop: a task woken between ``set_current_state()`` and ``schedule()``
is still on CPU, and a wakeup never ends the budget window.  ``stopped``
takes every scheduler event (``preempt``, ``sleep``, ``switch_in``,
``wakeup``) as a self-loop, so a task keeps going through the scheduler
between windows without disturbing the model.  See
``tools/verification/models/tlob.dot`` for the full model.

``stopped`` has no invariant and hence no timer.  A ``stop``
(``tlob_stop_task()``, window ended) does not reset the monitor: a later
``start`` (``tlob_start_task()``) begins a new window from ``stopped``,
while task exit, uprobe removal and monitor disable are the paths that
reset and free the state.  A START while still in ``running`` (a nested
window) is rejected with ``-EALREADY``.

The non-running condition (monitor not yet started, or reset after a
violation or monitor disable) is handled implicitly by the RV framework;
it is not an explicit DA state.

Usage
-----

tracefs interface (uprobe-based external monitoring)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The ``monitor`` tracefs file instruments an unmodified binary via
uprobes, following the ftrace ``uprobe_events`` convention.  Each
binding has a NAME::

  p:NAME PATH:OFFSET_START OFFSET_STOP threshold=NS

The probes at ``OFFSET_START`` and ``OFFSET_STOP`` (ELF file offsets of
entry points in ``PATH``) call ``tlob_start_task()`` and
``tlob_stop_task()``.  ``PATH`` may contain ``:``; the last ``:`` in
``PATH:OFFSET_START`` is the separator.  ``threshold`` is in
nanoseconds and must be between 1 us and 1 hour.  ``NAME`` follows the
ftrace event-name rule (``[A-Za-z_][A-Za-z0-9_]*``, 31 characters at
most) and must be unique among the registered bindings
(``-EEXIST``); overlapping ranges on the same binary are rejected
too, since a task runs one window at a time and a second range would
shadow the first::

  echo 1 > /sys/kernel/tracing/rv/monitors/tlob/enable

  echo "p:myregion /usr/bin/myapp:0x12a0 0x12f0 threshold=5000000" \
      >> /sys/kernel/tracing/rv/monitors/tlob/monitor

  # List registered bindings
  cat /sys/kernel/tracing/rv/monitors/tlob/monitor

  # Remove the binding NAME
  echo "-:myregion" >> /sys/kernel/tracing/rv/monitors/tlob/monitor

As with the ftrace dynamic-event files, opening ``monitor`` for write
with ``>`` truncates: ``echo > monitor`` removes all bindings.  Use
``>>`` for every individual command, removals included.

A task may hit the START/STOP pair any number of times: each pair of
hits is one window, and the per-task record is kept between windows.  A
window ends at whichever comes first, the STOP probe or the timer; if
the timer won, the STOP is a no-op and the next START opens the
following window.  Removing a binding while a task is between windows
frees that task's record; a task still inside a window keeps running and
is cleaned up on exit.  A task reaching a range bound to a different
binding while its record is between windows is reported through
``error_tlob``; while a window is open, any START is refused with
``-EALREADY`` and not reported.

Violation tracepoints
~~~~~~~~~~~~~~~~~~~~~

A budget violation emits the framework tracepoint ``error_env_tlob``:
``id`` (task pid), ``state``, ``event`` (``"budget_exceeded"``) and
``env`` (``clk_elapsed``).

Deriving the per-state breakdown
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Which phase consumed the budget (``running``: compute, ``waiting``:
scheduler pressure, ``sleeping``: I/O) is recovered from ``event_tlob``,
the standard state-transition tracepoint.  Each window is a run of
``event_tlob`` lines for one pid: ``start`` opens it, ``stop`` closes it,
and a violation closes it with ``error_env_tlob``.  Attribute the time
between consecutive lines to the ``next_state`` of the earlier one::

  echo mono > /sys/kernel/tracing/trace_clock
  echo 1 > /sys/kernel/tracing/events/rv/event_tlob/enable
  echo 1 > /sys/kernel/tracing/events/rv/error_env_tlob/enable
  trace-cmd record -e rv:event_tlob -e rv:error_env_tlob
  # ... run workload ...
  trace-cmd report

The ``mono`` clock is required: ``event_tlob`` timestamps are compared
across CPUs because a task migrates during a window.  The three buckets
sum to ``clk_elapsed`` up to the tracepoint overhead.

Design notes
------------

- A fresh window always starts in ``running``: monitoring a task that
  is already waiting or sleeping at START misattributes the first
  interval to ``running``.
- ``TASK_STOPPED`` and ``TASK_TRACED`` carry ``prev_state != 0`` and
  count as ``sleeping``, indistinguishable from I/O-blocked time.
- A task preempted between ``set_current_state()`` and ``schedule()``
  carries ``prev_state != 0`` and counts as ``sleeping`` although it
  stays runnable.
- ``sched_wakeup_new`` is not hooked: a task is attached by a START
  probe in its own context, after the initial wakeup.

Specification
-------------

- Graphviz Dot file: tools/verification/models/tlob.dot
- KUnit tests: kernel/trace/rv/monitors/tlob/tlob_kunit.c
- User-space tests: tools/testing/selftests/verification/
