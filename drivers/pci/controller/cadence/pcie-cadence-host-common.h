/* SPDX-License-Identifier: GPL-2.0 */
// Copyright (c) 2017 Cadence
// Cadence PCIe Endpoint controller driver

#ifndef _PCIE_CADENCE_HOST_COMMON_H
#define _PCIE_CADENCE_HOST_COMMON_H

#include <linux/kernel.h>
#include <linux/pci.h>

extern u64 bar_max_size[];

int cdns_pcie_host_training_complete(struct cdns_pcie *pcie);
int cdns_pcie_host_wait_for_link(struct cdns_pcie *pcie);
int cdns_pcie_retrain(struct cdns_pcie *pcie);
int cdns_pcie_host_start_link(struct cdns_pcie_rc *rc);
enum cdns_pcie_rp_bar
cdns_pcie_host_find_min_bar(struct cdns_pcie_rc *rc, u64 size);
enum cdns_pcie_rp_bar
cdns_pcie_host_find_max_bar(struct cdns_pcie_rc *rc, u64 size);
int cdns_pcie_host_dma_ranges_cmp(void *priv, const struct list_head *a,
				  const struct list_head *b);

#endif /* _PCIE_CADENCE_HOST_COMMON_H */
