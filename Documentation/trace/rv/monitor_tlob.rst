.. SPDX-License-Identifier: GPL-2.0

Monitor tlob
============

- Name: tlob - task latency over budget
- Type: per-object hybrid automaton (RV_MON_PER_OBJ)
- Author: Wen Yang <wen.yang@linux.dev>

Description
-----------

The tlob monitor tracks per-task elapsed wall-clock time (CLOCK_MONOTONIC,
spanning running, waiting, and sleeping states) and reports a violation when
the monitored task exceeds a configurable per-invocation budget threshold.

The monitor implements a three-state hybrid automaton with a single clock
environment variable ``clk_elapsed``.  The clock invariant
``clk_elapsed < BUDGET_NS()`` is active in all three states; when it is
violated the HA timer fires and the framework emits ``error_env_tlob``
then calls ``da_monitor_reset()`` automatically::

                  | (initial, via task_start)
                  v
           +--------------+
           |   running    | <-----------+
           +--------------+             |
             |         |                |
           sleep     preempt        switch_in
             |         |                |
             v         v                |
        +---------+  +---------+        |
        | sleeping|  | waiting | -------+
        +---------+  +---------+
             |            ^
             +---wakeup---+

  Key transitions:
    running  --(sleep)------> sleeping   (task blocks waiting for a resource)
    running  --(preempt)----> waiting    (task preempted, back in runqueue)
    sleeping --(wakeup)-----> waiting    (resource available, enters runqueue)
    waiting  --(switch_in)--> running    (scheduler picks task, back on CPU)

  ``tlob_start_task()`` calls ``da_handle_start_run_event(task->pid, ws, start_tlob)``.
  The ``start_tlob`` self-loop on the ``running`` state triggers
  ``ha_setup_invariants()``, which resets ``clk_elapsed`` and arms the budget
  timer automatically.  ``tlob_stop_task()`` cancels the HA timer synchronously
  via ``ha_cancel_timer_sync()``, then calls ``da_monitor_reset()``.

The non-running condition (monitor not yet started or reset after a
stop/violation) is handled implicitly by the RV framework
(``da_mon->monitoring == 0``) - it is not an explicit DA state.

Per-task state lives in ``struct tlob_task_state`` which is stored as
``monitor_target`` in the framework's ``da_monitor_storage``, indexed by
pid.  The per-invocation ``threshold_ns`` is read via
``ha_get_target(ha_mon)->threshold_ns`` inside the HA constraint functions,
following the same pattern as the ``nomiss`` monitor.

Usage
-----

tracefs interface (uprobe-based external monitoring)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The ``monitor`` tracefs file instruments an unmodified binary via uprobes.
The format follows the ftrace ``uprobe_events`` convention (``PATH:OFFSET``
for the probe location, ``key=value`` for configuration parameters)::

  p PATH:OFFSET_START OFFSET_STOP threshold=NS

The uprobe at ``OFFSET_START`` fires ``tlob_start_task()``; the uprobe at
``OFFSET_STOP`` fires ``tlob_stop_task()``.  Both offsets are ELF file
offsets of entry points in ``PATH``.  ``PATH`` may contain ``:``; the last
``:`` in the ``PATH:OFFSET_START`` token is the separator.

To remove a binding, use ``-PATH:OFFSET_START``::

  echo 1 > /sys/kernel/tracing/rv/monitors/tlob/enable

  echo "p /usr/bin/myapp:0x12a0 0x12f0 threshold=5000000" \
      > /sys/kernel/tracing/rv/monitors/tlob/monitor

  # Remove a binding
  echo "-/usr/bin/myapp:0x12a0" > /sys/kernel/tracing/rv/monitors/tlob/monitor

  # List registered bindings
  cat /sys/kernel/tracing/rv/monitors/tlob/monitor

  # Read violations from the trace buffer
  cat /sys/kernel/tracing/trace

Violation tracepoints
~~~~~~~~~~~~~~~~~~~~~

Two tracepoints are emitted together on a budget violation:

``error_env_tlob``
  Standard HA clock-invariant tracepoint (emitted by the RV framework).
  Fields: ``id`` (task pid), ``state``, ``event`` (``"budget_exceeded"``),
  ``env`` (``"clk_elapsed"``).

``detail_env_tlob``
  Tlob-specific breakdown of elapsed time per DA state.
  Fields: ``id`` (task pid), ``threshold_ns``, ``running_ns``,
  ``waiting_ns``, ``sleeping_ns``.

  Use ``detail_env_tlob`` to diagnose *which phase* consumed the budget:
  high ``sleeping_ns`` indicates I/O latency; high ``waiting_ns`` indicates
  scheduler pressure; high ``running_ns`` indicates a compute overrun.

Example: correlate the two tracepoints to see the breakdown::

  trace-cmd record -e error_env_tlob -e detail_env_tlob &
  # ... run workload ...
  trace-cmd report

tracefs files
~~~~~~~~~~~~~

The following files are specific to tlob under
``/sys/kernel/tracing/rv/monitors/tlob/``:

``monitor`` (rw)
  Write ``p PATH:OFFSET_START OFFSET_STOP threshold=NS``
  to bind two entry uprobes.  Write ``-PATH:OFFSET_START`` to remove a
  binding.  Read to list registered bindings in the same format.
  See the `tracefs interface (uprobe-based external monitoring)`_ section above.

Kernel API
----------

``tlob_start_task`` and ``tlob_stop_task`` are the implementation-level
functions called by the uprobe entry/exit handlers; the interface is
driven from userspace.

.. kernel-doc:: kernel/trace/rv/monitors/tlob/tlob.c
   :functions: tlob_start_task tlob_stop_task

``tlob_start_task(task, threshold_ns)``
  Begin monitoring *task* with a total latency budget of *threshold_ns*
  nanoseconds.  Allocates per-task state, sets initial DA state to
  ``running``, resets ``clk_elapsed``, and arms the HA budget timer.
  Returns 0, -ENODEV (monitor disabled), -ERANGE (threshold out of range),
  -EALREADY (already monitoring), -ENOSPC (at capacity), or -ENOMEM.

``tlob_stop_task(task)``
  Stop monitoring *task*.  Synchronously cancels the HA timer via
  ``ha_cancel_timer_sync()``, checks ``da_monitoring()`` to determine outcome.
  Returns 0 (clean stop, within budget), -EOVERFLOW (budget was exceeded),
  -ESRCH (not monitored), or -EAGAIN (concurrent stop racing).

Design notes
------------

Limitations:

- The initial DA state is always ``running``, set by feeding the synthetic
  event ``switch_in_tlob`` to ``da_handle_start_event()``.  Monitoring a non-current
  task that is already in waiting or sleeping state at call time misclassifies
  the first interval as ``running_ns``.
- ``TASK_STOPPED`` and ``TASK_TRACED`` carry ``prev_state != 0`` and are
  therefore counted as ``sleeping_ns``, indistinguishable from
  I/O-blocked time.
- ``sched_wakeup_new`` is not hooked.  In practice this is not an issue
  because ``tlob_start_task`` is always called from a running context.

Specification
-------------

Graphviz DOT file in tools/verification/models/tlob.dot.

KUnit tests under ``kernel/trace/rv/monitors/tlob/tlob_kunit.c``
(CONFIG_TLOB_KUNIT_TEST).

User-space integration tests under ``tools/testing/selftests/verification/``
(requires CONFIG_RV_MON_TLOB=y and root).
