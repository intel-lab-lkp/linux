// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2019-2024, Intel Corporation. All rights reserved.
 */

#include <linux/intel_dg_nvm_aux.h>
#include <linux/irq.h>
#include "i915_reg.h"
#include "i915_drv.h"
#include "intel_nvm.h"

#define GEN12_GUNIT_NVM_SIZE 0x80
#define HECI_FW_STATUS_2_NVM_ACCESS_MODE BIT(3)

static const struct intel_dg_nvm_region regions[INTEL_DG_NVM_REGIONS] = {
	[0] = { .name = "DESCRIPTOR", },
	[2] = { .name = "GSC", },
	[11] = { .name = "OptionROM", },
	[12] = { .name = "DAM", },
};

static void i915_nvm_release_dev(struct device *dev)
{
}

static bool i915_nvm_writeable_override(struct drm_i915_private *i915)
{
	resource_size_t base;
	bool writeable_override;

	if (IS_DG1(i915)) {
		base = DG1_GSC_HECI2_BASE;
	} else if (IS_DG2(i915)) {
		base = DG2_GSC_HECI2_BASE;
	} else {
		drm_err(&i915->drm, "Unknown platform\n");
		return true;
	}

	writeable_override =
		!(intel_uncore_read(&i915->uncore, HECI_FWSTS(base, 2)) &
		  HECI_FW_STATUS_2_NVM_ACCESS_MODE);
	if (writeable_override)
		drm_info(&i915->drm, "NVM access overridden by jumper\n");
	return writeable_override;
}

void intel_nvm_init(struct drm_i915_private *i915)
{
	struct pci_dev *pdev = to_pci_dev(i915->drm.dev);
	struct intel_dg_nvm_dev *nvm;
	struct auxiliary_device *aux_dev;
	int ret;

	/* Only the DGFX devices have internal NVM */
	if (!IS_DGFX(i915))
		return;

	/* Nvm pointer should be NULL here */
	if (WARN_ON(i915->nvm))
		return;

	i915->nvm = kzalloc(sizeof(*nvm), GFP_KERNEL);
	if (!i915->nvm)
		return;

	nvm = i915->nvm;

	nvm->writeable_override = i915_nvm_writeable_override(i915);
	nvm->bar.parent = &pdev->resource[0];
	nvm->bar.start = GEN12_GUNIT_NVM_BASE + pdev->resource[0].start;
	nvm->bar.end = nvm->bar.start + GEN12_GUNIT_NVM_SIZE - 1;
	nvm->bar.flags = IORESOURCE_MEM;
	nvm->bar.desc = IORES_DESC_NONE;
	nvm->regions = regions;

	aux_dev = &nvm->aux_dev;

	aux_dev->name = "nvm";
	aux_dev->id = (pci_domain_nr(pdev->bus) << 16) |
		       PCI_DEVID(pdev->bus->number, pdev->devfn);
	aux_dev->dev.parent = &pdev->dev;
	aux_dev->dev.release = i915_nvm_release_dev;

	ret = auxiliary_device_init(aux_dev);
	if (ret) {
		drm_err(&i915->drm, "i915-nvm aux init failed %d\n", ret);
		return;
	}

	ret = auxiliary_device_add(aux_dev);
	if (ret) {
		drm_err(&i915->drm, "i915-nvm aux add failed %d\n", ret);
		auxiliary_device_uninit(aux_dev);
		return;
	}
}

void intel_nvm_fini(struct drm_i915_private *i915)
{
	struct intel_dg_nvm_dev *nvm = i915->nvm;

	/* Only the DGFX devices have internal NVM */
	if (!IS_DGFX(i915))
		return;

	/* Nvm pointer should not be NULL here */
	if (WARN_ON(!nvm))
		return;

	auxiliary_device_delete(&nvm->aux_dev);
	auxiliary_device_uninit(&nvm->aux_dev);
	kfree(nvm);
	i915->nvm = NULL;
}
