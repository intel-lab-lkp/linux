/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef QCOM_JENC_RES_H
#define QCOM_JENC_RES_H

#include "qcom_jenc_defs.h"

/*
 * clk_rate == 0 means: do not change this clock rate.
 * Clock is still enabled/disabled normally.
 */
enum qcom_jpeg_clock_ids {
	JPEG_CAMNOC_AXI_CLK,
	JPEG_CORE_CLK,
	JPEG_CORE_AHB_CLK,
	JPEG_CPAS_AHB_CLK,
	JPEG_GCC_HF_AXI,
	JPEG_GCC_SF_AXI,
	JPEG_MAX_CLOCKS
};

struct qcom_icc_resource {
	const char *icc_id;
	struct {
		u32 aggr;
		u32 peak;
	} pair;
};

struct qcom_perf_resource {
	u64 clk_rate[JPEG_MAX_CLOCKS];
};

struct qcom_dev_resources {
	const struct qcom_jpeg_hw_ops	*hw_ops;
	const struct qcom_jpeg_reg_offs	*hw_offs;
	const u32			*hw_mask;

	const struct qcom_icc_resource	*icc_res;
	unsigned int			num_of_icc;
	const struct qcom_perf_resource	*perf_cfg;
	const char			*clk_names[JPEG_MAX_CLOCKS];
};

extern const struct qcom_dev_resources qcom_t165_t480_jpeg_drvdata;

extern const struct qcom_dev_resources qcom_t680_jpeg_drvdata;

extern const struct qcom_dev_resources qcom_t780_jpeg_drvdata;

#endif	/* QCOM_JENC_RES_H */
