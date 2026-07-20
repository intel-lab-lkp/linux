// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020 Xing Loong <xing.xl.loong@gmail.com>
 * ARM RPC calls to TEE - relies on SMC
 *
 * Uses arm_smccc_smc() with SMCCC-compatible calling convention:
 *   Fast call  : r0/x0 = fn (bit 31 set), r1-r3/x1-x3 = args.
 *   Yield call : r0/x0 = fn (bit 31 clear), r1/x1 = phys(rpc_cmd).
 *
 * ARM fast calls are synchronous SMC instructions.
 */
#include <linux/mm.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/arm-smccc.h>
#include "mbedtee_drv.h"

long mbedtee_rpc_yieldcall(struct mbedtee_device *mbedtee,
			   unsigned long fn, struct mbedtee_rpc_call *call,
			   bool interruptible)
{
	long ret;
	struct arm_smccc_res res;

	if (MBEDTEE_RPC_IS_FASTCALL(fn))
		return -EINVAL;

	call->rpc.id = fn;

	arm_smccc_smc(fn, call->rpc_phys,
		      0, 0, 0, 0, 0, 0, &res);
	ret = res.a0;
	if (ret != 0)
		return ret;

	return mbedtee_rpc_wait_for_completion(mbedtee, call, interruptible);
}

long mbedtee_rpc_fastcall(struct mbedtee_device *mbedtee,
			  unsigned long fn, unsigned long a0,
			  unsigned long a1, unsigned long a2)
{
	struct arm_smccc_res res;

	if (!MBEDTEE_RPC_IS_FASTCALL(fn))
		return -EINVAL;

	arm_smccc_smc(fn, a0, a1, a2, 0, 0, 0, 0, &res);

	return (long)res.a0;
}

/*
 * ARM uses direct SMC; no caller-side ring buffer initialisation needed.
 */
int mbedtee_r2t_init(struct mbedtee_device *mbedtee)
{
	return 0;
}

void mbedtee_r2t_uninit(struct mbedtee_device *mbedtee)
{
}
