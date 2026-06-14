// SPDX-License-Identifier: GPL-2.0+
/*
 * KUnit coverage for the V3D CPU-job copy-query OOB write. Drives the real
 * v3d_copy_query_results() (COPY_TIMESTAMP_QUERY) over a shmem-backed v3d_bo:
 * a copy->offset at the BO size makes the availability write land past the
 * one-page vmap, which KASAN reports. A NULL fence skips the result read, so
 * no V3D engine is needed (Level 2). Two in-bounds controls pass on both.
 * Included from v3d_sched.c.
 */
#include <linux/sizes.h>

#include <kunit/test.h>

#include <drm/drm_drv.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_kunit_helpers.h>
#include <drm/drm_syncobj.h>

/* One page per BO; the OOB write lands just past this vmap mapping. */
#define V3D_COPY_TEST_BO_SIZE	PAGE_SIZE

struct v3d_copy_test_ctx {
	struct drm_device *drm;
	struct v3d_bo *dst;			/* to_v3d_bo(job->base.bo[0]) */
	struct v3d_bo *ts;			/* to_v3d_bo(job->base.bo[1]) */
	struct drm_gem_object *bo_array[2];
	struct drm_syncobj syncobj[4];		/* fence == NULL -> unavailable */
	struct v3d_timestamp_query queries[4];
	struct v3d_cpu_job job;
};

/* Standard shmem teardown; we have no v3d_dev for v3d_free_object(). */
static void v3d_copy_test_free_bo(void *p)
{
	struct v3d_bo *bo = p;

	if (bo->vaddr)
		v3d_put_bo_vaddr(bo);
	if (refcount_read(&bo->base.pages_pin_count))
		drm_gem_shmem_unpin(&bo->base);
	drm_gem_shmem_free(&bo->base);
}

static struct v3d_bo *
v3d_copy_test_make_bo(struct kunit *test, struct drm_device *drm)
{
	struct drm_gem_shmem_object *shmem;
	struct drm_gem_object *obj;
	struct v3d_bo *bo;
	int ret;

	/* v3d_create_object sizes the alloc for struct v3d_bo (bo->vaddr). */
	shmem = drm_gem_shmem_create(drm, V3D_COPY_TEST_BO_SIZE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, shmem);

	obj = &shmem->base;
	bo = to_v3d_bo(obj);

	/* Real page backing for v3d_get_bo_vaddr()'s vmap(). */
	ret = drm_gem_shmem_pin(shmem);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = kunit_add_action_or_reset(test, v3d_copy_test_free_bo, bo);
	KUNIT_ASSERT_EQ(test, ret, 0);

	return bo;
}

/* Build a COPY_TIMESTAMP_QUERY job and run the real copy handler. */
static void v3d_copy_test_run(struct kunit *test, u32 offset, u32 stride,
			      u32 count)
{
	struct v3d_copy_test_ctx *c;
	struct drm_device *drm;
	struct device *dev;

	dev = drm_kunit_helper_alloc_device(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	drm = __drm_kunit_helper_alloc_drm_device(test, dev,
						  sizeof(*drm), 0,
						  DRIVER_GEM);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, drm);

	/* Route BO creation through the driver hook (devm drm_driver is writable). */
	((struct drm_driver *)drm->driver)->gem_create_object = v3d_create_object;

	c = kunit_kzalloc(test, sizeof(*c), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, c);
	c->drm = drm;

	c->dst = v3d_copy_test_make_bo(test, drm);
	c->ts = v3d_copy_test_make_bo(test, drm);

	c->bo_array[0] = &c->dst->base.base;
	c->bo_array[1] = &c->ts->base.base;

	/* fence == NULL: result read skipped; only the availability bit is written. */
	KUNIT_ASSERT_LE(test, count, (u32)ARRAY_SIZE(c->queries));
	for (u32 i = 0; i < count; i++) {
		c->syncobj[i].fence = NULL;
		c->queries[i].offset = 0;	/* in bounds for the source BO */
		c->queries[i].syncobj = &c->syncobj[i];
	}

	c->job.job_type = V3D_CPU_JOB_TYPE_COPY_TIMESTAMP_QUERY;
	c->job.base.bo = c->bo_array;
	c->job.base.bo_count = 2;
	c->job.timestamp_query.queries = c->queries;
	c->job.timestamp_query.count = count;

	/* drm_device is the first member of v3d_dev; the cast resolves &v3d->drm back. */
	c->job.base.v3d = (struct v3d_dev *)c->drm;

	c->job.copy.do_64bit = false;		/* 4-byte writes */
	c->job.copy.do_partial = false;
	c->job.copy.availability_bit = true;	/* drives the index-1 write */
	c->job.copy.offset = offset;
	c->job.copy.stride = stride;

#if defined(V3D_CPU_JOB_COPY_BOUNDS_CHECKED)
	/* Patched tree: run the same submit-time gate; out-of-range is rejected before the copy. */
	if (v3d_cpu_job_check_copy_bounds(&c->job)) {
		KUNIT_EXPECT_GT_MSG(test, c->job.copy.offset + count * stride,
				    (u32)V3D_COPY_TEST_BO_SIZE,
				    "bounds gate rejected an in-bounds geometry");
		return;
	}
#endif

	v3d_copy_query_results(&c->job);
}

/* Control: one query at offset 0; availability bit at byte 4, in bounds. */
static void v3d_copy_query_control_inbounds(struct kunit *test)
{
	v3d_copy_test_run(test, 0, 8, 1);
}

/* Control: two queries, stride 8; second availability bit at byte 12, in bounds. */
static void v3d_copy_query_control_inbounds_multi(struct kunit *test)
{
	v3d_copy_test_run(test, 0, 8, 2);
}

/* Trigger: copy->offset == BO size; write past the vmap mapping (stock OOB, patched rejected). */
static void v3d_copy_query_trigger_oob(struct kunit *test)
{
	v3d_copy_test_run(test, V3D_COPY_TEST_BO_SIZE, 8, 1);
}

static struct kunit_case v3d_copy_query_cases[] = {
	KUNIT_CASE(v3d_copy_query_control_inbounds),
	KUNIT_CASE(v3d_copy_query_control_inbounds_multi),
	KUNIT_CASE(v3d_copy_query_trigger_oob),
	{}
};

static struct kunit_suite v3d_copy_query_suite = {
	.name = "v3d_copy_query",
	.test_cases = v3d_copy_query_cases,
};

kunit_test_suite(v3d_copy_query_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
