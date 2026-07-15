// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023-2026 ARM Ltd.
 */

#include <linux/cpufeature.h>
#include <linux/memblock.h>
#include <linux/arm-rmi-cmds.h>
#include <linux/slab.h>

#include <asm/memory.h>
#include <asm/pgtable-hwdef.h>

/* Currently only the first 2 registers are used by Linux */
#define RMI_FEAT_REG_COUNT	2
static __ro_after_init unsigned long rmi_feat_reg_cache[RMI_FEAT_REG_COUNT];

unsigned long rmi_feat_reg(unsigned long id)
{
	if (WARN_ON(id >= RMI_FEAT_REG_COUNT))
		return 0;

	return rmi_feat_reg_cache[id];
}

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

static int rmi_read_features(void)
{
	/*
	 * Since we've negotiated a compatible version these feature registers
	 * should always be available
	 */
	for (int i = 0; i < RMI_FEAT_REG_COUNT; i++) {
		if (WARN_ON(rmi_features(i, &rmi_feat_reg_cache[i])))
			return -EINVAL;
	}

	return 0;
}

static int __init arm64_init_rmi(void)
{
	int ret;

	/* Continue without realm support if we can't agree on a version */
	ret = rmi_check_version();
	if (ret)
		return ret;

	ret = rmi_read_features();
	if (ret)
		return ret;

	return 0;
}

/*
 * Note arm64_init_rmi() must be called before kvm_init_rmi() otherwise KVM
 * will not support realm guests. subsys_initcall() is called before
 * module_init() (used for KVM) so this is OK.
 */
subsys_initcall(arm64_init_rmi);
