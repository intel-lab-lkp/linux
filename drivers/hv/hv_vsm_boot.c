// SPDX-License-Identifier: GPL-2.0
/*
 * VSM boot framework that enables VTL1, loads secure kernel
 * and boots VTL1.
 *
 * Copyright (c) 2023-2025, Microsoft Corporation.
 *
 * Author: Thara Gopinath <tgopinath@linux.microsoft.com>
 *
 */

#define pr_fmt(fmt) "vsm: " fmt

#include <linux/hyperv.h>
#include <linux/cpumask.h>
#include <asm/mshyperv.h>
#include "mshv.h"

#define HV_VTL1_ENABLE_BIT	BIT(1)

static int hv_vsm_get_register(u32 reg_name, u64 *result)
{
	struct hv_register_assoc reg = {
		.name = reg_name,
	};
	union hv_input_vtl input_vtl = {
		.as_uint8 = 0,
	};
	int ret;

	ret = hv_call_get_vp_registers(HV_VP_INDEX_SELF,
				       HV_PARTITION_ID_SELF,
				       1, input_vtl, &reg);
	if (ret)
		return ret;

	*result = reg.value.reg64;
	return 0;
}

static int __init hv_vsm_enable_partition_vtl(void)
{
	u64 status = 0;
	unsigned long flags;
	struct hv_input_enable_partition_vtl *hvin = NULL;

	local_irq_save(flags);

	hvin = *this_cpu_ptr(hyperv_pcpu_input_arg);
	memset(hvin, 0, sizeof(*hvin));

	hvin->partition_id = HV_PARTITION_ID_SELF;
	hvin->target_vtl.as_uint8 = 1;
	hvin->flags.enable_mbec = 1;

	status = hv_do_hypercall(HVCALL_ENABLE_PARTITION_VTL, hvin, NULL);
	if (hv_result(status))
		pr_err("Enable Partition VTL failed. status=0x%x\n",
		       hv_result(status));

	local_irq_restore(flags);

	return hv_result(status);
}

static int __init hv_vsm_get_partition_status(u16 *enabled_vtl_set, u8 *max_vtl,
					      u16 *mbec_enabled_vtl_set)
{
	u64 result;
	int ret;
	union hv_register_vsm_partition_status vsm_partition_status = { 0 };

	ret = hv_vsm_get_register(HV_REGISTER_VSM_PARTITION_STATUS, &result);
	if (ret)
		return ret;

	vsm_partition_status = (union hv_register_vsm_partition_status)result;
	*enabled_vtl_set = vsm_partition_status.enabled_vtl_set;
	*max_vtl = vsm_partition_status.max_vtl;
	*mbec_enabled_vtl_set = vsm_partition_status.mbec_enabled_vtl_set;
	return 0;
}

static int __init hv_vsm_bootstrap_vtl(void)
{
	u16 partition_enabled_vtl_set = 0, partition_mbec_enabled_vtl_set = 0;
	u8 partition_max_vtl;
	int ret;

	/* Check and enable VTL1 at the partition level */
	ret = hv_vsm_get_partition_status(&partition_enabled_vtl_set, &partition_max_vtl,
					  &partition_mbec_enabled_vtl_set);
	if (ret)
		return ret;

	if (partition_max_vtl < HV_VTL_SECURE) {
		pr_err("VTL1 is not supported by the partition\n");
		return -EINVAL;
	}

	if (partition_enabled_vtl_set & HV_VTL1_ENABLE_BIT) {
		pr_info("Partition VTL1 is already enabled\n");
	} else {
		ret = hv_vsm_enable_partition_vtl();
		if (ret) {
			pr_err("Enabling Partition VTL1 failed with status 0x%x\n",
			       ret);
			return -EINVAL;
		}
		ret = hv_vsm_get_partition_status(&partition_enabled_vtl_set, &partition_max_vtl,
						  &partition_mbec_enabled_vtl_set);
		if (ret)
			return ret;
		if (!(partition_enabled_vtl_set & HV_VTL1_ENABLE_BIT)) {
			pr_err("Tried Enabling Partition VTL 1 and still failed\n");
			return -EINVAL;
		}
		if (!partition_mbec_enabled_vtl_set) {
			pr_err("Tried Enabling Partition MBEC and failed\n");
			return -EINVAL;
		}
	}
	return 0;
}

static bool __init vsm_arch_has_vsm_access(void)
{
	if (!(ms_hyperv.features & HV_MSR_SYNIC_AVAILABLE))
		return false;
	if (!(ms_hyperv.priv_high & HV_ACCESS_VSM))
		return false;
	if (!(ms_hyperv.priv_high & HV_ACCESS_VP_REGS))
		return false;
	return true;
}

static int __init hv_vsm_boot_init(void)
{
	cpumask_var_t mask;
	unsigned int boot_cpu;
	int ret;

	if (!vsm_arch_has_vsm_access())
		return 0;

	/*
	 * Copy the current cpu mask and pin rest of the running code to boot cpu.
	 * Important since we want boot cpu of VTL0 to be the boot cpu for VTL1.
	 * ToDo: Check if copying and restoring current->cpus_mask is enough
	 * ToDo: Verify the assumption that cpumask_first(cpu_online_mask) is
	 * the boot cpu
	 */
	if (!alloc_cpumask_var(&mask, GFP_KERNEL))
		panic("Could not allocate cpumask");

	cpumask_copy(mask, &current->cpus_mask);
	boot_cpu = cpumask_first(cpu_online_mask);
	set_cpus_allowed_ptr(current, cpumask_of(boot_cpu));

	ret = hv_vsm_bootstrap_vtl();
	/*
	 * At this point VTL0 has already advertised VSM support to the
	 * bootloader/firmware via the Hyper-V OsLoaderIndications EFI
	 * variable (see the x86-stub change). That signals the platform
	 * that a trusted VTL1 will be brought up. If we fail to actually
	 * set VTL1 up here, the partition is left in a state where an
	 * attacker could race to configure VTL1 themselves and gain a
	 * higher-privilege foothold than VTL0. Panic rather than continue
	 * running with that exposure.
	 */
	if (ret)
		panic("VTL1 boot failure caused kernel panic; consult log for more details.\n");

	set_cpus_allowed_ptr(current, mask);
	free_cpumask_var(mask);
	return ret;
}
device_initcall(hv_vsm_boot_init);
