
#include "drm_sched_tests.h"

struct drm_mock_sched_entity *
drm_mock_new_sched_entity(struct kunit *test,
			  enum drm_sched_priority priority,
			  struct drm_mock_scheduler *sched)
{
	struct drm_mock_sched_entity *entity;
	struct drm_gpu_scheduler *drm_sched;
	int ret;

	entity = kunit_kzalloc(test, sizeof(*entity), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, entity);

	drm_sched = &sched->base;
	ret = drm_sched_entity_init(&entity->base,
				    priority,
				    &drm_sched, 1,
				    NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);

	entity->test = test;

	return entity;
}

void drm_mock_sched_entity_free(struct drm_mock_sched_entity *entity)
{
	drm_sched_entity_destroy(&entity->base);
}

static enum hrtimer_restart
drm_mock_sched_job_signal_timer(struct hrtimer *hrtimer)
{
	struct drm_mock_sched_job *upto =
		container_of(hrtimer, typeof(*upto), timer);
	struct drm_mock_scheduler *sched =
		drm_sched_to_mock_sched(upto->base.sched);
	struct drm_mock_sched_job *job, *next;
	ktime_t now = ktime_get();
	unsigned long flags;
	LIST_HEAD(signal);

	spin_lock_irqsave(&sched->lock, flags);
	list_for_each_entry_safe(job, next, &sched->job_list, link) {
		if (!job->duration_us)
			break;

		if (ktime_before(now, job->finish_at))
			break;

		list_move_tail(&job->link, &signal);
		sched->hw_timeline.cur_seqno = job->hw_fence.seqno;
	}
	spin_unlock_irqrestore(&sched->lock, flags);

	list_for_each_entry(job, &signal, link) {
		dma_fence_signal(&job->hw_fence);
		dma_fence_put(&job->hw_fence);
	}

	return HRTIMER_NORESTART;
}

struct drm_mock_sched_job *
drm_mock_new_sched_job(struct kunit *test,
		       struct drm_mock_sched_entity *entity)
{
	struct drm_mock_sched_job *job;
	int ret;

	job = kunit_kzalloc(test, sizeof(*job), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, job);

	ret = drm_sched_job_init(&job->base,
				 &entity->base,
				 1,
				 NULL);
	KUNIT_ASSERT_EQ(test, ret, 0);

	job->test = test;

	spin_lock_init(&job->lock);
	INIT_LIST_HEAD(&job->link);
	hrtimer_init(&job->timer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
	job->timer.function = drm_mock_sched_job_signal_timer;

	return job;
}

static const char *drm_mock_sched_hw_fence_driver_name(struct dma_fence *fence)
{
	return "drm_mock_sched";
}

static const char *
drm_mock_sched_hw_fence_timeline_name(struct dma_fence *fence)
{
	struct drm_mock_sched_job *job =
		container_of(fence, typeof(*job), hw_fence);

	return (const char *)job->base.sched->name;
}
static void drm_mock_sched_hw_fence_release(struct dma_fence *fence)
{
	struct drm_mock_sched_job *job =
		container_of(fence, typeof(*job), hw_fence);

	hrtimer_cancel(&job->timer);

	/* Freed by the kunit framework */
}

static const struct dma_fence_ops drm_mock_sched_hw_fence_ops = {
	.get_driver_name = drm_mock_sched_hw_fence_driver_name,
	.get_timeline_name = drm_mock_sched_hw_fence_timeline_name,
	.release = drm_mock_sched_hw_fence_release,
};

static struct dma_fence *mock_sched_run_job(struct drm_sched_job *sched_job)
{
	struct drm_mock_scheduler *sched =
		drm_sched_to_mock_sched(sched_job->sched);
	struct drm_mock_sched_job *job = drm_sched_job_to_mock_job(sched_job);

	dma_fence_init(&job->hw_fence,
		       &drm_mock_sched_hw_fence_ops,
		       &job->lock,
		       sched->hw_timeline.context,
		       atomic_inc_return(&sched->hw_timeline.next_seqno));

	dma_fence_get(&job->hw_fence); /* Reference for the job_list */

	spin_lock_irq(&sched->lock);
	if (job->duration_us) {
		ktime_t prev_finish_at = 0;

		if (!list_empty(&sched->job_list)) {
			struct drm_mock_sched_job *prev =
				list_last_entry(&sched->job_list, typeof(*prev),
						link);

			prev_finish_at = prev->finish_at;
		}

		if (!prev_finish_at)
			prev_finish_at = ktime_get();

		job->finish_at = ktime_add_us(prev_finish_at, job->duration_us);
	}
	list_add_tail(&job->link, &sched->job_list);
	if (job->finish_at)
		hrtimer_start(&job->timer, job->finish_at, HRTIMER_MODE_ABS);
	spin_unlock_irq(&sched->lock);

	return &job->hw_fence;
}

static enum drm_gpu_sched_stat
mock_sched_timedout_job(struct drm_sched_job *sched_job)
{
	struct drm_mock_sched_job *job = drm_sched_job_to_mock_job(sched_job);

	job->flags |= DRM_MOCK_SCHED_JOB_TIMEDOUT;

	return DRM_GPU_SCHED_STAT_NOMINAL;
}

static void mock_sched_free_job(struct drm_sched_job *sched_job)
{
	drm_sched_job_cleanup(sched_job);
}

static const struct drm_sched_backend_ops drm_mock_scheduler_ops = {
	.run_job = mock_sched_run_job,
	.timedout_job = mock_sched_timedout_job,
	.free_job = mock_sched_free_job
};

struct drm_mock_scheduler *
drm_mock_new_scheduler(struct kunit *test,
		       long timeout)
{
	struct drm_mock_scheduler *sched;
	int ret;

	sched = kunit_kzalloc(test, sizeof(*sched), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, sched);

	ret = drm_sched_init(&sched->base,
			     &drm_mock_scheduler_ops,
			     NULL, /* wq */
			     DRM_SCHED_PRIORITY_COUNT,
			     U32_MAX, /* max credits */
			     UINT_MAX, /* hang limit */
			     timeout,
			     NULL, /* timeout wq */
			     NULL, /* score */
			     "drm-mock-scheduler",
			     NULL /* dev */);
	KUNIT_ASSERT_EQ(test, ret, 0);

	sched->test = test;
	sched->hw_timeline.context = dma_fence_context_alloc(1);
	atomic_set(&sched->hw_timeline.next_seqno, 0);
	INIT_LIST_HEAD(&sched->job_list);
	spin_lock_init(&sched->lock);

	return sched;
}

void drm_mock_scheduler_fini(struct drm_mock_scheduler *sched)
{
	struct drm_mock_sched_job *job, *next;
	unsigned long flags;
	LIST_HEAD(signal);

	spin_lock_irqsave(&sched->lock, flags);
	list_for_each_entry_safe(job, next, &sched->job_list, link)
		list_move_tail(&job->link, &signal);
	spin_unlock_irqrestore(&sched->lock, flags);

	list_for_each_entry(job, &signal, link) {
		hrtimer_cancel(&job->timer);
		dma_fence_put(&job->hw_fence);
	}

	drm_sched_fini(&sched->base);
}

unsigned int drm_mock_sched_advance(struct drm_mock_scheduler *sched,
				    unsigned int num)
{
	struct drm_mock_sched_job *job, *next;
	unsigned int found = 0;
	unsigned long flags;
	LIST_HEAD(signal);

	spin_lock_irqsave(&sched->lock, flags);
	if (WARN_ON_ONCE(sched->hw_timeline.cur_seqno + num <
			 sched->hw_timeline.cur_seqno))
		goto unlock;
	sched->hw_timeline.cur_seqno += num;
	list_for_each_entry_safe(job, next, &sched->job_list, link) {
		if (sched->hw_timeline.cur_seqno < job->hw_fence.seqno)
			break;

		list_move_tail(&job->link, &signal);
		found++;
	}
unlock:
	spin_unlock_irqrestore(&sched->lock, flags);

	list_for_each_entry(job, &signal, link) {
		dma_fence_signal(&job->hw_fence);
		dma_fence_put(&job->hw_fence);
	}

	return found;
}
