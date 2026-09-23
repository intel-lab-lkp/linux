/* SPDX-License-Identifier: GPL-2.0 */
/* rwsem.h: R/W semaphores, public interface
 *
 * Written by David Howells (dhowells@redhat.com).
 * Derived from asm-i386/semaphore.h
 */

#ifndef _LINUX_RWSEM_H
#define _LINUX_RWSEM_H

#include <linux/linkage.h>

#include <linux/types.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/err.h>
#include <linux/cleanup.h>

#ifdef CONFIG_DEBUG_LOCK_ALLOC
# define __RWSEM_DEP_MAP_INIT(lockname)			\
	.dep_map = {					\
		.name = #lockname,			\
		.wait_type_inner = LD_WAIT_SLEEP,	\
	},
#else
# define __RWSEM_DEP_MAP_INIT(lockname)
#endif

#ifndef CONFIG_PREEMPT_RT

#ifdef CONFIG_RWSEM_SPIN_ON_OWNER
#include <linux/osq_lock.h>
#endif

/*
 * For an uncontended rwsem, count and owner are the only fields a task
 * needs to touch when acquiring the rwsem. So they are put next to each
 * other to increase the chance that they will share the same cacheline.
 *
 * In a contended rwsem, the owner is likely the most frequently accessed
 * field in the structure as the optimistic waiter that holds the osq lock
 * will spin on owner. For an embedded rwsem, other hot fields in the
 * containing structure should be moved further away from the rwsem to
 * reduce the chance that they will share the same cacheline causing
 * cacheline bouncing problem.
 */
context_lock_struct(rw_semaphore) {
	atomic_long_t count;
	/*
	 * Write owner or one of the read owners as well flags regarding
	 * the current state of the rwsem. Can be used as a speculative
	 * check to see if the write owner is running on the cpu.
	 */
	atomic_long_t owner;
#ifdef CONFIG_RWSEM_SPIN_ON_OWNER
	struct optimistic_spin_queue osq; /* spinner MCS lock */
#endif
	raw_spinlock_t wait_lock;
	struct rwsem_waiter *first_waiter __guarded_by(&wait_lock);
#ifdef CONFIG_DEBUG_RWSEMS
	void *magic;
#endif
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	dep_map;
#endif
};

#define RWSEM_UNLOCKED_VALUE		0UL
#define RWSEM_WRITER_LOCKED		(1UL << 0)
#ifdef CONFIG_TRACK_LOCK_HOLDERS
/*
 * Sticky: set by rwsem_track_holder(), cleared only by init. count bits
 * 3-7 are reserved, and the lock and unlock paths only add to count or
 * write back a value they read, so the bit survives. See
 * kernel/locking/rwsem.c.
 */
#define RWSEM_FLAG_TRACKED		(1UL << 3)
#else
#define RWSEM_FLAG_TRACKED		0UL
#endif
#define __RWSEM_COUNT_INIT(name)	.count = ATOMIC_LONG_INIT(RWSEM_UNLOCKED_VALUE)

static inline int rwsem_is_locked(struct rw_semaphore *sem)
{
	return (atomic_long_read(&sem->count) & ~RWSEM_FLAG_TRACKED) !=
		RWSEM_UNLOCKED_VALUE;
}

static inline void rwsem_assert_held_nolockdep(const struct rw_semaphore *sem)
	__assumes_ctx_lock(sem)
{
	WARN_ON((atomic_long_read(&sem->count) & ~RWSEM_FLAG_TRACKED) ==
		RWSEM_UNLOCKED_VALUE);
}

static inline void rwsem_assert_held_write_nolockdep(const struct rw_semaphore *sem)
	__assumes_ctx_lock(sem)
{
	WARN_ON(!(atomic_long_read(&sem->count) & RWSEM_WRITER_LOCKED));
}

static inline bool rwsem_is_tracked(const struct rw_semaphore *sem)
{
	if (!IS_ENABLED(CONFIG_TRACK_LOCK_HOLDERS))
		return false;

	return atomic_long_read(&sem->count) & RWSEM_FLAG_TRACKED;
}

#ifdef CONFIG_TRACK_LOCK_HOLDERS
static inline void __rwsem_set_tracked(struct rw_semaphore *sem)
{
	atomic_long_or(RWSEM_FLAG_TRACKED, &sem->count);
}
#endif

/* Common initializer macros and functions */

#ifdef CONFIG_DEBUG_RWSEMS
# define __RWSEM_DEBUG_INIT(lockname) .magic = &lockname,
#else
# define __RWSEM_DEBUG_INIT(lockname)
#endif

#ifdef CONFIG_RWSEM_SPIN_ON_OWNER
#define __RWSEM_OPT_INIT(lockname) .osq = OSQ_LOCK_UNLOCKED,
#else
#define __RWSEM_OPT_INIT(lockname)
#endif

#define __RWSEM_INITIALIZER(name)				\
	{ __RWSEM_COUNT_INIT(name),				\
	  .owner = ATOMIC_LONG_INIT(0),				\
	  __RWSEM_OPT_INIT(name)				\
	  .wait_lock = __RAW_SPIN_LOCK_UNLOCKED(name.wait_lock),\
	  .first_waiter = NULL,					\
	  __RWSEM_DEBUG_INIT(name)				\
	  __RWSEM_DEP_MAP_INIT(name) }

#define DECLARE_RWSEM(name) \
	struct rw_semaphore name = __RWSEM_INITIALIZER(name)

extern void __init_rwsem(struct rw_semaphore *sem, const char *name,
			 struct lock_class_key *key);

#define init_rwsem(sem)						\
do {								\
	static struct lock_class_key __key;			\
								\
	__init_rwsem((sem), #sem, &__key);			\
} while (0)

/*
 * This is the same regardless of which rwsem implementation that is being used.
 * It is just a heuristic meant to be called by somebody already holding the
 * rwsem to see if somebody from an incompatible type is wanting access to the
 * lock.
 */
static inline bool rwsem_is_contended(struct rw_semaphore *sem)
{
	return data_race(sem->first_waiter != NULL);
}

#if defined(CONFIG_DEBUG_RWSEMS) || defined(CONFIG_DETECT_HUNG_TASK_BLOCKER)
/*
 * Return just the real task structure pointer of the owner
 */
extern struct task_struct *rwsem_owner(struct rw_semaphore *sem);

/*
 * Return true if the rwsem is owned by a reader.
 */
extern bool is_rwsem_reader_owned(struct rw_semaphore *sem);
#endif

#else /* !CONFIG_PREEMPT_RT */

#include <linux/rwbase_rt.h>

context_lock_struct(rw_semaphore) {
	struct rwbase_rt	rwbase;
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	dep_map;
#endif
};

/* The opt-in lives in the padding inside the rtmutex. */
static inline bool rwsem_is_tracked(const struct rw_semaphore *sem)
{
#ifdef CONFIG_TRACK_LOCK_HOLDERS
	return sem->rwbase.rtmutex.tracked;
#else
	return false;
#endif
}

#ifdef CONFIG_TRACK_LOCK_HOLDERS
static inline void __rwsem_set_tracked(struct rw_semaphore *sem)
{
	sem->rwbase.rtmutex.tracked = true;
}
#endif

#define __RWSEM_INITIALIZER(name)				\
	{							\
		.rwbase = __RWBASE_INITIALIZER(name),		\
		__RWSEM_DEP_MAP_INIT(name)			\
	}

#define DECLARE_RWSEM(lockname) \
	struct rw_semaphore lockname = __RWSEM_INITIALIZER(lockname)

extern void  __init_rwsem(struct rw_semaphore *rwsem, const char *name,
			  struct lock_class_key *key);

#define init_rwsem(sem)						\
do {								\
	static struct lock_class_key __key;			\
								\
	__init_rwsem((sem), #sem, &__key);			\
} while (0)

static __always_inline int rwsem_is_locked(const struct rw_semaphore *sem)
{
	return rw_base_is_locked(&sem->rwbase);
}

static __always_inline void rwsem_assert_held_nolockdep(const struct rw_semaphore *sem)
	__assumes_ctx_lock(sem)
{
	WARN_ON(!rwsem_is_locked(sem));
}

static __always_inline void rwsem_assert_held_write_nolockdep(const struct rw_semaphore *sem)
	__assumes_ctx_lock(sem)
{
	WARN_ON(!rw_base_is_write_locked(&sem->rwbase));
}

static __always_inline int rwsem_is_contended(struct rw_semaphore *sem)
{
	return rw_base_is_contended(&sem->rwbase);
}

#endif /* CONFIG_PREEMPT_RT */

/*
 * The functions below are the same for all rwsem implementations including
 * the RT specific variant.
 */

static inline void rwsem_assert_held(const struct rw_semaphore *sem)
	__assumes_ctx_lock(sem)
{
	if (IS_ENABLED(CONFIG_LOCKDEP))
		lockdep_assert_held(sem);
	else
		rwsem_assert_held_nolockdep(sem);
}

static inline void rwsem_assert_held_write(const struct rw_semaphore *sem)
	__assumes_ctx_lock(sem)
{
	if (IS_ENABLED(CONFIG_LOCKDEP))
		lockdep_assert_held_write(sem);
	else
		rwsem_assert_held_write_nolockdep(sem);
}

#ifdef CONFIG_TRACK_LOCK_HOLDERS
/**
 * rwsem_track_holder - track the holders of an rw_semaphore
 * @sem: initialized rw_semaphore that no one holds yet
 *
 * Count @sem in task_nr_tracked_locks() of the tasks holding it. Use it
 * for locks whose holders can stall unrelated work. Call it before anyone
 * can take @sem, e.g. right after init_rwsem(). Do not use
 * down_read_non_owner() or up_read_non_owner() on a tracked rwsem: they
 * can run in different tasks, which a per-task count cannot follow.
 */
void rwsem_track_holder(struct rw_semaphore *sem);
#else
static inline void rwsem_track_holder(struct rw_semaphore *sem) { }
#endif

/*
 * lock for reading
 */
extern void down_read(struct rw_semaphore *sem) __acquires_shared(sem);
extern int __must_check down_read_interruptible(struct rw_semaphore *sem) __cond_acquires_shared(0, sem);
extern int __must_check down_read_killable(struct rw_semaphore *sem) __cond_acquires_shared(0, sem);

/*
 * trylock for reading -- returns 1 if successful, 0 if contention
 */
extern int down_read_trylock(struct rw_semaphore *sem) __cond_acquires_shared(true, sem);

/*
 * lock for writing
 */
extern void down_write(struct rw_semaphore *sem) __acquires(sem);
extern int __must_check down_write_killable(struct rw_semaphore *sem) __cond_acquires(0, sem);

/*
 * trylock for writing -- returns 1 if successful, 0 if contention
 */
extern int down_write_trylock(struct rw_semaphore *sem) __cond_acquires(true, sem);

/*
 * release a read lock
 */
extern void up_read(struct rw_semaphore *sem) __releases_shared(sem);

/*
 * release a write lock
 */
extern void up_write(struct rw_semaphore *sem) __releases(sem);

DEFINE_LOCK_GUARD_1(rwsem_read, struct rw_semaphore, down_read(_T->lock), up_read(_T->lock))
DEFINE_LOCK_GUARD_1_COND(rwsem_read, _try, down_read_trylock(_T->lock))
DEFINE_LOCK_GUARD_1_COND(rwsem_read, _intr, down_read_interruptible(_T->lock), _RET == 0)

DECLARE_LOCK_GUARD_1_ATTRS(rwsem_read, __acquires_shared(_T), __releases_shared(*(struct rw_semaphore **)_T))
#define class_rwsem_read_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(rwsem_read, _T)
DECLARE_LOCK_GUARD_1_ATTRS(rwsem_read_try, __acquires_shared(_T), __releases_shared(*(struct rw_semaphore **)_T))
#define class_rwsem_read_try_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(rwsem_read_try, _T)
DECLARE_LOCK_GUARD_1_ATTRS(rwsem_read_intr, __acquires_shared(_T), __releases_shared(*(struct rw_semaphore **)_T))
#define class_rwsem_read_intr_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(rwsem_read_intr, _T)

DEFINE_LOCK_GUARD_1(rwsem_write, struct rw_semaphore, down_write(_T->lock), up_write(_T->lock))
DEFINE_LOCK_GUARD_1_COND(rwsem_write, _try, down_write_trylock(_T->lock))
DEFINE_LOCK_GUARD_1_COND(rwsem_write, _kill, down_write_killable(_T->lock), _RET == 0)

DECLARE_LOCK_GUARD_1_ATTRS(rwsem_write, __acquires(_T), __releases(*(struct rw_semaphore **)_T))
#define class_rwsem_write_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(rwsem_write, _T)
DECLARE_LOCK_GUARD_1_ATTRS(rwsem_write_try, __acquires(_T), __releases(*(struct rw_semaphore **)_T))
#define class_rwsem_write_try_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(rwsem_write_try, _T)
DECLARE_LOCK_GUARD_1_ATTRS(rwsem_write_kill, __acquires(_T), __releases(*(struct rw_semaphore **)_T))
#define class_rwsem_write_kill_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(rwsem_write_kill, _T)

DEFINE_LOCK_GUARD_1(rwsem_init, struct rw_semaphore, init_rwsem(_T->lock), /* */)
DECLARE_LOCK_GUARD_1_ATTRS(rwsem_init, __acquires(_T), __releases(*(struct rw_semaphore **)_T))
#define class_rwsem_init_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(rwsem_init, _T)

/*
 * downgrade write lock to read lock
 */
extern void downgrade_write(struct rw_semaphore *sem) __releases(sem) __acquires_shared(sem);

#ifdef CONFIG_DEBUG_LOCK_ALLOC
/*
 * nested locking. NOTE: rwsems are not allowed to recurse
 * (which occurs if the same task tries to acquire the same
 * lock instance multiple times), but multiple locks of the
 * same lock class might be taken, if the order of the locks
 * is always the same. This ordering rule can be expressed
 * to lockdep via the _nested() APIs, but enumerating the
 * subclasses that are used. (If the nesting relationship is
 * static then another method for expressing nested locking is
 * the explicit definition of lock class keys and the use of
 * lockdep_set_class() at lock initialization time.
 * See Documentation/locking/lockdep-design.rst for more details.)
 */
extern void down_read_nested(struct rw_semaphore *sem, int subclass) __acquires_shared(sem);
extern int __must_check down_read_killable_nested(struct rw_semaphore *sem, int subclass) __cond_acquires_shared(0, sem);
extern void down_write_nested(struct rw_semaphore *sem, int subclass) __acquires(sem);
extern int down_write_killable_nested(struct rw_semaphore *sem, int subclass) __cond_acquires(0, sem);
extern void _down_write_nest_lock(struct rw_semaphore *sem, struct lockdep_map *nest_lock) __acquires(sem);

# define down_write_nest_lock(sem, nest_lock)			\
do {								\
	typecheck(struct lockdep_map *, &(nest_lock)->dep_map);	\
	_down_write_nest_lock(sem, &(nest_lock)->dep_map);	\
} while (0)

/*
 * Take/release a lock when not the owner will release it.
 *
 * [ This API should be avoided as much as possible - the
 *   proper abstraction for this case is completions. ]
 */
extern void down_read_non_owner(struct rw_semaphore *sem) __acquires_shared(sem);
extern void up_read_non_owner(struct rw_semaphore *sem) __releases_shared(sem);
#else
# define down_read_nested(sem, subclass)		down_read(sem)
# define down_read_killable_nested(sem, subclass)	down_read_killable(sem)
# define down_write_nest_lock(sem, nest_lock)	down_write(sem)
# define down_write_nested(sem, subclass)	down_write(sem)
# define down_write_killable_nested(sem, subclass)	down_write_killable(sem)
# define down_read_non_owner(sem)		down_read(sem)
# define up_read_non_owner(sem)			up_read(sem)
#endif

#endif /* _LINUX_RWSEM_H */
