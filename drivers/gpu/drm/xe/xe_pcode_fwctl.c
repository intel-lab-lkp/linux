// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include "xe_pcode_fwctl.h"

#include <linux/fwctl.h>
#include <uapi/fwctl/xe_pcode.h>

#include "xe_device.h"
#include "xe_pcode_api.h"
#include "xe_pcode.h"
#include "xe_pm.h"

/**
 * DOC: XE PCODE FWCTL
 *
 * Xe PCODE FWCTL implements the generic FWCTL IOCLTs to allow limited access
 * from user space (as admin) to some very specific PCODE Mailboxes.
 *
 * User space first needs to issue the ```FWCTL_INFO``` ioctl and check for the
 * capability flag, which will indicate which group of Mailboxes commands are
 * supported on that current running firmware.
 *
 * After verifying the availability of the desired Mailbox command,
 * ```FWCTL_RPC``` needs to be issued with in and out parameter both using
 * pointers to a ```struct fwctl_rpc_xe_pcode``` allocated by userspace.
 * In and out length needs to be sizeof(struct fwctl_rpc_xe_pcode).
 *
 * Any command that is not listed in the include/uapi/fwctl/xe_pcode.h or not
 * supported by the running firmware, will return ERR_PTR(-EBADMSG).
 *
 * Example:
 *
 * .. code-block:: C
 *
 *  struct fwctl_info_xe_pcode xe_pcode_info;
 *
 *  struct fwctl_info info = {
 *           .size = sizeof(struct fwctl_info),
 *           .flags = 0,
 *           .out_device_type = 0,
 *           .device_data_len = sizeof(struct fwctl_info_xe_pcode),
 *           .out_device_data = (__aligned_u64) &xe_pcode_info,
 *   };
 *
 *   fd = open("/dev/fwctl/fwctl0", O_RDWR);
 *   if (fd < 0) {
 *       perror("Failed to open /dev/fwctl/fwctl0");
 *       return -1;
 *   }
 *
 *   if (ioctl(fd, FWCTL_INFO, &info)) {
 *           perror("ioctl(FWCTL_INFO) failed");
 *           close(fd);
 *           return -1;
 *   }
 *
 *   if (xe_pcode_info.uctx_caps & FWCTL_XE_PCODE_LATEBINDING) {
 *           struct fwctl_rpc_xe_pcode rpc_in = {
 *                   .command = PCODE_CMD_LATE_BINDING,
 *                   .param1 = PARAM1_GET_CAPABILITY_STATUS,
 *           };
 *
 *           struct fwctl_rpc_xe_pcode rpc_out = {0};
 *
 *           struct fwctl_rpc rpc = {
 *                   .size = sizeof(struct fwctl_rpc),
 *                   .scope = FWCTL_RPC_CONFIGURATION,
 *                   .in_len = sizeof(struct fwctl_rpc_xe_pcode),
 *                   .out_len = sizeof(struct fwctl_rpc_xe_pcode),
 *                   .in = (__aligned_u64) &rpc_in,
 *                   .out = (__aligned_u64) &rpc_out,
 *           };
 *
 *           if (ioctl(fd, FWCTL_RPC, &rpc)) {
 *                   perror("ioctl(FWCTL_RPC) failed");
 *                   close(fd);
 *                   return -1;
 *           }
 *
 */

struct xe_pcode_fwctl_dev {
	struct fwctl_device fwctl;
	struct xe_device *xe;
};

DEFINE_FREE(xe_pcode_fwctl, struct xe_pcode_fwctl_dev *, if (_T) fwctl_put(&_T->fwctl))

static int xe_pcode_fwctl_uctx_open(struct fwctl_uctx *uctx)
{
	struct xe_pcode_fwctl_dev *fwctl_dev =
		container_of(uctx->fwctl, struct xe_pcode_fwctl_dev, fwctl);
	struct xe_device *xe = fwctl_dev->xe;

	xe_pm_runtime_get(xe);

	return 0;
}

static void xe_pcode_fwctl_uctx_close(struct fwctl_uctx *uctx)
{
	struct xe_pcode_fwctl_dev *fwctl_dev =
		container_of(uctx->fwctl, struct xe_pcode_fwctl_dev, fwctl);
	struct xe_device *xe = fwctl_dev->xe;

	xe_pm_runtime_put(xe);
}

static void *xe_pcode_fwctl_info(struct fwctl_uctx *uctx, size_t *length)
{
	struct xe_pcode_fwctl_dev *fwctl_dev =
		container_of(uctx->fwctl, struct xe_pcode_fwctl_dev, fwctl);
	struct xe_device *xe = fwctl_dev->xe;
	struct fwctl_info_xe_pcode *info;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return ERR_PTR(-ENOMEM);

	if (xe->info.platform == XE_BATTLEMAGE)
		info->uctx_caps = FWCTL_XE_PCODE_LATEBINDING;

	*length = sizeof(*info);

	return info;
}

static bool xe_pcode_fwctl_rpc_validate(struct fwctl_rpc_xe_pcode *rpc,
					enum fwctl_rpc_scope scope)
{
	u32 mbox = PCODE_MBOX(rpc->command, rpc->param1, rpc->param2);

	if (mbox == PCODE_MBOX(PCODE_CMD_LATE_BINDING,
			       PARAM1_GET_CAPABILITY_STATUS, 0))
		return scope == FWCTL_RPC_CONFIGURATION;

	if (mbox == PCODE_MBOX(PCODE_CMD_LATE_BINDING,
			       PARAM1_GET_VERSION_LOW, 0))
		return (rpc->data0 == DATA0_TYPE_FAN_CONTROLLER ||
			rpc->data0 == DATA0_TYPE_VOLTAGE_REGULATOR) &&
			scope == FWCTL_RPC_CONFIGURATION;

	return false;
}

static void *xe_pcode_fwctl_rpc(struct fwctl_uctx *uctx,
				enum fwctl_rpc_scope scope,
				void *in, size_t in_len, size_t *out_len)
{
	struct xe_pcode_fwctl_dev *fwctl_dev =
		container_of(uctx->fwctl, struct xe_pcode_fwctl_dev, fwctl);
	struct xe_tile *root_tile = xe_device_get_root_tile(fwctl_dev->xe);
	struct fwctl_rpc_xe_pcode *rpc = in;
	int err;

	if (in_len != sizeof(struct fwctl_rpc_xe_pcode) ||
	    *out_len != sizeof(struct fwctl_rpc_xe_pcode))
		return ERR_PTR(-EMSGSIZE);

	if (!xe_pcode_fwctl_rpc_validate(rpc, scope))
		return ERR_PTR(-EBADMSG);

	err = xe_pcode_read(root_tile, PCODE_MBOX(rpc->command,
						  rpc->param1,
						  rpc->param2),
			    &rpc->data0,
			    &rpc->data1);
	if (err)
		return ERR_PTR(err);

	return rpc;
}

static const struct fwctl_ops xe_pcode_fwctl_ops = {
	.device_type = FWCTL_DEVICE_TYPE_XE_PCODE,
	.uctx_size = sizeof(struct fwctl_uctx),
	.open_uctx = xe_pcode_fwctl_uctx_open,
	.close_uctx = xe_pcode_fwctl_uctx_close,
	.info = xe_pcode_fwctl_info,
	.fw_rpc = xe_pcode_fwctl_rpc,
};

static void xe_pcode_fwctl_fini(void *dev)
{
	struct fwctl_device *fwctl = dev;

	fwctl_unregister(fwctl);
	fwctl_put(fwctl);
}

int xe_pcode_fwctl_init(struct xe_device *xe)
{
	struct xe_pcode_fwctl_dev *fwctl_dev __free(xe_pcode_fwctl) =
		fwctl_alloc_device(xe->drm.dev, &xe_pcode_fwctl_ops,
				   struct xe_pcode_fwctl_dev, fwctl);
	int err;

	/* For now xe_pcode_fwctl supports only Late-Binding commands on BMG */
	if (xe->info.platform != XE_BATTLEMAGE)
		return -ENODEV;

	if (!fwctl_dev)
		return -ENOMEM;

	fwctl_dev->xe = xe;

	err = fwctl_register(&fwctl_dev->fwctl);
	if (err)
		return err;

	return devm_add_action_or_reset(xe->drm.dev, xe_pcode_fwctl_fini,
					&fwctl_dev->fwctl);
}

MODULE_IMPORT_NS("FWCTL");
