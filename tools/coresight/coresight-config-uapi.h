/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 Linaro Limited, All rights reserved.
 * Author: Mike Leach <mike.leach@linaro.org>
 */

#ifndef _CORESIGHT_CORESIGHT_CONFIG_UAPI_H
#define _CORESIGHT_CORESIGHT_CONFIG_UAPI_H

#include <linux/types.h>
#include <asm-generic/errno-base.h>

#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "coresight-config-desc.h"

/*
 * Userspace versions of the configuration and feature descriptors.
 * Used in the tools/coresight programs.
 *
 * Compatible with structures in coresight-config.h for use in
 * coresight-config-file.c common reader source file.
 */

/**
 * Device feature descriptor - combination of registers and parameters to
 * program a device to implement a specific complex function.
 *
 * UAPI version - removed kernel constructs.
 *
 * @name:	 feature name.
 * @description: brief description of the feature.
 * @match_flags: matching information if loading into a device
 * @nr_params:   number of parameters used.
 * @params_desc: array of parameters used.
 * @nr_regs:	 number of registers used.
 * @regs_desc:	 array of registers used.
 */
struct cscfg_feature_desc {
	const char *name;
	const char *description;
	u32 match_flags;
	int nr_params;
	struct cscfg_parameter_desc *params_desc;
	int nr_regs;
	struct cscfg_regval_desc *regs_desc;
};

/**
 * Configuration descriptor - describes selectable system configuration.
 *
 * A configuration describes device features in use, and may provide preset
 * values for the parameters in those features.
 *
 * A single set of presets is the sum of the parameters declared by
 * all the features in use - this value is @nr_total_params.
 *
 * UAPI version - removed kernel constructs.
 *
 * @name:		name of the configuration - used for selection.
 * @description:	description of the purpose of the configuration.
 * @nr_feat_refs:	Number of features used in this configuration.
 * @feat_ref_names:	references to features used in this configuration.
 * @nr_presets:		Number of sets of presets supplied by this configuration.
 * @nr_total_params:	Sum of all parameters declared by used features
 * @presets:		Array of preset values.
 */
struct cscfg_config_desc {
	const char *name;
	const char *description;
	int nr_feat_refs;
	const char **feat_ref_names;
	int nr_presets;
	int nr_total_params;
	const u64 *presets; /* nr_presets * nr_total_params */
};

/* UAPI allocators for descriptors in common config file buffer read code  */
static inline void *cscfg_calloc(size_t num, size_t size)
{
	return calloc(num, size);
}

static inline char *cscfg_strdup(const char *str)
{
	return strdup(str);
}

static inline void *cscfg_zalloc(size_t size)
{
	void *ptr = malloc(size);

	if (ptr)
		memset(ptr, 0, size);
	return ptr;
}

static inline void cscfg_free(void *mem)
{
	free(mem);
}

#endif /* _CORESIGHT_CORESIGHT_CONFIG_UAPI_H */
