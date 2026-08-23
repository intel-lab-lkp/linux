// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the NFS flexfiles GETDEVICEINFO decoder.
 */

#include <kunit/test.h>
#include <linux/mm.h>
#include <net/net_namespace.h>

#include "../flexfilelayout.h"

static void flexfile_deviceid_put_u32(char **p, u32 value)
{
	*(__be32 *)*p = cpu_to_be32(value);
	*p += XDR_UNIT;
}

static void flexfile_deviceid_put_string(char **p, const char *string)
{
	size_t len = strlen(string);

	flexfile_deviceid_put_u32(p, len);
	memcpy(*p, string, len);
	*p += xdr_align_size(len);
}

static struct nfs_server *flexfile_deviceid_server(struct kunit *test)
{
	struct nfs_server *server;
	struct nfs_client *client;

	server = kunit_kzalloc(test, sizeof(*server), GFP_KERNEL);
	if (!server)
		return NULL;
	client = kunit_kzalloc(test, sizeof(*client), GFP_KERNEL);
	if (!client)
		return NULL;

	client->cl_net = &init_net;
	client->cl_proto = IPPROTO_TCP;
	server->nfs_client = client;
	return server;
}

static struct pnfs_device *
flexfile_deviceid_pdev(struct kunit *test, struct page *page, size_t len)
{
	struct pnfs_device *pdev;

	pdev = kunit_kzalloc(test, sizeof(*pdev), GFP_KERNEL);
	if (!pdev)
		return NULL;
	pdev->pages = kunit_kzalloc(test, sizeof(*pdev->pages), GFP_KERNEL);
	if (!pdev->pages)
		return NULL;
	pdev->pages[0] = page;
	pdev->pglen = len;
	return pdev;
}

static void flexfile_deviceid_free_page(void *data)
{
	__free_page(data);
}

static struct page *flexfile_deviceid_page(struct kunit *test)
{
	struct page *page;

	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!page)
		return NULL;
	if (kunit_add_action_or_reset(test, flexfile_deviceid_free_page,
				      page))
		return NULL;
	return page;
}

static void flexfile_deviceid_free(void *data)
{
	nfs4_ff_layout_free_deviceid(data);
}

static int
flexfile_deviceid_track(struct kunit *test, struct nfs4_ff_layout_ds *device)
{
	if (!device)
		return 0;
	return kunit_add_action_or_reset(test, flexfile_deviceid_free, device);
}

static void flexfile_deviceid_reject_huge_multipath_count(struct kunit *test)
{
	struct nfs4_ff_layout_ds *device;
	struct nfs_server *server;
	struct pnfs_device *pdev;
	struct page *page;
	char *start, *p;

	server = flexfile_deviceid_server(test);
	KUNIT_ASSERT_NOT_NULL(test, server);
	page = flexfile_deviceid_page(test);
	KUNIT_ASSERT_NOT_NULL(test, page);
	start = page_address(page);
	p = start;
	flexfile_deviceid_put_u32(&p, U32_MAX);
	pdev = flexfile_deviceid_pdev(test, page, p - start);
	KUNIT_ASSERT_NOT_NULL(test, pdev);

	device = nfs4_ff_alloc_deviceid_node(server, pdev, GFP_KERNEL);
	KUNIT_ASSERT_EQ(test, flexfile_deviceid_track(test, device), 0);
	KUNIT_EXPECT_NULL(test, device);
}

static void flexfile_deviceid_reject_zero_multipath_count(struct kunit *test)
{
	struct nfs4_ff_layout_ds *device;
	struct nfs_server *server;
	struct pnfs_device *pdev;
	struct page *page;
	char *start, *p;

	server = flexfile_deviceid_server(test);
	KUNIT_ASSERT_NOT_NULL(test, server);
	page = flexfile_deviceid_page(test);
	KUNIT_ASSERT_NOT_NULL(test, page);
	start = page_address(page);
	p = start;
	flexfile_deviceid_put_u32(&p, 0);
	pdev = flexfile_deviceid_pdev(test, page, p - start);
	KUNIT_ASSERT_NOT_NULL(test, pdev);

	device = nfs4_ff_alloc_deviceid_node(server, pdev, GFP_KERNEL);
	KUNIT_ASSERT_EQ(test, flexfile_deviceid_track(test, device), 0);
	KUNIT_EXPECT_NULL(test, device);
}

static void flexfile_deviceid_decode_single_address(struct kunit *test)
{
	struct nfs4_ff_layout_ds *device;
	struct nfs_server *server;
	struct pnfs_device *pdev;
	struct page *page;
	char *start, *p;

	server = flexfile_deviceid_server(test);
	KUNIT_ASSERT_NOT_NULL(test, server);
	page = flexfile_deviceid_page(test);
	KUNIT_ASSERT_NOT_NULL(test, page);
	start = page_address(page);
	p = start;
	flexfile_deviceid_put_u32(&p, 1);
	flexfile_deviceid_put_string(&p, "tcp");
	flexfile_deviceid_put_string(&p, "10.0.0.9.8.1");
	flexfile_deviceid_put_u32(&p, 1);
	flexfile_deviceid_put_u32(&p, 4);
	flexfile_deviceid_put_u32(&p, 2);
	flexfile_deviceid_put_u32(&p, 1048576);
	flexfile_deviceid_put_u32(&p, 1048576);
	flexfile_deviceid_put_u32(&p, 0);
	pdev = flexfile_deviceid_pdev(test, page, p - start);
	KUNIT_ASSERT_NOT_NULL(test, pdev);

	device = nfs4_ff_alloc_deviceid_node(server, pdev, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, device);
	KUNIT_ASSERT_EQ(test, flexfile_deviceid_track(test, device), 0);
	KUNIT_EXPECT_EQ(test, device->ds_versions_cnt, (u32)1);
	KUNIT_EXPECT_EQ(test, device->ds_versions[0].version, (u32)4);
	KUNIT_EXPECT_EQ(test, device->ds_versions[0].minor_version, (u32)2);
}

static struct kunit_case flexfile_deviceid_test_cases[] = {
	KUNIT_CASE(flexfile_deviceid_reject_huge_multipath_count),
	KUNIT_CASE(flexfile_deviceid_reject_zero_multipath_count),
	KUNIT_CASE(flexfile_deviceid_decode_single_address),
	{}
};

static struct kunit_suite flexfile_deviceid_test_suite = {
	.name = "nfs_flexfile_deviceid",
	.test_cases = flexfile_deviceid_test_cases,
};

kunit_test_suite(flexfile_deviceid_test_suite);

MODULE_DESCRIPTION("KUnit tests for flexfiles GETDEVICEINFO decoding");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
