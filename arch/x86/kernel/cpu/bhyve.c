// SPDX-License-Identifier: GPL-2.0
/*
 * FreeBSD Bhyve guest enlightenments
 *
 * Copyright © 2025 Amazon.com, Inc. or its affiliates.
 *
 * Author: David Woodhouse <dwmw2@infradead.org>
 */

#include <linux/init.h>
#include <linux/export.h>
#include <asm/processor.h>
#include <asm/hypervisor.h>

static uint32_t bhyve_cpuid_base;
static uint32_t bhyve_cpuid_max;

#define CPUID_BHYVE_FEATURES		1

/* Features advertised in CPUID_BHYVE_FEATURES %eax */
#define CPUID_BHYVE_FEAT_EXT_DEST_ID	(1UL << 0) /* MSI Extended Dest ID */

static uint32_t __init bhyve_detect(void)
{
	if (boot_cpu_data.cpuid_level < 0 ||
            !boot_cpu_has(X86_FEATURE_HYPERVISOR))
                return 0;

	bhyve_cpuid_base = cpuid_base_hypervisor("bhyve bhyve ", 0);
	if (!bhyve_cpuid_base)
		return 0;

	bhyve_cpuid_max = cpuid_eax(bhyve_cpuid_max);
	return bhyve_cpuid_max;
}

static uint32_t bhyve_features(void)
{
	if (bhyve_cpuid_max < bhyve_cpuid_base + CPUID_BHYVE_FEATURES)
		return 0;

	return cpuid_eax(bhyve_cpuid_base + CPUID_BHYVE_FEATURES);
}

static bool __init bhyve_ext_dest_id(void)
{
	return !!(bhyve_features() & CPUID_BHYVE_FEAT_EXT_DEST_ID);
}

static bool __init bhyve_x2apic_available(void)
{
	/* Bhyve has always supported x2apic */
	return true;
}

const struct hypervisor_x86 x86_hyper_bhyve __refconst = {
	.name			= "Bhyve",
	.detect			= bhyve_detect,
	.init.init_platform	= x86_init_noop,
	.init.x2apic_available	= bhyve_x2apic_available,
	.init.msi_ext_dest_id	= bhyve_ext_dest_id,
};
