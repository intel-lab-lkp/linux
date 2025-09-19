// SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause
// SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION. All rights reserved.

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/iopoll.h>

#include "mlxbf_pka_dev.h"
#include "mlxbf_pka_ring.h"
#include "mlxbf_pka_trng.h"

/* Personalization string "NVIDIA-MELLANOX-BLUEFIELD-TRUE_RANDOM_NUMBER_GEN". */
static u32 mlxbf_pka_trng_drbg_ps_str[] = {
	0x4e564944, 0x49412d4d, 0x454c4c41, 0x4e4f582d,
	0x424c5545, 0x4649454c, 0x442d5452, 0x55455f52,
	0x414e444f, 0x4d5f4e55, 0x4d424552, 0x5f47454e
};

/* Personalization string for DRBG test. */
static u32 mlxbf_pka_trng_drbg_test_ps_str[] = {
	0x64299d83, 0xc34d7098, 0x5bd1f51d, 0xddccfdc1,
	0xdd0455b7, 0x166279e5, 0x0974cb1b, 0x2f2cd100,
	0x59a5060a, 0xca79940d, 0xd4e29a40, 0x56b7b779
};

/* First Entropy string for DRBG test. */
static u32 mlxbf_pka_trng_drbg_test_etpy_str1[] = {
	0xaa6bbcab, 0xef45e339, 0x136ca1e7, 0xbce1c881,
	0x9fa37b09, 0x63b53667, 0xb36e0053, 0xa202ed81,
	0x4650d90d, 0x8eed6127, 0x666f2402, 0x0dfd3af9
};

/* Second Entropy string for DRBG test. */
static u32 mlxbf_pka_trng_drbg_test_etpy_str2[] = {
	0x35c1b7a1, 0x0154c52b, 0xd5777390, 0x226a4fdb,
	0x5f16080d, 0x06b68369, 0xd0c93d00, 0x3336e27f,
	0x1abf2c37, 0xe6ab006c, 0xa4adc6e1, 0x8e1907a2
};

/* Known answer for DRBG test. */
static u32 mlxbf_pka_trng_drbg_test_output[] = {
	0xb663b9f1, 0x24943e13, 0x80f7dce5, 0xaba1a16f
};

/* Known answer for poker test. */
static u64 poker_test_exp_cnt[] = {
	0x20f42bf4, 0xaf415f4, 0xf4f4fff4, 0xfff4f4f4
};

static int mlxbf_pka_dev_config_trng_clk(struct device *dev,
					 struct mlxbf_pka_dev_res_t *aic_csr_ptr)
{
	u32 trng_clk_en = 0;
	void __iomem *csr_reg_ptr;
	u64 csr_reg_base;
	u64 csr_reg_off;
	u64 timer;

	if (aic_csr_ptr->status != MLXBF_PKA_DEV_RES_STATUS_MAPPED ||
	    aic_csr_ptr->type != MLXBF_PKA_DEV_RES_TYPE_REG)
		return -EPERM;

	dev_dbg(dev, "turn on TRNG clock\n");

	csr_reg_base = aic_csr_ptr->base;
	csr_reg_ptr = aic_csr_ptr->ioaddr;

	/*
	 * Enable the TRNG clock in MLXBF_PKA_CLK_FORCE. In general, this
	 * register should be left in its default state of all zeroes. Only
	 * when the TRNG is directly controlled via the Host slave interface,
	 * the engine needs to be turned on using the 'trng_clk_on' bit in
	 * this register. In case the TRNG is controlled via internal firmware,
	 * this is not required.
	 */
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_CLK_FORCE_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, MLXBF_PKA_CLK_FORCE_TRNG_ON);
	/*
	 * Check whether the system clock for TRNG engine is enabled. The clock
	 * MUST be running to provide access to the TRNG.
	 */
	timer = mlxbf_pka_dev_timer_start_msec(100);
	while (!trng_clk_en) {
		trng_clk_en |= mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off)
						     & MLXBF_PKA_CLK_FORCE_TRNG_ON;
		if (mlxbf_pka_dev_timer_done(timer)) {
			dev_dbg(dev, "failed to enable TRNG clock\n");
			return -ETIMEDOUT;
		}
	}
	dev_dbg(dev, "trng_clk_on is enabled\n");

	return 0;
}

static bool mlxbf_pka_dev_trng_wait_test_ready(struct device *dev,
					       void __iomem *csr_reg_ptr,
					       u64 csr_reg_base)
{
	u64 csr_reg_off, timer, csr_reg_val, test_ready = 0;

	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_STATUS_ADDR);
	timer = mlxbf_pka_dev_timer_start_msec(MSEC_PER_SEC);

	while (!test_ready) {
		csr_reg_val = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);
		test_ready = csr_reg_val & MLXBF_PKA_TRNG_STATUS_TEST_READY;

		if (mlxbf_pka_dev_timer_done(timer)) {
			dev_dbg(dev, "TRNG test ready timer done, 0x%llx\n", csr_reg_val);
			return false;
		}
	}

	return true;
}

static int mlxbf_pka_dev_trng_enable_test(struct device *dev,
					  void __iomem *csr_reg_ptr,
					  u64 csr_reg_base, u32 test)
{
	u64 csr_reg_val, csr_reg_off;
	int ret;

	/*
	 * Set the 'test_mode' bit in the TRNG_CONTROL register and the
	 * 'test_known_noise' bit in the TRNG_TEST register - this will
	 * immediately set the 'test_ready' bit (in the TRNG_STATUS register)
	 * to indicate that data can be written. It will also reset the
	 * 'monobit test', 'run test' and 'poker test' circuits to their
	 * initial states. Note that the TRNG need not be enabled for this
	 * test.
	 */
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_CONTROL_ADDR);
	csr_reg_val = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_CONTROL_ADDR);

	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off,
			       csr_reg_val | MLXBF_PKA_TRNG_CONTROL_TEST_MODE);
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_TEST_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, test);
	/* Wait until the 'test_ready' bit is set. */
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_STATUS_ADDR);
	ret = read_poll_timeout(mlxbf_pka_dev_io_read, csr_reg_val,
				csr_reg_val & MLXBF_PKA_TRNG_STATUS_TEST_READY,
				USEC_PER_MSEC / 100, USEC_PER_SEC, false,
				csr_reg_ptr, csr_reg_off);
	if (ret) {
		dev_err(dev, "timeout waiting for test ready\n");
		return -ETIMEDOUT;
	}

	/*
	 * Check whether the 'monobit test', 'run test' and 'poker test'
	 * are reset.
	 */
	if (csr_reg_val & MLXBF_PKA_TRNG_STATUS_FAIL_MODES) {
		dev_err(dev, "test bits aren't reset, TRNG_STATUS:0x%llx\n", csr_reg_val);
		return -EIO;
	}

	/*
	 * Set 'stall_run_poker' bit to allow inspecting the state of the
	 * result counters which would otherwise be reset immediately for the
	 * next 20,000 bits block to test.
	 */
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_ALARMCNT_ADDR);
	csr_reg_val = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);
	mlxbf_pka_dev_io_write(csr_reg_ptr,
			       csr_reg_off,
			       csr_reg_val | MLXBF_PKA_TRNG_ALARMCNT_STALL_RUN_POKER);

	return 0;
}

static int mlxbf_pka_dev_trng_test_circuits(struct device *dev,
					    void __iomem *csr_reg_ptr,
					    u64 csr_reg_base,
					    u64 datal, u64 datah,
					    int count, u8 add_half,
					    u64 *monobit_fail_cnt,
					    u64 *run_fail_cnt,
					    u64 *poker_fail_cnt)
{
	u64 status, csr_reg_off;
	unsigned int test_idx;
	int ret;

	if (!monobit_fail_cnt || !run_fail_cnt || !poker_fail_cnt)
		return -EINVAL;

	for (test_idx = 0; test_idx < count; test_idx++) {
		csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base,
								MLXBF_PKA_TRNG_RAW_L_ADDR);
		mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, datal);

		if (!add_half || test_idx < count - 1) {
			csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base,
									MLXBF_PKA_TRNG_RAW_H_ADDR);
			mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, datah);
		}

		/*
		 * Wait until the 'test_ready' bit in the TRNG_STATUS register
		 * becomes '1' again, signalling readiness for the next 64 bits
		 * of test data. At this point, the previous test data has been
		 * handled so the counter states can be inspected.
		 */
		csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base,
								MLXBF_PKA_TRNG_STATUS_ADDR);
		ret = read_poll_timeout(mlxbf_pka_dev_io_read, status,
					status & MLXBF_PKA_TRNG_STATUS_TEST_READY,
					USEC_PER_MSEC / 100, USEC_PER_SEC, false,
					csr_reg_ptr, csr_reg_off);
		if (ret) {
			dev_err(dev, "timeout waiting for test ready in circuits\n");
			return -ETIMEDOUT;
		}

		/* Check test status bits. */
		csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base,
								MLXBF_PKA_TRNG_INTACK_ADDR);
		if (status & MLXBF_PKA_TRNG_STATUS_MONOBIT_FAIL) {
			mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off,
					       MLXBF_PKA_TRNG_STATUS_MONOBIT_FAIL);
			*monobit_fail_cnt += 1;
		} else if (status & MLXBF_PKA_TRNG_STATUS_RUN_FAIL) {
			mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off,
					       MLXBF_PKA_TRNG_STATUS_RUN_FAIL);
			*run_fail_cnt += 1;
		} else if (status & MLXBF_PKA_TRNG_STATUS_POKER_FAIL) {
			mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off,
					       MLXBF_PKA_TRNG_STATUS_POKER_FAIL);
			*poker_fail_cnt += 1;
		}
	}

	return *monobit_fail_cnt || *poker_fail_cnt || *run_fail_cnt ? -EIO : 0;
}

static void mlxbf_pka_dev_trng_disable_test(struct device *dev,
					    void __iomem *csr_reg_ptr,
					    u64 csr_reg_base)
{
	u64 status, val, csr_reg_off;

	/*
	 * When done, clear the 'test_known_noise' bit in the TRNG_TEST
	 * register (will immediately clear the 'test_ready' bit in the
	 * TRNG_STATUS register and reset the 'monobit test', 'run test'
	 * and 'poker test' circuits) and clear the 'test_mode' bit in the
	 * TRNG_CONTROL register.
	 */
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_TEST_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);

	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_STATUS_ADDR);
	status = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);

	if (status & MLXBF_PKA_TRNG_STATUS_TEST_READY)
		dev_info(dev, "test ready bit is still set\n");

	if (status & MLXBF_PKA_TRNG_STATUS_FAIL_MODES)
		dev_info(dev, "test bits are still set, TRNG_STATUS:0x%llx\n", status);

	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_CONTROL_ADDR);
	val = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, val & ~MLXBF_PKA_TRNG_STATUS_TEST_READY);

	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_ALARMCNT_ADDR);
	val = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);
	mlxbf_pka_dev_io_write(csr_reg_ptr,
			       csr_reg_off,
			       val & ~MLXBF_PKA_TRNG_ALARMCNT_STALL_RUN_POKER);
}

static int mlxbf_pka_dev_trng_test_known_answer_basic(struct device *dev,
						      void __iomem *csr_reg_ptr,
						      u64 csr_reg_base)
{
	u64 poker_cnt[MLXBF_PKA_TRNG_POKER_TEST_CNT];
	u64 monobit_fail_cnt = 0;
	u64 poker_fail_cnt = 0;
	u64 run_fail_cnt = 0;
	u64 monobit_cnt;
	u64 csr_reg_off;
	int cnt_idx;
	int cnt_off;
	int ret;

	dev_dbg(dev, "run known-answer test circuits\n");

	ret = mlxbf_pka_dev_trng_enable_test(dev, csr_reg_ptr, csr_reg_base,
					     MLXBF_PKA_TRNG_TEST_KNOWN_NOISE);
	if (ret)
		return ret;

	ret = mlxbf_pka_dev_trng_test_circuits(dev,
					       csr_reg_ptr,
					       csr_reg_base,
					       MLXBF_PKA_TRNG_TEST_DATAL_BASIC_1,
					       MLXBF_PKA_TRNG_TEST_DATAH_BASIC_1,
					       MLXBF_PKA_TRNG_TEST_COUNT_BASIC_1,
					       MLXBF_PKA_TRNG_TEST_HALF_NO,
					       &monobit_fail_cnt,
					       &run_fail_cnt,
					       &poker_fail_cnt);

	ret |= mlxbf_pka_dev_trng_test_circuits(dev,
						csr_reg_ptr,
						csr_reg_base,
						MLXBF_PKA_TRNG_TEST_DATAL_BASIC_2,
						MLXBF_PKA_TRNG_TEST_DATAH_BASIC_2,
						MLXBF_PKA_TRNG_TEST_COUNT_BASIC_2,
						MLXBF_PKA_TRNG_TEST_HALF_ADD,
						&monobit_fail_cnt,
						&run_fail_cnt,
						&poker_fail_cnt);

	dev_dbg(dev, "monobit_fail_cnt : 0x%llx\n", monobit_fail_cnt);
	dev_dbg(dev, "poker_fail_cnt   : 0x%llx\n", poker_fail_cnt);
	dev_dbg(dev, "run_fail_cnt     : 0x%llx\n", run_fail_cnt);

	for (cnt_idx = 0, cnt_off = 0;
	     cnt_idx < MLXBF_PKA_TRNG_POKER_TEST_CNT;
	     cnt_idx++, cnt_off += 8) {
		csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base,
								MLXBF_PKA_TRNG_POKER_3_0_ADDR +
								cnt_off);
		poker_cnt[cnt_idx] = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);
	}

	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base,
							MLXBF_PKA_TRNG_MONOBITCNT_ADDR);
	monobit_cnt = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);

	if (ret)
		goto exit;

	if (memcmp(poker_cnt, poker_test_exp_cnt, sizeof(poker_test_exp_cnt))) {
		dev_dbg(dev, "invalid poker counters!\n");
		ret = -EIO;
		goto exit;
	}

	if (monobit_cnt != MLXBF_PKA_TRNG_MONOBITCNT_SUM) {
		dev_dbg(dev, "invalid sum of squares!\n");
		ret = -EIO;
		goto exit;
	}

exit:
	mlxbf_pka_dev_trng_disable_test(dev, csr_reg_ptr, csr_reg_base);
	return ret;
}

static int mlxbf_pka_dev_trng_test_known_answer_poker_fail(struct device *dev,
							   void __iomem *csr_reg_ptr,
							   u64 csr_reg_base)
{
	u64 monobit_fail_cnt = 0;
	u64 poker_fail_cnt = 0;
	u64 run_fail_cnt = 0;

	dev_dbg(dev, "run known-answer test circuits (poker fail)\n");

	mlxbf_pka_dev_trng_enable_test(dev, csr_reg_ptr, csr_reg_base,
				       MLXBF_PKA_TRNG_TEST_KNOWN_NOISE);

	/*
	 * Ignore the return value here as it is expected that poker test
	 * should fail. Check failure counts thereafter to assert only poker
	 * test has failed.
	 */
	mlxbf_pka_dev_trng_test_circuits(dev,
					 csr_reg_ptr,
					 csr_reg_base,
					 MLXBF_PKA_TRNG_TEST_DATAL_POKER,
					 MLXBF_PKA_TRNG_TEST_DATAH_POKER,
					 MLXBF_PKA_TRNG_TEST_COUNT_POKER,
					 MLXBF_PKA_TRNG_TEST_HALF_NO,
					 &monobit_fail_cnt,
					 &run_fail_cnt,
					 &poker_fail_cnt);

	dev_dbg(dev, "monobit_fail_cnt : 0x%llx\n", monobit_fail_cnt);
	dev_dbg(dev, "poker_fail_cnt   : 0x%llx\n", poker_fail_cnt);
	dev_dbg(dev, "run_fail_cnt     : 0x%llx\n", run_fail_cnt);

	mlxbf_pka_dev_trng_disable_test(dev, csr_reg_ptr, csr_reg_base);

	return poker_fail_cnt && !run_fail_cnt && !monobit_fail_cnt ? 0 : -EIO;
}

static int mlxbf_pka_dev_trng_test_unknown_answer(struct device *dev,
						  void __iomem *csr_reg_ptr,
						  u64 csr_reg_base)
{
	u64 datal = 0, datah = 0, csr_reg_off;
	int ret = 0, test_idx;

	dev_dbg(dev, "run unknown-answer self test\n");

	/* First reset, the RAW registers. */
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_RAW_L_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);

	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_RAW_H_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);

	/*
	 * There is a small probability for this test to fail. So run the test
	 * 10 times, if it succeeds once then assume that the test passed.
	 */
	for (test_idx = 0; test_idx < 10; test_idx++) {
		mlxbf_pka_dev_trng_enable_test(dev, csr_reg_ptr, csr_reg_base,
					       MLXBF_PKA_TRNG_TEST_NOISE);

		csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base,
								MLXBF_PKA_TRNG_RAW_L_ADDR);
		datal = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);

		csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base,
								MLXBF_PKA_TRNG_RAW_H_ADDR);
		datah = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);

		dev_dbg(dev, "datal=0x%llx\n", datal);
		dev_dbg(dev, "datah=0x%llx\n", datah);

		mlxbf_pka_dev_trng_disable_test(dev, csr_reg_ptr, csr_reg_base);

		if (!datah && !datal)
			ret = -EIO;
		else
			return 0;
	}
	return ret;
}

/* Test TRNG. */
static int mlxbf_pka_dev_test_trng(struct device *dev, void __iomem *csr_reg_ptr, u64 csr_reg_base)
{
	int ret;

	ret = mlxbf_pka_dev_trng_test_known_answer_basic(dev, csr_reg_ptr, csr_reg_base);
	if (ret)
		return ret;

	ret = mlxbf_pka_dev_trng_test_known_answer_poker_fail(dev, csr_reg_ptr, csr_reg_base);
	if (ret)
		return ret;

	ret = mlxbf_pka_dev_trng_test_unknown_answer(dev, csr_reg_ptr, csr_reg_base);
	if (ret)
		return ret;

	return ret;
}

static void mlxbf_pka_dev_trng_write_ps_ai_str(void __iomem *csr_reg_ptr,
					       u64 csr_reg_base,
					       u32 input_str[])
{
	u64 csr_reg_off;
	u8 i;

	for (i = 0; i < MLXBF_PKA_TRNG_PS_AI_REG_COUNT; i++) {
		csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base,
								MLXBF_PKA_TRNG_PS_AI_0_ADDR + i *
								MLXBF_PKA_TRNG_OUTPUT_REG_OFFSET);

		mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, input_str[i]);
	}
}

static void mlxbf_pka_dev_trng_drbg_generate(void __iomem *csr_reg_ptr, u64 csr_reg_base)
{
	u64 csr_reg_off;

	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_CONTROL_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, MLXBF_PKA_TRNG_CONTROL_REQ_DATA);
}

static int mlxbf_pka_dev_test_trng_drbg(struct device *dev,
					void __iomem *csr_reg_ptr,
					u64 csr_reg_base)
{
	u64 csr_reg_off, csr_reg_val;
	u8 i;

	/* Make sure the engine is idle. */
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_CONTROL_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);

	/* Enable DRBG, TRNG need not be enabled for this test. */
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_CONTROL_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, MLXBF_PKA_TRNG_CONTROL_DRBG_ENABLE);

	/* Set 'test_sp_800_90' bit in the TRNG_TEST register. */
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_TEST_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, MLXBF_PKA_TRNG_TEST_DRBG);

	/* Wait for 'test_ready' bit to be set. */
	if (!mlxbf_pka_dev_trng_wait_test_ready(dev, csr_reg_ptr, csr_reg_base))
		return -ETIMEDOUT;

	/* 'Instantiate' function. */
	mlxbf_pka_dev_trng_write_ps_ai_str(csr_reg_ptr,
					   csr_reg_base,
					   mlxbf_pka_trng_drbg_test_ps_str);
	if (!mlxbf_pka_dev_trng_wait_test_ready(dev, csr_reg_ptr, csr_reg_base))
		return -ETIMEDOUT;

	/* 'Generate' function. */
	mlxbf_pka_dev_trng_write_ps_ai_str(csr_reg_ptr,
					   csr_reg_base,
					   mlxbf_pka_trng_drbg_test_etpy_str1);
	if (!mlxbf_pka_dev_trng_wait_test_ready(dev, csr_reg_ptr, csr_reg_base))
		return -ETIMEDOUT;

	/*
	 * A standard NIST SP 800-90A DRBG known-answer test discards the
	 * result of the first 'Generate' function and only checks the result
	 * of the second 'Generate' function. Hence 'Generate' is performed
	 * again.
	 */

	/* 'Generate' function. */
	mlxbf_pka_dev_trng_write_ps_ai_str(csr_reg_ptr,
					   csr_reg_base,
					   mlxbf_pka_trng_drbg_test_etpy_str2);
	if (!mlxbf_pka_dev_trng_wait_test_ready(dev, csr_reg_ptr, csr_reg_base))
		return -ETIMEDOUT;

	/* Check output registers. */
	for (i = 0; i < MLXBF_PKA_TRNG_OUTPUT_CNT; i++) {
		csr_reg_off =
		mlxbf_pka_dev_get_register_offset(csr_reg_base,
						  MLXBF_PKA_TRNG_OUTPUT_0_ADDR +
						  (i * MLXBF_PKA_TRNG_OUTPUT_REG_OFFSET));

		csr_reg_val = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);

		if ((u32)csr_reg_val != mlxbf_pka_trng_drbg_test_output[i]) {
			dev_dbg(dev, "DRBG known answer test failed: output register:%d, 0x%x\n",
				i, (u32)csr_reg_val);
			return -EIO;
		}
	}

	/* Clear 'test_sp_800_90' bit in the TRNG_TEST register. */
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_TEST_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);

	return 0;
}

static bool mlxbf_pka_dev_trng_shutdown_oflo(struct mlxbf_pka_dev_res_t *trng_csr_ptr,
					     u64 *err_cycle)
{
	u64 curr_cycle_cnt, fro_stopped_mask, fro_enabled_mask;
	u64 csr_reg_base, csr_reg_off, csr_reg_value;
	void __iomem *csr_reg_ptr;

	csr_reg_base = trng_csr_ptr->base;
	csr_reg_ptr = trng_csr_ptr->ioaddr;

	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_STATUS_ADDR);
	csr_reg_value = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);

	if (csr_reg_value & MLXBF_PKA_TRNG_STATUS_SHUTDOWN_OFLO) {
		curr_cycle_cnt = get_cycles();
		/*
		 * See if any FROs were shut down. If they were, toggle bits in
		 * the FRO detune register and reenable the FROs.
		 */
		csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base,
								MLXBF_PKA_TRNG_ALARMSTOP_ADDR);
		fro_stopped_mask = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);
		if (fro_stopped_mask) {
			csr_reg_off =
			mlxbf_pka_dev_get_register_offset(csr_reg_base,
							  MLXBF_PKA_TRNG_FROENABLE_ADDR);
			fro_enabled_mask = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);
			csr_reg_off =
			mlxbf_pka_dev_get_register_offset(csr_reg_base,
							  MLXBF_PKA_TRNG_FRODETUNE_ADDR);
			mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, fro_stopped_mask);

			csr_reg_off =
			mlxbf_pka_dev_get_register_offset(csr_reg_base,
							  MLXBF_PKA_TRNG_FROENABLE_ADDR);
			mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off,
					       fro_stopped_mask | fro_enabled_mask);
		}

		/* Reset the error. */
		csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base,
								MLXBF_PKA_TRNG_ALARMMASK_ADDR);
		mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);

		csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base,
								MLXBF_PKA_TRNG_ALARMSTOP_ADDR);
		mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);

		csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base,
								MLXBF_PKA_TRNG_INTACK_ADDR);
		mlxbf_pka_dev_io_write(csr_reg_ptr,
				       csr_reg_off,
				       MLXBF_PKA_TRNG_STATUS_SHUTDOWN_OFLO);

		/*
		 * If this error occurs again within about a second, the hardware
		 * is malfunctioning. Disable the trng and return an error.
		 */
		if (*err_cycle &&
		    (curr_cycle_cnt - *err_cycle < MLXBF_PKA_TRNG_TEST_ERR_CYCLE_MAX)) {
			csr_reg_off =
			mlxbf_pka_dev_get_register_offset(csr_reg_base,
							  MLXBF_PKA_TRNG_CONTROL_ADDR);
			csr_reg_value  = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);
			csr_reg_value &= ~MLXBF_PKA_TRNG_CONTROL;
			mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, csr_reg_value);
			return false;
		}

		*err_cycle = curr_cycle_cnt;
	}

	return true;
}

static int mlxbf_pka_dev_trng_drbg_reseed(struct device *dev,
					  void __iomem *csr_reg_ptr,
					  u64 csr_reg_base)
{
	u64 csr_reg_off;

	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_CONTROL_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, MLXBF_PKA_TRNG_CONTROL_DRBG_RESEED);

	if (!mlxbf_pka_dev_trng_wait_test_ready(dev, csr_reg_ptr, csr_reg_base))
		return -ETIMEDOUT;

	/* Write personalization string. */
	mlxbf_pka_dev_trng_write_ps_ai_str(csr_reg_ptr, csr_reg_base, mlxbf_pka_trng_drbg_ps_str);

	return 0;
}

/* Configure the TRNG. */
int mlxbf_pka_dev_config_trng_drbg(struct device *dev,
				   struct mlxbf_pka_dev_res_t *aic_csr_ptr,
				   struct mlxbf_pka_dev_res_t *trng_csr_ptr)
{
	u64 csr_reg_base, csr_reg_off;
	void __iomem *csr_reg_ptr;
	int ret;

	if (trng_csr_ptr->status != MLXBF_PKA_DEV_RES_STATUS_MAPPED ||
	    trng_csr_ptr->type != MLXBF_PKA_DEV_RES_TYPE_REG)
		return -EPERM;

	dev_dbg(dev, "starting up the TRNG\n");

	ret = mlxbf_pka_dev_config_trng_clk(dev, aic_csr_ptr);
	if (ret)
		return ret;

	csr_reg_base = trng_csr_ptr->base;
	csr_reg_ptr = trng_csr_ptr->ioaddr;

	/*
	 * Perform NIST known-answer tests on the complete SP 800-90A DRBG
	 * without BC_DF functionality.
	 */
	ret = mlxbf_pka_dev_test_trng_drbg(dev, csr_reg_ptr, csr_reg_base);
	if (ret)
		return ret;

	/* Starting up the TRNG with a DRBG. */

	/* Make sure the engine is idle. */
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_CONTROL_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);

	/* Disable all FROs initially. */
	csr_reg_off =
	mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_FROENABLE_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);
	csr_reg_off =
	mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_FRODETUNE_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);

	/*
	 * Write all configuration values in the TRNG_CONFIG and TRNG_ALARMCNT,
	 * write zeroes to the TRNG_ALARMMASK and TRNG_ALARMSTOP registers.
	 */
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_CONFIG_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, MLXBF_PKA_TRNG_CONFIG);
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_ALARMCNT_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, MLXBF_PKA_TRNG_ALARMCNT);

	csr_reg_off =
	mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_ALARMMASK_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);
	csr_reg_off =
	mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_ALARMSTOP_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);

	/*
	 * Enable all FROs in the TRNG_FROENABLE register. Note that this can
	 * only be done after clearing the TRNG_ALARMSTOP register.
	 */
	csr_reg_off =
	mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_FROENABLE_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, MLXBF_PKA_TRNG_FROENABLE);

	/*
	 * Optionally, write 'Personalization string' of up to 384 bits in
	 * TRNG_PS_AI_xxx registers.
	 * The contents of these registers will be XOR-ed into the output of the
	 * SHA-256 'Conditioning Function' to be used as seed value for the
	 * actual DRBG.
	 */
	mlxbf_pka_dev_trng_write_ps_ai_str(csr_reg_ptr, csr_reg_base, mlxbf_pka_trng_drbg_ps_str);

	/*
	 * Run TRNG tests after configuring TRNG.
	 * NOTE: TRNG need not be enabled to carry out these tests.
	 */
	ret = mlxbf_pka_dev_test_trng(dev, csr_reg_ptr, csr_reg_base);
	if (ret)
		return ret;

	/*
	 * Start the actual engine by setting the 'enable_trng' and 'drbg_en'
	 * bit in the TRNG_CONTROL register (also a nice point to set the
	 * interrupt mask bits).
	 */
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_CONTROL_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, MLXBF_PKA_TRNG_CONTROL_DRBG);

	/*
	 * The engine is now ready to handle the first 'Generate' request using
	 * the 'request_data' bit of the TRNG_CONTROL register. The first output
	 * for these requests will take a while, as Noise Source and
	 * Conditioning Function must first generate seed entropy for the DRBG.
	 *
	 * Optionally, when buffer RAM is configured: Set a data available
	 * interrupt threshold using the 'load_thresh' and 'blocks_thresh'
	 * fields of the TRNG_INTACK register. This allows delaying the data
	 * available interrupt until the indicated number of 128-bit words a
	 * available in the buffer RAM.
	 *
	 * Start the actual 'Generate' operation using the 'request_data' and
	 * 'data_blocks' fields of the TRNG_CONTROL register.
	 */
	mlxbf_pka_dev_trng_drbg_generate(csr_reg_ptr, csr_reg_base);

	/* Delay 200 ms. */
	mdelay(200);

	return 0;
}

/* Read from DRBG enabled TRNG. */
int mlxbf_pka_dev_trng_read(struct device *dev,
			    struct mlxbf_pka_dev_shim_s *shim,
			    u32 *data, u32 cnt)
{
	u64 csr_reg_base, csr_reg_off, csr_reg_value, timer;
	struct mlxbf_pka_dev_res_t *trng_csr_ptr;
	u8 output_idx, trng_ready = 0;
	u32 data_idx, word_cnt;
	void __iomem *csr_reg_ptr;
	int ret = 0;

	if (!shim || !data || (cnt % MLXBF_PKA_TRNG_OUTPUT_CNT != 0))
		return -EINVAL;

	if (!cnt)
		return ret;

	guard(mutex)(&shim->mutex);

	trng_csr_ptr = &shim->resources.trng_csr;

	if (trng_csr_ptr->status != MLXBF_PKA_DEV_RES_STATUS_MAPPED ||
	    trng_csr_ptr->type != MLXBF_PKA_DEV_RES_TYPE_REG)
		return -EPERM;

	csr_reg_base = trng_csr_ptr->base;
	csr_reg_ptr = trng_csr_ptr->ioaddr;

	if (!mlxbf_pka_dev_trng_shutdown_oflo(trng_csr_ptr, &shim->trng_err_cycle))
		return -EWOULDBLOCK;

	word_cnt = cnt >> ilog2(sizeof(u32));

	for (data_idx = 0; data_idx < word_cnt; data_idx++) {
		output_idx = data_idx % MLXBF_PKA_TRNG_OUTPUT_CNT;

		/* Tell the hardware to advance. */
		if (!output_idx) {
			csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base,
									MLXBF_PKA_TRNG_INTACK_ADDR);
			mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off,
					       MLXBF_PKA_TRNG_STATUS_READY);
			trng_ready = 0;

			/*
			 * Check if 'data_blocks' field is zero in TRNG_CONTROL
			 * register. If it is zero, need to issue a 'Reseed and
			 * Generate' request for DRBG enabled TRNG.
			 */
			csr_reg_off =
			mlxbf_pka_dev_get_register_offset(csr_reg_base,
							  MLXBF_PKA_TRNG_CONTROL_ADDR);
			csr_reg_value = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);

			if (!((u32)csr_reg_value & MLXBF_PKA_TRNG_DRBG_DATA_BLOCK_MASK)) {
				/* Issue reseed. */
				ret = mlxbf_pka_dev_trng_drbg_reseed(dev,
								     csr_reg_ptr,
								     csr_reg_base);
				if (ret)
					return -EBUSY;

				/* Issue generate request. */
				mlxbf_pka_dev_trng_drbg_generate(csr_reg_ptr, csr_reg_base);
			}
		}

		/*
		 * Wait until a data word is available in the TRNG_OUTPUT_X
		 * registers, using the interrupt and/or 'ready' status bit
		 * in the TRNG_STATUS register. The only way this would hang
		 * is if the TRNG is never initialized. This function cannot
		 * be called if that happened.
		 */
		timer = mlxbf_pka_dev_timer_start_msec(1000);
		csr_reg_off =
		mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_STATUS_ADDR);
		while (!trng_ready) {
			csr_reg_value = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);
			trng_ready = csr_reg_value & MLXBF_PKA_TRNG_STATUS_READY;

			if (mlxbf_pka_dev_timer_done(timer)) {
				dev_dbg(dev, "shim %u got error obtaining random number\n",
					shim->shim_id);
				return -EBUSY;
			}
		}

		/* Read the registers. */
		csr_reg_off =
		mlxbf_pka_dev_get_register_offset(csr_reg_base,
						  MLXBF_PKA_TRNG_OUTPUT_0_ADDR +
						  (output_idx * MLXBF_PKA_TRNG_OUTPUT_REG_OFFSET));
		csr_reg_value = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);
		data[data_idx] = (u32)csr_reg_value;
	}

	return ret;
}

bool mlxbf_pka_dev_has_trng(struct mlxbf_pka_dev_shim_s *shim)
{
	if (!shim)
		return false;

	return shim->trng_enabled == MLXBF_PKA_SHIM_TRNG_ENABLED;
}
