// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023-2025 ARM Ltd.
 */

#include <linux/memblock.h>

#include <asm/rmi_cmds.h>

unsigned long rmm_feat_reg0;
unsigned long rmm_feat_reg1;

static int rmi_check_version(void)
{
	struct arm_smccc_res res;
	unsigned short version_major, version_minor;
	unsigned long host_version = RMI_ABI_VERSION(RMI_ABI_MAJOR_VERSION,
						     RMI_ABI_MINOR_VERSION);
	unsigned long aa64pfr0 = read_sanitised_ftr_reg(SYS_ID_AA64PFR0_EL1);

	/* If RME isn't supported, then RMI can't be */
	if (cpuid_feature_extract_unsigned_field(aa64pfr0, ID_AA64PFR0_EL1_RME_SHIFT) == 0)
		return -ENXIO;

	arm_smccc_1_1_invoke(SMC_RMI_VERSION, host_version, &res);

	if (res.a0 == SMCCC_RET_NOT_SUPPORTED)
		return -ENXIO;

	version_major = RMI_ABI_VERSION_GET_MAJOR(res.a1);
	version_minor = RMI_ABI_VERSION_GET_MINOR(res.a1);

	if (res.a0 != RMI_SUCCESS) {
		unsigned short high_version_major, high_version_minor;

		high_version_major = RMI_ABI_VERSION_GET_MAJOR(res.a2);
		high_version_minor = RMI_ABI_VERSION_GET_MINOR(res.a2);

		pr_err("Unsupported RMI ABI (v%d.%d - v%d.%d) we want v%d.%d\n",
		       version_major, version_minor,
		       high_version_major, high_version_minor,
		       RMI_ABI_MAJOR_VERSION,
		       RMI_ABI_MINOR_VERSION);
		return -ENXIO;
	}

	pr_info("RMI ABI version %d.%d\n", version_major, version_minor);

	return 0;
}

static int __init arm64_init_rmi(void)
{
	/* Continue without realm support if we can't agree on a version */
	if (rmi_check_version())
		return 0;

	if (WARN_ON(rmi_features(0, &rmm_feat_reg0)))
		return 0;
	if (WARN_ON(rmi_features(1, &rmm_feat_reg1)))
		return 0;

	return 0;
}
subsys_initcall(arm64_init_rmi);
