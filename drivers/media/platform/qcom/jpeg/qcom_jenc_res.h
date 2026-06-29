/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef QCOM_JENC_RES_H
#define QCOM_JENC_RES_H

#include "qcom_jenc_defs.h"

struct qcom_icc_resource {
	const char *icc_id;
	struct {
		u32 aggr;
		u32 peak;
	} pair;
};

struct qcom_dev_resources {
	const struct qcom_jpeg_hw_ops	*hw_ops;
	const struct qcom_jpeg_reg_offs	*hw_offs;
	const u32			*hw_mask;

	const struct qcom_icc_resource	*icc_res;
	unsigned int			num_of_icc;
};

extern const struct qcom_dev_resources qcom_t165_t480_jpeg_drvdata;

#endif	/* QCOM_JENC_RES_H */
