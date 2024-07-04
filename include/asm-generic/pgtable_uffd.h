#ifndef _ASM_GENERIC_PGTABLE_UFFD_H
#define _ASM_GENERIC_PGTABLE_UFFD_H

#ifndef CONFIG_HAVE_ARCH_USERFAULTFD_WP
static __always_inline int pte_uffd_wp(pte_t pte)
{
	return 0;
}

static __always_inline int pmd_uffd_wp(pmd_t pmd)
{
	return 0;
}

static __always_inline int pud_uffd_wp(pud_t pud)
{
	return 0;
}

static __always_inline pte_t pte_mkuffd_wp(pte_t pte)
{
	return pte;
}

static __always_inline pmd_t pmd_mkuffd_wp(pmd_t pmd)
{
	return pmd;
}

static __always_inline pud_t pud_mkuffd_wp(pud_t pud)
{
	return pmd;
}

static __always_inline pte_t pte_clear_uffd_wp(pte_t pte)
{
	return pte;
}

static __always_inline pmd_t pmd_clear_uffd_wp(pmd_t pmd)
{
	return pmd;
}

static __always_inline pud_t pud_clear_uffd_wp(pud_t pud)
{
	return pmd;
}

static __always_inline pte_t pte_swp_mkuffd_wp(pte_t pte)
{
	return pte;
}

static __always_inline int pte_swp_uffd_wp(pte_t pte)
{
	return 0;
}

static __always_inline pte_t pte_swp_clear_uffd_wp(pte_t pte)
{
	return pte;
}

static inline pmd_t pmd_swp_mkuffd_wp(pmd_t pmd)
{
	return pmd;
}

static inline int pmd_swp_uffd_wp(pmd_t pmd)
{
	return 0;
}

static inline pmd_t pmd_swp_clear_uffd_wp(pmd_t pmd)
{
	return pmd;
}

static inline pud_t pud_swp_mkuffd_wp(pud_t pud)
{
	return pud;
}

static inline int pud_swp_uffd_wp(pud_t pud)
{
	return 0;
}

static inline pud_t pud_swp_clear_uffd_wp(pud_t pud)
{
	return pud;
}
#endif /* CONFIG_HAVE_ARCH_USERFAULTFD_WP */

#endif /* _ASM_GENERIC_PGTABLE_UFFD_H */
