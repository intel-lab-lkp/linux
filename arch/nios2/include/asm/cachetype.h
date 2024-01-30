/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_NIOS2_CACHETYPE_H
#define __ASM_NIOS2_CACHETYPE_H

#include <asm/page.h>
#include <asm/cache.h>

#define dcache_is_aliasing()		(NIOS2_DCACHE_SIZE > PAGE_SIZE)

#endif
