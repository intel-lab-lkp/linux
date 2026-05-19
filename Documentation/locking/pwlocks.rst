.. SPDX-License-Identifier: GPL-2.0

=========
PW (Per-CPU Work) locks
=========

Some places in the kernel implement a parallel programming strategy
consisting on local_locks() for most of the work, and some rare remote
operations are scheduled on target cpu. This keeps cache bouncing low since
cacheline tends to be mostly local, and avoids the cost of locks in non-RT
kernels, even though the very few remote operations will be expensive due
to scheduling overhead.

On the other hand, for RT workloads this can represent a problem:
scheduling work on remote cpu that are executing low latency tasks
is undesired and can introduce unexpected deadline misses.

PW locks help to convert sites that use local_locks (for cpu local operations)
and queue_work_on (for queueing work remotely, to be executed
locally on the owner cpu of the lock) to a spinlocks.

The lock is declared pw_lock_t type.
The lock is initialized with pw_lock_init.
The lock is locked with pw_lock (takes a lock and cpu as a parameter).
The lock is unlocked with pw_unlock (takes a lock and cpu as a parameter).

The pw_lock_irqsave function disables interrupts and saves current interrupt state,
cpu as a parameter.

For trylock variant, there is the pw_trylock_t type, initialized with
pw_trylock_init. Then the corresponding pw_trylock and pw_trylock_irqsave.

work_struct should be replaced by pw_struct, which contains a cpu parameter
(owner cpu of the lock), initialized by INIT_PW.

The queue work related functions (analogous to queue_work_on and flush_work) are:
pw_queue_on and pw_flush.

The behaviour of the PW lock functions is as follows:

* !CONFIG_PWLOCKS (or CONFIG_PWLOCKS and pwlocks=off kernel boot parameter):
        - pw_lock:			local_lock
        - pw_lock_irqsave:		local_lock_irqsave
        - pw_trylock:			local_trylock
        - pw_trylock_irqsave:		local_trylock_irqsave
        - pw_unlock:			local_unlock
        - pw_lock_local:		local_lock
        - pw_trylock_local:		local_trylock
        - pw_unlock_local:		local_unlock
        - pw_queue_on:         		queue_work_on
        - pw_flush:	            	flush_work

* CONFIG_PWLOCKS (and CONFIG_PWLOCKS_DEFAULT=y or pwlocks=on kernel boot parameter),
        - pw_lock:			spin_lock
        - pw_lock_irqsave:		spin_lock_irqsave
        - pw_trylock:			spin_trylock
        - pw_trylock_irqsave:		spin_trylock_irqsave
        - pw_unlock:			spin_unlock
        - pw_lock_local:		preempt_disable OR migrate_disable + spin_lock
        - pw_trylock_local:		preempt_disable OR migrate_disable + spin_trylock
        - pw_unlock_local:		preempt_enable OR migrate_enable + spin_unlock
        - pw_queue_on:         		executes work function on caller cpu
        - pw_flush:            		empty

pw_get_cpu(work_struct), to be called from within per-cpu work function,
returns the target cpu.

On the locking functions above, there are the local locking functions
(pw_lock_local, pw_trylock_local and pw_unlock_local) that must only
be used to access per-CPU data from the CPU that owns that data,
and never remotely. They disable preemption/migration and don't require
a cpu parameter, making them a replacement for local_lock functions that
does not introduce overhead.

These should only be used when accessing per-CPU data of the local CPU.

