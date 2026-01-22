// SPDX-License-Identifier: GPL-2.0-only
/*
 * Tracepoints for PCI system
 *
 * Copyright (C) 2025 Alibaba Corporation
 */

#include <linux/pci.h>

#define CREATE_TRACE_POINTS
#include <trace/events/pci.h>
#include <trace/events/pci_controller.h>

static atomic_t pcie_ltssm_tp_enabled = ATOMIC_INIT(0);

bool pci_ltssm_tp_enabled(void)
{
	return atomic_read(&pcie_ltssm_tp_enabled) > 0;
}
EXPORT_SYMBOL(pci_ltssm_tp_enabled);

int pci_ltssm_tp_reg(void)
{
	atomic_inc(&pcie_ltssm_tp_enabled);
	return 0;
}

void pci_ltssm_tp_unreg(void)
{
	atomic_dec(&pcie_ltssm_tp_enabled);
}
