/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2018, The Linux Foundation. All rights reserved. */

#ifndef _DT_BINDINGS_POWER_QCOM_RPMPD_H
#define _DT_BINDINGS_POWER_QCOM_RPMPD_H

#include <dt-bindings/power/qcom,rpmhpd.h>

/* Generic RPM Power Domain Indexes */
#define RPMPD_VDDCX		0
#define RPMPD_VDDCX_AO		1
/* VFC and VFL are mutually exclusive and can not be present on the same platform */
#define RPMPD_VDDCX_VFC		2
#define RPMPD_VDDCX_VFL		2
#define RPMPD_VDDMX		3
#define RPMPD_VDDMX_AO		4
#define RPMPD_VDDMX_VFL		5
#define RPMPD_SSCCX		6
#define RPMPD_SSCCX_VFL		7
#define RPMPD_SSCMX		8
#define RPMPD_SSCMX_VFL		9

/*
 * Platform-specific power domain bindings. Don't add new entries here, use
 * RPMPD_* above.
 */

/* MSM8939 Power Domains */
#define MSM8939_VDDMDCX		0
#define MSM8939_VDDMDCX_AO	1
#define MSM8939_VDDMDCX_VFC	2
#define MSM8939_VDDCX		3
#define MSM8939_VDDCX_AO	4
#define MSM8939_VDDCX_VFC	5
#define MSM8939_VDDMX		6
#define MSM8939_VDDMX_AO	7

/* MSM8953 Power Domain Indexes */
#define MSM8953_VDDMD		0
#define MSM8953_VDDMD_AO	1
#define MSM8953_VDDCX		2
#define MSM8953_VDDCX_AO	3
#define MSM8953_VDDCX_VFL	4
#define MSM8953_VDDMX		5
#define MSM8953_VDDMX_AO	6

/* MSM8974 Power Domain Indexes */
#define MSM8974_VDDCX		0
#define MSM8974_VDDCX_AO	1
#define MSM8974_VDDCX_VFC	2
#define MSM8974_VDDGFX		3
#define MSM8974_VDDGFX_VFC	4

/* MSM8994 Power Domain Indexes */
#define MSM8994_VDDCX		0
#define MSM8994_VDDCX_AO	1
#define MSM8994_VDDCX_VFC	2
#define MSM8994_VDDMX		3
#define MSM8994_VDDMX_AO	4
#define MSM8994_VDDGFX		5
#define MSM8994_VDDGFX_VFC	6

/* MSM8996 Power Domain Indexes */
#define MSM8996_VDDCX		0
#define MSM8996_VDDCX_AO	1
#define MSM8996_VDDCX_VFC	2
#define MSM8996_VDDMX		3
#define MSM8996_VDDMX_AO	4
#define MSM8996_VDDSSCX		5
#define MSM8996_VDDSSCX_VFC	6

/* QCM2290 Power Domains */
#define QCM2290_VDDCX		0
#define QCM2290_VDDCX_AO	1
#define QCM2290_VDDCX_VFL	2
#define QCM2290_VDDMX		3
#define QCM2290_VDDMX_AO	4
#define QCM2290_VDDMX_VFL	5
#define QCM2290_VDD_LPI_CX	6
#define QCM2290_VDD_LPI_MX	7

/* QCS404 Power Domains */
#define QCS404_VDDMX		0
#define QCS404_VDDMX_AO		1
#define QCS404_VDDMX_VFL	2
#define QCS404_LPICX		3
#define QCS404_LPICX_VFL	4
#define QCS404_LPIMX		5
#define QCS404_LPIMX_VFL	6

/* SM6115 Power Domains */
#define SM6115_VDDCX		0
#define SM6115_VDDCX_AO		1
#define SM6115_VDDCX_VFL	2
#define SM6115_VDDMX		3
#define SM6115_VDDMX_AO		4
#define SM6115_VDDMX_VFL	5
#define SM6115_VDD_LPI_CX	6
#define SM6115_VDD_LPI_MX	7

/* SM6375 Power Domain Indexes */
#define SM6375_VDDCX		0
#define SM6375_VDDCX_AO	1
#define SM6375_VDDCX_VFL	2
#define SM6375_VDDMX		3
#define SM6375_VDDMX_AO	4
#define SM6375_VDDMX_VFL	5
#define SM6375_VDDGX		6
#define SM6375_VDDGX_AO	7
#define SM6375_VDD_LPI_CX	8
#define SM6375_VDD_LPI_MX	9

/* RPM SMD Power Domain performance levels */
#define RPM_SMD_LEVEL_RETENTION       16
#define RPM_SMD_LEVEL_RETENTION_PLUS  32
#define RPM_SMD_LEVEL_MIN_SVS         48
#define RPM_SMD_LEVEL_LOW_SVS         64
#define RPM_SMD_LEVEL_SVS             128
#define RPM_SMD_LEVEL_SVS_PLUS        192
#define RPM_SMD_LEVEL_NOM             256
#define RPM_SMD_LEVEL_NOM_PLUS        320
#define RPM_SMD_LEVEL_TURBO           384
#define RPM_SMD_LEVEL_TURBO_NO_CPR    416
#define RPM_SMD_LEVEL_TURBO_HIGH      448
#define RPM_SMD_LEVEL_BINNING         512

#endif
