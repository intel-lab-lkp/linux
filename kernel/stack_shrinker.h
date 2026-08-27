/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_STACK_SHRINKER_H
#define _LINUX_STACK_SHRINKER_H

#include <linux/cleanup.h>
#include <linux/list.h>
#include <linux/llist.h>
#include <linux/sched.h>
#include <linux/seq_file.h>

#ifdef CONFIG_RECLAIMABLE_STACK

void add_to_stack_shrinker(struct task_struct *tsk, int node);
void remove_from_stack_shrinker(struct task_struct *tsk);

bool __ensure_stack_is_present(struct task_struct *tsk, bool *need_deferred_repopulate);
void __prepare_stack_for_reclaim(struct task_struct *tsk);
void __allow_stack_reclaim(struct task_struct *tsk);
void wake_stack_repopulate(void);

static inline bool ensure_stack_is_present(struct task_struct *tsk, bool *need_deferred_repopulate)
{
	if (unlikely(tsk->flags & PF_RECLAIMABLE_STACK))
		return __ensure_stack_is_present(tsk, need_deferred_repopulate);
	return true;
}

static inline void prepare_stack_for_reclaim(struct task_struct *tsk)
{
	if (unlikely(tsk->flags & PF_RECLAIMABLE_STACK))
		__prepare_stack_for_reclaim(tsk);
}

static inline void allow_stack_reclaim(struct task_struct *tsk)
{
	if (unlikely(tsk->flags & PF_RECLAIMABLE_STACK))
		__allow_stack_reclaim(tsk);
}

#else /* !CONFIG_RECLAIMABLE_STACK */

static inline void add_to_stack_shrinker(struct task_struct *tsk, int node) {}
static inline void remove_from_stack_shrinker(struct task_struct *tsk) {}

static inline bool ensure_stack_is_present(struct task_struct *tsk, bool *need_deferred_repopulate)
{
	return true;
}

static inline void prepare_stack_for_reclaim(struct task_struct *tsk) {}

static inline void allow_stack_reclaim(struct task_struct *tsk) {}

static inline void wake_stack_repopulate(void) {}

#endif /* CONFIG_RECLAIMABLE_STACK */

#endif /* _LINUX_STACK_SHRINKER_H */
