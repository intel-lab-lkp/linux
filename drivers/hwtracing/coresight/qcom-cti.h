/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#define ARCHITECT_QCOM 0x477

/* CTI programming registers */
#define	QCOM_CTIINTACK		0x020
#define	QCOM_CTIAPPSET		0x004
#define	QCOM_CTIAPPCLEAR	0x008
#define	QCOM_CTIAPPPULSE	0x00C
#define	QCOM_CTIINEN		0x400
#define	QCOM_CTIOUTEN		0x800
#define	QCOM_CTITRIGINSTATUS	0x040
#define	QCOM_CTITRIGOUTSTATUS	0x060
#define	QCOM_CTICHINSTATUS	0x080
#define	QCOM_CTICHOUTSTATUS	0x084
#define	QCOM_CTIGATE		0x088
#define	QCOM_ASICCTL		0x08c
/* Integration test registers */
#define	QCOM_ITCHINACK		0xE70
#define	QCOM_ITTRIGINACK	0xE80
#define	QCOM_ITCHOUT		0xE74
#define	QCOM_ITTRIGOUT		0xEA0
#define	QCOM_ITCHOUTACK		0xE78
#define	QCOM_ITTRIGOUTACK	0xEC0
#define	QCOM_ITCHIN		0xE7C
#define	QCOM_ITTRIGIN		0xEE0
