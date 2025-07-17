/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Media device file handle
 *
 * Copyright (C) 2019--2023 Intel Corporation
 */

#ifndef MEDIA_FH_H
#define MEDIA_FH_H

#include <linux/list.h>
#include <linux/file.h>

#include <media/media-devnode.h>

struct media_device_context;

/**
 * struct media_device_fh - File handle specific information on MC
 *
 * @fh: The media device file handle
 * @mdev_list: This file handle in media device's list of file handles
 * @context: The media device context associated with the file handle
 */
struct media_device_fh {
	struct media_devnode_fh fh;
	struct list_head mdev_list;

	struct media_device_context *context;
};

static inline struct media_device_fh *media_device_fh(struct file *filp)
{
	return container_of(filp->private_data, struct media_device_fh, fh);
}

#endif /* MEDIA_FH_H */
