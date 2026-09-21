/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * arch/arm/include/asm/pgtable-2level-types.h
 *
 * Copyright (C) 1995-2003 Russell King
 */
#ifndef _ASM_PGTABLE_2LEVEL_TYPES_H
#define _ASM_PGTABLE_2LEVEL_TYPES_H

#include <asm/types.h>

typedef u32 pteval_t;
typedef u32 pmdval_t;

#undef STRICT_MM_TYPECHECKS

#ifdef STRICT_MM_TYPECHECKS
/*
 * These are used to make use of C type-checking..
 */
typedef struct { pteval_t pte; } pte_t;
typedef struct { pmdval_t pmd; } pmd_t;
typedef struct { pmdval_t pgd[2]; } pgd_t;
typedef struct { pteval_t pgprot; } pgprot_t;

#define pte_val(x)      ((x).pte)
#define pmd_val(x)      ((x).pmd)
#define pgd_val(x)      ((x).pgd[0])
#define pgprot_val(x)   ((x).pgprot)

#define __pte(x)        ((pte_t) { (x) } )
#define __pmd(x)        ((pmd_t) { (x) } )
#define __pgprot(x)     ((pgprot_t) { (x) } )

#else
/*
 * .. while these make it easier on the compiler
 */
typedef u64 pgdval_t;

typedef pteval_t pte_t;
typedef pmdval_t pmd_t;
typedef pgdval_t pgd_t;
typedef pteval_t pgprot_t;

#define pte_val(x)      (x)
#define pmd_val(x)      (x)

static inline pmdval_t pgd_val(pgd_t pgd)
{
	/*
	 * A PGD entry actually corresponds to two PMD entries in the PMD table.
	 * Both PMD entries point to PTE tables residing in the same physical page,
	 * but at different offsets. See include/asm/pgtable-2level.h for details.
	 *
	 * Historically, pgd_val() has returned the lower of the two PMD entries.
	 */
	return (*(pmdval_t (*)[2])&pgd)[0];
}

#define pgprot_val(x)   (x)

#define __pte(x)        (x)
#define __pmd(x)        (x)
#define __pgprot(x)     (x)

#endif /* STRICT_MM_TYPECHECKS */

#endif	/* _ASM_PGTABLE_2LEVEL_TYPES_H */
