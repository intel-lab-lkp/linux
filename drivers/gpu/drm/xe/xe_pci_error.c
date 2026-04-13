// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */
#include <linux/pci.h>

#include <drm/drm_drv.h>

#include "xe_device.h"
#include "xe_gt.h"
#include "xe_pci.h"
#include "xe_ras.h"
#include "xe_survivability_mode.h"
#include "xe_uc.h"

static void xe_pci_error_handling(struct pci_dev *pdev)
{
	struct xe_device *xe = pdev_to_xe_device(pdev);
	struct xe_gt *gt;
	u8 id;

	/* Return if device is wedged or in survivability mode */
	if (xe_survivability_mode_is_boot_enabled(xe) || xe_device_wedged(xe))
		return;

	/* Wedge the device to prevent userspace access but don't send the event yet */
	atomic_set(&xe->wedged.flag, 1);

	for_each_gt(gt, xe, id)
		xe_gt_declare_wedged(gt);

	pci_disable_device(pdev);
}

/* Mapping of RAS recovery action to PCI error result */
static const pci_ers_result_t ras_recovery_action_to_pci_result[] = {
	[XE_RAS_RECOVERY_ACTION_RECOVERED] = PCI_ERS_RESULT_RECOVERED,
	[XE_RAS_RECOVERY_ACTION_RESET] = PCI_ERS_RESULT_NEED_RESET,
	[XE_RAS_RECOVERY_ACTION_DISCONNECT] = PCI_ERS_RESULT_DISCONNECT,
};

static pci_ers_result_t xe_pci_error_detected(struct pci_dev *pdev, pci_channel_state_t state)
{
	struct xe_device *xe = pdev_to_xe_device(pdev);

	dev_err(&pdev->dev, "Xe Pci error recovery: error detected state %d\n", state);

	if (state == pci_channel_io_perm_failure)
		return PCI_ERS_RESULT_DISCONNECT;

	xe_device_set_in_recovery(xe);

	switch (state) {
	case pci_channel_io_normal:
		return PCI_ERS_RESULT_CAN_RECOVER;
	case pci_channel_io_frozen:
		xe_pci_error_handling(pdev);
		return PCI_ERS_RESULT_NEED_RESET;
	default:
		dev_err(&pdev->dev, "Unknown state %d\n", state);
		return PCI_ERS_RESULT_NEED_RESET;
	}
}

static pci_ers_result_t xe_pci_error_mmio_enabled(struct pci_dev *pdev)
{
	struct xe_device *xe = pdev_to_xe_device(pdev);
	enum xe_ras_recovery_action action;

	dev_err(&pdev->dev, "Xe Pci error recovery: MMIO enabled\n");
	action = xe_ras_process_errors(xe);

	return ras_recovery_action_to_pci_result[action];
}

static pci_ers_result_t xe_pci_error_slot_reset(struct pci_dev *pdev)
{
	const struct pci_device_id *ent = pci_match_id(pdev->driver->id_table, pdev);
	struct xe_device *xe = pdev_to_xe_device(pdev);

	dev_err(&pdev->dev, "Xe Pci error recovery: Slot reset\n");

	pci_restore_state(pdev);

	if (pci_enable_device(pdev)) {
		dev_err(&pdev->dev,
			"Cannot re-enable PCI device after reset\n");
		return PCI_ERS_RESULT_DISCONNECT;
	}

	/*
	 * Secondary Bus Reset wipes out all device memory
	 * requiring XE KMD to perform a device removal and reprobe.
	 */
	pdev->driver->remove(pdev);
	devres_release_group(&pdev->dev, xe->devres_group_id);

	if (!pdev->driver->probe(pdev, ent))
		return PCI_ERS_RESULT_RECOVERED;

	return PCI_ERS_RESULT_DISCONNECT;
}

static void xe_pci_error_resume(struct pci_dev *pdev)
{
	struct xe_device *xe = pdev_to_xe_device(pdev);

	dev_info(&pdev->dev, "Xe Pci error recovery: Recovered\n");

	xe_device_clear_in_recovery(xe);
}

const struct pci_error_handlers xe_pci_error_handlers = {
	.error_detected	= xe_pci_error_detected,
	.mmio_enabled	= xe_pci_error_mmio_enabled,
	.slot_reset	= xe_pci_error_slot_reset,
	.resume		= xe_pci_error_resume,
};
