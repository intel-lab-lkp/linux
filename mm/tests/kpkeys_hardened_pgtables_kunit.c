// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/mman.h>
#include <linux/pgtable.h>
#include <linux/set_memory.h>
#include <linux/vmalloc.h>

#ifdef CONFIG_ARM64
#include <asm/stacktrace.h>
#endif

static void free_page_wrapper(void *ctx)
{
	__free_page((struct page *)ctx);
}

KUNIT_DEFINE_ACTION_WRAPPER(vfree_wrapper, vfree, const void *);

static pud_t *pud_off_k(unsigned long va)
{
	return pud_offset(p4d_offset(pgd_offset_k(va), va), va);
}

static pte_t *get_kernel_pte(unsigned long addr)
{
	pmd_t *pmdp = pmd_off_k(addr);

	if (!pmdp || pmd_leaf(*pmdp))
		return NULL;

	return pte_offset_kernel(pmdp, addr);
}

#define write_pgtable(type, ptr) do {					\
	type##_t val;							\
	int ret;							\
									\
	pr_debug("%s: writing to "#type" at %px\n", __func__, (ptr));	\
									\
	val = type##p_get(ptr);						\
	ret = copy_to_kernel_nofault(ptr, &val, sizeof(val));		\
	KUNIT_EXPECT_EQ_MSG(test, ret, -EFAULT,				\
			    "Direct "#type" write wasn't prevented");	\
} while (0)

/*
 * Try to write linear map page tables, at every level. This is worthwhile
 * because those page table pages are obtained from different allocators:
 *
 * - Static memory (part of the kernel image) for PGD
 * - memblock for PUD and possibly PMD/PTE
 * - pagetable_alloc() (buddy allocator) for PMD/PTE if large block mappings are
 *   used and the linear map is split after being created
 */
static void write_direct_map_pgtables(struct kunit *test)
{
	struct page *page;
	unsigned long addr;
	pgd_t *pgdp;
	p4d_t *p4dp;
	pud_t *pudp;
	pmd_t *pmdp;
	pte_t *ptep;
	int ret;

	if (!arch_kpkeys_enabled())
		kunit_skip(test, "kpkeys are not supported");

	page = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, page);
	ret = kunit_add_action_or_reset(test, free_page_wrapper, page);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/* Ensure page is PTE-mapped (splitting the linear map if necessary) */
	ret = set_direct_map_invalid_noflush(page);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = set_direct_map_default_noflush(page);
	KUNIT_ASSERT_EQ(test, ret, 0);

	addr = (unsigned long)page_address(page);

	pgdp = pgd_offset_k(addr);
	KUNIT_ASSERT_NOT_NULL_MSG(test, pgdp, "Failed to get PGD");
	/*
	 * swapper_pg_dir is still writable at this stage, so don't check it.
	 * It is not protected by kpkeys_hardened_pgtables because it should be
	 * made read-only by mark_rodata_ro(). However since these
	 * KUnit tests are builtin, they are run before mark_rodata_ro() is
	 * called.
	 */

	p4dp = p4d_offset(pgdp, addr);
	KUNIT_ASSERT_NOT_NULL_MSG(test, p4dp, "Failed to get P4D");
	/* Not checked; same rationale as PGD in case P4D is folded */

	pudp = pud_offset(p4dp, addr);
	KUNIT_ASSERT_NOT_NULL_MSG(test, pudp, "Failed to get PUD");
	write_pgtable(pud, pudp);

	pmdp = pmd_offset(pudp, addr);
	KUNIT_ASSERT_NOT_NULL_MSG(test, pmdp, "Failed to get PMD");
	write_pgtable(pmd, pmdp);

	ptep = pte_offset_kernel(pmdp, addr);
	KUNIT_ASSERT_NOT_NULL_MSG(test, ptep, "Failed to get PTE");
	write_pgtable(pte, ptep);
}

/* Worth checking since the kernel image is mapped with static page tables */
static void write_kernel_image_pud(struct kunit *test)
{
	pud_t *pudp;

	if (!arch_kpkeys_enabled())
		kunit_skip(test, "kpkeys are not supported");

	/* The kernel is probably block-mapped, check the PUD to be safe */
	pudp = pud_off_k((unsigned long)&init_mm);
	KUNIT_ASSERT_NOT_NULL_MSG(test, pudp, "Failed to get PUD");

	write_pgtable(pud, pudp);
}

static void write_kernel_vmalloc_pte(struct kunit *test)
{
	void *mem;
	pte_t *ptep;
	int ret;

	if (!arch_kpkeys_enabled())
		kunit_skip(test, "kpkeys are not supported");

	mem = vmalloc(PAGE_SIZE);
	KUNIT_ASSERT_NOT_NULL(test, mem);
	ret = kunit_add_action_or_reset(test, vfree_wrapper, mem);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/* vmalloc() without VM_ALLOW_HUGE_VMAP is PTE-mapped */
	ptep = get_kernel_pte((unsigned long)mem);
	KUNIT_ASSERT_NOT_NULL_MSG(test, ptep, "Failed to get PTE");

	write_pgtable(pte, ptep);
}

#ifdef CONFIG_ARM64
static void write_early_kernel_vmap_pte(struct kunit *test)
{
	pte_t *ptep;

	if (!arch_kpkeys_enabled())
		kunit_skip(test, "kpkeys are not supported");

	/*
	 * When block mappings are used, the IRQ stacks are allocated before
	 * set_memory_pkey() is available - the pkey is set later by
	 * kpkeys_hardened_pgtables_init_late()
	 */
	ptep = get_kernel_pte((unsigned long)raw_cpu_read(irq_stack_ptr));
	KUNIT_ASSERT_NOT_NULL_MSG(test, ptep, "Failed to get PTE");

	write_pgtable(pte, ptep);
}
#endif

static void write_user_pmd(struct kunit *test)
{
	pmd_t *pmdp;
	unsigned long uaddr;

	if (!arch_kpkeys_enabled())
		kunit_skip(test, "kpkeys are not supported");

	uaddr = kunit_vm_mmap(test, NULL, 0, PAGE_SIZE, PROT_READ,
			      MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE, 0);
	KUNIT_ASSERT_NE_MSG(test, uaddr, 0, "Could not create userspace mm");

	/* We passed MAP_POPULATE so a PMD should already be allocated */
	pmdp = pmd_off(current->mm, uaddr);
	KUNIT_ASSERT_NOT_NULL_MSG(test, pmdp, "Failed to get PMD");

	write_pgtable(pmd, pmdp);
}

static struct kunit_case kpkeys_hardened_pgtables_test_cases[] = {
	KUNIT_CASE(write_direct_map_pgtables),
	KUNIT_CASE(write_kernel_image_pud),
	KUNIT_CASE(write_kernel_vmalloc_pte),
#ifdef CONFIG_ARM64
	KUNIT_CASE(write_early_kernel_vmap_pte),
#endif
	KUNIT_CASE(write_user_pmd),
	{}
};

static struct kunit_suite kpkeys_hardened_pgtables_test_suite = {
	.name = "kpkeys_hardened_pgtables",
	.test_cases = kpkeys_hardened_pgtables_test_cases,
};
kunit_test_suite(kpkeys_hardened_pgtables_test_suite);

MODULE_DESCRIPTION("Tests for the kpkeys_hardened_pgtables feature");
MODULE_LICENSE("GPL");
