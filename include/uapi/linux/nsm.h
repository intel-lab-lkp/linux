/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 */

#ifndef __UAPI_LINUX_NSM_H
#define __UAPI_LINUX_NSM_H

#include <linux/uio.h>
#include <linux/ioctl.h>
#include <linux/types.h>

/* NSM message from user-space */
struct nsm_message {
	/* Request from user */
	struct iovec request;
	/* Response to user */
	struct iovec response;
};

#define NSM_MAGIC		0x0A
#define NSM_IOCTL_REQUEST	_IOWR(NSM_MAGIC, 0, struct nsm_message)

#endif /* __UAPI_LINUX_MISC_BCM_VK_H */
