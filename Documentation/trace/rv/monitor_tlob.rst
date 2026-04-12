.. SPDX-License-Identifier: GPL-2.0

Monitor tlob
============

- Name: tlob - task latency over budget
- Type: per-task deterministic automaton
- Author: Wen Yang <wen.yang@linux.dev>

Description
-----------

The tlob monitor tracks per-task elapsed time (CLOCK_MONOTONIC, including
both on-CPU and off-CPU time) and reports a violation when the monitored
task exceeds a configurable latency budget threshold.

The monitor implements a three-state deterministic automaton::

                              |
                              | (initial)
                              v
                    +--------------+
          +-------> | unmonitored  |
          |         +--------------+
          |                |
          |          trace_start
          |                v
          |         +--------------+
          |         |   on_cpu     |
          |         +--------------+
          |           |         |
          |  switch_out|         | trace_stop / budget_expired
          |            v         v
          |  +--------------+  (unmonitored)
          |  |   off_cpu    |
          |  +--------------+
          |     |         |
          |     | switch_in| trace_stop / budget_expired
          |     v         v
          |  (on_cpu)  (unmonitored)
          |
          +-- trace_stop (from on_cpu or off_cpu)

  Key transitions:
    unmonitored   --(trace_start)-->   on_cpu
    on_cpu        --(switch_out)-->    off_cpu
    off_cpu       --(switch_in)-->     on_cpu
    on_cpu        --(trace_stop)-->    unmonitored
    off_cpu       --(trace_stop)-->    unmonitored
    on_cpu        --(budget_expired)-> unmonitored   [violation]
    off_cpu       --(budget_expired)-> unmonitored   [violation]

  sched_wakeup self-loops in on_cpu and unmonitored; switch_out and
  sched_wakeup self-loop in off_cpu.  budget_expired is fired by the one-shot hrtimer; it always
  transitions to unmonitored regardless of whether the task is on-CPU
  or off-CPU when the timer fires.

State Descriptions
------------------

- **unmonitored**: Task is not being traced.  Scheduling events
  (``switch_in``, ``switch_out``, ``sched_wakeup``) are silently
  ignored (self-loop).  The monitor waits for a ``trace_start`` event
  to begin a new observation window.

- **on_cpu**: Task is running on the CPU with the deadline timer armed.
  A one-shot hrtimer was set for ``threshold_us`` microseconds at
  ``trace_start`` time.  A ``switch_out`` event transitions to
  ``off_cpu``; the hrtimer keeps running (off-CPU time counts toward
  the budget).  A ``trace_stop`` cancels the timer and returns to
  ``unmonitored`` (normal completion).  If the hrtimer fires
  (``budget_expired``) the violation is recorded and the automaton
  transitions to ``unmonitored``.

- **off_cpu**: Task was preempted or blocked.  The one-shot hrtimer
  continues to run.  A ``switch_in`` event returns to ``on_cpu``.
  A ``trace_stop`` cancels the timer and returns to ``unmonitored``.
  If the hrtimer fires (``budget_expired``) while the task is off-CPU,
  the violation is recorded and the automaton transitions to
  ``unmonitored``.

Rationale
---------

The per-task latency budget threshold allows operators to express timing
requirements in microseconds and receive an immediate ftrace event when a
task exceeds its budget.  This is useful for real-time tasks
(``SCHED_FIFO`` / ``SCHED_DEADLINE``) where total elapsed time must
remain within a known bound.

Each task has an independent threshold, so up to ``TLOB_MAX_MONITORED``
(64) tasks with different timing requirements can be monitored
simultaneously.

On threshold violation the automaton records a ``tlob_budget_exceeded``
ftrace event carrying the final on-CPU / off-CPU time breakdown, but does
not kill or throttle the task.  Monitoring can be restarted by issuing a
new ``trace_start`` event (or a new ``TLOB_IOCTL_TRACE_START`` ioctl).

A per-task one-shot hrtimer is armed at ``trace_start`` for exactly
``threshold_us`` microseconds.  It fires at most once per monitoring
window, performs an O(1) hash lookup, records the violation, and injects
the ``budget_expired`` event into the DA.  When ``CONFIG_RV_MON_TLOB``
is not set there is zero runtime cost.

Usage
-----

tracefs interface (uprobe-based external monitoring)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The ``monitor`` tracefs file allows any privileged user to instrument an
unmodified binary via uprobes, without changing its source code.  Write a
four-field record to attach two plain entry uprobes: one at
``offset_start`` fires ``tlob_start_task()`` and one at ``offset_stop``
fires ``tlob_stop_task()``, so the latency budget covers exactly the code
region between the two offsets::

  threshold_us:offset_start:offset_stop:binary_path

``binary_path`` comes last so it may freely contain ``:`` (e.g. paths
inside a container namespace).

The uprobes fire for every task that executes the probed instruction in
the binary, consistent with the native uprobe semantics.  All tasks that
execute the code region get independent per-task monitoring slots.

Using two plain entry uprobes (rather than a uretprobe for the stop) means
that a mistyped offset can never corrupt the call stack; the worst outcome
of a bad ``offset_stop`` is a missed stop that causes the hrtimer to fire
and report a budget violation.

Example  --  monitor a code region in ``/usr/bin/myapp`` with a 5 ms
budget, where the region starts at offset 0x12a0 and ends at 0x12f0::

  echo 1 > /sys/kernel/tracing/rv/monitors/tlob/enable

  # Bind uprobes: start probe starts the clock, stop probe stops it
  echo "5000:0x12a0:0x12f0:/usr/bin/myapp" \
      > /sys/kernel/tracing/rv/monitors/tlob/monitor

  # Remove the uprobe binding for this code region
  echo "-0x12a0:/usr/bin/myapp" > /sys/kernel/tracing/rv/monitors/tlob/monitor

  # List registered uprobe bindings (mirrors the write format)
  cat /sys/kernel/tracing/rv/monitors/tlob/monitor
  # -> 5000:0x12a0:0x12f0:/usr/bin/myapp

  # Read violations from the trace buffer
  cat /sys/kernel/tracing/trace

Up to ``TLOB_MAX_MONITORED`` tasks may be monitored simultaneously.

The offsets can be obtained with ``nm`` or ``readelf``::

  nm -n /usr/bin/myapp | grep my_function
  # -> 0000000000012a0 T my_function

  readelf -s /usr/bin/myapp | grep my_function
  # -> 42: 0000000000012a0  336 FUNC GLOBAL DEFAULT  13 my_function

  # offset_start = 0x12a0 (function entry)
  # offset_stop  = 0x12a0 + 0x50 = 0x12f0 (or any instruction before return)

Notes:

- The uprobes fire for every task that executes the probed instruction,
  so concurrent calls from different threads each get independent
  monitoring slots.
- ``offset_stop`` need not be a function return; it can be any instruction
  within the region.  If the stop probe is never reached (e.g. early exit
  path bypasses it), the hrtimer fires and a budget violation is reported.
- Each ``(binary_path, offset_start)`` pair may only be registered once.
  A second write with the same ``offset_start`` for the same binary is
  rejected with ``-EEXIST``.  Two entry uprobes at the same address would
  both fire for every task, causing ``tlob_start_task()`` to be called
  twice; the second call would silently fail with ``-EEXIST`` and the
  second binding's threshold would never take effect.  Different code
  regions that share the same ``offset_stop`` (common exit point) are
  explicitly allowed.
- The uprobe binding is removed when ``-offset_start:binary_path`` is
  written to ``monitor``, or when the monitor is disabled.
- The ``tag`` field in every ``tlob_budget_exceeded`` event is
  automatically set to ``offset_start`` for the tracefs path, so
  violation events for different code regions are immediately
  distinguishable even when ``threshold_us`` values are identical.

ftrace ring buffer (budget violation events)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

When a monitored task exceeds its latency budget the hrtimer fires,
records the violation, and emits a single ``tlob_budget_exceeded`` event
into the ftrace ring buffer.  **Nothing is written to the ftrace ring
buffer while the task is within budget.**

The event carries the on-CPU / off-CPU time breakdown so that root-cause
analysis (CPU-bound vs. scheduling / I/O overrun) is immediate::

  cat /sys/kernel/tracing/trace

Example output::

  myapp-1234 [003] .... 12345.678: tlob_budget_exceeded: \
    myapp[1234]: budget exceeded threshold=5000 \
    on_cpu=820 off_cpu=4500 switches=3 state=off_cpu tag=0x00000000000012a0

Field descriptions:

``threshold``
  Configured latency budget in microseconds.

``on_cpu``
  Cumulative on-CPU time since ``trace_start``, in microseconds.

``off_cpu``
  Cumulative off-CPU (scheduling + I/O wait) time since ``trace_start``,
  in microseconds.

``switches``
  Number of times the task was scheduled out during this window.

``state``
  DA state when the hrtimer fired: ``on_cpu`` means the task was executing
  when the budget expired (CPU-bound overrun); ``off_cpu`` means the task
  was preempted or blocked (scheduling / I/O overrun).

``tag``
  Opaque 64-bit cookie supplied by the caller via ``tlob_start_args.tag``
  (ioctl path) or automatically set to ``offset_start`` (tracefs uprobe
  path).  Use it to distinguish violations from different code regions
  monitored by the same thread.  Zero when not set.

To capture violations in a file::

  trace-cmd record -e tlob_budget_exceeded &
  # ... run workload ...
  trace-cmd report

/dev/rv ioctl interface (self-instrumentation)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Tasks can self-instrument their own code paths via the ``/dev/rv`` misc
device (requires ``CONFIG_RV_CHARDEV``).  The kernel key is
``task_struct``; multiple threads sharing a single fd each get their own
independent monitoring slot.

**Synchronous mode**  --  the calling thread checks its own result::

  int fd = open("/dev/rv", O_RDWR);

  struct tlob_start_args args = {
      .threshold_us = 50000,   /* 50 ms */
      .tag          = 0,       /* optional; 0 = don't care */
      .notify_fd    = -1,      /* no fd notification */
  };
  ioctl(fd, TLOB_IOCTL_TRACE_START, &args);

  /* ... code path under observation ... */

  int ret = ioctl(fd, TLOB_IOCTL_TRACE_STOP, NULL);
  /* ret == 0:          within budget  */
  /* ret == -EOVERFLOW: budget exceeded */

  close(fd);

**Asynchronous mode**  --  a dedicated monitor thread receives violation
records via ``read()`` on a shared fd, decoupling the observation from
the critical path::

  /* Monitor thread: open a dedicated fd. */
  int monitor_fd = open("/dev/rv", O_RDWR);

  /* Worker thread: set notify_fd = monitor_fd in TRACE_START args. */
  int work_fd = open("/dev/rv", O_RDWR);
  struct tlob_start_args args = {
      .threshold_us = 10000,   /* 10 ms */
      .tag          = REGION_A,
      .notify_fd    = monitor_fd,
  };
  ioctl(work_fd, TLOB_IOCTL_TRACE_START, &args);
  /* ... critical section ... */
  ioctl(work_fd, TLOB_IOCTL_TRACE_STOP, NULL);

  /* Monitor thread: blocking read() returns one or more tlob_event records. */
  struct tlob_event ntfs[8];
  ssize_t n = read(monitor_fd, ntfs, sizeof(ntfs));
  for (int i = 0; i < n / sizeof(struct tlob_event); i++) {
      struct tlob_event *ntf = &ntfs[i];
      printf("tid=%u tag=0x%llx exceeded budget=%llu us "
             "(on_cpu=%llu off_cpu=%llu switches=%u state=%s)\n",
             ntf->tid, ntf->tag, ntf->threshold_us,
             ntf->on_cpu_us, ntf->off_cpu_us, ntf->switches,
             ntf->state ? "on_cpu" : "off_cpu");
  }

**mmap ring buffer**  --  zero-copy consumption of violation events::

  int fd = open("/dev/rv", O_RDWR);
  struct tlob_start_args args = {
      .threshold_us = 1000,   /* 1 ms */
      .notify_fd    = fd,     /* push violations to own ring buffer */
  };
  ioctl(fd, TLOB_IOCTL_TRACE_START, &args);

  /* Map the ring: one control page + capacity data records. */
  size_t pagesize = sysconf(_SC_PAGESIZE);
  size_t cap = 64;   /* read from page->capacity after mmap */
  size_t len = pagesize + cap * sizeof(struct tlob_event);
  void *map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  struct tlob_mmap_page *page = map;
  struct tlob_event *data =
      (struct tlob_event *)((char *)map + page->data_offset);

  /* Consumer loop: poll for events, read without copying. */
  while (1) {
      poll(&(struct pollfd){fd, POLLIN, 0}, 1, -1);

      uint32_t head = __atomic_load_n(&page->data_head, __ATOMIC_ACQUIRE);
      uint32_t tail = page->data_tail;
      while (tail != head) {
          handle(&data[tail & (page->capacity - 1)]);
          tail++;
      }
      __atomic_store_n(&page->data_tail, tail, __ATOMIC_RELEASE);
  }

Note: ``read()`` and ``mmap()`` share the same ring and ``data_tail``
cursor.  Do not use both simultaneously on the same fd.

``tlob_event`` fields:

``tid``
  Thread ID (``task_pid_vnr``) of the violating task.

``threshold_us``
  Budget that was exceeded, in microseconds.

``on_cpu_us``
  Cumulative on-CPU time at violation time, in microseconds.

``off_cpu_us``
  Cumulative off-CPU time at violation time, in microseconds.

``switches``
  Number of context switches since ``TRACE_START``.

``state``
  1 = timer fired while task was on-CPU; 0 = timer fired while off-CPU.

``tag``
  Cookie from ``tlob_start_args.tag``; for the tracefs uprobe path this
  equals ``offset_start``.  Zero when not set.

tracefs files
-------------

The following files are created under
``/sys/kernel/tracing/rv/monitors/tlob/``:

``enable`` (rw)
  Write ``1`` to enable the monitor; write ``0`` to disable it and
  stop all currently monitored tasks.

``desc`` (ro)
  Human-readable description of the monitor.

``monitor`` (rw)
  Write ``threshold_us:offset_start:offset_stop:binary_path`` to bind two
  plain entry uprobes in *binary_path*.  The uprobe at *offset_start* fires
  ``tlob_start_task()``; the uprobe at *offset_stop* fires
  ``tlob_stop_task()``.  Returns ``-EEXIST`` if a binding with the same
  *offset_start* already exists for *binary_path*.  Write
  ``-offset_start:binary_path`` to remove the binding.  Read to list
  registered bindings, one
  ``threshold_us:0xoffset_start:0xoffset_stop:binary_path`` entry per line.

Specification
-------------

Graphviz DOT file in tools/verification/models/tlob.dot
