// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0+

/*
 * Virtio-media driver.
 *
 * Copyright (c) 2024-2026 Google LLC.
 */

#include <linux/device.h>
#include <linux/dev_printk.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ids.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>

#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fh.h>

#include "uapi/linux/virtio_media.h"
#include "session.h"
#include "virtio_media.h"

/**
 * virtio_media_session_alloc() - Allocate a new session.
 * @vv: virtio-media device the session belongs to.
 * @file: file associated with the session.
 *
 * The caller must initialize the ``id`` field before calling
 * virtio_media_session_add().
 */
static struct virtio_media_session *
virtio_media_session_alloc(struct virtio_media *vv, struct file *file)
{
	struct virtio_media_session *session;

	session = kzalloc_obj(*session, GFP_KERNEL);
	if (!session)
		return ERR_PTR(-ENOMEM);

	// The caller is expected to set the ID once the host assigns a value.
	session->id = 0;

	INIT_LIST_HEAD(&session->list);
	v4l2_fh_init(&session->fh, &vv->video_dev);
	virtio_media_session_fh_add(session, file);

	return session;
}

/**
 * virtio_media_session_add() - Add a session to a device.
 * @vv: virtio-media device the session belongs to.
 * @session: session to add to the device.
 */
static void virtio_media_session_add(struct virtio_media *vv,
				     struct virtio_media_session *session)
{
	mutex_lock(&vv->sessions_lock);
	list_add_tail(&session->list, &vv->sessions);
	mutex_unlock(&vv->sessions_lock);
}

/**
 * virtio_media_session_free() - Free all resources of a session.
 * @session: session to destroy.
 *
 * All the resources of @session, as well as the backing memory of @session
 * itself, are freed.
 */
static void virtio_media_session_free(struct virtio_media_session *session)
{
	virtio_media_session_fh_del(session);
	v4l2_fh_exit(&session->fh);

	kfree(session);
}

/**
 * virtio_media_session_remove() - Remove a session from the device.
 * @vv: virtio-media device the session belongs to.
 * @session: session to remove.
 *
 * This only removes the session from the device. To free it, call
 * virtio_media_session_free().
 */
static void virtio_media_session_remove(struct virtio_media *vv,
					struct virtio_media_session *session)
{
	mutex_lock(&vv->sessions_lock);
	list_del(&session->list);
	mutex_unlock(&vv->sessions_lock);
}

/**
 * virtio_media_session_close() - Close a session.
 * @vv: virtio-media device the session belongs to.
 * @session: session to close.
 *
 * This sends the ``VIRTIO_MEDIA_CMD_CLOSE`` command to the device. If @session
 * is no longer used, it should be first removed by calling
 * virtio_media_session_remove(), then freed by calling
 * virtio_media_session_free().
 */
static int virtio_media_session_close(struct virtio_media *vv,
				      struct virtio_media_session *session)
{
	struct virtio_media_cmd_close *cmd_close = &session->cmd.close;
	struct scatterlist cmd_sg = {};
	struct scatterlist *sgs[1] = { &cmd_sg };
	int ret;

	mutex_lock(&vv->vlock);

	cmd_close->hdr.cmd = VIRTIO_MEDIA_CMD_CLOSE;
	cmd_close->hdr.__reserved = 0;
	cmd_close->session_id = session->id;
	cmd_close->__reserved = 0;

	sg_set_buf(&cmd_sg, cmd_close, sizeof(*cmd_close));
	sg_mark_end(&cmd_sg);

	ret = virtio_media_send_command(vv, sgs, 1, 0, 0, NULL);
	mutex_unlock(&vv->vlock);
	if (ret < 0)
		return ret;

	return 0;
}

/**
 * struct virtio_media_cmd_callback_param - Callback parameters to the virtio
 *                                          command queue.
 * @vv: virtio-media device in use.
 * @done: flag to be switched once the command is completed.
 * @resp_len: length of the received response from the command. Only valid
 *            after @done flag has switched to %true.
 */
struct virtio_media_cmd_callback_param {
	struct virtio_media *vv;
	bool done;
	size_t resp_len;
};

/**
 * commandq_callback() - Callback for the command queue.
 * @queue: command virtqueue.
 *
 * This just wakes up the thread that was waiting on the command to complete.
 */
static void commandq_callback(struct virtqueue *queue)
{
	unsigned int len;
	struct virtio_media_cmd_callback_param *param;

process_bufs:
	while ((param = virtqueue_get_buf(queue, &len))) {
		param->resp_len = len;
		param->done = true;
		wake_up(&param->vv->wq);
	}

	if (!virtqueue_enable_cb(queue)) {
		virtqueue_disable_cb(queue);
		goto process_bufs;
	}
}

/**
 * virtio_media_kick_command() - send a command to the commandq.
 * @vv: virtio-media device in use.
 * @sgs: descriptor chain to send.
 * @out_sgs: number of device-readable descriptors in @sgs.
 * @in_sgs: number of device-writable descriptors in @sgs.
 * @resp_len: output parameter. Upon success, contains the size of the response
 *            in bytes.
 */
static int virtio_media_kick_command(struct virtio_media *vv,
				     struct scatterlist **sgs,
				     const size_t out_sgs, const size_t in_sgs,
				     size_t *resp_len)
{
	struct virtio_media_cmd_callback_param cb_param = {
		.vv = vv,
		.done = false,
		.resp_len = 0,
	};
	struct virtio_media_resp_header *resp_header;
	int ret = virtqueue_add_sgs(vv->commandq, sgs, out_sgs, in_sgs,
		  &cb_param, GFP_ATOMIC);
	if (ret) {
		v4l2_err(&vv->v4l2_dev,
			 "failed to add sgs to command virtqueue\n");
		return ret;
	}

	if (!virtqueue_kick(vv->commandq)) {
		v4l2_err(&vv->v4l2_dev, "failed to kick command virtqueue\n");
		return -EINVAL;
	}

	/* Wait for the response. */
	wait_event(vv->wq, cb_param.done);

	if (resp_len)
		*resp_len = cb_param.resp_len;

	if (in_sgs > 0) {
		/*
		 * If we expect a response, make sure we have at least a
		 * response header - anything shorter is invalid.
		 */
		if (cb_param.resp_len < sizeof(*resp_header)) {
			v4l2_err(&vv->v4l2_dev,
				 "received response header is too short\n");
			return -EINVAL;
		}

		resp_header = sg_virt(sgs[out_sgs]);
		if (resp_header->status)
			/* Host returns a positive error code. */
			return -resp_header->status;
	}

	return 0;
}

/**
 * virtio_media_send_command() - Send a command to the device and wait for its
 * response.
 * @vv: virtio-media device in use.
 * @sgs: descriptor chain to send.
 * @out_sgs: number of device-readable descriptors in @sgs.
 * @in_sgs: number of device-writable descriptors in @sgs.
 * @minimum_resp_len: minimum length of the response expected by the caller
 *                    when the command is successful. Anything shorter than
 *                    that will result in %-EINVAL being returned.
 * @resp_len: output parameter. Upon success, contains the size of the response
 *            in bytes.
 */
int virtio_media_send_command(struct virtio_media *vv, struct scatterlist **sgs,
			      const size_t out_sgs, const size_t in_sgs,
			      size_t minimum_resp_len, size_t *resp_len)
{
	size_t local_resp_len = resp_len ? *resp_len : 0;
	int ret = virtio_media_kick_command(vv, sgs, out_sgs, in_sgs,
					    &local_resp_len);
	if (resp_len)
		*resp_len = local_resp_len;

	/*
	 * If the host could not process the command, there is no valid
	 * response.
	 */
	if (ret < 0)
		return ret;

	/* Make sure the host wrote a complete reply. */
	if (local_resp_len < minimum_resp_len) {
		v4l2_err(&vv->v4l2_dev,
			 "received response is too short: received %zu, expected at least %zu\n",
			 local_resp_len, minimum_resp_len);
		return -EINVAL;
	}

	return 0;
}

/**
 * virtio_media_device_open() - Create a new session from an opened file.
 * @file: opened file for the session.
 */
static int virtio_media_device_open(struct file *file)
{
	struct video_device *video_dev = video_devdata(file);
	struct virtio_media *vv = to_virtio_media(video_dev);
	struct virtio_media_cmd_open *cmd_open = &vv->cmd.open;
	struct virtio_media_resp_open *resp_open = &vv->resp.open;
	struct scatterlist cmd_sg = {}, resp_sg = {};
	struct scatterlist *sgs[2] = { &cmd_sg, &resp_sg };
	struct virtio_media_session *session;
	int ret;

	session = virtio_media_session_alloc(vv, file);
	if (IS_ERR(session))
		return PTR_ERR(session);

	mutex_lock(&vv->vlock);

	sg_set_buf(&cmd_sg, cmd_open, sizeof(*cmd_open));
	sg_mark_end(&cmd_sg);

	sg_set_buf(&resp_sg, resp_open, sizeof(*resp_open));
	sg_mark_end(&resp_sg);

	cmd_open->hdr.cmd = VIRTIO_MEDIA_CMD_OPEN;
	cmd_open->hdr.__reserved = 0;
	ret = virtio_media_send_command(vv, sgs, 1, 1, sizeof(*resp_open),
					NULL);
	session->id = resp_open->session_id;

	mutex_unlock(&vv->vlock);

	if (ret < 0) {
		virtio_media_session_free(session);
		return ret;
	}

	virtio_media_session_add(vv, session);
	file->private_data = &session->fh;

	return 0;
}

/**
 * virtio_media_device_close() - Close a previously opened session.
 * @file: file of the session to close.
 *
 * This sends the ``VIRTIO_MEDIA_CMD_CLOSE`` command to the device, closes the
 * session on the driver side, and frees the session.
 */
static int virtio_media_device_close(struct file *file)
{
	struct video_device *video_dev = video_devdata(file);
	struct virtio_media *vv = to_virtio_media(video_dev);
	struct virtio_media_session *session =
		fh_to_session(file->private_data);

	int ret = virtio_media_session_close(vv, session);

	virtio_media_session_remove(vv, session);
	virtio_media_session_free(session);

	return ret;
}

static const struct v4l2_file_operations virtio_media_fops = {
	.owner = THIS_MODULE,
	.open = virtio_media_device_open,
	.release = virtio_media_device_close,
};

static void virtio_media_release(struct v4l2_device *v4l2_dev)
{
	struct virtio_media *vv =
		container_of(v4l2_dev, struct virtio_media, v4l2_dev);

	v4l2_device_unregister(&vv->v4l2_dev);
	kfree(vv);
}

static int virtio_media_probe(struct virtio_device *virtio_dev)
{
	struct device *dev = &virtio_dev->dev;
	struct virtqueue *vqs[1];
	static struct virtqueue_info vq_info[1] = {
		{
			.name = "command",
			.callback = commandq_callback,
		},
	};
	struct virtio_media *vv;
	struct video_device *vd;
	int ret;

	vv = kzalloc_obj(struct virtio_media);
	if (!vv)
		return -ENOMEM;

	INIT_LIST_HEAD(&vv->sessions);
	mutex_init(&vv->sessions_lock);
	mutex_init(&vv->vlock);

	vv->virtio_dev = virtio_dev;
	virtio_dev->priv = vv;

	init_waitqueue_head(&vv->wq);

	ret = v4l2_device_register(dev, &vv->v4l2_dev);
	if (ret)
		goto err_free_virtio_media;

	vv->v4l2_dev.release = virtio_media_release;

	ret = virtio_find_vqs(virtio_dev, 1, vqs, vq_info, NULL);
	if (ret)
		goto err_v4l2_device_unregister;

	vv->commandq = vqs[0];

	vd = &vv->video_dev;
	vd->v4l2_dev = &vv->v4l2_dev;
	vd->fops = &virtio_media_fops;
	vd->release = video_device_release_empty;
	strscpy(vd->name, "virtio-media", sizeof(vd->name));

	video_set_drvdata(vd, vv);

	virtio_cread_le(virtio_dev, struct virtio_media_config, device_caps,
			&vd->device_caps);
	if (vd->device_caps & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE))
		vd->vfl_dir = VFL_DIR_M2M;
	else if (vd->device_caps &
		 (V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_VIDEO_OUTPUT_MPLANE))
		vd->vfl_dir = VFL_DIR_TX;
	else
		vd->vfl_dir = VFL_DIR_RX;

	__u32 device_type;

	virtio_cread_le(virtio_dev, struct virtio_media_config, device_type,
			&device_type);
	if (device_type >= VFL_TYPE_MAX) {
		dev_err(&virtio_dev->dev, "Invalid device_type: %u",
			device_type);
		ret = -EINVAL;
		goto err_del_vqs;
	}

	virtio_device_ready(virtio_dev);

	ret = video_register_device(vd, device_type, -1);
	if (ret)
		goto err_del_vqs;

	return 0;

err_del_vqs:
	virtio_reset_device(virtio_dev);
	virtio_dev->config->del_vqs(virtio_dev);
err_v4l2_device_unregister:
	v4l2_device_unregister(&vv->v4l2_dev);
err_free_virtio_media:
	kfree(vv);
	return ret;
}

static void virtio_media_remove(struct virtio_device *virtio_dev)
{
	struct virtio_media *vv = virtio_dev->priv;
	struct list_head *p, *n;

	/* Remove the video device to stop accessing the commandq virtqueue */
	video_unregister_device(&vv->video_dev);

	/*
	 * Acquire vlock to ensure no concurrent accesses to the commandq
	 * virtqueue
	 */
	mutex_lock(&vv->vlock);

	/* Stop all virtqueue callbacks */
	virtio_reset_device(virtio_dev);

	/*
	 * Only detach the unused buffers to allow internal virtqueue clean up.
	 * The actual buffers are freed in virtio_media_release()
	 */
	while (virtqueue_detach_unused_buf(vv->commandq))
		;

	/* Delete the virtqueues */
	virtio_dev->config->del_vqs(virtio_dev);

	mutex_unlock(&vv->vlock);

	list_for_each_safe(p, n, &vv->sessions) {
		struct virtio_media_session *s =
			list_entry(p, struct virtio_media_session, list);

		virtio_media_session_remove(vv, s);
		virtio_media_session_free(s);
	}

	v4l2_device_put(&vv->v4l2_dev);
}

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_MEDIA, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static unsigned int features[] = {};

static struct virtio_driver virtio_media_driver = {
	.feature_table = features,
	.feature_table_size = ARRAY_SIZE(features),
	.driver.name = VIRTIO_MEDIA_DEFAULT_DRIVER_NAME,
	.driver.owner = THIS_MODULE,
	.id_table = id_table,
	.probe = virtio_media_probe,
	.remove = virtio_media_remove,
};

module_virtio_driver(virtio_media_driver);

MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("virtio media driver");
MODULE_AUTHOR("Alexandre Courbot <gnurou@gmail.com>");
MODULE_LICENSE("Dual BSD/GPL");
