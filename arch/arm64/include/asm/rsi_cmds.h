/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2023 - 2025 ARM Ltd.
 */

#ifndef __ASM_RSI_CMDS_H
#define __ASM_RSI_CMDS_H

#include <linux/arm-smccc.h>
#include <linux/string.h>
#include <asm/memory.h>

#include <asm/rsi_smc.h>

#define RSI_GRANULE_SHIFT		12
#define RSI_GRANULE_SIZE		(_AC(1, UL) << RSI_GRANULE_SHIFT)

/*
 * Maximum measurement data size in bytes.
 * According to the RMM Specification, the width of the RmmRealmMeasurement type
 * is 512 bits.
 */
#define RSI_MAX_MEASUREMENT_DATA_SIZE_BYTES  64

/*
 * Indices for the Realm Initial Measurement register (RIM) and the Realm
 * Extensible Measurement registers (REMs).
 * According to the RMM Specification, Realm attributes of a Realm include
 * an array of measurement values. The first entry in this array is a RIM.
 * The remaining entries in this array are REMs.
 */
#define RSI_INDEX_RIM		0
#define RSI_INDEX_REM0		1
#define RSI_INDEX_REM1		2
#define RSI_INDEX_REM2		3
#define RSI_INDEX_REM3		4

enum ripas {
	RSI_RIPAS_EMPTY = 0,
	RSI_RIPAS_RAM = 1,
	RSI_RIPAS_DESTROYED = 2,
	RSI_RIPAS_DEV = 3,
};

static inline unsigned long rsi_request_version(unsigned long req,
						unsigned long *out_lower,
						unsigned long *out_higher)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_RSI_ABI_VERSION, req, 0, 0, 0, 0, 0, 0, &res);

	if (out_lower)
		*out_lower = res.a1;
	if (out_higher)
		*out_higher = res.a2;

	return res.a0;
}

static inline unsigned long rsi_get_realm_config(struct realm_config *cfg)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_RSI_REALM_CONFIG, virt_to_phys(cfg),
		      0, 0, 0, 0, 0, 0, &res);
	return res.a0;
}

static inline unsigned long rsi_ipa_state_get(phys_addr_t start,
					      phys_addr_t end,
					      enum ripas *state,
					      phys_addr_t *top)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_RSI_IPA_STATE_GET,
		      start, end, 0, 0, 0, 0, 0,
		      &res);

	if (res.a0 == RSI_SUCCESS) {
		if (top)
			*top = res.a1;
		if (state)
			*state = res.a2;
	}

	return res.a0;
}

static inline long rsi_set_addr_range_state(phys_addr_t start,
					    phys_addr_t end,
					    enum ripas state,
					    unsigned long flags,
					    phys_addr_t *top)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_RSI_IPA_STATE_SET, start, end, state,
		      flags, 0, 0, 0, &res);

	if (top)
		*top = res.a1;

	if (res.a2 != RSI_ACCEPT)
		return -EPERM;

	return res.a0;
}

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

/**
 * rsi_measurement_extend - Extend the measurement value to the Realm Extensible
 * Measurement (REM).
 *
 * @idx:		Index of the REM register.
 *				Where:
 *				Index	Register
 *				1 - 4	REM[0-3]
 * @digest:		The digest data to be extended.
 * @digest_size:	Size of the digest data in bytes.
 *
 * Returns:
 *  On success, returns RSI_SUCCESS.
 *  Otherwise, -EINVAL
 */
static inline unsigned long rsi_measurement_extend(u32 idx,
						   const u8 *digest,
						   unsigned long digest_size)
{
	struct arm_smccc_1_2_regs regs = { 0 };

	/*
	 * Index 0 is for RIM (which is Read Only), while
	 * REM[0-3] are indexed from 1 - 4.
	 * The digest size can be at the most 64 bytes.
	 */
	if (!digest || idx < RSI_INDEX_REM0 || idx > RSI_INDEX_REM3 ||
	    digest_size == 0 || digest_size > RSI_MAX_MEASUREMENT_DATA_SIZE_BYTES)
		return -EINVAL;

	regs.a0 = SMC_RSI_MEASUREMENT_EXTEND;
	regs.a1 = idx;
	regs.a2 = digest_size;
	memcpy(&regs.a3, digest, digest_size);
	arm_smccc_1_2_smc(&regs, &regs);

	if (regs.a0 != RSI_SUCCESS)
		return -EINVAL;

	return regs.a0;
}

/**
 * rsi_measurement_read - Read the measurement value from the Realm Initial
 * Measurement (RIM) or the Realm Extensible Measurement (REM) register.
 *
 * @idx:		Index of the RIM or REM register.
 *				Where:
 *				Index	Register
 *				0	RIM
 *				1 - 4	REM[0-3]
 * @digest:			The digest data to be returned.
 * @digest_size:	Size of the digest data buffer in bytes.
 *
 * Returns:
 *  On success, returns RSI_SUCCESS.
 *  Otherwise, -EINVAL
 */
static inline unsigned long rsi_measurement_read(u32 idx,
						 u8 *digest,
						 unsigned long digest_size)
{
	struct arm_smccc_1_2_regs regs = { 0 };

	/*
	 * The digest size can be at the most 64 bytes, if less then 64 bytes
	 * it is zero padded.
	 */
	if (!digest || idx > RSI_INDEX_REM3 ||
	    digest_size == 0 || digest_size > RSI_MAX_MEASUREMENT_DATA_SIZE_BYTES)
		return -EINVAL;

	regs.a0 = SMC_RSI_MEASUREMENT_READ;
	regs.a1 = idx;
	arm_smccc_1_2_smc(&regs, &regs);

	if (regs.a0 != RSI_SUCCESS)
		return -EINVAL;

	memcpy(digest, &regs.a1, digest_size);
	return regs.a0;
}

#endif /* __ASM_RSI_CMDS_H */
