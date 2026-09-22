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
#else
#include "pkvm/arm-smmu-v3-hyp.h"

typedef struct hyp_arm_smmu_v3_device ARM_SMMU_OBJ;
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

void arm_smmu_get_ste_used(const __le64 *ent, __le64 *used_bits);
void arm_smmu_get_ste_update_safe(const __le64 *cur, const __le64 *target,
				  __le64 *safe_bits);
void arm_smmu_write_entry(struct arm_smmu_entry_writer *writer, __le64 *cur,
			  const __le64 *target);

#endif /* __ARM_SMMU_V3_COMMON_LIB_H */
