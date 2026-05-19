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
#define DRM_QDA_QUERY		0x00
#define DRM_QDA_GEM_CREATE		0x01
#define DRM_QDA_GEM_MMAP_OFFSET	0x02
/* Command number 0x03 reserved for INIT_ATTACH; 0x05-0x06 reserved for MAP, MUNMAP */
#define DRM_QDA_REMOTE_SESSION_CREATE		0x04
#define DRM_QDA_REMOTE_INVOKE			0x07

/*
 * QDA IOCTL definitions
 *
 * These macros define the actual IOCTL numbers used by userspace applications.
 * They combine the command numbers with DRM_COMMAND_BASE and specify the
 * data structure and direction (read/write) for each IOCTL.
 */
#define DRM_IOCTL_QDA_QUERY		DRM_IOR(DRM_COMMAND_BASE + DRM_QDA_QUERY, \
					 struct drm_qda_query)
#define DRM_IOCTL_QDA_GEM_CREATE	DRM_IOWR(DRM_COMMAND_BASE + DRM_QDA_GEM_CREATE, \
					  struct drm_qda_gem_create)
#define DRM_IOCTL_QDA_GEM_MMAP_OFFSET	DRM_IOWR(DRM_COMMAND_BASE + DRM_QDA_GEM_MMAP_OFFSET, \
					  struct drm_qda_gem_mmap_offset)
#define DRM_IOCTL_QDA_REMOTE_SESSION_CREATE					\
	DRM_IOWR(DRM_COMMAND_BASE + DRM_QDA_REMOTE_SESSION_CREATE,		\
		 struct drm_qda_init_create)
#define DRM_IOCTL_QDA_REMOTE_INVOKE	DRM_IOWR(DRM_COMMAND_BASE + DRM_QDA_REMOTE_INVOKE, \
					  struct drm_qda_invoke_args)

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
	__u64 size;
	__u32 handle;
	__u32 pad;
};

/**
 * struct drm_qda_gem_mmap_offset - GEM object mmap offset query
 * @offset: mmap offset for the GEM object (output)
 * @handle: GEM handle (input)
 * @pad: Padding for 64-bit alignment
 *
 * This structure is used with DRM_IOCTL_QDA_GEM_MMAP_OFFSET to retrieve
 * the mmap offset that can be used with mmap() to map the GEM object into
 * user space.
 */
struct drm_qda_gem_mmap_offset {
	__u64 offset;
	__u32 handle;
	__u32 pad;
};

/**
 * struct drm_qda_fastrpc_invoke_args - FastRPC invocation argument descriptor
 * @ptr: Pointer to argument data (user virtual address)
 * @length: Length of the argument data in bytes
 * @fd: DMA-BUF file descriptor for buffer arguments, -1/0 for scalar arguments
 * @attr: Argument attributes and flags
 *
 * This structure describes a single argument passed to a FastRPC invocation.
 * Arguments can be either scalar values or buffer references (via DMA-BUF fd).
 */
struct drm_qda_fastrpc_invoke_args {
	__u64 ptr;
	__u64 length;
	__s32 fd;
	__u32 attr;
};

/**
 * struct drm_qda_init_create - Accelerator process initialization parameters
 * @filelen: Length of the ELF file in bytes
 * @filefd: DMA-BUF file descriptor containing the ELF file
 * @attrs: Process attributes flags
 * @siglen: Length of signature data in bytes
 * @file: Pointer to ELF file data if not using filefd
 *
 * This structure is used with DRM_IOCTL_QDA_INIT_CREATE to initialize
 * a new process on the accelerator. The process code is provided either
 * via a file descriptor (filefd, typically a GEM object) or a direct
 * pointer (file). Set file to 0 if using filefd.
 *
 * The attrs field contains bit flags for debug mode, privileged execution,
 * and other process attributes.
 */
struct drm_qda_init_create {
	__u32 filelen;
	__s32 filefd;
	__u32 attrs;
	__u32 siglen;
	__u64 file;
};

/**
 * struct drm_qda_invoke_args - Dynamic FastRPC invocation parameters
 * @handle: Remote handle to invoke on the DSP
 * @sc: FastRPC scalars value encoding the number of in/out buffers
 * @args: User-space pointer to array of drm_qda_fastrpc_invoke_args descriptors;
 *        the fd field in each entry must be a DMA-BUF fd (or -1/0 for
 *        inline scalar buffers)
 *
 * This structure is used with DRM_IOCTL_QDA_REMOTE_INVOKE to perform a
 * dynamic remote procedure call on the DSP. The args pointer must reference
 * an array of REMOTE_SCALARS_LENGTH(sc) drm_qda_fastrpc_invoke_args entries.
 */
struct drm_qda_invoke_args {
	__u32 handle;
	__u32 sc;
	__u64 args;
};

#if defined(__cplusplus)
}
#endif

#endif /* __QDA_ACCEL_H__ */
