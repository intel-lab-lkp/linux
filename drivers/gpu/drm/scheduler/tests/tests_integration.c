// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026 Jonghyuk Kim(MalHyuk) */

#include <linux/dma-fence.h>
#include <linux/rcupdate.h>

#include "sched_tests.h"

/*
 * Integration tests exercising the drm_sched interaction with the wider
 * dma-fence infrastructure, e.g. fences exported to userspace outliving the
 * objects they were created from.
 */

/*
 * Reproduce the drm_sched_fence get_timeline_name() lifetime bug.
 *
 * drm_sched_fence_get_timeline_name() reads the scheduler name, and the
 * drm_sched_fence ops keep .release set, so the fence is NOT ops-detached on
 * signal (dma_fence_signal_timestamp_locked() only clears ->ops for fences
 * without .release/.wait). A driver that frees a per-context drm_gpu_scheduler
 * while userspace still holds the exported ->finished fence (via sync_file /
 * drm_syncobj) leaves the scheduler dangling; querying the timeline name then
 * touches freed slab memory. KASAN reports a slab-use-after-free read in
 * drm_sched_fence_get_timeline_name(). Confirmed instances: amdxdna, nouveau,
 * msm; same class as CVE-2025-38703 (xe) and CVE-2025-71302 (panthor).
 */
static void drm_sched_dma_fence_timeline_name_uaf(struct kunit *test)
{
	struct drm_mock_sched_entity *entity;
	struct drm_mock_scheduler *sched;
	struct drm_mock_sched_job *job;
	struct dma_fence *finished;
	const char *name;
	bool done;

	sched = drm_mock_sched_new(test, MAX_SCHEDULE_TIMEOUT);
	entity = drm_mock_sched_entity_new(test, DRM_SCHED_PRIORITY_NORMAL,
					   sched);
	job = drm_mock_sched_job_new(test, entity);

	/* Arm + submit; the s_fence is only created by drm_sched_job_arm(). */
	drm_mock_sched_job_submit(job);

	/* Independent reference on the finished fence, as a sync_file would. */
	finished = dma_fence_get(&job->base.s_fence->finished);

	done = drm_mock_sched_job_wait_scheduled(job, HZ);
	KUNIT_ASSERT_TRUE(test, done);
	drm_mock_sched_advance(sched, 1);
	done = drm_mock_sched_job_wait_finished(job, HZ);
	KUNIT_ASSERT_TRUE(test, done);

	/* Free the per-context scheduler while the finished fence is held. */
	drm_mock_sched_entity_free(entity);
	drm_mock_sched_fini(sched);
	kunit_kfree(test, sched);

	/*
	 * Query the timeline name through the public dma-fence API, as a
	 * userspace SYNC_IOC_FILE_INFO consumer would. Before the fix this is a
	 * use-after-free read of the freed scheduler; after it the cached name
	 * is returned and the test passes.
	 */
	rcu_read_lock();
	name = (const char *)dma_fence_timeline_name(finished);
	rcu_read_unlock();
	kunit_info(test, "get_timeline_name() on stale fence returned %p\n", name);

	dma_fence_put(finished);
}

static struct kunit_case drm_sched_dma_fence_uaf_tests[] = {
	KUNIT_CASE(drm_sched_dma_fence_timeline_name_uaf),
	{}
};

static struct kunit_suite drm_sched_dma_fence_uaf = {
	.name = "drm-sched-dma-fence-uaf",
	.test_cases = drm_sched_dma_fence_uaf_tests,
};

kunit_test_suite(drm_sched_dma_fence_uaf);
