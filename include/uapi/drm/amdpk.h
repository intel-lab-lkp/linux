// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 */

#ifndef __AMDPK_H__
#define __AMDPK_H__

#if defined(__cplusplus)
extern "C" {
#endif

#define MAX_PK_REQS		256

struct amdpk_info {
	/** maximum available queue depth */
	unsigned int avail_qdepth;
};

struct amdpk_conf {
	/** queue depth to configure */
	unsigned int qdepth;
	/** eventfd's associated with the descriptors */
	int eventfd[MAX_PK_REQS];
};

/* IOCTL */
#define DRM_AMDPK_GET_INFO	0x0
#define DRM_AMDPK_SET_CONF	0x1

#define DRM_IOCTL_AMDPK_GET_INFO	DRM_IOWR(DRM_COMMAND_BASE + DRM_AMDPK_GET_INFO, \
						 struct amdpk_info)
#define DRM_IOCTL_AMDPK_SET_CONF	DRM_IOWR(DRM_COMMAND_BASE + DRM_AMDPK_SET_CONF, \
						 struct amdpk_conf)

/* MMAP */
#define AMDPK_MMAP_REGS		0
#define AMDPK_MMAP_MEM		1

/* Completion Status */
#define CQ_STATUS_INVALID	0x0
#define CQ_STATUS_VALID		0x80000000
#define CQ_COMPLETION_ERROR	0x40000000

#if defined(__cplusplus)
}
#endif

#endif /* __AMDPK_H__ */
