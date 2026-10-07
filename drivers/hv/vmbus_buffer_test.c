// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for Hyper-V VMBus buffer allocation and GPADL lifetime.
 *
 * Built into the hv_vmbus object rather than a separate module so the
 * cases can reach the internal helpers declared in hyperv_vmbus.h
 * without exporting them.
 */
#include <kunit/test.h>
#include <linux/hyperv.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "hyperv_vmbus.h"

static void vmbus_buffer_size_rounding_test(struct kunit *test)
{
	u32 rounded_size;

	KUNIT_EXPECT_EQ(test, vmbus_buffer_round_size(1, &rounded_size), 0);
	KUNIT_EXPECT_EQ(test, rounded_size, (u32)PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, vmbus_buffer_round_size(PAGE_SIZE, &rounded_size), 0);
	KUNIT_EXPECT_EQ(test, rounded_size, (u32)PAGE_SIZE);
}

static void vmbus_buffer_size_overflow_test(struct kunit *test)
{
	u32 rounded_size = 0;
	int ret;

	KUNIT_EXPECT_EQ(test, vmbus_buffer_round_size(0, &rounded_size), -EINVAL);
	ret = vmbus_buffer_round_size(U32_MAX, &rounded_size);
	KUNIT_EXPECT_EQ(test, ret, -EOVERFLOW);
	ret = vmbus_buffer_round_size(U32_MAX - PAGE_SIZE + 1, &rounded_size);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, rounded_size,
			(u32)(U32_MAX - PAGE_SIZE + 1));
}

static void vmbus_ring_fallback_order_zero_test(struct kunit *test)
{
	unsigned int order;

	order = vmbus_buffer_order(1UL << MAX_PAGE_ORDER, MAX_PAGE_ORDER);
	KUNIT_EXPECT_EQ(test, order, (unsigned int)MAX_PAGE_ORDER);
	while (order)
		KUNIT_ASSERT_TRUE(test, vmbus_buffer_lower_order(&order));
	KUNIT_EXPECT_FALSE(test, vmbus_buffer_lower_order(&order));
	KUNIT_EXPECT_EQ(test, order, 0U);
	KUNIT_EXPECT_EQ(test, vmbus_buffer_order(3, MAX_PAGE_ORDER), 1U);
}

static void vmbus_buffer_failed_teardown_leaks_test(struct kunit *test)
{
	struct vmbus_buffer buffer = {
		.addr = (void *)1,
		.gpadl.gpadl_handle = 1,
	};

	KUNIT_EXPECT_FALSE(test, vmbus_buffer_should_free(&buffer));
	buffer.gpadl.gpadl_handle = 0;
	buffer.gpadl.leak = true;
	KUNIT_EXPECT_FALSE(test, vmbus_buffer_should_free(&buffer));
	buffer.gpadl.leak = false;
	buffer.leak = true;
	KUNIT_EXPECT_FALSE(test, vmbus_buffer_should_free(&buffer));
	buffer.leak = false;
	KUNIT_EXPECT_TRUE(test, vmbus_buffer_should_free(&buffer));
}

static void vmbus_buffer_partial_allocation_cleanup_test(struct kunit *test)
{
	struct page **chunks;
	struct vmbus_buffer buffer = {};

	chunks = kmalloc_obj(*chunks, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, chunks);
	vmbus_free_buffer(NULL, chunks, 0);

	buffer.addr = vzalloc(PAGE_SIZE);
	KUNIT_ASSERT_NOT_NULL(test, buffer.addr);
	vmbus_release_buffer(&buffer);
	KUNIT_EXPECT_PTR_EQ(test, buffer.addr, NULL);
	vmbus_release_buffer(&buffer);
}

static struct kunit_case vmbus_buffer_test_cases[] = {
	KUNIT_CASE(vmbus_buffer_size_rounding_test),
	KUNIT_CASE(vmbus_buffer_size_overflow_test),
	KUNIT_CASE(vmbus_ring_fallback_order_zero_test),
	KUNIT_CASE(vmbus_buffer_failed_teardown_leaks_test),
	KUNIT_CASE(vmbus_buffer_partial_allocation_cleanup_test),
	{}
};

static struct kunit_suite vmbus_buffer_test_suite = {
	.name = "hyperv-vmbus-buffer",
	.test_cases = vmbus_buffer_test_cases,
};

kunit_test_suite(vmbus_buffer_test_suite);
