/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _LINUX_PAGE_HELPERS_H
#define _LINUX_PAGE_HELPERS_H

#include <asm/page.h>

#define offset_in_page(p)	((unsigned long)(p) & ~PAGE_MASK)
#define bytes_to_page_end(p)	(PAGE_SIZE - offset_in_page(p))

#endif /* _LINUX_PAGE_HELPERS_H */
