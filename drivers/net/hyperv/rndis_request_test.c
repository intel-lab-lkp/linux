// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for RNDIS request descriptor allocation.
 *
 * Built into the hv_netvsc object rather than a separate module so the
 * cases can reach the internal helpers declared in hyperv_net.h
 * without exporting them.
 */
#include <kunit/test.h>
#include <linux/hyperv.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/stddef.h>
#include <linux/vmalloc.h>

#include "hyperv_net.h"

static int rndis_test_device_init(struct rndis_device *dev)
{
	memset(dev, 0, sizeof(*dev));
	atomic_set(&dev->new_req_id, 0);
	spin_lock_init(&dev->request_lock);
	INIT_LIST_HEAD(&dev->req_list);
	return 0;
}

/*
 * The premise of the kvzalloc_obj() conversion: the descriptor embeds
 * two RNDIS_EXT_LEN protocol tails and sits past
 * KMALLOC_MAX_CACHE_SIZE, so kzalloc_obj() reached kmalloc_large() and
 * took an order-2 compound page on the subchannel open path. If the
 * struct ever drops back inside the cache, the conversion stops being
 * about high-order allocation and this assertion is the reminder.
 */
static void rndis_request_size_premise_test(struct kunit *test)
{
	KUNIT_EXPECT_GT(test, sizeof(struct rndis_request),
			(size_t)KMALLOC_MAX_CACHE_SIZE);
	/*
	 * The copy bound in rndis_filter_receive_resp() is
	 * sizeof(struct rndis_message) + RNDIS_EXT_LEN. That is exactly
	 * the response_msg + response_ext tail of the object. A reorder
	 * or a resize of either member makes the bound lie.
	 */
	KUNIT_EXPECT_EQ(test,
			offsetofend(struct rndis_request, response_ext),
			offsetof(struct rndis_request, response_msg) +
				sizeof(struct rndis_message) + RNDIS_EXT_LEN);
	KUNIT_EXPECT_EQ(test,
			offsetofend(struct rndis_request, request_ext),
			offsetof(struct rndis_request, request_msg) +
				sizeof(struct rndis_message) + RNDIS_EXT_LEN);
}

/*
 * kvzalloc_obj() keeps the zeroing kzalloc_obj() provided. The two
 * protocol tails are the fields a response is copied into; a stale
 * tail is a stale response.
 */
static void rndis_request_alloc_zeroed_test(struct kunit *test)
{
	struct rndis_device dev;
	struct rndis_request *req;

	KUNIT_ASSERT_EQ(test, rndis_test_device_init(&dev), 0);
	req = get_rndis_request(&dev, RNDIS_MSG_INIT, 0x20);
	KUNIT_ASSERT_NOT_NULL(test, req);

	KUNIT_EXPECT_EQ(test, req->response_ext[0], 0);
	KUNIT_EXPECT_EQ(test, req->response_ext[RNDIS_EXT_LEN - 1], 0);
	KUNIT_EXPECT_EQ(test, req->request_ext[0], 0);
	KUNIT_EXPECT_EQ(test, req->request_ext[RNDIS_EXT_LEN - 1], 0);
	KUNIT_EXPECT_EQ(test, req->response_msg.msg_len, 0U);

	put_rndis_request(&dev, req);
}

/* Request ids come from a per-device counter and the list tracks the live set. */
static void rndis_request_id_lifecycle_test(struct kunit *test)
{
	struct rndis_device dev;
	struct rndis_request *req0, *req1;
	struct rndis_request *cursor;
	unsigned int n = 0;

	KUNIT_ASSERT_EQ(test, rndis_test_device_init(&dev), 0);

	req0 = get_rndis_request(&dev, RNDIS_MSG_INIT, 0x20);
	KUNIT_ASSERT_NOT_NULL(test, req0);
	req1 = get_rndis_request(&dev, RNDIS_MSG_QUERY, 0x20);
	KUNIT_ASSERT_NOT_NULL(test, req1);

	/*
	 * get_rndis_request() stamps the id through the set_req template,
	 * which shares the union slot with every other request body. Read
	 * it back through the same member the producer used.
	 */
	KUNIT_EXPECT_EQ(test,
			req0->request_msg.msg.set_req.req_id, 1U);
	KUNIT_EXPECT_EQ(test,
			req1->request_msg.msg.set_req.req_id, 2U);

	list_for_each_entry(cursor, &dev.req_list, list_ent)
		n++;
	KUNIT_EXPECT_EQ(test, n, 2U);

	/*
	 * put_rndis_request() is the cleanup path every error and
	 * timeout branch takes. It unlinks before it frees.
	 */
	put_rndis_request(&dev, req0);
	n = 0;
	list_for_each_entry(cursor, &dev.req_list, list_ent)
		n++;
	KUNIT_EXPECT_EQ(test, n, 1U);
	KUNIT_EXPECT_TRUE(test,
			  list_first_entry(&dev.req_list, struct rndis_request,
					   list_ent) == req1);

	put_rndis_request(&dev, req1);
	KUNIT_EXPECT_TRUE(test, list_empty(&dev.req_list));
}

/*
 * A request left on the list after a completed exchange is a leak of
 * the descriptor and of its slot in the outstanding set. Repeated
 * get/put cycles must return to the empty list every time.
 */
static void rndis_request_put_cleanup_test(struct kunit *test)
{
	struct rndis_device dev;
	struct rndis_request *req;
	int i;

	KUNIT_ASSERT_EQ(test, rndis_test_device_init(&dev), 0);

	for (i = 0; i < 4; i++) {
		req = get_rndis_request(&dev, RNDIS_MSG_INIT, 0x20);
		KUNIT_ASSERT_NOT_NULL(test, req);
		KUNIT_EXPECT_FALSE(test, list_empty(&dev.req_list));
		put_rndis_request(&dev, req);
		KUNIT_EXPECT_TRUE(test, list_empty(&dev.req_list));
	}
}

/*
 * kvzalloc_obj() may return a vmalloc address under fragmentation. RNDIS
 * requests are GPA-direct packets, so each backing page must be represented
 * explicitly instead of deriving a single PFN with virt_to_phys().
 */
static void rndis_request_vmalloc_page_buffers_test(struct kunit *test)
{
	struct hv_page_buffer page_bufs[MAX_PAGE_BUFFER_COUNT];
	const u32 data_len = 16;
	u8 *allocation = vzalloc(2 * PAGE_SIZE);
	const u8 *cursor;
	phys_addr_t phys;
	u32 count = 0, remaining = data_len, i;
	int ret;

	if (!allocation) {
		KUNIT_FAIL(test, "could not allocate vmalloc test buffer");
		return;
	}

	cursor = allocation + PAGE_SIZE - 8;
	ret = rndis_build_page_buffers(cursor, data_len, page_bufs, &count);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto out;

	KUNIT_EXPECT_EQ(test, count, 2U);
	for (i = 0; i < count; i++) {
		struct page *page = vmalloc_to_page(cursor);
		unsigned long page_offset = offset_in_page(cursor);
		u32 offset, chunk;

		if (!page) {
			KUNIT_FAIL(test, "vmalloc test buffer has no backing page");
			goto out;
		}

		phys = page_to_phys(page) + page_offset;
		offset = offset_in_hvpage(phys);
		chunk = min_t(u32, remaining, PAGE_SIZE - page_offset);
		chunk = min_t(u32, chunk, HV_HYP_PAGE_SIZE - offset);

		KUNIT_EXPECT_EQ(test, page_bufs[i].pfn,
				(u64)(phys >> HV_HYP_PAGE_SHIFT));
		KUNIT_EXPECT_EQ(test, page_bufs[i].offset, offset);
		KUNIT_EXPECT_EQ(test, page_bufs[i].len, chunk);

		cursor += chunk;
		remaining -= chunk;
	}
	KUNIT_EXPECT_EQ(test, remaining, 0U);

out:
	vfree(allocation);
}

static struct kunit_case rndis_request_test_cases[] = {
	KUNIT_CASE(rndis_request_size_premise_test),
	KUNIT_CASE(rndis_request_alloc_zeroed_test),
	KUNIT_CASE(rndis_request_id_lifecycle_test),
	KUNIT_CASE(rndis_request_put_cleanup_test),
	KUNIT_CASE(rndis_request_vmalloc_page_buffers_test),
	{}
};

static struct kunit_suite rndis_request_test_suite = {
	.name = "hyperv-rndis-request",
	.test_cases = rndis_request_test_cases,
};

kunit_test_suite(rndis_request_test_suite);
