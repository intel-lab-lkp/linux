// SPDX-License-Identifier: GPL-2.0-only
/*
 * Media jobs framework
 *
 * Copyright 2025 Ideas on Board Oy
 *
 * Author: Daniel Scally <dan.scally@ideasonboard.com>
 */

#include <linux/cleanup.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <media/media-device.h>
#include <media/media-entity.h>
#include <media/media-jobs.h>

int media_jobs_add_job_step(struct media_job *job, void (*run_step)(void *data),
			    void *data, unsigned int flags, unsigned int pos)
{
	struct media_job_step *step, *tmp;
	unsigned int num = flags;
	unsigned int count = 0;

	guard(spinlock)(&job->lock);

	if (!flags) {
		WARN_ONCE(1, "%s(): No flag bits set\n", __func__);
		return -EINVAL;
	}

	/* Count the number of set flags; they're mutually exclusive. */
	while (num) {
		num &= (num - 1);
		count++;
	}

	if (count > 1) {
		WARN_ONCE(1, "%s(): Multiple flag bits set\n", __func__);
		return -EINVAL;
	}

	step = kzalloc(sizeof(*step), GFP_KERNEL);
	if (!step)
		return -ENOMEM;

	step->run_step = run_step;
	step->data = data;
	step->flags = flags;
	step->pos = pos;

	/*
	 * We need to decide where to place the step. If the list is empty that
	 * is really easy (and also the later code is much easier if the code is
	 * guaranteed not to be empty...)
	 */
	if (list_empty(&job->steps)) {
		list_add_tail(&step->list, &job->steps);
		return 0;
	}

	/*
	 * If we've been asked to place it at a specific position from the end
	 * of the list, we cycle back through it until either we exhaust the
	 * list or find an entry that needs to go further from the back than the
	 * new one.
	 */
	if ((flags & MEDIA_JOBS_FL_STEP_FROM_BACK)) {
		list_for_each_entry_reverse(tmp, &job->steps, list) {
			if (tmp->flags == flags && tmp->pos == pos)
				return -EINVAL;

			if (tmp->flags != MEDIA_JOBS_FL_STEP_FROM_BACK ||
			    tmp->pos > pos)
				break;
		}

		/*
		 * If the entry we broke on is also one placed from the back and
		 * should be closer to the back than the new one, we place the
		 * new one in front of it...otherwise place the new one behind
		 * it.
		 */
		if (tmp->flags == flags && tmp->pos < pos)
			list_add_tail(&step->list, &tmp->list);
		else
			list_add(&step->list, &tmp->list);

		return 0;
	}

	/*
	 * If we've been asked to place it a specific position from the front of
	 * the list we do the same kind of operation, but going from the front
	 * instead.
	 */
	if (flags & MEDIA_JOBS_FL_STEP_FROM_FRONT) {
		list_for_each_entry(tmp, &job->steps, list) {
			if (tmp->flags == flags && tmp->pos == pos)
				return -EINVAL;

			if (tmp->flags != MEDIA_JOBS_FL_STEP_FROM_FRONT ||
			    tmp->pos > pos)
				break;
		}

		/*
		 * If the entry we broke on is also placed from the front and
		 * should be closed to the front than the new one, we place the
		 * new one behind it, otherwise in front of it.
		 */
		if (tmp->flags == flags && tmp->pos < pos)
			list_add(&step->list, &tmp->list);
		else
			list_add_tail(&step->list, &tmp->list);

		return 0;
	}

	/*
	 * If the step is flagged as "can go anywhere" we just need to try to
	 * find the first "from the back" entry and add it immediately before
	 * that. If we can't find one, add it after whatever we did find.
	 */
	if (flags & MEDIA_JOBS_FL_STEP_ANYWHERE) {
		list_for_each_entry(tmp, &job->steps, list)
			if ((tmp->flags & MEDIA_JOBS_FL_STEP_FROM_BACK))
				break;

		if ((tmp->flags & MEDIA_JOBS_FL_STEP_FROM_BACK) ||
		    list_entry_is_head(tmp, &job->steps, list))
			list_add_tail(&step->list, &tmp->list);
		else
			list_add(&step->list, &tmp->list);

		return 0;
	}

	/* Shouldn't get here, unless the flag value is wrong. */
	WARN_ONCE(1, "%s(): Invalid flag value\n", __func__);
	return -EINVAL;
}
EXPORT_SYMBOL_GPL(media_jobs_add_job_step);

int media_jobs_add_job_dep(struct media_job *job, struct media_job_dep_ops *ops,
			   void *data)
{
	struct media_job_dep *dep;

	if (!ops || !ops->check_dep || !data)
		return -EINVAL;

	guard(spinlock)(&job->lock);

	/* Confirm the same dependency hasn't already been added */
	list_for_each_entry(dep, &job->deps, list)
		if (dep->ops == ops && dep->data == data)
			return -EINVAL;

	dep = kzalloc(sizeof(*dep), GFP_KERNEL);
	if (!dep)
		return -ENOMEM;

	dep->ops = ops;
	dep->data = data;
	list_add(&dep->list, &job->deps);

	return 0;
}
EXPORT_SYMBOL_GPL(media_jobs_add_job_dep);

static bool media_jobs_check_pending_job(struct media_job *job,
					 enum media_job_types type,
					 struct media_job_dep_ops *dep_ops,
					 void *data)
{
	struct media_job_dep *dep;

	guard(spinlock)(&job->lock);

	if (job->type != type)
		return false;

	list_for_each_entry(dep, &job->deps, list) {
		if (dep->ops == dep_ops && dep->data == data) {
			if (dep->met)
				return false;

			break;
		}
	}

	dep->met = true;
	return true;
}

static struct media_job *media_jobs_get_job(struct media_job_scheduler *sched,
					    enum media_job_types type,
					    struct media_job_dep_ops *dep_ops,
					    void *dep_data)
{
	struct media_job_setup_func *jsf;
	struct media_job *job;
	int ret;

	list_for_each_entry(job, &sched->pending, list)
		if (media_jobs_check_pending_job(job, type, dep_ops, dep_data))
			return job;

	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job)
		return ERR_PTR(-ENOMEM);

	spin_lock_init(&job->lock);
	INIT_LIST_HEAD(&job->deps);
	INIT_LIST_HEAD(&job->steps);
	job->type = type;
	job->sched = sched;

	list_for_each_entry(jsf, &sched->setup_funcs, list) {
		if (jsf->type != type)
			continue;

		ret = jsf->job_setup(job, jsf->data);
		if (ret) {
			kfree(job);
			return ERR_PTR(ret);
		}
	}

	list_add_tail(&job->list, &sched->pending);

	/* This marks the dependency as met */
	media_jobs_check_pending_job(job, type, dep_ops, dep_data);

	return job;
}

static void media_jobs_free_job(struct media_job *job, bool reset)
{
	struct media_job_step *step, *stmp;
	struct media_job_dep *dep, *dtmp;

	scoped_guard(spinlock, &job->lock) {
		list_for_each_entry_safe(dep, dtmp, &job->deps, list) {
			if (reset && dep->ops->reset_dep)
				dep->ops->reset_dep(dep->data);

			list_del(&dep->list);
			kfree(dep);
		}

		list_for_each_entry_safe(step, stmp, &job->steps, list) {
			list_del(&step->list);
			kfree(step);
		}
	}

	list_del(&job->list);
	kfree(job);
}

int media_jobs_try_queue_job(struct media_job_scheduler *sched,
			     enum media_job_types type,
			     struct media_job_dep_ops *dep_ops, void *dep_data)
{
	struct media_job_dep *dep;
	struct media_job *job;

	if (!sched)
		return 0;

	guard(spinlock)(&sched->lock);

	job = media_jobs_get_job(sched, type, dep_ops, dep_data);
	if (IS_ERR(job))
		return PTR_ERR(job);

	list_for_each_entry(dep, &job->deps, list)
		if (!dep->ops->check_dep(dep->data))
			return 0; /* Not a failure */

	list_for_each_entry(dep, &job->deps, list)
		if (dep->ops->clear_dep)
			dep->ops->clear_dep(dep->data);

	list_move_tail(&job->list, &sched->queue);
	queue_work(sched->async_wq, &sched->work);

	return 0;
}
EXPORT_SYMBOL_GPL(media_jobs_try_queue_job);

static void __media_jobs_run_jobs(struct work_struct *work)
{
	struct media_job_scheduler *sched = container_of(work,
							 struct media_job_scheduler,
							 work);
	struct media_job_step *step;
	struct media_job *job;

	while (true) {
		scoped_guard(spinlock, &sched->lock) {
			if (list_empty(&sched->queue))
				return;

			job = list_first_entry(&sched->queue, struct media_job,
					       list);
		}

		list_for_each_entry(step, &job->steps, list)
			step->run_step(step->data);

		media_jobs_free_job(job, false);
	}
}

void media_jobs_run_jobs(struct media_job_scheduler *sched)
{
	if (!sched)
		return;

	queue_work(sched->async_wq, &sched->work);
}
EXPORT_SYMBOL_GPL(media_jobs_run_jobs);

static void __media_jobs_cancel_jobs(struct media_job_scheduler *sched)
{
	struct media_job *job, *jtmp;

	list_for_each_entry_safe(job, jtmp, &sched->pending, list)
		media_jobs_free_job(job, true);

	list_for_each_entry_safe(job, jtmp, &sched->queue, list)
		media_jobs_free_job(job, true);
}

void media_jobs_cancel_jobs(struct media_job_scheduler *sched)
{
	if (!sched)
		return;

	guard(spinlock)(&sched->lock);
	__media_jobs_cancel_jobs(sched);
}
EXPORT_SYMBOL_GPL(media_jobs_cancel_jobs);

int media_jobs_add_job_setup_func(struct media_job_scheduler *sched,
				  int (*job_setup)(struct media_job *job, void *data),
				  void *data, enum media_job_types type)
{
	struct media_job_setup_func *new_setup_func;

	guard(spinlock)(&sched->lock);

	new_setup_func = kzalloc(sizeof(*new_setup_func), GFP_KERNEL);
	if (!new_setup_func)
		return -ENOMEM;

	new_setup_func->type = type;
	new_setup_func->job_setup = job_setup;
	new_setup_func->data = data;
	list_add_tail(&new_setup_func->list, &sched->setup_funcs);

	return 0;
}
EXPORT_SYMBOL_GPL(media_jobs_add_job_setup_func);

static void __media_jobs_put_scheduler(struct kref *kref)
{
	struct media_job_scheduler *sched =
		container_of(kref, struct media_job_scheduler, kref);
	struct media_job_setup_func *func, *ftmp;

	cancel_work_sync(&sched->work);
	destroy_workqueue(sched->async_wq);

	scoped_guard(spinlock, &sched->lock) {
		__media_jobs_cancel_jobs(sched);

		list_for_each_entry_safe(func, ftmp, &sched->setup_funcs, list) {
			list_del(&func->list);
			kfree(func);
		}
	}

	list_del(&sched->list);
	kfree(sched);
}

void media_jobs_put_scheduler(struct media_job_scheduler *sched)
{
	kref_put(&sched->kref, __media_jobs_put_scheduler);
}
EXPORT_SYMBOL_GPL(media_jobs_put_scheduler);

struct media_job_scheduler *media_jobs_get_scheduler(struct media_device *mdev)
{
	struct media_job_scheduler *sched;
	char workqueue_name[32];
	int ret;

	guard(mutex)(&media_job_schedulers_lock);

	list_for_each_entry(sched, &media_job_schedulers, list) {
		if (sched->mdev == mdev) {
			kref_get(&sched->kref);
			return sched;
		}
	}

	ret = snprintf(workqueue_name, sizeof(workqueue_name),
		       "mc jobs (%s)", mdev->driver_name);
	if (!ret)
		return ERR_PTR(-EINVAL);

	sched = kzalloc(sizeof(*sched), GFP_KERNEL);
	if (!sched)
		return ERR_PTR(-ENOMEM);

	sched->async_wq = alloc_workqueue(workqueue_name, 0, 0);
	if (!sched->async_wq) {
		kfree(sched);
		return ERR_PTR(-EINVAL);
	}

	sched->mdev = mdev;
	kref_init(&sched->kref);
	spin_lock_init(&sched->lock);
	INIT_LIST_HEAD(&sched->setup_funcs);
	INIT_LIST_HEAD(&sched->pending);
	INIT_LIST_HEAD(&sched->queue);
	INIT_WORK(&sched->work, __media_jobs_run_jobs);

	list_add_tail(&sched->list, &media_job_schedulers);

	return sched;
}
EXPORT_SYMBOL_GPL(media_jobs_get_scheduler);

LIST_HEAD(media_job_schedulers);

/* Synchronise access to the global schedulers list */
DEFINE_MUTEX(media_job_schedulers_lock);
