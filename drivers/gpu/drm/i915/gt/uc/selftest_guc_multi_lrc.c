// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#include <linux/sort.h>

#include "gt/intel_gt_print.h"
#include "selftests/igt_spinner.h"
#include "selftests/igt_reset.h"
#include "selftests/intel_scheduler_helpers.h"
#include "gt/intel_engine_heartbeat.h"
#include "gem/selftests/mock_context.h"

static int cmp_logical_instance(const void *a, const void *b)
{
	const struct intel_engine_cs *ea = *(const struct intel_engine_cs **)a;
	const struct intel_engine_cs *eb = *(const struct intel_engine_cs **)b;

	if (ea->logical_mask < eb->logical_mask)
		return -1;
	if (ea->logical_mask > eb->logical_mask)
		return 1;
	return 0;
}

static void logical_sort(struct intel_engine_cs **engines, int num_engines)
{
	sort(engines, num_engines, sizeof(*engines), cmp_logical_instance, NULL);
}

static struct intel_context *
multi_lrc_create_parent(struct intel_gt *gt, u8 class,
			unsigned long flags)
{
	struct intel_engine_cs *siblings[MAX_ENGINE_INSTANCE + 1];
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	int i = 0;

	for_each_engine(engine, gt, id) {
		if (engine->class != class)
			continue;

		siblings[i++] = engine;
	}

	if (i <= 1)
		return ERR_PTR(0);

	logical_sort(siblings, i);

	return intel_engine_create_parallel(siblings, 1, i);
}

static void multi_lrc_context_unpin(struct intel_context *ce)
{
	struct intel_context *child;

	GEM_BUG_ON(!intel_context_is_parent(ce));

	for_each_child(ce, child)
		intel_context_unpin(child);
	intel_context_unpin(ce);
}

static void multi_lrc_context_put(struct intel_context *ce)
{
	GEM_BUG_ON(!intel_context_is_parent(ce));

	/*
	 * Only the parent gets the creation ref put in the uAPI, the parent
	 * itself is responsible for creation ref put on the children.
	 */
	intel_context_put(ce);
}

static struct i915_request *
multi_lrc_nop_request(struct intel_context *ce)
{
	struct intel_context *child;
	struct i915_request *rq, *child_rq;
	int i = 0;

	GEM_BUG_ON(!intel_context_is_parent(ce));

	rq = intel_context_create_request(ce);
	if (IS_ERR(rq))
		return rq;

	i915_request_get(rq);
	i915_request_add(rq);

	for_each_child(ce, child) {
		child_rq = intel_context_create_request(child);
		if (IS_ERR(child_rq))
			goto child_error;

		if (++i == ce->parallel.number_children)
			set_bit(I915_FENCE_FLAG_SUBMIT_PARALLEL,
				&child_rq->fence.flags);
		i915_request_add(child_rq);
	}

	return rq;

child_error:
	i915_request_put(rq);

	return ERR_PTR(-ENOMEM);
}

static int __intel_guc_multi_lrc_basic(struct intel_gt *gt, unsigned int class)
{
	struct intel_context *parent;
	struct i915_request *rq;
	int ret;

	parent = multi_lrc_create_parent(gt, class, 0);
	if (IS_ERR(parent)) {
		gt_err(gt, "Failed creating contexts: %pe\n", parent);
		return PTR_ERR(parent);
	} else if (!parent) {
		gt_dbg(gt, "Not enough engines in class: %d\n", class);
		return 0;
	}

	rq = multi_lrc_nop_request(parent);
	if (IS_ERR(rq)) {
		ret = PTR_ERR(rq);
		gt_err(gt, "Failed creating requests: %pe\n", rq);
		goto out;
	}

	ret = intel_selftest_wait_for_rq(rq);
	if (ret)
		gt_err(gt, "Failed waiting on request: %pe\n", ERR_PTR(ret));

	i915_request_put(rq);

	if (ret >= 0) {
		ret = intel_gt_wait_for_idle(gt, HZ * 5);
		if (ret < 0)
			gt_err(gt, "GT failed to idle: %pe\n", ERR_PTR(ret));
	}

out:
	multi_lrc_context_unpin(parent);
	multi_lrc_context_put(parent);
	return ret;
}

static int intel_guc_multi_lrc_basic(void *arg)
{
	struct intel_gt *gt = arg;
	unsigned int class;
	int ret;

	for (class = 0; class < MAX_ENGINE_CLASS + 1; ++class) {
		/* We don't support breadcrumb handshake on these classes */
		if (class == COMPUTE_CLASS || class == RENDER_CLASS)
			continue;

		ret = __intel_guc_multi_lrc_basic(gt, class);
		if (ret)
			return ret;
	}

	return 0;
}

int intel_guc_multi_lrc_live_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(intel_guc_multi_lrc_basic),
	};
	struct intel_gt *gt = to_gt(i915);

	if (intel_gt_is_wedged(gt))
		return 0;

	if (!intel_uc_uses_guc_submission(&gt->uc))
		return 0;

	return intel_gt_live_subtests(tests, gt);
}
