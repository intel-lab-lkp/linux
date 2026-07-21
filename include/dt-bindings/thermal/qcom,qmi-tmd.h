/* SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause */
/*
 * Qualcomm QMI TMD (Thermal Mitigation Device) cooling device indices
 *
 * These indices are used in device tree cooling-maps to reference
 * specific TMD devices provided by remote processors via QMI.
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#ifndef _DT_BINDINGS_THERMAL_QCOM_QMI_TMD_H
#define _DT_BINDINGS_THERMAL_QCOM_QMI_TMD_H

/* CDSP thermal mitigation device id */
#define QCOM_TMD_CDSP_SW	0

/* Modem thermal mitigation device id */
#define QCOM_TMD_PA	0
#define QCOM_TMD_MODEM	1

#endif /* _DT_BINDINGS_THERMAL_QCOM_QMI_TMD_H */
