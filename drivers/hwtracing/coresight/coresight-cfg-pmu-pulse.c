// SPDX-License-Identifier: GPL-2.0
/*
 * Pulse ETM external output 1 on every traced cycle for a PMU event to count.
 * Based on coresight-cfg-afdo.c
 */

#include "coresight-config.h"

/* ETMv4 includes and features */
#if IS_ENABLED(CONFIG_CORESIGHT_SOURCE_ETM4X)
#include "coresight-etm4x-cfg.h"

/*
 * extout_pulse feature: event 1 selects the always-true resource, so ETM
 * external output 1 pulses on every cycle while the trace unit is enabled. A
 * PMU event counting that output (TRCEXTOUT1), for example with
 * aux-action=pause, then fires after as many traced cycles as its sample
 * period. Event 0 is avoided because it is the trace unit trigger, and the
 * TRBE driver treats a detected trigger as fatal.
 */
static struct cscfg_regval_desc extout_pulse_regs[] = {
	/* event 1: resource selector 1 (always true), drives external output 1 */
	{
		.type = CS_CFG_REG_TYPE_STD | CS_CFG_REG_TYPE_VAL_MASK,
		.offset = TRCEVENTCTL0R,
		.val32 = 0x0100,
		.mask32 = 0xff00,
	},
	/* end of regs */
};

struct cscfg_feature_desc extout_pulse_etm4x = {
	.name = "extout_pulse",
	.description = "Pulse external output 1 on every traced cycle.\n",
	.match_flags = CS_CFG_MATCH_CAP_PMU_EXTOUT1,
	.nr_regs = ARRAY_SIZE(extout_pulse_regs),
	.regs_desc = extout_pulse_regs,
};

/* create a pmu_pulse configuration */

static const char * const pmu_pulse_ref_names[] = {
	"extout_pulse",
};

struct cscfg_config_desc pmu_pulse_etm4x = {
	.name = "pmu_pulse",
	.description = "Pulse ETM external output 1 on every traced cycle for the PMU\n"
	"to count as TRCEXTOUT1\n",
	.nr_feat_refs = ARRAY_SIZE(pmu_pulse_ref_names),
	.feat_ref_names = pmu_pulse_ref_names,
};

/* end of ETM4x configurations */
#endif	/* IS_ENABLED(CONFIG_CORESIGHT_SOURCE_ETM4X) */
