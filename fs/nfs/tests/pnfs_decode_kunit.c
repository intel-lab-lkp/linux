// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for pNFS data server address decoding.
 */

#include <kunit/test.h>
#include <linux/mm.h>
#include <linux/sunrpc/xdr.h>
#include <net/net_namespace.h>

#include "../pnfs.h"

static struct page *
pnfs_decode_make_stream(const char *netid, const char *addr,
			struct xdr_stream *xdr, struct xdr_buf *buf,
			struct page **pages)
{
	size_t netid_len = strlen(netid);
	size_t addr_len = strlen(addr);
	struct page *page;
	char *p;
	size_t offset = 0;

	if (XDR_UNIT * 2 + xdr_align_size(netid_len) +
	    xdr_align_size(addr_len) > PAGE_SIZE)
		return ERR_PTR(-E2BIG);

	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!page)
		return ERR_PTR(-ENOMEM);
	pages[0] = page;
	p = page_address(page);

	*(__be32 *)(p + offset) = cpu_to_be32(netid_len);
	offset += XDR_UNIT;
	memcpy(p + offset, netid, netid_len);
	offset += xdr_align_size(netid_len);
	*(__be32 *)(p + offset) = cpu_to_be32(addr_len);
	offset += XDR_UNIT;
	memcpy(p + offset, addr, addr_len);
	offset += xdr_align_size(addr_len);

	xdr_init_decode_pages(xdr, buf, pages, offset);
	return page;
}

static void pnfs_decode_free_addr(struct nfs4_pnfs_ds_addr *addr)
{
	kfree(addr->da_remotestr);
	kfree(addr->da_netid);
	kfree(addr);
}

static struct nfs4_pnfs_ds_addr *
pnfs_decode_addr(const char *netid, const char *addr)
{
	struct page *pages[1] = { NULL };
	struct xdr_buf buf = {};
	struct xdr_stream xdr;
	struct nfs4_pnfs_ds_addr *decoded;
	struct page *page;

	page = pnfs_decode_make_stream(netid, addr, &xdr, &buf, pages);
	if (IS_ERR(page))
		return ERR_CAST(page);

	decoded = nfs4_decode_mp_ds_addr(&init_net, &xdr, GFP_KERNEL);
	xdr_finish_decode(&xdr);
	__free_page(page);
	return decoded;
}

static void pnfs_decode_valid_ipv4(struct kunit *test)
{
	struct nfs4_pnfs_ds_addr *addr;
	struct sockaddr_in *sin;

	addr = pnfs_decode_addr("tcp", "10.0.0.4.8.1");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, addr);
	sin = (struct sockaddr_in *)&addr->da_addr;
	KUNIT_EXPECT_EQ(test, sin->sin_family, AF_INET);
	KUNIT_EXPECT_EQ(test, ntohs(sin->sin_port), 2049);
	KUNIT_EXPECT_EQ(test, be32_to_cpu(sin->sin_addr.s_addr), 0x0a000004);
	pnfs_decode_free_addr(addr);
}

static void pnfs_decode_valid_ipv4_max_port(struct kunit *test)
{
	struct nfs4_pnfs_ds_addr *addr;
	struct sockaddr_in *sin;

	addr = pnfs_decode_addr("tcp", "10.0.0.4.255.255");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, addr);
	sin = (struct sockaddr_in *)&addr->da_addr;
	KUNIT_EXPECT_EQ(test, ntohs(sin->sin_port), 65535);
	pnfs_decode_free_addr(addr);
}

static void pnfs_decode_valid_ipv4_zero_port(struct kunit *test)
{
	struct nfs4_pnfs_ds_addr *addr;
	struct sockaddr_in *sin;

	addr = pnfs_decode_addr("tcp", "10.0.0.4.0.0");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, addr);
	sin = (struct sockaddr_in *)&addr->da_addr;
	KUNIT_EXPECT_EQ(test, ntohs(sin->sin_port), 0);
	pnfs_decode_free_addr(addr);
}

static void pnfs_decode_valid_ipv6(struct kunit *test)
{
	struct nfs4_pnfs_ds_addr *addr;
	struct sockaddr_in6 *sin6;

	addr = pnfs_decode_addr("tcp6", "2001:db8::5.8.1");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, addr);
	sin6 = (struct sockaddr_in6 *)&addr->da_addr;
	KUNIT_EXPECT_EQ(test, sin6->sin6_family, AF_INET6);
	KUNIT_EXPECT_EQ(test, ntohs(sin6->sin6_port), 2049);
	pnfs_decode_free_addr(addr);
}

static void pnfs_decode_expect_invalid(struct kunit *test, const char *addr)
{
	struct nfs4_pnfs_ds_addr *decoded = pnfs_decode_addr("tcp", addr);

	KUNIT_ASSERT_FALSE(test, IS_ERR(decoded));
	KUNIT_EXPECT_NULL(test, decoded);
	if (decoded)
		pnfs_decode_free_addr(decoded);
}

static void pnfs_decode_reject_high_octet_over_255(struct kunit *test)
{
	pnfs_decode_expect_invalid(test, "10.0.0.4.300.1");
}

static void pnfs_decode_reject_low_octet_over_255(struct kunit *test)
{
	pnfs_decode_expect_invalid(test, "10.0.0.4.1.300");
}

static void pnfs_decode_reject_integer_overflow(struct kunit *test)
{
	pnfs_decode_expect_invalid(test, "10.0.0.4.999999999999999999999.1");
}

static void pnfs_decode_reject_empty_low_octet(struct kunit *test)
{
	pnfs_decode_expect_invalid(test, "10.0.0.4.8.");
}

static void pnfs_decode_reject_empty_high_octet(struct kunit *test)
{
	pnfs_decode_expect_invalid(test, "10.0.0.4..1");
}

static void pnfs_decode_reject_negative_octet(struct kunit *test)
{
	pnfs_decode_expect_invalid(test, "10.0.0.4.8.-1");
}

static void pnfs_decode_reject_leading_plus(struct kunit *test)
{
	pnfs_decode_expect_invalid(test, "10.0.0.4.+8.1");
}

static void pnfs_decode_reject_non_numeric_octet(struct kunit *test)
{
	pnfs_decode_expect_invalid(test, "10.0.0.4.8.x");
}

static void pnfs_decode_reject_trailing_garbage(struct kunit *test)
{
	pnfs_decode_expect_invalid(test, "10.0.0.4.8.1x");
}

static void pnfs_decode_reject_trailing_newline(struct kunit *test)
{
	pnfs_decode_expect_invalid(test, "10.0.0.4.8.1\n");
}

static void pnfs_decode_reject_bad_address(struct kunit *test)
{
	pnfs_decode_expect_invalid(test, "999.1.1.1.8.1");
}

static void pnfs_decode_reject_missing_port_octet(struct kunit *test)
{
	pnfs_decode_expect_invalid(test, "10.0.0.4.8");
}

static struct kunit_case pnfs_decode_test_cases[] = {
	KUNIT_CASE(pnfs_decode_valid_ipv4),
	KUNIT_CASE(pnfs_decode_valid_ipv4_max_port),
	KUNIT_CASE(pnfs_decode_valid_ipv4_zero_port),
	KUNIT_CASE(pnfs_decode_valid_ipv6),
	KUNIT_CASE(pnfs_decode_reject_high_octet_over_255),
	KUNIT_CASE(pnfs_decode_reject_low_octet_over_255),
	KUNIT_CASE(pnfs_decode_reject_integer_overflow),
	KUNIT_CASE(pnfs_decode_reject_empty_low_octet),
	KUNIT_CASE(pnfs_decode_reject_empty_high_octet),
	KUNIT_CASE(pnfs_decode_reject_negative_octet),
	KUNIT_CASE(pnfs_decode_reject_leading_plus),
	KUNIT_CASE(pnfs_decode_reject_non_numeric_octet),
	KUNIT_CASE(pnfs_decode_reject_trailing_garbage),
	KUNIT_CASE(pnfs_decode_reject_trailing_newline),
	KUNIT_CASE(pnfs_decode_reject_bad_address),
	KUNIT_CASE(pnfs_decode_reject_missing_port_octet),
	{}
};

static struct kunit_suite pnfs_decode_test_suite = {
	.name = "nfs_pnfs_decode",
	.test_cases = pnfs_decode_test_cases,
};

kunit_test_suite(pnfs_decode_test_suite);

MODULE_DESCRIPTION("KUnit tests for pNFS data server address decoding");
MODULE_LICENSE("GPL");
