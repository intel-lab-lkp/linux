// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Arm Limited
 */

#define pr_fmt(fmt) "smccc: " fmt

#include <linux/cache.h>
#include <linux/init.h>
#include <linux/arm-smccc.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <asm/archrandom.h>

static u32 smccc_version = ARM_SMCCC_VERSION_1_0;
static enum arm_smccc_conduit smccc_conduit = SMCCC_CONDUIT_NONE;

bool __ro_after_init smccc_trng_available = false;
s32 __ro_after_init smccc_soc_id_version = SMCCC_RET_NOT_SUPPORTED;
s32 __ro_after_init smccc_soc_id_revision = SMCCC_RET_NOT_SUPPORTED;
char __ro_after_init smccc_soc_id_name[136] = "";

void __init arm_smccc_version_init(u32 version, enum arm_smccc_conduit conduit)
{
	struct arm_smccc_res res;
	struct arm_smccc_1_2_regs regs_1_2;

	smccc_version = version;
	smccc_conduit = conduit;

	smccc_trng_available = smccc_probe_trng();

	if ((smccc_version >= ARM_SMCCC_VERSION_1_2) &&
	    (smccc_conduit != SMCCC_CONDUIT_NONE)) {
		arm_smccc_1_1_invoke(ARM_SMCCC_ARCH_FEATURES_FUNC_ID,
				     ARM_SMCCC_ARCH_SOC_ID, &res);
		if ((s32)res.a0 >= 0) {
			arm_smccc_1_1_invoke(ARM_SMCCC_ARCH_SOC_ID, 0, &res);
			smccc_soc_id_version = (s32)res.a0;
			arm_smccc_1_1_invoke(ARM_SMCCC_ARCH_SOC_ID, 1, &res);
			smccc_soc_id_revision = (s32)res.a0;

			/* Issue Number 1.6 of the Arm SMC Calling Convention
			 * specification introduces an optional "name" string
			 * to the ARM_SMCCC_ARCH_SOC_ID function.  Fetch it if
			 * available.
			 */
			regs_1_2.a0 = ARM_SMCCC_ARCH_SOC_ID;
			regs_1_2.a1 = 2;	/* SOC_ID name */
			arm_smccc_1_2_smc(
				(const struct arm_smccc_1_2_regs *)&regs_1_2,
				(struct arm_smccc_1_2_regs *)&regs_1_2);

			if ((u32)regs_1_2.a0 == 0) {
				unsigned long *destination =
					(unsigned long *)smccc_soc_id_name;

				/*
				 * Copy regs_1_2.a1..regs_1_2.a17 to the
				 * smccc_soc_id_name string with consideration
				 * to the endianness of the values in a1..a17.
				 * As per Issue 1.6 of the Arm SMC Calling
				 * Convention, the string will be NUL terminated
				 * and padded, from the end of the string to
				 * the end of the 136 byte buffer, with NULs.
				 */
				*destination++ =
				    cpu_to_le64p((const __u64 *)&regs_1_2.a1);
				*destination++ =
				    cpu_to_le64p((const __u64 *)&regs_1_2.a2);
				*destination++ =
				    cpu_to_le64p((const __u64 *)&regs_1_2.a3);
				*destination++ =
				    cpu_to_le64p((const __u64 *)&regs_1_2.a4);
				*destination++ =
				    cpu_to_le64p((const __u64 *)&regs_1_2.a5);
				*destination++ =
				    cpu_to_le64p((const __u64 *)&regs_1_2.a6);
				*destination++ =
				    cpu_to_le64p((const __u64 *)&regs_1_2.a7);
				*destination++ =
				    cpu_to_le64p((const __u64 *)&regs_1_2.a8);
				*destination++ =
				    cpu_to_le64p((const __u64 *)&regs_1_2.a9);
				*destination++ =
				    cpu_to_le64p((const __u64 *)&regs_1_2.a10);
				*destination++ =
				    cpu_to_le64p((const __u64 *)&regs_1_2.a11);
				*destination++ =
				    cpu_to_le64p((const __u64 *)&regs_1_2.a12);
				*destination++ =
				    cpu_to_le64p((const __u64 *)&regs_1_2.a13);
				*destination++ =
				    cpu_to_le64p((const __u64 *)&regs_1_2.a14);
				*destination++ =
				    cpu_to_le64p((const __u64 *)&regs_1_2.a15);
				*destination++ =
				    cpu_to_le64p((const __u64 *)&regs_1_2.a16);
				*destination++ =
				    cpu_to_le64p((const __u64 *)&regs_1_2.a17);
			}
		}
	}
}

enum arm_smccc_conduit arm_smccc_1_1_get_conduit(void)
{
	if (smccc_version < ARM_SMCCC_VERSION_1_1)
		return SMCCC_CONDUIT_NONE;

	return smccc_conduit;
}
EXPORT_SYMBOL_GPL(arm_smccc_1_1_get_conduit);

u32 arm_smccc_get_version(void)
{
	return smccc_version;
}
EXPORT_SYMBOL_GPL(arm_smccc_get_version);

s32 arm_smccc_get_soc_id_version(void)
{
	return smccc_soc_id_version;
}

s32 arm_smccc_get_soc_id_revision(void)
{
	return smccc_soc_id_revision;
}
EXPORT_SYMBOL_GPL(arm_smccc_get_soc_id_revision);

char *arm_smccc_get_soc_id_name(void)
{
	if (strnlen(smccc_soc_id_name, sizeof(smccc_soc_id_name)))
		return smccc_soc_id_name;

	return NULL;
}

static int __init smccc_devices_init(void)
{
	struct platform_device *pdev;

	if (smccc_trng_available) {
		pdev = platform_device_register_simple("smccc_trng", -1,
						       NULL, 0);
		if (IS_ERR(pdev))
			pr_err("smccc_trng: could not register device: %ld\n",
			       PTR_ERR(pdev));
	}

	return 0;
}
device_initcall(smccc_devices_init);
