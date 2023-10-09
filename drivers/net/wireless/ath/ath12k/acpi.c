// SPDX-License-Identifier: BSD-3-Clause-Clear
/*
 * Copyright (c) 2018-2021 The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include "core.h"
#include "acpi.h"
#include "debug.h"

static int ath12k_acpi_dsm_get_data(struct ath12k_base *ab, int func)
{
	union acpi_object *obj;
	acpi_handle root_handle;
	int ret = 0;

	root_handle = ACPI_HANDLE(ab->dev);
	if (!root_handle) {
		ath12k_dbg(ab, ATH12K_DBG_BOOT, "invalid ACPI handler\n");
		return -EOPNOTSUPP;
	}

	obj = acpi_evaluate_dsm(root_handle, ab->hw_params->acpi_guid, 0, func,
				NULL);

	if (!obj) {
		ath12k_dbg(ab, ATH12K_DBG_BOOT, "ACPI _DSM method invocation failed\n");
		return -ENOENT;
	}

	if (obj->type == ACPI_TYPE_INTEGER) {
		ab->acdata->func_bit = obj->integer.value;
	} else if (obj->type == ACPI_TYPE_BUFFER) {
		switch (func) {
		case ATH12K_ACPI_DSM_FUNC_INDEX_TAS_CFG:
			if (obj->buffer.length != ATH12K_ACPI_DSM_TAS_CFG_SIZE) {
				ath12k_err(ab, "Invalid TAS cfg size %d\n",
					   obj->buffer.length);
				ret = -EINVAL;
				goto out;
			}
			memcpy(&ab->acdata->tas_cfg, obj->buffer.pointer,
			       obj->buffer.length);
			break;
		case ATH12K_ACPI_DSM_FUNC_INDEX_TAS_DATA:
			if (obj->buffer.length != ATH12K_ACPI_DSM_TAS_DATA_SIZE) {
				ath12k_err(ab, "Invalid TAS data size %d\n",
					   obj->buffer.length);
				ret = -EINVAL;
				goto out;
			}
			memcpy(&ab->acdata->tas_sar_power_table, obj->buffer.pointer,
			       obj->buffer.length);
			break;
		}
	} else {
		ath12k_err(ab,
			   "ACPI: DSM method did not return a valid object, type %d\n",
			   obj->type);
		ret = -EINVAL;
	}

out:
	ACPI_FREE(obj);
	return ret;
}

static int ath12k_set_tas_power_limit_data(struct ath12k_base *ab)
{
	int ret;

	if (ab->acdata->tas_sar_power_table[0] == ATH12K_ACPI_TAS_DATA_VERSION &&
	    ab->acdata->tas_sar_power_table[1] == ATH12K_ACPI_TAS_DATA_ENABLE_FLAG) {
		ret = ath12k_wmi_pdev_set_tas_data_table_param(ab,
							       ab->acdata->tas_sar_power_table);
		if (ret)
			ath12k_err(ab, "failed to pass tas data table %d\n", ret);
	} else {
		ath12k_err(ab, "the latest tas data is invalid\n");
		ret = -EINVAL;
	}

	return ret;
}

void acpi_dsm_notify(acpi_handle handle, u32 event, void *data)
{
	int ret;
	struct ath12k_base *ab = data;

	if (event == ATH12K_ACPI_NOTIFY_EVENT) {
		if (ab->acdata->acpi_tas_enable) {
			ret = ath12k_acpi_dsm_get_data(ab,
						       ATH12K_ACPI_DSM_FUNC_INDEX_TAS_DATA);
			if (ret) {
				ath12k_err(ab, "failed to update tas data table %d\n", ret);
				return;
			}

			ret = ath12k_set_tas_power_limit_data(ab);
			if (ret)
				return;
		}
	} else {
		ath12k_err(ab, "unknown acpi notify %u\n", event);
	}
}

void ath12k_acpi_remove_notify(struct ath12k_base *ab)
{
	acpi_remove_notify_handler(ACPI_HANDLE(ab->dev),
				   ACPI_DEVICE_NOTIFY,
				   acpi_dsm_notify);
}

static int ath12k_pass_acpi_cfg_and_data_to_fw(struct ath12k_base *ab)
{
	int ret;

	ret = ath12k_wmi_pdev_set_tas_cfg_table_param(ab,
						      ab->acdata->tas_cfg);
	if (ret) {
		ath12k_err(ab, "failed to pass tas cfg table to fw %d\n", ret);
		return ret;
	}

	ret = ath12k_wmi_pdev_set_tas_data_table_param(ab,
						       ab->acdata->tas_sar_power_table);
	if (ret)
		ath12k_err(ab, "failed to pass tas data table to fw %d\n", ret);

	return ret;
}

int ath12k_get_acpi_all_data(struct ath12k_base *ab)
{
	int ret;
	acpi_status status;

	ab->acdata = kzalloc(sizeof(*ab->acdata), GFP_KERNEL);
	if (!ab->acdata)
		return -ENOMEM;

	ab->acdata->acpi_tas_enable = false;

	ret = ath12k_acpi_dsm_get_data(ab,
				       ATH12K_ACPI_DSM_FUNC_INDEX_SUPPORT_FUNCS);

	if (ret)
		return ret;

	if (ATH12K_ACPI_FUNC_BIT_VALID(ab->acdata, ATH12K_ACPI_FUNC_BIT_TAS_CFG)) {
		ret = ath12k_acpi_dsm_get_data(ab,
					       ATH12K_ACPI_DSM_FUNC_INDEX_TAS_CFG);
		if (ret) {
			ath12k_err(ab, "failed to get tas cfg table %d\n", ret);
			goto err_free_acdata;
		}
	}

	if (ATH12K_ACPI_FUNC_BIT_VALID(ab->acdata, ATH12K_ACPI_FUNC_BIT_TAS_DATA)) {
		ret = ath12k_acpi_dsm_get_data(ab,
					       ATH12K_ACPI_DSM_FUNC_INDEX_TAS_DATA);
		if (ret) {
			ath12k_err(ab, "failed to get tas data table %d\n", ret);
			goto err_free_acdata;
		}

		if (ATH12K_ACPI_FUNC_BIT_VALID(ab->acdata, ATH12K_ACPI_FUNC_BIT_TAS_CFG) &&
		    ab->acdata->tas_sar_power_table[0] == ATH12K_ACPI_TAS_DATA_VERSION &&
		    ab->acdata->tas_sar_power_table[1] == ATH12K_ACPI_TAS_DATA_ENABLE_FLAG)
			ab->acdata->acpi_tas_enable = true;
	}

	if (ab->acdata->acpi_tas_enable) {
		ret = ath12k_pass_acpi_cfg_and_data_to_fw(ab);
		if (ret)
			goto err_free_acdata;
	}

	status = acpi_install_notify_handler(ACPI_HANDLE(ab->dev),
					     ACPI_DEVICE_NOTIFY,
					     acpi_dsm_notify, ab);
	if (ACPI_FAILURE(status)) {
		ath12k_err(ab, "failed to install DSM notify callback\n");
		goto err_remove_notify;
	}

	return 0;

err_remove_notify:
	acpi_remove_notify_handler(ACPI_HANDLE(ab->dev),
				   ACPI_DEVICE_NOTIFY,
				   acpi_dsm_notify);

	ret = -EIO;

err_free_acdata:
	kfree(ab->acdata);
	ab->acdata = NULL;

	return ret;
}
