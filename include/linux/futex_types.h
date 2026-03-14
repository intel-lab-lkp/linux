/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_FUTEX_TYPES_H
#define _LINUX_FUTEX_TYPES_H

#ifdef CONFIG_FUTEX
#include <linux/mutex_types.h>
#include <linux/types.h>

struct compat_robust_list_head;
struct futex_pi_state;
struct robust_list_head;

/**
 * struct futex_sched_data - Futex related per task data
 * @robust_list:	User space registered robust list pointer
 * @compat_robust_list:	User space registered robust list pointer for compat tasks
 * @pi_state_list:	List head for Priority Inheritance (PI) state management
 * @pi_state_cache:	Pointer to cache one PI state object per task
 * @exit_mutex:		Mutex for serializing exit
 * @state:		Futex handling state to handle exit races correctly
 */
struct futex_sched_data {
	struct robust_list_head __user		*robust_list;
#ifdef CONFIG_COMPAT
	struct compat_robust_list_head __user	*compat_robust_list;
#endif
	struct list_head			pi_state_list;
	struct futex_pi_state			*pi_state_cache;
	struct mutex				exit_mutex;
	unsigned int				state;
};

/**
 * struct futex_mm_data - Futex related per MM data
 * @phash_lock:			Mutex to protect the private hash operations
 * @phash:			RCU managed pointer to the private hash
 * @phash_new:			Pointer to a newly allocated private hash
 * @phash_batches:		Batch state for RCU synchronization
 * @phash_rcu:			RCU head for call_rcu()
 * @phash_atomic:		Aggregate value for @phash_ref
 * @phash_ref:			Per CPU reference counter for a private hash
 */
struct futex_mm_data {
#ifdef CONFIG_FUTEX_PRIVATE_HASH
	struct mutex			phash_lock;
	struct futex_private_hash	__rcu *phash;
	struct futex_private_hash	*phash_new;
	unsigned long			phash_batches;
	struct rcu_head			phash_rcu;
	atomic_long_t			phash_atomic;
	unsigned int			__percpu *phash_ref;
#endif
};

#else
struct futex_sched_data { };
struct futex_mm_data { };
#endif /* !CONFIG_FUTEX */

#endif /* _LINUX_FUTEX_TYPES_H */
