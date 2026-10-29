/* SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0+ */

/*
 * Definitions of virtio-media session related structures.
 *
 * Copyright (c) 2024-2026 Google LLC.
 */

#ifndef __VIRTIO_MEDIA_SESSION_H
#define __VIRTIO_MEDIA_SESSION_H

#include <linux/scatterlist.h>
#include <media/v4l2-fh.h>

#include "uapi/linux/virtio_media.h"

/*
 * Size of the per-session virtio shadow and event buffers. 16K should be
 * enough to contain everything we need.
 */
#define VIRTIO_SHADOW_BUF_SIZE 0x4000

/**
 * struct virtio_media_session - A session on a virtio_media device.
 * @fh: file handler for the session.
 * @file: file pointer associated with the session's file handler.
 * @id: session ID used to communicate with the device.
 * @cmd: union of session commands ``close`` and ``ioctl``. A session can have
 *       one command currently running. The rest of the commands are handled by
 *       &struct virtio_media.
 * @resp: union of responses to session commands. A session can wait on one
 *        command only. The rest of the responses are handled by
 *        &struct virtio_media.
 * @shadow_buf: shadow buffer where data to be added to the descriptor chain can
 *              be staged before being sent to the device.
 * @command_sgs: SG table gathering descriptors for a given command and its
 *               response.
 * @list: link into the list of sessions for the device.
 */
struct virtio_media_session {
	struct v4l2_fh fh;
	struct file *file;
	u32 id;

	__dma_from_device_group_begin();
	union {
		struct virtio_media_cmd_close close;
		struct virtio_media_cmd_ioctl ioctl;
	} cmd;

	union {
		struct virtio_media_resp_ioctl ioctl;
	} resp;
	__dma_from_device_group_end();

	void *shadow_buf;

	struct sg_table command_sgs;

	struct list_head list;
};

static inline struct virtio_media_session *fh_to_session(struct v4l2_fh *fh)
{
	return container_of(fh, struct virtio_media_session, fh);
}

static inline void
virtio_media_session_fh_add(struct virtio_media_session *session,
			    struct file *file)
{
	v4l2_fh_add(&session->fh, file);
	session->file = file;
}

static inline void
virtio_media_session_fh_del(struct virtio_media_session *session)
{
	v4l2_fh_del(&session->fh, session->file);
}

#endif // __VIRTIO_MEDIA_SESSION_H
