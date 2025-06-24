// SPDX-License-Identifier: GPL-2.0-only
/*
 * Media jobs framework
 *
 * Copyright 2025 Ideas on Board Oy
 *
 * Author: Daniel Scally <dan.scally@ideasonboard.com>
 */

#include <linux/cleanup.h>
#include <linux/slab.h>

#include <media/media-device.h>
#include <media/media-entity.h>
#include <media/media-jobs.h>

/* Synchronise access to the global schedulers list */
static DEFINE_MUTEX(media_job_schedulers_lock);
static LIST_HEAD(media_job_schedulers);

static struct media_job_contributor *
__media_jobs_next_contributor(struct media_job_scheduler *sched,
			      struct media_job_contributor *contributor,
			      enum media_job_types type)
{
	contributor = contributor ? list_next_entry(contributor, list)
		    : list_first_entry(&sched->contributors, typeof(*contributor), list);

	list_for_each_entry_from(contributor, &sched->contributors, list)
		if (contributor->type == type)
			return contributor;

	return NULL;
}

/**
 * for_each_media_job_contributor() - Iterate through an scheduler's job
 *				      contributors, filtering on type
 *
 * @sched:		Pointer to the &media_job_scheduler
 * @contributor:	Pointer to a &media_job_contributor to hold the values
 * @type:		Value from &media_job_types to filter on
 */
#define for_each_media_job_contributor(sched, contributor, type)		\
	for (contributor = __media_jobs_next_contributor(sched, NULL, type);	\
	     contributor;							\
	     contributor = __media_jobs_next_contributor(sched, contributor, type))


int media_jobs_add_job_step(struct media_job *job, void (*run_step)(void *data),
			    void *data, u32 flags, unsigned int pos)
{
	struct media_job_step *step, *tmp;
	int ret;

	if (!flags) {
		WARN_ONCE(1, "%s(): No flag bits set\n", __func__);
		return -EINVAL;
	}

	/* Check the number of set flags; they're mutually exclusive. */
	if (hweight32(flags) > 1) {
		WARN_ONCE(1, "%s(): Multiple flag bits set\n", __func__);
		return -EINVAL;
	}

	if (!run_step) {
		WARN_ONCE(1, "%s(): No run_step function passed\n", __func__);
		return -EINVAL;
	}

	guard(spinlock)(&job->lock);

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
	if (flags & MEDIA_JOBS_FL_STEP_FROM_BACK) {
		list_for_each_entry_reverse(tmp, &job->steps, list) {
			if (tmp->flags == flags && tmp->pos == pos) {
				ret = -EINVAL;
				goto err_free_step;
			}

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
			if (tmp->flags == flags && tmp->pos == pos) {
				ret = -EINVAL;
				goto err_free_step;
			}

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
	ret = -EINVAL;

err_free_step:
	kfree(step);
	return ret;
}
EXPORT_SYMBOL_GPL(media_jobs_add_job_step);

static void media_jobs_free_job(struct media_job *job)
{
	struct media_job_step *step, *stmp;

	scoped_guard(spinlock, &job->lock) {
		list_for_each_entry_safe(step, stmp, &job->steps, list) {
			list_del(&step->list);
			kfree(step);
		}
	}

	kfree(job);
}

int media_jobs_try_queue_job(struct media_job_scheduler *sched,
			     enum media_job_types type)
{
	struct media_job_contributor *contributor;
	struct media_job *job;
	int ret;

	if (!sched)
		return 0;

	guard(spinlock)(&sched->lock);

	for_each_media_job_contributor(sched, contributor, type) {
		if (contributor->ops->ready &&
		    !contributor->ops->ready(contributor->data))
			return 0;
	}

	for_each_media_job_contributor(sched, contributor, type)
		if (contributor->ops->queue)
			contributor->ops->queue(contributor->data);

	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job) {
		ret = -ENOMEM;
		goto err_abort_contributors;
	}

	spin_lock_init(&job->lock);
	INIT_LIST_HEAD(&job->steps);
	job->type = type;
	job->sched = sched;

	for_each_media_job_contributor(sched, contributor, type) {
		if (contributor->ops->add_steps) {
			ret = contributor->ops->add_steps(job, contributor->data);
			if (ret)
				goto err_free_job;
		}
	}

	list_add_tail(&job->list, &sched->queue);

	if (sched->running)
		queue_work(sched->async_wq, &sched->work);

	return 0;

err_free_job:
	media_jobs_free_job(job);
err_abort_contributors:
	for_each_media_job_contributor(sched, contributor, type)
		if (contributor->ops->abort)
			contributor->ops->abort(contributor->data);

	return ret;
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

			list_del(&job->list);
		}

		list_for_each_entry(step, &job->steps, list) {
			/*
			 * Theoretically impossible as this should have been
			 * validated in media_jobs_add_job_step()
			 */
			if (!step->run_step)
				WARN_ONCE(1, "%s(): No .run_step() operation\n",
					  __func__);

			step->run_step(step->data);
		}

		media_jobs_free_job(job);
	}
}

void media_jobs_run_jobs(struct media_job_scheduler *sched)
{
	if (!sched)
		return;

	guard(spinlock)(&sched->lock);

	sched->running = true;
	queue_work(sched->async_wq, &sched->work);
}
EXPORT_SYMBOL_GPL(media_jobs_run_jobs);

static void __media_jobs_cancel_jobs(struct media_job_scheduler *sched)
{
	struct media_job_contributor *contributor;
	struct media_job *job, *jtmp;

	lockdep_assert_held(&sched->lock);
	cancel_work_sync(&sched->work);

	list_for_each_entry_safe(job, jtmp, &sched->queue, list) {
		list_del(&job->list);

		list_for_each_entry(contributor, &sched->contributors, list)
			if (contributor->ops->abort)
				contributor->ops->abort(contributor->data);

		media_jobs_free_job(job);
	}
}

void media_jobs_cancel_jobs(struct media_job_scheduler *sched)
{
	if (!sched)
		return;

	guard(spinlock)(&sched->lock);
	sched->running = false;
	__media_jobs_cancel_jobs(sched);
}
EXPORT_SYMBOL_GPL(media_jobs_cancel_jobs);

int media_jobs_register_job_contributor(struct media_job_scheduler *sched,
					struct media_job_contributor_ops *ops,
					void *data, enum media_job_types type)
{
	struct media_job_contributor *contributor;

	if (!ops || !data)
		return -EINVAL;

	contributor = kzalloc(sizeof(*contributor), GFP_KERNEL);
	if (!contributor)
		return -ENOMEM;

	contributor->type = type;
	contributor->ops = ops;
	contributor->data = data;

	guard(spinlock)(&sched->lock);
	list_add_tail(&contributor->list, &sched->contributors);

	return 0;
}
EXPORT_SYMBOL_GPL(media_jobs_register_job_contributor);

static void __media_jobs_put_scheduler(struct kref *kref)
{
	struct media_job_scheduler *sched =
		container_of(kref, struct media_job_scheduler, kref);
	struct media_job_contributor *contributor, *tmp;

	cancel_work_sync(&sched->work);
	destroy_workqueue(sched->async_wq);

	scoped_guard(spinlock, &sched->lock) {
		__media_jobs_cancel_jobs(sched);

		list_for_each_entry_safe(contributor, tmp, &sched->contributors,
					 list) {
			list_del(&contributor->list);
			kfree(contributor);
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
	INIT_LIST_HEAD(&sched->contributors);
	INIT_LIST_HEAD(&sched->queue);
	INIT_WORK(&sched->work, __media_jobs_run_jobs);

	list_add_tail(&sched->list, &media_job_schedulers);

	return sched;
}
EXPORT_SYMBOL_GPL(media_jobs_get_scheduler);
