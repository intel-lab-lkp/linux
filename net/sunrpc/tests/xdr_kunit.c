// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for encoding an RPC Reply into pages that begin at a
 * nonzero page_base, as an NFSD direct READ leaves them. The entry past
 * the last Reply page holds a canary in place of the NULL sentinel,
 * since a store through page_address(NULL) kills the test thread.
 */
#include <kunit/test.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/xdr.h>

#define XDR_TEST_NPAGES		4
#define XDR_TEST_HEAD_LEN	(XDR_UNIT * 3)
#define XDR_TEST_CANARY		0xa5
#define XDR_TEST_TRAILER	0xfeedface

struct xdr_test_fixture {
	/* [0] is the head, then the Reply pages, then the canary */
	struct page		*respages[XDR_TEST_NPAGES + 2];
	struct svc_rqst		*rqstp;
};

static const unsigned int xdr_test_bases[] = {
	0, 4, PAGE_SIZE - 4, PAGE_SIZE + 4,
};

static void xdr_test_base_desc(const unsigned int *base, char *desc)
{
	snprintf(desc, KUNIT_PARAM_DESC_SIZE, "page_base=%u", *base);
}

KUNIT_ARRAY_PARAM(xdr_test_base, xdr_test_bases, xdr_test_base_desc);

static void xdr_test_free_pages(void *data)
{
	struct xdr_test_fixture *f = data;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(f->respages); i++)
		if (f->respages[i])
			__free_page(f->respages[i]);
}

static struct page *xdr_test_canary(struct kunit *test)
{
	struct xdr_test_fixture *f = test->priv;

	return f->respages[XDR_TEST_NPAGES + 1];
}

/*
 * Mirror a direct READ: nfsd_direct_read() sets page_base before the
 * payload is reserved, and nfsd4_encode_readv() takes the pad off buflen
 * because len never counts it.
 */
static struct xdr_stream *xdr_test_setup(struct kunit *test,
					 unsigned int page_base)
{
	struct xdr_test_fixture *f;
	struct svc_rqst *rqstp;
	struct xdr_buf *buf;
	unsigned int i;
	__be32 *p;
	int ret;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f);
	ret = kunit_add_action_or_reset(test, xdr_test_free_pages, f);
	KUNIT_ASSERT_EQ(test, ret, 0);
	for (i = 0; i < ARRAY_SIZE(f->respages); i++) {
		f->respages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		KUNIT_ASSERT_NOT_NULL(test, f->respages[i]);
	}
	memset(page_address(f->respages[XDR_TEST_NPAGES + 1]),
	       XDR_TEST_CANARY, PAGE_SIZE);

	rqstp = kunit_kzalloc(test, sizeof(*rqstp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, rqstp);
	f->rqstp = rqstp;
	test->priv = f;

	rqstp->rq_page_end = &f->respages[XDR_TEST_NPAGES + 1];
	buf = &rqstp->rq_res;
	buf->head[0].iov_base = page_address(f->respages[0]);
	buf->pages = &f->respages[1];

	svcxdr_init_encode(rqstp);
	p = xdr_reserve_space(&rqstp->rq_res_stream, XDR_TEST_HEAD_LEN);
	KUNIT_ASSERT_NOT_NULL(test, p);
	memset(p, 0, XDR_TEST_HEAD_LEN);

	buf->page_base = page_base;
	ret = xdr_restrict_buflen(&rqstp->rq_res_stream,
				  PAGE_SIZE * (rqstp->rq_page_end - buf->pages) -
				  page_base);
	KUNIT_ASSERT_EQ(test, ret, 0);
	return &rqstp->rq_res_stream;
}

static void xdr_test_check_stream(struct kunit *test, struct xdr_stream *xdr)
{
	struct xdr_buf *buf = xdr->buf;

	KUNIT_EXPECT_EQ(test, (unsigned long)xdr->p % XDR_UNIT, 0UL);
	KUNIT_EXPECT_TRUE(test,
			  xdr->page_ptr <= buf->pages + XDR_TEST_NPAGES - 1);
	KUNIT_EXPECT_NULL(test, memchr_inv(page_address(xdr_test_canary(test)),
					   XDR_TEST_CANARY, PAGE_SIZE));
}

/*
 * The i >> 8 term keeps a shift by whole pages from reading back right.
 * The i >> 16 term does the same on a 64 KiB page kernel, where the
 * first two terms repeat every page.
 */
static u8 xdr_test_pattern(unsigned int i)
{
	return (u8)(i * 7 + (i >> 8) + (i >> 16) + 1);
}

/* Pattern each byte by its logical offset so misplacement reads back wrong */
static void xdr_test_fill_payload(struct xdr_buf *buf, unsigned int start,
				  unsigned int count)
{
	unsigned int i, off;

	for (i = start; i < start + count; i++) {
		off = buf->page_base + i;
		((u8 *)page_address(buf->pages[off >> PAGE_SHIFT]))
			[offset_in_page(off)] = xdr_test_pattern(i);
	}
}

/* Reserve a payload whose bytes are already in the pages, then pad it */
static void xdr_test_reserve_payload(struct kunit *test, struct xdr_stream *xdr,
				     unsigned int start, unsigned int count)
{
	__be32 zero = xdr_zero;
	int ret;

	xdr_test_fill_payload(xdr->buf, start, count);
	KUNIT_ASSERT_EQ(test, xdr_reserve_space_vec(xdr, count), 0);
	ret = write_bytes_to_xdr_buf(xdr->buf, XDR_TEST_HEAD_LEN + start + count,
				     &zero, xdr_pad_size(count));
	KUNIT_ASSERT_EQ(test, ret, 0);
	xdr_test_check_stream(test, xdr);
}

/* The payload reads back by logical offset and its pad reads back as zero */
static void xdr_test_check_payload(struct kunit *test, struct xdr_buf *buf,
				   unsigned int start, unsigned int count)
{
	unsigned int end = xdr_align_size(start + count);
	unsigned int i;
	u8 *data;
	int ret;

	data = kunit_kmalloc(test, end - start, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, data);
	ret = read_bytes_from_xdr_buf(buf, XDR_TEST_HEAD_LEN + start, data,
				      end - start);
	KUNIT_ASSERT_EQ(test, ret, 0);
	for (i = 0; i < count; i++)
		if (data[i] != xdr_test_pattern(start + i))
			break;
	KUNIT_EXPECT_EQ(test, i, count);
	for (; i < end - start; i++)
		KUNIT_EXPECT_EQ(test, data[i], 0);
}

/*
 * One word per reserve, as an operation's result is encoded. At
 * page_base = PAGE_SIZE - 4 the second word steps to the next page.
 */
static void xdr_test_encode_two_words(struct kunit *test,
				      struct xdr_stream *xdr)
{
	__be32 *p;

	p = xdr_reserve_space(xdr, XDR_UNIT);
	KUNIT_ASSERT_NOT_NULL(test, p);
	*p = cpu_to_be32(1);
	p = xdr_reserve_space(xdr, XDR_UNIT);
	KUNIT_ASSERT_NOT_NULL(test, p);
	*p = cpu_to_be32(XDR_TEST_TRAILER);
	xdr_commit_encode(xdr);
	xdr_test_check_stream(test, xdr);
}

static void xdr_test_check_two_words(struct kunit *test, struct xdr_buf *buf,
				     unsigned int offset)
{
	__be32 words[2];
	int ret;

	ret = read_bytes_from_xdr_buf(buf, offset, words, sizeof(words));
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, be32_to_cpu(words[0]), 1);
	KUNIT_EXPECT_EQ(test, be32_to_cpu(words[1]), XDR_TEST_TRAILER);
}

static unsigned int xdr_test_room(unsigned int base)
{
	return XDR_TEST_NPAGES * PAGE_SIZE - XDR_TEST_HEAD_LEN - base;
}

/* A padded payload, then two more words; all read back at logical offsets */
static void xdr_test_payload(struct kunit *test)
{
	unsigned int base = *(const unsigned int *)test->param_value;
	unsigned int count = 2 * PAGE_SIZE + 5;
	struct xdr_stream *xdr;

	xdr = xdr_test_setup(test, base);
	xdr_test_reserve_payload(test, xdr, 0, count);
	KUNIT_EXPECT_EQ(test, xdr->buf->page_len, xdr_align_size(count));
	xdr_test_encode_two_words(test, xdr);
	xdr_test_check_payload(test, xdr->buf, 0, count);
	xdr_test_check_two_words(test, xdr->buf,
				 XDR_TEST_HEAD_LEN + xdr_align_size(count));
}

/* A payload that fills the Reply pages exactly leaves no more room */
static void xdr_test_exact_fit(struct kunit *test)
{
	unsigned int base = *(const unsigned int *)test->param_value;
	unsigned int room = xdr_test_room(base);
	struct xdr_stream *xdr;

	xdr = xdr_test_setup(test, base);
	KUNIT_ASSERT_EQ(test, xdr_reserve_space_vec(xdr, room), 0);
	xdr_test_check_stream(test, xdr);
	KUNIT_EXPECT_EQ(test, xdr->buf->page_len, room);
	KUNIT_EXPECT_NULL(test, xdr_reserve_space(xdr, XDR_UNIT));
	xdr_test_check_stream(test, xdr);
}

/* One byte more than the room, rounded to a word, is refused */
static void xdr_test_one_over(struct kunit *test)
{
	unsigned int base = *(const unsigned int *)test->param_value;
	unsigned int room = xdr_test_room(base);
	struct xdr_stream *xdr;

	xdr = xdr_test_setup(test, base);
	KUNIT_EXPECT_EQ(test, xdr_reserve_space_vec(xdr, room + 1), -EMSGSIZE);
	xdr_test_check_stream(test, xdr);
}

/* After a zero-byte xdr_reserve_space_vec(), the next reserve enters pages */
static void xdr_test_zero_vec(struct kunit *test)
{
	unsigned int base = *(const unsigned int *)test->param_value;
	struct xdr_stream *xdr;

	xdr = xdr_test_setup(test, base);
	KUNIT_ASSERT_EQ(test, xdr_reserve_space_vec(xdr, 0), 0);
	KUNIT_EXPECT_NULL(test, xdr->iov);

	xdr_test_encode_two_words(test, xdr);
	xdr_test_check_two_words(test, xdr->buf, XDR_TEST_HEAD_LEN);
}

/* A reserve entering the pages has to fit in the rest of the entry page */
static void xdr_test_entry_too_large(struct kunit *test)
{
	struct xdr_stream *xdr;

	xdr = xdr_test_setup(test, PAGE_SIZE - XDR_UNIT);
	KUNIT_ASSERT_EQ(test, xdr_reserve_space_vec(xdr, 0), 0);
	KUNIT_EXPECT_NULL(test, xdr_reserve_space(xdr, 2 * XDR_UNIT));
	KUNIT_EXPECT_EQ(test, xdr->buf->page_len, 0);
	xdr_test_check_stream(test, xdr);
}

/* Truncating back to the head clears page_base along with the payload */
static void xdr_test_truncate(struct kunit *test)
{
	unsigned int base = *(const unsigned int *)test->param_value;
	struct xdr_stream *xdr;
	struct xdr_buf *buf;

	xdr = xdr_test_setup(test, base);
	buf = xdr->buf;
	KUNIT_ASSERT_EQ(test, xdr_reserve_space_vec(xdr, PAGE_SIZE + 8), 0);

	xdr_truncate_encode(xdr, XDR_TEST_HEAD_LEN);
	KUNIT_EXPECT_EQ(test, buf->page_base, 0);
	KUNIT_EXPECT_EQ(test, buf->page_len, 0);
	KUNIT_EXPECT_EQ(test, buf->len, XDR_TEST_HEAD_LEN);
	KUNIT_EXPECT_PTR_EQ(test, xdr->page_ptr, buf->pages - 1);
	KUNIT_EXPECT_PTR_EQ(test, xdr->iov, &buf->head[0]);
	KUNIT_EXPECT_PTR_EQ(test, (void *)xdr->p,
			    buf->head[0].iov_base + XDR_TEST_HEAD_LEN);
	xdr_test_check_stream(test, xdr);
}

/* A server stream does not enter the pages at an unaligned page_base */
static void xdr_test_unaligned_base(struct kunit *test)
{
	struct xdr_stream *xdr;
	int ret = 0;

	xdr = xdr_test_setup(test, 1);
	kunit_warning_suppress(test) {
		ret = xdr_reserve_space_vec(xdr, 16);
	}
	KUNIT_EXPECT_EQ(test, ret, -EMSGSIZE);
	KUNIT_EXPECT_EQ(test, xdr->buf->page_len, 0);
	xdr_test_check_stream(test, xdr);
}

/*
 * A second payload at page_base + page_len, where a second READ in one
 * COMPOUND lands. Both payloads, and the words encoded after the second,
 * read back at logical offsets.
 */
static void xdr_test_second_payload(struct kunit *test)
{
	unsigned int base = *(const unsigned int *)test->param_value;
	unsigned int count1 = PAGE_SIZE + 5, count2 = PAGE_SIZE + 7;
	unsigned int start2 = xdr_align_size(count1);
	unsigned int end2 = start2 + xdr_align_size(count2);
	struct xdr_stream *xdr;

	xdr = xdr_test_setup(test, base);
	xdr_test_reserve_payload(test, xdr, 0, count1);
	xdr_test_reserve_payload(test, xdr, start2, count2);
	KUNIT_EXPECT_EQ(test, xdr->buf->page_len, end2);
	xdr_test_encode_two_words(test, xdr);
	xdr_test_check_payload(test, xdr->buf, 0, count1);
	xdr_test_check_payload(test, xdr->buf, start2, count2);
	xdr_test_check_two_words(test, xdr->buf, XDR_TEST_HEAD_LEN + end2);
}

/*
 * A reserve straddling the head's end is refused: the fragment would
 * be copied back to page offset 0, not to page_base.
 */
static void xdr_test_straddle(struct kunit *test)
{
	struct xdr_stream *xdr;
	__be32 *p;

	xdr = xdr_test_setup(test, XDR_UNIT);
	p = xdr_reserve_space(xdr, PAGE_SIZE - XDR_TEST_HEAD_LEN - XDR_UNIT);
	KUNIT_ASSERT_NOT_NULL(test, p);
	KUNIT_ASSERT_PTR_EQ(test, xdr->iov, &xdr->buf->head[0]);

	kunit_warning_suppress(test) {
		p = xdr_reserve_space(xdr, 2 * XDR_UNIT);
	}
	KUNIT_EXPECT_NULL(test, p);
	KUNIT_EXPECT_EQ(test, xdr->buf->page_len, 0);
	xdr_test_check_stream(test, xdr);
}

/* Set the auth slack aside as svcauth_gss_accept() does for krb5i */
static void xdr_test_set_slack(struct kunit *test, struct xdr_stream *xdr)
{
	struct xdr_test_fixture *f = test->priv;
	unsigned int buflen = xdr->buf->buflen;
	__be32 *end = xdr->end;

	svcxdr_set_auth_slack(f->rqstp, RPC_MAX_AUTH_SIZE);
	KUNIT_EXPECT_EQ(test, xdr->buf->buflen, buflen - RPC_MAX_AUTH_SIZE);
	KUNIT_EXPECT_PTR_EQ(test, xdr->end, end - XDR_QUADLEN(RPC_MAX_AUTH_SIZE));
}

/* Restoring the slack while still in the head gives the head its end back */
static void xdr_test_restore_slack_head(struct kunit *test)
{
	struct xdr_test_fixture *f;
	struct xdr_stream *xdr;
	unsigned int buflen;
	__be32 *end;

	xdr = xdr_test_setup(test, 0);
	f = test->priv;
	buflen = xdr->buf->buflen;
	end = xdr->end;

	xdr_test_set_slack(test, xdr);
	svcxdr_restore_auth_slack(f->rqstp);
	KUNIT_EXPECT_EQ(test, xdr->buf->buflen, buflen);
	KUNIT_EXPECT_PTR_EQ(test, xdr->end, end);
	KUNIT_EXPECT_PTR_EQ(test, xdr->iov, &xdr->buf->head[0]);
}

/*
 * After a zero-byte xdr_reserve_space_vec(), the restore leaves end at p
 * so the next reserve enters the pages.
 */
static void xdr_test_restore_slack_zero_vec(struct kunit *test)
{
	unsigned int base = *(const unsigned int *)test->param_value;
	struct xdr_test_fixture *f;
	struct xdr_stream *xdr;
	unsigned int buflen;

	xdr = xdr_test_setup(test, base);
	f = test->priv;
	buflen = xdr->buf->buflen;

	xdr_test_set_slack(test, xdr);
	KUNIT_ASSERT_EQ(test, xdr_reserve_space_vec(xdr, 0), 0);
	KUNIT_EXPECT_NULL(test, xdr->iov);
	KUNIT_EXPECT_PTR_EQ(test, xdr->end, xdr->p);

	svcxdr_restore_auth_slack(f->rqstp);
	KUNIT_EXPECT_EQ(test, xdr->buf->buflen, buflen);
	KUNIT_EXPECT_PTR_EQ(test, xdr->end, xdr->p);

	xdr_test_encode_two_words(test, xdr);
	xdr_test_check_two_words(test, xdr->buf, XDR_TEST_HEAD_LEN);
}

/*
 * A payload that fills the reduced buflen ends mid-page. The restore
 * re-seats end so exactly the slack fits after the payload.
 */
static void xdr_test_restore_slack_pages(struct kunit *test)
{
	unsigned int base = *(const unsigned int *)test->param_value;
	unsigned int room = XDR_TEST_NPAGES * PAGE_SIZE -
			    XDR_TEST_HEAD_LEN - base - RPC_MAX_AUTH_SIZE;
	struct xdr_test_fixture *f;
	struct xdr_stream *xdr;

	xdr = xdr_test_setup(test, base);
	f = test->priv;

	xdr_test_set_slack(test, xdr);
	KUNIT_ASSERT_EQ(test, xdr_reserve_space_vec(xdr, room), 0);
	KUNIT_EXPECT_NULL(test, xdr_reserve_space(xdr, XDR_UNIT));
	xdr_test_check_stream(test, xdr);

	svcxdr_restore_auth_slack(f->rqstp);
	KUNIT_EXPECT_EQ(test, xdr_reserve_space_vec(xdr, RPC_MAX_AUTH_SIZE), 0);
	KUNIT_EXPECT_EQ(test, xdr->buf->page_len, room + RPC_MAX_AUTH_SIZE);
	KUNIT_EXPECT_EQ(test, xdr->buf->len, xdr->buf->buflen);
	KUNIT_EXPECT_NULL(test, xdr_reserve_space(xdr, XDR_UNIT));
	xdr_test_check_stream(test, xdr);
}

/*
 * A payload ending on a page boundary leaves p in the page it fills.
 * The restore caps end at that page, so the next reserve steps
 * page_ptr forward rather than writing past the page.
 */
static void xdr_test_restore_slack_page_boundary(struct kunit *test)
{
	unsigned int base = *(const unsigned int *)test->param_value;
	unsigned int count = PAGE_SIZE - offset_in_page(base);
	struct xdr_test_fixture *f;
	struct xdr_stream *xdr;
	struct page **page_ptr;

	xdr = xdr_test_setup(test, base);
	f = test->priv;

	xdr_test_set_slack(test, xdr);
	KUNIT_ASSERT_EQ(test, xdr_reserve_space_vec(xdr, count), 0);
	KUNIT_EXPECT_EQ(test, offset_in_page(xdr->p), 0);
	page_ptr = xdr->page_ptr;

	svcxdr_restore_auth_slack(f->rqstp);
	KUNIT_EXPECT_PTR_EQ(test, xdr->end, xdr->p);

	xdr_test_encode_two_words(test, xdr);
	KUNIT_EXPECT_PTR_EQ(test, xdr->page_ptr, page_ptr + 1);
	xdr_test_check_two_words(test, xdr->buf, XDR_TEST_HEAD_LEN + count);
}

static struct kunit_case xdr_test_cases[] = {
	KUNIT_CASE_PARAM(xdr_test_payload, xdr_test_base_gen_params),
	KUNIT_CASE_PARAM(xdr_test_exact_fit, xdr_test_base_gen_params),
	KUNIT_CASE_PARAM(xdr_test_one_over, xdr_test_base_gen_params),
	KUNIT_CASE_PARAM(xdr_test_zero_vec, xdr_test_base_gen_params),
	KUNIT_CASE(xdr_test_entry_too_large),
	KUNIT_CASE_PARAM(xdr_test_truncate, xdr_test_base_gen_params),
	KUNIT_CASE(xdr_test_unaligned_base),
	KUNIT_CASE_PARAM(xdr_test_second_payload, xdr_test_base_gen_params),
	KUNIT_CASE(xdr_test_straddle),
	KUNIT_CASE(xdr_test_restore_slack_head),
	KUNIT_CASE_PARAM(xdr_test_restore_slack_zero_vec,
			 xdr_test_base_gen_params),
	KUNIT_CASE_PARAM(xdr_test_restore_slack_pages,
			 xdr_test_base_gen_params),
	KUNIT_CASE_PARAM(xdr_test_restore_slack_page_boundary,
			 xdr_test_base_gen_params),
	{}
};

static struct kunit_suite xdr_test_suite = {
	.name = "sunrpc_xdr",
	.test_cases = xdr_test_cases,
};

kunit_test_suite(xdr_test_suite);

MODULE_DESCRIPTION("KUnit tests for the SunRPC XDR encoder");
MODULE_LICENSE("GPL");
