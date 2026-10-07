// SPDX-License-Identifier: GPL-2.0-only
/*
 * Support for the hypercall interface exposed to protected guests by
 * pKVM.
 *
 * Author: Will Deacon <will@kernel.org>
 * Copyright (C) 2024 Google LLC
 */

#include <linux/arm-smccc.h>
#include <linux/array_size.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/mem_encrypt.h>
#include <linux/memblock.h>
#include <linux/memory.h>
#include <linux/mm.h>
#include <linux/pgtable.h>

#include <asm/hypervisor.h>

static size_t __ro_after_init pkvm_granule;
DEFINE_STATIC_KEY_FALSE_RO(pkvm_guest);

static int arm_smccc_do_range(u32 func_id, phys_addr_t phys, size_t size)
{
	phys_addr_t end = phys + size;

	if (!IS_ALIGNED(phys | size, pkvm_granule))
		return -EINVAL;

	for (; phys < end; phys += pkvm_granule) {
		struct arm_smccc_res res;

		arm_smccc_1_1_invoke(func_id, phys, 0, 0, &res);
		if (res.a0 != SMCCC_RET_SUCCESS)
			return -EPERM;
	}

	return 0;
}

static int pkvm_set_memory_encrypted(unsigned long addr, int numpages)
{
	return arm_smccc_do_range(ARM_SMCCC_VENDOR_HYP_KVM_MEM_UNSHARE_FUNC_ID,
				  virt_to_phys((void *)addr),
				  numpages * PAGE_SIZE);
}

static int pkvm_set_memory_decrypted(unsigned long addr, int numpages)
{
	return arm_smccc_do_range(ARM_SMCCC_VENDOR_HYP_KVM_MEM_SHARE_FUNC_ID,
				  virt_to_phys((void *)addr),
				  numpages * PAGE_SIZE);
}

static const struct arm64_mem_crypt_ops pkvm_crypt_ops = {
	.encrypt	= pkvm_set_memory_encrypted,
	.decrypt	= pkvm_set_memory_decrypted,
};

static int mmio_guard_ioremap_hook(phys_addr_t phys, size_t size,
				   pgprot_t *prot)
{
	phys_addr_t end;
	pteval_t protval = pgprot_val(*prot);

	/*
	 * We only expect MMIO emulation for regions mapped with device
	 * attributes.
	 */
	if (protval != PROT_DEVICE_nGnRE && protval != PROT_DEVICE_nGnRnE)
		return 0;

	/*
	 * It is fine to overshoot MMIO_GUARD requests. Its sole purpose is to
	 * indicate to the hypervisor where the MMIO regions are and we have
	 * validated the alignment of the memory regions beforehand.
	 */
	end = ALIGN(phys + size, max(pkvm_granule, PAGE_SIZE));
	phys = ALIGN_DOWN(phys, max(pkvm_granule, PAGE_SIZE));

	WARN_ON_ONCE(arm_smccc_do_range(ARM_SMCCC_VENDOR_HYP_KVM_MMIO_GUARD_FUNC_ID,
					phys, end - phys));
	return 0;
}

/*
 * Return true if the MMIO_GUARD service is available and if overshooting is
 * safe, which it is if the memory regions are aligned with pkvm_granule.
 */
static bool __init mmio_guard_available(void)
{
	struct memblock_region *region;
	phys_addr_t prev_end = 0;

	if (!kvm_arm_hyp_service_available(ARM_SMCCC_KVM_FUNC_MMIO_GUARD))
		return false;

	if (pkvm_granule <= PAGE_SIZE)
		return true;

	if (IS_ENABLED(CONFIG_MEMORY_HOTPLUG) &&
	    pkvm_granule > memory_block_size_bytes())
		return false;

	for_each_mem_region(region) {
		if (prev_end == region->base)
			goto contiguous;

		if (!IS_ALIGNED(prev_end | region->base, pkvm_granule))
			return false;

contiguous:
		prev_end = region->base + region->size;
	}

	return IS_ALIGNED(prev_end, pkvm_granule);
}

void __init pkvm_init_hyp_services(void)
{
	int i;
	struct arm_smccc_res res;
	const u32 funcs[] = {
		ARM_SMCCC_KVM_FUNC_HYP_MEMINFO,
		ARM_SMCCC_KVM_FUNC_MEM_SHARE,
		ARM_SMCCC_KVM_FUNC_MEM_UNSHARE,
	};

	for (i = 0; i < ARRAY_SIZE(funcs); ++i) {
		if (!kvm_arm_hyp_service_available(funcs[i]))
			return;
	}

	arm_smccc_1_1_invoke(ARM_SMCCC_VENDOR_HYP_KVM_HYP_MEMINFO_FUNC_ID,
			     0, 0, 0, &res);
	if ((long)res.a0 <= 0)
		return;

	pkvm_granule = res.a0;
	if (pkvm_granule > PAGE_SIZE)
		pr_info("pKVM: sharing memory in %zu-byte granules\n", pkvm_granule);
	arm64_mem_crypt_ops_register(&pkvm_crypt_ops);

	if (mmio_guard_available())
		arm64_ioremap_prot_hook_register(&mmio_guard_ioremap_hook);

	static_branch_enable(&pkvm_guest);
}
