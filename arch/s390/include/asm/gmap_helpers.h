/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  Helper functions for KVM guest address space mapping code
 *
 *    Copyright IBM Corp. 2007, 2025
 *    Author(s): Claudio Imbrenda <imbrenda@linux.ibm.com>
 */

#ifndef _ASM_S390_GMAP_HELPERS_H
#define _ASM_S390_GMAP_HELPERS_H

void __gmap_helper_zap_one(struct mm_struct *mm, unsigned long vmaddr);
void __gmap_helper_discard(struct mm_struct *mm, unsigned long vmaddr, unsigned long end);
void gmap_helper_discard(struct mm_struct *mm, unsigned long vmaddr, unsigned long end);
void __gmap_helper_set_unused(struct mm_struct *mm, unsigned long vmaddr);
int gmap_helper_disable_cow_sharing(void);

#endif /* _ASM_S390_GMAP_HELPERS_H */
