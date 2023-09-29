/* SPDX-License-Identifier: GPL-2.0
 *
 * Amazon Nitro Secure Module driver.
 *
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 */

#ifndef __LINUX_NSM_H
#define __LINUX_NSM_H

#include <linux/uio.h>
#include <linux/virtio.h>

#define NSM_RESPONSE_MAX_SIZE 0x3000

struct nsm_hwrng {
	int (*probe)(struct virtio_device *dev);
	void (*remove)(struct virtio_device *dev);
};

int nsm_register_hwrng(struct nsm_hwrng *nsm_hwrng);
void nsm_unregister_hwrng(struct nsm_hwrng *nsm_hwrng);

/* Copy of NSM message in kernel-space */
struct nsm_kernel_message {
	/* Copy of user request in kernel memory */
	struct kvec request;
	/* Copy of user response in kernel memory */
	struct kvec response;
};

int nsm_communicate_with_device(struct virtio_device *dev,
				struct nsm_kernel_message *message);

#endif /* __LINUX_NSM_H */
