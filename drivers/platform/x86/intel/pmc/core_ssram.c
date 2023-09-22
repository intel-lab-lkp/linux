// SPDX-License-Identifier: GPL-2.0
/*
 * This file contains functions to handle discovery of PMC metrics located
 * in the PMC SSRAM PCI device.
 *
 * Copyright (c) 2023, Intel Corporation.
 * All Rights Reserved.
 *
 */

#include <linux/pci.h>
#include <linux/io-64-nonatomic-lo-hi.h>

#include "core.h"

#define SSRAM_HDR_SIZE		0x100
#define SSRAM_PWRM_OFFSET	0x14
#define SSRAM_DVSEC_OFFSET	0x1C
#define SSRAM_DVSEC_SIZE	0x10
#define SSRAM_PCH_OFFSET	0x60
#define SSRAM_IOE_OFFSET	0x68
#define SSRAM_DEVID_OFFSET	0x70

static const struct pmc_reg_map *pmc_core_find_regmap(struct pmc_info *list, u16 devid)
{
	for (; list->map; ++list)
		if (devid == list->devid)
			return list->map;

	return NULL;
}

static inline u64 get_base(void __iomem *addr, u32 offset)
{
	return lo_hi_readq(addr + offset) & GENMASK_ULL(63, 3);
}

static int
pmc_core_pmc_add(struct pmc_dev *pmcdev, u64 pwrm_base,
		 const struct pmc_reg_map *reg_map, int pmc_index)
{
	struct pmc *pmc = pmcdev->pmcs[pmc_index];

	if (!pwrm_base)
		return -ENODEV;

	/* Memory for primary PMC has been allocated in core.c */
	if (!pmc) {
		pmc = devm_kzalloc(&pmcdev->pdev->dev, sizeof(*pmc), GFP_KERNEL);
		if (!pmc)
			return -ENOMEM;
	}

	pmc->map = reg_map;
	pmc->base_addr = pwrm_base;
	pmc->regbase = ioremap(pmc->base_addr, pmc->map->regmap_length);

	if (!pmc->regbase) {
		devm_kfree(&pmcdev->pdev->dev, pmc);
		return -ENOMEM;
	}

	pmcdev->pmcs[pmc_index] = pmc;

	return 0;
}

static int
pmc_core_get_secondary_pmc(struct pmc_dev *pmcdev, int pmc_idx, u32 offset)
{
	struct pci_dev *ssram_pcidev = pmcdev->ssram_pcidev;
	const struct pmc_reg_map *map;
	void __iomem *main_ssram, *secondary_ssram;
	u64 ssram_base, pwrm_base;
	u16 devid;
	int ret;

	if (!pmcdev->regmap_list)
		return -ENOENT;

	/*
	 * The secondary PMC BARS (which are behind hidden PCI devices) are read
	 * from fixed offsets in MMIO of the primary PMC BAR.
	 */
	ssram_base = ssram_pcidev->resource[0].start;
	main_ssram = ioremap(ssram_base, SSRAM_HDR_SIZE);
	if (!main_ssram)
		return -ENOMEM;

	ssram_base = get_base(main_ssram, offset);
	secondary_ssram = ioremap(ssram_base, SSRAM_HDR_SIZE);
	if (!secondary_ssram) {
		ret = -ENOMEM;
		goto secondary_remap_fail;
	}

	pwrm_base = get_base(secondary_ssram, SSRAM_PWRM_OFFSET);
	devid = readw(secondary_ssram + SSRAM_DEVID_OFFSET);

	map = pmc_core_find_regmap(pmcdev->regmap_list, devid);
	if (!map) {
		ret = -ENODEV;
		goto find_regmap_fail;
	}

	ret = pmc_core_pmc_add(pmcdev, pwrm_base, map, pmc_idx);

find_regmap_fail:
	iounmap(secondary_ssram);
secondary_remap_fail:
	iounmap(main_ssram);

	return ret;

}

static int
pmc_core_get_primary_pmc(struct pmc_dev *pmcdev)
{
	struct pci_dev *ssram_pcidev = pmcdev->ssram_pcidev;
	const struct pmc_reg_map *map;
	void __iomem *ssram;
	u64 ssram_base, pwrm_base;
	u16 devid;
	int ret;

	if (!pmcdev->regmap_list)
		return -ENOENT;

	/* The primary PMC (SOC die) BAR is BAR 0 in config space. */
	ssram_base = ssram_pcidev->resource[0].start;
	ssram = ioremap(ssram_base, SSRAM_HDR_SIZE);
	if (!ssram)
		return -ENOMEM;

	pwrm_base = get_base(ssram, SSRAM_PWRM_OFFSET);
	devid = readw(ssram + SSRAM_DEVID_OFFSET);

	map = pmc_core_find_regmap(pmcdev->regmap_list, devid);
	if (!map) {
		ret = -ENODEV;
		goto find_regmap_fail;
	}

	ret = pmc_core_pmc_add(pmcdev, pwrm_base, map, PMC_IDX_MAIN);

find_regmap_fail:
	iounmap(ssram);

	return ret;
}

int pmc_core_ssram_init(struct pmc_dev *pmcdev)
{
	struct pci_dev *pcidev;
	int ret;

	pcidev = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(20, 2));
	if (!pcidev)
		return -ENODEV;

	ret = pcim_enable_device(pcidev);
	if (ret)
		goto release_dev;

	pmcdev->ssram_pcidev = pcidev;

	ret = pmc_core_get_primary_pmc(pmcdev);
	if (ret)
		goto disable_dev;

	pmc_core_get_secondary_pmc(pmcdev, PMC_IDX_IOE, SSRAM_IOE_OFFSET);
	pmc_core_get_secondary_pmc(pmcdev, PMC_IDX_PCH, SSRAM_PCH_OFFSET);

	return 0;

disable_dev:
	pmcdev->ssram_pcidev = NULL;
	pci_disable_device(pcidev);
release_dev:
	pci_dev_put(pcidev);

	return ret;
}
