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
#include <media/v4l2-mem2mem.h>

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

	ret = mali_vpu_session_init(sess);
	if (ret)
		goto err_v4l2_fh_deinit;

	sess->fh.m2m_ctx = sess->m2m_ctx;

	return 0;

err_v4l2_fh_deinit:
	v4l2_fh_del(&sess->fh, filp);
	v4l2_fh_exit(&sess->fh);
err_free_sess:
	kfree(sess);
	return ret;
}

static int mali_vpu_release(struct file *filp)
{
	struct mali_vpu_session *sess = container_of(file_to_v4l2_fh(filp),
						     struct mali_vpu_session, fh);

	mali_vpu_session_cleanup(sess);
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
	.poll		= v4l2_m2m_fop_poll,
	.mmap		= v4l2_m2m_fop_mmap,
};

static int mali_vpu_m2m_job_ready(void *priv)
{
	/*
	 * The Mali scheduler/LSID path owns hardware dispatch.
	 * v4l2-mem2mem is used only for vb2 queue bookkeeping.
	 */
	return 0;
}

static void mali_vpu_m2m_device_run(void *priv)
{
	WARN_ON_ONCE(1);
}

const struct v4l2_m2m_ops mali_vpu_m2m_ops = {
	.job_ready = mali_vpu_m2m_job_ready,
	.device_run = mali_vpu_m2m_device_run,
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
	.vidioc_querycap	= mali_vpu_querycap,
	.vidioc_reqbufs		= v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf	= v4l2_m2m_ioctl_querybuf,
	.vidioc_create_bufs	= v4l2_m2m_ioctl_create_bufs,
	.vidioc_prepare_buf	= v4l2_m2m_ioctl_prepare_buf,
	.vidioc_expbuf		= v4l2_m2m_ioctl_expbuf,
	.vidioc_qbuf		= v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf		= v4l2_m2m_ioctl_dqbuf,
	.vidioc_remove_bufs	= v4l2_m2m_ioctl_remove_bufs,
};
