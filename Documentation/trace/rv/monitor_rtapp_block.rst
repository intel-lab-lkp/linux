Monitor rtapp_block
=======================

- Name: rtapp_block - real time applications are undesirably blocked
- Type: per-task linear temporal logic monitor
- Author: Nam Cao <namcao@linutronix.de>

Introduction
------------

Real time threads could be blocked and fail to finish their execution timely. For instance, they
need to access shared resources which are already acquired by other threads. Or they could be
waiting for non-realtime threads to signal them to proceed: as the non-realtime threads are not
prioritized by the scheduler, the execution of realtime threads could be delayed indefinitely.
These scenarios are often unintentional, and cause unexpected latency to the realtime application.

The rtapp_block monitor reports this type of scenario, by monitoring for:

  * Realtime threads going to sleep without explicitly asking for it (namely, with nanosleep
    syscall).
  * Realtime threads are woken up by non-realtime threads.

How to fix the monitor's warnings?
----------------------------------

There is no single answer, the solution needs to be evaluated depending on the specific cases.

If the realtime thread is blocked trying to take a `pthread_mutex_t` which is already taken by a
non-realtime thread, the solution could be enabling priority inheritance for the mutex, so that the
blocking non-realtime thread would be priority-boosted to run at realtime priority.

If realtime thread needs to wait for non-realtime thread to signal it to proceed, perhaps the design
needs to be reconsidered to remove this dependency. Often, the work executed by the realtime thread
needs not to be realtime at all.
