/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SPARC_IO_H
#define __SPARC_IO_H

#include <linux/kernel.h>
#include <linux/ioport.h>  /* struct resource */

#define IO_SPACE_LIMIT 0xffffffff

#define memset_io(d,c,sz)     _memset_io(d,c,sz)
#define memcpy_fromio(d,s,sz) _memcpy_fromio(d,s,sz)
#define memcpy_toio(d,s,sz)   _memcpy_toio(d,s,sz)

/*
 * Bus number may be embedded in the higher bits of the physical address.
 * This is why we have no bus number argument to ioremap().
 */
void __iomem *ioremap(phys_addr_t offset, size_t size);
void iounmap(volatile void __iomem *addr);

#include <asm-generic/io.h>

static inline void _memset_io(volatile void __iomem *dst,
                              int c, __kernel_size_t n)
{
	volatile void __iomem *d = dst;

	while (n--) {
		writeb(c, d);
		d++;
	}
}

static inline void _memcpy_fromio(void *dst, const volatile void __iomem *src,
                                  __kernel_size_t n)
{
	char *d = dst;

	while (n--) {
		char tmp = readb(src);
		*d++ = tmp;
		src++;
	}
}

static inline void _memcpy_toio(volatile void __iomem *dst, const void *src,
                                __kernel_size_t n)
{
	const char *s = src;
	volatile void __iomem *d = dst;

	while (n--) {
		char tmp = *s++;
		writeb(tmp, d);
		d++;
	}
}

/* Create a virtual mapping cookie for an IO port range */
void __iomem *ioport_map(unsigned long port, unsigned int nr);
void ioport_unmap(void __iomem *);

/* Create a virtual mapping cookie for a PCI BAR (memory or IO) */
struct pci_dev;
void pci_iounmap(struct pci_dev *dev, void __iomem *);

#define __ARCH_HAS_NO_PAGE_ZERO_MAPPED		1


#endif /* !(__SPARC_IO_H) */
