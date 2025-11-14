/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (C) Pinecone Inc. 2019
 * Copyright (C) Xiang Xiao <xiaoxiang@pinecone.net>
 */

#ifndef _LINUX_VIRTIO_RPMSG_H
#define _LINUX_VIRTIO_RPMSG_H

#include <linux/types.h>

/* The feature bitmap for virtio rpmsg */
#define VIRTIO_RPMSG_F_NS	0 /* RP supports name service notifications */
#define VIRTIO_RPMSG_F_BUFSZ	2 /* RP get buffer size from config space */

struct virtio_rpmsg_config {
	/* The tx/rx individual buffer size(if VIRTIO_RPMSG_F_BUFSZ) */
	__u32 txbuf_size;
	__u32 rxbuf_size;
	__u32 reserved[14]; /* Reserve for the future use */
	/* Put the customize config here */
} __attribute__((packed));

#endif /* _LINUX_VIRTIO_RPMSG_H */
