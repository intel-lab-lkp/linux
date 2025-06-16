/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_PAGE_32_H
#define _ASM_X86_PAGE_32_H

#include <asm/page_32_types.h>

#ifndef __ASSEMBLER__

#define __phys_addr_nodebug(x)	((x) - PAGE_OFFSET)
#ifdef CONFIG_DEBUG_VIRTUAL
extern unsigned long __phys_addr(unsigned long);
#else
#define __phys_addr(x)		__phys_addr_nodebug(x)
#endif
#define __phys_addr_symbol(x)	__phys_addr(x)
#define __phys_reloc_hide(x)	RELOC_HIDE((x), 0)

#include <linux/string.h>

/*
 * clear_pages() - clear kernel page range.
 * @addr: page aligned pointer
 * @npages: number of pages
 *
 * Assumes that (@addr, +@npage) references a kernel region.
 * Does absolutely no exception handling.
 */
static inline void clear_pages(void *addr, u64 npages)
{
	for (u64 i = 0; i < npages; i++)
		memset(addr + i * PAGE_SIZE, 0, PAGE_SIZE);
}

static inline void clear_page(void *addr)
{
	clear_pages(addr, 1);
}

static inline void copy_page(void *to, void *from)
{
	memcpy(to, from, PAGE_SIZE);
}
#endif	/* !__ASSEMBLER__ */

#endif /* _ASM_X86_PAGE_32_H */
