/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_WAIT_TYPES_H
#define _LINUX_WAIT_TYPES_H
/*
 * Linux wait queue related types and methods
 */
#include <linux/list.h>
#include <linux/spinlock_types.h>

typedef struct wait_queue_entry wait_queue_entry_t;

typedef int (*wait_queue_func_t)(struct wait_queue_entry *wq_entry, unsigned mode, int flags, void *key);
int default_wake_function(struct wait_queue_entry *wq_entry, unsigned mode, int flags, void *key);

/*
 * A single wait-queue entry structure:
 */
struct wait_queue_entry {
	unsigned int		flags;
	void			*private;
	wait_queue_func_t	func;
	struct list_head	entry;
};

struct wait_queue_head {
	spinlock_t		lock;
	struct list_head	head;
};
typedef struct wait_queue_head wait_queue_head_t;

#endif /* _LINUX_WAIT_TYPES_H */
