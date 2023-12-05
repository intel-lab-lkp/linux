// SPDX-License-Identifier: GPL-2.0
/*
 * Implement the default iomap interfaces
 *
 * (C) Copyright 2004 Linus Torvalds
 */
#include <linux/pci.h>
#include <linux/io.h>

#include <linux/export.h>

/**
 * pci_iomap_range - create a virtual mapping cookie for a PCI BAR
 * @dev: PCI device that owns the BAR
 * @bar: BAR number
 * @offset: map memory at the given offset in BAR
 * @maxlen: max length of the memory to map
 *
 * Using this function you will get a __iomem address to your device BAR.
 * You can access it using ioread*() and iowrite*(). These functions hide
 * the details if this is a MMIO or PIO address space and will just do what
 * you expect from them in the correct way.
 *
 * @maxlen specifies the maximum length to map. If you want to get access to
 * the complete BAR from offset to the end, pass %0 here.
 * */
void __iomem *pci_iomap_range(struct pci_dev *dev,
			      int bar,
			      unsigned long offset,
			      unsigned long maxlen)
{
	resource_size_t start = pci_resource_start(dev, bar);
	resource_size_t len = pci_resource_len(dev, bar);
	unsigned long flags = pci_resource_flags(dev, bar);

	if (len <= offset || !start)
		return NULL;
	len -= offset;
	start += offset;
	if (maxlen && len > maxlen)
		len = maxlen;
	if (flags & IORESOURCE_IO)
		return __pci_ioport_map(dev, start, len);
	if (flags & IORESOURCE_MEM)
		return ioremap(start, len);
	/* What? */
	return NULL;
}
EXPORT_SYMBOL(pci_iomap_range);

/**
 * pci_iomap_wc_range - create a virtual WC mapping cookie for a PCI BAR
 * @dev: PCI device that owns the BAR
 * @bar: BAR number
 * @offset: map memory at the given offset in BAR
 * @maxlen: max length of the memory to map
 *
 * Using this function you will get a __iomem address to your device BAR.
 * You can access it using ioread*() and iowrite*(). These functions hide
 * the details if this is a MMIO or PIO address space and will just do what
 * you expect from them in the correct way. When possible write combining
 * is used.
 *
 * @maxlen specifies the maximum length to map. If you want to get access to
 * the complete BAR from offset to the end, pass %0 here.
 * */
void __iomem *pci_iomap_wc_range(struct pci_dev *dev,
				 int bar,
				 unsigned long offset,
				 unsigned long maxlen)
{
	resource_size_t start = pci_resource_start(dev, bar);
	resource_size_t len = pci_resource_len(dev, bar);
	unsigned long flags = pci_resource_flags(dev, bar);


	if (flags & IORESOURCE_IO)
		return NULL;

	if (len <= offset || !start)
		return NULL;

	len -= offset;
	start += offset;
	if (maxlen && len > maxlen)
		len = maxlen;

	if (flags & IORESOURCE_MEM)
		return ioremap_wc(start, len);

	/* What? */
	return NULL;
}
EXPORT_SYMBOL_GPL(pci_iomap_wc_range);

/**
 * pci_iomap - create a virtual mapping cookie for a PCI BAR
 * @dev: PCI device that owns the BAR
 * @bar: BAR number
 * @maxlen: length of the memory to map
 *
 * Using this function you will get a __iomem address to your device BAR.
 * You can access it using ioread*() and iowrite*(). These functions hide
 * the details if this is a MMIO or PIO address space and will just do what
 * you expect from them in the correct way.
 *
 * @maxlen specifies the maximum length to map. If you want to get access to
 * the complete BAR without checking for its length first, pass %0 here.
 * */
void __iomem *pci_iomap(struct pci_dev *dev, int bar, unsigned long maxlen)
{
	return pci_iomap_range(dev, bar, 0, maxlen);
}
EXPORT_SYMBOL(pci_iomap);

/**
 * pci_iomap_wc - create a virtual WC mapping cookie for a PCI BAR
 * @dev: PCI device that owns the BAR
 * @bar: BAR number
 * @maxlen: length of the memory to map
 *
 * Using this function you will get a __iomem address to your device BAR.
 * You can access it using ioread*() and iowrite*(). These functions hide
 * the details if this is a MMIO or PIO address space and will just do what
 * you expect from them in the correct way. When possible write combining
 * is used.
 *
 * @maxlen specifies the maximum length to map. If you want to get access to
 * the complete BAR without checking for its length first, pass %0 here.
 * */
void __iomem *pci_iomap_wc(struct pci_dev *dev, int bar, unsigned long maxlen)
{
	return pci_iomap_wc_range(dev, bar, 0, maxlen);
}
EXPORT_SYMBOL_GPL(pci_iomap_wc);

/*
 * This check is still necessary due to legacy reasons.
 *
 * TODO: Have all architectures that provide their own pci_iounmap() provide
 * iomem_is_ioport() instead. Remove this #if afterwards.
 */
#if defined(ARCH_WANTS_GENERIC_PCI_IOUNMAP)

/**
 * pci_iounmap - Unmapp a mapping
 * @dev: PCI device the mapping belongs to
 * @addr: start address of the mapping
 *
 * Unmapp a PIO or MMIO mapping.
 */
void pci_iounmap(struct pci_dev *dev, void __iomem *addr)
{
#ifdef CONFIG_HAS_IOPORT_MAP
	if (iomem_is_ioport(addr)) {
		ioport_unmap(addr);
		return;
	}
#endif

	iounmap(addr);
}
EXPORT_SYMBOL(pci_iounmap);

#endif /* ARCH_WANTS_GENERIC_PCI_IOUNMAP */
