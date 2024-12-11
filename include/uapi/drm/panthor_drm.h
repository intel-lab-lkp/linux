/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2023 Collabora ltd. */
#ifndef _PANTHOR_DRM_H_
#define _PANTHOR_DRM_H_

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * DOC: Introduction
 *
 * This documentation describes the Panthor IOCTLs.
 *
 * Just a few generic rules about the data passed to the Panthor IOCTLs:
 *
 * - Structures must be aligned on 64-bit/8-byte. If the object is not
 *   naturally aligned, a padding field must be added.
 * - Fields must be explicitly aligned to their natural type alignment with
 *   pad[0..N] fields.
 * - All padding fields will be checked by the driver to make sure they are
 *   zeroed.
 * - Flags can be added, but not removed/replaced.
 * - New fields can be added to the main structures (the structures
 *   directly passed to the ioctl). Those fields can be added at the end of
 *   the structure, or replace existing padding fields. Any new field being
 *   added must preserve the behavior that existed before those fields were
 *   added when a value of zero is passed.
 * - New fields can be added to indirect objects (objects pointed by the
 *   main structure), iff those objects are passed a size to reflect the
 *   size known by the userspace driver (see drm_panthor_obj_array::stride
 *   or drm_panthor_dev_query::size).
 * - If the kernel driver is too old to know some fields, those will be
 *   ignored if zero, and otherwise rejected (and so will be zero on output).
 * - If userspace is too old to know some fields, those will be zeroed
 *   (input) before the structure is parsed by the kernel driver.
 * - Each new flag/field addition must come with a driver version update so
 *   the userspace driver doesn't have to trial and error to know which
 *   flags are supported.
 * - Structures should not contain unions, as this would defeat the
 *   extensibility of such structures.
 * - IOCTLs can't be removed or replaced. New IOCTL IDs should be placed
 *   at the end of the drm_panthor_ioctl_id enum.
 */

/**
 * DOC: MMIO regions exposed to userspace.
 *
 * .. c:macro:: DRM_PANTHOR_USER_MMIO_OFFSET
 *
 * File offset for all MMIO regions being exposed to userspace. Don't use
 * this value directly, use DRM_PANTHOR_USER_<name>_OFFSET values instead.
 * pgoffset passed to mmap2() is an unsigned long, which forces us to use a
 * different offset on 32-bit and 64-bit systems.
 *
 * .. c:macro:: DRM_PANTHOR_USER_FLUSH_ID_MMIO_OFFSET
 *
 * File offset for the LATEST_FLUSH_ID register. The Userspace driver controls
 * GPU cache flushing through CS instructions, but the flush reduction
 * mechanism requires a flush_id. This flush_id could be queried with an
 * ioctl, but Arm provides a well-isolated register page containing only this
 * read-only register, so let's expose this page through a static mmap offset
 * and allow direct mapping of this MMIO region so we can avoid the
 * user <-> kernel round-trip.
 */
#define DRM_PANTHOR_USER_MMIO_OFFSET_32BIT	(1ull << 43)
#define DRM_PANTHOR_USER_MMIO_OFFSET_64BIT	(1ull << 56)
#define DRM_PANTHOR_USER_MMIO_OFFSET		(sizeof(unsigned long) < 8 ? \
						 DRM_PANTHOR_USER_MMIO_OFFSET_32BIT : \
						 DRM_PANTHOR_USER_MMIO_OFFSET_64BIT)
#define DRM_PANTHOR_USER_FLUSH_ID_MMIO_OFFSET	(DRM_PANTHOR_USER_MMIO_OFFSET | 0)

/**
 * DOC: IOCTL IDs
 *
 * enum drm_panthor_ioctl_id - IOCTL IDs
 *
 * Place new ioctls at the end, don't re-order, don't replace or remove entries.
 *
 * These IDs are not meant to be used directly. Use the DRM_IOCTL_PANTHOR_xxx
 * definitions instead.
 */
enum drm_panthor_ioctl_id {
	/** @DRM_PANTHOR_DEV_QUERY: Query device information. */
	DRM_PANTHOR_DEV_QUERY = 0,

	/** @DRM_PANTHOR_VM_CREATE: Create a VM. */
	DRM_PANTHOR_VM_CREATE,

	/** @DRM_PANTHOR_VM_DESTROY: Destroy a VM. */
	DRM_PANTHOR_VM_DESTROY,

	/** @DRM_PANTHOR_VM_BIND: Bind/unbind memory to a VM. */
	DRM_PANTHOR_VM_BIND,

	/** @DRM_PANTHOR_VM_GET_STATE: Get VM state. */
	DRM_PANTHOR_VM_GET_STATE,

	/** @DRM_PANTHOR_BO_CREATE: Create a buffer object. */
	DRM_PANTHOR_BO_CREATE,

	/**
	 * @DRM_PANTHOR_BO_MMAP_OFFSET: Get the file offset to pass to
	 * mmap to map a GEM object.
	 */
	DRM_PANTHOR_BO_MMAP_OFFSET,

	/** @DRM_PANTHOR_GROUP_CREATE: Create a scheduling group. */
	DRM_PANTHOR_GROUP_CREATE,

	/** @DRM_PANTHOR_GROUP_DESTROY: Destroy a scheduling group. */
	DRM_PANTHOR_GROUP_DESTROY,

	/**
	 * @DRM_PANTHOR_GROUP_SUBMIT: Submit jobs to queues belonging
	 * to a specific scheduling group.
	 */
	DRM_PANTHOR_GROUP_SUBMIT,

	/** @DRM_PANTHOR_GROUP_GET_STATE: Get the state of a scheduling group. */
	DRM_PANTHOR_GROUP_GET_STATE,

	/** @DRM_PANTHOR_TILER_HEAP_CREATE: Create a tiler heap. */
	DRM_PANTHOR_TILER_HEAP_CREATE,

	/** @DRM_PANTHOR_TILER_HEAP_DESTROY: Destroy a tiler heap. */
	DRM_PANTHOR_TILER_HEAP_DESTROY,

	/** @DRM_PANTHOR_PERF_CONTROL: Control a performance counter session. */
	DRM_PANTHOR_PERF_CONTROL,
};

/**
 * DRM_IOCTL_PANTHOR() - Build a Panthor IOCTL number
 * @__access: Access type. Must be R, W or RW.
 * @__id: One of the DRM_PANTHOR_xxx id.
 * @__type: Suffix of the type being passed to the IOCTL.
 *
 * Don't use this macro directly, use the DRM_IOCTL_PANTHOR_xxx
 * values instead.
 *
 * Return: An IOCTL number to be passed to ioctl() from userspace.
 */
#define DRM_IOCTL_PANTHOR(__access, __id, __type) \
	DRM_IO ## __access(DRM_COMMAND_BASE + DRM_PANTHOR_ ## __id, \
			   struct drm_panthor_ ## __type)

#define DRM_IOCTL_PANTHOR_DEV_QUERY \
	DRM_IOCTL_PANTHOR(WR, DEV_QUERY, dev_query)
#define DRM_IOCTL_PANTHOR_VM_CREATE \
	DRM_IOCTL_PANTHOR(WR, VM_CREATE, vm_create)
#define DRM_IOCTL_PANTHOR_VM_DESTROY \
	DRM_IOCTL_PANTHOR(WR, VM_DESTROY, vm_destroy)
#define DRM_IOCTL_PANTHOR_VM_BIND \
	DRM_IOCTL_PANTHOR(WR, VM_BIND, vm_bind)
#define DRM_IOCTL_PANTHOR_VM_GET_STATE \
	DRM_IOCTL_PANTHOR(WR, VM_GET_STATE, vm_get_state)
#define DRM_IOCTL_PANTHOR_BO_CREATE \
	DRM_IOCTL_PANTHOR(WR, BO_CREATE, bo_create)
#define DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET \
	DRM_IOCTL_PANTHOR(WR, BO_MMAP_OFFSET, bo_mmap_offset)
#define DRM_IOCTL_PANTHOR_GROUP_CREATE \
	DRM_IOCTL_PANTHOR(WR, GROUP_CREATE, group_create)
#define DRM_IOCTL_PANTHOR_GROUP_DESTROY \
	DRM_IOCTL_PANTHOR(WR, GROUP_DESTROY, group_destroy)
#define DRM_IOCTL_PANTHOR_GROUP_SUBMIT \
	DRM_IOCTL_PANTHOR(WR, GROUP_SUBMIT, group_submit)
#define DRM_IOCTL_PANTHOR_GROUP_GET_STATE \
	DRM_IOCTL_PANTHOR(WR, GROUP_GET_STATE, group_get_state)
#define DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE \
	DRM_IOCTL_PANTHOR(WR, TILER_HEAP_CREATE, tiler_heap_create)
#define DRM_IOCTL_PANTHOR_TILER_HEAP_DESTROY \
	DRM_IOCTL_PANTHOR(WR, TILER_HEAP_DESTROY, tiler_heap_destroy)
#define DRM_IOCTL_PANTHOR_PERF_CONTROL \
	DRM_IOCTL_PANTHOR(WR, PERF_CONTROL, perf_control)

/**
 * DOC: IOCTL arguments
 */

/**
 * struct drm_panthor_obj_array - Object array.
 *
 * This object is used to pass an array of objects whose size is subject to changes in
 * future versions of the driver. In order to support this mutability, we pass a stride
 * describing the size of the object as known by userspace.
 *
 * You shouldn't fill drm_panthor_obj_array fields directly. You should instead use
 * the DRM_PANTHOR_OBJ_ARRAY() macro that takes care of initializing the stride to
 * the object size.
 */
struct drm_panthor_obj_array {
	/** @stride: Stride of object struct. Used for versioning. */
	__u32 stride;

	/** @count: Number of objects in the array. */
	__u32 count;

	/** @array: User pointer to an array of objects. */
	__u64 array;
};

/**
 * DRM_PANTHOR_OBJ_ARRAY() - Initialize a drm_panthor_obj_array field.
 * @cnt: Number of elements in the array.
 * @ptr: Pointer to the array to pass to the kernel.
 *
 * Macro initializing a drm_panthor_obj_array based on the object size as known
 * by userspace.
 */
#define DRM_PANTHOR_OBJ_ARRAY(cnt, ptr) \
	{ .stride = sizeof((ptr)[0]), .count = (cnt), .array = (__u64)(uintptr_t)(ptr) }

/**
 * enum drm_panthor_sync_op_flags - Synchronization operation flags.
 */
enum drm_panthor_sync_op_flags {
	/** @DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_MASK: Synchronization handle type mask. */
	DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_MASK = 0xff,

	/** @DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ: Synchronization object type. */
	DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_SYNCOBJ = 0,

	/**
	 * @DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ: Timeline synchronization
	 * object type.
	 */
	DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ = 1,

	/** @DRM_PANTHOR_SYNC_OP_WAIT: Wait operation. */
	DRM_PANTHOR_SYNC_OP_WAIT = 0 << 31,

	/** @DRM_PANTHOR_SYNC_OP_SIGNAL: Signal operation. */
	DRM_PANTHOR_SYNC_OP_SIGNAL = (int)(1u << 31),
};

/**
 * struct drm_panthor_sync_op - Synchronization operation.
 */
struct drm_panthor_sync_op {
	/** @flags: Synchronization operation flags. Combination of DRM_PANTHOR_SYNC_OP values. */
	__u32 flags;

	/** @handle: Sync handle. */
	__u32 handle;

	/**
	 * @timeline_value: MBZ if
	 * (flags & DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_MASK) !=
	 * DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ.
	 */
	__u64 timeline_value;
};

/**
 * enum drm_panthor_dev_query_type - Query type
 *
 * Place new types at the end, don't re-order, don't remove or replace.
 */
enum drm_panthor_dev_query_type {
	/** @DRM_PANTHOR_DEV_QUERY_GPU_INFO: Query GPU information. */
	DRM_PANTHOR_DEV_QUERY_GPU_INFO = 0,

	/** @DRM_PANTHOR_DEV_QUERY_CSIF_INFO: Query command-stream interface information. */
	DRM_PANTHOR_DEV_QUERY_CSIF_INFO,

	/** @DRM_PANTHOR_DEV_QUERY_TIMESTAMP_INFO: Query timestamp information. */
	DRM_PANTHOR_DEV_QUERY_TIMESTAMP_INFO,

	/**
	 * @DRM_PANTHOR_DEV_QUERY_GROUP_PRIORITIES_INFO: Query allowed group priorities information.
	 */
	DRM_PANTHOR_DEV_QUERY_GROUP_PRIORITIES_INFO,

	/** @DRM_PANTHOR_DEV_QUERY_PERF_INFO: Query performance counter interface information. */
	DRM_PANTHOR_DEV_QUERY_PERF_INFO,
};

/**
 * struct drm_panthor_gpu_info - GPU information
 *
 * Structure grouping all queryable information relating to the GPU.
 */
struct drm_panthor_gpu_info {
	/** @gpu_id : GPU ID. */
	__u32 gpu_id;
#define DRM_PANTHOR_ARCH_MAJOR(x)		((x) >> 28)
#define DRM_PANTHOR_ARCH_MINOR(x)		(((x) >> 24) & 0xf)
#define DRM_PANTHOR_ARCH_REV(x)			(((x) >> 20) & 0xf)
#define DRM_PANTHOR_PRODUCT_MAJOR(x)		(((x) >> 16) & 0xf)
#define DRM_PANTHOR_VERSION_MAJOR(x)		(((x) >> 12) & 0xf)
#define DRM_PANTHOR_VERSION_MINOR(x)		(((x) >> 4) & 0xff)
#define DRM_PANTHOR_VERSION_STATUS(x)		((x) & 0xf)

	/** @gpu_rev: GPU revision. */
	__u32 gpu_rev;

	/** @csf_id: Command stream frontend ID. */
	__u32 csf_id;
#define DRM_PANTHOR_CSHW_MAJOR(x)		(((x) >> 26) & 0x3f)
#define DRM_PANTHOR_CSHW_MINOR(x)		(((x) >> 20) & 0x3f)
#define DRM_PANTHOR_CSHW_REV(x)			(((x) >> 16) & 0xf)
#define DRM_PANTHOR_MCU_MAJOR(x)		(((x) >> 10) & 0x3f)
#define DRM_PANTHOR_MCU_MINOR(x)		(((x) >> 4) & 0x3f)
#define DRM_PANTHOR_MCU_REV(x)			((x) & 0xf)

	/** @l2_features: L2-cache features. */
	__u32 l2_features;

	/** @tiler_features: Tiler features. */
	__u32 tiler_features;

	/** @mem_features: Memory features. */
	__u32 mem_features;

	/** @mmu_features: MMU features. */
	__u32 mmu_features;
#define DRM_PANTHOR_MMU_VA_BITS(x)		((x) & 0xff)

	/** @thread_features: Thread features. */
	__u32 thread_features;

	/** @max_threads: Maximum number of threads. */
	__u32 max_threads;

	/** @thread_max_workgroup_size: Maximum workgroup size. */
	__u32 thread_max_workgroup_size;

	/**
	 * @thread_max_barrier_size: Maximum number of threads that can wait
	 * simultaneously on a barrier.
	 */
	__u32 thread_max_barrier_size;

	/** @coherency_features: Coherency features. */
	__u32 coherency_features;

	/** @texture_features: Texture features. */
	__u32 texture_features[4];

	/** @as_present: Bitmask encoding the number of address-space exposed by the MMU. */
	__u32 as_present;

	/** @shader_present: Bitmask encoding the shader cores exposed by the GPU. */
	__u64 shader_present;

	/** @l2_present: Bitmask encoding the L2 caches exposed by the GPU. */
	__u64 l2_present;

	/** @tiler_present: Bitmask encoding the tiler units exposed by the GPU. */
	__u64 tiler_present;

	/** @core_features: Used to discriminate core variants when they exist. */
	__u32 core_features;

	/** @pad: MBZ. */
	__u32 pad;
};

/**
 * struct drm_panthor_csif_info - Command stream interface information
 *
 * Structure grouping all queryable information relating to the command stream interface.
 */
struct drm_panthor_csif_info {
	/** @csg_slot_count: Number of command stream group slots exposed by the firmware. */
	__u32 csg_slot_count;

	/** @cs_slot_count: Number of command stream slots per group. */
	__u32 cs_slot_count;

	/** @cs_reg_count: Number of command stream registers. */
	__u32 cs_reg_count;

	/** @scoreboard_slot_count: Number of scoreboard slots. */
	__u32 scoreboard_slot_count;

	/**
	 * @unpreserved_cs_reg_count: Number of command stream registers reserved by
	 * the kernel driver to call a userspace command stream.
	 *
	 * All registers can be used by a userspace command stream, but the
	 * [cs_slot_count - unpreserved_cs_reg_count .. cs_slot_count] registers are
	 * used by the kernel when DRM_PANTHOR_IOCTL_GROUP_SUBMIT is called.
	 */
	__u32 unpreserved_cs_reg_count;

	/**
	 * @pad: Padding field, set to zero.
	 */
	__u32 pad;
};

/**
 * struct drm_panthor_timestamp_info - Timestamp information
 *
 * Structure grouping all queryable information relating to the GPU timestamp.
 */
struct drm_panthor_timestamp_info {
	/**
	 * @timestamp_frequency: The frequency of the timestamp timer or 0 if
	 * unknown.
	 */
	__u64 timestamp_frequency;

	/** @current_timestamp: The current timestamp. */
	__u64 current_timestamp;

	/** @timestamp_offset: The offset of the timestamp timer. */
	__u64 timestamp_offset;
};

/**
 * struct drm_panthor_group_priorities_info - Group priorities information
 *
 * Structure grouping all queryable information relating to the allowed group priorities.
 */
struct drm_panthor_group_priorities_info {
	/**
	 * @allowed_mask: Bitmask of the allowed group priorities.
	 *
	 * Each bit represents a variant of the enum drm_panthor_group_priority.
	 */
	__u8 allowed_mask;

	/** @pad: Padding fields, MBZ. */
	__u8 pad[3];
};

/**
 * enum drm_panthor_perf_feat_flags - Performance counter configuration feature flags.
 */
enum drm_panthor_perf_feat_flags {
	/** @DRM_PANTHOR_PERF_BLOCK_STATES_SUPPORT: Coarse-grained block states are supported. */
	DRM_PANTHOR_PERF_BLOCK_STATES_SUPPORT = 1 << 0,
};

/**
 * enum drm_panthor_perf_block_type - Performance counter supported block types.
 */
enum drm_panthor_perf_block_type {
	/** DRM_PANTHOR_PERF_BLOCK_METADATA: Internal use only. */
	DRM_PANTHOR_PERF_BLOCK_METADATA = 0,

	/** @DRM_PANTHOR_PERF_BLOCK_FW: The FW counter block. */
	DRM_PANTHOR_PERF_BLOCK_FW,

	/** @DRM_PANTHOR_PERF_BLOCK_CSG: A CSG counter block. */
	DRM_PANTHOR_PERF_BLOCK_CSG,

	/** @DRM_PANTHOR_PERF_BLOCK_CSHW: The CSHW counter block. */
	DRM_PANTHOR_PERF_BLOCK_CSHW,

	/** @DRM_PANTHOR_PERF_BLOCK_TILER: The tiler counter block. */
	DRM_PANTHOR_PERF_BLOCK_TILER,

	/** @DRM_PANTHOR_PERF_BLOCK_MEMSYS: A memsys counter block. */
	DRM_PANTHOR_PERF_BLOCK_MEMSYS,

	/** @DRM_PANTHOR_PERF_BLOCK_SHADER: A shader core counter block. */
	DRM_PANTHOR_PERF_BLOCK_SHADER,

	/** @DRM_PANTHOR_PERF_BLOCK_LAST: Internal use only. */
	DRM_PANTHOR_PERF_BLOCK_LAST = DRM_PANTHOR_PERF_BLOCK_SHADER,

	/** @DRM_PANTHOR_PERF_BLOCK_MAX: Internal use only. */
	DRM_PANTHOR_PERF_BLOCK_MAX = DRM_PANTHOR_PERF_BLOCK_LAST + 1,
};

/**
 * enum drm_panthor_perf_clock - Identifier of the clock used to produce the cycle count values
 * in a given block.
 *
 * Since the integrator has the choice of using one or more clocks, there may be some confusion
 * as to which blocks are counted by which clock values unless this information is explicitly
 * provided as part of every block sample. Not every single clock here can be used: in the simplest
 * case, all cycle counts will be associated with the top-level clock.
 */
enum drm_panthor_perf_clock {
	/** @DRM_PANTHOR_PERF_CLOCK_TOPLEVEL: Top-level CSF clock. */
	DRM_PANTHOR_PERF_CLOCK_TOPLEVEL,

	/**
	 * @DRM_PANTHOR_PERF_CLOCK_COREGROUP: Core group clock, responsible for the MMU, L2
	 * caches and the tiler.
	 */
	DRM_PANTHOR_PERF_CLOCK_COREGROUP,

	/** @DRM_PANTHOR_PERF_CLOCK_SHADER: Clock for the shader cores. */
	DRM_PANTHOR_PERF_CLOCK_SHADER,
};

/**
 * struct drm_panthor_perf_info - Performance counter interface information
 *
 * Structure grouping all queryable information relating to the performance counter
 * interfaces.
 */
struct drm_panthor_perf_info {
	/**
	 * @counters_per_block: The number of 8-byte counters available in a block.
	 */
	__u32 counters_per_block;

	/**
	 * @sample_header_size: The size of the header struct available at the beginning
	 * of every sample.
	 */
	__u32 sample_header_size;

	/**
	 * @block_header_size: The size of the header struct inline with the counters for a
	 * single block.
	 */
	__u32 block_header_size;

	/** @flags: Combination of drm_panthor_perf_feat_flags flags. */
	__u32 flags;

	/**
	 * @supported_clocks: Bitmask of the clocks supported by the GPU.
	 *
	 * Each bit represents a variant of the enum drm_panthor_perf_clock.
	 *
	 * For the same GPU, different implementers may have different clocks for the same hardware
	 * block. At the moment, up to four clocks are supported, and any clocks that are present
	 * will be reported here.
	 */
	__u32 supported_clocks;

	/** @fw_blocks: Number of FW blocks available. */
	__u32 fw_blocks;

	/** @csg_blocks: Number of CSG blocks available. */
	__u32 csg_blocks;

	/** @cshw_blocks: Number of CSHW blocks available. */
	__u32 cshw_blocks;

	/** @tiler_blocks: Number of tiler blocks available. */
	__u32 tiler_blocks;

	/** @memsys_blocks: Number of memsys blocks available. */
	__u32 memsys_blocks;

	/** @shader_blocks: Number of shader core blocks available. */
	__u32 shader_blocks;

	/** @pad: MBZ. */
	__u32 pad;
};

/**
 * struct drm_panthor_dev_query - Arguments passed to DRM_PANTHOR_IOCTL_DEV_QUERY
 */
struct drm_panthor_dev_query {
	/** @type: the query type (see drm_panthor_dev_query_type). */
	__u32 type;

	/**
	 * @size: size of the type being queried.
	 *
	 * If pointer is NULL, size is updated by the driver to provide the
	 * output structure size. If pointer is not NULL, the driver will
	 * only copy min(size, actual_structure_size) bytes to the pointer,
	 * and update the size accordingly. This allows us to extend query
	 * types without breaking userspace.
	 */
	__u32 size;

	/**
	 * @pointer: user pointer to a query type struct.
	 *
	 * Pointer can be NULL, in which case, nothing is copied, but the
	 * actual structure size is returned. If not NULL, it must point to
	 * a location that's large enough to hold size bytes.
	 */
	__u64 pointer;
};

/**
 * struct drm_panthor_vm_create - Arguments passed to DRM_PANTHOR_IOCTL_VM_CREATE
 */
struct drm_panthor_vm_create {
	/** @flags: VM flags, MBZ. */
	__u32 flags;

	/** @id: Returned VM ID. */
	__u32 id;

	/**
	 * @user_va_range: Size of the VA space reserved for user objects.
	 *
	 * The kernel will pick the remaining space to map kernel-only objects to the
	 * VM (heap chunks, heap context, ring buffers, kernel synchronization objects,
	 * ...). If the space left for kernel objects is too small, kernel object
	 * allocation will fail further down the road. One can use
	 * drm_panthor_gpu_info::mmu_features to extract the total virtual address
	 * range, and chose a user_va_range that leaves some space to the kernel.
	 *
	 * If user_va_range is zero, the kernel will pick a sensible value based on
	 * TASK_SIZE and the virtual range supported by the GPU MMU (the kernel/user
	 * split should leave enough VA space for userspace processes to support SVM,
	 * while still allowing the kernel to map some amount of kernel objects in
	 * the kernel VA range). The value chosen by the driver will be returned in
	 * @user_va_range.
	 *
	 * User VA space always starts at 0x0, kernel VA space is always placed after
	 * the user VA range.
	 */
	__u64 user_va_range;
};

/**
 * struct drm_panthor_vm_destroy - Arguments passed to DRM_PANTHOR_IOCTL_VM_DESTROY
 */
struct drm_panthor_vm_destroy {
	/** @id: ID of the VM to destroy. */
	__u32 id;

	/** @pad: MBZ. */
	__u32 pad;
};

/**
 * enum drm_panthor_vm_bind_op_flags - VM bind operation flags
 */
enum drm_panthor_vm_bind_op_flags {
	/**
	 * @DRM_PANTHOR_VM_BIND_OP_MAP_READONLY: Map the memory read-only.
	 *
	 * Only valid with DRM_PANTHOR_VM_BIND_OP_TYPE_MAP.
	 */
	DRM_PANTHOR_VM_BIND_OP_MAP_READONLY = 1 << 0,

	/**
	 * @DRM_PANTHOR_VM_BIND_OP_MAP_NOEXEC: Map the memory not-executable.
	 *
	 * Only valid with DRM_PANTHOR_VM_BIND_OP_TYPE_MAP.
	 */
	DRM_PANTHOR_VM_BIND_OP_MAP_NOEXEC = 1 << 1,

	/**
	 * @DRM_PANTHOR_VM_BIND_OP_MAP_UNCACHED: Map the memory uncached.
	 *
	 * Only valid with DRM_PANTHOR_VM_BIND_OP_TYPE_MAP.
	 */
	DRM_PANTHOR_VM_BIND_OP_MAP_UNCACHED = 1 << 2,

	/**
	 * @DRM_PANTHOR_VM_BIND_OP_TYPE_MASK: Mask used to determine the type of operation.
	 */
	DRM_PANTHOR_VM_BIND_OP_TYPE_MASK = (int)(0xfu << 28),

	/** @DRM_PANTHOR_VM_BIND_OP_TYPE_MAP: Map operation. */
	DRM_PANTHOR_VM_BIND_OP_TYPE_MAP = 0 << 28,

	/** @DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP: Unmap operation. */
	DRM_PANTHOR_VM_BIND_OP_TYPE_UNMAP = 1 << 28,

	/**
	 * @DRM_PANTHOR_VM_BIND_OP_TYPE_SYNC_ONLY: No VM operation.
	 *
	 * Just serves as a synchronization point on a VM queue.
	 *
	 * Only valid if %DRM_PANTHOR_VM_BIND_ASYNC is set in drm_panthor_vm_bind::flags,
	 * and drm_panthor_vm_bind_op::syncs contains at least one element.
	 */
	DRM_PANTHOR_VM_BIND_OP_TYPE_SYNC_ONLY = 2 << 28,
};

/**
 * struct drm_panthor_vm_bind_op - VM bind operation
 */
struct drm_panthor_vm_bind_op {
	/** @flags: Combination of drm_panthor_vm_bind_op_flags flags. */
	__u32 flags;

	/**
	 * @bo_handle: Handle of the buffer object to map.
	 * MBZ for unmap or sync-only operations.
	 */
	__u32 bo_handle;

	/**
	 * @bo_offset: Buffer object offset.
	 * MBZ for unmap or sync-only operations.
	 */
	__u64 bo_offset;

	/**
	 * @va: Virtual address to map/unmap.
	 * MBZ for sync-only operations.
	 */
	__u64 va;

	/**
	 * @size: Size to map/unmap.
	 * MBZ for sync-only operations.
	 */
	__u64 size;

	/**
	 * @syncs: Array of struct drm_panthor_sync_op synchronization
	 * operations.
	 *
	 * This array must be empty if %DRM_PANTHOR_VM_BIND_ASYNC is not set on
	 * the drm_panthor_vm_bind object containing this VM bind operation.
	 *
	 * This array shall not be empty for sync-only operations.
	 */
	struct drm_panthor_obj_array syncs;

};

/**
 * enum drm_panthor_vm_bind_flags - VM bind flags
 */
enum drm_panthor_vm_bind_flags {
	/**
	 * @DRM_PANTHOR_VM_BIND_ASYNC: VM bind operations are queued to the VM
	 * queue instead of being executed synchronously.
	 */
	DRM_PANTHOR_VM_BIND_ASYNC = 1 << 0,
};

/**
 * struct drm_panthor_vm_bind - Arguments passed to DRM_IOCTL_PANTHOR_VM_BIND
 */
struct drm_panthor_vm_bind {
	/** @vm_id: VM targeted by the bind request. */
	__u32 vm_id;

	/** @flags: Combination of drm_panthor_vm_bind_flags flags. */
	__u32 flags;

	/** @ops: Array of struct drm_panthor_vm_bind_op bind operations. */
	struct drm_panthor_obj_array ops;
};

/**
 * enum drm_panthor_vm_state - VM states.
 */
enum drm_panthor_vm_state {
	/**
	 * @DRM_PANTHOR_VM_STATE_USABLE: VM is usable.
	 *
	 * New VM operations will be accepted on this VM.
	 */
	DRM_PANTHOR_VM_STATE_USABLE,

	/**
	 * @DRM_PANTHOR_VM_STATE_UNUSABLE: VM is unusable.
	 *
	 * Something put the VM in an unusable state (like an asynchronous
	 * VM_BIND request failing for any reason).
	 *
	 * Once the VM is in this state, all new MAP operations will be
	 * rejected, and any GPU job targeting this VM will fail.
	 * UNMAP operations are still accepted.
	 *
	 * The only way to recover from an unusable VM is to create a new
	 * VM, and destroy the old one.
	 */
	DRM_PANTHOR_VM_STATE_UNUSABLE,
};

/**
 * struct drm_panthor_vm_get_state - Get VM state.
 */
struct drm_panthor_vm_get_state {
	/** @vm_id: VM targeted by the get_state request. */
	__u32 vm_id;

	/**
	 * @state: state returned by the driver.
	 *
	 * Must be one of the enum drm_panthor_vm_state values.
	 */
	__u32 state;
};

/**
 * enum drm_panthor_bo_flags - Buffer object flags, passed at creation time.
 */
enum drm_panthor_bo_flags {
	/** @DRM_PANTHOR_BO_NO_MMAP: The buffer object will never be CPU-mapped in userspace. */
	DRM_PANTHOR_BO_NO_MMAP = (1 << 0),
};

/**
 * struct drm_panthor_bo_create - Arguments passed to DRM_IOCTL_PANTHOR_BO_CREATE.
 */
struct drm_panthor_bo_create {
	/**
	 * @size: Requested size for the object
	 *
	 * The (page-aligned) allocated size for the object will be returned.
	 */
	__u64 size;

	/**
	 * @flags: Flags. Must be a combination of drm_panthor_bo_flags flags.
	 */
	__u32 flags;

	/**
	 * @exclusive_vm_id: Exclusive VM this buffer object will be mapped to.
	 *
	 * If not zero, the field must refer to a valid VM ID, and implies that:
	 *  - the buffer object will only ever be bound to that VM
	 *  - cannot be exported as a PRIME fd
	 */
	__u32 exclusive_vm_id;

	/**
	 * @handle: Returned handle for the object.
	 *
	 * Object handles are nonzero.
	 */
	__u32 handle;

	/** @pad: MBZ. */
	__u32 pad;
};

/**
 * struct drm_panthor_bo_mmap_offset - Arguments passed to DRM_IOCTL_PANTHOR_BO_MMAP_OFFSET.
 */
struct drm_panthor_bo_mmap_offset {
	/** @handle: Handle of the object we want an mmap offset for. */
	__u32 handle;

	/** @pad: MBZ. */
	__u32 pad;

	/** @offset: The fake offset to use for subsequent mmap calls. */
	__u64 offset;
};

/**
 * struct drm_panthor_queue_create - Queue creation arguments.
 */
struct drm_panthor_queue_create {
	/**
	 * @priority: Defines the priority of queues inside a group. Goes from 0 to 15,
	 * 15 being the highest priority.
	 */
	__u8 priority;

	/** @pad: Padding fields, MBZ. */
	__u8 pad[3];

	/** @ringbuf_size: Size of the ring buffer to allocate to this queue. */
	__u32 ringbuf_size;
};

/**
 * enum drm_panthor_group_priority - Scheduling group priority
 */
enum drm_panthor_group_priority {
	/** @PANTHOR_GROUP_PRIORITY_LOW: Low priority group. */
	PANTHOR_GROUP_PRIORITY_LOW = 0,

	/** @PANTHOR_GROUP_PRIORITY_MEDIUM: Medium priority group. */
	PANTHOR_GROUP_PRIORITY_MEDIUM,

	/**
	 * @PANTHOR_GROUP_PRIORITY_HIGH: High priority group.
	 *
	 * Requires CAP_SYS_NICE or DRM_MASTER.
	 */
	PANTHOR_GROUP_PRIORITY_HIGH,

	/**
	 * @PANTHOR_GROUP_PRIORITY_REALTIME: Realtime priority group.
	 *
	 * Requires CAP_SYS_NICE or DRM_MASTER.
	 */
	PANTHOR_GROUP_PRIORITY_REALTIME,
};

/**
 * struct drm_panthor_group_create - Arguments passed to DRM_IOCTL_PANTHOR_GROUP_CREATE
 */
struct drm_panthor_group_create {
	/** @queues: Array of drm_panthor_queue_create elements. */
	struct drm_panthor_obj_array queues;

	/**
	 * @max_compute_cores: Maximum number of cores that can be used by compute
	 * jobs across CS queues bound to this group.
	 *
	 * Must be less or equal to the number of bits set in @compute_core_mask.
	 */
	__u8 max_compute_cores;

	/**
	 * @max_fragment_cores: Maximum number of cores that can be used by fragment
	 * jobs across CS queues bound to this group.
	 *
	 * Must be less or equal to the number of bits set in @fragment_core_mask.
	 */
	__u8 max_fragment_cores;

	/**
	 * @max_tiler_cores: Maximum number of tilers that can be used by tiler jobs
	 * across CS queues bound to this group.
	 *
	 * Must be less or equal to the number of bits set in @tiler_core_mask.
	 */
	__u8 max_tiler_cores;

	/** @priority: Group priority (see enum drm_panthor_group_priority). */
	__u8 priority;

	/** @pad: Padding field, MBZ. */
	__u32 pad;

	/**
	 * @compute_core_mask: Mask encoding cores that can be used for compute jobs.
	 *
	 * This field must have at least @max_compute_cores bits set.
	 *
	 * The bits set here should also be set in drm_panthor_gpu_info::shader_present.
	 */
	__u64 compute_core_mask;

	/**
	 * @fragment_core_mask: Mask encoding cores that can be used for fragment jobs.
	 *
	 * This field must have at least @max_fragment_cores bits set.
	 *
	 * The bits set here should also be set in drm_panthor_gpu_info::shader_present.
	 */
	__u64 fragment_core_mask;

	/**
	 * @tiler_core_mask: Mask encoding cores that can be used for tiler jobs.
	 *
	 * This field must have at least @max_tiler_cores bits set.
	 *
	 * The bits set here should also be set in drm_panthor_gpu_info::tiler_present.
	 */
	__u64 tiler_core_mask;

	/**
	 * @vm_id: VM ID to bind this group to.
	 *
	 * All submission to queues bound to this group will use this VM.
	 */
	__u32 vm_id;

	/**
	 * @group_handle: Returned group handle. Passed back when submitting jobs or
	 * destroying a group.
	 */
	__u32 group_handle;
};

/**
 * struct drm_panthor_group_destroy - Arguments passed to DRM_IOCTL_PANTHOR_GROUP_DESTROY
 */
struct drm_panthor_group_destroy {
	/** @group_handle: Group to destroy */
	__u32 group_handle;

	/** @pad: Padding field, MBZ. */
	__u32 pad;
};

/**
 * struct drm_panthor_queue_submit - Job submission arguments.
 *
 * This is describing the userspace command stream to call from the kernel
 * command stream ring-buffer. Queue submission is always part of a group
 * submission, taking one or more jobs to submit to the underlying queues.
 */
struct drm_panthor_queue_submit {
	/** @queue_index: Index of the queue inside a group. */
	__u32 queue_index;

	/**
	 * @stream_size: Size of the command stream to execute.
	 *
	 * Must be 64-bit/8-byte aligned (the size of a CS instruction)
	 *
	 * Can be zero if stream_addr is zero too.
	 *
	 * When the stream size is zero, the queue submit serves as a
	 * synchronization point.
	 */
	__u32 stream_size;

	/**
	 * @stream_addr: GPU address of the command stream to execute.
	 *
	 * Must be aligned on 64-byte.
	 *
	 * Can be zero is stream_size is zero too.
	 */
	__u64 stream_addr;

	/**
	 * @latest_flush: FLUSH_ID read at the time the stream was built.
	 *
	 * This allows cache flush elimination for the automatic
	 * flush+invalidate(all) done at submission time, which is needed to
	 * ensure the GPU doesn't get garbage when reading the indirect command
	 * stream buffers. If you want the cache flush to happen
	 * unconditionally, pass a zero here.
	 *
	 * Ignored when stream_size is zero.
	 */
	__u32 latest_flush;

	/** @pad: MBZ. */
	__u32 pad;

	/** @syncs: Array of struct drm_panthor_sync_op sync operations. */
	struct drm_panthor_obj_array syncs;
};

/**
 * struct drm_panthor_group_submit - Arguments passed to DRM_IOCTL_PANTHOR_GROUP_SUBMIT
 */
struct drm_panthor_group_submit {
	/** @group_handle: Handle of the group to queue jobs to. */
	__u32 group_handle;

	/** @pad: MBZ. */
	__u32 pad;

	/** @queue_submits: Array of drm_panthor_queue_submit objects. */
	struct drm_panthor_obj_array queue_submits;
};

/**
 * enum drm_panthor_group_state_flags - Group state flags
 */
enum drm_panthor_group_state_flags {
	/**
	 * @DRM_PANTHOR_GROUP_STATE_TIMEDOUT: Group had unfinished jobs.
	 *
	 * When a group ends up with this flag set, no jobs can be submitted to its queues.
	 */
	DRM_PANTHOR_GROUP_STATE_TIMEDOUT = 1 << 0,

	/**
	 * @DRM_PANTHOR_GROUP_STATE_FATAL_FAULT: Group had fatal faults.
	 *
	 * When a group ends up with this flag set, no jobs can be submitted to its queues.
	 */
	DRM_PANTHOR_GROUP_STATE_FATAL_FAULT = 1 << 1,
};

/**
 * struct drm_panthor_group_get_state - Arguments passed to DRM_IOCTL_PANTHOR_GROUP_GET_STATE
 *
 * Used to query the state of a group and decide whether a new group should be created to
 * replace it.
 */
struct drm_panthor_group_get_state {
	/** @group_handle: Handle of the group to query state on */
	__u32 group_handle;

	/**
	 * @state: Combination of DRM_PANTHOR_GROUP_STATE_* flags encoding the
	 * group state.
	 */
	__u32 state;

	/** @fatal_queues: Bitmask of queues that faced fatal faults. */
	__u32 fatal_queues;

	/** @pad: MBZ */
	__u32 pad;
};

/**
 * struct drm_panthor_tiler_heap_create - Arguments passed to DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE
 */
struct drm_panthor_tiler_heap_create {
	/** @vm_id: VM ID the tiler heap should be mapped to */
	__u32 vm_id;

	/** @initial_chunk_count: Initial number of chunks to allocate. Must be at least one. */
	__u32 initial_chunk_count;

	/**
	 * @chunk_size: Chunk size.
	 *
	 * Must be page-aligned and lie in the [128k:8M] range.
	 */
	__u32 chunk_size;

	/**
	 * @max_chunks: Maximum number of chunks that can be allocated.
	 *
	 * Must be at least @initial_chunk_count.
	 */
	__u32 max_chunks;

	/**
	 * @target_in_flight: Maximum number of in-flight render passes.
	 *
	 * If the heap has more than tiler jobs in-flight, the FW will wait for render
	 * passes to finish before queuing new tiler jobs.
	 */
	__u32 target_in_flight;

	/** @handle: Returned heap handle. Passed back to DESTROY_TILER_HEAP. */
	__u32 handle;

	/** @tiler_heap_ctx_gpu_va: Returned heap GPU virtual address returned */
	__u64 tiler_heap_ctx_gpu_va;

	/**
	 * @first_heap_chunk_gpu_va: First heap chunk.
	 *
	 * The tiler heap is formed of heap chunks forming a single-link list. This
	 * is the first element in the list.
	 */
	__u64 first_heap_chunk_gpu_va;
};

/**
 * struct drm_panthor_tiler_heap_destroy - Arguments passed to DRM_IOCTL_PANTHOR_TILER_HEAP_DESTROY
 */
struct drm_panthor_tiler_heap_destroy {
	/**
	 * @handle: Handle of the tiler heap to destroy.
	 *
	 * Must be a valid heap handle returned by DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE.
	 */
	__u32 handle;

	/** @pad: Padding field, MBZ. */
	__u32 pad;
};

/**
 * DOC: Performance counter decoding in userspace.
 *
 * Each sample will be exposed to userspace in the following manner:
 *
 * +--------+--------+------------------------+--------+-------------------------+-----+
 * | Sample | Block  |        Block           | Block  |         Block           | ... |
 * | header | header |        counters        | header |         counters        |     |
 * +--------+--------+------------------------+--------+-------------------------+-----+
 *
 * Each sample will start with a sample header of type @struct drm_panthor_perf_sample header,
 * providing sample-wide information like the start and end timestamps, the counter set currently
 * configured, and any errors that may have occurred during sampling.
 *
 * After the fixed size header, the sample will consist of blocks of
 * 64-bit @drm_panthor_dev_query_perf_info::counters_per_block counters, each prefaced with a
 * header of its own, indicating source block type, as well as the cycle count needed to normalize
 * cycle values within that block, and a clock source identifier.
 */

/**
 * enum drm_panthor_perf_block_state - Bitmask of the power and execution states that an individual
 * hardware block went through in a sampling period.
 *
 * Because the sampling period is controlled from userspace, the block may undergo multiple
 * state transitions, so this must be interpreted as one or more such transitions occurring.
 */
enum drm_panthor_perf_block_state {
	/**
	 * @DRM_PANTHOR_PERF_BLOCK_STATE_UNKNOWN: The state of this block was unknown during
	 * the sampling period.
	 */
	DRM_PANTHOR_PERF_BLOCK_STATE_UNKNOWN = 0,

	/**
	 * @DRM_PANTHOR_PERF_BLOCK_STATE_ON: This block was powered on for some or all of
	 * the sampling period.
	 */
	DRM_PANTHOR_PERF_BLOCK_STATE_ON = 1 << 0,

	/**
	 * @DRM_PANTHOR_PERF_BLOCK_STATE_OFF: This block was powered off for some or all of the
	 * sampling period.
	 */
	DRM_PANTHOR_PERF_BLOCK_STATE_OFF = 1 << 1,

	/**
	 * @DRM_PANTHOR_PERF_BLOCK_STATE_AVAILABLE: This block was available for execution for
	 * some or all of the sampling period.
	 */
	DRM_PANTHOR_PERF_BLOCK_STATE_AVAILABLE = 1 << 2,
	/**
	 * @DRM_PANTHOR_PERF_BLOCK_STATE_UNAVAILABLE: This block was unavailable for execution for
	 * some or all of the sampling period.
	 */
	DRM_PANTHOR_PERF_BLOCK_STATE_UNAVAILABLE = 1 << 3,

	/**
	 * @DRM_PANTHOR_PERF_BLOCK_STATE_NORMAL: This block was executing in normal mode
	 * for some or all of the sampling period.
	 */
	DRM_PANTHOR_PERF_BLOCK_STATE_NORMAL = 1 << 4,

	/**
	 * @DRM_PANTHOR_PERF_BLOCK_STATE_PROTECTED: This block was executing in protected mode
	 * for some or all of the sampling period.
	 */
	DRM_PANTHOR_PERF_BLOCK_STATE_PROTECTED = 1 << 5,
};

/**
 * struct drm_panthor_perf_block_header - Header present before every block in the
 * sample ringbuffer.
 */
struct drm_panthor_perf_block_header {
	/** @block_type: Type of the block. */
	__u8 block_type;

	/** @block_idx: Block index. */
	__u8 block_idx;

	/**
	 * @block_states: Coarse-grained block transitions, bitmask of enum
	 * drm_panthor_perf_block_states.
	 */
	__u8 block_states;

	/**
	 * @clock: Clock used to produce the cycle count for this block, taken from
	 * enum drm_panthor_perf_clock. The cycle counts are stored in the sample header.
	 */
	__u8 clock;

	/** @pad: MBZ. */
	__u8 pad[4];

	/** @enable_mask: Bitmask of counters requested during the session setup. */
	__u64 enable_mask[2];
};

/**
 * enum drm_panthor_perf_sample_flags - Sample-wide events that occurred over the sampling
 * period.
 */
enum drm_panthor_perf_sample_flags {
	/**
	 * @DRM_PANTHOR_PERF_SAMPLE_OVERFLOW: This sample contains overflows due to the duration
	 * of the sampling period.
	 */
	DRM_PANTHOR_PERF_SAMPLE_OVERFLOW = 1 << 0,

	/**
	 * @DRM_PANTHOR_PERF_SAMPLE_ERROR: This sample encountered an error condition during
	 * the sample duration.
	 */
	DRM_PANTHOR_PERF_SAMPLE_ERROR = 1 << 1,
};

/**
 * struct drm_panthor_perf_sample_header - Header present before every sample.
 */
struct drm_panthor_perf_sample_header {
	/**
	 * @timestamp_start_ns: Earliest timestamp that values in this sample represent, in
	 * nanoseconds. Derived from CLOCK_MONOTONIC_RAW.
	 */
	__u64 timestamp_start_ns;

	/**
	 * @timestamp_end_ns: Latest timestamp that values in this sample represent, in
	 * nanoseconds. Derived from CLOCK_MONOTONIC_RAW.
	 */
	__u64 timestamp_end_ns;

	/** @block_set: Set of performance counter blocks. */
	__u8 block_set;

	/** @pad: MBZ. */
	__u8 pad[3];

	/** @flags: Current sample flags, combination of drm_panthor_perf_sample_flags. */
	__u32 flags;

	/**
	 * @user_data: User data provided as part of the command that triggered this sample.
	 *
	 * - Automatic samples (periodic ones or those around non-counting periods or power state
	 * transitions) will be tagged with the user_data provided as part of the
	 * DRM_PANTHOR_PERF_COMMAND_START call.
	 * - Manual samples will be tagged with the user_data provided with the
	 * DRM_PANTHOR_PERF_COMMAND_SAMPLE call.
	 * - A session's final automatic sample will be tagged with the user_data provided with the
	 * DRM_PANTHOR_PERF_COMMAND_STOP call.
	 */
	__u64 user_data;

	/**
	 * @toplevel_clock_cycles: The number of cycles elapsed between
	 * drm_panthor_perf_sample_header::timestamp_start_ns and
	 * drm_panthor_perf_sample_header::timestamp_end_ns on the top-level clock if the
	 * corresponding bit is set in drm_panthor_perf_info::supported_clocks.
	 */
	__u64 toplevel_clock_cycles;

	/**
	 * @coregroup_clock_cycles: The number of cycles elapsed between
	 * drm_panthor_perf_sample_header::timestamp_start_ns and
	 * drm_panthor_perf_sample_header::timestamp_end_ns on the coregroup clock if the
	 * corresponding bit is set in drm_panthor_perf_info::supported_clocks.
	 */
	__u64 coregroup_clock_cycles;

	/**
	 * @shader_clock_cycles: The number of cycles elapsed between
	 * drm_panthor_perf_sample_header::timestamp_start_ns and
	 * drm_panthor_perf_sample_header::timestamp_end_ns on the shader core clock if the
	 * corresponding bit is set in drm_panthor_perf_info::supported_clocks.
	 */
	__u64 shader_clock_cycles;
};

/**
 * enum drm_panthor_perf_command - Command type passed to the DRM_PANTHOR_PERF_CONTROL
 * IOCTL.
 */
enum drm_panthor_perf_command {
	/** @DRM_PANTHOR_PERF_COMMAND_SETUP: Create a new performance counter sampling context. */
	DRM_PANTHOR_PERF_COMMAND_SETUP,

	/** @DRM_PANTHOR_PERF_COMMAND_TEARDOWN: Teardown a performance counter sampling context. */
	DRM_PANTHOR_PERF_COMMAND_TEARDOWN,

	/** @DRM_PANTHOR_PERF_COMMAND_START: Start a sampling session on the indicated context. */
	DRM_PANTHOR_PERF_COMMAND_START,

	/** @DRM_PANTHOR_PERF_COMMAND_STOP: Stop the sampling session on the indicated context. */
	DRM_PANTHOR_PERF_COMMAND_STOP,

	/**
	 * @DRM_PANTHOR_PERF_COMMAND_SAMPLE: Request a manual sample on the indicated context.
	 *
	 * When the sampling session is configured with a non-zero sampling frequency, any
	 * DRM_PANTHOR_PERF_CONTROL calls with this command will be ignored and return an
	 * -EINVAL.
	 */
	DRM_PANTHOR_PERF_COMMAND_SAMPLE,
};

/**
 * struct drm_panthor_perf_control - Arguments passed to DRM_PANTHOR_IOCTL_PERF_CONTROL.
 */
struct drm_panthor_perf_control {
	/** @cmd: Command from enum drm_panthor_perf_command. */
	__u32 cmd;

	/**
	 * @handle: session handle.
	 *
	 * Returned by the DRM_PANTHOR_PERF_COMMAND_SETUP call.
	 * It must be used in subsequent commands for the same context.
	 */
	__u32 handle;

	/**
	 * @size: size of the command structure.
	 *
	 * If the pointer is NULL, the size is updated by the driver to provide the size of the
	 * output structure. If the pointer is not NULL, the driver will only copy min(size,
	 * struct_size) to the pointer and update the size accordingly.
	 */
	__u64 size;

	/** @pointer: user pointer to a command type struct. */
	__u64 pointer;
};

/**
 * enum drm_panthor_perf_counter_set - The counter set to be requested from the hardware.
 *
 * The hardware supports a single performance counter set at a time, so requesting any set other
 * than the primary may fail if another process is sampling at the same time.
 *
 * If in doubt, the primary counter set has the most commonly used counters and requires no
 * additional permissions to open.
 */
enum drm_panthor_perf_counter_set {
	/**
	 * @DRM_PANTHOR_PERF_SET_PRIMARY: The default set configured on the hardware.
	 *
	 * This is the only set for which all counters in all blocks are defined.
	 */
	DRM_PANTHOR_PERF_SET_PRIMARY,

	/**
	 * @DRM_PANTHOR_PERF_SET_SECONDARY: The secondary performance counter set.
	 *
	 * Some blocks may not have any defined counters for this set, and the block will
	 * have the UNAVAILABLE block state permanently set in the block header.
	 *
	 * Accessing this set requires the calling process to have the CAP_PERFMON capability.
	 */
	DRM_PANTHOR_PERF_SET_SECONDARY,

	/**
	 * @DRM_PANTHOR_PERF_SET_TERTIARY: The tertiary performance counter set.
	 *
	 * Some blocks may not have any defined counters for this set, and the block will have
	 * the UNAVAILABLE block state permanently set in the block header. Note that the
	 * tertiary set has the fewest defined counter blocks.
	 *
	 * Accessing this set requires the calling process to have the CAP_PERFMON capability.
	 */
	DRM_PANTHOR_PERF_SET_TERTIARY,
};

/**
 * struct drm_panthor_perf_cmd_setup - Arguments passed to DRM_PANTHOR_IOCTL_PERF_CONTROL
 * when the DRM_PANTHOR_PERF_COMMAND_SETUP command is specified.
 */
struct drm_panthor_perf_cmd_setup {
	/**
	 * @block_set: Set of performance counter blocks, member of
	 *             enum drm_panthor_perf_block_set.
	 *
	 * This is a global configuration and only one set can be active at a time. If
	 * another client has already requested a counter set, any further requests
	 * for a different counter set will fail and return an -EBUSY.
	 *
	 * If the requested set does not exist, the request will fail and return an -EINVAL.
	 *
	 * Some sets have additional requirements to be enabled, and the setup request will
	 * fail with an -EACCES if these requirements are not satisfied.
	 */
	__u8 block_set;

	/** @pad: MBZ. */
	__u8 pad[7];

	/** @fd: eventfd for signalling the availability of a new sample. */
	__u32 fd;

	/** @ringbuf_handle: Handle to the BO to write perf counter sample to. */
	__u32 ringbuf_handle;

	/**
	 * @control_handle: Handle to the BO containing a contiguous 16 byte range, used for the
	 * insert and extract indices for the ringbuffer.
	 */
	__u32 control_handle;

	/**
	 * @sample_slots: The number of slots available in the userspace-provided BO. Must be
	 * a power of 2.
	 *
	 * If sample_slots * sample_size does not match the BO size, the setup request will fail.
	 */
	__u32 sample_slots;

	/**
	 * @control_offset: Offset into the control BO where the insert and extract indices are
	 * located.
	 */
	__u64 control_offset;

	/**
	 * @sample_freq_ns: Period between automatic counter sample collection in nanoseconds. Zero
	 * disables automatic collection and all collection must be done through explicit calls
	 * to DRM_PANTHOR_PERF_CONTROL.SAMPLE. Non-zero values will disable manual counter sampling
	 * via the DRM_PANTHOR_PERF_COMMAND_SAMPLE command.
	 *
	 * This disables software-triggered periodic sampling, but hardware will still trigger
	 * automatic samples on certain events, including shader core power transitions, and
	 * entries to and exits from non-counting periods. The final stop command will also
	 * trigger a sample to ensure no data is lost.
	 */
	__u64 sample_freq_ns;

	/**
	 * @fw_enable_mask: Bitmask of counters to request from the FW counter block. Any bits
	 * past the first drm_panthor_perf_info.counters_per_block bits will be ignored.
	 */
	__u64 fw_enable_mask[2];

	/**
	 * @csg_enable_mask: Bitmask of counters to request from the CSG counter blocks. Any bits
	 * past the first drm_panthor_perf_info.counters_per_block bits will be ignored.
	 */
	__u64 csg_enable_mask[2];

	/**
	 * @cshw_enable_mask: Bitmask of counters to request from the CSHW counter block. Any bits
	 * past the first drm_panthor_perf_info.counters_per_block bits will be ignored.
	 */
	__u64 cshw_enable_mask[2];

	/**
	 * @tiler_enable_mask: Bitmask of counters to request from the tiler counter block. Any
	 * bits past the first drm_panthor_perf_info.counters_per_block bits will be ignored.
	 */
	__u64 tiler_enable_mask[2];

	/**
	 * @memsys_enable_mask: Bitmask of counters to request from the memsys counter blocks. Any
	 * bits past the first drm_panthor_perf_info.counters_per_block bits will be ignored.
	 */
	__u64 memsys_enable_mask[2];

	/**
	 * @shader_enable_mask: Bitmask of counters to request from the shader core counter blocks.
	 * Any bits past the first drm_panthor_perf_info.counters_per_block bits will be ignored.
	 */
	__u64 shader_enable_mask[2];
};

/**
 * struct drm_panthor_perf_cmd_start - Arguments passed to DRM_PANTHOR_IOCTL_PERF_CONTROL
 * when the DRM_PANTHOR_PERF_COMMAND_START command is specified.
 */
struct drm_panthor_perf_cmd_start {
	/**
	 * @user_data: User provided data that will be attached to automatic samples collected
	 * until the next DRM_PANTHOR_PERF_COMMAND_STOP.
	 */
	__u64 user_data;
};

/**
 * struct drm_panthor_perf_cmd_stop - Arguments passed to DRM_PANTHOR_IOCTL_PERF_CONTROL
 * when the DRM_PANTHOR_PERF_COMMAND_STOP command is specified.
 */
struct drm_panthor_perf_cmd_stop {
	/**
	 * @user_data: User provided data that will be attached to the automatic sample collected
	 * at the end of this sampling session.
	 */
	__u64 user_data;
};

/**
 * struct drm_panthor_perf_cmd_sample - Arguments passed to DRM_PANTHOR_IOCTL_PERF_CONTROL
 * when the DRM_PANTHOR_PERF_COMMAND_SAMPLE command is specified.
 */
struct drm_panthor_perf_cmd_sample {
	/** @user_data: User provided data that will be attached to the sample.*/
	__u64 user_data;
};

#if defined(__cplusplus)
}
#endif

#endif /* _PANTHOR_DRM_H_ */
