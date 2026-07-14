// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit coverage for net/ceph/osd_client.c internals.
 *
 * Included from osd_client.c so the test can drive the real static sparse-read
 * parser and cursor-advance helper without exporting test-only symbols.
 */

#include <kunit/test.h>

struct ceph_osd_sparse_read_test {
	struct ceph_osd osd;
	struct ceph_connection con;
	struct ceph_msg *msg;
	struct ceph_msg_data_cursor cursor;
	struct ceph_osd_request *req;
	struct page **pages;
	struct page *page;
};

static int ceph_osd_sparse_read_test_init(struct kunit *test)
{
	struct ceph_osd_sparse_read_test *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);

	ctx->req = kunit_kzalloc(test, struct_size(ctx->req, r_ops, 1),
				 GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->req);

	ctx->pages = kunit_kcalloc(test, 1, sizeof(*ctx->pages), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->pages);

	ctx->page = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx->page);
	ctx->pages[0] = ctx->page;

	ctx->msg = ceph_msg_new2(CEPH_MSG_OSD_OPREPLY, 0, 1, GFP_KERNEL, true);
	KUNIT_ASSERT_NOT_NULL(test, ctx->msg);

	osd_init(&ctx->osd);
	ctx->osd.o_sparse_op_idx = -1;
	ceph_init_sparse_read(&ctx->osd.o_sparse_read);

	request_init(ctx->req);
	ctx->req->r_tid = 1;
	ctx->req->r_num_ops = 1;
	osd_req_op_extent_init(ctx->req, 0, CEPH_OSD_OP_SPARSE_READ,
			       0, PAGE_SIZE, 0, 0);
	KUNIT_ASSERT_EQ(test, ctx->req->r_ops[0].op, CEPH_OSD_OP_SPARSE_READ);

	insert_request(&ctx->osd.o_requests, ctx->req);

	ctx->msg->hdr.tid = cpu_to_le64(ctx->req->r_tid);
	ceph_msg_data_add_pages(ctx->msg, ctx->pages, PAGE_SIZE, 0, false);
	ceph_msg_data_cursor_init(&ctx->cursor, ctx->msg, PAGE_SIZE);

	ctx->con.private = &ctx->osd;
	ctx->con.in_msg = ctx->msg;

	test->priv = ctx;
	return 0;
}

static void ceph_osd_sparse_read_test_exit(struct kunit *test)
{
	struct ceph_osd_sparse_read_test *ctx = test->priv;

	if (!ctx)
		return;

	if (ctx->req && !RB_EMPTY_NODE(&ctx->req->r_node))
		erase_request(&ctx->osd.o_requests, ctx->req);
	ceph_init_sparse_read(&ctx->osd.o_sparse_read);
	if (ctx->msg)
		ceph_msg_put(ctx->msg);
	if (ctx->page)
		__free_page(ctx->page);
}

static int ceph_osd_sparse_read_feed_one_extent(struct kunit *test,
						u64 off, u64 len, u32 datalen)
{
	struct ceph_osd_sparse_read_test *ctx = test->priv;
	struct ceph_sparse_read *sr = &ctx->osd.o_sparse_read;
	char *buf = NULL;
	int ret;

	ret = osd_sparse_read(&ctx->con, &ctx->cursor, &buf);
	KUNIT_ASSERT_EQ(test, ret, (int)sizeof(sr->sr_count));
	KUNIT_ASSERT_PTR_EQ(test, buf, (char *)&sr->sr_count);

	sr->sr_count = (__force u32)cpu_to_le32(1);
	ret = osd_sparse_read(&ctx->con, &ctx->cursor, &buf);
	KUNIT_ASSERT_EQ(test, ret, (int)sizeof(*sr->sr_extent));
	KUNIT_ASSERT_NOT_NULL(test, sr->sr_extent);
	KUNIT_ASSERT_PTR_EQ(test, buf, (char *)sr->sr_extent);

	sr->sr_extent[0].off = (__force u64)cpu_to_le64(off);
	sr->sr_extent[0].len = (__force u64)cpu_to_le64(len);
	ret = osd_sparse_read(&ctx->con, &ctx->cursor, &buf);
	KUNIT_ASSERT_EQ(test, ret, (int)sizeof(sr->sr_datalen));
	KUNIT_ASSERT_PTR_EQ(test, buf, (char *)&sr->sr_datalen);

	sr->sr_datalen = (__force u32)cpu_to_le32(datalen);
	return osd_sparse_read(&ctx->con, &ctx->cursor, &buf);
}

static void ceph_osd_sparse_read_in_range_extent(struct kunit *test)
{
	struct ceph_osd_sparse_read_test *ctx = test->priv;
	struct ceph_sparse_read *sr = &ctx->osd.o_sparse_read;
	int ret;

	ret = ceph_osd_sparse_read_feed_one_extent(test, 0, 16, 16);

	KUNIT_EXPECT_EQ(test, ret, 16);
	KUNIT_EXPECT_EQ(test, sr->sr_pos, 16);
	KUNIT_EXPECT_EQ(test, ctx->cursor.sr_resid, 16);
	KUNIT_EXPECT_EQ(test, ctx->cursor.resid, PAGE_SIZE);
}

static void ceph_osd_sparse_read_rejects_out_of_range_extent(struct kunit *test)
{
	int ret;

	ret = ceph_osd_sparse_read_feed_one_extent(test, PAGE_SIZE + 16, 16, 16);

	KUNIT_EXPECT_EQ(test, ret, -EREMOTEIO);
}

static struct kunit_case ceph_osd_sparse_read_test_cases[] = {
	KUNIT_CASE(ceph_osd_sparse_read_in_range_extent),
	KUNIT_CASE(ceph_osd_sparse_read_rejects_out_of_range_extent),
	{}
};

static struct kunit_suite ceph_osd_sparse_read_test_suite = {
	.name = "ceph_osd_sparse_read",
	.init = ceph_osd_sparse_read_test_init,
	.exit = ceph_osd_sparse_read_test_exit,
	.test_cases = ceph_osd_sparse_read_test_cases,
};

kunit_test_suite(ceph_osd_sparse_read_test_suite);
