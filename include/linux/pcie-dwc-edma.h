/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * DesignWare PCIe controller helpers for integrated DesignWare eDMA.
 */

#ifndef LINUX_PCIE_DWC_EDMA_H
#define LINUX_PCIE_DWC_EDMA_H

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
#else
static inline int
dwc_pcie_edma_get_reg_window(struct pci_epc *epc, phys_addr_t *phys,
			     resource_size_t *sz)
{
	return -ENODEV;
}
#endif /* CONFIG_PCIE_DW */

#endif /* LINUX_PCIE_DWC_EDMA_H */
