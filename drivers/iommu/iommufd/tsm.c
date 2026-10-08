// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Ltd.
 */

#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/limits.h>
#include <linux/tsm.h>
#include "iommufd_private.h"

/**
 * iommufd_vdevice_tsm_req_ioctl - Forward TSM requests
 * @ucmd: user command data for IOMMU_VDEVICE_TSM_REQ
 *
 * Resolve @iommu_vdevice_tsm_req::vdevice_id to a vdevice and pass the
 * request/response buffers to its vIOMMU implementation.
 *
 * Return:
 *  -errno on error.
 *  positive residue if response/request bytes were left unconsumed.
 *    if response buffer is provided, residue indicates the number of bytes
 *    not used in response buffer
 *    if there is no response buffer, residue indicates the number of bytes
 *    not consumed in req buffer
 *  0 otherwise.
 */
int iommufd_vdevice_tsm_req_ioctl(struct iommufd_ucmd *ucmd)
{
	int ret;
	struct iommufd_object *obj;
	struct iommufd_vdevice *vdev;
	struct iommu_vdevice_tsm_req *cmd = ucmd->cmd;
	struct tsm_guest_req_info info = {
		.op = cmd->op,
		.tvm_arch = cmd->tvm_arch,
		.req = u64_to_user_ptr(cmd->req_uptr),
		.req_len = cmd->req_len,
		.resp = u64_to_user_ptr(cmd->resp_uptr),
		.resp_len = cmd->resp_len,
	};

	/* Every defined operation must fit in the vdevice's u64 mask. */
	BUILD_BUG_ON(BITS_PER_TYPE(u64) < TSM_REQ_MAX);

	/* Successful residues must fit in the ioctl's int return value. */
	if (cmd->req_len > INT_MAX || cmd->resp_len > INT_MAX)
		return -EINVAL;

	obj = iommufd_get_object(ucmd->ictx, cmd->vdevice_id,
				 IOMMUFD_OBJ_VDEVICE);
	if (IS_ERR(obj))
		return PTR_ERR(obj);
	vdev = container_of(obj, struct iommufd_vdevice, obj);

	cmd->out_tsm_code = 0;
	if (!vdev->viommu->ops || !vdev->viommu->ops->vdevice_tsm_req ||
	    !vdev->tsm_req_op_mask) {
		ret = -EOPNOTSUPP;
		goto out_respond;
	}
	/* Bounds-check the user-supplied shift before BIT_ULL(). */
	if (cmd->tvm_arch != vdev->tsm_tvm_arch ||
	    cmd->op >= TSM_REQ_MAX ||
	    !(vdev->tsm_req_op_mask & BIT_ULL(cmd->op))) {
		ret = -EINVAL;
		goto out_put_object;
	}
	ret = vdev->viommu->ops->vdevice_tsm_req(vdev, &info,
						 &cmd->out_tsm_code);

out_respond:
	/* Always copy the tsm_code as response */
	if (iommufd_ucmd_respond(ucmd, sizeof(*cmd)))
		ret = -EFAULT;

out_put_object:
	iommufd_put_object(ucmd->ictx, obj);
	return ret;
}
