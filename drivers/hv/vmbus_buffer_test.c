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

struct vmbus_gpadl_post_test_context {
	struct kunit *test;
	unsigned int call_count;
	unsigned int fail_call;
	u32 first_msgtype;
	u32 expected_gpadl;
};

static int vmbus_gpadl_test_post(void *context, void *buffer,
				 size_t buflen, bool can_sleep)
{
	struct vmbus_gpadl_post_test_context *test_context = context;
	struct vmbus_channel_message_header *header = buffer;

	KUNIT_EXPECT_TRUE(test_context->test, can_sleep);
	KUNIT_EXPECT_GT(test_context->test, buflen,
			(size_t)sizeof(*header));
	test_context->call_count++;
	KUNIT_EXPECT_EQ(test_context->test, header->msgtype,
			test_context->call_count == 1 ?
			test_context->first_msgtype : CHANNELMSG_GPADL_BODY);
	switch (header->msgtype) {
	case CHANNELMSG_GPADL_HEADER: {
		struct vmbus_channel_gpadl_header *gpadl_header = buffer;

		KUNIT_EXPECT_EQ(test_context->test, gpadl_header->gpadl,
				test_context->expected_gpadl);
		break;
	}
	case CHANNELMSG_GPADL_BODY: {
		struct vmbus_channel_gpadl_body *gpadl_body = buffer;

		KUNIT_EXPECT_EQ(test_context->test, gpadl_body->gpadl,
				test_context->expected_gpadl);
		break;
	}
	case CHANNELMSG_GPADL_TEARDOWN: {
		struct vmbus_channel_gpadl_teardown *teardown = buffer;

		KUNIT_EXPECT_EQ(test_context->test, teardown->gpadl,
				test_context->expected_gpadl);
		KUNIT_EXPECT_EQ(test_context->test, teardown->child_relid,
				7U);
		break;
	}
	default:
		KUNIT_FAIL(test_context->test,
			   "unexpected GPADL message type: %d",
			   header->msgtype);
	}
	if (test_context->call_count == test_context->fail_call)
		return -EIO;

	return 0;
}

static struct vmbus_channel_msginfo *
vmbus_gpadl_test_msginfo(struct kunit *test, unsigned int body_count)
{
	struct vmbus_channel_msginfo *msginfo;
	struct vmbus_channel_msginfo *body_info;
	unsigned int i;

	msginfo = kunit_kzalloc(test, sizeof(*msginfo) +
			       sizeof(struct vmbus_channel_gpadl_header),
			       GFP_KERNEL);
	if (!msginfo)
		return NULL;

	msginfo->msgsize = sizeof(*msginfo) +
			   sizeof(struct vmbus_channel_gpadl_header);
	INIT_LIST_HEAD(&msginfo->submsglist);

	for (i = 0; i < body_count; i++) {
		body_info = kunit_kzalloc(test,
					  sizeof(*body_info) +
					  sizeof(struct vmbus_channel_gpadl_body),
					  GFP_KERNEL);
		if (!body_info)
			return NULL;

		body_info->msgsize = sizeof(*body_info) +
				     sizeof(struct vmbus_channel_gpadl_body);
		INIT_LIST_HEAD(&body_info->msglistentry);
		list_add_tail(&body_info->msglistentry, &msginfo->submsglist);
	}

	return msginfo;
}

static void vmbus_gpadl_post_failure_test(struct kunit *test)
{
	unsigned int fail_call;

	for (fail_call = 1; fail_call <= 3; fail_call++) {
		struct vmbus_gpadl_post_test_context context = {
			.test = test,
			.fail_call = fail_call,
			.first_msgtype = CHANNELMSG_GPADL_HEADER,
			.expected_gpadl = 17,
		};
		struct vmbus_channel_msginfo *msginfo;
		bool posted = false;
		int ret;

		msginfo = vmbus_gpadl_test_msginfo(test, 2);
		KUNIT_ASSERT_NOT_NULL(test, msginfo);

		ret = vmbus_post_gpadl_messages(msginfo, 17, &posted,
						vmbus_gpadl_test_post, &context);
		KUNIT_EXPECT_EQ(test, ret, -EIO);
		KUNIT_EXPECT_EQ(test, context.call_count, fail_call);
		KUNIT_EXPECT_TRUE(test, posted);
	}
}

static void vmbus_gpadl_post_success_test(struct kunit *test)
{
	struct vmbus_gpadl_post_test_context context = {
		.test = test,
		.first_msgtype = CHANNELMSG_GPADL_HEADER,
		.expected_gpadl = 17,
	};
	struct vmbus_channel_msginfo *msginfo;
	bool posted = false;
	int ret;

	msginfo = vmbus_gpadl_test_msginfo(test, 2);
	KUNIT_ASSERT_NOT_NULL(test, msginfo);

	ret = vmbus_post_gpadl_messages(msginfo, 17, &posted,
					vmbus_gpadl_test_post, &context);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, context.call_count, 3U);
	KUNIT_EXPECT_TRUE(test, posted);
}

static void vmbus_gpadl_response_state_test(struct kunit *test)
{
	bool posted = true;

	KUNIT_EXPECT_EQ(test, vmbus_gpadl_response_status(0, false, &posted), 0);
	KUNIT_EXPECT_FALSE(test, posted);

	posted = true;
	KUNIT_EXPECT_EQ(test,
			vmbus_gpadl_response_status(1, false, &posted), -EDQUOT);
	KUNIT_EXPECT_FALSE(test, posted);

	posted = true;
	KUNIT_EXPECT_EQ(test,
			vmbus_gpadl_response_status(0, true, &posted), -ENODEV);
	KUNIT_EXPECT_FALSE(test, posted);
}

static void vmbus_gpadl_teardown_post_failure_test(struct kunit *test)
{
	struct vmbus_gpadl_post_test_context context = {
		.test = test,
		.fail_call = 1,
		.first_msgtype = CHANNELMSG_GPADL_TEARDOWN,
		.expected_gpadl = 17,
	};
	struct vmbus_channel_gpadl_teardown msg = {};
	int ret;

	ret = vmbus_post_gpadl_teardown(7, &msg, 17,
					vmbus_gpadl_test_post, &context);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	KUNIT_EXPECT_EQ(test, context.call_count, 1U);
}

static struct kunit_case vmbus_buffer_test_cases[] = {
	KUNIT_CASE(vmbus_buffer_size_rounding_test),
	KUNIT_CASE(vmbus_buffer_size_overflow_test),
	KUNIT_CASE(vmbus_ring_fallback_order_zero_test),
	KUNIT_CASE(vmbus_buffer_failed_teardown_leaks_test),
	KUNIT_CASE(vmbus_buffer_partial_allocation_cleanup_test),
	KUNIT_CASE(vmbus_gpadl_post_failure_test),
	KUNIT_CASE(vmbus_gpadl_post_success_test),
	KUNIT_CASE(vmbus_gpadl_response_state_test),
	KUNIT_CASE(vmbus_gpadl_teardown_post_failure_test),
	{}
};

static struct kunit_suite vmbus_buffer_test_suite = {
	.name = "hyperv-vmbus-buffer",
	.test_cases = vmbus_buffer_test_cases,
};

kunit_test_suite(vmbus_buffer_test_suite);
