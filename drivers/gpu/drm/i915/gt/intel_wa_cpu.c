// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 *
 * This file is introduced to avoid platform redefinition from
 * intel_device_info.h :(
 */

#include "intel_workarounds.h"

#ifdef CONFIG_X86
#include <asm/cpu_device_id.h>
#include <asm/intel-family.h>

static const struct x86_cpu_id wa_cpu_ids[] = {
	X86_MATCH_VFM(INTEL_ALDERLAKE,		NULL),
	X86_MATCH_VFM(INTEL_ALDERLAKE_L,	NULL),
	X86_MATCH_VFM(INTEL_COMETLAKE,		NULL),
	X86_MATCH_VFM(INTEL_KABYLAKE,		NULL),
	X86_MATCH_VFM(INTEL_KABYLAKE_L,		NULL),
	X86_MATCH_VFM(INTEL_RAPTORLAKE,		NULL),
	X86_MATCH_VFM(INTEL_RAPTORLAKE_P,	NULL),
	X86_MATCH_VFM(INTEL_RAPTORLAKE_S,	NULL),
	X86_MATCH_VFM(INTEL_ROCKETLAKE,		NULL),
	{}
};

bool intel_match_wa_cpu(void)
{
	return x86_match_cpu(wa_cpu_ids);
}
#else
bool intel_match_wa_cpu(void) { return false; }
#endif
