// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright 2023 Collabora Ltd */
/* Copyright 2025 Arm ltd. */

#include <linux/bitops.h>
#include <drm/drm_managed.h>
#include <drm/panthor_drm.h>
#include <drm/drm_print.h>

#include "panthor_device.h"
#include "panthor_fw.h"
#include "panthor_gpu.h"
#include "panthor_gpu_regs.h"
#include "panthor_perf.h"

/**
 * PANTHOR_PERF_COUNTERS_PER_BLOCK - On CSF architectures pre-11.x, the number of counters
 * per block was hardcoded to be 64. Arch 11.0 onwards supports the PRFCNT_FEATURES GPU register,
 * which indicates the same information.
 */
#define PANTHOR_PERF_COUNTERS_PER_BLOCK (64)

/**
 * PANTHOR_HW_COUNTER_SIZE - The size of a hardware counter in the FW ring buffer.
 */
#define PANTHOR_HW_COUNTER_SIZE (sizeof(u32))

#define MAX_SESSIONS_PER_FILE 1

struct panthor_perf_counter_block {
	struct drm_panthor_perf_block_header header;
	u64 counters[];
};

struct panthor_perf {
	/** @info: Pointer to the previously initialized perf info. */
	struct drm_panthor_perf_info *info;
};

struct panthor_perf_pool {
	/** @session_range: The number of sessions supported at a time. */
	struct xa_limit session_range;

	/** @xa: Xarray used to manage session handles. */
	struct xarray xa;
};

static size_t get_annotated_block_size(size_t counters_per_block)
{
	return struct_size_t(struct panthor_perf_counter_block, counters, counters_per_block);
}

static size_t session_get_sample_size(const struct drm_panthor_perf_info *const info)
{
	const size_t block_size = get_annotated_block_size(info->counters_per_block);
	const size_t block_nr = info->cshw_blocks + info->fw_blocks +
		info->tiler_blocks + info->memsys_blocks + info->shader_blocks;

	return info->sample_header_size + (block_size * block_nr);
}

static void panthor_perf_info_init(struct panthor_device *const ptdev)
{
	struct panthor_fw_global_iface *glb_iface = panthor_fw_get_glb_iface(ptdev);
	struct drm_panthor_perf_info *const perf_info = &ptdev->perf_info;

	if (GPU_ARCH_MAJOR(ptdev->gpu_info.gpu_id) < 11) {
		perf_info->counters_per_block = PANTHOR_PERF_COUNTERS_PER_BLOCK;
	} else {
		u32 features = panthor_gpu_get_prfcnt_features(ptdev);

		perf_info->counters_per_block = PRFCNT_FEATURES_COUNTER_BLOCK_SIZE(features) /
			PANTHOR_HW_COUNTER_SIZE;
	}

	perf_info->sample_header_size = sizeof(struct drm_panthor_perf_sample_header);
	perf_info->block_header_size = sizeof(struct drm_panthor_perf_block_header);

	if (GLB_PERFCNT_FW_SIZE(glb_iface->control->perfcnt_size))
		perf_info->fw_blocks = 1;

	perf_info->cshw_blocks = 1;
	perf_info->tiler_blocks = 1;
	perf_info->memsys_blocks = GPU_MEM_FEATURES_L2_SLICES(ptdev->gpu_info.mem_features);
	perf_info->shader_blocks = hweight64(ptdev->gpu_info.shader_present);

	perf_info->sample_size = session_get_sample_size(perf_info);
}

/**
 * panthor_perf_init - Initialize the performance counter subsystem.
 * @ptdev: Panthor device
 *
 * The performance counters require the FW interface to be available to setup the
 * sampling ringbuffers, so this must be called only after FW is initialized.
 *
 * Return: 0 on success, negative error code on failure.
 */
int panthor_perf_init(struct panthor_device *ptdev)
{
	struct panthor_perf *perf;
	int ret = 0;

	if (!ptdev)
		return -EINVAL;

	panthor_perf_info_init(ptdev);

	perf = drmm_kzalloc(&ptdev->base, sizeof(*perf), GFP_KERNEL);
	if (!perf)
		return -ENOMEM;

	drm_info(&ptdev->base, "Performance counter subsystem initialized");

	perf->info = &ptdev->perf_info;
	ptdev->perf = perf;

	return ret;
}

/**
 * panthor_perf_unplug - Terminate the performance counter subsystem.
 * @ptdev: Panthor device.
 *
 * Release the performance counter hardware resources. The control structure is
 * released with the DRM device.
 */
void panthor_perf_unplug(struct panthor_device *ptdev)
{
	struct panthor_perf *perf = ptdev->perf;

	if (!perf)
		return;

	ptdev->perf = NULL;
}

int panthor_perf_pool_create(struct panthor_file *pfile)
{
	struct panthor_perf_pool *pool;

	pool = kzalloc_obj(*pool);
	if (!pool)
		return -ENOMEM;

	xa_init_flags(&pool->xa, XA_FLAGS_ALLOC);

	pool->session_range.min = 0;
	pool->session_range.max = MAX_SESSIONS_PER_FILE - 1;

	pfile->sessions = pool;

	return 0;
}

void panthor_perf_pool_destroy(struct panthor_file *pfile)
{
	struct panthor_perf_pool *pool = pfile->sessions;

	if (IS_ERR_OR_NULL(pool))
		return;

	if (!xa_empty(&pool->xa)) {
		drm_err(&pfile->ptdev->base,
			"Performance counter sessions active when unplugging the driver!");
	}

	xa_destroy(&pool->xa);
	kfree(pool);

	pfile->sessions = NULL;
}
