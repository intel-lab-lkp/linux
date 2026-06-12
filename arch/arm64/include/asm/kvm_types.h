/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_ARM64_KVM_TYPES_H
#define _ASM_ARM64_KVM_TYPES_H

#define KVM_ARCH_NR_OBJS_PER_MEMORY_CACHE 40

enum vcpu_pmu_register_access {
	VCPU_PMU_ACCESS_FREE,
	VCPU_PMU_ACCESS_GUEST_OWNED,
};

#endif /* _ASM_ARM64_KVM_TYPES_H */
