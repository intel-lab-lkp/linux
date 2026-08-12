/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_S390_SAE_H
#define __ASM_S390_SAE_H

#include <linux/linkage.h>
#include <linux/types.h>

#ifndef __ASSEMBLER__

/* defined in arch/s390/kernel/entry.S */
asmlinkage void __sae64a(phys_addr_t sae_block_phys);

#include <linux/io.h>
#include <asm/kvm_host_arm64_types.h>

/**
 * __sae64a() - Start Arm Execution
 */
static inline void sae64a(struct kvm_sae_block *sae_block)
{
	__sae64a(virt_to_phys(sae_block));
}

/**
 * stiasrm() - STore and Invalidate Arm System Register Multiple
 * @save_area: Pointer to SAE save area
 *
 * Store the guest system register to the save area.
 * The values in the guest are not valid anymore..
 */
static __always_inline void stiasrm(struct kvm_sae_save_area *save_area)
{
	asm volatile(
		"	.insn	rre,0xb9a70000,%[r1],0\n"
		: "+m"(*save_area)
		: [r1] "a"(save_area)
	);
}

/**
 * lasrm() - Load Arm System Register Multiple
 *
 * @save_area: Pointer to SAE save area
 *
 * Load the system registers from save_area into the guest.
 */
static __always_inline void lasrm(struct kvm_sae_save_area *save_area)
{
	asm volatile(
		"	.insn	rre,0xb9a60000,%[r1],0\n"
		:
		: "m"(*save_area), [r1] "a"(save_area)
	);
}

#endif /* !__ASSEMBLER__ */
#endif /* __ASM_S390_SAE_H */
