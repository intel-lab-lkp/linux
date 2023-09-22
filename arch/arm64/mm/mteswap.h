/* SPDX-License-Identifier: GPL-2.0 */

#ifndef ARCH_ARM64_MM_MTESWAP_H_
#define ARCH_ARM64_MM_MTESWAP_H_

struct page;

void *_mte_alloc_and_save_tags(struct page *page);
void _mte_free_saved_tags(void *tags);
void _mte_restore_tags(void *tags, struct page *page);

#endif // ARCH_ARM64_MM_MTESWAP_H_
