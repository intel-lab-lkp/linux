// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include <linux/pci.h>
#include <drm/drm_drv.h>

#include "xe_device.h"
#include "xe_gt.h"
#include "xe_gt_printk.h"
#include "xe_pci.h"
#include "xe_pm.h"

/**
 * xe_pci_reset_prepare - Called when user issued a function level reset
 * via /sys/bus/pci/devices/.../reset.
 * @pdev: PCI device struct
 */
static void xe_pci_reset_prepare(struct pci_dev *pdev)
{
	struct xe_device *xe = pci_get_drvdata(pdev);
	struct xe_gt *gt;
	int id, err;

	pci_warn(pdev, "preparing for PCIe FLR reset\n");

	drm_warn(&xe->drm, "removing device access to userspace\n");
	drm_dev_unplug(&xe->drm);

	xe_pm_runtime_get(xe);
	/* idle the GTs */
	for_each_gt(gt, xe, id) {
		err = xe_force_wake_get(gt_to_fw(gt), XE_FORCEWAKE_ALL);
		if (err)
			goto reset;
		err = xe_idle_gt(gt);
		if (err) {
			xe_gt_err(gt, "failed to idle gt (%pe)\n", ERR_PTR(err));
			goto reset;
		}

		err = xe_force_wake_put(gt_to_fw(gt), XE_FORCEWAKE_ALL);
		XE_WARN_ON(err);
	}
	xe_pm_runtime_put(xe);

reset:
	pci_disable_device(pdev);
}

/**
 * xe_pci_reset_done - Called when function level reset is done.
 * @pdev: PCI device struct
 */
static void xe_pci_reset_done(struct pci_dev *pdev)
{
	const struct pci_device_id *ent = pci_match_id(pdev->driver->id_table, pdev);
	struct xe_device *xe = pci_get_drvdata(pdev);

	dev_info(&pdev->dev,
		 "PCI device went through FLR, reenabling the device\n");

	if (pci_enable_device(pdev)) {
		dev_err(&pdev->dev,
			"Cannot re-enable PCI device after reset\n");
		return;
	}
	pci_set_master(pdev);
	xe_load_pci_state(pdev);

	/*
	 * We want to completely clean the driver and even destroy
	 * the xe private data and reinitialize afresh similar to
	 * probe
	 */
	pdev->driver->remove(pdev);
	if (pci_dev_msi_enabled(pdev))
		pci_free_irq_vectors(pdev);

	devm_drm_release_action(&xe->drm);
	pci_disable_device(pdev);

	/*
	 * if this fails the driver might be in a stale state, only option is
	 * to unbind and rebind
	 */
	xe_pci_probe(pdev, ent);
}

const struct pci_error_handlers xe_pci_err_handlers = {
	.reset_prepare = xe_pci_reset_prepare,
	.reset_done = xe_pci_reset_done,
};
