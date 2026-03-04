/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef _XE_WATCH_QUEUE_H_
#define _XE_WATCH_QUEUE_H_

#include <linux/types.h>

struct drm_device;
struct drm_file;
struct xe_file;

int xe_watch_queue_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file);
void xe_watch_queue_post_vm_err_event(struct xe_file *xef, u32 vm_id,
				      int error_code);

#endif /* _XE_WATCH_QUEUE_H_ */
