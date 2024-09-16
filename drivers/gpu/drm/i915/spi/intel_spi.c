// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2019-2024, Intel Corporation. All rights reserved.
 */

#include <linux/intel_dg_spi_aux.h>
#include <linux/irq.h>
#include "i915_reg.h"
#include "i915_drv.h"
#include "spi/intel_spi.h"

#define GEN12_GUNIT_SPI_SIZE 0x80

static void i915_spi_release_dev(struct device *dev)
{
}

void intel_spi_init(struct drm_i915_private *dev_priv)
{
	struct intel_dg_spi_dev *spi = &dev_priv->spi;
	struct pci_dev *pdev = to_pci_dev(dev_priv->drm.dev);
	struct auxiliary_device *aux_dev = &spi->aux_dev;
	int ret;

	/* Only the DGFX devices have internal SPI */
	if (!IS_DGFX(dev_priv))
		return;

	spi->bar.parent = &pdev->resource[0];
	spi->bar.start = GEN12_GUNIT_SPI_BASE + pdev->resource[0].start;
	spi->bar.end = spi->bar.start + GEN12_GUNIT_SPI_SIZE - 1;
	spi->bar.flags = IORESOURCE_MEM;
	spi->bar.desc = IORES_DESC_NONE;

	aux_dev->name = "spi";
	aux_dev->id = (pci_domain_nr(pdev->bus) << 16) |
		       PCI_DEVID(pdev->bus->number, pdev->devfn);
	aux_dev->dev.parent = &pdev->dev;
	aux_dev->dev.release = i915_spi_release_dev;

	ret = auxiliary_device_init(aux_dev);
	if (ret) {
		dev_err(&pdev->dev, "i915-spi aux init failed %d\n", ret);
		return;
	}

	ret = auxiliary_device_add(aux_dev);
	if (ret) {
		dev_err(&pdev->dev, "i915-spi aux add failed %d\n", ret);
		auxiliary_device_uninit(aux_dev);
		return;
	}
}

void intel_spi_fini(struct drm_i915_private *dev_priv)
{
	struct intel_dg_spi_dev *spi = &dev_priv->spi;
	struct pci_dev *pdev = to_pci_dev(dev_priv->drm.dev);

	/* Only the DGFX devices have internal SPI */
	if (!IS_DGFX(dev_priv))
		return;

	dev_dbg(&pdev->dev, "removing i915-spi cell\n");

	auxiliary_device_delete(&spi->aux_dev);
	auxiliary_device_uninit(&spi->aux_dev);
}
