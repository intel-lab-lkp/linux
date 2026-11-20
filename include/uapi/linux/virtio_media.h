/* SPDX-License-Identifier: ((GPL-2.0+ WITH Linux-syscall-note) OR BSD-3-Clause) */

/*
 * Definitions of virtio-media protocol structures.
 *
 * Copyright (c) 2024-2026 Google LLC.
 */

#ifndef _UAPI__LINUX_VIRTIO_MEDIA_H
#define _UAPI__LINUX_VIRTIO_MEDIA_H

#include <linux/types.h>
#include <linux/videodev2.h>

/*
 * Virtio protocol definition.
 */

/**
 * struct virtio_media_config - Video device configuration space.
 * @device_caps: flags representing the device capabilities as used in &struct
 *               v4l2_capability. It corresponds with the device_caps field in
 *               the &struct video_device
 * @ device_type: informs the driver of the type of the video device. It
 *                corresponds to with the ``vfl_devnod_type`` field of the
 *                device.
 * @card: name of the device, a NUL-terminated UTF-8 string. It corresponds
 *        with the card field of the struct v4l2_capability. If all the
 *        characters of the field are used, it does not need to be
 *        NUL-terminated.
 */
struct virtio_media_config {
	__le32 device_caps;
	__le32 device_type;
	__u8 card[32];
};

/**
 * struct virtio_media_cmd_header - Header for all virtio-media commands.
 * @cmd: one of VIRTIO_MEDIA_CMD_*.
 * @__reserved: must be set to zero by the driver.
 *
 * This header starts all commands from the driver to the device on the
 * commandq.
 */
struct virtio_media_cmd_header {
	__le32 cmd;
	__le32 __reserved;
};

/**
 * struct virtio_media_resp_header - Header for all virtio-media responses.
 * @status: 0 if the command was successful, or one of the standard Linux error
 *          codes as a positive integer.
 * @__reserved: must be set to zero by the device.
 *
 * This header starts all responses from the device to the driver on the
 * commandq.
 */
struct virtio_media_resp_header {
	__le32 status;
	__le32 __reserved;
};

/**
 * VIRTIO_MEDIA_CMD_OPEN - Command for creating a new session.
 *
 * This is the equivalent of calling ``open`` on a V4L2 device node. Upon
 * success, a session id is returned which can be used to perform other
 * commands on the session, notably ioctls.
 */
#define VIRTIO_MEDIA_CMD_OPEN 1

/**
 * struct virtio_media_cmd_open - Driver command for VIRTIO_MEDIA_CMD_OPEN.
 * @hdr: header with cmd member set to VIRTIO_MEDIA_CMD_OPEN.
 */
struct virtio_media_cmd_open {
	struct virtio_media_cmd_header hdr;
};

/**
 * struct virtio_media_resp_open - Device response for VIRTIO_MEDIA_CMD_OPEN.
 * @hdr: header containing the status of the command.
 * @session_id: if &struct virtio_media_resp_header.status == 0, contains the
 *              id of the newly created session.
 * @__reserved: must be set to zero by the device.
 */
struct virtio_media_resp_open {
	struct virtio_media_resp_header hdr;
	__le32 session_id;
	__le32 __reserved;
};

/**
 * VIRTIO_MEDIA_CMD_CLOSE - Command for closing an active session.
 *
 * This is the equivalent of calling ``close`` on a previously opened V4L2
 * session. All resources associated with this session will be freed and the
 * session ID shall not be used again after queueing this command.
 *
 * This command does not require a response from the device.
 */
#define VIRTIO_MEDIA_CMD_CLOSE 2

/**
 * struct virtio_media_cmd_close - Driver command for VIRTIO_MEDIA_CMD_CLOSE.
 * @hdr: header with cmd member set to VIRTIO_MEDIA_CMD_CLOSE.
 * @session_id: id of the session to close.
 * @__reserved: must be set to zero by the driver.
 */
struct virtio_media_cmd_close {
	struct virtio_media_cmd_header hdr;
	__le32 session_id;
	__le32 __reserved;
};

#endif // _UAPI__LINUX_VIRTIO_MEDIA_H
