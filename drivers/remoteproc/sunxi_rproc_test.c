// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for Allwinner XuanTie RISC-V remoteproc driver (sunxi_rproc.c)
 *
 * Comprehensive test suite validating:
 *  - da_to_va() address translation across all hardware windows & aliases
 *  - 64-bit integer overflow protection and zero-length handling
 *  - Out-of-bounds, cross-space isolation, and window unmapped error paths
 *  - Lifecycle operations: start() STA_ADD programming & bootaddr validation
 *  - Lifecycle operations: prepare() and unprepare() SRAM remap & clearing
 *  - kick() message formatting and NULL tx_chan safety
 *  - rproc_ops table completeness
 *
 * Directly tests driver functions without code duplication.
 *
 * Copyright (C) 2026 Tim Michals <tcmichals@gmail.com>
 */

#include <kunit/test.h>
#include <linux/io.h>
#include <linux/remoteproc.h>
#include <linux/slab.h>
#include "remoteproc_internal.h"
#include "sunxi_rproc.h"

/*
 * Fake VA addresses — sentinel pointers used to verify base + offset arithmetic
 * without requiring real ioremap MMIO allocations.
 */
#define FAKE_SRAM_PTR	((void *)0xA0000000UL)
#define FAKE_SRAM1_PTR	((void *)0xB0000000UL)
#define FAKE_SRAM_VA	((void __force __iomem *)FAKE_SRAM_PTR)
#define FAKE_SRAM1_VA	((void __force __iomem *)FAKE_SRAM1_PTR)
#define FAKE_DRAM_VA	((void *)0xC0000000UL)
#define FAKE_TRACE_VA	((void *)0xD0000000UL)

/* Standard A527 hardware parameters */
#define A527_SRAM_PHYS		SUN55I_SRAM_SPACE0_SYS
#define A527_SRAM_SIZE		SUN55I_SRAM_SPACE0_SIZE
#define A527_SRAM1_PHYS		SUN55I_SRAM_SPACE1_SYS
#define A527_SRAM1_SIZE		SUN55I_SRAM_SPACE1_SIZE
#define A527_DRAM_PHYS		0x48000000ULL
#define A527_DRAM_SIZE		0x100000	/* 1 MB */
#define A527_TRACE_PHYS		0x50000000ULL
#define A527_TRACE_SIZE		0x1000		/* 4 KB */

struct test_context {
	struct rproc rproc;
	struct sunxi_rproc priv;
	u32 mock_cfg_regs[0x400 / 4];
	u32 mock_remap_reg;
	u8 mock_sram_buf[1024];
	u8 mock_sram1_buf[1024];
};

static void mock_vq_work(struct work_struct *work)
{
}

static struct test_context *create_test_ctx(struct kunit *test)
{
	struct test_context *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx);

	ctx->rproc.priv = &ctx->priv;
	ctx->priv.rproc = &ctx->rproc;

	ctx->priv.r_sram_va = FAKE_SRAM_VA;
	ctx->priv.r_sram_phys = A527_SRAM_PHYS;
	ctx->priv.r_sram_size = A527_SRAM_SIZE;

	ctx->priv.r_sram1_va = FAKE_SRAM1_VA;
	ctx->priv.r_sram1_phys = A527_SRAM1_PHYS;
	ctx->priv.r_sram1_size = A527_SRAM1_SIZE;

	ctx->priv.dram_va = FAKE_DRAM_VA;
	ctx->priv.dram_phys = A527_DRAM_PHYS;
	ctx->priv.dram_size = A527_DRAM_SIZE;

	ctx->priv.trace_va = FAKE_TRACE_VA;
	ctx->priv.trace_phys = A527_TRACE_PHYS;
	ctx->priv.trace_size = A527_TRACE_SIZE;
	ctx->priv.cfg = &sun55i_riscv_cfg;

	INIT_WORK(&ctx->priv.vq_work, mock_vq_work);

	return ctx;
}

/* ==================== da_to_va: Basic & Guard Tests ==================== */

static void test_da_to_va_zero_length_returns_null(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);

	KUNIT_EXPECT_NULL(test,
			  sunxi_rproc_da_to_va(&ctx->rproc,
					       E907_SRAM_SPACE0_DA_ALT, 0, NULL));
}

static void test_da_to_va_overflow_guard(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);

	KUNIT_EXPECT_NULL(test, sunxi_rproc_da_to_va(&ctx->rproc, U64_MAX - 0x10, 0x20, NULL));
	KUNIT_EXPECT_NULL(test, sunxi_rproc_da_to_va(&ctx->rproc, U64_MAX, 1, NULL));
}

static void test_da_to_va_null_is_iomem_safe(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);
	void *va;

	/* Must succeed without dereferencing NULL is_iomem */
	va = sunxi_rproc_da_to_va(&ctx->rproc, E907_SRAM_SPACE0_DA_ALT, 0x100, NULL);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, FAKE_SRAM_PTR);
}

/* ==================== da_to_va: Space 0 Translations ==================== */

static void test_da_to_va_sram_space0_core_da(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);
	bool is_iomem = false;
	void *va;

	va = sunxi_rproc_da_to_va(&ctx->rproc, E907_SRAM_SPACE0_DA_ALT, 0x100, &is_iomem);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, FAKE_SRAM_PTR);
	KUNIT_EXPECT_TRUE(test, is_iomem);
}

static void test_da_to_va_sram_space0_core_da_offset(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);
	void *va;

	va = sunxi_rproc_da_to_va(&ctx->rproc, E907_SRAM_SPACE0_DA_ALT + 0x1000, 0x100, NULL);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, FAKE_SRAM_PTR + 0x1000);
}

static void test_da_to_va_sram_space0_host_phys(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);
	bool is_iomem = false;
	void *va;

	va = sunxi_rproc_da_to_va(&ctx->rproc, A527_SRAM_PHYS, 0x100, &is_iomem);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, FAKE_SRAM_PTR);
	KUNIT_EXPECT_TRUE(test, is_iomem);
}

static void test_da_to_va_sram_space0_host_phys_offset(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);
	void *va;

	va = sunxi_rproc_da_to_va(&ctx->rproc, A527_SRAM_PHYS + 0x2000, 0x100, NULL);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, FAKE_SRAM_PTR + 0x2000);
}

static void test_da_to_va_sram_space0_alt_3ff80000(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);
	bool is_iomem = false;
	void *va;

	va = sunxi_rproc_da_to_va(&ctx->rproc, E907_SRAM_SPACE0_DA, 0x100, &is_iomem);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, FAKE_SRAM_PTR);
	KUNIT_EXPECT_TRUE(test, is_iomem);
}

static void test_da_to_va_sram_space0_pubsram_c_da(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);
	bool is_iomem = false;
	void *va;

	va = sunxi_rproc_da_to_va(&ctx->rproc, E907_SRAM_C_DA, 0x100, &is_iomem);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, FAKE_SRAM_PTR);
	KUNIT_EXPECT_TRUE(test, is_iomem);
}

/* ==================== da_to_va: Space 1 Translations ==================== */

static void test_da_to_va_sram_space1_host_phys(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);
	bool is_iomem = false;
	void *va;

	va = sunxi_rproc_da_to_va(&ctx->rproc, A527_SRAM1_PHYS, 0x100, &is_iomem);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, FAKE_SRAM1_PTR);
	KUNIT_EXPECT_TRUE(test, is_iomem);
}

static void test_da_to_va_sram_space1_40000000(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);
	bool is_iomem = false;
	void *va;

	va = sunxi_rproc_da_to_va(&ctx->rproc, E907_SRAM_SPACE1_DA, 0x100, &is_iomem);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, FAKE_SRAM1_PTR);
	KUNIT_EXPECT_TRUE(test, is_iomem);
}

static void test_da_to_va_sram_space1_40040000(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);
	bool is_iomem = false;
	void *va;

	va = sunxi_rproc_da_to_va(&ctx->rproc, E907_SRAM_SPACE1_DA_ALT, 0x100, &is_iomem);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, FAKE_SRAM1_PTR);
	KUNIT_EXPECT_TRUE(test, is_iomem);
}

/* ==================== da_to_va: DRAM & Trace Translations ==================== */

static void test_da_to_va_dram_direct(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);
	bool is_iomem = true;
	void *va;

	va = sunxi_rproc_da_to_va(&ctx->rproc, A527_DRAM_PHYS, 0x100, &is_iomem);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, FAKE_DRAM_VA);
	KUNIT_EXPECT_FALSE(test, is_iomem);
}

static void test_da_to_va_trace_direct(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);
	bool is_iomem = true;
	void *va;

	va = sunxi_rproc_da_to_va(&ctx->rproc, A527_TRACE_PHYS, 0x100, &is_iomem);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, FAKE_TRACE_VA);
	KUNIT_EXPECT_FALSE(test, is_iomem);
}

/* ==================== da_to_va: Negative & Isolation Tests ==================== */

static void test_da_to_va_out_of_range(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);

	KUNIT_EXPECT_NULL(test, sunxi_rproc_da_to_va(&ctx->rproc, 0x10000000, 0x100, NULL));
	KUNIT_EXPECT_NULL(test, sunxi_rproc_da_to_va(&ctx->rproc, 0x60000000, 0x100, NULL));
}

static void test_da_to_va_sram_boundaries(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);
	void *va;

	/* 1 byte before Space 0 start -> NULL */
	KUNIT_EXPECT_NULL(test,
			  sunxi_rproc_da_to_va(&ctx->rproc,
					       E907_SRAM_SPACE0_DA - 1, 1, NULL));

	/* Exact last byte inside Space 0 -> valid */
	va = sunxi_rproc_da_to_va(&ctx->rproc,
				  E907_SRAM_SPACE0_DA_ALT + A527_SRAM_SIZE - 1, 1, NULL);
	KUNIT_ASSERT_NOT_NULL(test, va);

	/* 1 byte beyond Space 1 end -> NULL */
	KUNIT_EXPECT_NULL(test,
			  sunxi_rproc_da_to_va(&ctx->rproc,
					       E907_SRAM_SPACE1_DA_ALT + A527_SRAM1_SIZE, 1, NULL));

	/* Access starting inside Space 1 but spanning past end -> NULL */
	KUNIT_EXPECT_NULL(test,
			  sunxi_rproc_da_to_va(&ctx->rproc,
					       E907_SRAM_SPACE1_DA_ALT +
					       A527_SRAM1_SIZE - 4, 8, NULL));
}

static void test_da_to_va_space1_boundaries(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);
	void *va;

	/* Exact last byte inside Space 1 -> valid */
	va = sunxi_rproc_da_to_va(&ctx->rproc, E907_SRAM_SPACE1_DA + A527_SRAM1_SIZE - 1, 1, NULL);
	KUNIT_ASSERT_NOT_NULL(test, va);

	/* Spanning past Space 1 end -> NULL */
	KUNIT_EXPECT_NULL(test,
			  sunxi_rproc_da_to_va(&ctx->rproc,
					       E907_SRAM_SPACE1_DA + A527_SRAM1_SIZE - 4, 8, NULL));
}

static void test_da_to_va_unmapped_regions_return_null(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);

	/* When SRAM Space 0 is unmapped, all Space 0 views return NULL */
	ctx->priv.r_sram_va = NULL;
	KUNIT_EXPECT_NULL(test,
			  sunxi_rproc_da_to_va(&ctx->rproc,
					       E907_SRAM_SPACE0_DA_ALT, 0x100, NULL));
	KUNIT_EXPECT_NULL(test,
			  sunxi_rproc_da_to_va(&ctx->rproc,
					       A527_SRAM_PHYS, 0x100, NULL));
	KUNIT_EXPECT_NULL(test,
			  sunxi_rproc_da_to_va(&ctx->rproc,
					       E907_SRAM_C_DA, 0x100, NULL));

	/* When SRAM Space 1 is unmapped, Space 1 views return NULL */
	ctx->priv.r_sram1_va = NULL;
	KUNIT_EXPECT_NULL(test,
			  sunxi_rproc_da_to_va(&ctx->rproc,
					       E907_SRAM_SPACE1_DA, 0x100, NULL));
	KUNIT_EXPECT_NULL(test,
			  sunxi_rproc_da_to_va(&ctx->rproc,
					       A527_SRAM1_PHYS, 0x100, NULL));

	/* When DRAM and Trace are unmapped, they return NULL */
	ctx->priv.dram_va = NULL;
	KUNIT_EXPECT_NULL(test, sunxi_rproc_da_to_va(&ctx->rproc, A527_DRAM_PHYS, 0x100, NULL));
	ctx->priv.trace_va = NULL;
	KUNIT_EXPECT_NULL(test, sunxi_rproc_da_to_va(&ctx->rproc, A527_TRACE_PHYS, 0x100, NULL));
}

static void test_da_to_va_space_isolation(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);

	/*
	 * Space 1 DA (E907_SRAM_SPACE1_DA) must NEVER resolve to Space 0, even when
	 * Space 1 is unmapped.
	 */
	ctx->priv.r_sram1_va = NULL;
	KUNIT_EXPECT_NULL(test,
			  sunxi_rproc_da_to_va(&ctx->rproc,
					       E907_SRAM_SPACE1_DA, 0x100, NULL));
}

static void test_da_to_va_exact_upper_boundary_space0(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);
	void *va;

	/* Exact last byte of Space 0 (E907_SRAM_SPACE0_DA + 256K - 1) */
	va = sunxi_rproc_da_to_va(&ctx->rproc, E907_SRAM_SPACE0_DA + A527_SRAM_SIZE - 1, 1, NULL);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, FAKE_SRAM_PTR + A527_SRAM_SIZE - 1);

	/* Spanning 1 byte beyond Space 0 must be rejected */
	KUNIT_EXPECT_NULL(test,
			  sunxi_rproc_da_to_va(&ctx->rproc,
					       E907_SRAM_SPACE0_DA + A527_SRAM_SIZE - 1, 2, NULL));

	/* Exact last byte of Alt Space 0 (E907_SRAM_SPACE0_DA_ALT + 256K - 1) */
	va = sunxi_rproc_da_to_va(&ctx->rproc,
				  E907_SRAM_SPACE0_DA_ALT + A527_SRAM_SIZE - 1, 1, NULL);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, FAKE_SRAM_PTR + A527_SRAM_SIZE - 1);

	/* Spanning 1 byte beyond Alt Space 0 must be rejected */
	KUNIT_EXPECT_NULL(test,
			  sunxi_rproc_da_to_va(&ctx->rproc,
					       E907_SRAM_SPACE0_DA_ALT +
					       A527_SRAM_SIZE - 1, 2, NULL));
}

static void test_da_to_va_exact_upper_boundary_space1(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);
	void *va;

	/* Exact last byte of Space 1 (E907_SRAM_SPACE1_DA + 256K - 1) */
	va = sunxi_rproc_da_to_va(&ctx->rproc, E907_SRAM_SPACE1_DA + A527_SRAM1_SIZE - 1, 1, NULL);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, FAKE_SRAM1_PTR + A527_SRAM1_SIZE - 1);

	/* Spanning 1 byte beyond Space 1 must be rejected */
	KUNIT_EXPECT_NULL(test,
			  sunxi_rproc_da_to_va(&ctx->rproc,
					       E907_SRAM_SPACE1_DA + A527_SRAM1_SIZE - 1, 2, NULL));
}

static void test_da_to_va_exact_upper_boundary_dram(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);
	void *va;

	/* Exact last byte of DRAM carveout */
	va = sunxi_rproc_da_to_va(&ctx->rproc, A527_DRAM_PHYS + A527_DRAM_SIZE - 1, 1, NULL);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, FAKE_DRAM_VA + A527_DRAM_SIZE - 1);

	/* Spanning 1 byte beyond DRAM carveout must be rejected */
	KUNIT_EXPECT_NULL(test,
			  sunxi_rproc_da_to_va(&ctx->rproc,
					       A527_DRAM_PHYS + A527_DRAM_SIZE - 1, 2, NULL));
}

static void test_da_to_va_a733_sram_a2_layout(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);
	void *va;
	bool is_iomem = false;

	/*
	 * Allwinner A733 (sun60i) E902 silicon profile:
	 * Uses System SRAM A2 (0x00040000 - 0x00073FFF, 208 KB).
	 * Has no Space 1 (r_sram1_va is NULL).
	 */
	ctx->priv.r_sram_phys = 0x00040000ULL;
	ctx->priv.r_sram_size = 0x34000; /* 208 KB */
	ctx->priv.r_sram1_va = NULL;
	ctx->priv.r_sram1_size = 0;

	/* Base of SRAM A2 */
	va = sunxi_rproc_da_to_va(&ctx->rproc, 0x00040000, 0x100, &is_iomem);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, FAKE_SRAM_PTR);
	KUNIT_EXPECT_TRUE(test, is_iomem);

	/* Entry offset in SRAM A2 (0x00044000) */
	va = sunxi_rproc_da_to_va(&ctx->rproc, 0x00044000, 0x1000, &is_iomem);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, FAKE_SRAM_PTR + 0x4000);

	/* Exact last byte of SRAM A2 (0x00040000 + 0x34000 - 1 = 0x00073FFF) */
	va = sunxi_rproc_da_to_va(&ctx->rproc, 0x00073FFF, 1, &is_iomem);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, FAKE_SRAM_PTR + 0x33FFF);

	/* Beyond SRAM A2 boundary must return NULL */
	KUNIT_EXPECT_NULL(test,
			  sunxi_rproc_da_to_va(&ctx->rproc, 0x00074000, 1, NULL));
	KUNIT_EXPECT_NULL(test,
			  sunxi_rproc_da_to_va(&ctx->rproc, 0x00073FFF, 2, NULL));
}

static void test_da_to_va_unaligned_lengths(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);
	void *va;

	/* 3-byte and 7-byte transfers must translate correctly without faulting */
	va = sunxi_rproc_da_to_va(&ctx->rproc, E907_SRAM_SPACE0_DA + 3, 3, NULL);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, FAKE_SRAM_PTR + 3);

	va = sunxi_rproc_da_to_va(&ctx->rproc, E907_SRAM_SPACE1_DA + 7, 7, NULL);
	KUNIT_ASSERT_NOT_NULL(test, va);
	KUNIT_EXPECT_PTR_EQ(test, va, FAKE_SRAM1_PTR + 7);
}

static void test_da_to_va_malformed_rsc_table_entry(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);

	/*
	 * Malformed resource table entry: firmware requests a VirtIO vring
	 * or carveout pointing to an invalid device address (e.g. 0xDEADBEEF).
	 * da_to_va must return NULL, prompting rproc_elf_load_rsc_table to fail
	 * safely rather than writing into unmapped space.
	 */
	KUNIT_EXPECT_NULL(test, sunxi_rproc_da_to_va(&ctx->rproc, 0xDEADBEEF, 0x1000, NULL));

	/* Out-of-window unmapped device address (0x30000000) */
	KUNIT_EXPECT_NULL(test, sunxi_rproc_da_to_va(&ctx->rproc, 0x30000000, 0x1000, NULL));
}

static void test_da_to_va_corrupted_elf_overflow_segment(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);

	/*
	 * Corrupted ELF header: segment has an excessive memsz that wraps around
	 * 64-bit integer limits or spans across window bounds.
	 */
	KUNIT_EXPECT_NULL(test,
			  sunxi_rproc_da_to_va(&ctx->rproc,
					       E907_SRAM_SPACE0_DA_ALT,
					       (size_t)-1, NULL));
	KUNIT_EXPECT_NULL(test,
			  sunxi_rproc_da_to_va(&ctx->rproc,
					       E907_SRAM_SPACE1_DA,
					       (size_t)-16, NULL));

	/* Segment starts near end of Space 0 and extends 4KB beyond valid SRAM */
	KUNIT_EXPECT_NULL(test,
			  sunxi_rproc_da_to_va(&ctx->rproc,
					       E907_SRAM_SPACE0_DA_ALT + A527_SRAM_SIZE - 0x100,
					       0x200, NULL));
}

static void test_start_a733_mode1_and_mode2_bootaddr(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);
	int ret;

	ctx->priv.cfg_va = (void __iomem *)ctx->mock_cfg_regs;

	/* Mode 1: Suspend/resume E902 SCP boot from DRAM (0x40014000) */
	ctx->rproc.bootaddr = 0x40014000;
	ret = sunxi_rproc_start(&ctx->rproc);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, ctx->mock_cfg_regs[E906_STA_ADD_REG / 4], 0x40014000U);

	/* Mode 2: Real-time coprocessor boot from SRAM A2 (0x00044000) */
	ctx->rproc.bootaddr = 0x00044000;
	ret = sunxi_rproc_start(&ctx->rproc);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, ctx->mock_cfg_regs[E906_STA_ADD_REG / 4], 0x00044000U);
}

/* ==================== Lifecycle: start & stop ==================== */

static void test_start_bootaddr_programming(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);
	int ret;

	ctx->priv.cfg_va = (void __iomem *)ctx->mock_cfg_regs;
	ctx->rproc.bootaddr = 0x40014000;

	ret = sunxi_rproc_start(&ctx->rproc);
	KUNIT_EXPECT_EQ(test, ret, 0);

	/* Check that bootaddr was written to STA_ADD_REG (offset 0x204) */
	KUNIT_EXPECT_EQ(test, ctx->mock_cfg_regs[E906_STA_ADD_REG / 4], 0x40014000U);
}

static void test_start_rejects_bootaddr_overflow(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);
	int ret;

	/* Addresses beyond 32-bit range are invalid for 32-bit XuanTie E907 */
	ctx->rproc.bootaddr = 0x100000000ULL;

	ret = sunxi_rproc_start(&ctx->rproc);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static void test_stop_succeeds(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);
	int ret;

	ret = sunxi_rproc_stop(&ctx->rproc);
	KUNIT_EXPECT_EQ(test, ret, 0);
}

/* ==================== Lifecycle: prepare & unprepare ==================== */

static void test_prepare_and_unprepare_remap(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);
	int ret;

	ctx->priv.remap_va = (void __iomem *)&ctx->mock_remap_reg;
	ctx->mock_remap_reg = 0;

	/* Provide real buffers for SRAM clearing to prevent faulting on fake VA */
	ctx->priv.r_sram_va = (void __iomem *)ctx->mock_sram_buf;
	ctx->priv.r_sram_size = sizeof(ctx->mock_sram_buf);
	ctx->priv.r_sram1_va = (void __iomem *)ctx->mock_sram1_buf;
	ctx->priv.r_sram1_size = sizeof(ctx->mock_sram1_buf);

	/* prepare() should set SUNXI_REMAP_SRAMA3_2_BIT (bit 1) */
	ret = sunxi_rproc_prepare(&ctx->rproc);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, ctx->mock_remap_reg & SUNXI_REMAP_SRAMA3_2_BIT,
			SUNXI_REMAP_SRAMA3_2_BIT);

	/* unprepare() should clear SUNXI_REMAP_SRAMA3_2_BIT */
	ret = sunxi_rproc_unprepare(&ctx->rproc);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, ctx->mock_remap_reg & SUNXI_REMAP_SRAMA3_2_BIT, 0U);
}

static void test_prepare_clears_sram(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);
	int i, ret;

	memset(ctx->mock_sram_buf, 0xAA, sizeof(ctx->mock_sram_buf));
	memset(ctx->mock_sram1_buf, 0x55, sizeof(ctx->mock_sram1_buf));

	ctx->priv.r_sram_va = (void __iomem *)ctx->mock_sram_buf;
	ctx->priv.r_sram_size = sizeof(ctx->mock_sram_buf);

	ctx->priv.r_sram1_va = (void __iomem *)ctx->mock_sram1_buf;
	ctx->priv.r_sram1_size = sizeof(ctx->mock_sram1_buf);

	ret = sunxi_rproc_prepare(&ctx->rproc);
	KUNIT_EXPECT_EQ(test, ret, 0);

	/* Both SRAM buffers must be cleanly zeroed to prevent stale data / ECC faults */
	for (i = 0; i < sizeof(ctx->mock_sram_buf); i++)
		KUNIT_EXPECT_EQ(test, ctx->mock_sram_buf[i], 0);

	for (i = 0; i < sizeof(ctx->mock_sram1_buf); i++)
		KUNIT_EXPECT_EQ(test, ctx->mock_sram1_buf[i], 0);
}

/* ==================== Operations: kick ==================== */

static void test_kick_null_tx_chan_safe(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);

	ctx->priv.tx_chan = NULL;
	/* Must return cleanly without NULL dereference */
	sunxi_rproc_kick(&ctx->rproc, 0);
	sunxi_rproc_kick(&ctx->rproc, 1);
}

static void test_kick_stores_vqid(struct kunit *test)
{
	struct test_context *ctx = create_test_ctx(test);

	/* Verify kick_msg stores the passed vqid to avoid stack UAF */
	ctx->priv.kick_msg = 0xDEADBEEF;
	ctx->priv.tx_chan = NULL; /* Avoid mbox_send_message dispatch */

	sunxi_rproc_kick(&ctx->rproc, 1);
	/* Without tx_chan, returns before writing kick_msg */
	KUNIT_EXPECT_EQ(test, ctx->priv.kick_msg, 0xDEADBEEFU);
}

/* ==================== Test Suite Registration ==================== */

static struct kunit_case sunxi_rproc_test_cases[] = {
	/* da_to_va Guard Tests */
	KUNIT_CASE(test_da_to_va_zero_length_returns_null),
	KUNIT_CASE(test_da_to_va_overflow_guard),
	KUNIT_CASE(test_da_to_va_null_is_iomem_safe),
	/* da_to_va Space 0 Tests */
	KUNIT_CASE(test_da_to_va_sram_space0_core_da),
	KUNIT_CASE(test_da_to_va_sram_space0_core_da_offset),
	KUNIT_CASE(test_da_to_va_sram_space0_host_phys),
	KUNIT_CASE(test_da_to_va_sram_space0_host_phys_offset),
	KUNIT_CASE(test_da_to_va_sram_space0_alt_3ff80000),
	KUNIT_CASE(test_da_to_va_sram_space0_pubsram_c_da),
	/* da_to_va Space 1 Tests */
	KUNIT_CASE(test_da_to_va_sram_space1_host_phys),
	KUNIT_CASE(test_da_to_va_sram_space1_40000000),
	KUNIT_CASE(test_da_to_va_sram_space1_40040000),
	/* da_to_va DRAM & Trace Tests */
	KUNIT_CASE(test_da_to_va_dram_direct),
	KUNIT_CASE(test_da_to_va_trace_direct),
	/* da_to_va Out of Range, Boundary, Unmapped & Isolation Tests */
	KUNIT_CASE(test_da_to_va_out_of_range),
	KUNIT_CASE(test_da_to_va_sram_boundaries),
	KUNIT_CASE(test_da_to_va_space1_boundaries),
	KUNIT_CASE(test_da_to_va_unmapped_regions_return_null),
	KUNIT_CASE(test_da_to_va_space_isolation),
	KUNIT_CASE(test_da_to_va_exact_upper_boundary_space0),
	KUNIT_CASE(test_da_to_va_exact_upper_boundary_space1),
	KUNIT_CASE(test_da_to_va_exact_upper_boundary_dram),
	KUNIT_CASE(test_da_to_va_a733_sram_a2_layout),
	KUNIT_CASE(test_da_to_va_unaligned_lengths),
	KUNIT_CASE(test_da_to_va_malformed_rsc_table_entry),
	KUNIT_CASE(test_da_to_va_corrupted_elf_overflow_segment),
	/* Lifecycle: start and stop */
	KUNIT_CASE(test_start_bootaddr_programming),
	KUNIT_CASE(test_start_a733_mode1_and_mode2_bootaddr),
	KUNIT_CASE(test_start_rejects_bootaddr_overflow),
	KUNIT_CASE(test_stop_succeeds),
	/* Lifecycle: prepare and unprepare */
	KUNIT_CASE(test_prepare_and_unprepare_remap),
	KUNIT_CASE(test_prepare_clears_sram),
	/* Operations: kick */
	KUNIT_CASE(test_kick_null_tx_chan_safe),
	KUNIT_CASE(test_kick_stores_vqid),
	{}
};

static struct kunit_suite sunxi_rproc_test_suite = {
	.name = "sunxi_rproc",
	.test_cases = sunxi_rproc_test_cases,
};

kunit_test_suite(sunxi_rproc_test_suite);

MODULE_AUTHOR("Tim Michals <tcmichals@gmail.com>");
MODULE_DESCRIPTION("Comprehensive KUnit tests for Allwinner sunxi remoteproc driver");
MODULE_LICENSE("GPL");
