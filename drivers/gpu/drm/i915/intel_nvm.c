// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2019-2024, Intel Corporation. All rights reserved.
 */

#include <linux/intel_dg_nvm_aux.h>
#include <linux/irq.h>
#include <linux/pci.h>
#include "i915_reg.h"
#include "i915_drv.h"
#include "intel_nvm.h"

#define GEN12_GUNIT_NVM_SIZE 0x80

static const struct intel_dg_nvm_region regions[INTEL_DG_NVM_REGIONS] = {
	[0] = { .name = "DESCRIPTOR", },
	[2] = { .name = "GSC", },
	[11] = { .name = "OptionROM", },
	[12] = { .name = "DAM", },
};

static void i915_nvm_release_dev(struct device *dev)
{
}

int intel_nvm_init(struct drm_i915_private *i915)
{
	struct pci_dev *pdev = to_pci_dev(i915->drm.dev);
	struct auxiliary_device *aux_dev;
	struct intel_dg_nvm_dev *nvm;
	int ret;

	/* Only the DGFX devices have internal NVM */
	if (!IS_DGFX(i915))
		return 0;

	/* Nvm pointer should be NULL here */
	if (WARN_ON(i915->nvm))
		return -EFAULT;

	i915->nvm = kzalloc(sizeof(*nvm), GFP_KERNEL);
	if (!i915->nvm)
		return -ENOMEM;

	nvm = i915->nvm;

	nvm->writable_override = true;
	nvm->bar.parent = &pdev->resource[0];
	nvm->bar.start = GEN12_GUNIT_NVM_BASE + pdev->resource[0].start;
	nvm->bar.end = nvm->bar.start + GEN12_GUNIT_NVM_SIZE - 1;
	nvm->bar.flags = IORESOURCE_MEM;
	nvm->bar.desc = IORES_DESC_NONE;
	nvm->regions = regions;

	aux_dev = &nvm->aux_dev;

	aux_dev->name = "nvm";
	aux_dev->id = (pci_domain_nr(pdev->bus) << 16) | pci_dev_id(pdev);
	aux_dev->dev.parent = &pdev->dev;
	aux_dev->dev.release = i915_nvm_release_dev;

	ret = auxiliary_device_init(aux_dev);
	if (ret) {
		drm_err(&i915->drm, "i915-nvm aux init failed %d\n", ret);
		goto err;
	}

	ret = auxiliary_device_add(aux_dev);
	if (ret) {
		drm_err(&i915->drm, "i915-nvm aux add failed %d\n", ret);
		auxiliary_device_uninit(aux_dev);
		goto err;
	}
	return 0;

err:
	kfree(nvm);
	i915->nvm = NULL;
	return ret;
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
