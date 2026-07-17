/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * UAPI definitions for the CLA character device.
 */

#ifndef _UAPI_LINUX_CLA_H
#define _UAPI_LINUX_CLA_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define ARM_CLA_UABI_VERSION	1
#define ARM_CLA_IOC_MAGIC	'C'

/**
 * define ARM_CLA_IOCTL_GET_PARAM - Get the value of a parameter.
 *
 * ioctl command whose argument is a pointer to &struct arm_cla_param.
 * &arm_cla_param->param and &arm_cla_param->index are input parameters.
 * &arm_cla_param->value is an output parameter.
 *
 * Return:
 * * %0 - OK
 * * %-EINVAL - Unrecognised param or param does not support GET
 * * Return code as defined for param
 */
#define ARM_CLA_IOCTL_GET_PARAM	\
	_IOWR(ARM_CLA_IOC_MAGIC, 0x00, struct arm_cla_param)

/**
 * define ARM_CLA_PARAM_UABI_VERSION - CLA driver UABI version (RO).
 *
 * &arm_cla_param->index must be 0.
 *
 * Return:
 * * %0 - OK
 * * %-EINVAL - index not 0
 */
#define ARM_CLA_PARAM_UABI_VERSION	0

/**
 * define ARM_CLA_PARAM_DEV_NR - Number of attached CLA devices (RO).
 *
 * &arm_cla_param->index must be 0.
 *
 * Return:
 * * %0 - OK
 * * %-EINVAL - index not 0
 */
#define ARM_CLA_PARAM_DEV_NR		1

/**
 * define ARM_CLA_PARAM_DEV_CPU_ID - CPU to which the CLA is attached (RO).
 *
 * &arm_cla_param->index is the dev_id in range (0, DEV_NR - 1).
 *
 * Return:
 * * %0 - OK
 * * %-EINVAL - dev_id (index) not in range
 */
#define ARM_CLA_PARAM_DEV_CPU_ID	2

/**
 * define ARM_CLA_PARAM_DEV_DOMAIN_ID - Domain to which the CLA belongs (RO).
 *
 * &arm_cla_param->index is the dev_id in range (0, DEV_NR - 1).
 *
 * A CLA domain contains a set of CLA devices whose accelerators can communicate
 * with each other. All CLA devices within a CLA domain are atomically assigned
 * to a single context at a time.
 *
 * Return:
 * * %0 - OK
 * * %-EINVAL - dev_id (index) not in range
 */
#define ARM_CLA_PARAM_DEV_DOMAIN_ID	3

/**
 * define ARM_CLA_PARAM_DEV_PGOFF - CLA's ``mmap()`` page offset (RO).
 *
 * &arm_cla_param->index is the dev_id in range (0, DEV_NR - 1).
 *
 * The page offset at which the selected CLA device's register page is exposed
 * through ``mmap()``.
 *
 * Return:
 * * %0 - OK
 * * %-EINVAL - dev_id (index) not in range
 */
#define ARM_CLA_PARAM_DEV_PGOFF		4

/**
 * define ARM_CLA_PARAM_DEV_AIDR - CLA Architecture Identification Register
 * (RO).
 *
 * &arm_cla_param->index is the dev_id in range (0, DEV_NR - 1).
 *
 * The CLA Architecture Identification Register is used to obtain version
 * information of the CLA Programming model.
 *
 * Return:
 * * %0 - OK
 * * %-EINVAL - dev_id (index) not in range
 */
#define ARM_CLA_PARAM_DEV_AIDR		5

/**
 * define ARM_CLA_PARAM_DEV_ACCELS - Mask of CLA's attached accelerators (RO).
 *
 * &arm_cla_param->index is the dev_id in range (0, DEV_NR - 1).
 *
 * A bitmask describing which accelerator slots are attached on the selected CLA
 * device. A CLA may have up to 8 attached accelerators, each with an accel_id
 * in the range (0, 7), each with a corresponding bit in the bitmask.
 *
 * Return:
 * * %0 - OK
 * * %-EINVAL - dev_id (index) not in range
 */
#define ARM_CLA_PARAM_DEV_ACCELS	6

/**
 * define ARM_CLA_PARAM_ACCEL_IIDR - Accelerator IIDR register (RO).
 *
 * &arm_cla_param->index is ARM_CLA_PARAM_ACCEL_INDEX(accel_id, dev_id).
 *
 * Return:
 * * %0 - OK
 * * %-EINVAL - dev_id not in range
 * * %-EINVAL - accel_id not in range
 * * %-ENODEV - accel_id not attached to CLA
 */
#define ARM_CLA_PARAM_ACCEL_IIDR	7

/**
 * define ARM_CLA_PARAM_ACCEL_DEVARCH - Accelerator DEVARCH register (RO).
 *
 * &arm_cla_param->index is ARM_CLA_PARAM_ACCEL_INDEX(accel_id, dev_id).
 *
 * Return:
 * * %0 - OK
 * * %-EINVAL - dev_id not in range
 * * %-EINVAL - accel_id not in range
 * * %-ENODEV - accel_id not attached to CLA
 */
#define ARM_CLA_PARAM_ACCEL_DEVARCH	8

/**
 * define ARM_CLA_PARAM_ACCEL_REVIDR - Accelerator REVIDR register (RO).
 *
 * &arm_cla_param->index is ARM_CLA_PARAM_ACCEL_INDEX(accel_id, dev_id).
 *
 * Return:
 * * %0 - OK
 * * %-EINVAL - dev_id not in range
 * * %-EINVAL - accel_id not in range
 * * %-ENODEV - accel_id not attached to CLA
 */
#define ARM_CLA_PARAM_ACCEL_REVIDR	9

/**
 * ARM_CLA_PARAM_ACCEL_INDEX() - encode index for specific accelerator.
 * @accel_id:	accelerator id in the range (0, 7).
 * @dev_id:	CLA device id in the range (0, DEV_NR - 1).
 *
 * Return: encoded index for use in &arm_cla_param->index.
 */
#define ARM_CLA_PARAM_ACCEL_INDEX(accel_id, dev_id) \
	(((accel_id) << 24) | (dev_id))

/**
 * ARM_CLA_PARAM_INDEX_ACCEL() - extract accel_id from index.
 * @index:	Encoded index as returned by ARM_CLA_PARAM_ACCEL_INDEX()
 *
 * Return: extracted accel_id.
 */
#define ARM_CLA_PARAM_INDEX_ACCEL(index) (((index) >> 24) & 0xff)

/**
 * ARM_CLA_PARAM_INDEX_DEV() - extract dev_id from index.
 * @index:	Encoded index as returned by ARM_CLA_PARAM_ACCEL_INDEX()
 *
 * Return: extracted dev_id.
 */
#define ARM_CLA_PARAM_INDEX_DEV(index) ((index) & 0xffffff)

/**
 * struct arm_cla_param - Get/Set CLA parameters.
 */
struct arm_cla_param {
	/**
	 * @param: Param selector - One of the ARM_CLA_PARAM_* values.
	 */
	__u64 param;

	/**
	 * @index: Index for params that have multiple instances.
	 */
	__u64 index;

	/**
	 * @value: Param value.
	 */
	__u64 value;
};

#endif /* _UAPI_LINUX_CLA_H */
