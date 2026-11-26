// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2023, Intel Corporation. All rights reserved.
 */

#include <linux/irq.h>
#include <linux/mei_aux.h>
#include <linux/pci.h>
#include <linux/sizes.h>

#include <drm/drm_print.h>

#include "xe_bo.h"
#include "xe_device.h"
#include "xe_device_types.h"
#include "xe_heci_gsc.h"
#include "xe_map.h"
#include "regs/xe_gsc_regs.h"
#include "xe_platform_types.h"
#include "xe_survivability_mode.h"

#define GSC_BAR_LENGTH  0x00000FFC

static void heci_gsc_irq_mask(struct irq_data *d)
{
	/* generic irq handling */
}

static void heci_gsc_irq_unmask(struct irq_data *d)
{
	/* generic irq handling */
}

static const struct irq_chip heci_gsc_irq_chip = {
	.name = "gsc_irq_chip",
	.irq_mask = heci_gsc_irq_mask,
	.irq_unmask = heci_gsc_irq_unmask,
};

static int heci_gsc_irq_init(int irq)
{
	irq_set_chip_and_handler_name(irq, &heci_gsc_irq_chip,
				      handle_simple_irq, "heci_gsc_irq_handler");

	return irq_set_chip_data(irq, NULL);
}

/**
 * struct heci_gsc_def - graphics security controller heci interface definitions
 *
 * @name: name of the heci device
 * @bar: address of the mmio bar
 * @bar_size: size of the mmio bar
 * @use_polling: indication of using polling mode for the device
 * @slow_firmware: indication of whether the device is slow (needs longer timeouts)
 * @lmem_size: size of the extended operational memory the GSC needs, in VRAM
 */
struct heci_gsc_def {
	const char *name;
	unsigned long bar;
	size_t bar_size;
	bool use_polling;
	bool slow_firmware;
	size_t lmem_size;
};

/* gsc resources and definitions */
static const struct heci_gsc_def heci_gsc_def_dg1 = {
	.name = "mei-gscfi",
	.bar = DG1_GSC_HECI2_BASE,
	.bar_size = GSC_BAR_LENGTH,
};

static const struct heci_gsc_def heci_gsc_def_dg2 = {
	.name = "mei-gscfi",
	.bar = DG2_GSC_HECI2_BASE,
	.bar_size = GSC_BAR_LENGTH,
};

/* HECI1 carries the PXP client that loads and authenticates the HuC on DG2 */
static const struct heci_gsc_def heci_gsc_def_dg2_heci1 = {
	.name = "mei-gsc",
	.bar = DG2_GSC_HECI1_BASE,
	.bar_size = GSC_BAR_LENGTH,
	.lmem_size = SZ_4M,
};

static const struct heci_gsc_def heci_gsc_def_pvc = {
	.name = "mei-gscfi",
	.bar = PVC_GSC_HECI2_BASE,
	.bar_size = GSC_BAR_LENGTH,
	.slow_firmware = true,
};

static void heci_gsc_release_dev(struct device *dev)
{
	struct auxiliary_device *aux_dev = to_auxiliary_dev(dev);
	struct mei_aux_device *adev = auxiliary_dev_to_mei_aux_dev(aux_dev);

	kfree(adev);
}

static void heci_gsc_intf_fini(struct xe_heci_gsc_intf *intf)
{
	if (intf->adev) {
		struct auxiliary_device *aux_dev = &intf->adev->aux_dev;

		auxiliary_device_delete(aux_dev);
		auxiliary_device_uninit(aux_dev);
		intf->adev = NULL;
	}

	if (intf->irq >= 0) {
		int irq = intf->irq;

		/* the interrupt handler checks it first */
		intf->irq = -1;
		irq_free_desc(irq);
	}

	if (intf->ext_op_mem) {
		xe_bo_unpin_map_no_vm(intf->ext_op_mem);
		intf->ext_op_mem = NULL;
	}
}

static void xe_heci_gsc_fini(void *arg)
{
	struct xe_heci_gsc *heci_gsc = arg;

	heci_gsc_intf_fini(&heci_gsc->intf[XE_HECI1]);
	heci_gsc_intf_fini(&heci_gsc->intf[XE_HECI2]);
}

static int heci_gsc_irq_setup(struct xe_device *xe, struct xe_heci_gsc_intf *intf)
{
	int ret;

	intf->irq = irq_alloc_desc(0);
	if (intf->irq < 0) {
		drm_err(&xe->drm, "gsc irq error %d\n", intf->irq);
		return intf->irq;
	}

	ret = heci_gsc_irq_init(intf->irq);
	if (ret < 0)
		drm_err(&xe->drm, "gsc irq init failed %d\n", ret);

	return ret;
}

static int heci_gsc_ext_op_mem_alloc(struct xe_device *xe, struct xe_heci_gsc_intf *intf,
				     struct mei_aux_device *adev, size_t size)
{
	struct xe_tile *tile = xe_device_get_root_tile(xe);
	struct xe_bo *bo;

	/* pinned VRAM BOs are contiguous, the GSC only gets a base and a limit */
	bo = xe_bo_create_pin_map_novm(xe, tile, size, ttm_bo_type_kernel,
				       XE_BO_FLAG_VRAM_IF_DGFX(tile), false);
	if (IS_ERR(bo))
		return PTR_ERR(bo);

	xe_map_memset(xe, &bo->vmap, 0, 0, size);
	intf->ext_op_mem = bo;

	adev->ext_op_mem.start = xe_bo_main_addr(bo, PAGE_SIZE);
	adev->ext_op_mem.end = adev->ext_op_mem.start + size;

	drm_dbg(&xe->drm, "GSC HECI1: extended operational memory at DPA %pa\n",
		&adev->ext_op_mem.start);

	return 0;
}

static int heci_gsc_add_device(struct xe_device *xe, const struct heci_gsc_def *def,
			       enum xe_heci_intf intf_id)
{
	struct xe_heci_gsc_intf *intf = &xe->heci_gsc.intf[intf_id];
	struct pci_dev *pdev = to_pci_dev(xe->drm.dev);
	struct auxiliary_device *aux_dev;
	struct mei_aux_device *adev;
	int ret;

	if (!def->use_polling && !xe_survivability_mode_is_boot_enabled(xe)) {
		ret = heci_gsc_irq_setup(xe, intf);
		if (ret)
			return ret;
	}

	adev = kzalloc_obj(*adev);
	if (!adev)
		return -ENOMEM;

	if (def->lmem_size) {
		ret = heci_gsc_ext_op_mem_alloc(xe, intf, adev, def->lmem_size);
		if (ret) {
			drm_err(&xe->drm, "gsc extended operational memory failed %d\n", ret);
			kfree(adev);
			return ret;
		}
	}

	adev->irq = intf->irq;
	adev->bar.parent = &pdev->resource[0];
	adev->bar.start = def->bar + pdev->resource[0].start;
	adev->bar.end = adev->bar.start + def->bar_size - 1;
	adev->bar.flags = IORESOURCE_MEM;
	adev->bar.desc = IORES_DESC_NONE;
	adev->slow_firmware = def->slow_firmware;

	aux_dev = &adev->aux_dev;
	aux_dev->name = def->name;
	aux_dev->id = (pci_domain_nr(pdev->bus) << 16) |
		      PCI_DEVID(pdev->bus->number, pdev->devfn);
	aux_dev->dev.parent = &pdev->dev;
	aux_dev->dev.release = heci_gsc_release_dev;

	ret = auxiliary_device_init(aux_dev);
	if (ret < 0) {
		drm_err(&xe->drm, "gsc aux init failed %d\n", ret);
		kfree(adev);
		return ret;
	}

	intf->adev = adev; /* needed by the notifier */
	ret = auxiliary_device_add(aux_dev);
	if (ret < 0) {
		drm_err(&xe->drm, "gsc aux add failed %d\n", ret);
		intf->adev = NULL;

		/* adev will be freed with the put_device() and .release sequence */
		auxiliary_device_uninit(aux_dev);
	}
	return ret;
}

int xe_heci_gsc_init(struct xe_device *xe)
{
	struct xe_heci_gsc *heci_gsc = &xe->heci_gsc;
	const struct heci_gsc_def *def = NULL;
	int ret;

	if (!xe->info.has_heci_gscfi && !xe->info.has_heci_cscfi)
		return 0;

	heci_gsc->intf[XE_HECI1].irq = -1;
	heci_gsc->intf[XE_HECI2].irq = -1;

	if (xe->info.platform == XE_BATTLEMAGE) {
		def = &heci_gsc_def_dg2;
	} else if (xe->info.platform == XE_PVC) {
		def = &heci_gsc_def_pvc;
	} else if (xe->info.platform == XE_DG2) {
		def = &heci_gsc_def_dg2;
	} else if (xe->info.platform == XE_DG1) {
		def = &heci_gsc_def_dg1;
	}

	if (!def || !def->name) {
		drm_warn(&xe->drm, "HECI is not implemented!\n");
		return 0;
	}

	ret = devm_add_action_or_reset(xe->drm.dev, xe_heci_gsc_fini, heci_gsc);
	if (ret)
		return ret;

	ret = heci_gsc_add_device(xe, def, XE_HECI2);
	if (ret)
		return ret;

	/*
	 * HECI1 failing only costs the HuC (it stays unauthenticated), so it
	 * must not fail the probe.
	 */
	if (xe->info.platform == XE_DG2 && !xe_survivability_mode_is_boot_enabled(xe) &&
	    heci_gsc_add_device(xe, &heci_gsc_def_dg2_heci1, XE_HECI1)) {
		drm_warn(&xe->drm, "GSC HECI1 not available, HuC will not be authenticated\n");
		heci_gsc_intf_fini(&heci_gsc->intf[XE_HECI1]);
	}

	return 0;
}

static void heci_gsc_intf_irq(struct xe_device *xe, struct xe_heci_gsc_intf *intf)
{
	int ret;

	if (intf->irq < 0)
		return;

	ret = generic_handle_irq_safe(intf->irq);
	if (ret)
		drm_err_ratelimited(&xe->drm, "error handling GSC irq: %d\n", ret);
}

void xe_heci_gsc_irq_handler(struct xe_device *xe, u32 iir)
{
	if ((iir & (GSC_IRQ_INTF(0) | GSC_IRQ_INTF(1))) == 0)
		return;

	if (!xe->info.has_heci_gscfi) {
		drm_warn_once(&xe->drm, "GSC irq: not supported");
		return;
	}

	if (iir & GSC_IRQ_INTF(0))
		heci_gsc_intf_irq(xe, &xe->heci_gsc.intf[XE_HECI1]);
	if (iir & GSC_IRQ_INTF(1))
		heci_gsc_intf_irq(xe, &xe->heci_gsc.intf[XE_HECI2]);
}

void xe_heci_csc_irq_handler(struct xe_device *xe, u32 iir)
{
	if ((iir & CSC_IRQ_INTF(1)) == 0)
		return;

	if (!xe->info.has_heci_cscfi) {
		drm_warn_once(&xe->drm, "CSC irq: not supported");
		return;
	}

	heci_gsc_intf_irq(xe, &xe->heci_gsc.intf[XE_HECI2]);
}
