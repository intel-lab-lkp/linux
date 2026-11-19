// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 ARM Limited.
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>

#include "mali-vpu-session.h"
#include "mali-vpu-vidioc.h"

static int mali_vpu_open(struct file *filp)
{
	struct video_device *vdev	= video_devdata(filp);
	struct mali_vpu_dev *vpu_dev	= video_drvdata(filp);
	struct mali_vpu_session *sess;
	int ret;

	sess = kzalloc_obj(*sess);
	if (!sess)
		return -ENOMEM;

	if (vdev == vpu_dev->vdev_dec) {
		sess->role = MALI_VPU_DECODER;
	} else {
		ret = -EINVAL;
		goto err_free_sess;
	}

	sess->vpu_dev = vpu_dev;

	v4l2_fh_init(&sess->fh, vdev);
	v4l2_fh_add(&sess->fh, filp);

	return 0;

err_free_sess:
	kfree(sess);
	return ret;
}

static int mali_vpu_release(struct file *filp)
{
	struct mali_vpu_session *sess = container_of(file_to_v4l2_fh(filp),
						     struct mali_vpu_session, fh);

	v4l2_fh_del(&sess->fh, filp);
	v4l2_fh_exit(&sess->fh);
	kfree(sess);
	return 0;
}

const struct v4l2_file_operations mali_vpu_fops = {
	.owner		= THIS_MODULE,
	.open		= mali_vpu_open,
	.release	= mali_vpu_release,
	.unlocked_ioctl = video_ioctl2,
};

static int mali_vpu_querycap(struct file *filp, void *fh,
			     struct v4l2_capability *cap)
{
	struct video_device *vdev = video_devdata(filp);

	strscpy(cap->driver, MALI_VPU_DRV_NAME, sizeof(cap->driver));
	strscpy(cap->card, vdev->name, sizeof(cap->card));

	return 0;
}

const struct v4l2_ioctl_ops mali_vpu_ioctl_ops_dec = {
	.vidioc_querycap = mali_vpu_querycap,
};
