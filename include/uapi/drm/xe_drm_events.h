/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef _UAPI_XE_DRM_EVENTS_H_
#define _UAPI_XE_DRM_EVENTS_H_

#include <linux/types.h>
#include <linux/watch_queue.h>

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * enum drm_xe_watch_event - Xe device watch event subtypes
 *
 * Subtypes for notifications delivered via %WATCH_TYPE_DRM_XE_NOTIFY when
 * reading from a pipe subscribed with %DRM_IOCTL_XE_WATCH_QUEUE.
 */
enum drm_xe_watch_event {
	/**
	 * @DRM_XE_WATCH_EVENT_VM_ERR: a VM has encountered an error.
	 *
	 * Indicates that a memory allocation failure occurred within the
	 * given VM.  The vm_id of the affected VM is carried in the
	 * @drm_xe_watch_notification_vm_err::vm_id field of the extended
	 * notification record.
	 */
	DRM_XE_WATCH_EVENT_VM_ERR = 0,
};

/**
 * struct drm_xe_watch_notification_vm_err - VM error event notification
 *
 * Notification record delivered for %DRM_XE_WATCH_EVENT_VM_ERR.
 * The record type is always %WATCH_TYPE_DRM_XE_NOTIFY and the subtype is
 * %DRM_XE_WATCH_EVENT_VM_ERR.
 */
struct drm_xe_watch_notification_vm_err {
	/** @base: common watch notification header */
	struct watch_notification base;

	/** @vm_id: ID of the VM that hit out-of-memory */
	__u32 vm_id;

	/** @error_code: error code describing the error condition (negative errno) */
	__s32 error_code;
};

#if defined(__cplusplus)
}
#endif

#endif /* _UAPI_XE_DRM_H_ */
