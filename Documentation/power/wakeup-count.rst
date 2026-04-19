.. SPDX-License-Identifier: GPL-2.0
.. Copyright (C) 2025 The Linux Foundation

The wakeup_count mechanism for race-free suspend
================================================

Overview
--------

The ``/sys/power/wakeup_count`` sysfs interface provides a stable userspace
mechanism to perform race-free system suspend transitions. It eliminates the
well-known race condition between suspend permission check and actual system
suspend entry.

Userspace may use it in a standard three-step sequence:

1. Read the current global wakeup event counter. The read operation blocks
   until all ongoing wakeup event processing is finished, returning a stable value.
2. Perform necessary suspend preparation steps in userspace.
3. Write the previously-read counter value back to the interface.
   The write operation will only succeed if no new wakeup events have occurred
   since the read.

Only after a successful write may userspace safely trigger system suspend.

Interface semantics
-------------------

``/sys/power/wakeup_count``

**Read**
    Returns the global monotonically-increasing wakeup event counter.
    This call blocks until there are no wakeup events under active processing
    inside the kernel. If interrupted by a signal, it returns -EINTR.

**Write**
    Accepts the counter value obtained from a prior read.
    The write succeeds only if the kernel's current counter exactly matches
    the written value. Mismatch indicates new wakeup events arrived during
    userspace preparation, and suspend must be aborted.

Standard userspace usage example
--------------------------------

.. code-block:: shell

    count=$(cat /sys/power/wakeup_count)
    do_suspend_preparation
    echo "$count" > /sys/power/wakeup_count && echo mem > /sys/power/state

Blocking behavior
-----------------

The blocking read ensures that userspace never observes an inconsistent state
where wakeup events are still being handled within the kernel. This stability
is the core guarantee of the interface.

Related kernel interfaces
-------------------------

- ``/sys/power/state``: System suspend state control interface.
- ``/sys/kernel/debug/wakeup_sources``: Per-device wakeup source statistics.
- ``Documentation/power/wakeup-events.rst``: General wakeup event framework.