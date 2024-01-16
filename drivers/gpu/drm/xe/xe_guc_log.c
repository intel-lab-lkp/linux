// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_guc_log.h"

#include <drm/drm_managed.h>

#include "xe_bo.h"
#include "xe_gt.h"
#include "xe_gt_printk.h"
#include "xe_map.h"
#include "xe_module.h"

#define MISSING_CASE(x) WARN(1, "Missing case (%s == %ld)\n", \
			     __stringify(x), (long)(x))

#define GUC_LOG_DEFAULT_CRASH_BUFFER_SIZE	CRASH_BUFFER_SIZE
#define GUC_LOG_DEFAULT_DEBUG_BUFFER_SIZE	DEBUG_BUFFER_SIZE
#define GUC_LOG_DEFAULT_CAPTURE_BUFFER_SIZE	CAPTURE_BUFFER_SIZE

struct guc_log_section {
	u32 max;
	u32 flag;
	u32 default_val;
	const char *name;
};

static struct xe_gt *
guc_to_gt(struct xe_guc *guc)
{
	return container_of(guc, struct xe_gt, uc.guc);
}

static struct xe_gt *
log_to_gt(struct xe_guc_log *log)
{
	return container_of(log, struct xe_gt, uc.guc.log);
}

static struct xe_device *
log_to_xe(struct xe_guc_log *log)
{
	return gt_to_xe(log_to_gt(log));
}

static size_t guc_log_size(void)
{
	/*
	 *  GuC Log buffer Layout
	 *
	 *  +===============================+ 00B
	 *  |    Crash dump state header    |
	 *  +-------------------------------+ 32B
	 *  |      Debug state header       |
	 *  +-------------------------------+ 64B
	 *  |     Capture state header      |
	 *  +-------------------------------+ 96B
	 *  |                               |
	 *  +===============================+ PAGE_SIZE (4KB)
	 *  |        Crash Dump logs        |
	 *  +===============================+ + CRASH_SIZE
	 *  |          Debug logs           |
	 *  +===============================+ + DEBUG_SIZE
	 *  |         Capture logs          |
	 *  +===============================+ + CAPTURE_SIZE
	 */
	return PAGE_SIZE + CRASH_BUFFER_SIZE + DEBUG_BUFFER_SIZE +
		CAPTURE_BUFFER_SIZE;
}

void xe_guc_log_print(struct xe_guc_log *log, struct drm_printer *p)
{
	struct xe_device *xe = log_to_xe(log);
	size_t size;
	int i, j;

	xe_assert(xe, log->bo);

	size = log->bo->size;

#define DW_PER_READ		128
	xe_assert(xe, !(size % (DW_PER_READ * sizeof(u32))));
	for (i = 0; i < size / sizeof(u32); i += DW_PER_READ) {
		u32 read[DW_PER_READ];

		xe_map_memcpy_from(xe, read, &log->bo->vmap, i * sizeof(u32),
				   DW_PER_READ * sizeof(u32));
#define DW_PER_PRINT		4
		for (j = 0; j < DW_PER_READ / DW_PER_PRINT; ++j) {
			u32 *print = read + j * DW_PER_PRINT;

			drm_printf(p, "0x%08x 0x%08x 0x%08x 0x%08x\n",
				   *(print + 0), *(print + 1),
				   *(print + 2), *(print + 3));
		}
	}
}

int xe_guc_log_init(struct xe_guc_log *log)
{
	struct xe_device *xe = log_to_xe(log);
	struct xe_tile *tile = gt_to_tile(log_to_gt(log));
	struct xe_bo *bo;

	bo = xe_managed_bo_create_pin_map(xe, tile, guc_log_size(),
					  XE_BO_CREATE_VRAM_IF_DGFX(tile) |
					  XE_BO_CREATE_GGTT_BIT);
	if (IS_ERR(bo))
		return PTR_ERR(bo);

	xe_map_memset(xe, &bo->vmap, 0, 0, guc_log_size());
	log->bo = bo;
	log->level = xe_modparam.guc_log_level;

	return 0;
}

static void _guc_log_init_sizes(struct xe_guc_log *log)
{
	struct xe_guc *guc = log_to_guc(log);
	static const struct guc_log_section sections[GUC_LOG_SECTIONS_LIMIT] = {
		{
			GUC_LOG_CRASH_MASK >> GUC_LOG_CRASH_SHIFT,
			GUC_LOG_LOG_ALLOC_UNITS,
			GUC_LOG_DEFAULT_CRASH_BUFFER_SIZE,
			"crash dump"
		},
		{
			GUC_LOG_DEBUG_MASK >> GUC_LOG_DEBUG_SHIFT,
			GUC_LOG_LOG_ALLOC_UNITS,
			GUC_LOG_DEFAULT_DEBUG_BUFFER_SIZE,
			"debug",
		},
		{
			GUC_LOG_CAPTURE_MASK >> GUC_LOG_CAPTURE_SHIFT,
			GUC_LOG_CAPTURE_ALLOC_UNITS,
			GUC_LOG_DEFAULT_CAPTURE_BUFFER_SIZE,
			"capture",
		}
	};
	int i;

	for (i = 0; i < GUC_LOG_SECTIONS_LIMIT; i++)
		log->sizes[i].bytes = sections[i].default_val;

	/* If debug size > 1MB then bump default crash size to keep the same units */
	if (log->sizes[GUC_LOG_SECTIONS_DEBUG].bytes >= SZ_1M &&
	    GUC_LOG_DEFAULT_CRASH_BUFFER_SIZE < SZ_1M)
		log->sizes[GUC_LOG_SECTIONS_CRASH].bytes = SZ_1M;

	/* Prepare the GuC API structure fields: */
	for (i = 0; i < GUC_LOG_SECTIONS_LIMIT; i++) {
		/* Convert to correct units */
		if ((log->sizes[i].bytes % SZ_1M) == 0) {
			log->sizes[i].units = SZ_1M;
			log->sizes[i].flag = sections[i].flag;
		} else {
			log->sizes[i].units = SZ_4K;
			log->sizes[i].flag = 0;
		}

		if (!IS_ALIGNED(log->sizes[i].bytes, log->sizes[i].units))
			xe_gt_err(guc_to_gt(guc), "Mis-aligned log %s size: 0x%X vs 0x%X!\n",
				  sections[i].name, log->sizes[i].bytes, log->sizes[i].units);
		log->sizes[i].count = log->sizes[i].bytes / log->sizes[i].units;

		if (!log->sizes[i].count) {
			xe_gt_err(guc_to_gt(guc), "Zero log %s size!\n", sections[i].name);
		} else {
			/* Size is +1 unit */
			log->sizes[i].count--;
		}

		/* Clip to field size */
		if (log->sizes[i].count > sections[i].max) {
			xe_gt_err(guc_to_gt(guc), "log %s size too large: %d vs %d!\n",
				  sections[i].name, log->sizes[i].count + 1, sections[i].max + 1);
			log->sizes[i].count = sections[i].max;
		}
	}

	if (log->sizes[GUC_LOG_SECTIONS_CRASH].units != log->sizes[GUC_LOG_SECTIONS_DEBUG].units) {
		xe_gt_err(guc_to_gt(guc), "Unit mismatch for crash and debug sections: %d vs %d!\n",
			  log->sizes[GUC_LOG_SECTIONS_CRASH].units,
			  log->sizes[GUC_LOG_SECTIONS_DEBUG].units);
		log->sizes[GUC_LOG_SECTIONS_CRASH].units = log->sizes[GUC_LOG_SECTIONS_DEBUG].units;
		log->sizes[GUC_LOG_SECTIONS_CRASH].count = 0;
	}

	log->sizes_initialised = true;
}

static void guc_log_init_sizes(struct xe_guc_log *log)
{
	if (log->sizes_initialised)
		return;

	_guc_log_init_sizes(log);
}

static u32 xe_guc_log_section_size_crash(struct xe_guc_log *log)
{
	guc_log_init_sizes(log);

	return log->sizes[GUC_LOG_SECTIONS_CRASH].bytes;
}

static u32 xe_guc_log_section_size_debug(struct xe_guc_log *log)
{
	guc_log_init_sizes(log);

	return log->sizes[GUC_LOG_SECTIONS_DEBUG].bytes;
}

u32 xe_guc_log_section_size_capture(struct xe_guc_log *log)
{
	guc_log_init_sizes(log);

	return log->sizes[GUC_LOG_SECTIONS_CAPTURE].bytes;
}

bool xe_guc_check_log_buf_overflow(struct xe_guc_log *log, enum guc_log_buffer_type type,
				   unsigned int full_cnt)
{
	unsigned int prev_full_cnt = log->stats[type].sampled_overflow;
	bool overflow = false;

	if (full_cnt != prev_full_cnt) {
		overflow = true;

		log->stats[type].overflow = full_cnt;
		log->stats[type].sampled_overflow += full_cnt - prev_full_cnt;

		if (full_cnt < prev_full_cnt) {
			/* buffer_full_cnt is a 4 bit counter */
			log->stats[type].sampled_overflow += 16;
		}
		xe_gt_notice_ratelimited(log_to_gt(log), "log buffer overflow\n");
	}

	return overflow;
}

unsigned int xe_guc_get_log_buffer_size(struct xe_guc_log *log,
					enum guc_log_buffer_type type)
{
	switch (type) {
	case GUC_DEBUG_LOG_BUFFER:
		return xe_guc_log_section_size_debug(log);
	case GUC_CRASH_DUMP_LOG_BUFFER:
		return xe_guc_log_section_size_crash(log);
	case GUC_CAPTURE_LOG_BUFFER:
		return xe_guc_log_section_size_capture(log);
	default:
		MISSING_CASE(type);
	}

	return 0;
}

size_t xe_guc_get_log_buffer_offset(struct xe_guc_log *log,
				    enum guc_log_buffer_type type)
{
	enum guc_log_buffer_type i;
	size_t offset = PAGE_SIZE;/* for the log_buffer_states */

	for (i = GUC_DEBUG_LOG_BUFFER; i < GUC_MAX_LOG_BUFFER; ++i) {
		if (i == type)
			break;
		offset += xe_guc_get_log_buffer_size(log, i);
	}

	return offset;
}
