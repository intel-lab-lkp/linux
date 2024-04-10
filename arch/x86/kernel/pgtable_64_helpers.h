/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PGTABLES_64_H__
#define __PGTABLES_64_H__

#ifdef __ASSEMBLY__

#define l4_index(x)	(((x) >> 39) & 511)
#define pud_index(x)	(((x) >> PUD_SHIFT) & (PTRS_PER_PUD-1))

L4_PAGE_OFFSET = l4_index(__PAGE_OFFSET_BASE_L4)
L4_START_KERNEL = l4_index(__START_KERNEL_map)

L3_START_KERNEL = pud_index(__START_KERNEL_map)

#define SYM_DATA_START_PAGE_ALIGNED(name)			\
	SYM_START(name, SYM_L_GLOBAL, .balign PAGE_SIZE)

/* Automate the creation of 1 to 1 mapping pmd entries */
#define PMDS(START, PERM, COUNT)			\
	i = 0 ;						\
	.rept (COUNT) ;					\
	.quad	(START) + (i << PMD_SHIFT) + (PERM) ;	\
	i = i + 1 ;					\
	.endr

#endif /* __ASSEMBLY__ */

#endif /* __PGTABLES_64_H__ */
