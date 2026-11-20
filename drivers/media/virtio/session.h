/* SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0+ */

/*
 * Definitions of virtio-media session related structures.
 *
 * Copyright (c) 2024-2026 Google LLC.
 */

#ifndef __VIRTIO_MEDIA_SESSION_H
#define __VIRTIO_MEDIA_SESSION_H

#include <media/v4l2-fh.h>

#include "uapi/linux/virtio_media.h"

/**
 * struct virtio_media_session - A session on a virtio_media device.
 * @fh: file handler for the session.
 * @file: file pointer associated with the session's file handler.
 * @id: session ID used to communicate with the device.
 * @cmd: union of session-level commands. A session can have one command
 *       currently running.
 * @list: link into the list of sessions for the device.
 */
struct virtio_media_session {
	struct v4l2_fh fh;
	struct file *file;
	u32 id;

	__dma_from_device_group_begin();
	union {
		struct virtio_media_cmd_close close;
	} cmd;
	__dma_from_device_group_end();

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
