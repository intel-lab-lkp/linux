// SPDX-License-Identifier: GPL-2.0

#include <linux/workqueue.h>

__rust_helper void rust_helper_init_work_with_key(struct work_struct *work,
						  work_func_t func,
						  bool onstack,
						  const char *name,
						  struct lock_class_key *key)
{
	__init_work(work, onstack);
	work->data = (atomic_long_t)WORK_DATA_INIT();
	lockdep_init_map(&work->lockdep_map, name, key, 0);
	INIT_LIST_HEAD(&work->entry);
	work->func = func;
}

__rust_helper bool rust_helper_work_pending(struct work_struct *work)
{
	return work_pending(work);
}

__rust_helper bool rust_helper_cancel_work_sync(struct work_struct *work)
{
	return cancel_work_sync(work);
}

__rust_helper bool rust_helper_cancel_delayed_work_sync(struct delayed_work *dwork)
{
	return cancel_delayed_work_sync(dwork);
}

__rust_helper void rust_helper_init_delayed_work(struct delayed_work *dwork,
						 work_func_t func,
						 const char *name,
						 struct lock_class_key *key,
						 const char *tname,
						 struct lock_class_key *tkey)
{
	rust_helper_init_work_with_key(&dwork->work, func, false, name, key);
	timer_init_key(&dwork->timer, delayed_work_timer_fn, TIMER_IRQSAFE, tname, tkey);
}

