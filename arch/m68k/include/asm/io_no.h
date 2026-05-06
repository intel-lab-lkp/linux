/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _M68KNOMMU_IO_H
#define _M68KNOMMU_IO_H

/*
 * Historically the raw native endian IO access macros for non-MMU m68k and
 * ColdFire have accepted any integral value as the address argument.
 * The asm-generic versions of these expect "void __iomem *" for the address,
 * so define local permissive versions here.
 */
#define __raw_readb(addr) \
    ({ u8 __v = (*(__force volatile u8 *) (addr)); __v; })
#define __raw_readw(addr) \
    ({ u16 __v = (*(__force volatile u16 *) (addr)); __v; })
#define __raw_readl(addr) \
    ({ u32 __v = (*(__force volatile u32 *) (addr)); __v; })

#define __raw_writeb(b, addr) (void)((*(__force volatile u8 *) (addr)) = (b))
#define __raw_writew(b, addr) (void)((*(__force volatile u16 *) (addr)) = (b))
#define __raw_writel(b, addr) (void)((*(__force volatile u32 *) (addr)) = (b))

#if defined(CONFIG_COLDFIRE)
/*
 * For ColdFire platforms we may need to do some extra checks for what
 * type of address range we are accessing. Include the ColdFire platform
 * definitions so we can figure out if need to do something special.
 */
#include <asm/byteorder.h>
#include <asm/coldfire.h>
#include <asm/mcfsim.h>
#endif /* CONFIG_COLDFIRE */

#if defined(IOMEMBASE)
/*
 * The ColdFire SoC internal peripherals are mapped into virtual address
 * space using the ACR registers of the cache control unit. This means we
 * are using a 1:1 physical:virtual mapping for them. We can quickly
 * determine if we are accessing an internal peripheral device given the
 * physical or vitrual address using the same range check. This check logic
 * applies just the same of there is no MMU but something like a PCI bus
 * is present.
 */
static inline int __cf_internalio(unsigned long addr)
{
	return (addr >= IOMEMBASE) && (addr <= IOMEMBASE + IOMEMSIZE - 1);
}

static inline int cf_internalio(const volatile void __iomem *addr)
{
	return __cf_internalio((unsigned long) addr);
}
#endif /* IOMEMBASE */

#if defined(CONFIG_COLDFIRE)
/*
 * The ColdFire internal peripheral registers are big-endian, so you
 * cannot use the conventional little-endian readb/readw/readl and
 * writeb/writew/writel access functions. Define a family of access
 * functions to give correct endian access that can be used by all
 * architecture code.
 */
#define mcf_read8	__raw_readb
#define mcf_read16	__raw_readw
#define mcf_read32	__raw_readl
#define mcf_write8	__raw_writeb
#define mcf_write16	__raw_writew
#define mcf_write32	__raw_writel
#endif /* CONFIG_COLDFIRE */

#if defined(CONFIG_PCI)
/*
 * Support for PCI bus access uses the asm-generic access functions.
 * We need to supply the base address and masks for the normal memory
 * and IO address space mappings.
 */
#define PCI_MEM_PA	0xf0000000		/* Host physical address */
#define PCI_MEM_BA	0xf0000000		/* Bus physical address */
#define PCI_MEM_SIZE	0x08000000		/* 128 MB */
#define PCI_MEM_MASK	(PCI_MEM_SIZE - 1)

#define PCI_IO_PA	0xf8000000		/* Host physical address */
#define PCI_IO_BA	0x00000000		/* Bus physical address */
#define PCI_IO_SIZE	0x00010000		/* 64k */
#define PCI_IO_MASK	(PCI_IO_SIZE - 1)

#define PCI_IOBASE	((void __iomem *) PCI_IO_PA)
#define PCI_SPACE_LIMIT	PCI_IO_MASK
#endif /* CONFIG_PCI */

#include <asm/kmap.h>
#include <asm/virtconvert.h>

#endif /* _M68KNOMMU_IO_H */
