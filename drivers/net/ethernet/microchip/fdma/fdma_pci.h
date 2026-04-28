/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _FDMA_PCI_H_
#define _FDMA_PCI_H_

#include <linux/types.h>

#define FDMA_PCI_ATU_REGION_MAX 6
#define FDMA_PCI_DB_ALIGN 128
#define FDMA_PCI_DB_SIZE(mtu) ALIGN(mtu, FDMA_PCI_DB_ALIGN)

struct fdma_pci_atu;

struct fdma_pci_atu_region {
	struct fdma_pci_atu *atu;
	u64 base_addr; /* Base addr of the OB window */
	u64 limit_addr; /* Limit addr of the OB window */
	u64 target_addr; /* Host DMA address this region maps to */
	int idx;
	bool in_use;
};

struct fdma_pci_atu {
	void __iomem *addr;
	struct fdma_pci_atu_region regions[FDMA_PCI_ATU_REGION_MAX];
};

/* Initialize ATU, dividing OB space into regions. */
void fdma_pci_atu_init(struct fdma_pci_atu *atu, void __iomem *addr);

/* Unmap an ATU region, clearing its translation and disabling it. */
void fdma_pci_atu_region_unmap(struct fdma_pci_atu_region *region);

/* Map a host DMA address into a free ATU region. */
struct fdma_pci_atu_region *fdma_pci_atu_region_map(struct fdma_pci_atu *atu,
						    u64 target_addr,
						    int size);

/* Translate a host DMA address to the OB address space. */
u64 fdma_pci_atu_translate_addr(struct fdma_pci_atu_region *region, u64 addr);

#endif
