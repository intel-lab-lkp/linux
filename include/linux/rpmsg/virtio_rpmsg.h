/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) Pinecone Inc. 2019
 * Copyright (C) Xiang Xiao <xiaoxiang@pinecone.net>
 * Copyright (C) Advanced Micro Devices, Inc. 2026
 */

#ifndef _LINUX_VIRTIO_RPMSG_H
#define _LINUX_VIRTIO_RPMSG_H

#include <linux/types.h>
#include <linux/virtio_types.h>

/* The feature bitmap for virtio rpmsg */
#define VIRTIO_RPMSG_F_NS	0 /* RP supports name service notifications */
#define VIRTIO_RPMSG_F_BUFSZ	1 /* RP get buffer size from config space */

/* Version of struct virtio_rpmsg_config understood by this driver */
#define RPMSG_VDEV_CONFIG_V1	1

/**
 * struct virtio_rpmsg_config - config space for rpmsg virtio device
 *
 * @version:	version of this structure, currently %RPMSG_VDEV_CONFIG_V1.
 * @size:	size of this structure in bytes.
 * @rpmsg_buf_align: alignment in bytes for each buffer. Must be a power of
 *		     two. If 0 then no alignment will be done. This alignment
 *		     will not decide actual size of the buffer but will be
 *		     used to decided the start address of the buffer. The
 *		     actual size of the buffer can be different than the
 *		     aligned size of the buffer.
 * @txbuf_size:	Tx buf size from remote's view. For Linux this is rx buf size.
 * @rxbuf_size:	Rx buf size from remote's view. For Linux this is tx buf size.
 *
 * This is the configuration structure shared by the device and the driver,
 * read when %VIRTIO_RPMSG_F_BUFSZ is negotiated. The fields are laid out so
 * the structure is naturally 32-bit aligned.
 */
struct virtio_rpmsg_config {
	u8 version;
	__virtio16 size;
	__virtio16 rpmsg_buf_align;
	/* The tx/rx individual buffer size (if VIRTIO_RPMSG_F_BUFSZ) */
	__virtio32 txbuf_size;
	__virtio32 rxbuf_size;
} __packed;

#endif /* _LINUX_VIRTIO_RPMSG_H */
