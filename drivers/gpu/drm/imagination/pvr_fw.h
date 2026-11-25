/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright (c) 2023 Imagination Technologies Ltd. */

#ifndef PVR_FW_H
#define PVR_FW_H

#include "pvr_fw_trace.h"
#include "pvr_gem.h"

#include <drm/drm_mm.h>

#include <linux/array_size.h>
#include <linux/types.h>

/* Forward declarations from "pvr_device.h". */
struct pvr_device;
struct pvr_file;

/* Forward declaration from "pvr_vm.h". */
struct pvr_vm_context;

/* Forward declaration from <linux/firmware.h> */
struct firmware;

#define ROGUE_FWIF_FWCCB_NUMCMDS_LOG2 5

#define ROGUE_FWIF_KCCB_NUMCMDS_LOG2_DEFAULT 7

extern bool pvr_exp_fw_support;

/**
 * struct pvr_fw_version - Firmware version information
 * @major: Major version number.
 * @minor: Minor version number.
 * @build: Build number. This is informative only.
 * @open_source: Whether this firmware was built with open-source driver
 *               compatibility enabled.
 */
struct pvr_fw_version {
	u16 major;
	u16 minor;
	u32 build;
	bool open_source;
};

#define PVR_FW_VERSION_FMT "v%u.%u (build %u%s)"
#define PVR_FW_VERSION_FMT_ARGS(fw_version) \
	(fw_version)->major, (fw_version)->minor, (fw_version)->build, \
	(fw_version)->open_source ? " OS" : ""

#define PVR_FW_MAX_SUPPORTED_MAJOR_VERSION 1

/**
 * struct pvr_fw_object - container for firmware memory allocations
 */
struct pvr_fw_object {
	/** @ref_count: FW object reference counter. */
	struct kref ref_count;

	/** @gem: GEM object backing the FW object. */
	struct pvr_gem_object *gem;

	/**
	 * @fw_mm_node: Node representing mapping in FW address space. @pvr_obj->lock must
	 *              be held when writing.
	 */
	struct drm_mm_node fw_mm_node;

	/**
	 * @fw_addr_offset: Virtual address offset of firmware mapping. Only
	 *                  valid if @flags has %PVR_GEM_OBJECT_FLAGS_FW_MAPPED
	 *                  set.
	 */
	u32 fw_addr_offset;

	/**
	 * @init: Initialisation callback. Will be called on object creation and FW hard reset.
	 *        Object will have been zeroed before this is called.
	 */
	void (*init)(void *cpu_ptr, void *priv);

	/** @init_priv: Private data for initialisation callback. */
	void *init_priv;

	/** @node: Node for firmware object list. */
	struct list_head node;
};

enum pvr_fw_section_name {
	PVR_FW_SECTION_CODE = 0,
	PVR_FW_SECTION_PRIVATE_DATA,
	PVR_FW_SECTION_RODATA,
	PVR_FW_SECTION_COREMEM_CODE,
	PVR_FW_SECTION_COREMEM_DATA,
	PVR_FW_SECTION_EXCEPTIONS_CODE,
	PVR_FW_SECTION_BOOT_CODE,
	PVR_FW_SECTION_BOOT_DATA,
	PVR_FW_SECTION_STACK,
	PVR_FW_SECTION_UNCACHED_CODE,
	PVR_FW_SECTION_CACHED_CODE,

	PVR_FW_SECTION_COUNT,
	PVR_FW_SECTION_INVALID = -1,
};

enum pvr_fw_segment_name {
	PVR_FW_SEGMENT_CODE = 0,
	PVR_FW_SEGMENT_DATA,
	PVR_FW_SEGMENT_CORE_CODE,
	PVR_FW_SEGMENT_CORE_DATA,

	PVR_FW_SEGMENT_COUNT,
	PVR_FW_SEGMENT_INVALID = -1,
};

struct pvr_fw_section_desc {
	enum pvr_fw_section_name name;
	enum pvr_fw_segment_name segment;
	u32 base_addr;
	u32 size;
	u32 alloc_offset;
};

#define pvr_for_each_fw_section_desc(fw_metadata, name) \
	for (const struct pvr_fw_section_desc *name = (fw_metadata)->sections; \
	     name < (fw_metadata)->sections + (fw_metadata)->num_sections; \
	     name++)

struct pvr_fw_metadata {
	/** @version: Version of the loaded firmware file. */
	struct pvr_fw_version version;

	/** @num_sections: Number of section descriptors in @sections. */
	u32 num_sections;

	/** @sections: Section descriptors loaded from firmware header. */
	struct pvr_fw_section_desc sections[] __counted_by(num_sections);
};

struct pvr_fw_segment {
	enum pvr_fw_segment_name id;
	u32 size;
	struct pvr_fw_object *obj;
	void *host_copy;
};

struct pvr_fw_segment_desc {
	const char *name;
	bool required;
	void *(*create_and_map)(struct pvr_device *pvr_dev,
				struct pvr_fw_segment *segment,
				u32 fixed_data_addr);
};

extern const struct pvr_fw_segment_desc pvr_fw_segment_descs[PVR_FW_SEGMENT_COUNT];

/**
 * struct pvr_fw_defs - FW processor function table and static definitions
 */
struct pvr_fw_defs {
	/**
	 * @init:
	 *
	 * FW processor specific initialisation.
	 * @pvr_dev: Target PowerVR device.
	 *
	 * This function must call pvr_fw_heap_calculate() to initialise the firmware heap for this
	 * FW processor.
	 *
	 * This function is mandatory.
	 *
	 * Returns:
	 *  * 0 on success, or
	 *  * Any appropriate error on failure.
	 */
	int (*init)(struct pvr_device *pvr_dev);

	/**
	 * @fini:
	 *
	 * FW processor specific finalisation.
	 * @pvr_dev: Target PowerVR device.
	 *
	 * This function is mandatory.
	 */
	void (*fini)(struct pvr_device *pvr_dev);

	/**
	 * @fw_process:
	 *
	 * Load and process firmware image.
	 * @pvr_dev: Target PowerVR device.
	 * @firmware: Pointer to firmware image.
	 * @segments: Firmware segments, includes host pointers for populating
	 * and firmware objects for reference.
	 *
	 * This function is mandatory.
	 *
	 * Returns:
	 *  * 0 on success, or
	 *  * Any appropriate error on failure.
	 */
	int (*fw_process)(struct pvr_device *pvr_dev, const struct firmware *firmware,
			  const struct pvr_fw_segment segments[static PVR_FW_SEGMENT_COUNT]);

	/**
	 * @vm_map:
	 *
	 * Map FW object into FW processor address space.
	 * @pvr_dev: Target PowerVR device.
	 * @fw_obj: FW object to map.
	 *
	 * This function is mandatory.
	 *
	 * Returns:
	 *  * 0 on success, or
	 *  * Any appropriate error on failure.
	 */
	int (*vm_map)(struct pvr_device *pvr_dev, struct pvr_fw_object *fw_obj);

	/**
	 * @vm_unmap:
	 *
	 * Unmap FW object from FW processor address space.
	 * @pvr_dev: Target PowerVR device.
	 * @fw_obj: FW object to map.
	 *
	 * This function is mandatory.
	 */
	void (*vm_unmap)(struct pvr_device *pvr_dev, struct pvr_fw_object *fw_obj);

	/**
	 * @get_fw_addr_with_offset:
	 *
	 * Called to get address of object in firmware address space, with offset.
	 * @fw_obj: Pointer to object.
	 * @offset: Desired offset from start of object.
	 *
	 * This function is mandatory.
	 *
	 * Returns:
	 *  * Address in firmware address space.
	 */
	u32 (*get_fw_addr_with_offset)(struct pvr_fw_object *fw_obj, u32 offset);

	/**
	 * @wrapper_init:
	 *
	 * Called to initialise FW wrapper.
	 * @pvr_dev: Target PowerVR device.
	 *
	 * This function is mandatory.
	 *
	 * Returns:
	 *  * 0 on success.
	 *  * Any appropriate error on failure.
	 */
	int (*wrapper_init)(struct pvr_device *pvr_dev);

	/**
	 * @irq_pending: Check interrupt status register for pending interrupts.
	 *
	 * @pvr_dev: Target PowerVR device.
	 *
	 * This function is mandatory.
	 */
	bool (*irq_pending)(struct pvr_device *pvr_dev);

	/**
	 * @irq_clear: Clear pending interrupts.
	 *
	 * @pvr_dev: Target PowerVR device.
	 *
	 * This function is mandatory.
	 */
	void (*irq_clear)(struct pvr_device *pvr_dev);

	/**
	 * @has_fixed_data_addr: Specify whether the firmware fixed data must be loaded at the
	 *                       address given by the firmware layout table.
	 *
	 * This value is mandatory.
	 */
	bool has_fixed_data_addr;
};

/**
 * struct pvr_fw_mem - FW memory allocations
 */
struct pvr_fw_mem {
	/** @segments: Firmware segments loaded from file. */
	struct pvr_fw_segment segments[PVR_FW_SEGMENT_COUNT];

	/**
	 * @fwif_connection_ctl_obj: Object representing FWIF connection control
	 *                           structure.
	 */
	struct pvr_fw_object *fwif_connection_ctl_obj;

	/** @osinit_obj: Object representing FW OSINIT structure. */
	struct pvr_fw_object *osinit_obj;

	/** @sysinit_obj: Object representing FW SYSINIT structure. */
	struct pvr_fw_object *sysinit_obj;

	/** @osdata_obj: Object representing FW OSDATA structure. */
	struct pvr_fw_object *osdata_obj;

	/** @hwrinfobuf_obj: Object representing FW hwrinfobuf structure. */
	struct pvr_fw_object *hwrinfobuf_obj;

	/** @sysdata_obj: Object representing FW SYSDATA structure. */
	struct pvr_fw_object *sysdata_obj;

	/** @power_sync_obj: Object representing power sync state. */
	struct pvr_fw_object *power_sync_obj;

	/** @fault_page_obj: Object representing FW fault page. */
	struct pvr_fw_object *fault_page_obj;

	/** @gpu_util_fwcb_obj: Object representing FW GPU utilisation control structure. */
	struct pvr_fw_object *gpu_util_fwcb_obj;

	/** @runtime_cfg_obj: Object representing FW runtime config structure. */
	struct pvr_fw_object *runtime_cfg_obj;

	/** @mmucache_sync_obj: Object used as the sync parameter in an MMU cache operation. */
	struct pvr_fw_object *mmucache_sync_obj;
};

struct pvr_fw_device {
	/** @metadata: Metadata loaded from firmware headers. */
	const struct pvr_fw_metadata *metadata;

	/** @mem: Structure containing objects representing firmware memory allocations. */
	struct pvr_fw_mem mem;

	/**
	 * @initialised: %true if the firmware has been successfully initialised,
	 *               %false otherwise.
	 */
	bool initialised;

	/**
	 * @processor_type: FW processor type for this device. Must be one of
	 *                  %PVR_FW_PROCESSOR_TYPE_*.
	 */
	u16 processor_type;

	/** @funcs: Function table for the FW processor used by this device. */
	const struct pvr_fw_defs *defs;

	/** @processor_data: Pointer to data specific to FW processor. */
	union {
		/** @mips_data: Pointer to MIPS-specific data. */
		struct pvr_fw_mips_data *mips_data;
	} processor_data;

	/** @fw_heap_info: Firmware heap information. */
	struct {
		/** @gpu_addr: Base address of firmware heap in GPU address space. */
		u64 gpu_addr;

		/** @size: Size of main area of heap. */
		u32 size;

		/** @offset_mask: Mask for offsets within FW heap. */
		u32 offset_mask;

		/** @raw_size: Raw size of heap, including reserved areas. */
		u32 raw_size;

		/** @log2_size: Log2 of raw size of heap. */
		u32 log2_size;

		/** @config_offset: Offset of config area within heap. */
		u32 config_offset;

		/** @reserved_size: Size of reserved area in heap. */
		u32 reserved_size;
	} fw_heap_info;

	/** @fw_mm: Firmware address space allocator. */
	struct drm_mm fw_mm;

	/** @fw_mm_lock: Lock protecting access to &fw_mm. */
	spinlock_t fw_mm_lock;

	/** @fw_mm_base: Base address of address space managed by @fw_mm. */
	u64 fw_mm_base;

	/** @fwif_sysinit: Pointer to CPU mapping of FW SYSINIT structure. */
	struct rogue_fwif_sysinit *fwif_sysinit;

	/** @fwif_sysdata: Pointer to CPU mapping of FW SYSDATA structure. */
	struct rogue_fwif_sysdata *fwif_sysdata;

	/** @fwif_osdata: Pointer to CPU mapping of FW OSDATA structure. */
	struct rogue_fwif_osdata *fwif_osdata;

	/** @power_sync: Pointer to CPU mapping of power sync state. */
	u32 *power_sync;

	/** @fw_trace: Device firmware trace buffer state. */
	struct pvr_fw_trace fw_trace;

	/** @fw_objs: Structure tracking FW objects. */
	struct {
		/** @list: Head of FW object list. */
		struct list_head list;

		/** @lock: Lock protecting access to FW object list. */
		struct mutex lock;
	} fw_objs;
};

enum pvr_fw_processor_type {
	PVR_FW_PROCESSOR_TYPE_META = 0,
	PVR_FW_PROCESSOR_TYPE_MIPS,
	PVR_FW_PROCESSOR_TYPE_RISCV,
	PVR_FW_PROCESSOR_TYPE_COUNT,
};

extern const struct pvr_fw_defs pvr_fw_defs_meta;
extern const struct pvr_fw_defs pvr_fw_defs_mips;
extern const struct pvr_fw_defs pvr_fw_defs_riscv;

int pvr_fw_init(struct pvr_device *pvr_dev, const struct firmware *firmware);
void pvr_fw_fini(struct pvr_device *pvr_dev);

int pvr_wait_for_fw_boot(struct pvr_device *pvr_dev);

int
pvr_fw_hard_reset(struct pvr_device *pvr_dev);

void pvr_fw_mts_schedule(struct pvr_device *pvr_dev, u32 val);

void
pvr_fw_heap_info_init(struct pvr_device *pvr_dev, u32 log2_size, u32 reserved_size);

const struct pvr_fw_section_desc *
pvr_fw_get_section_desc(struct pvr_device *pvr_dev, enum pvr_fw_section_name section);
int
pvr_fw_find_mmu_segment(struct pvr_device *pvr_dev, u32 addr, u32 size,
			const struct pvr_fw_segment segments[static PVR_FW_SEGMENT_COUNT],
			void **host_addr_out);

int
pvr_fw_structure_cleanup(struct pvr_device *pvr_dev, u32 type, struct pvr_fw_object *fw_obj,
			 u32 offset);

int pvr_fw_object_create(struct pvr_device *pvr_dev, size_t size, u64 flags,
			 void (*init)(void *cpu_ptr, void *priv), void *init_priv,
			 struct pvr_fw_object **pvr_obj_out);

void *pvr_fw_object_create_and_map(struct pvr_device *pvr_dev, size_t size, u64 flags,
				   void (*init)(void *cpu_ptr, void *priv),
				   void *init_priv, struct pvr_fw_object **pvr_obj_out);

int
pvr_fw_object_create_offset(struct pvr_device *pvr_dev, u32 fw_addr,
			    size_t size, u64 flags,
			    void (*init)(void *cpu_ptr, void *priv),
			    void *init_priv, struct pvr_fw_object **pvr_obj_out);

void *
pvr_fw_object_create_and_map_offset(struct pvr_device *pvr_dev, u32 dev_offset, size_t size,
				    u64 flags, void (*init)(void *cpu_ptr, void *priv),
				    void *init_priv, struct pvr_fw_object **pvr_obj_out);

static __always_inline void *
pvr_fw_object_vmap(struct pvr_fw_object *fw_obj)
{
	return pvr_gem_object_vmap(fw_obj->gem);
}

static __always_inline void
pvr_fw_object_vunmap(struct pvr_fw_object *fw_obj)
{
	pvr_gem_object_vunmap(fw_obj->gem);
}

void pvr_fw_object_destroy(struct pvr_fw_object *fw_obj);

static __always_inline void
pvr_fw_object_unmap_and_destroy(struct pvr_fw_object *fw_obj)
{
	pvr_fw_object_vunmap(fw_obj);
	pvr_fw_object_destroy(fw_obj);
}

/**
 * pvr_fw_object_get_dma_addr() - Get DMA address for given offset in firmware
 * object.
 * @fw_obj: Pointer to object to lookup address in.
 * @offset: Offset within object to lookup address at.
 * @dma_addr_out: Pointer to location to store DMA address.
 *
 * Returns:
 *  * 0 on success, or
 *  * -%EINVAL if object is not currently backed, or if @offset is out of valid
 *    range for this object.
 */
static __always_inline int
pvr_fw_object_get_dma_addr(struct pvr_fw_object *fw_obj, u32 offset, dma_addr_t *dma_addr_out)
{
	return pvr_gem_get_dma_addr(fw_obj->gem, offset, dma_addr_out);
}

void pvr_fw_object_get_fw_addr_offset(struct pvr_fw_object *fw_obj, u32 offset, u32 *fw_addr_out);

static __always_inline void
pvr_fw_object_get_fw_addr(struct pvr_fw_object *fw_obj, u32 *fw_addr_out)
{
	pvr_fw_object_get_fw_addr_offset(fw_obj, 0, fw_addr_out);
}

u64
pvr_fw_obj_get_gpu_addr(struct pvr_fw_object *fw_obj);

static __always_inline size_t
pvr_fw_obj_get_object_size(struct pvr_fw_object *fw_obj)
{
	return pvr_gem_object_size(fw_obj->gem);
}

/*
 * Declared in pvr_fw_info.c, these functions represent the entire bridge
 * between the types defined above (and in pvr_device_info.h) and the ABI types
 * defined in pvr_fw_info.h.
 */
int pvr_fw_load_metadata_and_device_info(struct pvr_device *pvr_dev,
					 const struct firmware *firmware);
void pvr_fw_free_metadata_and_device_info(struct pvr_device *pvr_dev);

/* Util functions defined in pvr_fw_util.c. These are intended for use in pvr_fw_<arch>.c files. */
int pvr_fw_process_elf_command_stream(
	struct pvr_device *pvr_dev, const u8 *fw,
	const struct pvr_fw_segment segments[static PVR_FW_SEGMENT_COUNT]);

int
pvr_fw_info_get(const struct pvr_device *pvr_dev, struct drm_pvr_ioctl_dev_query_args *args);

#endif /* PVR_FW_H */
