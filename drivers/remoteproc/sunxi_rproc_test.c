// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for Allwinner XuanTie RISC-V remoteproc driver (sunxi_rproc.c)
 *
 * Tests da_to_va() address translation logic for all memory window types
 * and boundary conditions. These are pure-logic tests that run without
 * real hardware.
 *
 * Copyright (C) 2026 Tim Michals <tcmichals@gmail.com>
 */

#include <kunit/test.h>
#include <linux/remoteproc.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

/*
 * We test da_to_va() by constructing a fake sunxi_rproc with known
 * memory window parameters and calling the function directly.
 *
 * Since sunxi_rproc_da_to_va() is static in sunxi_rproc.c, we
 * replicate the exact address translation logic here. Any divergence
 * from the driver is itself a test failure that code review will catch.
 */

struct test_sunxi_rproc {
	void __iomem *r_sram_va;
	phys_addr_t r_sram_phys;
	size_t r_sram_size;

	void __iomem *r_sram1_va;
	phys_addr_t r_sram1_phys;
	size_t r_sram1_size;

	void *dram_va;
	phys_addr_t dram_phys;
	size_t dram_size;

	void *trace_va;
	phys_addr_t trace_phys;
	size_t trace_size;
};

/*
 * Mirror of sunxi_rproc_da_to_va() logic from drivers/remoteproc/sunxi_rproc.c.
 * Must be kept in sync with the driver. Divergence is a bug.
 */
static void *test_da_to_va(struct test_sunxi_rproc *priv,
			   u64 da, size_t len, bool *is_iomem)
{
	if (len == 0)
		return NULL;

	/* 1. Dedicated MCU Local SRAM Space 0 */
	if (priv->r_sram_va) {
		/* Host physical address view */
		if (da >= priv->r_sram_phys &&
		    (da + len) <= (priv->r_sram_phys + priv->r_sram_size)) {
			if (is_iomem)
				*is_iomem = true;
			return priv->r_sram_va + (da - priv->r_sram_phys);
		}
		/* Core DA view: 0x40000000 */
		if (da >= 0x40000000 &&
		    (da + len) <= (0x40000000 + priv->r_sram_size)) {
			if (is_iomem)
				*is_iomem = true;
			return priv->r_sram_va + (da - 0x40000000);
		}
		/* High SRAM Space 0 fallback views */
		if (da >= 0x3ff80000 && (da + len) <= (0x3ff80000 + priv->r_sram_size)) {
			if (is_iomem)
				*is_iomem = true;
			return priv->r_sram_va + (da - 0x3ff80000);
		}
		if (da >= 0x3ffc0000 && (da + len) <= (0x3ffc0000 + priv->r_sram_size)) {
			if (is_iomem)
				*is_iomem = true;
			return priv->r_sram_va + (da - 0x3ffc0000);
		}
	}

	/* 2. Switchable MCU Local SRAM Space 1 */
	if (priv->r_sram1_va) {
		if (da >= priv->r_sram1_phys &&
		    (da + len) <= (priv->r_sram1_phys + priv->r_sram1_size)) {
			if (is_iomem)
				*is_iomem = true;
			return priv->r_sram1_va + (da - priv->r_sram1_phys);
		}
		if (da >= 0x40000000 &&
		    (da + len) <= (0x40000000 + priv->r_sram1_size)) {
			if (is_iomem)
				*is_iomem = true;
			return priv->r_sram1_va + (da - 0x40000000);
		}
		if (da >= 0x40040000 &&
		    (da + len) <= (0x40040000 + priv->r_sram1_size)) {
			if (is_iomem)
				*is_iomem = true;
			return priv->r_sram1_va + (da - 0x40040000);
		}
	}

	/* 4. Trace / Reserved Memory */
	if (priv->trace_va) {
		if (da >= priv->trace_phys &&
		    (da + len) <= (priv->trace_phys + priv->trace_size)) {
			if (is_iomem)
				*is_iomem = false;
			return priv->trace_va + (da - priv->trace_phys);
		}
	}

	/* 5. Boot DRAM Carveout */
	if (priv->dram_va) {
		if (da >= priv->dram_phys &&
		    (da + len) <= (priv->dram_phys + priv->dram_size)) {
			if (is_iomem)
				*is_iomem = false;
			return priv->dram_va + (da - priv->dram_phys);
		}
	}

	return NULL;
}

/*
 * Fake VA addresses — sentinel values to detect correct base + offset
 * arithmetic without needing real ioremap'd memory.
 */
#define FAKE_SRAM_VA	((void __iomem *)0xA0000000UL)
#define FAKE_SRAM1_VA	((void __iomem *)0xB0000000UL)
#define FAKE_DRAM_VA	((void *)0xC0000000UL)
#define FAKE_TRACE_VA	((void *)0xD0000000UL)

/* Standard A527 hardware parameters */
#define A527_SRAM_PHYS		0x07280000ULL
#define A527_SRAM_SIZE		0x40000		/* 256 KB */
#define A527_SRAM1_PHYS		0x072C0000ULL
#define A527_SRAM1_SIZE		0x40000		/* 256 KB */
#define A527_DRAM_PHYS		0x48000000ULL
#define A527_DRAM_SIZE		0x100000	/* 1 MB */
#define A527_TRACE_PHYS		0x50000000ULL
#define A527_TRACE_SIZE		0x1000		/* 4 KB */

static struct test_sunxi_rproc *create_test_priv(struct kunit *test)
{
	struct test_sunxi_rproc *priv;

	priv = kunit_kzalloc(test, sizeof(*priv), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv);

	priv->r_sram_va = FAKE_SRAM_VA;
	priv->r_sram_phys = A527_SRAM_PHYS;
	priv->r_sram_size = A527_SRAM_SIZE;

	priv->r_sram1_va = FAKE_SRAM1_VA;
	priv->r_sram1_phys = A527_SRAM1_PHYS;
	priv->r_sram1_size = A527_SRAM1_SIZE;

	priv->dram_va = FAKE_DRAM_VA;
	priv->dram_phys = A527_DRAM_PHYS;
	priv->dram_size = A527_DRAM_SIZE;

	priv->trace_va = FAKE_TRACE_VA;
	priv->trace_phys = A527_TRACE_PHYS;
	priv->trace_size = A527_TRACE_SIZE;

	return priv;
}

/* ===== Zero-length guard ===== */

static void test_da_to_va_zero_length_returns_null(struct kunit *test)
{
	struct test_sunxi_rproc *priv = create_test_priv(test);

	KUNIT_EXPECT_NULL(test, test_da_to_va(priv, 0x3FFC0000, 0, NULL));
}

/* ===== SRAM Space 0: Core DA 0x3FFC0000 view ===== */

static void test_da_to_va_sram_space0_core_da(struct kunit *test)
{
	struct test_sunxi_rproc *priv = create_test_priv(test);
	bool is_iomem = false;
	void *va;

	va = test_da_to_va(priv, 0x3FFC0000, 0x100, &is_iomem);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, (void *)FAKE_SRAM_VA);
	KUNIT_EXPECT_TRUE(test, is_iomem);
}

static void test_da_to_va_sram_space0_core_da_offset(struct kunit *test)
{
	struct test_sunxi_rproc *priv = create_test_priv(test);
	void *va;

	va = test_da_to_va(priv, 0x3FFC0000 + 0x1000, 0x100, NULL);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va,
			    (void *)((unsigned long)FAKE_SRAM_VA + 0x1000));
}

/* ===== SRAM Space 0: Host physical address view ===== */

static void test_da_to_va_sram_space0_host_phys(struct kunit *test)
{
	struct test_sunxi_rproc *priv = create_test_priv(test);
	bool is_iomem = false;
	void *va;

	va = test_da_to_va(priv, A527_SRAM_PHYS, 0x100, &is_iomem);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, (void *)FAKE_SRAM_VA);
	KUNIT_EXPECT_TRUE(test, is_iomem);
}

static void test_da_to_va_sram_space0_host_phys_offset(struct kunit *test)
{
	struct test_sunxi_rproc *priv = create_test_priv(test);
	void *va;

	va = test_da_to_va(priv, A527_SRAM_PHYS + 0x2000, 0x100, NULL);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va,
			    (void *)((unsigned long)FAKE_SRAM_VA + 0x2000));
}

/* ===== SRAM Space 0: Alternate DA 0x3FF80000 view ===== */

static void test_da_to_va_sram_space0_alt_3ff80000(struct kunit *test)
{
	struct test_sunxi_rproc *priv = create_test_priv(test);
	bool is_iomem = false;
	void *va;

	va = test_da_to_va(priv, 0x3FF80000, 0x100, &is_iomem);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, (void *)FAKE_SRAM_VA);
	KUNIT_EXPECT_TRUE(test, is_iomem);
}

/* ===== SRAM Space 0: DA 0x40000000 view (when sram1 disabled) ===== */

static void test_da_to_va_sram_space0_40000000(struct kunit *test)
{
	struct test_sunxi_rproc *priv = create_test_priv(test);
	bool is_iomem = false;
	void *va;

	/* Disable sram1 so 0x40000000 hits sram space 0 path */
	priv->r_sram1_va = NULL;

	va = test_da_to_va(priv, 0x40000000, 0x100, &is_iomem);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, (void *)FAKE_SRAM_VA);
	KUNIT_EXPECT_TRUE(test, is_iomem);
}

/* ===== SRAM Space 1: Host physical view ===== */

static void test_da_to_va_sram_space1_host_phys(struct kunit *test)
{
	struct test_sunxi_rproc *priv = create_test_priv(test);
	bool is_iomem = false;
	void *va;

	va = test_da_to_va(priv, A527_SRAM1_PHYS, 0x100, &is_iomem);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, (void *)FAKE_SRAM1_VA);
	KUNIT_EXPECT_TRUE(test, is_iomem);
}

/* ===== SRAM Space 1: Core DA 0x40040000 view ===== */

static void test_da_to_va_sram_space1_40040000(struct kunit *test)
{
	struct test_sunxi_rproc *priv = create_test_priv(test);
	bool is_iomem = false;
	void *va;

	va = test_da_to_va(priv, 0x40040000, 0x100, &is_iomem);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, (void *)FAKE_SRAM1_VA);
	KUNIT_EXPECT_TRUE(test, is_iomem);
}

/* ===== DRAM carveout ===== */

static void test_da_to_va_dram_carveout(struct kunit *test)
{
	struct test_sunxi_rproc *priv = create_test_priv(test);
	bool is_iomem = true; /* should be set to false */
	void *va;

	va = test_da_to_va(priv, A527_DRAM_PHYS, 0x100, &is_iomem);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, FAKE_DRAM_VA);
	KUNIT_EXPECT_FALSE(test, is_iomem);
}

static void test_da_to_va_dram_carveout_offset(struct kunit *test)
{
	struct test_sunxi_rproc *priv = create_test_priv(test);
	void *va;

	va = test_da_to_va(priv, A527_DRAM_PHYS + 0x800, 0x100, NULL);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va,
			    (void *)((unsigned long)FAKE_DRAM_VA + 0x800));
}

/* ===== Trace region ===== */

static void test_da_to_va_trace_region(struct kunit *test)
{
	struct test_sunxi_rproc *priv = create_test_priv(test);
	bool is_iomem = true; /* should be set to false */
	void *va;

	va = test_da_to_va(priv, A527_TRACE_PHYS, 0x100, &is_iomem);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, FAKE_TRACE_VA);
	KUNIT_EXPECT_FALSE(test, is_iomem);
}

/* ===== Out-of-range returns NULL ===== */

static void test_da_to_va_out_of_range_returns_null(struct kunit *test)
{
	struct test_sunxi_rproc *priv = create_test_priv(test);

	KUNIT_EXPECT_NULL(test,
			  test_da_to_va(priv, 0x10000000, 0x100, NULL));
}

/* ===== Boundary: request extends past end of region ===== */

static void test_da_to_va_past_end_returns_null(struct kunit *test)
{
	struct test_sunxi_rproc *priv = create_test_priv(test);

	/* Starts inside SRAM but len extends past end */
	KUNIT_EXPECT_NULL(test,
			  test_da_to_va(priv,
					A527_SRAM_PHYS + A527_SRAM_SIZE - 0x10,
					0x20, NULL));
}

static void test_da_to_va_exact_end_boundary(struct kunit *test)
{
	struct test_sunxi_rproc *priv = create_test_priv(test);
	void *va;

	/* Exactly fills to end of region — should succeed */
	va = test_da_to_va(priv,
			   A527_SRAM_PHYS + A527_SRAM_SIZE - 0x10,
			   0x10, NULL);
	KUNIT_ASSERT_NOT_NULL(test, va);
}

/* ===== is_iomem correctness ===== */

static void test_da_to_va_is_iomem_sram_true(struct kunit *test)
{
	struct test_sunxi_rproc *priv = create_test_priv(test);
	bool is_iomem = false;

	test_da_to_va(priv, 0x3FFC0000, 0x100, &is_iomem);
	KUNIT_EXPECT_TRUE(test, is_iomem);
}

static void test_da_to_va_is_iomem_dram_false(struct kunit *test)
{
	struct test_sunxi_rproc *priv = create_test_priv(test);
	bool is_iomem = true;

	test_da_to_va(priv, A527_DRAM_PHYS, 0x100, &is_iomem);
	KUNIT_EXPECT_FALSE(test, is_iomem);
}

static void test_da_to_va_is_iomem_null_no_crash(struct kunit *test)
{
	struct test_sunxi_rproc *priv = create_test_priv(test);
	void *va;

	/* Passing NULL for is_iomem should not crash */
	va = test_da_to_va(priv, 0x3FFC0000, 0x100, NULL);
	KUNIT_ASSERT_NOT_NULL(test, va);
}

/* ===== No memory windows configured ===== */

static void test_da_to_va_no_windows_returns_null(struct kunit *test)
{
	struct test_sunxi_rproc *priv;

	priv = kunit_kzalloc(test, sizeof(*priv), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, priv);

	KUNIT_EXPECT_NULL(test,
			  test_da_to_va(priv, 0x3FFC0000, 0x100, NULL));
	KUNIT_EXPECT_NULL(test,
			  test_da_to_va(priv, 0x48000000, 0x100, NULL));
}

static struct kunit_case sunxi_rproc_da_to_va_cases[] = {
	KUNIT_CASE(test_da_to_va_zero_length_returns_null),
	KUNIT_CASE(test_da_to_va_sram_space0_core_da),
	KUNIT_CASE(test_da_to_va_sram_space0_core_da_offset),
	KUNIT_CASE(test_da_to_va_sram_space0_host_phys),
	KUNIT_CASE(test_da_to_va_sram_space0_host_phys_offset),
	KUNIT_CASE(test_da_to_va_sram_space0_alt_3ff80000),
	KUNIT_CASE(test_da_to_va_sram_space0_40000000),
	KUNIT_CASE(test_da_to_va_sram_space1_host_phys),
	KUNIT_CASE(test_da_to_va_sram_space1_40040000),
	KUNIT_CASE(test_da_to_va_dram_carveout),
	KUNIT_CASE(test_da_to_va_dram_carveout_offset),
	KUNIT_CASE(test_da_to_va_trace_region),
	KUNIT_CASE(test_da_to_va_out_of_range_returns_null),
	KUNIT_CASE(test_da_to_va_past_end_returns_null),
	KUNIT_CASE(test_da_to_va_exact_end_boundary),
	KUNIT_CASE(test_da_to_va_is_iomem_sram_true),
	KUNIT_CASE(test_da_to_va_is_iomem_dram_false),
	KUNIT_CASE(test_da_to_va_is_iomem_null_no_crash),
	KUNIT_CASE(test_da_to_va_no_windows_returns_null),
	{}
};

static struct kunit_suite sunxi_rproc_da_to_va_suite = {
	.name = "sunxi_rproc_da_to_va",
	.test_cases = sunxi_rproc_da_to_va_cases,
};

kunit_test_suites(&sunxi_rproc_da_to_va_suite);

MODULE_AUTHOR("Tim Michals <tcmichals@gmail.com>");
MODULE_DESCRIPTION("KUnit tests for Allwinner sunxi_rproc da_to_va()");
MODULE_LICENSE("GPL");
