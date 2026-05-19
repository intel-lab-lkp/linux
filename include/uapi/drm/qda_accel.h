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
/* Command number 0x03 reserved for INIT_ATTACH; 0x06 reserved for MUNMAP */
#define DRM_QDA_REMOTE_SESSION_CREATE		0x04
#define DRM_QDA_REMOTE_MAP			0x05
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
#define DRM_IOCTL_QDA_REMOTE_MAP	DRM_IOWR(DRM_COMMAND_BASE + DRM_QDA_REMOTE_MAP, \
					  struct drm_qda_mem_map)
#define DRM_IOCTL_QDA_REMOTE_INVOKE	DRM_IOWR(DRM_COMMAND_BASE + DRM_QDA_REMOTE_INVOKE, \
					  struct drm_qda_invoke_args)

/* Request type definitions for qda_mem_map */
#define QDA_MAP_REQUEST_LEGACY    1  /* Legacy MMAP operation */
#define QDA_MAP_REQUEST_ATTR      2  /* Handle-based MEM_MAP operation with attributes */

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

/**
 * struct drm_qda_mem_map - Memory mapping request structure
 * @request: Request type (QDA_MAP_REQUEST_LEGACY or QDA_MAP_REQUEST_ATTR)
 * @flags: Mapping flags for DSP (cache attributes, permissions)
 * @fd: DMA-BUF file descriptor of the buffer to map
 * @attrs: Mapping attributes (used for ATTR request)
 * @offset: Offset within buffer (used for ATTR request)
 * @pad: Padding for 64-bit alignment (must be zero)
 * @vaddrin: Optional virtual address hint for mapping
 * @size: Size of the memory region to map in bytes
 * @vaddrout: Output DSP virtual address after successful mapping
 *
 * This structure is used to request mapping of a DMA buffer into the
 * DSP's virtual address space. The DSP will map the buffer according
 * to the specified flags and return the virtual address in vaddrout.
 *
 * For QDA_MAP_REQUEST_LEGACY (value 1):
 *   - Uses fields: fd, flags, vaddrin, size, vaddrout
 *   - Legacy MMAP operation for backward compatibility
 *
 * For QDA_MAP_REQUEST_ATTR (value 2):
 *   - Uses all fields including attrs and offset
 *   - FD-based MEM_MAP operation with custom SMMU attributes
 */
struct drm_qda_mem_map {
	__u32 request;
	__u32 flags;
	__s32 fd;
	__u32 attrs;
	__u32 offset;
	__u32 pad;
	__u64 vaddrin;
	__u64 size;
	__u64 vaddrout;
};

#if defined(__cplusplus)
}
#endif

#endif /* __QDA_ACCEL_H__ */
