/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ARM_SMMU_V3_COMMON_LIB_H
#define __ARM_SMMU_V3_COMMON_LIB_H

#include <linux/build_bug.h>
#include <linux/compiler_types.h>
#include <linux/kernel.h>

/*
 * The IDR probe functions are used by the kernel and the
 * hypervisor drivers where ARM_SMMU_OBJ might be defined
 * differently.
 * Ensure fields used by them are defined and has the correct
 * types.
 */
#ifndef __KVM_NVHE_HYPERVISOR__
typedef struct arm_smmu_device ARM_SMMU_OBJ;
#endif

static_assert(__same_type(typeof_member(ARM_SMMU_OBJ, features), u32));
static_assert(__same_type(typeof_member(ARM_SMMU_OBJ, options), u32));
static_assert(__same_type(typeof_member(ARM_SMMU_OBJ, oas), unsigned long));
static_assert(__same_type(typeof_member(ARM_SMMU_OBJ, pgsize_bitmap), unsigned long));
static_assert(__same_type(typeof_member(ARM_SMMU_OBJ, base), void __iomem *));

void arm_smmu_device_iidr_probe(ARM_SMMU_OBJ *smmu);
u32 arm_smmu_idr0_probe(ARM_SMMU_OBJ *smmu);
void arm_smmu_idr3_probe(ARM_SMMU_OBJ *smmu);
u32 arm_smmu_idr5_probe(ARM_SMMU_OBJ *smmu);

#endif /* __ARM_SMMU_V3_COMMON_LIB_H */
