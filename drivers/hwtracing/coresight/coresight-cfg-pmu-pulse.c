// SPDX-License-Identifier: GPL-2.0
/* Pulse trace-unit external output 1 with the always-true resource. */

#include "coresight-config.h"

#if IS_ENABLED(CONFIG_CORESIGHT_SOURCE_ETM4X)
#include "coresight-etm4x-cfg.h"
#include "coresight-cfg-preload.h"

/* Output 0 is the trace trigger and can stop TRBE capture. */
static struct cscfg_regval_desc extout_pulse_regs[] = {
	{
		.type = CS_CFG_REG_TYPE_STD | CS_CFG_REG_TYPE_VAL_MASK,
		.offset = TRCEVENTCTL0R,
		.val32 = ETM4_RES_SEL_TRUE << 8,
		.mask32 = GENMASK_U32(15, 8),
	},
};

struct cscfg_feature_desc extout_pulse_etm4x = {
	.name = "extout_pulse",
	.description = "Pulse external output 1 with the always-true resource.\n",
	.match_flags = CS_CFG_MATCH_CLASS_SRC_ETM4,
	.nr_regs = ARRAY_SIZE(extout_pulse_regs),
	.regs_desc = extout_pulse_regs,
};

static const char *pmu_pulse_ref_names[] = {
	"extout_pulse",
};

struct cscfg_config_desc pmu_pulse_etm4x = {
	.name = "pmu_pulse",
	.description = "Pulse external output 1 for an external consumer.\n",
	.nr_feat_refs = ARRAY_SIZE(pmu_pulse_ref_names),
	.feat_ref_names = pmu_pulse_ref_names,
};

#endif
