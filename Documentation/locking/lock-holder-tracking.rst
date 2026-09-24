.. SPDX-License-Identifier: GPL-2.0

=============================
Sleeping lock holder tracking
=============================

:Date: September 2026
:Author: Shakeel Butt <shakeel.butt@linux.dev>

Why
===

A task that stalls while holding a widely shared lock also stalls the
tasks waiting for that lock. Some kernel code wants to avoid stalling
such a task.

One source of such stalls is a memory allocation by the lock holder,
which can get stuck in memory reclaim because the system is low on
memory or because a memcg limit is being enforced. If an allocation made
while holding ``cgroup_mutex`` or ``kernfs_rwsem`` gets stuck in reclaim,
other cgroupfs operations can end up waiting too, even for unrelated
cgroups.

Knowing that the task holds such a lock lets the allocation path treat it
differently. Under a memcg limit it can skip the throttling that
enforcement would otherwise do inline, and leave it for the return to
user space. Under global pressure it can let the allocation reach the
memory reserves, so it finishes instead of waiting for reclaim. Those
policies are not part of this feature; they only need the answer.

What
====

Locks do not record all their holders, so the answer comes from the task
instead. Each task counts the opted-in locks it holds::

	static inline bool task_holds_tracked_lock(struct task_struct *p);
	static inline bool current_holds_tracked_lock(void);
	static inline unsigned int task_nr_tracked_locks(struct task_struct *p);

Only the task itself changes its count, so the count needs no atomics or
locking. Without ``CONFIG_TRACK_LOCK_HOLDERS``, these always return
``false`` or ``0``.

BPF programs can read ``task->nr_tracked_locks`` through BTF.

Opting a lock in
================

No lock is tracked by default. A lock opts in right after it is
initialized::

	init_rwsem(&root->kernfs_rwsem);
	rwsem_track_holder(&root->kernfs_rwsem);

	mutex_init(&foo->lock);
	mutex_track_holder(&foo->lock);

	percpu_init_rwsem(&foo->sem);
	percpu_rwsem_track_holder(&foo->sem);

Only a few locks should opt in: those whose holders can stall unrelated
work. A lock must opt in before anyone can take it. A task already
holding it was not counted, so its release would make its count wrong.
All three helpers warn and do nothing if the lock is held. Initializing
a lock again clears its opt-in.

A tracked rw_semaphore must not use ``down_read_non_owner()`` or
``up_read_non_owner()``, which can run in different tasks.
``down_read_non_owner()`` warns on a tracked rwsem and does not count it.

A percpu_rw_semaphore can be passed to another task with
``percpu_rwsem_release()`` and ``percpu_rwsem_acquire()``, as filesystem
freeze does. The count moves with the lock.

Cost
====

With ``CONFIG_TRACK_LOCK_HOLDERS=n``, the generated code does not change.

With it enabled, no lock type grows on 64-bit, so neither do the
structures that embed them. That relies on padding which is not there on
every configuration: see the end of this section.

A mutex and an rw_semaphore keep the opt-in in a spare bit of a word they
already have: bit 3 of ``rw_semaphore::count``, which is reserved, and
bit 3 of ``mutex::owner``, which is below the alignment of a task_struct
pointer. The bit is sticky, so unlike the other flag bits it survives
unlock, and an unlocked lock that has opted in does not read as zero.
Paths that add to these words, or write back a value they read, keep the
bit as they are. The three that write an absolute value -
``rwsem_write_trylock()``, ``__mutex_trylock_fast()`` and
``__mutex_unlock_fast()`` - read the bit first and put it in both the
expected and the new value, so each still does a single cmpxchg. Reading
it needs no lock, because it is sticky.

``percpu_rw_semaphore`` keeps a plain flag, which fits in the padding
after ``->block``. On ``PREEMPT_RT``, a mutex and an rw_semaphore are
built on ``rt_mutex_base``, which has no spare bit but does have padding
after ``->wait_lock``, so the flag goes there.

Those two rely on padding that only exists on 64-bit. On 32-bit
``percpu_rw_semaphore`` grows by a word, and on 32-bit ``PREEMPT_RT`` so
does ``rt_mutex_base``, which means everything built on it including
``spinlock_t``. The sticky bit in ``mutex`` and ``rw_semaphore`` has no
such problem, as it uses no storage at all.

Until the first lock opts in, each hook is a static branch that is off,
a 2-byte NOP on x86_64, and the flag reads sit behind it, so every lock
runs the instructions it ran before. The first opt-in turns the branch
on for good. After that, an acquire and a release each also read the
word they are about to update and test the flag, which costs one load
from a cacheline the lock operation touches anyway.
``__mutex_trylock_common()`` needs no load of its own, as it already
has the value.

Where the count changes
=======================

The count goes up once the lock is taken and down before it is released.
Waiting is not counted, as the task does not hold the lock yet.

mutex (``!PREEMPT_RT``)
	Only ``__mutex_trylock_common()`` and ``__mutex_trylock_fast()`` take
	the lock for current, and only ``__mutex_unlock_fast()`` and
	``__mutex_unlock_slowpath()`` release it. These four places cover
	every ``mutex_lock*()``, ``mutex_trylock()``, ``ww_mutex_lock*()``
	and handoff. ``__mutex_lock_common()`` has no hook of its own: it
	reaches ``__mutex_trylock_common()`` through ``__mutex_trylock()``
	and ``__mutex_trylock_or_handoff()``.

mutex (``PREEMPT_RT``)
	``__mutex_lock_common()``, ``mutex_trylock()``,
	``_mutex_trylock_nest_lock()`` and ``mutex_unlock()`` in
	``rtmutex_api.c``.

rw_semaphore
	The API functions in ``rwsem.c``, which ``PREEMPT_RT`` and
	``!PREEMPT_RT`` share. ``downgrade_write()`` keeps the lock held, so
	it needs no hook.

percpu_rw_semaphore
	``percpu_down_read_internal()``, ``percpu_down_read_trylock()`` and
	``percpu_up_read()`` in the header, ``percpu_down_write()`` and
	``percpu_up_write()`` in ``percpu-rwsem.c``, and
	``percpu_rwsem_acquire()``/``percpu_rwsem_release()``. The read fast
	and slow paths meet before the hook, so one hook covers both.

Not covered: ``rt_mutex`` used directly, ``ww_mutex`` on ``PREEMPT_RT``
(it is an ``rt_mutex`` there), ``struct semaphore``, and spinning locks.
Only ``mutex`` and ``rw_semaphore`` have a ``*_track_holder()`` helper.
To add a lock type, find it a place to keep the opt-in that does not make
it bigger, then call ``lock_holder_acquired_if()`` and
``lock_holder_released_if()`` where it is taken and released. If the lock has a ``bool tracked`` member, as
``percpu_rw_semaphore`` does, ``lock_holder_acquired()`` and
``lock_holder_released()`` are the same thing with the member filled in.

Debugging
=========

``do_exit()`` warns once if a task exits with a non-zero count, and the
release hook warns once instead of letting the count go below zero.
Either means a bug, usually a missed hook.

``CONFIG_TRACK_LOCK_HOLDERS_KUNIT_TEST`` tests the count for all three
lock types::

	tools/testing/kunit/kunit.py run --kunitconfig=kernel/locking/ lockholder
