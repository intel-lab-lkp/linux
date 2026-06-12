// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/clk.h>

#include "qcom_jenc_ops.h"
#include "qcom_jenc_res.h"

#include "qcom_jenc_v420_hw_info.h"

#define QCOM_PERF_ROW(_axi_rate, _jpeg_rate) \
{ \
	.clk_rate = { \
		[JPEG_CAMNOC_AXI_CLK]	= (_axi_rate),	\
		[JPEG_CORE_CLK]		= (_jpeg_rate),	\
	}, \
}

/*
 * Baseline AXI clock rates shared across t165/t480, reused by later
 * derivatives where the hardware does not change these domains.
 */
static const u64 cnoc_axi_clk_t165_t480[] = {
	[QCOM_SOC_PERF_SUSPEND]	=  19200000,
	[QCOM_SOC_PERF_LOWSVS]	= 300000000,
	[QCOM_SOC_PERF_SVS]	= 300000000,
	[QCOM_SOC_PERF_SVS_L1]	= 300000000,
	[QCOM_SOC_PERF_NOMINAL]	= 400000000,
	[QCOM_SOC_PERF_TURBO]	= 400000000,
};

/*
 * Derivative with an improved CAMNOC AXI frequency range
 */
static const u64 cnoc_axi_clk_t680[] = {
	[QCOM_SOC_PERF_SUSPEND]	=  19200000,
	[QCOM_SOC_PERF_LOWSVS]	= 150000000,
	[QCOM_SOC_PERF_SVS]	= 240000000,
	[QCOM_SOC_PERF_SVS_L1]	= 320000000,
	[QCOM_SOC_PERF_NOMINAL]	= 400000000,
	[QCOM_SOC_PERF_TURBO]	= 480000000,
};

/*
 * Baseline JPEG clock rates shared across t165/t480, reused by later
 * derivatives where the hardware does not change these domains.
 */
static const u64 qcom_jpeg_clk_t165_t480[] = {
	[QCOM_SOC_PERF_SUSPEND]	=  19200000,
	[QCOM_SOC_PERF_LOWSVS]	= 300000000,
	[QCOM_SOC_PERF_SVS]	= 400000000,
	[QCOM_SOC_PERF_SVS_L1]	= 480000000,
	[QCOM_SOC_PERF_NOMINAL]	= 600000000,
	[QCOM_SOC_PERF_TURBO]	= 600000000,
};

/*
 * Derivative with an improved maximum JPEG frequency
 */
static const u64 qcom_jpeg_clk_t780[] = {
	[QCOM_SOC_PERF_SUSPEND]	=  19200000,
	[QCOM_SOC_PERF_LOWSVS]	= 200000000,
	[QCOM_SOC_PERF_SVS]	= 200000000,
	[QCOM_SOC_PERF_SVS_L1]	= 400000000,
	[QCOM_SOC_PERF_NOMINAL]	= 480000000,
	[QCOM_SOC_PERF_TURBO]	= 785000000,
};

static const struct qcom_perf_resource qcom_perf_rates_t165_t480[] = {
	[QCOM_SOC_PERF_SUSPEND]	=
		QCOM_PERF_ROW(cnoc_axi_clk_t165_t480[QCOM_SOC_PERF_SUSPEND],
			      qcom_jpeg_clk_t165_t480[QCOM_SOC_PERF_SUSPEND]),

	[QCOM_SOC_PERF_LOWSVS]		=
		QCOM_PERF_ROW(cnoc_axi_clk_t165_t480[QCOM_SOC_PERF_LOWSVS],
			      qcom_jpeg_clk_t165_t480[QCOM_SOC_PERF_LOWSVS]),

	[QCOM_SOC_PERF_SVS]		=
		QCOM_PERF_ROW(cnoc_axi_clk_t165_t480[QCOM_SOC_PERF_SVS],
			      qcom_jpeg_clk_t165_t480[QCOM_SOC_PERF_SVS]),

	[QCOM_SOC_PERF_SVS_L1]		=
		QCOM_PERF_ROW(cnoc_axi_clk_t165_t480[QCOM_SOC_PERF_SVS_L1],
			      qcom_jpeg_clk_t165_t480[QCOM_SOC_PERF_SVS_L1]),

	[QCOM_SOC_PERF_NOMINAL]	=
		QCOM_PERF_ROW(cnoc_axi_clk_t165_t480[QCOM_SOC_PERF_NOMINAL],
			      qcom_jpeg_clk_t165_t480[QCOM_SOC_PERF_NOMINAL]),

	[QCOM_SOC_PERF_TURBO]		=
		QCOM_PERF_ROW(cnoc_axi_clk_t165_t480[QCOM_SOC_PERF_TURBO],
			      qcom_jpeg_clk_t165_t480[QCOM_SOC_PERF_TURBO]),
};

static const struct qcom_perf_resource qcom_perf_rates_v680[] = {
	[QCOM_SOC_PERF_SUSPEND]	=
		QCOM_PERF_ROW(cnoc_axi_clk_t680[QCOM_SOC_PERF_SUSPEND],
			      qcom_jpeg_clk_t165_t480[QCOM_SOC_PERF_SUSPEND]),

	[QCOM_SOC_PERF_LOWSVS]		=
		QCOM_PERF_ROW(cnoc_axi_clk_t680[QCOM_SOC_PERF_LOWSVS],
			      qcom_jpeg_clk_t165_t480[QCOM_SOC_PERF_LOWSVS]),

	[QCOM_SOC_PERF_SVS]		=
		QCOM_PERF_ROW(cnoc_axi_clk_t680[QCOM_SOC_PERF_SVS],
			      qcom_jpeg_clk_t165_t480[QCOM_SOC_PERF_SVS]),

	[QCOM_SOC_PERF_SVS_L1]		=
		QCOM_PERF_ROW(cnoc_axi_clk_t680[QCOM_SOC_PERF_SVS_L1],
			      qcom_jpeg_clk_t165_t480[QCOM_SOC_PERF_SVS_L1]),

	[QCOM_SOC_PERF_NOMINAL]	=
		QCOM_PERF_ROW(cnoc_axi_clk_t680[QCOM_SOC_PERF_NOMINAL],
			      qcom_jpeg_clk_t165_t480[QCOM_SOC_PERF_NOMINAL]),

	[QCOM_SOC_PERF_TURBO]		=
		QCOM_PERF_ROW(cnoc_axi_clk_t680[QCOM_SOC_PERF_TURBO],
			      qcom_jpeg_clk_t165_t480[QCOM_SOC_PERF_TURBO]),
};

static const struct qcom_perf_resource qcom_perf_rates_v780[] = {
	[QCOM_SOC_PERF_SUSPEND]	=
		QCOM_PERF_ROW(cnoc_axi_clk_t165_t480[QCOM_SOC_PERF_SUSPEND],
			      qcom_jpeg_clk_t780[QCOM_SOC_PERF_SUSPEND]),

	[QCOM_SOC_PERF_LOWSVS]		=
		QCOM_PERF_ROW(cnoc_axi_clk_t165_t480[QCOM_SOC_PERF_LOWSVS],
			      qcom_jpeg_clk_t780[QCOM_SOC_PERF_LOWSVS]),

	[QCOM_SOC_PERF_SVS]		=
		QCOM_PERF_ROW(cnoc_axi_clk_t165_t480[QCOM_SOC_PERF_SVS],
			      qcom_jpeg_clk_t780[QCOM_SOC_PERF_SVS]),

	[QCOM_SOC_PERF_SVS_L1]		=
		QCOM_PERF_ROW(cnoc_axi_clk_t165_t480[QCOM_SOC_PERF_SVS_L1],
			      qcom_jpeg_clk_t780[QCOM_SOC_PERF_SVS_L1]),

	[QCOM_SOC_PERF_NOMINAL]	=
		QCOM_PERF_ROW(cnoc_axi_clk_t165_t480[QCOM_SOC_PERF_NOMINAL],
			      qcom_jpeg_clk_t780[QCOM_SOC_PERF_NOMINAL]),

	[QCOM_SOC_PERF_TURBO]		=
		QCOM_PERF_ROW(cnoc_axi_clk_t165_t480[QCOM_SOC_PERF_TURBO],
			      qcom_jpeg_clk_t780[QCOM_SOC_PERF_TURBO]),
};

static const struct qcom_icc_resource qcom_jpeg_default_icc[] = {
	{
		.icc_id = "cam_ahb",
		.pair	= { 38400, 76800 }
	},
	{
		.icc_id = "cam_hf_0_mnoc",
		.pair	= { 2097152, 2097152 }
	},
	{
		.icc_id = "cam_sf_0_mnoc",
		.pair	= { 0, 2097152 }
	},
	{
		.icc_id	= "cam_sf_icp_mnoc",
		.pair	= { 2097152, 2097152 }
	},
};

/*
 * Resources for T165, T170, T480 JPEG version and derivatives
 */
const struct qcom_dev_resources qcom_t165_t480_jpeg_drvdata = {
	.hw_ops		= &qcom_jpeg_default_ops,
	.hw_offs	= &qcom_jpeg_v420_hw_reg_offs,
	.hw_mask	= &qcom_jpeg_v420_hw_reg_mask[0],
	.icc_res	= qcom_jpeg_default_icc,
	.num_of_icc	= ARRAY_SIZE(qcom_jpeg_default_icc),
	.perf_cfg	= qcom_perf_rates_t165_t480,
	.clk_names = {
		[JPEG_CAMNOC_AXI_CLK]	= "camnoc_axi",
		[JPEG_CORE_CLK]		= "jpeg",
		[JPEG_CORE_AHB_CLK]	= "core_ahb",
		[JPEG_CPAS_AHB_CLK]	= "cpas_ahb",
		[JPEG_GCC_HF_AXI]	= "gcc_hf_axi",
		[JPEG_GCC_SF_AXI]	= "gcc_sf_axi",
	}
};

/*
 * Resources for T680 JPEG version and derivatives
 */
const struct qcom_dev_resources qcom_t680_jpeg_drvdata = {
	.hw_ops		= &qcom_jpeg_default_ops,
	.hw_offs	= &qcom_jpeg_v420_hw_reg_offs,
	.hw_mask	= &qcom_jpeg_v420_hw_reg_mask[0],
	.icc_res	= qcom_jpeg_default_icc,
	.num_of_icc	= ARRAY_SIZE(qcom_jpeg_default_icc),
	.perf_cfg	= qcom_perf_rates_v680,
	.clk_names = {
		[JPEG_CAMNOC_AXI_CLK]	= "camnoc_axi",
		[JPEG_CORE_CLK]		= "jpeg",
		[JPEG_CORE_AHB_CLK]	= "core_ahb",
		[JPEG_CPAS_AHB_CLK]	= "cpas_ahb",
		[JPEG_GCC_HF_AXI]	= "gcc_hf_axi",
		[JPEG_GCC_SF_AXI]	= "gcc_sf_axi",
	}
};

/*
 * Resources for T780 JPEG version and derivatives
 */
const struct qcom_dev_resources qcom_t780_jpeg_drvdata = {
	.hw_ops		= &qcom_jpeg_default_ops,
	.hw_offs	= &qcom_jpeg_v420_hw_reg_offs,
	.hw_mask	= &qcom_jpeg_v420_hw_reg_mask[0],
	.icc_res	= qcom_jpeg_default_icc,
	.num_of_icc	= ARRAY_SIZE(qcom_jpeg_default_icc),
	.perf_cfg	= qcom_perf_rates_v780,
	.clk_names = {
		[JPEG_CAMNOC_AXI_CLK]	= "camnoc_axi",
		[JPEG_CORE_CLK]		= "jpeg",
		[JPEG_CORE_AHB_CLK]	= "core_ahb",
		[JPEG_CPAS_AHB_CLK]	= "cpas_ahb",
		[JPEG_GCC_HF_AXI]	= "gcc_hf_axi",
		[JPEG_GCC_SF_AXI]	= "gcc_sf_axi",
	}
};
