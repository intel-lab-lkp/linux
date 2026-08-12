// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for ACPI HMAT PCIe peer-to-peer matrices.
 */

#include <kunit/test.h>

#include <linux/acpi.h>
#include <linux/init.h>
#include <linux/module.h>

#include "hmat_test.h"

struct hmat_test_ctx {
	struct list_head localities;
};

static int __init hmat_test_init(struct kunit *test)
{
	struct hmat_test_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	INIT_LIST_HEAD(&ctx->localities);
	test->priv = ctx;
	return 0;
}

static void __init hmat_test_exit(struct kunit *test)
{
	struct hmat_test_ctx *ctx = test->priv;

	hmat_free_p2p_localities(&ctx->localities);
}

static struct acpi_hmat_p2p_latency * __init
hmat_test_alloc_table(struct kunit *test, u32 initiators, u32 targets)
{
	struct acpi_hmat_p2p_latency *p2p;
	size_t length;

	length = sizeof(*p2p) + sizeof(u32) * (initiators + targets) +
		 sizeof(u16) * initiators * targets;
	p2p = kunit_kzalloc(test, length, GFP_KERNEL);
	if (!p2p)
		return NULL;

	p2p->header.type = ACPI_HMAT_TYPE_P2P_LATENCY;
	p2p->header.length = length;
	p2p->number_of_initiator_Pds = initiators;
	p2p->number_of_target_Pds = targets;
	return p2p;
}

static u32 * __init hmat_test_initiators(struct acpi_hmat_p2p_latency *p2p)
{
	return (u32 *)(p2p + 1);
}

static u32 * __init hmat_test_targets(struct acpi_hmat_p2p_latency *p2p)
{
	return hmat_test_initiators(p2p) + p2p->number_of_initiator_Pds;
}

static u16 * __init hmat_test_entries(struct acpi_hmat_p2p_latency *p2p)
{
	return (u16 *)(hmat_test_targets(p2p) + p2p->number_of_target_Pds);
}

static void __init
hmat_test_expect_coord(struct kunit *test, int initiator, int target,
		       enum hmat_p2p_class class, u32 read_bandwidth,
		       u32 write_bandwidth, u32 read_latency,
		       u32 write_latency)
{
	struct hmat_test_ctx *ctx = test->priv;
	struct access_coordinate coord;

	KUNIT_ASSERT_EQ(test,
			hmat_get_p2p_coordinates(&ctx->localities, initiator,
						 target, class, &coord),
			0);
	KUNIT_EXPECT_EQ(test, coord.read_bandwidth, read_bandwidth);
	KUNIT_EXPECT_EQ(test, coord.write_bandwidth, write_bandwidth);
	KUNIT_EXPECT_EQ(test, coord.read_latency, read_latency);
	KUNIT_EXPECT_EQ(test, coord.write_latency, write_latency);
}

static void __init hmat_p2p_matrix_test(struct kunit *test)
{
	struct hmat_test_ctx *ctx = test->priv;
	struct acpi_hmat_p2p_latency *p2p;
	struct access_coordinate coord;
	u32 *initiators, *targets;
	u16 *entries;

	p2p = hmat_test_alloc_table(test, 2, 2);
	KUNIT_ASSERT_NOT_NULL(test, p2p);
	p2p->flags = ACPI_HMAT_P2P_NON_UIO | ACPI_HMAT_P2P_UIO;
	p2p->data_type = ACPI_HMAT_ACCESS_LATENCY;
	p2p->entry_base_unit = 1000;

	initiators = hmat_test_initiators(p2p);
	targets = hmat_test_targets(p2p);
	entries = hmat_test_entries(p2p);
	initiators[0] = 11;
	initiators[1] = 22;
	targets[0] = 33;
	targets[1] = 44;
	entries[0] = 5;
	entries[1] = 10;
	entries[2] = 0xffff;
	entries[3] = 20;

	KUNIT_ASSERT_EQ(test, hmat_parse_p2p(p2p, 2, &ctx->localities), 0);
	hmat_test_expect_coord(test, 11, 33, HMAT_P2P_NON_UIO, 0, 0, 5, 5);
	hmat_test_expect_coord(test, 11, 44, HMAT_P2P_UIO, 0, 0, 10, 10);
	hmat_test_expect_coord(test, 22, 44, HMAT_P2P_NON_UIO, 0, 0, 20, 20);

	KUNIT_EXPECT_EQ(test,
			hmat_get_p2p_coordinates(&ctx->localities, 22, 33,
						 HMAT_P2P_NON_UIO, &coord),
			-ENODATA);
	KUNIT_EXPECT_EQ(test,
			hmat_get_p2p_coordinates(&ctx->localities, 33, 11,
						 HMAT_P2P_NON_UIO, &coord),
			-ENOENT);
}

static int __init hmat_test_add_coordinate(struct kunit *test, u8 flags,
					   u8 type, u64 base, u16 entry)
{
	struct hmat_test_ctx *ctx = test->priv;
	struct acpi_hmat_p2p_latency *p2p;

	p2p = hmat_test_alloc_table(test, 1, 1);
	if (!p2p)
		return -ENOMEM;

	p2p->flags = flags;
	p2p->data_type = type;
	p2p->entry_base_unit = base;
	hmat_test_initiators(p2p)[0] = 1;
	hmat_test_targets(p2p)[0] = 2;
	hmat_test_entries(p2p)[0] = entry;
	return hmat_parse_p2p(p2p, 2, &ctx->localities);
}

static void __init hmat_p2p_coordinate_merge_test(struct kunit *test)
{
	KUNIT_ASSERT_EQ(test,
			hmat_test_add_coordinate(test, ACPI_HMAT_P2P_NON_UIO,
				ACPI_HMAT_ACCESS_BANDWIDTH, 1, 400),
			0);
	KUNIT_ASSERT_EQ(test,
			hmat_test_add_coordinate(test, ACPI_HMAT_P2P_NON_UIO,
				ACPI_HMAT_READ_LATENCY, 1000, 7),
			0);
	KUNIT_ASSERT_EQ(test,
			hmat_test_add_coordinate(test, ACPI_HMAT_P2P_NON_UIO,
				ACPI_HMAT_WRITE_LATENCY, 1000, 9),
			0);
	KUNIT_ASSERT_EQ(test,
			hmat_test_add_coordinate(test, ACPI_HMAT_P2P_UIO,
				ACPI_HMAT_READ_BANDWIDTH, 1, 200),
			0);
	KUNIT_ASSERT_EQ(test,
			hmat_test_add_coordinate(test, ACPI_HMAT_P2P_UIO,
				ACPI_HMAT_WRITE_BANDWIDTH, 1, 300),
			0);
	KUNIT_ASSERT_EQ(test,
			hmat_test_add_coordinate(test, ACPI_HMAT_P2P_UIO,
				ACPI_HMAT_ACCESS_LATENCY, 1000, 11),
			0);

	hmat_test_expect_coord(test, 1, 2, HMAT_P2P_NON_UIO,
			       400, 400, 7, 9);
	hmat_test_expect_coord(test, 1, 2, HMAT_P2P_UIO,
			       200, 300, 11, 11);
}

static void __init hmat_p2p_invalid_length_test(struct kunit *test)
{
	struct hmat_test_ctx *ctx = test->priv;
	struct acpi_hmat_p2p_latency *p2p;
	struct access_coordinate coord;

	p2p = hmat_test_alloc_table(test, 1, 1);
	KUNIT_ASSERT_NOT_NULL(test, p2p);
	p2p->header.length = sizeof(*p2p) - 1;
	KUNIT_EXPECT_EQ(test, hmat_parse_p2p(p2p, 2, &ctx->localities),
			-EINVAL);

	p2p = hmat_test_alloc_table(test, 1, 1);
	KUNIT_ASSERT_NOT_NULL(test, p2p);
	p2p->header.length = sizeof(*p2p);
	KUNIT_EXPECT_EQ(test, hmat_parse_p2p(p2p, 2, &ctx->localities),
			-EINVAL);
	KUNIT_EXPECT_EQ(test,
			hmat_get_p2p_coordinates(&ctx->localities, 0, 0,
						 HMAT_P2P_NON_UIO, &coord),
			-ENOENT);
}

static struct kunit_case __refdata hmat_p2p_test_cases[] = {
	KUNIT_CASE(hmat_p2p_matrix_test),
	KUNIT_CASE(hmat_p2p_coordinate_merge_test),
	KUNIT_CASE(hmat_p2p_invalid_length_test),
	{}
};

static struct kunit_suite __refdata hmat_p2p_test_suite = {
	.name = "acpi_hmat_p2p",
	.init = hmat_test_init,
	.exit = hmat_test_exit,
	.test_cases = hmat_p2p_test_cases,
};
kunit_test_init_section_suite(hmat_p2p_test_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit tests for ACPI HMAT PCIe P2P matrices");
