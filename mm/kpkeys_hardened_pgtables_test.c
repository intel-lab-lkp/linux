// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/pgtable.h>
#include <linux/mman.h>

static void write_kernel_pte(struct kunit *test)
{
	pte_t *ptep;
	pte_t pte;
	int ret;

	/*
	 * The choice of address is mostly arbitrary - we just need a page
	 * that is definitely mapped, such as the current function.
	 */
	ptep = virt_to_kpte((unsigned long)&write_kernel_pte);
	KUNIT_ASSERT_NOT_NULL_MSG(test, ptep, "Failed to get PTE");

	pte = ptep_get(ptep);
	pte = set_pte_bit(pte, __pgprot(PTE_WRITE));
	ret = copy_to_kernel_nofault(ptep, &pte, sizeof(pte));
	KUNIT_EXPECT_EQ_MSG(test, ret, -EFAULT,
			    "Direct PTE write wasn't prevented");
}

static void write_user_pmd(struct kunit *test)
{
	pmd_t *pmdp;
	pmd_t pmd;
	unsigned long uaddr;
	int ret;

	uaddr = kunit_vm_mmap(test, NULL, 0, PAGE_SIZE, PROT_READ,
			      MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE, 0);
	KUNIT_ASSERT_NE_MSG(test, uaddr, 0, "Could not create userspace mm");

	/* We passed MAP_POPULATE so a PMD should already be allocated */
	pmdp = pmd_off(current->mm, uaddr);
	KUNIT_ASSERT_NOT_NULL_MSG(test, pmdp, "Failed to get PMD");

	pmd = pmdp_get(pmdp);
	pmd = set_pmd_bit(pmd, __pgprot(PROT_SECT_NORMAL));
	ret = copy_to_kernel_nofault(pmdp, &pmd, sizeof(pmd));
	KUNIT_EXPECT_EQ_MSG(test, ret, -EFAULT,
			    "Direct PMD write wasn't prevented");
}

static int kpkeys_hardened_pgtables_suite_init(struct kunit_suite *suite)
{
	if (!arch_kpkeys_enabled()) {
		pr_err("Cannot run kpkeys_hardened_pgtables tests: kpkeys are not supported\n");
		return 1;
	}

	return 0;
}

static struct kunit_case kpkeys_hardened_pgtables_test_cases[] = {
	KUNIT_CASE(write_kernel_pte),
	KUNIT_CASE(write_user_pmd),
	{}
};

static struct kunit_suite kpkeys_hardened_pgtables_test_suite = {
	.name = "Hardened pgtables using kpkeys",
	.test_cases = kpkeys_hardened_pgtables_test_cases,
	.suite_init = kpkeys_hardened_pgtables_suite_init,
};
kunit_test_suite(kpkeys_hardened_pgtables_test_suite);

MODULE_DESCRIPTION("Tests for the kpkeys_hardened_pgtables feature");
MODULE_LICENSE("GPL");
