// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe TLP Log handling
 *
 * Copyright (C) 2024 Intel Corporation
 */

#include <linux/aer.h>
#include <linux/array_size.h>
#include <linux/pci.h>
#include <linux/string.h>

#include "../pci.h"

/**
 * aer_tlp_log_len - Calculates AER Capability TLP Header/Prefix Log length
 * @dev: PCIe device
 *
 * Return: TLP Header/Prefix Log length
 */
unsigned int aer_tlp_log_len(struct pci_dev *dev)
{
	return 4 + dev->eetlp_prefix_max;
}

#ifdef CONFIG_PCIE_DPC
/**
 * dpc_tlp_log_len - Calculates DPC RP PIO TLP Header/Prefix Log length
 * @dev: PCIe device
 *
 * Return: TLP Header/Prefix Log length
 */
unsigned int dpc_tlp_log_len(struct pci_dev *dev)
{
	/* Remove ImpSpec Log register from the count */
	if (dev->dpc_rp_log_size >= 5)
		return dev->dpc_rp_log_size - 1;

	return dev->dpc_rp_log_size;
}
#endif

/**
 * pcie_read_tlp_log - read TLP Header Log
 * @dev: PCIe device
 * @where: PCI Config offset of TLP Header Log
 * @where2: PCI Config offset of TLP Prefix Log
 * @tlp_len: TLP Log length (Header Log + TLP Prefix Log in DWORDs)
 * @log: TLP Log structure to fill
 *
 * Fill @log from TLP Header Log registers, e.g., AER or DPC.
 *
 * Return: 0 on success and filled TLP Log structure, <0 on error.
 */
int pcie_read_tlp_log(struct pci_dev *dev, int where, int where2,
		      unsigned int tlp_len, struct pcie_tlp_log *log)
{
	unsigned int i;
	int off, ret;
	u32 *to;

	memset(log, 0, sizeof(*log));

	for (i = 0; i < tlp_len; i++) {
		if (i < 4) {
			off = where + i * 4;
			to = &log->dw[i];
		} else {
			off = where2 + (i - 4) * 4;
			to = &log->prefix[i - 4];
		}

		ret = pci_read_config_dword(dev, off, to);
		if (ret)
			return pcibios_err_to_errno(ret);
	}

	return 0;
}

/**
 * pcie_print_tlp_log - Print TLP Header / Prefix Log contents
 * @dev: PCIe device
 * @log: TLP Log structure
 * @pfx: String prefix (for print out indentation)
 *
 * Prints TLP Header and Prefix Log information held by @log.
 */
void pcie_print_tlp_log(const struct pci_dev *dev,
			const struct pcie_tlp_log *log, const char *pfx)
{
	char buf[(10 + 1) * (4 + ARRAY_SIZE(log->prefix)) + 14 + 1];
	unsigned int i;
	int len;

	len = scnprintf(buf, sizeof(buf), "%#010x %#010x %#010x %#010x",
			log->dw[0], log->dw[1], log->dw[2], log->dw[3]);

	if (log->prefix[0])
		len += scnprintf(buf + len, sizeof(buf) - len, " E-E Prefixes:");
	for (i = 0; i < ARRAY_SIZE(log->prefix); i++) {
		if (!log->prefix[i])
			break;
		len += scnprintf(buf + len, sizeof(buf) - len,
				 " %#010x", log->prefix[i]);
	}

	pci_err(dev, "%sTLP Header: %s\n", pfx, buf);
}
