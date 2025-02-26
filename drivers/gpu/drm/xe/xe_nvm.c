// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2019-2025, Intel Corporation. All rights reserved.
 */

#include <linux/intel_dg_nvm_aux.h>
#include <linux/pci.h>

#include "xe_device_types.h"
#include "xe_nvm.h"
#include "xe_sriov.h"

#define GEN12_GUNIT_NVM_BASE 0x00102040
#define GEN12_GUNIT_NVM_SIZE 0x80
#define HECI_FW_STATUS_2_NVM_ACCESS_MODE BIT(3)

static const struct intel_dg_nvm_region regions[INTEL_DG_NVM_REGIONS] = {
	[0] = { .name = "DESCRIPTOR", },
	[2] = { .name = "GSC", },
	[11] = { .name = "OptionROM", },
	[12] = { .name = "DAM", },
};

static void xe_nvm_release_dev(struct device *dev)
{
}

void xe_nvm_init(struct xe_device *xe)
{
	struct pci_dev *pdev = to_pci_dev(xe->drm.dev);
	struct intel_dg_nvm_dev *nvm;
	struct auxiliary_device *aux_dev;
	int ret;

	if (!xe->info.has_gsc_nvm)
		return;

	/* No access to internal NVM from VFs */
	if (IS_SRIOV_VF(xe))
		return;

	/* Nvm pointer should be NULL here */
	if (WARN_ON(xe->nvm))
		return;

	xe->nvm = kzalloc(sizeof(*nvm), GFP_KERNEL);
	if (!xe->nvm)
		return;

	nvm = xe->nvm;

	nvm->writeable_override = false;
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
	aux_dev->dev.release = xe_nvm_release_dev;

	ret = auxiliary_device_init(aux_dev);
	if (ret) {
		drm_err(&xe->drm, "xe-nvm aux init failed %d\n", ret);
		return;
	}

	ret = auxiliary_device_add(aux_dev);
	if (ret) {
		drm_err(&xe->drm, "xe-nvm aux add failed %d\n", ret);
		auxiliary_device_uninit(aux_dev);
		return;
	}
}

void xe_nvm_fini(struct xe_device *xe)
{
	struct intel_dg_nvm_dev *nvm = xe->nvm;

	if (!xe->info.has_gsc_nvm)
		return;

	/* No access to internal NVM from VFs */
	if (IS_SRIOV_VF(xe))
		return;

	/* Nvm pointer should not be NULL here */
	if (WARN_ON(!nvm))
		return;

	auxiliary_device_delete(&nvm->aux_dev);
	auxiliary_device_uninit(&nvm->aux_dev);
	kfree(nvm);
	xe->nvm = NULL;
}
