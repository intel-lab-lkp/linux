#ifndef _DRM_SCHED_TESTS_H_
#define _DRM_SCHED_TESTS_H_

#include <kunit/test.h>
#include <linux/atomic.h>
#include <linux/dma-fence.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/types.h>

#include <drm/gpu_scheduler.h>

struct drm_mock_scheduler {
	struct drm_gpu_scheduler base;

	struct kunit		*test;

	spinlock_t		lock;
	struct list_head	job_list; /* Protected by the lock */

	struct {
		u64		context;
		atomic_t	next_seqno;
		unsigned int	cur_seqno; /* Protected by the lock */
	} hw_timeline;
};

struct drm_mock_sched_entity {
	struct drm_sched_entity base;

	struct kunit		*test;
};

struct drm_mock_sched_job {
	struct drm_sched_job	base;

#define DRM_MOCK_SCHED_JOB_TIMEDOUT 0x1
	unsigned long		flags;

	struct list_head	link;
	struct hrtimer		timer;

	unsigned int		duration_us;
	ktime_t			finish_at;

	spinlock_t		lock;
	struct dma_fence	hw_fence;

	struct kunit		*test;
};

static inline struct drm_mock_scheduler *
drm_sched_to_mock_sched(struct drm_gpu_scheduler *sched)
{
	return container_of(sched, struct drm_mock_scheduler, base);
};

static inline struct drm_mock_sched_entity *
drm_sched_entity_to_mock_entity(struct drm_sched_entity *sched_entity)
{
	return container_of(sched_entity, struct drm_mock_sched_entity, base);
};

static inline struct drm_mock_sched_job *
drm_sched_job_to_mock_job(struct drm_sched_job *sched_job)
{
	return container_of(sched_job, struct drm_mock_sched_job, base);
};

struct drm_mock_scheduler *drm_mock_new_scheduler(struct kunit *test,
						  long timeout);
void drm_mock_scheduler_fini(struct drm_mock_scheduler *sched);
unsigned int drm_mock_sched_advance(struct drm_mock_scheduler *sched,
				    unsigned int num);

struct drm_mock_sched_entity *
drm_mock_new_sched_entity(struct kunit *test,
			  enum drm_sched_priority priority,
			  struct drm_mock_scheduler *sched);
void drm_mock_sched_entity_free(struct drm_mock_sched_entity *entity);

struct drm_mock_sched_job *
drm_mock_new_sched_job(struct kunit *test,
		       struct drm_mock_sched_entity *entity);

static inline void drm_mock_sched_job_submit(struct drm_mock_sched_job *job)
{
	drm_sched_job_arm(&job->base);
	drm_sched_entity_push_job(&job->base);
}

static inline void
drm_mock_sched_job_set_duration_us(struct drm_mock_sched_job *job,
				   unsigned int duration_us)
{
	job->duration_us = duration_us;
}

static inline bool
drm_mock_sched_job_is_finished(struct drm_mock_sched_job *job)
{
	return dma_fence_is_signaled(&job->base.s_fence->finished);
}

static inline bool
drm_mock_sched_job_wait_finished(struct drm_mock_sched_job *job, long timeout)
{
	long ret;

	ret = dma_fence_wait_timeout(&job->base.s_fence->finished,
				      false,
				      timeout);

	return ret != 0;
}

static inline long
drm_mock_sched_job_wait_scheduled(struct drm_mock_sched_job *job, long timeout)
{
	long ret;

	ret = dma_fence_wait_timeout(&job->base.s_fence->scheduled,
				      false,
				      timeout);

	return ret != 0;
}

#endif
