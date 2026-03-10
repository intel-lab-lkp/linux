// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <linux/slab.h>
#include <linux/watch_queue.h>

#include <uapi/drm/xe_drm.h>
#include <uapi/drm/xe_drm_events.h>

#include "xe_device.h"
#include "xe_device_types.h"
#include "xe_macros.h"
#include "xe_watch_queue.h"

/**
 * struct xe_watch_notification_vm_err - kernel-side VM error event notification
 *
 * Layout mirrors &struct drm_xe_watch_notification_vm_err.
 *
 * @base: common watch notification header; type is %WATCH_TYPE_DRM_XE_NOTIFY,
 *        subtype is %DRM_XE_WATCH_EVENT_VM_ERR
 * @vm_id: ID of the VM that hit error
 * @error_code: error code describing the error condition (negative errno)
 */
struct xe_watch_notification_vm_err {
	struct watch_notification base;
	u32 vm_id;
	s32 error_code;
};

/**
 * xe_watch_queue_ioctl() - Subscribe a pipe to per-file device event notifications
 * @dev: DRM device
 * @data: pointer to &struct drm_xe_watch_queue from userspace
 * @file: DRM file handle of the subscribing process
 *
 * Subscribes a notification pipe to receive Xe device events for the calling
 * process's file handle.  Only events scoped to this file (e.g. VM error on a
 * VM owned by this file) are delivered.  The pipe must have been opened with
 * O_NOTIFICATION_PIPE and sized with %IOC_WATCH_QUEUE_SET_SIZE before calling
 * this IOCTL.
 *
 * Return: 0 on success, negative errno on failure.
 */
int xe_watch_queue_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct xe_file *xef = file->driver_priv;
	struct xe_device *xe = to_xe_device(dev);
	struct drm_xe_watch_queue *args = data;
	struct watch_queue *wqueue;
	struct watch *watch;
	int ret;

	if (XE_IOCTL_DBG(xe, args->flags || args->pad))
		return -EINVAL;
	if (XE_IOCTL_DBG(xe, args->watch_id > 0xff))
		return -EINVAL;

	wqueue = get_watch_queue(args->fd);
	if (XE_IOCTL_DBG(xe, IS_ERR(wqueue)))
		return PTR_ERR(wqueue);

	watch = kzalloc(sizeof(*watch), GFP_KERNEL | __GFP_ACCOUNT);
	if (XE_IOCTL_DBG(xe, !watch)) {
		ret = -ENOMEM;
		goto out_put_queue;
	}

	init_watch(watch, wqueue);
	watch->id = 0;
	watch->info_id = (u32)args->watch_id << WATCH_INFO_ID__SHIFT;

	ret = add_watch_to_object(watch, &xef->watch_list);
	if (XE_IOCTL_DBG(xe, ret))
		kfree(watch);

out_put_queue:
	put_watch_queue(wqueue);
	return ret;
}

/**
 * xe_watch_queue_post_vm_err_event() - Post a VM error event
 * @xef: xe file handle that owns the VM
 * @vm_id: userspace ID of the VM that hit error
 * @error_code: error code describing the error condition (negative errno)
 *
 * Posts a %DRM_XE_WATCH_EVENT_VM_ERR notification carrying @vm_id and
 * @error_code to every pipe that @xef has subscribed via
 * %DRM_IOCTL_XE_WATCH_QUEUE.  Only the owning process is notified,
 * preventing information leaks to other clients.
 */
void xe_watch_queue_post_vm_err_event(struct xe_file *xef, u32 vm_id,
				      int error_code)
{
	struct xe_watch_notification_vm_err n = {};

	n.base.type    = WATCH_TYPE_DRM_XE_NOTIFY;
	n.base.subtype = DRM_XE_WATCH_EVENT_VM_ERR;
	n.base.info    = watch_sizeof(struct xe_watch_notification_vm_err);
	n.vm_id        = vm_id;
	n.error_code   = error_code;

	post_watch_notification(&xef->watch_list, &n.base, current_cred(), 0);
}
