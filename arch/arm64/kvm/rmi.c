// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023-2025 ARM Ltd.
 */

#include <linux/kvm_host.h>

#include <asm/kvm_pgtable.h>
#include <asm/rmi_cmds.h>
#include <asm/virt.h>

static bool rmi_has_feature(unsigned long feature)
{
	return !!u64_get_bits(rmm_feat_reg0, feature);
}

static int rmm_check_features(void)
{
	if (kvm_lpa2_is_enabled() && !rmi_has_feature(RMI_FEATURE_REGISTER_0_LPA2)) {
		kvm_err("RMM doesn't support LPA2");
		return -ENXIO;
	}

	return 0;
}

void kvm_init_rmi(void)
{
	/*
	 * TODO: Support Realm guests in nVHE mode, this will require adding
	 * EL2 stub(s) for REC entry and possibly other things.
	 */
	if (!is_kernel_in_hyp_mode())
		return;

	if (!rmi_is_available())
		return;

	if (rmm_check_features())
		return;

	/* Future patch will enable static branch kvm_rmi_is_available */
}
