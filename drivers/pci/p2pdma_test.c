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

static void hmat_test_device_release(struct device *dev)
{
}

static void hmat_test_put_device(void *data)
{
	put_device(data);
}

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
	device_initialize(&pdev->dev);
	pdev->dev.bus = &pci_bus_type;
	pdev->dev.release = hmat_test_device_release;
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, hmat_test_put_device,
						  &pdev->dev),
			0);

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

struct hmat_rank_case {
	const char *desc;
	struct access_coordinate coord[2];
	enum pci_p2pdma_rank_type expected_type;
	u32 expected_bandwidth;
	u32 expected_latency;
	bool expected_latency_valid;
};

static const struct hmat_rank_case hmat_rank_cases[] = {
	{
		.desc = "bandwidth_and_latency",
		.coord = {
			{
				.read_bandwidth = 100,
				.write_bandwidth = 90,
				.read_latency = 7,
				.write_latency = 8,
			},
			{
				.read_bandwidth = 75,
				.write_bandwidth = 80,
				.read_latency = 10,
				.write_latency = 9,
			},
		},
		.expected_type = PCI_P2PDMA_RANK_HMAT_BANDWIDTH,
		.expected_bandwidth = 75,
		.expected_latency = 10,
		.expected_latency_valid = true,
	},
	{
		.desc = "bandwidth_only",
		.coord = {
			{
				.read_bandwidth = 100,
				.write_bandwidth = 90,
			},
			{
				.read_bandwidth = 75,
				.write_bandwidth = 80,
			},
		},
		.expected_type = PCI_P2PDMA_RANK_HMAT_BANDWIDTH,
		.expected_bandwidth = 75,
	},
	{
		.desc = "latency_only",
		.coord = {
			{
				.read_latency = 7,
				.write_latency = 8,
			},
			{
				.read_latency = 10,
				.write_latency = 9,
			},
		},
		.expected_type = PCI_P2PDMA_RANK_HMAT_LATENCY,
		.expected_latency = 10,
		.expected_latency_valid = true,
	},
	{
		.desc = "incomplete_coordinates",
		.coord = {
			{
				.read_bandwidth = 100,
				.write_bandwidth = 90,
				.read_latency = 7,
				.write_latency = 8,
			},
			{
				.read_bandwidth = 75,
				.read_latency = 10,
			},
		},
		.expected_type = PCI_P2PDMA_RANK_DISTANCE,
	},
};

static void hmat_rank_case_desc(const struct hmat_rank_case *c, char *desc)
{
	strscpy(desc, c->desc, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(hmat_rank, hmat_rank_cases, hmat_rank_case_desc);

struct hmat_rank_ctx {
	const struct hmat_rank_case *test_case;
	struct pci_dev *provider;
	struct pci_dev *client[2];
	int lookup_calls;
	bool unexpected_lookup;
};

static int pci_host_bridge_rank_pxm_stub(struct pci_dev *pdev)
{
	struct kunit *test = kunit_get_current_test();
	struct hmat_rank_ctx *ctx = test->priv;
	unsigned int i;

	if (pdev == ctx->provider)
		return 22;
	for (i = 0; i < ARRAY_SIZE(ctx->client); i++)
		if (pdev == ctx->client[i])
			return 11 + i;

	ctx->unexpected_lookup = true;
	return -ENODEV;
}

static int acpi_get_p2p_rank_coordinates_stub(int initiator, int target,
					      enum hmat_p2p_class class,
					      struct access_coordinate *coord)
{
	struct kunit *test = kunit_get_current_test();
	struct hmat_rank_ctx *ctx = test->priv;
	int index = initiator - 11;

	ctx->lookup_calls++;
	if (index < 0 || index >= (int)ARRAY_SIZE(ctx->client) || target != 22 ||
	    class != HMAT_P2P_NON_UIO) {
		ctx->unexpected_lookup = true;
		return -ENOENT;
	}

	*coord = ctx->test_case->coord[index];
	return 0;
}

static void pci_p2pdma_hmat_rank_many_test(struct kunit *test)
{
	const struct hmat_rank_case *test_case = test->param_value;
	struct hmat_rank_ctx ctx = { .test_case = test_case };
	struct device *clients[ARRAY_SIZE(ctx.client)];
	struct pci_p2pdma_rank rank;
	unsigned int i;

	ctx.provider = hmat_add_root_device(test, 0);
	for (i = 0; i < ARRAY_SIZE(ctx.client); i++) {
		ctx.client[i] = hmat_add_root_device(test, i + 1);
		clients[i] = &ctx.client[i]->dev;
	}
	test->priv = &ctx;

	kunit_activate_static_stub(test, cpu_supports_p2pdma,
				   cpu_supports_p2pdma_stub);
	kunit_activate_static_stub(test, pci_host_bridge_pxm,
				   pci_host_bridge_rank_pxm_stub);
	kunit_activate_static_stub(test, acpi_get_p2p_coordinates,
				   acpi_get_p2p_rank_coordinates_stub);

	KUNIT_ASSERT_EQ(test,
			pci_p2pdma_rank_many(ctx.provider, clients,
					       ARRAY_SIZE(clients), false, &rank),
			0);
	KUNIT_EXPECT_EQ(test, rank.type, test_case->expected_type);
	KUNIT_EXPECT_EQ(test, rank.distance, 4);
	KUNIT_EXPECT_EQ(test, ctx.lookup_calls, 2);
	KUNIT_EXPECT_FALSE(test, ctx.unexpected_lookup);
	if (rank.type == PCI_P2PDMA_RANK_HMAT_BANDWIDTH)
		KUNIT_EXPECT_EQ(test, rank.bandwidth,
				test_case->expected_bandwidth);
	if (rank.latency_valid)
		KUNIT_EXPECT_EQ(test, rank.latency,
				test_case->expected_latency);
	KUNIT_EXPECT_EQ(test, rank.latency_valid,
			test_case->expected_latency_valid);
}

static void pci_p2pdma_hmat_rank_compare_test(struct kunit *test)
{
	struct pci_p2pdma_rank direct = {
		.type = PCI_P2PDMA_RANK_DIRECT,
		.distance = 8,
	};
	struct pci_p2pdma_rank bandwidth = {
		.type = PCI_P2PDMA_RANK_HMAT_BANDWIDTH,
		.bandwidth = 100,
		.latency = 20,
		.distance = 4,
		.latency_valid = true,
	};
	struct pci_p2pdma_rank other = bandwidth;

	KUNIT_EXPECT_LT(test, pci_p2pdma_rank_cmp(&direct, &bandwidth), 0);

	other.bandwidth = 90;
	KUNIT_EXPECT_LT(test, pci_p2pdma_rank_cmp(&bandwidth, &other), 0);
	other = bandwidth;
	other.latency = 30;
	KUNIT_EXPECT_LT(test, pci_p2pdma_rank_cmp(&bandwidth, &other), 0);
	other = bandwidth;
	other.latency_valid = false;
	KUNIT_EXPECT_LT(test, pci_p2pdma_rank_cmp(&bandwidth, &other), 0);
	other = bandwidth;
	other.distance = 5;
	KUNIT_EXPECT_LT(test, pci_p2pdma_rank_cmp(&bandwidth, &other), 0);
	KUNIT_EXPECT_EQ(test, pci_p2pdma_rank_cmp(&bandwidth, &bandwidth), 0);

	bandwidth.type = PCI_P2PDMA_RANK_HMAT_LATENCY;
	other = bandwidth;
	other.type = PCI_P2PDMA_RANK_DISTANCE;
	KUNIT_EXPECT_LT(test, pci_p2pdma_rank_cmp(&bandwidth, &other), 0);
	other = bandwidth;
	other.latency = 30;
	KUNIT_EXPECT_LT(test, pci_p2pdma_rank_cmp(&bandwidth, &other), 0);

	bandwidth.type = PCI_P2PDMA_RANK_DISTANCE;
	bandwidth.distance = 4;
	other = bandwidth;
	other.distance = 5;
	KUNIT_EXPECT_LT(test, pci_p2pdma_rank_cmp(&bandwidth, &other), 0);
}

static void pci_p2pdma_direct_rank_test(struct kunit *test)
{
	struct pci_dev *provider = hmat_add_root_device(test, 0);
	struct device *clients[] = { &provider->dev };
	struct pci_p2pdma_rank rank;

	KUNIT_ASSERT_EQ(test,
			pci_p2pdma_rank_many(provider, clients,
					       ARRAY_SIZE(clients), false, &rank),
			0);
	KUNIT_EXPECT_EQ(test, rank.type, PCI_P2PDMA_RANK_DIRECT);
	KUNIT_EXPECT_EQ(test, rank.distance, 0);
}

static void pci_p2pdma_distance_rank_fallback_test(struct kunit *test)
{
	struct pci_dev *provider = hmat_add_root_device(test, 0);
	struct pci_dev *client = hmat_add_root_device(test, 1);
	struct device *clients[] = { &client->dev };
	struct pci_p2pdma_rank rank;

	kunit_activate_static_stub(test, cpu_supports_p2pdma,
				   cpu_supports_p2pdma_true_stub);

	KUNIT_ASSERT_EQ(test,
			pci_p2pdma_rank_many(provider, clients,
					       ARRAY_SIZE(clients), false, &rank),
			0);
	KUNIT_EXPECT_EQ(test, rank.type, PCI_P2PDMA_RANK_DISTANCE);
	KUNIT_EXPECT_EQ(test, rank.distance, 2);
}

static struct kunit_case pci_p2pdma_hmat_test_cases[] = {
	KUNIT_CASE_PARAM(pci_p2pdma_hmat_route_test, hmat_route_gen_params),
	KUNIT_CASE_PARAM(pci_p2pdma_hmat_rank_many_test, hmat_rank_gen_params),
	KUNIT_CASE(pci_p2pdma_hmat_rank_compare_test),
	KUNIT_CASE(pci_p2pdma_direct_rank_test),
	KUNIT_CASE(pci_p2pdma_distance_rank_fallback_test),
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
