/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_UNWIND_USER_DEFERRED_H
#define _LINUX_UNWIND_USER_DEFERRED_H

#include <linux/task_work.h>
#include <linux/unwind_user.h>
#include <linux/unwind_deferred_types.h>

struct unwind_work;

typedef void (*unwind_callback_t)(struct unwind_work *work, struct unwind_stacktrace *trace, u64 timestamp);

struct unwind_work {
	struct list_head		list;
	unwind_callback_t		func;
	int				bit;
};

#ifdef CONFIG_UNWIND_USER

#define UNWIND_PENDING_BIT	(BITS_PER_LONG - 1)
#define UNWIND_PENDING		(1UL << UNWIND_PENDING_BIT)

enum {
	UNWIND_ALREADY_PENDING	= 1,
	UNWIND_ALREADY_EXECUTED	= 2,
};

void unwind_task_init(struct task_struct *task);
void unwind_task_free(struct task_struct *task);

int unwind_deferred_trace(struct unwind_stacktrace *trace);

int unwind_deferred_init(struct unwind_work *work, unwind_callback_t func);
int unwind_deferred_request(struct unwind_work *work, u64 *timestamp);
void unwind_deferred_cancel(struct unwind_work *work);

static __always_inline void unwind_exit_to_user_mode(void)
{
	unsigned long bits;

	/* Was there any unwinding? */
	if (likely(!current->unwind_mask))
		return;

	bits = current->unwind_mask;
	do {
		/* Is a task_work going to run again before going back */
		if (bits & UNWIND_PENDING)
			return;
	} while (!try_cmpxchg(&current->unwind_mask, &bits, 0UL));

	if (likely(current->unwind_info.cache))
		current->unwind_info.cache->nr_entries = 0;
	current->unwind_info.timestamp = 0;
}

#else /* !CONFIG_UNWIND_USER */

static inline void unwind_task_init(struct task_struct *task) {}
static inline void unwind_task_free(struct task_struct *task) {}

static inline int unwind_deferred_trace(struct unwind_stacktrace *trace) { return -ENOSYS; }
static inline int unwind_deferred_init(struct unwind_work *work, unwind_callback_t func) { return -ENOSYS; }
static inline int unwind_deferred_request(struct unwind_work *work, u64 *timestamp) { return -ENOSYS; }
static inline void unwind_deferred_cancel(struct unwind_work *work) {}

static inline void unwind_exit_to_user_mode(void) {}

#endif /* !CONFIG_UNWIND_USER */

#endif /* _LINUX_UNWIND_USER_DEFERRED_H */
