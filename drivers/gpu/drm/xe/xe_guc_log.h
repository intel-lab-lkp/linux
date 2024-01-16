/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GUC_LOG_H_
#define _XE_GUC_LOG_H_

#include "xe_guc_log_types.h"
#include "xe_guc_types.h"

struct drm_printer;

#if IS_ENABLED(CONFIG_DRM_XE_LARGE_GUC_BUFFER)
#define CRASH_BUFFER_SIZE       SZ_1M
#define DEBUG_BUFFER_SIZE       SZ_8M
#define CAPTURE_BUFFER_SIZE     SZ_2M
#else
#define CRASH_BUFFER_SIZE	SZ_8K
#define DEBUG_BUFFER_SIZE	SZ_64K
#define CAPTURE_BUFFER_SIZE	SZ_16K
#endif
/*
 * While we're using plain log level in i915, GuC controls are much more...
 * "elaborate"? We have a couple of bits for verbosity, separate bit for actual
 * log enabling, and separate bit for default logging - which "conveniently"
 * ignores the enable bit.
 */
#define GUC_LOG_LEVEL_DISABLED		0
#define GUC_LOG_LEVEL_NON_VERBOSE	1
#define GUC_LOG_LEVEL_IS_ENABLED(x)	((x) > GUC_LOG_LEVEL_DISABLED)
#define GUC_LOG_LEVEL_IS_VERBOSE(x)	((x) > GUC_LOG_LEVEL_NON_VERBOSE)
#define GUC_LOG_LEVEL_TO_VERBOSITY(x) ({		\
	typeof(x) _x = (x);				\
	GUC_LOG_LEVEL_IS_VERBOSE(_x) ? _x - 2 : 0;	\
})
#define GUC_VERBOSITY_TO_LOG_LEVEL(x)	((x) + 2)
#define GUC_LOG_LEVEL_MAX GUC_VERBOSITY_TO_LOG_LEVEL(GUC_LOG_VERBOSITY_MAX)

static inline struct xe_guc *log_to_guc(struct xe_guc_log *log)
{
	return container_of(log, struct xe_guc, log);
}

int xe_guc_log_init(struct xe_guc_log *log);
void xe_guc_log_print(struct xe_guc_log *log, struct drm_printer *p);

static inline u32
xe_guc_log_get_level(struct xe_guc_log *log)
{
	return log->level;
}

u32 xe_guc_log_section_size_capture(struct xe_guc_log *log);

bool xe_guc_check_log_buf_overflow(struct xe_guc_log *log,
				   enum guc_log_buffer_type type,
				   unsigned int full_cnt);
unsigned int xe_guc_get_log_buffer_size(struct xe_guc_log *log,
					enum guc_log_buffer_type type);
size_t xe_guc_get_log_buffer_offset(struct xe_guc_log *log,
				    enum guc_log_buffer_type type);
#endif
