// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2019-2024, Intel Corporation. All rights reserved.
 */

#include <linux/intel_dg_spi_aux.h>
#include <linux/pci.h>
#include "xe_device.h"
#include "xe_device_types.h"
#include "xe_mmio.h"
#include "regs/xe_gsc_regs.h"
#include "xe_spi.h"
#include "xe_sriov.h"

#define GEN12_GUNIT_SPI_BASE 0x00102040
#define GEN12_GUNIT_SPI_SIZE 0x80
#define HECI_FW_STATUS_2_SPI_ACCESS_MODE BIT(3)

static const struct intel_dg_spi_region regions[INTEL_DG_SPI_REGIONS] = {
	[0] = { .name = "DESCRIPTOR", },
	[2] = { .name = "GSC", },
	[11] = { .name = "OptionROM", },
	[12] = { .name = "DAM", },
};

static void xe_spi_release_dev(struct device *dev)
{
}

static bool xe_spi_writeable_override(struct xe_device *xe)
{
	struct xe_gt *gt = xe_root_mmio_gt(xe);
	struct pci_dev *pdev = to_pci_dev(xe->drm.dev);
	resource_size_t base;
	bool writeable_override;

	if (xe->info.platform == XE_BATTLEMAGE) {
		base = DG2_GSC_HECI2_BASE;
	} else if (xe->info.platform == XE_PVC) {
		base = PVC_GSC_HECI2_BASE;
	} else if (xe->info.platform == XE_DG2) {
		base = DG2_GSC_HECI2_BASE;
	} else if (xe->info.platform == XE_DG1) {
		base = DG1_GSC_HECI2_BASE;
	} else {
		dev_err(&pdev->dev, "Unknown platform\n");
		return true;
	}

	writeable_override =
		!(xe_mmio_read32(&gt->mmio, HECI_H_GS1(base)) &
		  HECI_FW_STATUS_2_SPI_ACCESS_MODE);
	if (writeable_override)
		dev_info(&pdev->dev, "SPI access overridden by jumper\n");
	return writeable_override;
}

void xe_spi_init(struct xe_device *xe)
{
	struct intel_dg_spi_dev *spi = &xe->spi;
	struct pci_dev *pdev = to_pci_dev(xe->drm.dev);
	struct auxiliary_device *aux_dev = &spi->aux_dev;
	int ret;

	if (!HAS_GSC_SPI(xe))
		return;

	/* No access to internal SPI from VFs */
	if (IS_SRIOV_VF(xe))
		return;

	spi->writeable_override = xe_spi_writeable_override(xe);
	spi->bar.parent = &pdev->resource[0];
	spi->bar.start = GEN12_GUNIT_SPI_BASE + pdev->resource[0].start;
	spi->bar.end = spi->bar.start + GEN12_GUNIT_SPI_SIZE - 1;
	spi->bar.flags = IORESOURCE_MEM;
	spi->bar.desc = IORES_DESC_NONE;
	spi->regions = regions;

	aux_dev->name = "spi";
	aux_dev->id = (pci_domain_nr(pdev->bus) << 16) |
		       PCI_DEVID(pdev->bus->number, pdev->devfn);
	aux_dev->dev.parent = &pdev->dev;
	aux_dev->dev.release = xe_spi_release_dev;

	ret = auxiliary_device_init(aux_dev);
	if (ret) {
		dev_err(&pdev->dev, "xe-spi aux init failed %d\n", ret);
		return;
	}

	ret = auxiliary_device_add(aux_dev);
	if (ret) {
		dev_err(&pdev->dev, "xe-spi aux add failed %d\n", ret);
		auxiliary_device_uninit(aux_dev);
		return;
	}
}

void xe_spi_fini(struct xe_device *xe)
{
	struct intel_dg_spi_dev *spi = &xe->spi;

	if (!HAS_GSC_SPI(xe))
		return;

	/* No access to internal SPI from VFs */
	if (IS_SRIOV_VF(xe))
		return;

	auxiliary_device_delete(&spi->aux_dev);
	auxiliary_device_uninit(&spi->aux_dev);
}
