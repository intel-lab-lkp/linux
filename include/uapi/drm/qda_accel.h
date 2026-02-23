/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __QDA_ACCEL_H__
#define __QDA_ACCEL_H__

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * QDA IOCTL command numbers
 *
 * These define the command numbers for QDA-specific IOCTLs.
 * They are used with DRM_COMMAND_BASE to create the full IOCTL numbers.
 */
#define DRM_QDA_QUERY	0x00
#define DRM_QDA_GEM_CREATE		0x01
#define DRM_QDA_GEM_MMAP_OFFSET	0x02
/*
 * QDA IOCTL definitions
 *
 * These macros define the actual IOCTL numbers used by userspace applications.
 * They combine the command numbers with DRM_COMMAND_BASE and specify the
 * data structure and direction (read/write) for each IOCTL.
 */
#define DRM_IOCTL_QDA_QUERY	DRM_IOR(DRM_COMMAND_BASE + DRM_QDA_QUERY, struct drm_qda_query)
#define DRM_IOCTL_QDA_GEM_CREATE	DRM_IOWR(DRM_COMMAND_BASE + DRM_QDA_GEM_CREATE, \
						 struct drm_qda_gem_create)
#define DRM_IOCTL_QDA_GEM_MMAP_OFFSET	DRM_IOWR(DRM_COMMAND_BASE + DRM_QDA_GEM_MMAP_OFFSET, \
						 struct drm_qda_gem_mmap_offset)

/**
 * struct drm_qda_query - Device information query structure
 * @dsp_name: Name of DSP (e.g., "adsp", "cdsp", "cdsp1", "gdsp0", "gdsp1")
 *
 * This structure is used with DRM_IOCTL_QDA_QUERY to query device type,
 * allowing userspace to identify which DSP a device node represents. The
 * kernel provides the DSP name directly as a null-terminated string.
 */
struct drm_qda_query {
	__u8 dsp_name[16];
};

/**
 * struct drm_qda_gem_create - GEM buffer object creation parameters
 * @size: Size of the GEM object to create in bytes (input)
 * @handle: Allocated GEM handle (output)
 *
 * This structure is used with DRM_IOCTL_QDA_GEM_CREATE to allocate
 * a new GEM buffer object.
 */
struct drm_qda_gem_create {
	__u32 handle;
	__u32 pad;
	__u64 size;
};

/**
 * struct drm_qda_gem_mmap_offset - GEM object mmap offset query
 * @handle: GEM handle (input)
 * @pad: Padding for 64-bit alignment
 * @offset: mmap offset for the GEM object (output)
 *
 * This structure is used with DRM_IOCTL_QDA_GEM_MMAP_OFFSET to retrieve
 * the mmap offset that can be used with mmap() to map the GEM object into
 * user space.
 */
struct drm_qda_gem_mmap_offset {
	__u32 handle;
	__u32 pad;
	__u64 offset;
};

#if defined(__cplusplus)
}
#endif

#endif /* __QDA_ACCEL_H__ */
