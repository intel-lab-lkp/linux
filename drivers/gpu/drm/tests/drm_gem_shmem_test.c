// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit test suite for GEM objects backed by shmem buffers
 *
 * Copyright (C) 2023 Red Hat, Inc.
 *
 * Author: Marco Pagani <marpagan@redhat.com>
 */

#include <linux/dma-buf.h>
#include <linux/iosys-map.h>
#include <linux/sizes.h>

#include <kunit/test.h>

#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_kunit_helpers.h>

#define TEST_SIZE		SZ_1M
#define TEST_BYTE		0xae

struct fake_dev {
	struct drm_device drm_dev;
	struct device *dev;
};

/* Test creating a shmem GEM object */
static void drm_gem_shmem_test_obj_create(struct kunit *test)
{
	struct fake_dev *fdev = test->priv;
	struct drm_gem_shmem_object *shmem;

	shmem = drm_gem_shmem_create(&fdev->drm_dev, TEST_SIZE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, shmem);
	KUNIT_ASSERT_EQ(test, shmem->base.size, TEST_SIZE);
	KUNIT_ASSERT_NOT_NULL(test, shmem->base.filp);
	KUNIT_ASSERT_NOT_NULL(test, shmem->base.funcs);

	drm_gem_shmem_free(shmem);
}

/* Test creating a private shmem GEM object from a scatter/gather table */
static void drm_gem_shmem_test_obj_create_private(struct kunit *test)
{
	struct fake_dev *fdev = test->priv;
	struct drm_gem_shmem_object *shmem;
	struct drm_gem_object *gem_obj;
	struct dma_buf buf_mock;
	struct dma_buf_attachment attach_mock;
	struct sg_table *sgt;
	char *buf;
	int ret;

	/* Create a mock scatter/gather table */
	buf = kunit_kzalloc(test, TEST_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, sgt);

	ret = sg_alloc_table(sgt, 1, GFP_KERNEL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	sg_init_one(sgt->sgl, buf, TEST_SIZE);

	/* Init a mock DMA-BUF */
	buf_mock.size = TEST_SIZE;
	attach_mock.dmabuf = &buf_mock;

	gem_obj = drm_gem_shmem_prime_import_sg_table(&fdev->drm_dev, &attach_mock, sgt);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, gem_obj);
	KUNIT_ASSERT_EQ(test, gem_obj->size, TEST_SIZE);
	KUNIT_ASSERT_NULL(test, gem_obj->filp);
	KUNIT_ASSERT_NOT_NULL(test, gem_obj->funcs);

	shmem = to_drm_gem_shmem_obj(gem_obj);
	KUNIT_ASSERT_PTR_EQ(test, shmem->sgt, sgt);

	/* The scatter/gather table is freed by drm_gem_shmem_free */
	drm_gem_shmem_free(shmem);
}

/* Test pinning backing pages */
static void drm_gem_shmem_test_pin_pages(struct kunit *test)
{
	struct fake_dev *fdev = test->priv;
	struct drm_gem_shmem_object *shmem;
	int i, ret;

	shmem = drm_gem_shmem_create(&fdev->drm_dev, TEST_SIZE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, shmem);
	KUNIT_ASSERT_NULL(test, shmem->pages);
	KUNIT_ASSERT_EQ(test, shmem->pages_use_count, 0);

	ret = drm_gem_shmem_pin(shmem);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_NOT_NULL(test, shmem->pages);
	KUNIT_ASSERT_EQ(test, shmem->pages_use_count, 1);

	for (i = 0; i < (shmem->base.size >> PAGE_SHIFT); i++)
		KUNIT_ASSERT_NOT_NULL(test, shmem->pages[i]);

	drm_gem_shmem_unpin(shmem);
	KUNIT_ASSERT_NULL(test, shmem->pages);
	KUNIT_ASSERT_EQ(test, shmem->pages_use_count, 0);

	drm_gem_shmem_free(shmem);
}

/* Test creating a virtual mapping */
static void drm_gem_shmem_test_vmap(struct kunit *test)
{
	struct fake_dev *fdev = test->priv;
	struct drm_gem_shmem_object *shmem;
	struct iosys_map map;
	int ret, i;

	shmem = drm_gem_shmem_create(&fdev->drm_dev, TEST_SIZE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, shmem);
	KUNIT_ASSERT_NULL(test, shmem->vaddr);
	KUNIT_ASSERT_EQ(test, shmem->vmap_use_count, 0);

	ret = drm_gem_shmem_vmap(shmem, &map);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_NOT_NULL(test, shmem->vaddr);
	KUNIT_ASSERT_EQ(test, shmem->vmap_use_count, 1);
	KUNIT_ASSERT_FALSE(test, iosys_map_is_null(&map));

	iosys_map_memset(&map, 0, TEST_BYTE, TEST_SIZE);
	for (i = 0; i < TEST_SIZE; i++)
		KUNIT_ASSERT_EQ(test, iosys_map_rd(&map, i, u8), TEST_BYTE);

	drm_gem_shmem_vunmap(shmem, &map);
	KUNIT_ASSERT_NULL(test, shmem->vaddr);
	KUNIT_ASSERT_EQ(test, shmem->vmap_use_count, 0);

	drm_gem_shmem_free(shmem);
}

/* Test exporting a scatter/gather table */
static void drm_gem_shmem_test_get_pages_sgt(struct kunit *test)
{
	struct fake_dev *fdev = test->priv;
	struct drm_gem_shmem_object *shmem;
	struct sg_table *sgt;
	struct scatterlist *sg;
	unsigned int si, len = 0;
	int ret;

	shmem = drm_gem_shmem_create(&fdev->drm_dev, TEST_SIZE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, shmem);

	ret = drm_gem_shmem_pin(shmem);
	KUNIT_ASSERT_EQ(test, ret, 0);

	sgt = drm_gem_shmem_get_sg_table(shmem);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, sgt);
	KUNIT_ASSERT_NULL(test, shmem->sgt);

	for_each_sgtable_sg(sgt, sg, si) {
		KUNIT_ASSERT_NOT_NULL(test, sg);
		len += sg->length;
	}
	KUNIT_ASSERT_GE(test, len, TEST_SIZE);

	kfree(sgt);
	drm_gem_shmem_free(shmem);
}

/* Test exporting a scatter/gather pinned table for PRIME */
static void drm_gem_shmem_test_get_sg_table(struct kunit *test)
{
	struct fake_dev *fdev = test->priv;
	struct drm_gem_shmem_object *shmem;
	struct sg_table *sgt;
	struct scatterlist *sg;
	unsigned int si, len = 0;

	shmem = drm_gem_shmem_create(&fdev->drm_dev, TEST_SIZE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, shmem);

	sgt = drm_gem_shmem_get_pages_sgt(shmem);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, sgt);
	KUNIT_ASSERT_PTR_EQ(test, sgt, shmem->sgt);

	for_each_sgtable_sg(sgt, sg, si) {
		KUNIT_ASSERT_NOT_NULL(test, sg);
		len += sg->length;
	}
	KUNIT_ASSERT_GE(test, len, TEST_SIZE);

	/* The scatter/gather table is freed by drm_gem_shmem_free */
	drm_gem_shmem_free(shmem);
}

/* Test updating madvise status */
static void drm_gem_shmem_test_madvise(struct kunit *test)
{
	struct fake_dev *fdev = test->priv;
	struct drm_gem_shmem_object *shmem;
	int ret;

	shmem = drm_gem_shmem_create(&fdev->drm_dev, TEST_SIZE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, shmem);
	KUNIT_ASSERT_EQ(test, shmem->madv, 0);

	ret = drm_gem_shmem_madvise(shmem, 1);
	KUNIT_ASSERT_TRUE(test, ret);
	KUNIT_ASSERT_EQ(test, shmem->madv, 1);

	ret = drm_gem_shmem_madvise(shmem, -1);
	KUNIT_ASSERT_FALSE(test, ret);
	KUNIT_ASSERT_EQ(test, shmem->madv, -1);

	ret = drm_gem_shmem_madvise(shmem, 0);
	KUNIT_ASSERT_FALSE(test, ret);
	KUNIT_ASSERT_EQ(test, shmem->madv, -1);

	drm_gem_shmem_free(shmem);
}

/* Test purging */
static void drm_gem_shmem_test_purge(struct kunit *test)
{
	struct fake_dev *fdev = test->priv;
	struct drm_gem_shmem_object *shmem;
	struct sg_table *sgt;
	int ret;

	shmem = drm_gem_shmem_create(&fdev->drm_dev, TEST_SIZE);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, shmem);

	ret = drm_gem_shmem_is_purgeable(shmem);
	KUNIT_ASSERT_FALSE(test, ret);

	ret = drm_gem_shmem_madvise(shmem, 1);
	KUNIT_ASSERT_TRUE(test, ret);

	sgt = drm_gem_shmem_get_pages_sgt(shmem);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, sgt);

	ret = drm_gem_shmem_is_purgeable(shmem);
	KUNIT_ASSERT_TRUE(test, ret);

	drm_gem_shmem_purge(shmem);
	KUNIT_ASSERT_TRUE(test, ret);

	drm_gem_shmem_free(shmem);
}

static int drm_gem_shmem_test_init(struct kunit *test)
{
	struct fake_dev *fdev;
	struct device *dev;

	/* Allocate a parent device */
	dev = drm_kunit_helper_alloc_device(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	/*
	 * The DRM core will automatically initialize the GEM core and create
	 * a DRM Memory Manager object which provides an address space pool
	 * for GEM objects allocation.
	 */
	fdev = drm_kunit_helper_alloc_drm_device(test, dev, struct fake_dev,
						 drm_dev, DRIVER_GEM);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, fdev);

	fdev->dev = dev;
	test->priv = fdev;

	return 0;
}

static void drm_gem_shmem_test_exit(struct kunit *test)
{
	struct fake_dev *fdev = test->priv;

	drm_kunit_helper_free_device(test, fdev->dev);
}

static struct kunit_case drm_gem_shmem_test_cases[] = {
	KUNIT_CASE(drm_gem_shmem_test_obj_create),
	KUNIT_CASE(drm_gem_shmem_test_obj_create_private),
	KUNIT_CASE(drm_gem_shmem_test_pin_pages),
	KUNIT_CASE(drm_gem_shmem_test_vmap),
	KUNIT_CASE(drm_gem_shmem_test_get_pages_sgt),
	KUNIT_CASE(drm_gem_shmem_test_get_sg_table),
	KUNIT_CASE(drm_gem_shmem_test_madvise),
	KUNIT_CASE(drm_gem_shmem_test_purge),
	{}
};

static struct kunit_suite drm_gem_shmem_suite = {
	.name = "drm_gem_shmem",
	.init = drm_gem_shmem_test_init,
	.exit = drm_gem_shmem_test_exit,
	.test_cases = drm_gem_shmem_test_cases
};

kunit_test_suite(drm_gem_shmem_suite);
