// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright 2023 Collabora Ltd */
/* Copyright 2024 Arm ltd. */

#include <drm/drm_file.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/panthor_drm.h>

#include "panthor_device.h"
#include "panthor_fw.h"
#include "panthor_gpu.h"
#include "panthor_perf.h"
#include "panthor_regs.h"

/**
 * PANTHOR_PERF_COUNTERS_PER_BLOCK - On CSF architectures pre-11.x, the number of counters
 * per block was hardcoded to be 64. Arch 11.0 onwards supports the PRFCNT_FEATURES GPU register,
 * which indicates the same information.
 */
#define PANTHOR_PERF_COUNTERS_PER_BLOCK (64)

void panthor_perf_info_init(struct panthor_device *ptdev)
{
	struct panthor_fw_global_iface *glb_iface = panthor_fw_get_glb_iface(ptdev);
	struct drm_panthor_perf_info *const perf_info = &ptdev->perf_info;

	if (PERFCNT_FEATURES_MD_SIZE(glb_iface->control->perfcnt_features))
		perf_info->flags |= DRM_PANTHOR_PERF_BLOCK_STATES_SUPPORT;

	if (GPU_ARCH_MAJOR(ptdev->gpu_info.gpu_id) < 11)
		perf_info->counters_per_block = PANTHOR_PERF_COUNTERS_PER_BLOCK;

	perf_info->sample_header_size = sizeof(struct drm_panthor_perf_sample_header);
	perf_info->block_header_size = sizeof(struct drm_panthor_perf_block_header);

	if (GLB_PERFCNT_FW_SIZE(glb_iface->control->perfcnt_size)) {
		perf_info->fw_blocks = 1;
		perf_info->csg_blocks = glb_iface->control->group_num;
	}

	perf_info->cshw_blocks = 1;
	perf_info->tiler_blocks = 1;
	perf_info->memsys_blocks = hweight64(ptdev->gpu_info.l2_present);
	perf_info->shader_blocks = hweight64(ptdev->gpu_info.shader_present);
}

