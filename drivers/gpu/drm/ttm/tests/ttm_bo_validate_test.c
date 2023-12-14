// SPDX-License-Identifier: GPL-2.0 AND MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include <drm/ttm/ttm_resource.h>
#include <drm/ttm/ttm_placement.h>
#include <drm/ttm/ttm_tt.h>

#include "ttm_kunit_helpers.h"

#define BO_SIZE		SZ_4K

struct ttm_bo_validate_test_case {
	const char *description;
	enum ttm_bo_type bo_type;
	bool with_ttm;
};

static struct ttm_placement *ttm_placement_kunit_init(struct kunit *test,
						      struct ttm_place *places,
						      unsigned int num_places,
						      struct ttm_place *busy_places,
						      unsigned int num_busy_places)
{
	struct ttm_placement *placement;

	placement = kunit_kzalloc(test, sizeof(*placement), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, placement);

	placement->num_placement = num_places;
	placement->placement = places;
	placement->num_busy_placement = num_busy_places;
	placement->busy_placement = busy_places;

	return placement;
}

static void ttm_bo_validate_case_desc(const struct ttm_bo_validate_test_case *t,
				      char *desc)
{
	strscpy(desc, t->description, KUNIT_PARAM_DESC_SIZE);
}

static const struct ttm_bo_validate_test_case ttm_bo_type_cases[] = {
	{
		.description = "Buffer object for userspace",
		.bo_type = ttm_bo_type_device,
	},
	{
		.description = "Kernel buffer object",
		.bo_type = ttm_bo_type_kernel,
	},
	{
		.description = "Shared buffer object",
		.bo_type = ttm_bo_type_sg,
	},
};

KUNIT_ARRAY_PARAM(ttm_bo_types, ttm_bo_type_cases,
		  ttm_bo_validate_case_desc);

static void ttm_bo_init_reserved_sys_man(struct kunit *test)
{
	const struct ttm_bo_validate_test_case *params = test->param_value;
	struct ttm_test_devices *priv = test->priv;
	struct ttm_buffer_object *bo;
	struct ttm_place *place;
	struct ttm_placement *placement;
	enum ttm_bo_type bo_type = params->bo_type;
	struct ttm_operation_ctx ctx = { };
	uint32_t size = ALIGN(BO_SIZE, PAGE_SIZE);
	int err;

	bo = kunit_kzalloc(test, sizeof(*bo), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, bo);

	place = ttm_place_kunit_init(test, TTM_PL_SYSTEM, 0);
	placement = ttm_placement_kunit_init(test, place, 1, NULL, 0);

	drm_gem_private_object_init(priv->drm, &bo->base, size);

	err = ttm_bo_init_reserved(priv->ttm_dev, bo, bo_type, placement,
				   PAGE_SIZE, &ctx, NULL, NULL,
				   &dummy_ttm_bo_destroy);
	dma_resv_unlock(bo->base.resv);

	KUNIT_EXPECT_EQ(test, err, 0);
	KUNIT_EXPECT_EQ(test, kref_read(&bo->kref), 1);
	KUNIT_EXPECT_PTR_EQ(test, bo->bdev, priv->ttm_dev);
	KUNIT_EXPECT_EQ(test, bo->type, bo_type);
	KUNIT_EXPECT_EQ(test, bo->page_alignment, PAGE_SIZE);
	KUNIT_EXPECT_PTR_EQ(test, bo->destroy, &dummy_ttm_bo_destroy);
	KUNIT_EXPECT_EQ(test, bo->pin_count, 0);
	KUNIT_EXPECT_NULL(test, bo->bulk_move);
	KUNIT_EXPECT_NOT_NULL(test, bo->ttm);
	KUNIT_EXPECT_FALSE(test, ttm_tt_is_populated(bo->ttm));
	KUNIT_EXPECT_NOT_NULL(test, (void *)bo->base.resv->fences);
	KUNIT_EXPECT_EQ(test, ctx.bytes_moved, size);

	if (bo_type != ttm_bo_type_kernel)
		KUNIT_EXPECT_TRUE(test,
				  drm_mm_node_allocated(&bo->base.vma_node.vm_node));

	ttm_resource_free(bo, &bo->resource);
	ttm_bo_put(bo);
}

static void ttm_bo_init_reserved_resv(struct kunit *test)
{
	struct ttm_test_devices *priv = test->priv;
	struct ttm_buffer_object *bo;
	struct ttm_place *place;
	struct ttm_placement *placement;
	struct dma_resv resv;
	enum ttm_bo_type bo_type = ttm_bo_type_device;
	struct ttm_operation_ctx ctx = { };
	uint32_t size = ALIGN(BO_SIZE, PAGE_SIZE);
	int err;

	bo = kunit_kzalloc(test, sizeof(*bo), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, bo);

	place = ttm_place_kunit_init(test, TTM_PL_SYSTEM, 0);
	placement = ttm_placement_kunit_init(test, place, 1, NULL, 0);

	drm_gem_private_object_init(priv->drm, &bo->base, size);
	dma_resv_init(&resv);
	dma_resv_lock(&resv, NULL);

	err = ttm_bo_init_reserved(priv->ttm_dev, bo, bo_type, placement,
				   PAGE_SIZE, &ctx, NULL, &resv,
				   &dummy_ttm_bo_destroy);
	dma_resv_unlock(bo->base.resv);

	KUNIT_EXPECT_EQ(test, err, 0);
	KUNIT_EXPECT_PTR_EQ(test, bo->base.resv, &resv);

	ttm_resource_free(bo, &bo->resource);
	ttm_bo_put(bo);
}

static void ttm_bo_validate_invalid_placement(struct kunit *test)
{
	struct ttm_buffer_object *bo;
	struct ttm_place *place;
	struct ttm_placement *placement;
	enum ttm_bo_type bo_type = ttm_bo_type_device;
	struct ttm_operation_ctx ctx = { };
	uint32_t size = ALIGN(BO_SIZE, PAGE_SIZE);
	uint32_t unknown_mem_type = TTM_PL_PRIV + 1;
	int err;

	place = ttm_place_kunit_init(test, unknown_mem_type, 0);
	placement = ttm_placement_kunit_init(test, place, 1, NULL, 0);

	bo = ttm_bo_kunit_init(test, test->priv, size);
	bo->type = bo_type;

	ttm_bo_reserve(bo, false, false, NULL);
	err = ttm_bo_validate(bo, placement, &ctx);
	dma_resv_unlock(bo->base.resv);

	KUNIT_EXPECT_EQ(test, err, -EINVAL);

	ttm_bo_put(bo);
}

static void ttm_bo_validate_pinned(struct kunit *test)
{
	struct ttm_buffer_object *bo;
	struct ttm_place *place;
	struct ttm_placement *placement;
	uint32_t mem_type = TTM_PL_SYSTEM;
	enum ttm_bo_type bo_type = ttm_bo_type_device;
	struct ttm_operation_ctx ctx = { };
	uint32_t size = ALIGN(BO_SIZE, PAGE_SIZE);
	int err;

	place = ttm_place_kunit_init(test, mem_type, 0);
	placement = ttm_placement_kunit_init(test, place, 1, NULL, 0);

	bo = ttm_bo_kunit_init(test, test->priv, size);
	bo->type = bo_type;

	ttm_bo_reserve(bo, false, false, NULL);
	ttm_bo_pin(bo);
	err = ttm_bo_validate(bo, placement, &ctx);
	dma_resv_unlock(bo->base.resv);

	KUNIT_EXPECT_EQ(test, err, -EINVAL);
}

static struct kunit_case ttm_bo_validate_test_cases[] = {
	KUNIT_CASE_PARAM(ttm_bo_init_reserved_sys_man, ttm_bo_types_gen_params),
	KUNIT_CASE(ttm_bo_init_reserved_resv),
	KUNIT_CASE(ttm_bo_validate_invalid_placement),
	KUNIT_CASE(ttm_bo_validate_pinned),
	{}
};

static struct kunit_suite ttm_bo_validate_test_suite = {
	.name = "ttm_bo_validate",
	.init = ttm_test_devices_all_init,
	.exit = ttm_test_devices_fini,
	.test_cases = ttm_bo_validate_test_cases,
};

kunit_test_suites(&ttm_bo_validate_test_suite);

MODULE_LICENSE("GPL");
