// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include "qcom_jenc_ops.h"
#include "qcom_jenc_res.h"

#include "qcom_jenc_v420_hw_info.h"

static const struct qcom_icc_resource qcom_jpeg_default_icc[] = {
	{
		.icc_id = "cpu-cfg",
		.pair	= { 38400, 76800 }
	},
	{
		.icc_id = "hf-mnoc",
		.pair	= { 2097152, 2097152 }
	},
	{
		.icc_id = "sf-mnoc",
		.pair	= { 0, 2097152 }
	},
	{
		.icc_id	= "icp-mnoc",
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
};
