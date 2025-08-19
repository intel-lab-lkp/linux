// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2017 Cadence
// Cadence PCIe host controller driver.

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/list_sort.h>
#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/platform_device.h>

#include "pcie-cadence.h"
#include "pcie-cadence-host-common.h"

#define LINK_RETRAIN_TIMEOUT HZ

u64 bar_max_size[] = {
	[RP_BAR0] = _ULL(128 * SZ_2G),
	[RP_BAR1] = SZ_2G,
	[RP_NO_BAR] = _BITULL(63),
};
EXPORT_SYMBOL_GPL(bar_max_size);

int cdns_pcie_host_training_complete(struct cdns_pcie *pcie)
{
	u32 pcie_cap_off = CDNS_PCIE_RP_CAP_OFFSET;
	unsigned long end_jiffies;
	u16 lnk_stat;

	/* Wait for link training to complete. Exit after timeout. */
	end_jiffies = jiffies + LINK_RETRAIN_TIMEOUT;
	do {
		lnk_stat = cdns_pcie_rp_readw(pcie, pcie_cap_off + PCI_EXP_LNKSTA);
		if (!(lnk_stat & PCI_EXP_LNKSTA_LT))
			break;
		usleep_range(0, 1000);
	} while (time_before(jiffies, end_jiffies));

	if (!(lnk_stat & PCI_EXP_LNKSTA_LT))
		return 0;

	return -ETIMEDOUT;
}
EXPORT_SYMBOL_GPL(cdns_pcie_host_training_complete);

int cdns_pcie_host_wait_for_link(struct cdns_pcie *pcie)
{
	struct device *dev = pcie->dev;
	int retries;

	/* Check if the link is up or not */
	for (retries = 0; retries < LINK_WAIT_MAX_RETRIES; retries++) {
		if (cdns_pcie_link_up(pcie)) {
			dev_info(dev, "Link up\n");
			return 0;
		}
		usleep_range(LINK_WAIT_USLEEP_MIN, LINK_WAIT_USLEEP_MAX);
	}

	return -ETIMEDOUT;
}
EXPORT_SYMBOL_GPL(cdns_pcie_host_wait_for_link);

int cdns_pcie_retrain(struct cdns_pcie *pcie)
{
	u32 lnk_cap_sls, pcie_cap_off = CDNS_PCIE_RP_CAP_OFFSET;
	u16 lnk_stat, lnk_ctl;
	int ret = 0;

	/*
	 * Set retrain bit if current speed is 2.5 GB/s,
	 * but the PCIe root port support is > 2.5 GB/s.
	 */

	lnk_cap_sls = cdns_pcie_readl(pcie, (CDNS_PCIE_RP_BASE + pcie_cap_off +
					     PCI_EXP_LNKCAP));
	if ((lnk_cap_sls & PCI_EXP_LNKCAP_SLS) <= PCI_EXP_LNKCAP_SLS_2_5GB)
		return ret;

	lnk_stat = cdns_pcie_rp_readw(pcie, pcie_cap_off + PCI_EXP_LNKSTA);
	if ((lnk_stat & PCI_EXP_LNKSTA_CLS) == PCI_EXP_LNKSTA_CLS_2_5GB) {
		lnk_ctl = cdns_pcie_rp_readw(pcie,
					     pcie_cap_off + PCI_EXP_LNKCTL);
		lnk_ctl |= PCI_EXP_LNKCTL_RL;
		cdns_pcie_rp_writew(pcie, pcie_cap_off + PCI_EXP_LNKCTL,
				    lnk_ctl);

		ret = cdns_pcie_host_training_complete(pcie);
		if (ret)
			return ret;

		ret = cdns_pcie_host_wait_for_link(pcie);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(cdns_pcie_retrain);

int cdns_pcie_host_start_link(struct cdns_pcie_rc *rc)
{
	struct cdns_pcie *pcie = &rc->pcie;
	int ret;

	ret = cdns_pcie_host_wait_for_link(pcie);

	/*
	 * Retrain link for Gen2 training defect
	 * if quirk flag is set.
	 */
	if (!ret && rc->quirk_retrain_flag)
		ret = cdns_pcie_retrain(pcie);

	return ret;
}
EXPORT_SYMBOL_GPL(cdns_pcie_host_start_link);

enum cdns_pcie_rp_bar
cdns_pcie_host_find_min_bar(struct cdns_pcie_rc *rc, u64 size)
{
	enum cdns_pcie_rp_bar bar, sel_bar;

	sel_bar = RP_BAR_UNDEFINED;
	for (bar = RP_BAR0; bar <= RP_NO_BAR; bar++) {
		if (!rc->avail_ib_bar[bar])
			continue;

		if (size <= bar_max_size[bar]) {
			if (sel_bar == RP_BAR_UNDEFINED) {
				sel_bar = bar;
				continue;
			}

			if (bar_max_size[bar] < bar_max_size[sel_bar])
				sel_bar = bar;
		}
	}

	return sel_bar;
}
EXPORT_SYMBOL_GPL(cdns_pcie_host_find_min_bar);

enum cdns_pcie_rp_bar
cdns_pcie_host_find_max_bar(struct cdns_pcie_rc *rc, u64 size)
{
	enum cdns_pcie_rp_bar bar, sel_bar;

	sel_bar = RP_BAR_UNDEFINED;
	for (bar = RP_BAR0; bar <= RP_NO_BAR; bar++) {
		if (!rc->avail_ib_bar[bar])
			continue;

		if (size >= bar_max_size[bar]) {
			if (sel_bar == RP_BAR_UNDEFINED) {
				sel_bar = bar;
				continue;
			}

			if (bar_max_size[bar] > bar_max_size[sel_bar])
				sel_bar = bar;
		}
	}

	return sel_bar;
}
EXPORT_SYMBOL_GPL(cdns_pcie_host_find_max_bar);

int cdns_pcie_host_dma_ranges_cmp(void *priv, const struct list_head *a,
				  const struct list_head *b)
{
	struct resource_entry *entry1, *entry2;

	entry1 = container_of(a, struct resource_entry, node);
	entry2 = container_of(b, struct resource_entry, node);

	return resource_size(entry2->res) - resource_size(entry1->res);
}
EXPORT_SYMBOL_GPL(cdns_pcie_host_dma_ranges_cmp);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cadence PCIe controller driver");
