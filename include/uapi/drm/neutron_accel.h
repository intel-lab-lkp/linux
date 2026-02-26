/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */
/* Copyright 2025-2026 NXP */

#ifndef __NEUTRON_ACCEL_H__
#define __NEUTRON_ACCEL_H__

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * enum drm_neutron_ioctl - Neutron IOCTL IDs
 *
 * @DRM_NEUTRON_CREATE_BO: Create a buffer object
 * @DRM_NEUTRON_SYNC_BO: Sync (parts of) the buffer object memory
 * @DRM_NEUTRON_SUBMIT_JOB: Submit a job to the device
 */
enum drm_neutron_ioctl {
	DRM_NEUTRON_CREATE_BO = 0,
	DRM_NEUTRON_SYNC_BO,
	DRM_NEUTRON_SUBMIT_JOB,
};

/**
 * struct drm_neutron_create_bo - Create a buffer object and return buffer
 *                                info to user
 *
 * @size: Size in bytes of requested buffer. May be updated by driver
 *        if allocated size different than requested
 * @handle: Returned handle for the new buffer object
 * @pad: MBZ
 * @map_offset: Returned offset for mmap() calls
 */
struct drm_neutron_create_bo {
	__u64 size;
	__u32 handle;
	__u32 pad;
	__u64 map_offset;
};

/**
 * enum drm_neutron_sync_dir - Direction of buffer object synchronization
 *
 * @DRM_NEUTRON_SYNC_TO_DEVICE: Sync from CPU to device
 * @DRM_NEUTRON_SYNC_FROM_DEVICE: Sync from device to CPU
 */
enum drm_neutron_sync_dir {
	DRM_NEUTRON_SYNC_TO_DEVICE = 0,
	DRM_NEUTRON_SYNC_FROM_DEVICE,
};

/**
 * struct drm_neutron_sync_bo - Sync buffer object memory
 *
 * @handle: Handle of buffer object to sync
 * @direction: Direction of sync, can be one of enum drm_neutron_sync_dir
 * @size: Size of the memory to sync, in bytes
 * @offset: Offset inside the buffer, in bytes
 */
struct drm_neutron_sync_bo {
	__u32 handle;
	__u32 direction;
	__u64 size;
	__u64 offset;
};

/**
 * enum drm_neutron_job_type - Type of job to submit to Neutron device
 *
 * @DRM_NEUTRON_JOB_INFERENCE: Inference job
 */
enum drm_neutron_job_type {
	DRM_NEUTRON_JOB_INFERENCE = 0,
};

/**
 * struct drm_neutron_inference_job - Inference job descriptor
 *
 * @tensor_offset: Offset of tensor array inside job BO
 * @microcode_offset: Microcode offset inside BO
 * @tensor_count: Number of valid tensors
 * @pad: MBZ
 */
struct drm_neutron_inference_job {
	__u32 tensor_offset;
	__u32 microcode_offset;
	__u32 tensor_count;
	__u32 pad[5];
};

/**
 * struct drm_neutron_submit_job - Submit a job to Neutron device
 *
 * @type: Job type, one of enum drm_neutron_job_type
 * @bo_handle: BO handle for this job
 * @inference: Inference job descriptor (when type is DRM_NEUTRON_JOB_INFERENCE)
 * @reserved: Reserved for future job types
 * @syncobj_handle: Handle of syncobj on which user waits for job completion
 * @pad: MBZ
 */
struct drm_neutron_submit_job {
	__u32 type;
	__u32 bo_handle;
	union {
		struct drm_neutron_inference_job inference;
		__u32 reserved[8];
	};
	__u32 syncobj_handle;
	__u32 pad;
};

#define DRM_IOCTL_NEUTRON_CREATE_BO \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEUTRON_CREATE_BO, \
		 struct drm_neutron_create_bo)

#define DRM_IOCTL_NEUTRON_SYNC_BO \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEUTRON_SYNC_BO, \
		 struct drm_neutron_sync_bo)

#define DRM_IOCTL_NEUTRON_SUBMIT_JOB \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEUTRON_SUBMIT_JOB, \
		 struct drm_neutron_submit_job)

#if defined(__cplusplus)
}
#endif

#endif /* __NEUTRON_ACCEL_H__ */
