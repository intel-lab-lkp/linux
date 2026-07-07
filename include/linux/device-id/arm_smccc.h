/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_DEVICE_ID_ARM_SMCCC_H
#define __LINUX_DEVICE_ID_ARM_SMCCC_H

#define ARM_SMCCC_NAME_SIZE 40
#define ARM_SMCCC_MODULE_PREFIX "arm_smccc:"

/**
 * struct arm_smccc_device_id - Arm SMCCC bus device identifier
 * @name: SMCCC device name
 */
struct arm_smccc_device_id {
	char name[ARM_SMCCC_NAME_SIZE];
};

#endif /* __LINUX_DEVICE_ID_ARM_SMCCC_H */
