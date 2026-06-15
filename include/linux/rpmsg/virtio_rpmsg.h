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
 * @reserved:	reserved for padding, must be zero.
 * @size:	size of this structure in bytes.
 * @rpmsg_buf_align:	required alignment in bytes for each buffer. Must be a
 *		power of two so that both the buffer sizes and the TX buffer
 *		base address can be aligned (e.g. to a cache line).
 * @reserved1:	reserved for padding, must be zero. Keeps the following 32-bit
 *		fields naturally aligned.
 * @txbuf_size:	Tx buf size from remote's view. For Linux this is rx buf size.
 * @rxbuf_size:	Rx buf size from remote's view. For Linux this is tx buf size.
 *
 * This is the configuration structure shared by the device and the driver,
 * read when %VIRTIO_RPMSG_F_BUFSZ is negotiated. The fields are laid out so
 * the structure is naturally 32-bit aligned.
 */
struct virtio_rpmsg_config {
	u8 version;
	u8 reserved;
	__virtio16 size;
	__virtio16 rpmsg_buf_align;
	__virtio16 reserved1;
	/* The tx/rx individual buffer size (if VIRTIO_RPMSG_F_BUFSZ) */
	__virtio32 txbuf_size;
	__virtio32 rxbuf_size;
} __packed;

#endif /* _LINUX_VIRTIO_RPMSG_H */
