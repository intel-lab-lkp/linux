// SPDX-License-Identifier: GPL-2.0

#include <linux/dma-fence.h>
#include <linux/rcupdate.h>

#include "sched_tests.h"

/*
 * Integration-style regression tests that exercise the interaction between the
 * DRM scheduler and the dma-fence API, rather than scheduler behaviour in
 * isolation.
 */

/*
 * Reproduce the drm_sched_fence timeline-name use-after-free.
 *
 * drm_sched_fence_get_timeline_name() used to dereference fence->sched->name.
 * A driver may free a per-context/per-queue/per-VM drm_gpu_scheduler while
 * userspace still holds the exported ->finished fence (via sync_file /
 * drm_syncobj). Querying the timeline name afterwards must not touch the freed
 * scheduler.
 *
 * Without the fix this reads fence->sched->name from freed slab memory and
 * KASAN reports a slab-use-after-free in drm_sched_fence_get_timeline_name();
 * with the fix the name is cached at init and the freed scheduler is never
 * dereferenced. Same class as CVE-2025-38703 (drm/xe) and CVE-2025-71302
 * (drm/panthor).
 */
static void drm_sched_dma_fence_uaf(struct kunit *test)
{
	struct drm_mock_sched_entity *entity;
	struct drm_mock_scheduler *sched;
	struct drm_mock_sched_job *job;
	struct dma_fence *finished;
	const char __rcu *name;
	bool done;

	sched = drm_mock_sched_new(test, MAX_SCHEDULE_TIMEOUT);
	entity = drm_mock_sched_entity_new(test, DRM_SCHED_PRIORITY_NORMAL,
					   sched);
	job = drm_mock_sched_job_new(test, entity);

	/* The s_fence is only created by drm_sched_job_arm(). */
	drm_mock_sched_job_submit(job);

	/* Independent reference on the finished fence == userspace sync_file. */
	finished = dma_fence_get(&job->base.s_fence->finished);

	/* Let the job get scheduled (hw fence created), then signal + finish. */
	done = drm_mock_sched_job_wait_scheduled(job, HZ);
	KUNIT_ASSERT_TRUE(test, done);
	drm_mock_sched_advance(sched, 1);
	done = drm_mock_sched_job_wait_finished(job, HZ);
	KUNIT_ASSERT_TRUE(test, done);

	/*
	 * Free the per-context scheduler while the finished fence is held.
	 * kunit_kfree() releases the backing memory immediately (rather than at
	 * test teardown) so that fence->sched becomes a dangling pointer now.
	 */
	drm_mock_sched_entity_free(entity);
	drm_mock_sched_fini(sched);
	kunit_kfree(test, sched);

	/*
	 * Query the timeline name of the now-stale fence. With the fix the name
	 * was cached at init, so the freed scheduler is not dereferenced;
	 * without it this is a use-after-free read of the freed scheduler.
	 */
	rcu_read_lock();
	name = dma_fence_timeline_name(finished);
	KUNIT_EXPECT_NOT_NULL(test, name);
	rcu_read_unlock();

	dma_fence_put(finished);
}

static struct kunit_case drm_sched_dma_fence_tests[] = {
	KUNIT_CASE(drm_sched_dma_fence_uaf),
	{}
};

static struct kunit_suite drm_sched_dma_fence = {
	.name = "drm_sched_dma_fence_uaf_tests",
	.test_cases = drm_sched_dma_fence_tests,
};

kunit_test_suite(drm_sched_dma_fence);
