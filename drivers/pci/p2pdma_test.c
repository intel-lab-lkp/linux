// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for HMAT-described PCI peer-to-peer routing.
 */

#include <kunit/static_stub.h>
#include <kunit/test.h>

#include <linux/acpi.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pci-p2pdma.h>

#include "pci.h"

struct hmat_route_case {
	const char *desc;
	bool cpu_p2pdma;
	int client_pxm;
	int provider_pxm;
	int lookup_ret;
	int lookup_calls;
	enum pci_p2pdma_map_type expected;
};

static const struct hmat_route_case hmat_route_cases[] = {
	{
		.desc = "ordered_path",
		.client_pxm = 11,
		.provider_pxm = 22,
		.lookup_ret = 0,
		.lookup_calls = 1,
		.expected = PCI_P2PDMA_MAP_THRU_HOST_BRIDGE,
	},
	{
		.desc = "missing_path",
		.client_pxm = 11,
		.provider_pxm = 22,
		.lookup_ret = -ENOENT,
		.lookup_calls = 1,
		.expected = PCI_P2PDMA_MAP_NOT_SUPPORTED,
	},
	{
		.desc = "uio_only_path",
		.client_pxm = 11,
		.provider_pxm = 22,
		.lookup_ret = -ENODATA,
		.lookup_calls = 1,
		.expected = PCI_P2PDMA_MAP_NOT_SUPPORTED,
	},
	{
		.desc = "missing_client_pxm",
		.client_pxm = -ENODEV,
		.provider_pxm = 22,
		.lookup_ret = 0,
		.lookup_calls = 0,
		.expected = PCI_P2PDMA_MAP_NOT_SUPPORTED,
	},
	{
		.desc = "missing_provider_pxm",
		.client_pxm = 11,
		.provider_pxm = -ENODEV,
		.lookup_ret = 0,
		.lookup_calls = 0,
		.expected = PCI_P2PDMA_MAP_NOT_SUPPORTED,
	},
	{
		.desc = "platform_authorized",
		.cpu_p2pdma = true,
		.client_pxm = 11,
		.provider_pxm = 22,
		.lookup_calls = 0,
		.expected = PCI_P2PDMA_MAP_THRU_HOST_BRIDGE,
	},
};

static void hmat_route_case_desc(const struct hmat_route_case *c, char *desc)
{
	strscpy(desc, c->desc, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(hmat_route, hmat_route_cases, hmat_route_case_desc);

struct hmat_route_ctx {
	const struct hmat_route_case *test_case;
	struct pci_dev *provider;
	struct pci_dev *client;
	int lookup_calls;
	int lookup_initiator;
	int lookup_target;
	enum hmat_p2p_class lookup_class;
	bool unexpected_device;
};

static struct pci_dev *hmat_add_root_device(struct kunit *test, u8 busnr)
{
	struct pci_host_bridge *host;
	struct pci_bus *bus;
	struct pci_dev *pdev;

	host = kunit_kzalloc(test, sizeof(*host), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, host);
	bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, bus);
	pdev = kunit_kzalloc(test, sizeof(*pdev), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, pdev);

	bus->number = busnr;
	bus->bridge = &host->dev;
	INIT_LIST_HEAD(&bus->devices);
	host->bus = bus;

	pdev->bus = bus;
	pdev->devfn = PCI_DEVFN(0, 0);
	pdev->vendor = 0xffff;
	pdev->device = 0xffff;
	list_add_tail(&pdev->bus_list, &bus->devices);
	return pdev;
}

static bool cpu_supports_p2pdma_stub(void)
{
	return false;
}

static bool cpu_supports_p2pdma_true_stub(void)
{
	return true;
}

static int pci_host_bridge_pxm_stub(struct pci_dev *pdev)
{
	struct kunit *test = kunit_get_current_test();
	struct hmat_route_ctx *ctx = test->priv;

	if (pdev == ctx->client)
		return ctx->test_case->client_pxm;
	if (pdev == ctx->provider)
		return ctx->test_case->provider_pxm;

	ctx->unexpected_device = true;
	return -ENODEV;
}

static int acpi_get_p2p_coordinates_stub(int initiator, int target,
					 enum hmat_p2p_class class,
					 struct access_coordinate *coord)
{
	struct kunit *test = kunit_get_current_test();
	struct hmat_route_ctx *ctx = test->priv;

	ctx->lookup_calls++;
	ctx->lookup_initiator = initiator;
	ctx->lookup_target = target;
	ctx->lookup_class = class;
	if (!ctx->test_case->lookup_ret)
		coord->read_bandwidth = 1;
	return ctx->test_case->lookup_ret;
}

static void pci_p2pdma_hmat_route_test(struct kunit *test)
{
	const struct hmat_route_case *test_case = test->param_value;
	struct hmat_route_ctx ctx = { .test_case = test_case };
	enum pci_p2pdma_map_type map;
	int distance;

	ctx.provider = hmat_add_root_device(test, 0);
	ctx.client = hmat_add_root_device(test, 1);
	test->priv = &ctx;

	if (test_case->cpu_p2pdma)
		kunit_activate_static_stub(test, cpu_supports_p2pdma,
					   cpu_supports_p2pdma_true_stub);
	else
		kunit_activate_static_stub(test, cpu_supports_p2pdma,
					   cpu_supports_p2pdma_stub);
	kunit_activate_static_stub(test, pci_host_bridge_pxm,
				   pci_host_bridge_pxm_stub);
	kunit_activate_static_stub(test, acpi_get_p2p_coordinates,
				   acpi_get_p2p_coordinates_stub);

	map = calc_map_type_and_dist(ctx.provider, ctx.client, &distance, false);
	KUNIT_EXPECT_EQ(test, map, test_case->expected);
	KUNIT_EXPECT_EQ(test, distance, 2);
	KUNIT_EXPECT_EQ(test, ctx.lookup_calls, test_case->lookup_calls);
	KUNIT_EXPECT_FALSE(test, ctx.unexpected_device);
	if (ctx.lookup_calls) {
		KUNIT_EXPECT_EQ(test, ctx.lookup_initiator,
				test_case->client_pxm);
		KUNIT_EXPECT_EQ(test, ctx.lookup_target,
				test_case->provider_pxm);
		KUNIT_EXPECT_EQ(test, ctx.lookup_class, HMAT_P2P_NON_UIO);
	}
}

static struct kunit_case pci_p2pdma_hmat_test_cases[] = {
	KUNIT_CASE_PARAM(pci_p2pdma_hmat_route_test, hmat_route_gen_params),
	{}
};

static struct kunit_suite pci_p2pdma_hmat_test_suite = {
	.name = "pci_p2pdma_hmat",
	.test_cases = pci_p2pdma_hmat_test_cases,
};
kunit_test_suite(pci_p2pdma_hmat_test_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit tests for HMAT-described PCI P2P routing");
