// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include <drm/intel/xe_sriov_vfio.h>

#include "xe_pm.h"
#include "xe_sriov.h"
#include "xe_sriov_pf_control.h"
#include "xe_sriov_pf_migration.h"
#include "xe_sriov_pf_migration_data.h"

/**
 * xe_sriov_vfio_migration_supported() - Check if migration is supported.
 * @pdev: PF PCI device
 *
 * Return: true if migration is supported, false otherwise.
 */
bool xe_sriov_vfio_migration_supported(struct pci_dev *pdev)
{
	struct xe_device *xe = pci_get_drvdata(pdev);

	if (!IS_SRIOV_PF(xe))
		return -ENODEV;

	return xe_sriov_pf_migration_supported(xe);
}
EXPORT_SYMBOL_FOR_MODULES(xe_sriov_vfio_migration_supported, "xe-vfio-pci");

/**
 * xe_sriov_vfio_wait_flr_done - Wait for VF FLR completion.
 * @pdev: PF PCI device
 * @vfid: VF identifier
 *
 * This function will wait until VF FLR is processed by PF on all tiles (or
 * until timeout occurs).
 *
 * Return: 0 on success or a negative error code on failure.
 */
int xe_sriov_vfio_wait_flr_done(struct pci_dev *pdev, unsigned int vfid)
{
	struct xe_device *xe = pci_get_drvdata(pdev);

	if (!IS_SRIOV_PF(xe))
		return -ENODEV;

	return xe_sriov_pf_control_wait_flr(xe, vfid);
}
EXPORT_SYMBOL_FOR_MODULES(xe_sriov_vfio_wait_flr_done, "xe-vfio-pci");

/**
 * xe_sriov_vfio_stop - Stop VF.
 * @pdev: PF PCI device
 * @vfid: VF identifier
 *
 * This function will pause VF on all tiles/GTs.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int xe_sriov_vfio_stop(struct pci_dev *pdev, unsigned int vfid)
{
	struct xe_device *xe = pci_get_drvdata(pdev);
	int ret;

	if (!IS_SRIOV_PF(xe))
		return -ENODEV;

	xe_pm_runtime_get(xe);
	ret = xe_sriov_pf_control_pause_vf(xe, vfid);
	xe_pm_runtime_put(xe);

	return ret;
}
EXPORT_SYMBOL_FOR_MODULES(xe_sriov_vfio_stop, "xe-vfio-pci");

/**
 * xe_sriov_vfio_run - Run VF.
 * @pdev: PF PCI device
 * @vfid: VF identifier
 *
 * This function will resume VF on all tiles.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int xe_sriov_vfio_run(struct pci_dev *pdev, unsigned int vfid)
{
	struct xe_device *xe = pci_get_drvdata(pdev);
	int ret;

	if (!IS_SRIOV_PF(xe))
		return -ENODEV;

	xe_pm_runtime_get(xe);
	ret = xe_sriov_pf_control_resume_vf(xe, vfid);
	xe_pm_runtime_put(xe);

	return ret;
}
EXPORT_SYMBOL_FOR_MODULES(xe_sriov_vfio_run, "xe-vfio-pci");

/**
 * xe_sriov_vfio_stop_copy_enter - Copy VF migration data from device (while stopped).
 * @pdev: PF PCI device
 * @vfid: VF identifier
 *
 * This function will save VF migration data on all tiles.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int xe_sriov_vfio_stop_copy_enter(struct pci_dev *pdev, unsigned int vfid)
{
	struct xe_device *xe = pci_get_drvdata(pdev);
	int ret;

	if (!IS_SRIOV_PF(xe))
		return -ENODEV;

	xe_pm_runtime_get(xe);
	ret = xe_sriov_pf_control_save_vf(xe, vfid);
	xe_pm_runtime_put(xe);

	return ret;
}
EXPORT_SYMBOL_FOR_MODULES(xe_sriov_vfio_stop_copy_enter, "xe-vfio-pci");

/**
 * xe_sriov_vfio_stop_copy_exit - Wait until VF migration data save is done.
 * @pdev: PF PCI device
 * @vfid: VF identifier
 *
 * This function will wait until VF migration data is saved on all tiles.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int xe_sriov_vfio_stop_copy_exit(struct pci_dev *pdev, unsigned int vfid)
{
	struct xe_device *xe = pci_get_drvdata(pdev);
	int ret;

	if (!IS_SRIOV_PF(xe))
		return -ENODEV;

	xe_pm_runtime_get(xe);
	ret = xe_sriov_pf_control_wait_save_vf(xe, vfid);
	xe_pm_runtime_put(xe);

	return ret;
}
EXPORT_SYMBOL_FOR_MODULES(xe_sriov_vfio_stop_copy_exit, "xe-vfio-pci");

/**
 * xe_sriov_vfio_resume_enter - Copy VF migration data to device (while stopped).
 * @pdev: PF PCI device
 * @vfid: VF identifier
 *
 * This function will restore VF migration data on all tiles.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int xe_sriov_vfio_resume_enter(struct pci_dev *pdev, unsigned int vfid)
{
	struct xe_device *xe = pci_get_drvdata(pdev);
	int ret;

	if (!IS_SRIOV_PF(xe))
		return -ENODEV;

	xe_pm_runtime_get(xe);
	ret = xe_sriov_pf_control_restore_vf(xe, vfid);
	xe_pm_runtime_put(xe);

	return ret;
}
EXPORT_SYMBOL_FOR_MODULES(xe_sriov_vfio_resume_enter, "xe-vfio-pci");

/**
 * xe_sriov_vfio_resume_exit - Wait until VF migration data is copied to the device.
 * @pdev: PF PCI device
 * @vfid: VF identifier
 *
 * This function will wait until VF migration data is restored on all tiles.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int xe_sriov_vfio_resume_exit(struct pci_dev *pdev, unsigned int vfid)
{
	struct xe_device *xe = pci_get_drvdata(pdev);
	int ret;

	if (!IS_SRIOV_PF(xe))
		return -ENODEV;

	xe_pm_runtime_get(xe);
	ret = xe_sriov_pf_control_wait_restore_vf(xe, vfid);
	xe_pm_runtime_put(xe);

	return ret;
}
EXPORT_SYMBOL_FOR_MODULES(xe_sriov_vfio_resume_exit, "xe-vfio-pci");

/**
 * xe_sriov_vfio_error - Move VF to error state.
 * @pdev: PF PCI device
 * @vfid: VF identifier
 *
 * This function will stop VF on all tiles.
 * Reset is needed to move it out of error state.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int xe_sriov_vfio_error(struct pci_dev *pdev, unsigned int vfid)
{
	struct xe_device *xe = pci_get_drvdata(pdev);
	int ret;

	if (!IS_SRIOV_PF(xe))
		return -ENODEV;

	xe_pm_runtime_get(xe);
	ret = xe_sriov_pf_control_stop_vf(xe, vfid);
	xe_pm_runtime_put(xe);

	return ret;
}
EXPORT_SYMBOL_FOR_MODULES(xe_sriov_vfio_error, "xe-vfio-pci");

ssize_t xe_sriov_vfio_data_read(struct pci_dev *pdev, unsigned int vfid,
				char __user *buf, size_t len)
{
	struct xe_device *xe = pci_get_drvdata(pdev);

	return xe_sriov_pf_migration_data_read(xe, vfid, buf, len);
}
EXPORT_SYMBOL_FOR_MODULES(xe_sriov_vfio_data_read, "xe-vfio-pci");

ssize_t xe_sriov_vfio_data_write(struct pci_dev *pdev, unsigned int vfid,
				 const char __user *buf, size_t len)
{
	struct xe_device *xe = pci_get_drvdata(pdev);

	return xe_sriov_pf_migration_data_write(xe, vfid, buf, len);
}
EXPORT_SYMBOL_FOR_MODULES(xe_sriov_vfio_data_write, "xe-vfio-pci");

ssize_t xe_sriov_vfio_stop_copy_size(struct pci_dev *pdev, unsigned int vfid)
{
	struct xe_device *xe = pci_get_drvdata(pdev);

	return xe_sriov_pf_migration_size(xe, vfid);
}
EXPORT_SYMBOL_FOR_MODULES(xe_sriov_vfio_stop_copy_size, "xe-vfio-pci");
