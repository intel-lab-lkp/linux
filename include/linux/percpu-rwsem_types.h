/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PERCPU_RWSEM_TYPES_H
#define _LINUX_PERCPU_RWSEM_TYPES_H

#include <linux/rcu_sync.h>
#include <linux/rcuwait.h>
#include <linux/types.h>
#include <linux/wait_types.h>
#ifdef CONFIG_DEBUG_LOCK_ALLOC
#include <linux/lockdep_types.h>
#endif

struct percpu_rw_semaphore {
	struct rcu_sync		rss;
	unsigned int __percpu	*read_count;
	struct rcuwait		writer;
	wait_queue_head_t	waiters;
	atomic_t		block;
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	dep_map;
#endif
};

#endif
