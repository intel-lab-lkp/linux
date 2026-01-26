/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * DesignWare PCIe controller helpers for integrated DesignWare eDMA.
 */

#ifndef LINUX_PCIE_DWC_EDMA_H
#define LINUX_PCIE_DWC_EDMA_H

#include <linux/dma/edma.h>
#include <linux/dmaengine.h>
#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/pci-epc.h>
#include <linux/types.h>

#ifdef CONFIG_PCIE_DW
/**
 * dwc_pcie_edma_get_reg_window() - get integrated DW eDMA register window
 * @epc:  EPC device associated with the integrated eDMA instance
 * @phys: pointer to receive the CPU-physical base address
 * @sz:   pointer to receive the size in bytes
 *
 * Some DesignWare PCIe endpoint controllers integrate a DesignWare eDMA
 * instance. Higher-level code (e.g. BAR/window setup for remote use) may
 * need the CPU-physical base and size of the eDMA register aperture.
 *
 * Return: 0 on success, -ENODEV if the EPC has no integrated eDMA register
 *         window, or -EINVAL if @epc is %NULL.
 */
int dwc_pcie_edma_get_reg_window(struct pci_epc *epc, phys_addr_t *phys,
				 resource_size_t *sz);

/**
 * dwc_pcie_edma_get_ll_region() - get linked-list (LL) region for a HW channel
 * @epc:    EPC device associated with the integrated eDMA instance
 * @dir:    DMA transfer direction (%DMA_DEV_TO_MEM or %DMA_MEM_TO_DEV)
 * @hw_id:  hardware channel identifier (equals to dw_edma_chan.id)
 * @region: pointer to a region descriptor to fill in
 *
 * Some integrated DesignWare eDMA instances allocate per-channel linked-list
 * (LL) regions for descriptor storage. This helper returns the LL region
 * corresponding to @dir and @hw_id.
 *
 * The mapping between @dir and the underlying eDMA read/write LL region
 * depends on whether the integrated eDMA instance represents a local or a
 * remote view.
 *
 * Return: 0 on success, -EINVAL on invalid arguments (including out-of-range
 *         @hw_id), or -ENODEV if the integrated eDMA instance is not present
 *         or not initialized.
 */
int dwc_pcie_edma_get_ll_region(struct pci_epc *epc,
				enum dma_transfer_direction dir, int hw_id,
				struct dw_edma_region *region);
#else
static inline int
dwc_pcie_edma_get_reg_window(struct pci_epc *epc, phys_addr_t *phys,
			     resource_size_t *sz)
{
	return -ENODEV;
}

static inline int
dwc_pcie_edma_get_ll_region(struct pci_epc *epc,
			    enum dma_transfer_direction dir, int hw_id,
			    struct dw_edma_region *region)
{
	return -ENODEV;
}
#endif /* CONFIG_PCIE_DW */

#endif /* LINUX_PCIE_DWC_EDMA_H */
