// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the KVM stage 2 page table walker (hyp/pgtable.c).
 *
 * The walker takes all of its dependencies through kvm_pgtable_mm_ops,
 * so these tests plug in a mock allocator with a bounded page budget
 * and assert exactly how map and collapse operations consume it. A
 * fault path that fails to stage enough memory for a stage 2 update
 * shows up here as a clean ENOMEM at unit level, rather than as a
 * WARN or worse under a running guest on arm64 hardware.
 *
 * All map calls pass KVM_PGTABLE_WALK_SKIP_BBM_TLBI and SKIP_CMO
 * since no hardware walker can ever observe these tables.
 */
#include <kunit/test.h>

#include <linux/kvm_host.h>
#include <linux/sizes.h>

#include <asm/cpufeature.h>
#include <asm/kvm_pgtable.h>
#include <asm/sysreg.h>

#define TEST_PHYS_SHIFT		40
#define TEST_IPA		SZ_1G
/* Never dereferenced: CMOs are skipped and leaf PAs are never followed. */
#define TEST_PA			(4UL * SZ_1G)
#define TEST_WALK_FLAGS		(KVM_PGTABLE_WALK_SKIP_BBM_TLBI | \
				 KVM_PGTABLE_WALK_SKIP_CMO)

/* Stands in for the fault handler memcache: a bounded page budget. */
struct test_memcache {
	int avail;
	int allocated;
};

static void *test_zalloc_page(void *arg)
{
	struct test_memcache *mc = arg;

	if (!mc || mc->avail <= 0)
		return NULL;

	mc->avail--;
	mc->allocated++;
	return (void *)get_zeroed_page(GFP_KERNEL);
}

static void *test_zalloc_pages_exact(size_t size)
{
	return alloc_pages_exact(size, GFP_KERNEL | __GFP_ZERO);
}

static void test_free_pages_exact(void *addr, size_t size)
{
	free_pages_exact(addr, size);
}

static void test_get_page(void *addr)
{
	get_page(virt_to_page(addr));
}

static void test_put_page(void *addr)
{
	put_page(virt_to_page(addr));
}

static int test_page_count(void *addr)
{
	return page_count(virt_to_page(addr));
}

static void *test_phys_to_virt(phys_addr_t phys)
{
	return __va(phys);
}

static phys_addr_t test_virt_to_phys(void *addr)
{
	return __pa(addr);
}

static void test_cmo_nop(void *addr, size_t size)
{
}

static struct kvm_pgtable_mm_ops test_mm_ops;

static void test_free_unlinked_table(void *addr, s8 level)
{
	kvm_pgtable_stage2_free_unlinked(&test_mm_ops, addr, level);
}

static struct kvm_pgtable_mm_ops test_mm_ops = {
	.zalloc_page		= test_zalloc_page,
	.zalloc_pages_exact	= test_zalloc_pages_exact,
	.free_pages_exact	= test_free_pages_exact,
	.free_unlinked_table	= test_free_unlinked_table,
	.get_page		= test_get_page,
	.put_page		= test_put_page,
	.page_count		= test_page_count,
	.phys_to_virt		= test_phys_to_virt,
	.virt_to_phys		= test_virt_to_phys,
	.dcache_clean_inval_poc	= test_cmo_nop,
	.icache_inval_pou	= test_cmo_nop,
};

struct pgtable_test_ctx {
	struct kvm *kvm;
	struct kvm_pgtable pgt;
	struct test_memcache mc;
};

static void pgtable_test_init_ctx(struct kunit *test,
				  struct pgtable_test_ctx *ctx)
{
	u64 mmfr0 = read_sanitised_ftr_reg(SYS_ID_AA64MMFR0_EL1);
	u64 mmfr1 = read_sanitised_ftr_reg(SYS_ID_AA64MMFR1_EL1);
	struct kvm_s2_mmu *mmu;

	if (PAGE_SIZE != SZ_4K)
		kunit_skip(test, "test expects 4K pages");

	ctx->kvm = kunit_kzalloc(test, sizeof(*ctx->kvm), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx->kvm);

	mmu = &ctx->kvm->arch.mmu;
	mmu->arch = &ctx->kvm->arch;
	mmu->vtcr = kvm_get_vtcr(mmfr0, mmfr1, TEST_PHYS_SHIFT);

	KUNIT_ASSERT_EQ(test,
			kvm_pgtable_stage2_init(&ctx->pgt, mmu, &test_mm_ops),
			0);
}

/* Table pages needed to take one page mapping from the PGD to a leaf. */
static int pgtable_test_max_tables(struct pgtable_test_ctx *ctx)
{
	return KVM_PGTABLE_LAST_LEVEL - ctx->pgt.start_level;
}

static void stage2_map_page_stocked_memcache(struct kunit *test)
{
	struct pgtable_test_ctx ctx = {};
	kvm_pte_t pte = 0;
	s8 level = 0;
	int ret;

	pgtable_test_init_ctx(test, &ctx);

	ctx.mc.avail = pgtable_test_max_tables(&ctx);
	ret = kvm_pgtable_stage2_map(&ctx.pgt, TEST_IPA, PAGE_SIZE, TEST_PA,
				     KVM_PGTABLE_PROT_RW, &ctx.mc,
				     TEST_WALK_FLAGS);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_GT(test, ctx.mc.allocated, 0);

	KUNIT_EXPECT_EQ(test,
			kvm_pgtable_get_leaf(&ctx.pgt, TEST_IPA, &pte, &level),
			0);
	KUNIT_EXPECT_TRUE(test, kvm_pte_valid(pte));
	KUNIT_EXPECT_EQ(test, level, (s8)KVM_PGTABLE_LAST_LEVEL);
	KUNIT_EXPECT_EQ(test,
			kvm_pgtable_stage2_pte_prot(pte) & KVM_PGTABLE_PROT_RW,
			KVM_PGTABLE_PROT_RW);

	kvm_pgtable_stage2_destroy(&ctx.pgt);
}

static void stage2_map_page_empty_memcache(struct kunit *test)
{
	struct pgtable_test_ctx ctx = {};
	int ret;

	pgtable_test_init_ctx(test, &ctx);

	/*
	 * A fault path that reaches the walker without staging memory
	 * must fail cleanly instead of installing a partial mapping.
	 */
	ctx.mc.avail = 0;
	ret = kvm_pgtable_stage2_map(&ctx.pgt, TEST_IPA, PAGE_SIZE, TEST_PA,
				     KVM_PGTABLE_PROT_RW, &ctx.mc,
				     TEST_WALK_FLAGS);
	KUNIT_EXPECT_EQ(test, ret, -ENOMEM);
	KUNIT_EXPECT_EQ(test, ctx.mc.allocated, 0);

	kvm_pgtable_stage2_destroy(&ctx.pgt);
}

static void stage2_collapse_pages_into_block(struct kunit *test)
{
	struct pgtable_test_ctx ctx = {};
	int ret, before, i;
	kvm_pte_t pte = 0;
	s8 level = 0;
	u64 off;

	pgtable_test_init_ctx(test, &ctx);

	/* Fault the whole block range in at page granularity. */
	ctx.mc.avail = pgtable_test_max_tables(&ctx) + 1;
	for (i = 0; i < SZ_2M / PAGE_SIZE; i++) {
		off = (u64)i * PAGE_SIZE;
		ret = kvm_pgtable_stage2_map(&ctx.pgt, TEST_IPA + off,
					     PAGE_SIZE, TEST_PA + off,
					     KVM_PGTABLE_PROT_RW, &ctx.mc,
					     TEST_WALK_FLAGS);
		KUNIT_ASSERT_EQ(test, ret, 0);
	}

	KUNIT_ASSERT_EQ(test,
			kvm_pgtable_get_leaf(&ctx.pgt, TEST_IPA, &pte, &level),
			0);
	KUNIT_ASSERT_EQ(test, level, (s8)KVM_PGTABLE_LAST_LEVEL);

	/*
	 * Coalescing the pages back into a block replaces a table with
	 * a leaf, so it must consume nothing from the memcache. This is
	 * the transition that disabling dirty logging forces at fault
	 * time.
	 */
	before = ctx.mc.avail;
	ret = kvm_pgtable_stage2_map(&ctx.pgt, TEST_IPA, SZ_2M, TEST_PA,
				     KVM_PGTABLE_PROT_RW, &ctx.mc,
				     TEST_WALK_FLAGS);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, ctx.mc.avail, before);

	KUNIT_EXPECT_EQ(test,
			kvm_pgtable_get_leaf(&ctx.pgt, TEST_IPA, &pte, &level),
			0);
	KUNIT_EXPECT_TRUE(test, kvm_pte_valid(pte));
	KUNIT_EXPECT_EQ(test, level, (s8)(KVM_PGTABLE_LAST_LEVEL - 1));

	kvm_pgtable_stage2_destroy(&ctx.pgt);
}

static struct kunit_case stage2_pgtable_test_cases[] = {
	KUNIT_CASE(stage2_map_page_stocked_memcache),
	KUNIT_CASE(stage2_map_page_empty_memcache),
	KUNIT_CASE(stage2_collapse_pages_into_block),
	{}
};

static struct kunit_suite stage2_pgtable_suite = {
	.name = "kvm-stage2-pgtable",
	.test_cases = stage2_pgtable_test_cases,
};

kunit_test_suite(stage2_pgtable_suite);
