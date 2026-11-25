// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright (c) 2026 Imagination Technologies Ltd. */

#include "pvr_device.h"
#include "pvr_device_info.h"
#include "pvr_fw.h"
#include "pvr_fw_info.h"

#include <drm/drm_print.h>

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/types.h>

/* Forward declaration from <drm/drm_device.h> */
struct drm_device;

static enum pvr_fw_section_name
pvr_fw_section_for_section_id(enum pvr_fw_section_id section_id)
{
	switch (section_id) {
	case META_CODE:
	case MIPS_CODE:
		return PVR_FW_SECTION_CODE;
	case META_PRIVATE_DATA:
	case MIPS_PRIVATE_DATA:
	case RISCV_PRIVATE_DATA:
		return PVR_FW_SECTION_PRIVATE_DATA;
	case META_COREMEM_CODE:
	case RISCV_COREMEM_CODE:
		return PVR_FW_SECTION_COREMEM_CODE;
	case META_COREMEM_DATA:
	case RISCV_COREMEM_DATA:
		return PVR_FW_SECTION_COREMEM_DATA;
	case MIPS_EXCEPTIONS_CODE:
		return PVR_FW_SECTION_EXCEPTIONS_CODE;
	case MIPS_BOOT_CODE:
		return PVR_FW_SECTION_BOOT_CODE;
	case MIPS_BOOT_DATA:
		return PVR_FW_SECTION_BOOT_DATA;
	case MIPS_STACK:
		return PVR_FW_SECTION_STACK;
	case RISCV_UNCACHED_CODE:
		return PVR_FW_SECTION_UNCACHED_CODE;
	case RISCV_CACHED_CODE:
		return PVR_FW_SECTION_CACHED_CODE;
	default:
		return PVR_FW_SECTION_INVALID;
	}
}

static enum pvr_fw_segment_name
pvr_fw_segment_for_section_type(enum pvr_fw_section_type section_type)
{
	switch (section_type) {
	case FW_CODE:
		return PVR_FW_SEGMENT_CODE;
	case FW_DATA:
		return PVR_FW_SEGMENT_DATA;
	case FW_COREMEM_CODE:
		return PVR_FW_SEGMENT_CORE_CODE;
	case FW_COREMEM_DATA:
		return PVR_FW_SEGMENT_CORE_DATA;
	default:
		return PVR_FW_SEGMENT_INVALID;
	}
}

static int
pvr_fw_section_desc_for_layout_entry(struct pvr_device *pvr_dev,
				     const struct pvr_fw_layout_entry *entry,
				     struct pvr_fw_section_desc *section_out)
{
	struct drm_device *drm_dev = from_pvr_device(pvr_dev);
	enum pvr_fw_segment_name segment;
	enum pvr_fw_section_name name;
	bool invalid = false;
	u32 end_addr;

	name = pvr_fw_section_for_section_id(entry->id);
	if (name == PVR_FW_SECTION_INVALID) {
		drm_err(drm_dev, "Firmware section with unknown id (%u)\n",
			entry->id);
		invalid = true;
	}

	segment = pvr_fw_segment_for_section_type(entry->type);
	if (segment == PVR_FW_SEGMENT_INVALID) {
		drm_err(drm_dev, "Firmware section with unknown type (%u)\n",
			entry->type);
		invalid = true;
	}

	if (entry->alloc_size == 0) {
		drm_err(drm_dev, "Firmware section is empty\n");
		invalid = true;
	}

	if (entry->max_size < entry->alloc_size) {
		drm_err(drm_dev, "Firmware section size mismatch (max=0x%x < alloc=0x%x)\n",
			entry->max_size, entry->alloc_size);
		invalid = true;
	}

	if (check_add_overflow(entry->base_addr, entry->alloc_size, &end_addr)) {
		drm_err(drm_dev, "Firmware section size overflow (end would be 0x%llx)\n",
			(u64)entry->base_addr + (u64)entry->alloc_size);
		invalid = true;
	}

	if (invalid)
		return -EINVAL;

	*section_out = (struct pvr_fw_section_desc){
		.name = name,
		.segment = segment,
		.base_addr = entry->base_addr,
		.size = entry->alloc_size,
		.alloc_offset = entry->alloc_offset,
	};

	return 0;
}

static bool
pvr_fw_load_version(struct pvr_device *pvr_dev,
		    const struct pvr_fw_info_header *header,
		    struct pvr_fw_version *version_out)
{
	struct drm_device *drm_dev = from_pvr_device(pvr_dev);
	bool invalid = false;

	struct pvr_fw_version version = {
		.major = header->fw_version_major,
		.minor = header->fw_version_minor,
		.build = header->fw_version_build,
		.open_source = header->flags & PVR_FW_FLAGS_OPEN_SOURCE,
	};

	if (!version.open_source) {
		drm_err(drm_dev, "Firmware is not open-source compatible (found version " PVR_FW_VERSION_FMT ")\n",
			PVR_FW_VERSION_FMT_ARGS(&version));
		invalid = true;
	}

	if (version.major == 0 ||
	    (version.major > PVR_FW_MAX_SUPPORTED_MAJOR_VERSION && !pvr_exp_fw_support) ||
	    (version.major != PVR_FW_MAX_SUPPORTED_MAJOR_VERSION && pvr_exp_fw_support)) {
		drm_err(drm_dev, "Firmware version is unsupported (known range is 1..%u inclusive, found "
				 PVR_FW_VERSION_FMT ")\n",
			PVR_FW_MAX_SUPPORTED_MAJOR_VERSION,
			PVR_FW_VERSION_FMT_ARGS(&version));
		invalid = true;
	}

	if (invalid)
		return false;

	*version_out = version;
	return true;
}

static int
pvr_fw_info_header_validate(struct pvr_device *pvr_dev,
			    const struct pvr_fw_info_header *header,
			    struct pvr_fw_version *fw_version_out)
{
	struct drm_device *drm_dev = from_pvr_device(pvr_dev);
	bool invalid = false;

	if (header->info_version != PVR_FW_INFO_VERSION) {
		drm_err(drm_dev,
			"Firmware version is unsupported (known version is %u, found %u)\n",
			PVR_FW_INFO_VERSION, header->info_version);
		return -EINVAL;
	}

	if (header->header_len != sizeof(struct pvr_fw_info_header)) {
		drm_err(drm_dev, "Firmware header size mismatch (found=%u, expected=%zu)\n)",
			header->header_len, sizeof(struct pvr_fw_info_header));
		return -EINVAL;
	}

	if (!header->layout_entry_num || !header->layout_entry_size) {
		drm_err(drm_dev, "Firmware layout table missing\n");
		invalid = true;
	} else if (header->layout_entry_size != sizeof(struct pvr_fw_layout_entry)) {
		drm_err(drm_dev,
			"Firmware layout table entry size mismatch (found=%u, expected=%zu)\n)",
			header->layout_entry_size, sizeof(struct pvr_fw_layout_entry));
		invalid = true;
	} else if (header->layout_entry_num > PVR_FW_INFO_MAX_NUM_ENTRIES) {
		drm_err(drm_dev, "Firmware layout table is too large (found=%u, max=%u)\n)",
			header->layout_entry_num, PVR_FW_INFO_MAX_NUM_ENTRIES);
		invalid = true;
	} else if (header->layout_entry_size * header->layout_entry_num >
		   FW_BLOCK_SIZE - header->header_len) {
		drm_err(drm_dev, "Firmware layout table is too large (computed=0x%x, max=0x%x)\n",
			header->layout_entry_size * header->layout_entry_num,
			FW_BLOCK_SIZE - header->header_len);
		invalid = true;
	}

	if (pvr_gpu_id_to_packed_bvnc(&pvr_dev->gpu_id) != header->bvnc) {
		drm_err(drm_dev,
			"Firmware built for incorrect GPU ID " PVR_GPU_ID_FORMAT
			" (expected " PVR_GPU_ID_FORMAT ")\n",
			(u16)(header->bvnc >> PVR_GPU_ID_ARG_B),
			(u16)(header->bvnc >> PVR_GPU_ID_ARG_V),
			(u16)(header->bvnc >> PVR_GPU_ID_ARG_N),
			(u16)(header->bvnc >> PVR_GPU_ID_ARG_C),
			pvr_dev->gpu_id.b,
			pvr_dev->gpu_id.v,
			pvr_dev->gpu_id.n,
			pvr_dev->gpu_id.c);
		invalid = true;
	}

	if (!pvr_fw_load_version(pvr_dev, header, fw_version_out))
		invalid = true;

	if (invalid)
		return -EINVAL;

	return 0;
}

static const struct pvr_fw_metadata *
pvr_fw_load_metadata(struct pvr_device *pvr_dev, const struct firmware *firmware,
		     ptrdiff_t *device_info_offset_out)
{
	struct drm_device *drm_dev = from_pvr_device(pvr_dev);
	const struct pvr_fw_layout_entry *layout_entries;
	const struct pvr_fw_info_header *header;
	struct pvr_fw_version fw_version;
	const void *fw = firmware->data;
	struct pvr_fw_metadata *meta;
	ptrdiff_t header_offset;
	int err;

	if (check_sub_overflow(firmware->size, FW_BLOCK_SIZE, &header_offset)) {
		drm_err(drm_dev, "Firmware is smaller than one block\n");
		return ERR_PTR(-EINVAL);
	}

	if (firmware->size % FW_BLOCK_SIZE) {
		drm_err(drm_dev, "Firmware size is not block-aligned\n");
		return ERR_PTR(-EINVAL);
	}

	header = fw + header_offset;

	err = pvr_fw_info_header_validate(pvr_dev, header, &fw_version);
	if (err)
		return ERR_PTR(err);

	layout_entries = fw + header_offset + header->header_len;

	meta = kzalloc_flex(*meta, sections, header->layout_entry_num);
	if (!meta)
		return ERR_PTR(-ENOMEM);

	meta->version = fw_version;
	meta->num_sections = header->layout_entry_num;

	for (int i = 0; i < header->layout_entry_num; i++) {
		err = pvr_fw_section_desc_for_layout_entry(pvr_dev,
							   &layout_entries[i],
							   &meta->sections[i]);
		if (err)
			goto err_free_meta;
	}

	*device_info_offset_out = header_offset - header->device_info_size;

	drm_info(drm_dev, "Loaded firmware " PVR_FW_VERSION_FMT "\n",
		 PVR_FW_VERSION_FMT_ARGS(&meta->version));

	return meta;

err_free_meta:
	kfree(meta);

	return ERR_PTR(err);
}

static int
pvr_fw_get_device_info(struct pvr_device *pvr_dev, const void *device_info_ptr)
{
	const struct pvr_fw_device_info_header *header = device_info_ptr;
	const u64 *dev_info = (u64 *)(header + 1);

	pvr_device_info_set_quirks(pvr_dev, dev_info, header->brn_mask_size);
	dev_info += header->brn_mask_size;

	pvr_device_info_set_enhancements(pvr_dev, dev_info, header->ern_mask_size);
	dev_info += header->ern_mask_size;

	return pvr_device_info_set_features(pvr_dev, dev_info, header->feature_mask_size,
					    header->feature_param_size);
}

/**
 * pvr_fw_load_metadata_and_device_info() - Load firmware metadata and
 * initialise device information
 * @pvr_dev: Target PowerVR device.
 * @firmware: Firmware to load from.
 *
 * This function must be called before querying device information.
 *
 * Returns:
 *  * 0 on success, or
 *  * -%EINVAL if firmware validation fails.
 */
int
pvr_fw_load_metadata_and_device_info(struct pvr_device *pvr_dev,
				     const struct firmware *firmware)
{
	const struct pvr_fw_metadata *metadata;
	ptrdiff_t device_info_offset = 0;

	metadata = pvr_fw_load_metadata(pvr_dev, firmware, &device_info_offset);
	if (IS_ERR(metadata))
		return PTR_ERR(metadata);

	pvr_dev->fw_dev.metadata = metadata;

	return pvr_fw_get_device_info(pvr_dev, firmware->data + device_info_offset);
}

void
pvr_fw_free_metadata_and_device_info(struct pvr_device *pvr_dev)
{
	kfree(pvr_dev->fw_dev.metadata);
	pvr_dev->fw_dev.metadata = NULL;
}
