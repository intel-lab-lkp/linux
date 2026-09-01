// SPDX-License-Identifier: GPL-2.0
/*
 * VSM boot framework that enables VTL1, loads secure kernel
 * and boots VTL1.
 *
 * Copyright (c) 2023-2025, Microsoft Corporation.
 *
 * Author: Thara Gopinath <tgopinath@linux.microsoft.com>
 *         Stanislav Kinsburskii <skinsburskii@linux.microsoft.com>
 *
 */

#define pr_fmt(fmt) "vsm: " fmt

#include <linux/hyperv.h>
#include <linux/cpumask.h>
#include <linux/namei.h>
#include <linux/acpi.h>
#include <linux/firmware.h>
#include <hyperv/vsm.h>
#include <asm/e820/types.h>
#include <asm/mshyperv.h>
#include "mshv.h"
#include "hv_vsm.h"

#define HV_VTL1_ENABLE_BIT	BIT(1)
/*
 * Firmware name looked up via request_firmware() under /lib/firmware/.
 *
 * The secure kernel image is expected to be delivered inside the signed
 * UKI/initramfs so that it is authenticated end-to-end via Secure Boot
 * before request_firmware() returns it.
 */
#define SK_FW_NAME		"vsm_sk"

static void *vsm_skm_va;

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

static Elf64_Addr __init hv_vsm_elf_min_load_paddr(void *image)
{
	Elf64_Ehdr *ehdr = image;
	Elf64_Phdr *phdr = image + ehdr->e_phoff;
	Elf64_Addr paddr = U64_MAX;
	int i;

	for (i = 0; i < ehdr->e_phnum; i++, phdr++) {
		if (phdr->p_type != PT_LOAD)
			continue;

		if (phdr->p_paddr < paddr)
			paddr = phdr->p_paddr;
	}

	return paddr;
}

static size_t __init hv_vsm_elf_binary_size(void *image)
{
	Elf64_Ehdr *ehdr = image;
	Elf64_Phdr *phdr = image + ehdr->e_phoff;
	Elf64_Addr min_paddr, max_paddr = 0;
	int i;

	min_paddr = hv_vsm_elf_min_load_paddr(image);
	if (min_paddr == U64_MAX)
		return 0;

	for (i = 0; i < ehdr->e_phnum; i++, phdr++) {
		if (phdr->p_type != PT_LOAD)
			continue;

		max_paddr = max(max_paddr, phdr->p_paddr + phdr->p_memsz);
	}

	return max_paddr - min_paddr;
}

static int __init hv_vsm_load_elf(void *image, Elf64_Addr *sk_entry_pa)
{
	Elf64_Ehdr *ehdr = image;
	Elf64_Phdr *phdr = image + ehdr->e_phoff;
	Elf64_Addr min_paddr;
	Elf64_Xword first_load_align = 0;
	size_t size;
	void *base_addr;
	int i;

	/* Find alignment of the first PT_LOAD segment. */
	for (i = 0; i < ehdr->e_phnum; i++) {
		if (phdr[i].p_type == PT_LOAD) {
			first_load_align = phdr[i].p_align;
			break;
		}
	}
	if (!first_load_align) {
		pr_err("Secure kernel does not have loadable segments\n");
		return -EINVAL;
	}

	/* Align the base load address up to the first PT_LOAD segment alignment */
	base_addr = PTR_ALIGN(vsm_skm_va + first_load_align, first_load_align);

	size = hv_vsm_elf_binary_size(image);
	if (vsm_skm_va + VSM_SK_INITIAL_MAP_SIZE - base_addr < size) {
		pr_err("secure kernel does not fit: %zu > %td\n", size,
		       vsm_skm_va + VSM_SK_INITIAL_MAP_SIZE - base_addr);
		return -EFBIG;
	}

	pr_debug("secure kernel binary size: %#zx\n", size);

	min_paddr = hv_vsm_elf_min_load_paddr(image);
	pr_debug("secure kernel minimal paddr: %#llx\n", min_paddr);

	pr_debug("loading secure kernel ELF segments:\n");

	/* Validate PT_LOAD alignment first, before touching any target memory. */
	for (i = 0; i < ehdr->e_phnum; i++) {
		if (phdr[i].p_type != PT_LOAD)
			continue;
		if (phdr[i].p_align % SZ_2M) {
			pr_err("LOAD segment is not aligned by 2MB\n");
			return -EINVAL;
		}
	}

	for (i = 0; i < ehdr->e_phnum; i++, phdr++) {
		void *load_addr;

		if (phdr->p_type != PT_LOAD)
			continue;

		/*
		 * Adjust the load address by min_paddr to compensate the
		 * offset.
		 */
		load_addr = base_addr + (phdr->p_paddr - min_paddr);

		pr_debug("  p_offset: %#016llx, p_filesz: %#016llx, p_memsz: %#016llx to pa %#016llx\n",
			 phdr->p_offset, phdr->p_filesz, phdr->p_memsz,
			 virt_to_phys(load_addr));
		memcpy(load_addr, image + phdr->p_offset, phdr->p_filesz);

		if (phdr->p_memsz == phdr->p_filesz)
			continue;

		pr_debug("    zeroing %#016llx bytes at pa %#016llx\n",
			 phdr->p_memsz - phdr->p_filesz,
			 virt_to_phys(load_addr + phdr->p_filesz));
		memset(load_addr + phdr->p_filesz, 0,
		       phdr->p_memsz - phdr->p_filesz);
	}

	*sk_entry_pa = virt_to_phys(base_addr + (ehdr->e_entry - min_paddr));
	pr_debug("secure kernel entry pa: %#llx\n", *sk_entry_pa);

	return 0;
}

static int __init hv_vsm_load_secure_kernel(Elf64_Addr *sk_entry_pa)
{
	const struct firmware *fw;
	Elf64_Ehdr *ehdr;
	int ret;

	ret = request_firmware(&fw, SK_FW_NAME, NULL);
	if (ret) {
		pr_err("Failed to load %s firmware: %d\n", SK_FW_NAME, ret);
		return ret;
	}

	ehdr = (Elf64_Ehdr *)fw->data;
	if (fw->size < sizeof(*ehdr) ||
	    memcmp(ehdr->e_ident, ELFMAG, SELFMAG) ||
	    (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN)) {
		pr_err("Not a valid ELF file: %s\n", SK_FW_NAME);
		ret = -ENOEXEC;
		goto out_release;
	}

	if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
		pr_err("Not a 64-bit compatible ELF file: %s\n", SK_FW_NAME);
		ret = -ENOEXEC;
		goto out_release;
	}

	if (!elf_check_arch(ehdr)) {
		pr_err("Not a valid ELF file: %s\n", SK_FW_NAME);
		ret = -ENOEXEC;
		goto out_release;
	}

	ret = hv_vsm_load_elf((void *)fw->data, sk_entry_pa);

out_release:
	release_firmware(fw);
	return ret;
}

static int __init hv_vsm_enable_vp_vtl(Elf64_Addr sk_entry_pa)
{
	u64 status = 0;
	unsigned long flags;
	struct hv_enable_vp_vtl *hvin;

	local_irq_save(flags);

	hvin = *this_cpu_ptr(hyperv_pcpu_input_arg);
	memset(hvin, 0, sizeof(*hvin));

	hvin->partition_id = HV_PARTITION_ID_SELF;
	hvin->vp_index = HV_VP_INDEX_SELF;
	hvin->target_vtl.target_vtl = HV_VTL_SECURE;

	hv_vsm_arch_init_vp(&hvin->vp_context, sk_entry_pa, sk_res.start);

	status = hv_do_hypercall(HVCALL_ENABLE_VP_VTL, hvin, NULL);

	local_irq_restore(flags);

	return hv_result(status);
}

static int __init hv_vsm_get_vp_status(u16 *enabled_vtl_set, u8 *active_mbec_enabled)
{
	u64 result;
	int ret;
	union hv_register_vsm_vp_status vsm_vp_status = { 0 };

	ret = hv_vsm_get_register(HV_REGISTER_VSM_VP_STATUS, &result);
	if (ret)
		return ret;

	vsm_vp_status = (union hv_register_vsm_vp_status)result;
	*enabled_vtl_set = vsm_vp_status.enabled_vtl_set;
	*active_mbec_enabled = vsm_vp_status.active_mbec_enabled;

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
	u16 vp_enabled_vtl_set = 0;
	u8 partition_max_vtl, active_mbec_enabled = 0;
	Elf64_Addr sk_entry_pa;
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

	ret = hv_vsm_load_secure_kernel(&sk_entry_pa);
	if (ret)
		return ret;

	/* Check and enable VTL1 for the primary virtual processor */
	ret = hv_vsm_get_vp_status(&vp_enabled_vtl_set, &active_mbec_enabled);
	if (ret)
		return ret;

	if (vp_enabled_vtl_set & HV_VTL1_ENABLE_BIT) {
		pr_info("VP VTL1 is already enabled\n");
	} else {
		ret = hv_vsm_enable_vp_vtl(sk_entry_pa);
		if (ret) {
			pr_err("Enabling VP VTL1 failed with status 0x%x\n", ret);
			/* TODO: Should we disable VTL1 at partition level in this case? */
			return -EINVAL;
		}
		ret = hv_vsm_get_vp_status(&vp_enabled_vtl_set, &active_mbec_enabled);
		if (ret)
			return ret;

		if (!(vp_enabled_vtl_set & HV_VTL1_ENABLE_BIT)) {
			pr_err("Tried Enabling VP VTL1 and still failed\n");
			return -EINVAL;
		}
	}
	return 0;
}

static void __init hv_vsm_get_sk_mem(void)
{
	/*
	 * The reserved secure kernel region is mandatory once VSM support has
	 * been advertised. Without it we cannot load the secure kernel and
	 * bringing up VTL1 is impossible, so fail hard rather than continuing
	 * in an unusable state.
	 */
	if (!sk_res.start)
		panic("No memory reserved in cmdline for secure kernel");

	vsm_skm_va = phys_to_virt(sk_res.start);

	pr_info("secure kernel region: %#llx-%#llx (%lld MB)\n",
		sk_res.start, sk_res.end, resource_size(&sk_res) >> 20);
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

	hv_vsm_get_sk_mem();

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
