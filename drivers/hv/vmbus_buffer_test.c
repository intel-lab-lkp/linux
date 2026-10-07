// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for Hyper-V VMBus buffer allocation and GPADL lifetime.
 *
 * Built into the hv_vmbus object rather than a separate module so the
 * cases can reach the internal helpers declared in hyperv_vmbus.h
 * without exporting them.
 */
#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/hyperv.h>
#include <linux/mm.h>
#include <linux/sizes.h>
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

static void vmbus_buffer_private_shared_selection_test(struct kunit *test)
{
	static const struct {
		bool isolation;
		bool encrypted;
		bool confidential;
		bool shared;
	} cases[] = {
		{ false, false, false, false },
		{ false, false, true, false },
		{ false, true, false, true },
		{ false, true, true, false },
		{ true, false, false, true },
		{ true, false, true, false },
		{ true, true, false, true },
		{ true, true, true, false },
	};
	unsigned int i;

	/* Either platform signal requires visibility; confidential stays private. */
	for (i = 0; i < ARRAY_SIZE(cases); i++)
		KUNIT_EXPECT_EQ(test,
				vmbus_needs_shared_pages(cases[i].isolation,
							 cases[i].encrypted,
							 cases[i].confidential),
				cases[i].shared);
}

static void vmbus_buffer_owner_reclaim_gate_test(struct kunit *test)
{
	struct vmbus_buffer_retained owner = {
		.released = false,
	};

	KUNIT_EXPECT_FALSE(test, vmbus_buffer_owner_can_reclaim(&owner));
	owner.released = true;
	KUNIT_EXPECT_TRUE(test, vmbus_buffer_owner_can_reclaim(&owner));

	owner.host_may_own = true;
	KUNIT_EXPECT_FALSE(test, vmbus_buffer_owner_can_reclaim(&owner));

	owner.host_may_own = false;	/* GPADL teardown acknowledgment */
	KUNIT_EXPECT_TRUE(test, vmbus_buffer_owner_can_reclaim(&owner));

	owner.permanent_leak = true;
	KUNIT_EXPECT_FALSE(test, vmbus_buffer_owner_can_reclaim(&owner));

	owner.permanent_leak = false;
	owner.encryption_unknown = true;
	KUNIT_EXPECT_FALSE(test, vmbus_buffer_owner_can_reclaim(&owner));
}

static void vmbus_buffer_reclaim_schedule_gate_test(struct kunit *test)
{
	struct vmbus_buffer_retained owner = {
		.released = true,
	};

	KUNIT_EXPECT_TRUE(test,
			  vmbus_buffer_owner_should_schedule(&owner, false, true));
	owner.work_active = true;
	KUNIT_EXPECT_FALSE(test,
			   vmbus_buffer_owner_should_schedule(&owner, false, true));
	owner.work_active = false;
	owner.reclaiming = true;
	KUNIT_EXPECT_FALSE(test,
			   vmbus_buffer_owner_should_schedule(&owner, false, true));
	owner.reclaiming = false;
	KUNIT_EXPECT_FALSE(test,
			   vmbus_buffer_owner_should_schedule(&owner, true, true));
	KUNIT_EXPECT_FALSE(test,
			   vmbus_buffer_owner_should_schedule(&owner, false, false));
}

struct vmbus_reclaim_test_work {
	struct vmbus_buffer_retained owner;
	struct workqueue_struct *wq;
	struct completion entered;
	struct completion proceed;
	atomic_t calls;
	bool claim_owner;
};

static void vmbus_reclaim_test_callback(struct work_struct *work)
{
	struct vmbus_buffer_retained *owner =
		container_of(to_delayed_work(work),
			     struct vmbus_buffer_retained, reclaim_work);
	struct vmbus_reclaim_test_work *ctx =
		container_of(owner, struct vmbus_reclaim_test_work, owner);

	atomic_inc(&ctx->calls);
	if (ctx->claim_owner)
		owner->reclaiming = true;
	complete(&ctx->entered);
	wait_for_completion(&ctx->proceed);
}

static void vmbus_reclaim_test_cleanup(void *data)
{
	struct vmbus_reclaim_test_work *ctx = data;

	complete_all(&ctx->proceed);
	cancel_delayed_work_sync(&ctx->owner.reclaim_work);
	destroy_workqueue(ctx->wq);
}

static struct vmbus_reclaim_test_work *
vmbus_reclaim_test_init(struct kunit *test)
{
	struct vmbus_reclaim_test_work *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;
	init_completion(&ctx->entered);
	init_completion(&ctx->proceed);
	atomic_set(&ctx->calls, 0);
	ctx->owner.released = true;
	ctx->owner.work_active = true;
	INIT_DELAYED_WORK(&ctx->owner.reclaim_work,
			  vmbus_reclaim_test_callback);
	ctx->wq = alloc_workqueue("vmbus-reclaim-test",
				  WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!ctx->wq)
		return NULL;
	if (kunit_add_action_or_reset(test, vmbus_reclaim_test_cleanup, ctx))
		return NULL;
	return ctx;
}

static void vmbus_reclaim_running_test(struct kunit *test, bool claim_owner)
{
	struct vmbus_reclaim_test_work *ctx = vmbus_reclaim_test_init(test);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->claim_owner = claim_owner;
	KUNIT_ASSERT_TRUE(test,
			  queue_delayed_work(ctx->wq, &ctx->owner.reclaim_work, 0));
	KUNIT_ASSERT_NE(test,
			wait_for_completion_timeout(&ctx->entered,
						    msecs_to_jiffies(1000)), 0UL);

	/*
	 * The native workqueue cleared pending before entering the callback.
	 * Test both windows around the callback taking ownership. Keep the
	 * fixture alive so an incorrect second queue fails without a UAF.
	 */
	vmbus_buffer_owner_drain(&ctx->owner, ctx->wq);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&ctx->owner.reclaim_work));
	KUNIT_EXPECT_TRUE(test, ctx->owner.work_active);
	complete_all(&ctx->proceed);
	flush_workqueue(ctx->wq);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->calls), 1);
}

static void vmbus_reclaim_shutdown_before_claim_test(struct kunit *test)
{
	vmbus_reclaim_running_test(test, false);
}

static void vmbus_reclaim_shutdown_during_reclaim_test(struct kunit *test)
{
	vmbus_reclaim_running_test(test, true);
}

static void vmbus_reclaim_shutdown_pending_test(struct kunit *test)
{
	struct vmbus_reclaim_test_work *ctx = vmbus_reclaim_test_init(test);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	KUNIT_ASSERT_TRUE(test,
			  queue_delayed_work(ctx->wq, &ctx->owner.reclaim_work,
					     msecs_to_jiffies(60000)));
	complete_all(&ctx->proceed);
	vmbus_buffer_owner_drain(&ctx->owner, ctx->wq);
	flush_workqueue(ctx->wq);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->calls), 1);
}

static void vmbus_reclaim_shutdown_unsafe_pending_test(struct kunit *test)
{
	struct vmbus_reclaim_test_work *ctx = vmbus_reclaim_test_init(test);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->owner.host_may_own = true;
	KUNIT_ASSERT_TRUE(test,
			  queue_delayed_work(ctx->wq, &ctx->owner.reclaim_work,
					     msecs_to_jiffies(60000)));
	vmbus_buffer_owner_drain(&ctx->owner, ctx->wq);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&ctx->owner.reclaim_work));
	KUNIT_EXPECT_FALSE(test, ctx->owner.work_active);
	flush_workqueue(ctx->wq);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->calls), 0);
}

static struct vmbus_channel_msginfo *vmbus_test_alloc_teardown_fail(void)
{
	return NULL;
}

static void vmbus_buffer_rescind_retains_gpadl_test(struct kunit *test)
{
	struct vmbus_channel channel = { .rescind = true };
	struct vmbus_buffer_retained owner = {
		.released = true,
		.host_may_own = true,
	};
	struct vmbus_buffer buffer = { .owner = &owner };
	unsigned int origin, pending;
	u32 handle;
	int ret;

	/* Exercise the request core without touching the live connection. */
	for (origin = 0; origin < 2; origin++) {
		channel.rescind_from_host = origin;
		for (pending = 0; pending < 2; pending++) {
			buffer.gpadl.gpadl_handle = pending ? 0 : 17;
			buffer.gpadl.leak = pending;
			handle = buffer.gpadl.gpadl_handle;
			ret = vmbus_gpadl_teardown_request(&channel, &buffer,
							   vmbus_test_alloc_teardown_fail,
							   NULL, NULL, 1);
			KUNIT_EXPECT_EQ(test, ret, !origin ? -ENODEV :
					  pending ? -EINPROGRESS : -ENOMEM);
			KUNIT_EXPECT_EQ(test, buffer.gpadl.gpadl_handle, handle);
			KUNIT_EXPECT_TRUE(test, buffer.gpadl.leak);
			KUNIT_EXPECT_TRUE(test, owner.host_may_own);
			KUNIT_EXPECT_FALSE(test, owner.work_active);
			KUNIT_EXPECT_FALSE(test, vmbus_buffer_should_free(&buffer));
			KUNIT_EXPECT_FALSE(test, vmbus_buffer_owner_can_reclaim(&owner));
		}
	}
}

enum vmbus_test_teardown_reply {
	VMBUS_TEST_REPLY_ACK,
	VMBUS_TEST_REPLY_NONE,
	VMBUS_TEST_REPLY_WRONG_HANDLE,
	VMBUS_TEST_REPLY_WRONG_TYPE,
	VMBUS_TEST_REPLY_POST_FAILURE,
};

struct vmbus_teardown_test_context {
	struct vmbus_connection connection;
	struct vmbus_channel channel;
	struct vmbus_buffer buffer;
	struct vmbus_buffer_retained owner;
	struct kunit *test;
	enum vmbus_test_teardown_reply reply;
	unsigned int post_calls;
};

static struct vmbus_channel_msginfo *vmbus_test_alloc_teardown(void)
{
	return kzalloc(sizeof(struct vmbus_channel_msginfo) +
		       sizeof(struct vmbus_channel_gpadl_teardown), GFP_KERNEL);
}

static int vmbus_test_post_teardown(struct vmbus_connection *connection,
				    struct vmbus_channel_msginfo *info)
{
	struct vmbus_teardown_test_context *ctx =
		container_of(connection, struct vmbus_teardown_test_context,
			     connection);
	struct vmbus_channel_gpadl_teardown *msg = (void *)info->msg;
	struct vmbus_channel_gpadl_torndown response = {
		.header.msgtype = CHANNELMSG_GPADL_TORNDOWN,
		.gpadl = msg->gpadl,
	};

	ctx->post_calls++;
	KUNIT_EXPECT_EQ(ctx->test, msg->header.msgtype, CHANNELMSG_GPADL_TEARDOWN);
	KUNIT_EXPECT_EQ(ctx->test, msg->child_relid, 71U);
	KUNIT_EXPECT_EQ(ctx->test, msg->gpadl, 51U);
	KUNIT_EXPECT_PTR_EQ(ctx->test, info->waiting_channel, NULL);
	if (ctx->reply == VMBUS_TEST_REPLY_POST_FAILURE)
		return -EIO;
	if (ctx->reply == VMBUS_TEST_REPLY_NONE)
		return 0;
	if (ctx->reply == VMBUS_TEST_REPLY_WRONG_HANDLE)
		response.gpadl++;
	if (ctx->reply == VMBUS_TEST_REPLY_WRONG_TYPE)
		response.header.msgtype = CHANNELMSG_GPADL_CREATED;
	vmbus_complete_gpadl_teardown(connection, &response);
	return 0;
}

static struct vmbus_teardown_test_context *
vmbus_test_teardown_context(struct kunit *test)
{
	struct vmbus_teardown_test_context *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;
	ctx->test = test;
	ctx->buffer.gpadl.gpadl_handle = 51;
	ctx->buffer.owner = &ctx->owner;
	ctx->owner.host_may_own = true;
	ctx->channel.rescind = true;
	ctx->channel.rescind_from_host = true;
	ctx->channel.offermsg.child_relid = 71;
	INIT_LIST_HEAD(&ctx->connection.chn_msg_list);
	spin_lock_init(&ctx->connection.channelmsg_lock);
	return ctx;
}

static int vmbus_test_teardown_request(struct vmbus_teardown_test_context *ctx)
{
	return vmbus_gpadl_teardown_request(&ctx->channel, &ctx->buffer,
					  vmbus_test_alloc_teardown,
					  &ctx->connection,
					  vmbus_test_post_teardown, 1);
}

static void vmbus_host_rescind_teardown_ack_test(struct kunit *test)
{
	struct vmbus_teardown_test_context *ctx = vmbus_test_teardown_context(test);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	KUNIT_EXPECT_EQ(test, vmbus_test_teardown_request(ctx), 0);
	KUNIT_EXPECT_EQ(test, ctx->post_calls, 1U);
	KUNIT_EXPECT_EQ(test, ctx->buffer.gpadl.gpadl_handle, 0U);
	KUNIT_EXPECT_FALSE(test, ctx->buffer.gpadl.leak);
	KUNIT_EXPECT_FALSE(test, ctx->owner.host_may_own);
	KUNIT_EXPECT_TRUE(test, vmbus_buffer_should_free(&ctx->buffer));
	KUNIT_EXPECT_TRUE(test, list_empty(&ctx->connection.chn_msg_list));
}

static void vmbus_test_teardown_failure(struct kunit *test,
					enum vmbus_test_teardown_reply reply,
					int expected)
{
	struct vmbus_teardown_test_context *ctx = vmbus_test_teardown_context(test);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->reply = reply;
	KUNIT_EXPECT_EQ(test, vmbus_test_teardown_request(ctx), expected);
	KUNIT_EXPECT_EQ(test, ctx->post_calls, 1U);
	KUNIT_EXPECT_EQ(test, ctx->buffer.gpadl.gpadl_handle, 51U);
	KUNIT_EXPECT_FALSE(test, vmbus_buffer_should_free(&ctx->buffer));
	KUNIT_EXPECT_TRUE(test, ctx->owner.host_may_own);
	KUNIT_EXPECT_TRUE(test, list_empty(&ctx->connection.chn_msg_list));
}

static void vmbus_host_rescind_teardown_timeout_test(struct kunit *test)
{
	vmbus_test_teardown_failure(test, VMBUS_TEST_REPLY_NONE, -ETIMEDOUT);
}

static void vmbus_host_rescind_teardown_wrong_handle_test(struct kunit *test)
{
	vmbus_test_teardown_failure(test, VMBUS_TEST_REPLY_WRONG_HANDLE,
				    -ETIMEDOUT);
}

static void vmbus_host_rescind_teardown_wrong_type_test(struct kunit *test)
{
	vmbus_test_teardown_failure(test, VMBUS_TEST_REPLY_WRONG_TYPE, -ENODEV);
}

static void vmbus_host_rescind_teardown_post_failure_test(struct kunit *test)
{
	vmbus_test_teardown_failure(test, VMBUS_TEST_REPLY_POST_FAILURE, -EIO);
}

static void vmbus_local_rescind_skips_teardown_test(struct kunit *test)
{
	struct vmbus_teardown_test_context *ctx = vmbus_test_teardown_context(test);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->channel.rescind_from_host = false;
	KUNIT_EXPECT_EQ(test, vmbus_test_teardown_request(ctx), -ENODEV);
	KUNIT_EXPECT_EQ(test, ctx->post_calls, 0U);
	KUNIT_EXPECT_TRUE(test, ctx->buffer.gpadl.leak);
	KUNIT_EXPECT_TRUE(test, ctx->owner.host_may_own);
	KUNIT_EXPECT_EQ(test, ctx->buffer.gpadl.gpadl_handle, 51U);
}

static void vmbus_invalid_relid_skips_teardown_test(struct kunit *test)
{
	struct vmbus_teardown_test_context *ctx = vmbus_test_teardown_context(test);

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->channel.offermsg.child_relid = INVALID_RELID;
	KUNIT_EXPECT_EQ(test, vmbus_test_teardown_request(ctx), -ENODEV);
	KUNIT_EXPECT_EQ(test, ctx->post_calls, 0U);
	KUNIT_EXPECT_EQ(test, ctx->buffer.gpadl.gpadl_handle, 51U);
}

static void vmbus_host_rescind_teardown_late_ack_test(struct kunit *test)
{
	struct vmbus_teardown_test_context *ctx = vmbus_test_teardown_context(test);
	struct vmbus_channel_gpadl_torndown response = {
		.header.msgtype = CHANNELMSG_GPADL_TORNDOWN,
		.gpadl = 51,
	};

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->reply = VMBUS_TEST_REPLY_NONE;
	KUNIT_ASSERT_EQ(test, vmbus_test_teardown_request(ctx), -ETIMEDOUT);
	/* Run the production response matcher after its waiter has been freed. */
	vmbus_complete_gpadl_teardown(&ctx->connection, &response);
	KUNIT_EXPECT_TRUE(test, list_empty(&ctx->connection.chn_msg_list));
	KUNIT_EXPECT_EQ(test, ctx->buffer.gpadl.gpadl_handle, 51U);
	KUNIT_EXPECT_FALSE(test, vmbus_buffer_should_free(&ctx->buffer));
	KUNIT_EXPECT_TRUE(test, ctx->owner.host_may_own);
}

static void vmbus_buffer_mapping_reference_test(struct kunit *test)
{
	struct vmbus_buffer_retained owner = {};
	struct page *page;
	struct page *pages[1];

	page = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, page);
	pages[0] = page;
	owner.pages = pages;
	owner.page_cnt = ARRAY_SIZE(pages);

	KUNIT_EXPECT_FALSE(test, vmbus_buffer_pages_busy(&owner));
	get_page(page); /* vm_insert_pages() holds one reference per mapping */
	KUNIT_EXPECT_TRUE(test, vmbus_buffer_pages_busy(&owner));
	put_page(page);
	KUNIT_EXPECT_FALSE(test, vmbus_buffer_pages_busy(&owner));
	__free_page(page);
}

static void vmbus_buffer_repeated_owner_release_test(struct kunit *test)
{
	struct vmbus_channel channel = {};
	struct vmbus_buffer buffer = {};
	struct vmbus_buffer empty = {};

	buffer.addr = vzalloc(PAGE_SIZE);
	KUNIT_ASSERT_NOT_NULL(test, buffer.addr);
	buffer.owner = vmbus_buffer_owner_alloc(&channel);
	if (!buffer.owner) {
		vfree(buffer.addr);
		KUNIT_FAIL(test, "failed to allocate a VMBus buffer owner");
		return;
	}
	buffer.size = PAGE_SIZE;
	empty.owner = vmbus_buffer_owner_alloc(&channel);
	if (!empty.owner) {
		vmbus_release_buffer(&buffer);
		KUNIT_FAIL(test, "failed to allocate a second VMBus buffer owner");
		return;
	}
	KUNIT_EXPECT_TRUE(test, empty.owner->owner_id != buffer.owner->owner_id);
	vmbus_release_buffer(&empty);
	KUNIT_EXPECT_PTR_EQ(test, empty.owner, NULL);

	vmbus_release_buffer(&buffer);
	KUNIT_EXPECT_PTR_EQ(test, buffer.addr, NULL);
	KUNIT_EXPECT_PTR_EQ(test, buffer.owner, NULL);
	vmbus_release_buffer(&buffer);
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
	KUNIT_EXPECT_TRUE(test, posted);
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

struct vmbus_buffer_page_alloc_test_context {
	struct kunit *test;
	unsigned int attempts;
	unsigned int expected_order;
	bool fail_order_zero;
};

static struct page *vmbus_buffer_test_alloc_page(void *context, int nid,
						 gfp_t gfp,
						 unsigned int order)
{
	struct vmbus_buffer_page_alloc_test_context *test_context = context;

	(void)nid;
	KUNIT_EXPECT_EQ(test_context->test, order,
			test_context->expected_order);
	test_context->attempts++;
	if (test_context->expected_order)
		test_context->expected_order--;

	if (order)
		return NULL;
	if (test_context->fail_order_zero)
		return NULL;

	return alloc_pages(gfp, 0);
}

static void vmbus_buffer_order_zero_allocation_test(struct kunit *test)
{
	struct vmbus_buffer_page_alloc_test_context context = {
		.test = test,
		.expected_order = MAX_PAGE_ORDER,
	};
	struct page *page;
	unsigned int order = MAX_PAGE_ORDER;

	page = vmbus_alloc_pages_with_fallback(0, GFP_KERNEL, &order,
					       vmbus_buffer_test_alloc_page, &context);
	KUNIT_ASSERT_NOT_NULL(test, page);
	KUNIT_EXPECT_EQ(test, order, 0U);
	KUNIT_EXPECT_EQ(test, context.expected_order, 0U);
	KUNIT_EXPECT_EQ(test, context.attempts, (unsigned int)MAX_PAGE_ORDER + 1);
	__free_pages(page, 0);

	context.attempts = 0;
	context.expected_order = MAX_PAGE_ORDER;
	context.fail_order_zero = true;
	order = MAX_PAGE_ORDER;
	page = vmbus_alloc_pages_with_fallback(0, GFP_KERNEL, &order,
					       vmbus_buffer_test_alloc_page, &context);
	KUNIT_EXPECT_PTR_EQ(test, page, NULL);
	KUNIT_EXPECT_EQ(test, order, 0U);
	KUNIT_EXPECT_EQ(test, context.expected_order, 0U);
	KUNIT_EXPECT_EQ(test, context.attempts, (unsigned int)MAX_PAGE_ORDER + 1);
}

/*
 * Requestor metadata must use vmalloc backing because both the array and its
 * bitmap can require multiple pages. The requestor is guest-private
 * bookkeeping: the host never sees the slot values, so this mapping has no
 * Confidential Computing implication and needs no set_memory_decrypted().
 * Cover a typical 8 KiB array, a requestor whose bitmap exceeds one page,
 * and a size beyond KMALLOC_MAX_SIZE without memory pressure.
 */
static void vmbus_requestor_alloc_free_test(struct kunit *test)
{
	struct vmbus_requestor rqstor = {};

	KUNIT_ASSERT_EQ(test, vmbus_alloc_requestor(&rqstor, 4), 0);
	KUNIT_EXPECT_EQ(test, rqstor.size, 4U);
	KUNIT_EXPECT_EQ(test, rqstor.next_request_id, 0U);
	KUNIT_EXPECT_NOT_NULL(test, rqstor.req_arr);
	KUNIT_EXPECT_NOT_NULL(test, rqstor.req_bitmap);

	/* The free list links 0->1->2->3 and terminates in U64_MAX. */
	KUNIT_EXPECT_EQ(test, rqstor.req_arr[0], 1U);
	KUNIT_EXPECT_EQ(test, rqstor.req_arr[1], 2U);
	KUNIT_EXPECT_EQ(test, rqstor.req_arr[2], 3U);
	KUNIT_EXPECT_EQ(test, rqstor.req_arr[3], U64_MAX);

	vmbus_free_requestor(&rqstor);
}

static void vmbus_requestor_vmalloc_backing_test(struct kunit *test)
{
	/*
	 * Cover the small array, a requestor whose bitmap exceeds one page,
	 * and a size beyond KMALLOC_MAX_SIZE. All requestor metadata must use
	 * vmalloc backing regardless of size or allocator pressure.
	 */
	u32 small_size = 1024;
	u32 bitmap_size = (PAGE_SIZE / sizeof(unsigned long)) * BITS_PER_LONG + 1;
	u32 size = (KMALLOC_MAX_SIZE / sizeof(u64)) + 1;
	struct vmbus_requestor rqstor = {};
	u64 *req_arr;

	KUNIT_EXPECT_PTR_EQ(test, request_arr_init(0), NULL);

	req_arr = request_arr_init(small_size);
	KUNIT_ASSERT_NOT_NULL(test, req_arr);
	KUNIT_EXPECT_TRUE(test, is_vmalloc_addr(req_arr));
	KUNIT_EXPECT_EQ(test, req_arr[0], 1U);
	KUNIT_EXPECT_EQ(test, req_arr[small_size - 1], U64_MAX);
	vfree(req_arr);

	req_arr = request_arr_init(size);
	KUNIT_ASSERT_NOT_NULL(test, req_arr);
	KUNIT_EXPECT_TRUE(test, is_vmalloc_addr(req_arr));
	KUNIT_EXPECT_EQ(test, req_arr[0], 1U);
	KUNIT_EXPECT_EQ(test, req_arr[size - 1], U64_MAX);
	vfree(req_arr);

	KUNIT_ASSERT_EQ(test, vmbus_alloc_requestor(&rqstor, bitmap_size), 0);
	KUNIT_EXPECT_TRUE(test, is_vmalloc_addr(rqstor.req_arr));
	KUNIT_EXPECT_TRUE(test, is_vmalloc_addr(rqstor.req_bitmap));
	vmbus_free_requestor(&rqstor);
}

static void vmbus_requestor_id_lifecycle_test(struct kunit *test)
{
	struct vmbus_channel channel = { .rqstor_size = 4 };
	struct vmbus_requestor *rqstor = &channel.requestor;
	u64 id0, id1, id2, id3, addr;

	KUNIT_ASSERT_EQ(test, vmbus_alloc_requestor(rqstor, 4), 0);

	/* IDs are 1-based; 0 is reserved for unsolicited host messages. */
	id0 = vmbus_next_request_id(&channel, 0x1000);
	KUNIT_EXPECT_EQ(test, id0, 1U);
	id1 = vmbus_next_request_id(&channel, 0x2000);
	KUNIT_EXPECT_EQ(test, id1, 2U);

	/* Consumption returns the registered address and frees the slot. */
	addr = vmbus_request_addr_match(&channel, id0, VMBUS_RQST_ADDR_ANY);
	KUNIT_EXPECT_EQ(test, addr, 0x1000U);
	addr = vmbus_request_addr_match(&channel, id1, 0x2000);
	KUNIT_EXPECT_EQ(test, addr, 0x2000U);

	/*
	 * Consumed slots are reusable. The free list is LIFO -- each
	 * consume pushes its slot onto the head -- so the slot freed last
	 * is the one handed out first. Freeing id0 then id1 leaves slot 1
	 * at the head, and the next ID is therefore id1 again, not id0.
	 */
	id2 = vmbus_next_request_id(&channel, 0x3000);
	KUNIT_EXPECT_EQ(test, id2, id1);
	id3 = vmbus_next_request_id(&channel, 0x4000);
	KUNIT_EXPECT_EQ(test, id3, id0);

	vmbus_free_requestor(rqstor);
}

static void vmbus_requestor_invalid_ids_test(struct kunit *test)
{
	struct vmbus_channel channel = { .rqstor_size = 4 };
	struct vmbus_requestor *rqstor = &channel.requestor;
	u64 id, addr;

	KUNIT_ASSERT_EQ(test, vmbus_alloc_requestor(rqstor, 4), 0);

	/* ID 0 is the unsolicited-message sentinel and is never in the set. */
	KUNIT_EXPECT_EQ(test, vmbus_request_addr_match(&channel, 0, 0),
			VMBUS_RQST_ERROR);

	/* Out-of-range IDs are refused, not wrapped. */
	KUNIT_EXPECT_EQ(test, vmbus_request_addr_match(&channel, 5, 0),
			VMBUS_RQST_ERROR);
	KUNIT_EXPECT_EQ(test, vmbus_request_addr_match(&channel, U64_MAX, 0),
			VMBUS_RQST_ERROR);

	id = vmbus_next_request_id(&channel, 0xABCD);
	KUNIT_ASSERT_EQ(test, id, 1U);

	/* A wrong expected address does not consume the slot. */
	addr = vmbus_request_addr_match(&channel, id, 0x9999);
	KUNIT_EXPECT_EQ(test, addr, 0xABCDU);

	/* The slot is still live and a matching lookup consumes it once. */
	addr = vmbus_request_addr_match(&channel, id, 0xABCD);
	KUNIT_EXPECT_EQ(test, addr, 0xABCDU);

	/* Replay: the slot is gone. */
	addr = vmbus_request_addr_match(&channel, id, VMBUS_RQST_ADDR_ANY);
	KUNIT_EXPECT_EQ(test, addr, VMBUS_RQST_ERROR);

	vmbus_free_requestor(rqstor);
}

static void vmbus_requestor_exhaustion_test(struct kunit *test)
{
	struct vmbus_channel channel = { .rqstor_size = 2 };
	struct vmbus_requestor *rqstor = &channel.requestor;

	KUNIT_ASSERT_EQ(test, vmbus_alloc_requestor(rqstor, 2), 0);

	KUNIT_EXPECT_EQ(test, vmbus_next_request_id(&channel, 0x1), 1U);
	KUNIT_EXPECT_EQ(test, vmbus_next_request_id(&channel, 0x2), 2U);

	/* Both slots taken: the free list is empty and the API says so. */
	KUNIT_EXPECT_EQ(test, vmbus_next_request_id(&channel, 0x3),
			VMBUS_RQST_ERROR);

	/* An uninitialized requestor is not a full one. */
	channel.rqstor_size = 0;
	KUNIT_EXPECT_EQ(test, vmbus_next_request_id(&channel, 0x4),
			VMBUS_NO_RQSTOR);

	vmbus_free_requestor(rqstor);
}

static struct kunit_case vmbus_buffer_test_cases[] = {
	KUNIT_CASE(vmbus_buffer_size_rounding_test),
	KUNIT_CASE(vmbus_buffer_size_overflow_test),
	KUNIT_CASE(vmbus_buffer_private_shared_selection_test),
	KUNIT_CASE(vmbus_ring_fallback_order_zero_test),
	KUNIT_CASE(vmbus_buffer_failed_teardown_leaks_test),
	KUNIT_CASE(vmbus_buffer_owner_reclaim_gate_test),
	KUNIT_CASE(vmbus_buffer_reclaim_schedule_gate_test),
	KUNIT_CASE(vmbus_reclaim_shutdown_before_claim_test),
	KUNIT_CASE(vmbus_reclaim_shutdown_during_reclaim_test),
	KUNIT_CASE(vmbus_reclaim_shutdown_pending_test),
	KUNIT_CASE(vmbus_reclaim_shutdown_unsafe_pending_test),
	KUNIT_CASE(vmbus_buffer_rescind_retains_gpadl_test),
	KUNIT_CASE(vmbus_host_rescind_teardown_ack_test),
	KUNIT_CASE(vmbus_host_rescind_teardown_timeout_test),
	KUNIT_CASE(vmbus_host_rescind_teardown_wrong_handle_test),
	KUNIT_CASE(vmbus_host_rescind_teardown_wrong_type_test),
	KUNIT_CASE(vmbus_host_rescind_teardown_post_failure_test),
	KUNIT_CASE(vmbus_local_rescind_skips_teardown_test),
	KUNIT_CASE(vmbus_invalid_relid_skips_teardown_test),
	KUNIT_CASE(vmbus_host_rescind_teardown_late_ack_test),
	KUNIT_CASE(vmbus_buffer_mapping_reference_test),
	KUNIT_CASE(vmbus_buffer_partial_allocation_cleanup_test),
	KUNIT_CASE(vmbus_buffer_repeated_owner_release_test),
	KUNIT_CASE(vmbus_buffer_order_zero_allocation_test),
	KUNIT_CASE(vmbus_gpadl_post_failure_test),
	KUNIT_CASE(vmbus_gpadl_post_success_test),
	KUNIT_CASE(vmbus_gpadl_response_state_test),
	KUNIT_CASE(vmbus_gpadl_teardown_post_failure_test),
	KUNIT_CASE(vmbus_requestor_alloc_free_test),
	KUNIT_CASE(vmbus_requestor_vmalloc_backing_test),
	KUNIT_CASE(vmbus_requestor_id_lifecycle_test),
	KUNIT_CASE(vmbus_requestor_invalid_ids_test),
	KUNIT_CASE(vmbus_requestor_exhaustion_test),
	{}
};

static struct kunit_suite vmbus_buffer_test_suite = {
	.name = "hyperv-vmbus-buffer",
	.test_cases = vmbus_buffer_test_cases,
};

kunit_test_suite(vmbus_buffer_test_suite);
