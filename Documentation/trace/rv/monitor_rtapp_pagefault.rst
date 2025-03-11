Monitor rtapp_pagefault
=======================

- Name: rtapp_pagefault - realtime applications raising page faults
- Type: per-task linear temporal logic monitor
- Author: Nam Cao <namcao@linutronix.de>

Introduction
------------

One of the most devastating situations for a real-time application is the need to assign or "page
in" memory. This can be due to the over-commitment behavior of Linux when accessing allocated or
reserved memory for the first time. Or it can be paging in disk data (such as text segments) when
calling functions for the first time. Whatever the case, it must be avoided in order to meet
response requirements.

The monitor reports these situation where real-time applications raise page faults.

How to fix the monitor's warnings?
----------------------------------

The first thing a real-time application needs to do is configure glibc to use a single
non-shrinkable heap for the application. This guarantees that a pool of readily accessible physical
RAM can be made available to the real-time application. This is accomplished using the mallopt(3)
function (M_MMAP_MAX=0, M_ARENA_MAX=1, M_TRIM_THRESHOLD=-1).

Next, all allocated and mapped virtual memory must be assigned to physical RAM and locked so that it
cannot be reclaimed for other purposes. This is accomplished using the mlockall(2) function
(MCL_CURRENT | MCL_FUTURE).

Finally, the amounts of stack and heap needed during the lifetime of the real-time application must
be allocated and written to in order to trigger heap and stack assignments to physical RAM. This is
known as pre-faulting and is usually accomplished by memsetting a large buffer within a stack frame
and allocating, memsetting, and freeing a large heap buffer.

Pitfall: Keep in mind that each thread will have its own stack.
