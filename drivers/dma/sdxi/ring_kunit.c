// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDXI descriptor ring management tests.
 *
 * Copyright Advanced Micro Devices, Inc.
 */
#include <kunit/device.h>
#include <kunit/test-bug.h>
#include <kunit/test.h>
#include <linux/container_of.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/packing.h>
#include <linux/string.h>

#include "ring.h"

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

static void valid(struct kunit *t)
{
	__le64 wi, ri;
	struct sdxi_ring_state r;
	struct sdxi_ring_resv resv;
	struct sdxi_desc *descs, *desc;


	descs = kunit_kmalloc_array(t, SZ_1K, sizeof(descs[0]),
				    GFP_KERNEL | __GFP_ZERO);
	KUNIT_ASSERT_NOT_NULL(t, descs);

	ri = wi = 0;
	sdxi_ring_state_init(&r, &ri, &wi, SZ_1K, descs);

	KUNIT_EXPECT_EQ(t, sdxi_ring_try_reserve(&r, r.entries, &resv), 0);
	KUNIT_EXPECT_EQ(t, resv.range.start, 0);
	KUNIT_EXPECT_EQ(t, resv.range.end, r.entries - 1);
	KUNIT_EXPECT_EQ(t, le64_to_cpu(wi), r.entries);
	sdxi_ring_resv_foreach(&resv, desc) {
		KUNIT_EXPECT_NOT_NULL_MSG(t, sdxi_ring_resv_next(&resv),
			"unexpected null descriptor for index %llu", resv.iter);
	}

	ri = cpu_to_le64(1);
	KUNIT_EXPECT_EQ(t, sdxi_ring_try_reserve(&r, 1, &resv), 0);
	KUNIT_EXPECT_EQ(t, le64_to_cpu(wi), r.entries + 1);
	KUNIT_EXPECT_NOT_NULL(t, sdxi_ring_resv_next(&resv));
}

static void invalid(struct kunit *t)
{
	__le64 wi, ri;
	struct sdxi_ring_state rs;
	struct sdxi_ring_resv resv;
	struct sdxi_desc *descs;

	descs = kunit_kmalloc_array(t, SZ_1K, sizeof(descs[0]),
				    GFP_KERNEL | __GFP_ZERO);
	KUNIT_ASSERT_NOT_NULL(t, descs);

	ri = wi = 0;
	sdxi_ring_state_init(&rs, &ri, &wi, SZ_1K, descs);

	KUNIT_EXPECT_EQ(t, sdxi_ring_try_reserve(&rs, 0, &resv), -EINVAL);
	KUNIT_EXPECT_EQ(t, sdxi_ring_try_reserve(&rs, rs.entries + 1, &resv), -EINVAL);

	ri = cpu_to_le64(1);
	KUNIT_EXPECT_EQ(t, sdxi_ring_try_reserve(&rs, 1, &resv), -EIO);

	ri = 0;
	wi = cpu_to_le64(rs.entries);
	sdxi_ring_state_init(&rs, &ri, &wi, SZ_1K, descs);
	KUNIT_EXPECT_EQ(t, sdxi_ring_try_reserve(&rs, 1, &resv), -EBUSY);

	ri = cpu_to_le64(rs.entries);
	wi = cpu_to_le64(rs.entries + 1);
	sdxi_ring_state_init(&rs, &ri, &wi, SZ_1K, descs);
	KUNIT_EXPECT_EQ(t, sdxi_ring_try_reserve(&rs, rs.entries, &resv), -EBUSY);
}

static struct kunit_case testcases[] = {
	KUNIT_CASE(valid),
	KUNIT_CASE(invalid),
	{}
};

static int setup_device(struct kunit *t)
{
	struct device *dev = kunit_device_register(t, "sdxi-mock-device");

	KUNIT_ASSERT_NOT_ERR_OR_NULL(t, dev);
	t->priv = dev;
	return 0;
}

static struct kunit_suite generic_desc_ts = {
	.name = "SDXI descriptor ring management",
	.test_cases = testcases,
	.init = setup_device,
};
kunit_test_suite(generic_desc_ts);

MODULE_DESCRIPTION("SDXI descriptor ring tests");
MODULE_AUTHOR("Nathan Lynch");
MODULE_LICENSE("GPL");
