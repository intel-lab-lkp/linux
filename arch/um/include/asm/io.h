/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_UM_IO_H
#define _ASM_UM_IO_H
#include <linux/types.h>

/* get emulated iomem (if desired) */
#include <asm-generic/logic_io.h>

#ifndef ioremap
#define ioremap ioremap
static inline void __iomem *ioremap(phys_addr_t offset, size_t size)
{
	return NULL;
}
#endif /* ioremap */

#ifndef iounmap
#define iounmap iounmap
static inline void iounmap(void __iomem *addr)
{
}
#endif /* iounmap */

static inline void memset_io(volatile void __iomem *dst, int c, size_t count)
{
	memset((void *)dst, c, count);
}

static inline void memcpy_toio(volatile void __iomem *to, const void *from,
			       size_t count)
{
	memcpy((void *)to, from, count);
}

static inline void memcpy_fromio(void *to, const volatile void __iomem *from,
				 size_t count)
{
	memcpy(to, (void *)from, count);
}

#include <asm-generic/io.h>

#endif
