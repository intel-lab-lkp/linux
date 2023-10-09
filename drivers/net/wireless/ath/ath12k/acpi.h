/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/*
 * Copyright (c) 2018-2021 The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#ifndef ATH12K_ACPI_H
#define ATH12K_ACPI_H

#include <linux/acpi.h>

#define ATH12K_ACPI_DSM_FUNC_INDEX_SUPPORT_FUNCS	0
#define ATH12K_ACPI_DSM_FUNC_INDEX_TAS_CFG		8
#define ATH12K_ACPI_DSM_FUNC_INDEX_TAS_DATA		9

#define ATH12K_ACPI_FUNC_BIT_TAS_CFG			BIT(7)
#define ATH12K_ACPI_FUNC_BIT_TAS_DATA			BIT(8)

#define ATH12K_ACPI_NOTIFY_EVENT			0x86
#define ATH12K_ACPI_FUNC_BIT_VALID(_acdata, _func)	((((_acdata)->func_bit) & (_func)) != 0)

#define ATH12K_ACPI_TAS_DATA_VERSION		0x1
#define ATH12K_ACPI_TAS_DATA_ENABLE_FLAG	0x1

#define ATH12K_ACPI_DSM_TAS_DATA_SIZE			69
#define ATH12K_ACPI_DSM_TAS_CFG_SIZE			108

int ath12k_get_acpi_all_data(struct ath12k_base *ab);
void acpi_dsm_notify(acpi_handle handle, u32 event, void *data);
#endif
