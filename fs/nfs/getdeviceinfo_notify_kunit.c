// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit coverage for decode_getdeviceinfo()'s notification-bitmap length.
 * Drives the real static decoder over a crafted reply: with len = 0x40000000
 * the u32 "4 * len" wraps to 0, defeating the bounds check, and the verify
 * loop reads past the buffer. The length word sits at the page edge so the
 * first over-read word hits the KASAN redzone. Level 2; from nfs4xdr.c.
 */
#include <kunit/test.h>

#define GDI_OPNUM	47U		/* OP_GETDEVICEINFO */
#define GDI_LAYOUT_TYPE	1U		/* arbitrary; pdev->layout_type matches */

/* Fixed fields in the XDR head, notification bitmap in the tail ending at @tail_end. */
static int run_decode(struct kunit *test, __be32 *tail_end, u32 notify_len,
		      u32 notify_word0)
{
	struct pnfs_device pdev = { .layout_type = GDI_LAYOUT_TYPE };
	struct nfs4_getdeviceinfo_res res = { .pdev = &pdev };
	struct xdr_stream xdr;
	struct xdr_buf buf;
	__be32 head[4];
	unsigned int tail_words = notify_len <= 2 ? notify_len + 1 : 1;
	__be32 *tail = tail_end - tail_words;

	head[0] = cpu_to_be32(GDI_OPNUM);	/* op_hdr: opnum    */
	head[1] = cpu_to_be32(0);		/* op_hdr: NFS_OK   */
	head[2] = cpu_to_be32(GDI_LAYOUT_TYPE);	/* device type      */
	head[3] = cpu_to_be32(0);		/* mincount = 0     */
	tail[0] = cpu_to_be32(notify_len);	/* notification len */
	if (notify_len == 1) {
		tail[1] = cpu_to_be32(notify_word0);
	} else if (notify_len == 2) {
		tail[1] = cpu_to_be32(notify_word0);
		tail[2] = cpu_to_be32(1);
	}

	memset(&buf, 0, sizeof(buf));
	buf.head[0].iov_base = head;
	buf.head[0].iov_len = sizeof(head);
	buf.tail[0].iov_base = tail;
	buf.tail[0].iov_len = tail_words * sizeof(*tail);
	buf.len = buf.head[0].iov_len + buf.tail[0].iov_len;
	buf.buflen = buf.len;
	xdr_init_decode(&xdr, &buf, head, NULL);
	return decode_getdeviceinfo(&xdr, &res);
}

/* Control: one-word bitmap (len 1, word 0) decodes cleanly; PASS stock+patched. */
static void getdeviceinfo_notify_control_len1(struct kunit *test)
{
	__be32 *obj = kmalloc(PAGE_SIZE, GFP_KERNEL);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, obj);
	/* Place reply mid-buffer; nothing reads past it. */
	ret = run_decode(test, obj + 32, 1, 0);
	KUNIT_EXPECT_EQ(test, ret, 0);
	kfree(obj);
}

/* Control: len 2, nonzero unsupported word -> -EIO in bounds; PASS stock+patched. */
static void getdeviceinfo_notify_control_unsupported_len2(struct kunit *test)
{
	__be32 *obj = kmalloc(PAGE_SIZE, GFP_KERNEL);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, obj);
	ret = run_decode(test, obj + 32, 2, 0);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	kfree(obj);
}

/* Trigger: wrapping len 0x40000000 at the page edge -> KASAN OOB on stock, -EIO patched. */
static void getdeviceinfo_notify_trigger_oob(struct kunit *test)
{
	__be32 *obj = kmalloc(PAGE_SIZE, GFP_KERNEL);
	__be32 *obj_end = (__be32 *)((char *)obj + PAGE_SIZE);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, obj);
	ret = run_decode(test, obj_end, 0x40000000U, 0);
	/* Reached only on the patched tree. */
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	kfree(obj);
}

static struct kunit_case getdeviceinfo_notify_cases[] = {
	KUNIT_CASE(getdeviceinfo_notify_control_len1),
	KUNIT_CASE(getdeviceinfo_notify_control_unsupported_len2),
	KUNIT_CASE(getdeviceinfo_notify_trigger_oob),
	{}
};

static struct kunit_suite getdeviceinfo_notify_suite = {
	.name = "nfs4_getdeviceinfo_notify",
	.test_cases = getdeviceinfo_notify_cases,
};

kunit_test_suite(getdeviceinfo_notify_suite);
