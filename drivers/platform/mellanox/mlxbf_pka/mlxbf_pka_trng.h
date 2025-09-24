/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
/* SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION. All rights reserved. */

#ifndef __MLXBF_PKA_TRNG_H__
#define __MLXBF_PKA_TRNG_H__

/*
 * The True Random Number Generator CSR addresses/offsets. These are accessed
 * from the ARM as 8 byte reads/writes. However only the bottom 32 bits are
 * implemented.
 */
#define MLXBF_PKA_TRNG_OUTPUT_0_ADDR	0x12000
#define MLXBF_PKA_TRNG_STATUS_ADDR	0x12020
#define MLXBF_PKA_TRNG_INTACK_ADDR	MLXBF_PKA_TRNG_STATUS_ADDR
#define MLXBF_PKA_TRNG_CONTROL_ADDR	0x12028
#define MLXBF_PKA_TRNG_CONFIG_ADDR	0x12030
#define MLXBF_PKA_TRNG_ALARMCNT_ADDR	0x12038
#define MLXBF_PKA_TRNG_FROENABLE_ADDR	0x12040
#define MLXBF_PKA_TRNG_FRODETUNE_ADDR	0x12048
#define MLXBF_PKA_TRNG_ALARMMASK_ADDR	0x12050
#define MLXBF_PKA_TRNG_ALARMSTOP_ADDR	0x12058
#define MLXBF_PKA_TRNG_TEST_ADDR	0x120E0
#define MLXBF_PKA_TRNG_RAW_L_ADDR	0x12060
#define MLXBF_PKA_TRNG_RAW_H_ADDR	0x12068
#define MLXBF_PKA_TRNG_MONOBITCNT_ADDR	0x120B8
#define MLXBF_PKA_TRNG_POKER_3_0_ADDR	0x120C0
#define MLXBF_PKA_TRNG_PS_AI_0_ADDR	0x12080

/*
 * 'trng_clk_on' mask for PKA Clock Switch Forcing Register. Turn on the TRNG
 * clock. When the TRNG is controlled via the host slave interface, this engine
 * needs to be turned on by setting bit 11.
 */
#define MLXBF_PKA_CLK_FORCE_TRNG_ON BIT(11)

/* Number of TRNG output registers. */
#define MLXBF_PKA_TRNG_OUTPUT_CNT 4

/* Number of TRNG poker test counts. */
#define MLXBF_PKA_TRNG_POKER_TEST_CNT 4

/* TRNG configuration. */
#define MLXBF_PKA_TRNG_CONFIG  0x00020008
/* TRNG Alarm Counter Register value. */
#define MLXBF_PKA_TRNG_ALARMCNT 0x000200ff
/* TRNG FRO Enable Register value. */
#define MLXBF_PKA_TRNG_FROENABLE 0x00ffffff
/*
 * TRNG Control Register value. Set bit 10 to start the EIP-76 (i.e. TRNG
 * engine), gathering entropy from the Free Running Oscillators (FROs).
 */
#define MLXBF_PKA_TRNG_CONTROL 0x00000400

/* TRNG Control bit. */
#define MLXBF_PKA_TRNG_CONTROL_TEST_MODE BIT(8)

/*
 * TRNG Control Register value. Set bit 10 and 12 to start the EIP-76 (i.e.
 * TRNG engine) with DRBG enabled, gathering entropy from the FROs.
 */
#define MLXBF_PKA_TRNG_CONTROL_DRBG 0x00001400

/*
 * DRBG enabled TRNG 'request_data' value. REQ_DATA (in accordance with
 * DATA_BLOCK_MASK) requests 256 blocks of 128-bit random output. 4095 blocks
 * is the maximum number that can be requested for the TRNG (with DRBG)
 * configuration on Bluefield platforms.
 */
#define MLXBF_PKA_TRNG_CONTROL_REQ_DATA 0x10010000

/* Mask for 'Data Block' in TRNG Control Register. */
#define MLXBF_PKA_TRNG_DRBG_DATA_BLOCK_MASK GENMASK(31, 20)

/* Set bit 12 of TRNG Control Register to enable DRBG functionality. */
#define MLXBF_PKA_TRNG_CONTROL_DRBG_ENABLE BIT(12)

/* Set bit 7 (i.e. 'test_sp_800_90 DRBG' bit) in the TRNG Test Register. */
#define MLXBF_PKA_TRNG_TEST_DRBG BIT(7)

/* Number of Personalization String/Additional Input Registers. */
#define MLXBF_PKA_TRNG_PS_AI_REG_COUNT 12

/* Offset bytes of Personalization String/Additional Input Registers. */
#define MLXBF_PKA_TRNG_OUTPUT_REG_OFFSET 0x8

/* Maximum TRNG test error cycle, about one second. */
#define MLXBF_PKA_TRNG_TEST_ERR_CYCLE_MAX (1000 * 1000 * 1000)

/* DRBG Reseed enable. */
#define MLXBF_PKA_TRNG_CONTROL_DRBG_RESEED BIT(15)

/* TRNG Status bits. */
#define MLXBF_PKA_TRNG_STATUS_READY		BIT(0)
#define MLXBF_PKA_TRNG_STATUS_SHUTDOWN_OFLO	BIT(1)
#define MLXBF_PKA_TRNG_STATUS_TEST_READY	BIT(8)
#define MLXBF_PKA_TRNG_STATUS_MONOBIT_FAIL	BIT(7)
#define MLXBF_PKA_TRNG_STATUS_RUN_FAIL		BIT(4)
#define MLXBF_PKA_TRNG_STATUS_POKER_FAIL	BIT(6)

#define MLXBF_PKA_TRNG_STATUS_FAIL_MODES (MLXBF_PKA_TRNG_STATUS_MONOBIT_FAIL | \
					 MLXBF_PKA_TRNG_STATUS_RUN_FAIL | \
					 MLXBF_PKA_TRNG_STATUS_POKER_FAIL)

/* TRNG Alarm Counter bits. */
#define MLXBF_PKA_TRNG_ALARMCNT_STALL_RUN_POKER BIT(15)

/* TRNG Test bits. */
#define MLXBF_PKA_TRNG_TEST_KNOWN_NOISE	BIT(5)
#define MLXBF_PKA_TRNG_TEST_NOISE	BIT(13)

/* TRNG Test constants*/
#define MLXBF_PKA_TRNG_MONOBITCNT_SUM	9978

#define MLXBF_PKA_TRNG_TEST_HALF_ADD	1
#define MLXBF_PKA_TRNG_TEST_HALF_NO	0

#define MLXBF_PKA_TRNG_TEST_DATAL_BASIC_1	0x11111333
#define MLXBF_PKA_TRNG_TEST_DATAH_BASIC_1	0x3555779f
#define MLXBF_PKA_TRNG_TEST_COUNT_BASIC_1	11

#define MLXBF_PKA_TRNG_TEST_DATAL_BASIC_2	0x01234567
#define MLXBF_PKA_TRNG_TEST_DATAH_BASIC_2	0x89abcdef
#define MLXBF_PKA_TRNG_TEST_COUNT_BASIC_2	302

#define MLXBF_PKA_TRNG_TEST_DATAL_POKER		0xffffffff
#define MLXBF_PKA_TRNG_TEST_DATAH_POKER		0xffffffff
#define MLXBF_PKA_TRNG_TEST_COUNT_POKER		11

#define MLXBF_PKA_TRNG_NUM_OF_FOUR_WORD		128

/* Defines for mlxbf_pka_dev_shim->trng_enabled. */
#define MLXBF_PKA_SHIM_TRNG_ENABLED	1
#define MLXBF_PKA_SHIM_TRNG_DISABLED	0

/* Configure the TRNG. */
int mlxbf_pka_dev_config_trng_drbg(struct device *dev,
				   struct mlxbf_pka_dev_res_t *aic_csr_ptr,
				   struct mlxbf_pka_dev_res_t *trng_csr_ptr);

/*
 * Read data from the TRNG. Drivers can fill up to 'cnt' bytes of data into the
 * buffer 'data'. The buffer 'data' is aligned for any type and 'cnt' is a
 * multiple of 4.
 */
int mlxbf_pka_dev_trng_read(struct device *dev,
			    struct mlxbf_pka_dev_shim_s *shim,
			    u32 *data, u32 cnt);

/* Return true if the TRNG engine is enabled, false if not. */
bool mlxbf_pka_dev_has_trng(struct mlxbf_pka_dev_shim_s *shim);

#endif /* __MLXBF_PKA_TRNG_H__ */
