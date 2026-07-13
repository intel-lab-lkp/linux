.. SPDX-License-Identifier: GPL-2.0

=======================================
The padata parallel execution mechanism
=======================================

:Date: May 2020

Padata is a mechanism by which the kernel can farm jobs out to be done in
parallel on multiple CPUs while coordinating between threads.

Padata supports multithreaded jobs, splitting up the job evenly while load
balancing and coordinating between threads.

Running Multithreaded Jobs
==========================

A multithreaded job has a main thread and zero or more helper threads, with the
main thread participating in the job and then waiting until all helpers have
finished.  padata splits the job into units called chunks, where a chunk is a
piece of the job that one thread completes in one call to the thread function.

A user has to do three things to run a multithreaded job.  First, describe the
job by defining a padata_mt_job structure, which is explained in the Interface
section.  This includes a pointer to the thread function, which padata will
call each time it assigns a job chunk to a thread.  Then, define the thread
function, which accepts three arguments, ``start``, ``end``, and ``arg``, where
the first two delimit the range that the thread operates on and the last is a
pointer to the job's shared state, if any.  Prepare the shared state, which is
typically allocated on the main thread's stack.  Last, call
padata_do_multithreaded(), which will return once the job is finished.

Interface
=========

.. kernel-doc:: include/linux/padata.h
.. kernel-doc:: kernel/padata.c
