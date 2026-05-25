// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Ltd.
 */

#include <linux/tsm.h>
#include "iommufd_private.h"

/**
 * iommufd_vdevice_tsm_op_ioctl - Handle vdevice TSM operations
 * @ucmd: user command data for IOMMU_VDEVICE_TSM_OP
 *
 * Currently only supports TSM bind/unbind operations
 * Resolve @iommu_vdevice_tsm_op::vdevice_id to a vdevice and dispatch the
 * requested bind/unbind operation through the TSM core.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int iommufd_vdevice_tsm_op_ioctl(struct iommufd_ucmd *ucmd)
{
	int rc;
	struct kvm *kvm = NULL;
	struct iommufd_vdevice *vdev;
	struct iommu_vdevice_tsm_op *cmd = ucmd->cmd;

	if (cmd->flags)
		return -EOPNOTSUPP;

	vdev = iommufd_get_vdevice(ucmd->ictx, cmd->vdevice_id);
	if (IS_ERR(vdev))
		return PTR_ERR(vdev);

	if (vdev->viommu->kvm_file)
		kvm = vdev->viommu->kvm_file->private_data;

	if (!kvm) {
		rc = -ENODEV;
		goto out_put_vdev;
	}

	/* tsm layer will take care of parallel calls to tsm_bind/unbind */
	switch (cmd->type) {
	case IOMMU_VDEVICE_TSM_BIND:
		rc = tsm_bind(vdev->idev->dev, kvm, vdev->virt_id);
		break;
	case IOMMU_VDEVICE_TSM_UNBIND:
		rc = tsm_unbind(vdev->idev->dev);
		break;
	default:
		rc = -EINVAL;
		goto out_put_vdev;
	}

	if (rc)
		goto out_put_vdev;

	rc = iommufd_ucmd_respond(ucmd, sizeof(*cmd));

out_put_vdev:
	iommufd_put_object(ucmd->ictx, &vdev->obj);
	return rc;
}
