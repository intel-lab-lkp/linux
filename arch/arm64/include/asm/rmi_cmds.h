/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2023-2026 ARM Ltd.
 */

#ifndef __ASM_RMI_CMDS_H
#define __ASM_RMI_CMDS_H

#include <linux/arm-rmi-cmds.h>
#include <linux/arm-smccc-rmi.h>

/**
 * rmi_psci_complete() - Complete pending PSCI command
 * @calling_rec: PA of the calling REC
 * @status: Status of the PSCI request
 *
 * Completes a pending PSCI command.
 *
 * Return: RMI return code
 */
static inline int rmi_psci_complete(unsigned long calling_rec,
				    unsigned long status)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_PSCI_COMPLETE, calling_rec, status, &res);

	return res.a0;
}

/**
 * rmi_realm_activate() - Active a realm
 * @rd: PA of the RD
 *
 * Mark a realm as Active signalling that creation is complete and allowing
 * execution of the realm.
 *
 * Return: RMI return code
 */
static inline int rmi_realm_activate(unsigned long rd)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_REALM_ACTIVATE, rd, &res);

	return res.a0;
}

/**
 * rmi_rec_enter() - Enter a REC
 * @rec: PA of the target REC
 * @run_ptr: PA of RecRun structure
 *
 * Starts (or continues) execution within a REC.
 *
 * Return: RMI return code
 */
static inline int rmi_rec_enter(unsigned long rec, unsigned long run_ptr)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RMI_REC_ENTER, rec, run_ptr, &res);

	return res.a0;
}

#endif /* __ASM_RMI_CMDS_H */
