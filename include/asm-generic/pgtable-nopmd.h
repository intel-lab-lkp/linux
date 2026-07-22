/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _PGTABLE_NOPMD_H
#define _PGTABLE_NOPMD_H

#ifndef __ASSEMBLER__

#include <asm-generic/pgtable-nopud.h>

struct mm_struct;

#define __PAGETABLE_PMD_FOLDED 1

/*
 * Having the pmd type consist of a pud gets the size right, and allows
 * us to conceptually access the pud entry that this pmd is folded into
 * without casting.
 */
typedef struct { pud_t pud; } pmd_t;

#define PMD_SHIFT	PUD_SHIFT
#define PTRS_PER_PMD	1
#define PMD_SIZE  	(1UL << PMD_SHIFT)
#define PMD_MASK  	(~(PMD_SIZE-1))

/*
 * The "pud_xxx()" functions here are trivial for a folded two-level
 * setup: the pmd is never bad, and a pmd always exists (as it's folded
 * into the pud entry)
 */
static inline int pud_none(pud_t pud)		{ return 0; }
static inline int pud_bad(pud_t pud)		{ return 0; }
static inline int pud_present(pud_t pud)	{ return 1; }
static inline int pud_user(pud_t pud)		{ return 0; }
static inline bool pud_leaf(pud_t pud)		{ return false; }
#define pud_leaf pud_leaf
static inline void pud_clear(pud_t *pud)	{ }
#define pmd_ERROR(pmd)				(pud_ERROR((pmd).pud))

#define pud_populate(mm, pmd, pte)		do { } while (0)

#define set_pud(pudptr, pudval)						\
({									\
	pud_check_dummy(pudval);					\
	set_pmd((pmd_t *)(pudptr), (pmd_t) { pudval });			\
})

static __always_inline pud_t pudp_get(pud_t *pudp)
{
	pud_t dummy = { 0 };

	return dummy;
}
#define pudp_get pudp_get

#define pud_check_dummy(pud) BUILD_BUG_ON(__builtin_constant_p(pud_val(pud)))

static __always_inline pmd_t *__pmd_offset(pud_t *pudp, unsigned long address)
{
	return (pmd_t *)pudp;
}

#define pmd_offset(pudp, address)					\
({									\
	pud_check_dummy(*(pudp));					\
	__pmd_offset(pudp, address);					\
})

static __always_inline pmd_t *__pmd_offset_lockless(pud_t *pudp, pud_t pud,
		unsigned long address)
{
	return (pmd_t *)pudp;
}

#define pmd_offset_lockless(pudp, pud, address)				\
({									\
	pud_check_dummy(*(pudp));					\
	__pmd_offset_lockless(pudp, pud, address);			\
})

#define pmd_val(x)				(pud_val((x).pud))
#define __pmd(x)				((pmd_t) { __pud(x) } )

#define pud_page(pud)				({ BUILD_BUG(); (struct page *)NULL; })
#define pud_pgtable(pud)						\
({									\
	pud_check_dummy(pud);						\
	((pmd_t *)(pmd_page_vaddr((pmd_t) { pud })));			\
})

/*
 * allocating and freeing a pmd is trivial: the 1-entry pmd is
 * inside the pud, so has no extra memory associated with it.
 */
#define pmd_alloc_one(mm, address)		NULL
static inline void pmd_free(struct mm_struct *mm, pmd_t *pmd)
{
}
#define pmd_free_tlb(tlb, x, a)		do { } while (0)

#undef  pmd_addr_end
#define pmd_addr_end(addr, end)			(end)

#endif /* __ASSEMBLER__ */

#endif /* _PGTABLE_NOPMD_H */
