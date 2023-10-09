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
		case ATH12K_ACPI_DSM_FUNC_INDEX_BIOS_SAR:
			if (obj->buffer.length != ATH12K_ACPI_DSM_BIOS_SAR_DATA_SIZE) {
				ath12k_err(ab, "Invalid BIOS SAR data size %d\n",
					   obj->buffer.length);
				ret = -EINVAL;
				goto out;
			}
			memcpy(&ab->acdata->bios_sar_data, obj->buffer.pointer,
			       obj->buffer.length);
			break;
		case ATH12K_ACPI_DSM_FUNC_INDEX_GEO_OFFSET:
			if (obj->buffer.length != ATH12K_ACPI_DSM_GEO_OFFSET_DATA_SIZE) {
				ath12k_err(ab, "Invalid GEO OFFSET data size %d\n",
					   obj->buffer.length);
				ret = -EINVAL;
				goto out;
			}
			memcpy(&ab->acdata->geo_offset_data, obj->buffer.pointer,
			       obj->buffer.length);
			break;
		case ATH12K_ACPI_DSM_FUNC_INDEX_CCA:
			if (obj->buffer.length != ATH12K_ACPI_DSM_CCA_DATA_SIZE) {
				ath12k_err(ab, "Invalid CCA data size %d\n",
					   obj->buffer.length);
				ret = -EINVAL;
				goto out;
			}
			memcpy(&ab->acdata->cca_data, obj->buffer.pointer,
			       obj->buffer.length);
			break;
		case ATH12K_ACPI_DSM_FUNC_INDEX_BAND_EDGE:
			if (obj->buffer.length != ATH12K_ACPI_DSM_BAND_EDGE_DATA_SIZE) {
				ath12k_err(ab, "Invalid BAND EDGE data size %d\n",
					   obj->buffer.length);
				ret = -EINVAL;
				goto out;
			}
			memcpy(&ab->acdata->band_edge_power, obj->buffer.pointer,
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

static int ath12k_set_bios_sar_power_limit_data(struct ath12k_base *ab)
{
	int ret;

	if (ab->acdata->bios_sar_data[0] == ATH12K_ACPI_POWER_LIMIT_VERSION &&
	    ab->acdata->bios_sar_data[1] == ATH12K_ACPI_POWER_LIMIT_ENABLE_FLAG) {
		ret = ath12k_wmi_pdev_set_bios_sar_table_param(ab,
							       ab->acdata->bios_sar_data);
		if (ret)
			ath12k_err(ab, "failed to pass bios sar table %d\n", ret);
	} else {
		ath12k_err(ab, "the latest bios sar data is invalid\n");
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

		if (ab->acdata->acpi_bios_sar_enable) {
			ret = ath12k_acpi_dsm_get_data(ab,
						       ATH12K_ACPI_DSM_FUNC_INDEX_BIOS_SAR);
			if (ret) {
				ath12k_err(ab, "failed to update bios sar %d\n", ret);
				return;
			}

			ret = ath12k_set_bios_sar_power_limit_data(ab);
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

static int ath12k_pass_acpi_bios_sar_and_geo_to_fw(struct ath12k_base *ab)
{
	int ret;

	ret = ath12k_wmi_pdev_set_bios_sar_table_param(ab,
						       ab->acdata->bios_sar_data);

	if (ret) {
		ath12k_err(ab, "failed to pass bios sar table to fw %d\n", ret);
		return ret;
	}

	ret = ath12k_wmi_pdev_set_bios_geo_table_param(ab,
						       ab->acdata->geo_offset_data);

	if (ret)
		ath12k_err(ab, "failed to pass bios geo table to fw %d\n", ret);

	return ret;
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

	if (ATH12K_ACPI_FUNC_BIT_VALID(ab->acdata, ATH12K_ACPI_FUNC_BIT_BIOS_SAR)) {
		ret = ath12k_acpi_dsm_get_data(ab,
					       ATH12K_ACPI_DSM_FUNC_INDEX_BIOS_SAR);
		if (ret) {
			ath12k_err(ab, "failed to get bios sar data %d\n", ret);
			goto err_free_acdata;
		}
	}

	if (ATH12K_ACPI_FUNC_BIT_VALID(ab->acdata, ATH12K_ACPI_FUNC_BIT_GEO_OFFSET)) {
		ret = ath12k_acpi_dsm_get_data(ab,
					       ATH12K_ACPI_DSM_FUNC_INDEX_GEO_OFFSET);
		if (ret) {
			ath12k_err(ab, "failed to get geo offset data %d\n", ret);
			goto err_free_acdata;
		}

		if (ATH12K_ACPI_FUNC_BIT_VALID(ab->acdata, ATH12K_ACPI_FUNC_BIT_BIOS_SAR) &&
		    ab->acdata->bios_sar_data[0] == ATH12K_ACPI_POWER_LIMIT_VERSION &&
		    ab->acdata->bios_sar_data[1] == ATH12K_ACPI_POWER_LIMIT_ENABLE_FLAG &&
		    !ab->acdata->acpi_tas_enable)
			ab->acdata->acpi_bios_sar_enable = true;
	}

	if (ab->acdata->acpi_tas_enable) {
		ret = ath12k_pass_acpi_cfg_and_data_to_fw(ab);
		if (ret)
			goto err_free_acdata;
	}

	if (ab->acdata->acpi_bios_sar_enable) {
		ret = ath12k_pass_acpi_bios_sar_and_geo_to_fw(ab);
		if (ret)
			goto err_free_acdata;
	}

	if (ATH12K_ACPI_FUNC_BIT_VALID(ab->acdata, ATH12K_ACPI_FUNC_BIT_CCA)) {
		ret = ath12k_acpi_dsm_get_data(ab,
					       ATH12K_ACPI_DSM_FUNC_INDEX_CCA);
		if (ret) {
			ath12k_err(ab, "failed to get cca threshold configuration %d\n", ret);
			goto err_free_acdata;
		}

		if (ab->acdata->cca_data[0] == ATH12K_ACPI_CCA_THR_VERSION &&
		    ab->acdata->cca_data[ATH12K_ACPI_CCA_THR_OFFSET_DATA_OFFSET] ==
		    ATH12K_ACPI_CCA_THR_ENABLE_FLAG) {
			ret = ath12k_wmi_pdev_set_cca_thr_table_param(ab,
								      ab->acdata->cca_data);
			if (ret) {
				ath12k_err(ab, "set cca threshold failed %d\n", ret);
				goto err_free_acdata;
			}
		}
	}

	if (ATH12K_ACPI_FUNC_BIT_VALID(ab->acdata, ATH12K_ACPI_FUNC_BIT_BAND_EDGE_CHAN_POWER)) {
		ret = ath12k_acpi_dsm_get_data(ab,
					       ATH12K_ACPI_DSM_FUNC_INDEX_BAND_EDGE);
		if (ret) {
			ath12k_err(ab, "failed to get band edge channel power %d\n", ret);
			goto err_free_acdata;
		}

		if (ab->acdata->band_edge_power[0] == ATH12K_ACPI_BAND_EDGE_VERSION &&
		    ab->acdata->band_edge_power[1] == ATH12K_ACPI_BAND_EDGE_ENABLE_FLAG) {
			ret = ath12k_wmi_pdev_set_band_edge_power(ab,
								  ab->acdata->band_edge_power);
			if (ret) {
				ath12k_err(ab, "set band edge channel power failed %d\n", ret);
				goto err_free_acdata;
			}
		}
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
