/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Ltd.
 */

#ifndef _VIRT_COCO_RSI_H_
#define _VIRT_COCO_RSI_H_

#include <linux/arm-smccc-rsi.h>

/**
 * rsi_attestation_token_init - Initialise the operation to retrieve an
 * attestation token.
 *
 * @challenge:	The challenge data to be used in the attestation token
 *		generation.
 * @size:	Size of the challenge data in bytes.
 *
 * Initialises the attestation token generation and returns an upper bound
 * on the attestation token size that can be used to allocate an adequate
 * buffer. The caller is expected to subsequently call
 * rsi_attestation_token_continue() to retrieve the attestation token data on
 * the same CPU.
 *
 * Returns:
 *  On success, returns the upper limit of the attestation report size.
 *  Otherwise, -EINVAL
 */
static inline long
rsi_attestation_token_init(const u8 *challenge, unsigned long size)
{
	struct arm_smccc_1_2_regs regs = { 0 };

	/* The challenge must be at least 32bytes and at most 64bytes */
	if (!challenge || size < 32 || size > 64)
		return -EINVAL;

	regs.a0 = SMC_RSI_ATTESTATION_TOKEN_INIT;
	memcpy(&regs.a1, challenge, size);
	arm_smccc_1_2_smc(&regs, &regs);

	if (regs.a0 == RSI_SUCCESS)
		return regs.a1;

	return -EINVAL;
}

/**
 * rsi_attestation_token_continue - Continue the operation to retrieve an
 * attestation token.
 *
 * @granule: {I}PA of the Granule to which the token will be written.
 * @offset:  Offset within Granule to start of buffer in bytes.
 * @size:    The size of the buffer.
 * @len:     The number of bytes written to the buffer.
 *
 * Retrieves up to a RSI_GRANULE_SIZE worth of token data per call. The caller
 * is expected to call rsi_attestation_token_init() before calling this
 * function to retrieve the attestation token.
 *
 * Return:
 * * %RSI_SUCCESS     - Attestation token retrieved successfully.
 * * %RSI_INCOMPLETE  - Token generation is not complete.
 * * %RSI_ERROR_INPUT - A parameter was not valid.
 * * %RSI_ERROR_STATE - Attestation not in progress.
 */
static inline unsigned long rsi_attestation_token_continue(phys_addr_t granule,
							   unsigned long offset,
							   unsigned long size,
							   unsigned long *len)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(SMC_RSI_ATTESTATION_TOKEN_CONTINUE,
			     granule, offset, size, 0, &res);

	if (len)
		*len = res.a1;
	return res.a0;
}



#endif
