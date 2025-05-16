// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright 2023 Collabora Ltd */
/* Copyright 2025 Arm ltd. */

#include <linux/bitops.h>
#include <drm/panthor_drm.h>

#include "panthor_device.h"
#include "panthor_fw.h"
#include "panthor_perf.h"

struct panthor_perf_counter_block {
	struct drm_panthor_perf_block_header header;
	u64 counters[];
};

static size_t get_annotated_block_size(size_t counters_per_block)
{
	return struct_size_t(struct panthor_perf_counter_block, counters, counters_per_block);
}

static size_t session_get_user_sample_size(const struct drm_panthor_perf_info *const info)
{
	const size_t block_size = get_annotated_block_size(info->counters_per_block);
	const size_t block_nr = info->cshw_blocks + info->fw_blocks +
		info->tiler_blocks + info->memsys_blocks + info->shader_blocks;

	return sizeof(struct drm_panthor_perf_sample_header) + (block_size * block_nr);
}

/**
 * PANTHOR_PERF_COUNTERS_PER_BLOCK - On CSF architectures pre-11.x, the number of counters
 * per block was hardcoded to be 64. Arch 11.0 onwards supports the PRFCNT_FEATURES GPU register,
 * which indicates the same information.
 */
#define PANTHOR_PERF_COUNTERS_PER_BLOCK (64)

static void panthor_perf_info_init(struct panthor_device *ptdev)
{
	struct panthor_fw_global_iface *glb_iface = panthor_fw_get_glb_iface(ptdev);
	struct drm_panthor_perf_info *const perf_info = &ptdev->perf_info;

	if (PERFCNT_FEATURES_MD_SIZE(glb_iface->control->perfcnt_features))
		perf_info->flags |= DRM_PANTHOR_PERF_BLOCK_STATES_SUPPORT;

	perf_info->counters_per_block = PANTHOR_PERF_COUNTERS_PER_BLOCK;

	perf_info->sample_header_size = sizeof(struct drm_panthor_perf_sample_header);
	perf_info->block_header_size = sizeof(struct drm_panthor_perf_block_header);

	if (GLB_PERFCNT_FW_SIZE(glb_iface->control->perfcnt_size))
		perf_info->fw_blocks = 1;

	perf_info->cshw_blocks = 1;
	perf_info->tiler_blocks = 1;
	perf_info->memsys_blocks = GPU_MEM_FEATURES_L2_SLICES(ptdev->gpu_info.mem_features);
	perf_info->shader_blocks = hweight64(ptdev->gpu_info.shader_present);

	perf_info->sample_size = session_get_user_sample_size(perf_info);
}

/**
 * panthor_perf_init - Initialize the performance counter subsystem.
 * @ptdev: Panthor device
 *
 * Return: 0 on success, negative error code on failure.
 */
int panthor_perf_init(struct panthor_device *ptdev)
{
	if (!ptdev)
		return -EINVAL;

	panthor_perf_info_init(ptdev);

	return 0;
}
