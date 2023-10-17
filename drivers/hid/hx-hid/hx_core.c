// SPDX-License-Identifier: GPL-2.0
/*  Himax Driver Code for Common IC to simulate HID
 *
 *  Copyright (C) 2023 Himax Corporation.
 *
 *  This software is licensed under the terms of the GNU General Public
 *  License version 2,  as published by the Free Software Foundation,  and
 *  may be copied,  distributed,  and modified under those terms.
 *
 *  This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include "hx_ic_core.h"
#include "hx_plat.h"
#include "hx_hid.h"
#include "hx_inspect.h"
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/errno.h>

bool debug_flag;
struct himax_ts_data *g_himax_ts;
struct himax_core_fp g_core_fp;

#define FW_EXT_NAME(FWNAME)	FWNAME ".bin"
char *g_fw_boot_upgrade_name = FW_EXT_NAME(BOOT_UPGRADE_FWNAME);
char *g_fw_mp_upgrade_name = MPAP_FWNAME;

#if !defined(__HIMAX_MOD__)
/* calculate time diff and return as milliseconds */
static unsigned int time_diff(struct time_var *start, struct time_var *end)
{
	unsigned int diff = 0;

	diff = (end->tv_sec - start->tv_sec) * 1000;
	diff += (end->time_var_fine - start->time_var_fine) / time_var_fine_unit;

	return diff;
}
#endif

/* start himax_touch_get */
static int himax_touch_get(struct himax_ts_data *ts, u8 *buf, int ts_path)
{
	u32 read_size = 0;
	int ts_status = 0;

	switch (ts_path) {
	/*normal*/
	case HX_REPORT_COORD:
	case HX_REPORT_COORD_RAWDATA:
		read_size = ts->touch_all_size;
		break;
	default:
		break;
	}

	if (read_size == 0) {
		E("Read size fault!");
		ts_status = HX_TS_GET_DATA_FAIL;
	} else {
		if (!g_core_fp.fp_read_event_stack(ts, buf, read_size)) {
			E("can't read data from chip!");
			ts_status = HX_TS_GET_DATA_FAIL;
		}
	}

	return ts_status;
}

/* start error_control*/
static int himax_checksum_cal(struct himax_ts_data *ts, u8 *buf, int ts_path)
{
	u16 check_sum_cal = 0;
	s32	i = 0;
	int length = 0;
	int zero_cnt = 0;
	int ret_val = HX_TS_NORMAL_END;

	/* Normal */
	switch (ts_path) {
	case HX_REPORT_COORD:
	case HX_REPORT_COORD_RAWDATA:
		length = ts->touch_info_size;
		break;
	default:
		I("Neither Normal Nor SMWP error!");
		ret_val = HX_PATH_FAIL;
		goto END_FUNCTION;
	}

	for (i = 0; i < length; i++) {
		check_sum_cal += buf[i];
		if (buf[i] == 0x00)
			zero_cnt++;
	}

	if (check_sum_cal % 0x100 != 0 && ts_path != HX_REPORT_COORD &&
	    ts_path != HX_REPORT_COORD_RAWDATA) {
		I("point data_checksum not match check_sum_cal: 0x%02X",
		  check_sum_cal);
		ret_val = HX_CHKSUM_FAIL;
	} else if (zero_cnt == length) {
		if (ts->use_irq)
			I("[HIMAX TP MSG] All Zero event");

		ret_val = HX_CHKSUM_FAIL;
	} else {
		if (ts_path == HX_REPORT_COORD ||
		    ts_path == HX_REPORT_COORD_RAWDATA) {
			ret_val = HX_REPORT_DATA;
			goto END_FUNCTION;
		}
		/*Need to clear event stack here*/
		g_core_fp.fp_read_event_stack(ts, buf,
			(HX_STACK_ORG_LEN -	ts->touch_info_size));
	}

END_FUNCTION:
	return ret_val;
}

static void himax_excp_hw_reset(struct himax_ts_data *ts)
{
	int result = 0;

	I("START EXCEPTION Reset");
	hx_hid_remove(ts);
	if (!ts->ic_data->has_flash) {
		result = g_core_fp.fp_0f_op_file_dirly(g_fw_boot_upgrade_name, ts);
		if (result) {
			E("update FW fail, code[%d]!!", result);
		} else {
			I("update FW success!!");
			hx_hid_probe(ts);
		}
	} else {
		g_core_fp.fp_excp_ic_reset(ts);
		hx_hid_probe(ts);
	}
	I("END EXCEPTION Reset");
}

#if defined(HW_ED_EXCP_EVENT)
static int himax_ts_event_check(struct himax_ts_data *ts,
				const u8 *buf, int ts_path, int ts_status)
{
	u32 hx_EB_event = 0;
	u32 hx_EC_event = 0;
	u32 hx_EE_event = 0;
	u32 hx_ED_event = 0;
	u32 hx_excp_event = 0;
	int shaking_ret = 0;

	u32 i = 0;
	u32 length = 0;
	int ret_val = ts_status;

	/* Normal */
	switch (ts_path) {
	case HX_REPORT_COORD:
		length = ts->touch_info_size;
		break;
	case HX_REPORT_COORD_RAWDATA:
		length = ts->touch_info_size;
		break;
	default:
		I("Neither Normal Nor SMWP error!");
		ret_val = HX_PATH_FAIL;
		goto END_FUNCTION;
	}

	if (ts_path == HX_REPORT_COORD || ts_path == HX_REPORT_COORD_RAWDATA) {
		for (i = 0; i < length; i++) {
			/* case 1 EXCEEPTION recovery flow */
			if (buf[i] == 0xEB) {
				hx_EB_event++;
			} else if (buf[i] == 0xEC) {
				hx_EC_event++;
			} else if (buf[i] == 0xEE) {
				hx_EE_event++;
			/* case 2 EXCEPTION recovery flow-Disable */
			} else if (buf[i] == 0xED) {
				hx_ED_event++;
			} else {
				ts->excp_zero_event_count = 0;
				break;
			}
		}
	}

	if (hx_EB_event == length) {
		hx_excp_event = length;
		ts->excp_eb_event_flag++;
		I("[HIMAX TP MSG]: EXCEPTION event checked - ALL 0xEB.");
	} else if (hx_EC_event == length) {
		hx_excp_event = length;
		ts->excp_ec_event_flag++;
		I("[HIMAX TP MSG]: EXCEPTION event checked - ALL 0xEC.");
	} else if (hx_EE_event == length) {
		hx_excp_event = length;
		ts->excp_ee_event_flag++;
		I("[HIMAX TP MSG]: EXCEPTION event checked - ALL 0xEE.");
	} else if (hx_ED_event == length) {
		g_core_fp.fp_0f_reload_to_active();
	}

	if ((hx_excp_event == length || hx_ED_event == length) &&
	    ts->excp_reset_active == 0) {
		shaking_ret = g_core_fp.fp_ic_excp_recovery(ts,
			hx_excp_event, hx_ED_event, length);

		if (shaking_ret == HX_EXCP_EVENT) {
			g_core_fp.fp_read_FW_status();
			himax_excp_hw_reset(ts);
			ret_val = HX_EXCP_EVENT;
		} else if (shaking_ret == HX_ZERO_EVENT_COUNT) {
			g_core_fp.fp_read_FW_status();
			ret_val = HX_ZERO_EVENT_COUNT;
		} else {
			I("IC is running. Nothing to be done!");
			ret_val = HX_IC_RUNNING;
		}

	/* drop 1st interrupts after chip reset */
	} else if (ts->excp_reset_active) {
		ts->excp_reset_active = 0;
		I("Skip by excp_reset_active.");
		ret_val = HX_EXCP_REC_OK;
	}

END_FUNCTION:
	if (g_ts_dbg != 0)
		I("END, ret_val=%d!", ret_val);

	return ret_val;
}
#else
static int himax_ts_event_check(struct himax_ts_data *ts,
				const u8 *buf, int ts_path)
{
	u32 hx_EB_event = 0;
	u32 hx_EC_event = 0;
	u32 hx_ED_event = 0;
	u32 hx_excp_event = 0;
	u32 hx_zero_event = 0;
	int shaking_ret = 0;

	u32 i = 0;
	u32 length = 0;
	int ret_val = 0;

	/* Normal */
	switch (ts_path) {
	case HX_REPORT_COORD:
		length = ts->touch_info_size;
		break;
	case HX_REPORT_COORD_RAWDATA:
		length = ts->touch_info_size;
		break;
	default:
		I("Neither Normal Nor SMWP error!");
		ret_val = HX_PATH_FAIL;
		goto END_FUNCTION;
	}

	if (ts_path == HX_REPORT_COORD || ts_path == HX_REPORT_COORD_RAWDATA) {
		for (i = 0; i < length; i++) {
			/* case 1 EXCEEPTION recovery flow */
			if (buf[i] == 0xEB) {
				hx_EB_event++;
			} else if (buf[i] == 0xEC) {
				hx_EC_event++;
			} else if (buf[i] == 0xED) {
				hx_ED_event++;

			/* case 2 EXCEPTION recovery flow-Disable */
			} else if (buf[i] == 0x00) {
				hx_zero_event++;
			} else {
				ts->excp_zero_event_count = 0;
				break;
			}
		}
	}

	if (hx_EB_event == length) {
		hx_excp_event = length;
		ts->excp_eb_event_flag++;
		I("[HIMAX TP MSG]: EXCEPTION event checked - ALL 0xEB.");
	} else if (hx_EC_event == length) {
		hx_excp_event = length;
		ts->excp_ec_event_flag++;
		I("[HIMAX TP MSG]: EXCEPTION event checked - ALL 0xEC.");
	} else if (hx_ED_event == length) {
		hx_excp_event = length;
		ts->excp_ed_event_flag++;
		I("[HIMAX TP MSG]: EXCEPTION event checked - ALL 0xED.");
	}

	if ((hx_excp_event == length || hx_zero_event == length) &&
	    ts->excp_reset_active == 0) {
		shaking_ret = g_core_fp.fp_ic_excp_recovery(ts,
			hx_excp_event, hx_zero_event, length);

		if (shaking_ret == HX_EXCP_EVENT) {
			g_core_fp.fp_read_FW_status(ts);
			himax_excp_hw_reset(ts);
			ret_val = HX_EXCP_EVENT;
		} else if (shaking_ret == HX_ZERO_EVENT_COUNT) {
			g_core_fp.fp_read_FW_status(ts);
			ret_val = HX_ZERO_EVENT_COUNT;
		} else {
			I("IC is running. Nothing to be done!");
			ret_val = HX_IC_RUNNING;
		}

	/* drop 1st interrupts after chip reset */
	} else if (ts->excp_reset_active) {
		ts->excp_reset_active = 0;
		I("Skip by excp_reset_active.");
		ret_val = HX_EXCP_REC_OK;
	}

END_FUNCTION:

	return ret_val;
}
#endif

static int himax_err_ctrl(struct himax_ts_data *ts,
			  u8 *buf, int ts_path)
{
	int ts_status = HX_CHKSUM_FAIL;

	ts_status = himax_checksum_cal(ts, buf, ts_path);
	if (ts_status == HX_CHKSUM_FAIL) {
		goto CHK_FAIL;
	} else {
		/* continuous N times record, not total N times. */
		ts->excp_zero_event_count = 0;
		goto END_FUNCTION;
	}

CHK_FAIL:
	ts_status = himax_ts_event_check(ts, buf, ts_path);
END_FUNCTION:
	return ts_status;
}

/* end error_control*/

#if defined(HX_HEATMAP_EN)
static void heatmap_decompress_12BITS(struct himax_ts_data *ts,
				      u8 *in_buf, u8 *target)
{
	int i = 0, in_offset = 0;
	u8 *in_ptr = NULL;
	u16 *target_ptr = NULL;

	target[0] = in_buf[0];
	memcpy(&target[1], &in_buf[1], HEAT_MAP_INFO_SZ);
	for (i = 0, in_offset = HEAT_MAP_INFO_SZ + 1;
		i < ts->ic_data->HX_RX_NUM * ts->ic_data->HX_TX_NUM; i += 2) {
		in_ptr = &in_buf[in_offset + i * 3 / 2];
		target_ptr = (u16 *)&target[HEAT_MAP_INFO_SZ + 1 + i * 2];
		*target_ptr = (u16)in_ptr[0] | ((u16)(in_ptr[2] & 0xF0) << 4);
		*(target_ptr + 1) = (u16)(in_ptr[2] & 0x0F) << 8 | (u16)in_ptr[1];
	}
}
#endif

static int himax_ts_operation(struct himax_ts_data *ts,
			      int ts_path)
{
	int ts_status = HX_TS_NORMAL_END;
	int ret = 0;
	u32 offset = 0;

	memset(ts->xfer_buff,
	       0x00,
		ts->touch_all_size * sizeof(u8));
	ts_status = himax_touch_get(ts, ts->xfer_buff, ts_path);
	if (ts_status == HX_TS_GET_DATA_FAIL)
		goto END_FUNCTION;

	ts_status = himax_err_ctrl(ts, ts->xfer_buff, ts_path);
	if (!(ts_status == HX_REPORT_DATA || ts_status == HX_TS_NORMAL_END))
		goto END_FUNCTION;
	if (ts->hid_probe) {
		offset = 0;
		if (!ts->hid_req_cfg.input_RD_de) {
			ret = hx_hid_report(ts, ts->xfer_buff + offset + HID_REPORT_HDR_SZ,
					    ts->hid_desc.max_input_length - HID_REPORT_HDR_SZ);
		}
		offset += ts->hid_desc.max_input_length;
		if (ts->ic_data->HX_STYLUS_FUNC) {
			if (!ts->hid_req_cfg.input_RD_de) {
				ret += hx_hid_report(ts,
					ts->xfer_buff + offset + HID_REPORT_HDR_SZ,
					ts->hid_desc.max_input_length - HID_REPORT_HDR_SZ);
			}
			offset += ts->hid_desc.max_input_length;
		}
		#if defined(HX_HEATMAP_EN)
		if (!ts->ic_data->enc16bits)
			heatmap_decompress_12BITS(ts,
						  ts->xfer_buff + offset + HID_REPORT_HDR_SZ,
						  ts->hx_heatmap_buf);
		else
			memcpy(ts->hx_heatmap_buf,
			       ts->xfer_buff + offset + HID_REPORT_HDR_SZ,
			       ts->heatmap_data_size + HEAT_MAP_INFO_SZ + 1);

		ret += hx_hid_report(ts, ts->hx_heatmap_buf,
			(ts->ic_data->HX_RX_NUM * ts->ic_data->HX_TX_NUM * 2) +
			HEAT_MAP_INFO_SZ + 1);
		#endif
	}

	if (ret != 0)
		ts_status = HX_TS_GET_DATA_FAIL;

END_FUNCTION:
	return ts_status;
}

void himax_cable_detect_func(struct himax_ts_data *ts, bool force_renew)
{
	/*u32 connect_status = 0;*/
	u8 connect_status = 0;

	connect_status = ts->latest_power_status;

	/* I("Touch: cable status=%d, cable_config=%p, usb_connected=%d\n",*/
	/*		connect_status, ts->cable_config, ts->usb_connected); */
	if (ts->cable_config) {
		if (connect_status != ts->usb_connected || force_renew) {
			if (connect_status) {
				ts->cable_config[1] = 0x01;
				ts->usb_connected = 0x01;
			} else {
				ts->cable_config[1] = 0x00;
				ts->usb_connected = 0x00;
			}

			g_core_fp.fp_usb_detect_set(ts, ts->cable_config);
			I("Cable status change: 0x%2.2X",
			  ts->usb_connected);
		}
	}
}

void himax_ts_work(struct himax_ts_data *ts)
{
	int ts_status = HX_TS_NORMAL_END;
	int ts_path = 0;

	if (ts->notouch_frame > 0) {
		ts->notouch_frame--;
		return;
	}

	himax_cable_detect_func(ts, false);

#if defined(HX_HEATMAP_EN)
	ts_path = HX_REPORT_COORD_RAWDATA;
#else
	ts_path = HX_REPORT_COORD;
#endif
	ts_status = himax_ts_operation(ts, ts_path);
	if (ts_status == HX_TS_GET_DATA_FAIL) {
		I("Now reset the Touch chip.");
		g_core_fp.fp_ic_reset(ts, false, true);
		if (!ts->ic_data->has_flash) {
			if (g_core_fp.fp_0f_reload_to_active)
				g_core_fp.fp_0f_reload_to_active(ts);
		}
	}
}

/*end ts_work*/
enum hrtimer_restart himax_ts_timer_func(struct hrtimer *timer)
{
	struct himax_ts_data *ts;

	ts = container_of(timer, struct himax_ts_data, timer);
	queue_work(ts->himax_wq, &ts->work);
	hrtimer_start(&ts->timer, ktime_set(0, 12500000), HRTIMER_MODE_REL);

	return HRTIMER_NORESTART;
}

static int hx_chk_flash_sts(struct himax_ts_data *ts, u32 size)
{
	int rslt = 0;

	I("Entering, %d", size);

	rslt = (!g_core_fp.fp_calculate_checksum(ts, false, size));
	/*avoid the FW is full of zero*/
	rslt |= g_core_fp.fp_flash_lastdata_check(ts, size);

	return rslt;
}

static int i_get_FW(struct himax_ts_data *ts)
{
	int ret = -1;
	int result = NO_ERR;

	if (ts->hid_req_cfg.fw) {
		ts->hxfw = ts->hid_req_cfg.fw;
		I("get fw from hid_req_cfg");
		result = NO_ERR;
		goto OUT;
	}

	ret = request_firmware(&ts->hxfw, g_fw_boot_upgrade_name, ts->dev);
	I("request file %s finished", g_fw_boot_upgrade_name);
	if (ret < 0) {
		E("%d: error code = %d", __LINE__, ret);
		result = OPEN_FILE_FAIL;
	}

OUT:
	return result;
}

static int i_update_FW(struct himax_ts_data *ts)
{
	int upgrade_times = 0;
	s8 ret = 0;
	s8 result = 0;

update_retry:
	if (!ts->ic_data->has_flash) {
		ret = g_core_fp.fp_firmware_update_0f(ts->hxfw, ts, 0);
		if (ret != 0) {
			upgrade_times++;
			E("TP upgrade error, upgrade_times = %d",
			  upgrade_times);

			if (upgrade_times < 3)
				goto update_retry;
			else
				result = -1;

		} else {
			result = 1;/*upgrade success*/
			I("TP upgrade OK");
		}
	} else {
		if (ts->hxfw->size == FW_SIZE_32k)
			ret = g_core_fp.fp_fts_ctpm_fw_upgrade_with_sys_fs_32k(ts,
				(unsigned char *)ts->hxfw->data, ts->hxfw->size, false);
		else if (ts->hxfw->size == FW_SIZE_60k)
			ret = g_core_fp.fp_fts_ctpm_fw_upgrade_with_sys_fs_60k(ts,
				(unsigned char *)ts->hxfw->data, ts->hxfw->size, false);
		else if (ts->hxfw->size == FW_SIZE_64k)
			ret = g_core_fp.fp_fts_ctpm_fw_upgrade_with_sys_fs_64k(ts,
				(unsigned char *)ts->hxfw->data, ts->hxfw->size, false);
		else if (ts->hxfw->size == FW_SIZE_124k)
			ret = g_core_fp.fp_fts_ctpm_fw_upgrade_with_sys_fs_124k(ts,
				(unsigned char *)ts->hxfw->data, ts->hxfw->size, false);
		else if (ts->hxfw->size == FW_SIZE_128k)
			ret = g_core_fp.fp_fts_ctpm_fw_upgrade_with_sys_fs_128k(ts,
				(unsigned char *)ts->hxfw->data, ts->hxfw->size, false);
		else if (ts->hxfw->size == FW_SIZE_255k)
			ret = g_core_fp.fp_fts_ctpm_fw_upgrade_with_sys_fs_255k(ts,
				(unsigned char *)ts->hxfw->data, ts->hxfw->size, false);

		if (ret == 0) {
			upgrade_times++;
			E("TP upgrade error, upgrade_times = %d",
			  upgrade_times);

			if (upgrade_times < 3)
				goto update_retry;
			else
				result = -1;

		} else {
			result = 1;/*upgrade success*/
			I("TP upgrade OK");
		}
	}

	return result;
}

static int hx_hid_rd_init(struct himax_ts_data *ts)
{
	int ret = 0;
	const u32 x_num = ts->ic_data->HX_RX_NUM;
	const u32 y_num = ts->ic_data->HX_TX_NUM;
	unsigned int raw_data_sz = (x_num * y_num + x_num + y_num) * 2 + 4;
	u32 rd_sz = 0;

	if (ts->hid_req_cfg.input_RD_de == 0)
		rd_sz = ts->hid_desc.report_desc_length + host_ext_report_desc_sz;
	else
		rd_sz = host_heatmap_report_desc_sz + host_ext_report_desc_sz;

	if (FLASH_VER_GET_VAL(addr_hid_rd_desc) != 0) {
		if (ts->hid_rd_data.rd_data &&
		    rd_sz != ts->hid_rd_data.rd_length) {
			kfree(ts->hid_rd_data.rd_data);
			ts->hid_rd_data.rd_data = NULL;
		}

		if (!ts->hid_rd_data.rd_data)
			ts->hid_rd_data.rd_data = kzalloc(rd_sz, GFP_KERNEL);

		if (ts->hid_rd_data.rd_data) {
			if (ts->hid_req_cfg.input_RD_de == 0) {
				memcpy((void *)ts->hid_rd_data.rd_data,
				       &ts->hxfw->data[FLASH_VER_GET_VAL(addr_hid_rd_desc)],
				       ts->hid_desc.report_desc_length);
				ts->hid_rd_data.rd_length = ts->hid_desc.report_desc_length;
			} else {
				memcpy((void *)ts->hid_rd_data.rd_data,
				       g_heatmap_rd.host_report_descriptor,
				       host_heatmap_report_desc_sz);
				ts->hid_rd_data.rd_length = host_heatmap_report_desc_sz;
			}
			I("Re-assign HID DIAG size: original = %d, new = %d",
			  le16_to_cpu(g_host_ext_rd.rd_struct.monitor.report_cnt),
			  raw_data_sz);
			g_host_ext_rd.rd_struct.monitor.report_cnt = cpu_to_le16(raw_data_sz);
			memcpy((void *)(ts->hid_rd_data.rd_data + ts->hid_rd_data.rd_length),
			       &g_host_ext_rd.host_report_descriptor, host_ext_report_desc_sz);
			ts->hid_rd_data.rd_length += host_ext_report_desc_sz;
		} else {
			E("hid rd data alloc fail");
			ret = -ENOMEM;
		}
	}

	return ret;
}

static void hx_hid_register(struct himax_ts_data *ts)
{
	if (ts->hid_probe) {
		hid_destroy_device(ts->hid);
		ts->hid = NULL;
		ts->hid_probe = false;
	}

	if (hx_hid_probe(ts) != 0) {
		E("hid probe fail");
		ts->hid_probe = false;
	} else {
		I("hid probe success");
		ts->hid_probe = true;
	}
}

static int hx_hid_report_data_init(struct himax_ts_data *ts)
{
	int ret = 0;

	ts->touch_info_size = ts->hid_desc.max_input_length;
	I("base touch_info_size = %d", ts->touch_info_size);
	if (ts->ic_data->HX_STYLUS_FUNC) {
		ts->touch_info_size += ts->hid_desc.max_input_length;
		I("include stylus touch_info_size = %d", ts->touch_info_size);
	}
#if defined(HX_HEATMAP_EN)
	ts->touch_info_size += HEAT_MAP_HEADER_SZ;
	ts->touch_info_size += HEAT_MAP_INFO_SZ;
	if (!ts->ic_data->enc16bits)
		ts->heatmap_data_size =
			(ts->ic_data->HX_RX_NUM * ts->ic_data->HX_TX_NUM * 3) / 2;
	else
		ts->heatmap_data_size =
			(ts->ic_data->HX_RX_NUM * ts->ic_data->HX_TX_NUM * 2);
	ts->touch_info_size += ts->heatmap_data_size;
	I("include heatmap touch_info_size = %d", ts->touch_info_size);
#endif
	ts->touch_all_size = ts->touch_info_size;

	if (himax_report_data_init(ts) != 0) {
		E("report data init fail");
		ret = -ENOMEM;
	}

	return ret;
}

static void himax_boot_upgrade(struct work_struct *work)
{
	struct himax_ts_data *ts = container_of(work, struct himax_ts_data,
			work_boot_upgrade.work);
	int fw_sts = -1;
	bool upgrade_result = false;
	bool fw_load_status = false;

	I("Entering");
	ts->ic_boot_done = false;
	if (!ts->ic_data->has_flash) {
		ts->boot_upgrade_flag = true;
	} else {
		if (hx_chk_flash_sts(ts, ts->ic_data->flash_size) == 1) {
			E("check flash fail, please upgrade FW");
			goto END;
		} else {
			g_core_fp.fp_reload_disable(ts, 0);
			g_core_fp.fp_power_on_init(ts);
			g_core_fp.fp_read_FW_ver(ts);
			g_core_fp.fp_tp_info_check(ts);
		}
	}

	if (!ts->ic_data->has_flash) {
		fw_sts = i_get_FW(ts);
		if (fw_sts < NO_ERR)
			goto err_get_fw_failed;

		fw_load_status = g_core_fp.fp_bin_desc_get
			((unsigned char *)ts->hxfw->data, ts, HX1K);

		if (ts->boot_upgrade_flag) {
			if (i_update_FW(ts) <= 0) {
				E("Update FW fail");
				goto err_update_fw_failed;
			} else {
				I("Update FW success");
				if (!ts->has_alg_overlay) {
					g_core_fp.fp_reload_disable(ts, 0);
					g_core_fp.fp_power_on_init(ts);
				}
				// preload hid descriptors
				if (FLASH_VER_GET_VAL(addr_hid_desc) != 0) {
					memcpy(&ts->hid_desc,
					       &ts->hxfw->data[FLASH_VER_GET_VAL(addr_hid_desc)],
					       sizeof(struct hx_hid_desc_t));
					ts->hid_desc.desc_length =
						le16_to_cpu(ts->hid_desc.desc_length);
					ts->hid_desc.bcd_version =
						le16_to_cpu(ts->hid_desc.bcd_version);
					ts->hid_desc.report_desc_length =
						le16_to_cpu(ts->hid_desc.report_desc_length);
					ts->hid_desc.max_input_length =
						le16_to_cpu(ts->hid_desc.max_input_length);
					ts->hid_desc.max_output_length =
						le16_to_cpu(ts->hid_desc.max_output_length);
					ts->hid_desc.max_fragment_length =
						le16_to_cpu(ts->hid_desc.max_fragment_length);
					ts->hid_desc.vendor_id =
						le16_to_cpu(ts->hid_desc.vendor_id);
					ts->hid_desc.product_id =
						le16_to_cpu(ts->hid_desc.product_id);
					ts->hid_desc.version_id =
						le16_to_cpu(ts->hid_desc.version_id);
					ts->hid_desc.flags =
						le16_to_cpu(ts->hid_desc.flags);
				}
				g_core_fp.fp_tp_info_check(ts);
				g_core_fp.fp_read_FW_ver(ts);
				if (ts->pdata->pid != 0) {
					if (ts->pdata->pid != ts->hid_desc.product_id) {
						E("pid mismatch, dtsi pid = 0x%x, fw pid = 0x%x",
						  ts->pdata->pid, ts->hid_desc.product_id);
						goto err_pid_match_failed;
					} else {
						I("pid match, dtsi pid = 0x%x, fw pid = 0x%x",
						  ts->pdata->pid, ts->hid_desc.product_id);
					}
				}
				if (hx_hid_rd_init(ts) != 0) {
					E("hid rd init fail");
					goto err_hid_rd_init_failed;
				} else {
					I("hid RD init success");
					upgrade_result = true;
				}
			}

			if (upgrade_result) {
				ts->hid_req_cfg.handshake_get = FWUP_ERROR_BL_COMPLETE;
				mutex_unlock(&ts->hid_ioctl_lock);
				usleep_range(1000 * 1000, 1000 * 1000);
				hx_hid_register(ts);
				if (!ts->hid_probe) {
					goto err_hid_probe_failed;
				} else {
					if (hx_hid_report_data_init(ts) != 0) {
						E("report data init fail");
						goto err_report_data_init_failed;
					}
				}
			} else {
				ts->hid_req_cfg.handshake_get = FWUP_ERROR_FLASH_PROGRAMMING;
				mutex_unlock(&ts->hid_ioctl_lock);
			}
		} else {
			I("No need to upgrade FW");
			ts->hid_req_cfg.handshake_get = FWUP_ERROR_BL_COMPLETE;
			mutex_unlock(&ts->hid_ioctl_lock);
		}

		if (fw_sts == NO_ERR && !ts->hid_req_cfg.fw)
			release_firmware(ts->hxfw);
		ts->hxfw = NULL;
	}
END:
	ts->ic_boot_done = true;
	himax_int_enable(ts, true);

	return;

err_report_data_init_failed:
	hx_hid_remove(ts);
	ts->hid_probe = false;
err_hid_probe_failed:
err_hid_rd_init_failed:
err_pid_match_failed:
err_update_fw_failed:
	if (fw_sts == NO_ERR && !ts->hid_req_cfg.fw)
		release_firmware(ts->hxfw);
	ts->hxfw = NULL;
err_get_fw_failed:
	mutex_unlock(&ts->hid_ioctl_lock);
}

void hx_hid_update(struct work_struct *work)
{
	struct himax_ts_data *ts = container_of(work, struct himax_ts_data,
			work_hid_update.work);

	himax_int_enable(ts, false);

	if (ts->hid_req_cfg.input_RD_de == 0) {
		himax_boot_upgrade(&ts->work_boot_upgrade.work);
	} else {
		if (hx_hid_rd_init(ts) == 0) {
			I("hid rd init success");
			hx_hid_register(ts);
			if (ts->hid_probe)
				hx_hid_report_data_init(ts);
		}
		himax_int_enable(ts, true);
	}
}

int himax_report_data_init(struct himax_ts_data *ts)
{
	int ret = 0;

	kfree(ts->hx_rawdata_buf);
	ts->hx_rawdata_buf = NULL;

	ts->hx_rawdata_buf = kzalloc(ts->touch_info_size, GFP_KERNEL);
	if (!ts->hx_rawdata_buf) {
		E("ts->hx_rawdata_buf kzalloc failed!");
		ret = -ENOMEM;
		goto fail_1;
	}
#if defined(HX_HEATMAP_EN)
	kfree(ts->hx_heatmap_buf);
	ts->hx_heatmap_buf = NULL;

	ts->hx_heatmap_buf = kzalloc
		((ts->ic_data->HX_RX_NUM * ts->ic_data->HX_TX_NUM) * 2 +
		HEAT_MAP_INFO_SZ + 1, GFP_KERNEL);
	if (!ts->hx_heatmap_buf) {
		E("ts->hx_heatmap_buf kzalloc failed!");
		ret = -ENOMEM;
		goto fail_heatmap;
	}
#endif

	return 0;

#if defined(HX_HEATMAP_EN)
fail_heatmap:
#endif
	kfree(ts->hx_rawdata_buf);
	ts->hx_rawdata_buf = NULL;
fail_1:

	return ret;
}

void himax_report_data_deinit(struct himax_ts_data *ts)
{
#if defined(HX_HEATMAP_EN)
	kfree(ts->hx_heatmap_buf);
	ts->hx_heatmap_buf = NULL;
#endif
	kfree(ts->hx_rawdata_buf);
	ts->hx_rawdata_buf = NULL;
}

static void print_config(void)
{
#if defined(__HIMAX_MOD__)
	D("__HIMAX_MOD__ defined.");
#endif
#if defined(CONFIG_DRM_ROCKCHIP)
	D("CONFIG_DRM_ROCKCHIP defined.");
#endif
#if defined(CONFIG_FB)
	D("CONFIG_FB defined.");
#endif
#if defined(CONFIG_OF)
	D("CONFIG_OF defined.");
#endif
#if defined(CONFIG_ACPI)
	D("CONFIG_ACPI defined.");
#endif
#if defined(HW_ED_EXCP_EVENT)
	D("HW_ED_EXCP_EVENT defined.");
#endif
#if defined(HX_HEATMAP_EN)
	D("HX_HEATMAP_EN defined.");
#endif
#if defined(BUS_R_DLEN)
	D("BUS_R_DLEN defined : %d.", BUS_R_DLEN);
#endif
#if defined(BUS_W_DLEN)
	D("BUS_W_DLEN defined : %d.", BUS_W_DLEN);
#endif
#if defined(BOOT_UPGRADE_FWNAME)
	D("BOOT_UPGRADE_FWNAME defined : %s.", BOOT_UPGRADE_FWNAME);
#endif
#if defined(HIMAX_DRIVER_VER)
	D("HIMAX_DRIVER_VER defined : %s.", HIMAX_DRIVER_VER);
#endif
#if defined(HX_HID_PM)
	D("HX_HID_PM defined.");
#endif
}

int himax_chip_suspend(struct himax_ts_data *ts)
{
	int ret = 0;

	if (ts->suspended) {
		I("Already suspended, skip...");
		goto END;
	} else {
		ts->suspended = true;
	}

	I("enter");
	g_core_fp.fp_suspend_proc(ts, ts->suspended);

	himax_int_enable(ts, false);

	if (g_core_fp.fp_suspend_ic_action)
		g_core_fp.fp_suspend_ic_action(ts);

	if (!ts->use_irq) {
		s32 cancel_state;

		cancel_state = cancel_work_sync(&ts->work);
		if (cancel_state)
			himax_int_enable(ts, true);
	}

	atomic_set(&ts->suspend_mode, 1);

	if (ts->pdata) {
		if (ts->pdata->power_off_3v3) {
			if (ts->pdata->vcca_supply)
				ret = regulator_disable(ts->pdata->vcca_supply);
		}
	}

END:
	hx_hid_remove(ts);
	I("END");

	return 0;
}

int himax_chip_resume(struct himax_ts_data *ts)
{
	int ret = 0;

	if (!ts->suspended && ts->resume_success) {
		I("Already resumed, skip...");
		goto END;
	} else {
		ts->suspended = false;
	}
	ts->resume_success = false;

	I("enter");
	/* continuous N times record, not total N times. */
	ts->excp_zero_event_count = 0;

	atomic_set(&ts->suspend_mode, 0);
	if (ts->pdata) {
		if (ts->pdata->power_off_3v3) {
			if (ts->pdata->vcca_supply)
				ret = regulator_enable(ts->pdata->vcca_supply);
		}
	}

	g_core_fp.fp_resume_proc(ts, ts->suspended);
	// hx_report_all_leave_event(ts);
	if (ts->resume_success) {
		hx_hid_probe(ts);
		himax_int_enable(ts, true);
	} else {
		E("resume failed!");
		ret = -ECANCELED;
	}
END:
	I("END");

	return ret;
}

int himax_suspend(struct device *dev)
{
	struct himax_ts_data *ts = dev_get_drvdata(dev);

	I("enter");
	if (!ts->initialized) {
		E("init not ready, skip!");
		return -ECANCELED;
	}
	himax_chip_suspend(ts);
	return 0;
}

int himax_resume(struct device *dev)
{
	int ret = 0;
	struct himax_ts_data *ts = dev_get_drvdata(dev);

	I("enter");
	/*
	 *	wait until device resume for TDDI
	 *	TDDI: Touch and display Driver IC
	 */
	if (!ts->initialized) {
#if !defined(CONFIG_FB)
		if (himax_chip_init())
			return -ECANCELED;
#else
		E("init not ready, skip!");
		return -ECANCELED;
#endif
	}
	ret = himax_chip_resume(ts);
	if (ret < 0) {
		E("resume failed!");
		I("retry resume");
		schedule_delayed_work(&ts->work_resume_delayed_work,
				      msecs_to_jiffies(ts->pdata->ic_resume_delay));
		// I("try int rescue");
		// himax_int_enable(ts, 1);
	}

	return ret;
}

static void himax_resume_work_func(struct work_struct *work)
{
	struct himax_ts_data *ts = NULL;

	ts = container_of(work, struct himax_ts_data,
			  work_resume_delayed_work.work);
	if (!ts) {
		E("ts is NULL");
		return;
	}
	himax_chip_resume(ts);
}

#if defined(CONFIG_PM_SLEEP)
static const struct dev_pm_ops hx_hid_pm = {
	.suspend = himax_suspend,
	.resume = himax_resume,
	.restore = himax_resume,
};

#define HX_HID_PM (&hx_hid_pm)
#else
#define HX_HID_PM NULL
#endif

#if defined(CONFIG_OF)
static const struct of_device_id himax_match_table[] = {
	{ .compatible = "himax,hid" },
	{},
};
MODULE_DEVICE_TABLE(of, himax_match_table);
#define himax_match_table of_match_ptr(himax_match_table)
#else
#define himax_match_table NULL
#endif

#ifdef CONFIG_ACPI
static const struct acpi_device_id hx_acpi_spi_match[] = {
	{ "HXSPIHID", 0 }, // _CID name should be "HXSPIHID"
	{ },
};
MODULE_DEVICE_TABLE(acpi, hx_acpi_spi_match);
#define hx_acpi_spi_table ACPI_PTR(hx_acpi_spi_match)
#else
#define hx_acpi_spi_table NULL
#endif

int himax_chip_init(struct himax_ts_data *ts)
{
	int err = PROBE_FAIL;
	struct himax_platform_data *pdata = ts->pdata;

	ts->chip_max_dsram_size = 0;
	ts->notouch_frame = 0;
	ts->ic_notouch_frame = 0;

	if (g_core_fp.fp_chip_init) {
		g_core_fp.fp_chip_init(ts);
	} else {
		E("function point of chip_init is NULL!");
		goto error_ic_init_failed;
	}
	g_core_fp.fp_touch_information(ts);

	spin_lock_init(&ts->irq_lock);

	if (himax_ts_register_interrupt(ts)) {
		E("register interrupt failed");
		goto err_register_interrupt_failed;
	}

	himax_int_enable(ts, false);

	if (!ts->ic_data->has_flash) {
		ts->zf_update_cfg_buffer = kcalloc(ts->chip_max_dsram_size,
						   sizeof(u8), GFP_KERNEL);
		if (!ts->zf_update_cfg_buffer) {
			err = -ENOMEM;
			goto err_update_cfg_buf_adlled;
		}
	}

	if (!ts->ic_data->has_flash) {
		ts->himax_boot_upgrade_wq =
			create_singlethread_workqueue("HX_boot_upgrade");
		if (!ts->himax_boot_upgrade_wq) {
			E("allocate himax_boot_upgrade_wq failed");
			err = -ENOMEM;
			goto err_boot_upgrade_wq_failed;
		}
		INIT_DELAYED_WORK(&ts->work_boot_upgrade, himax_boot_upgrade);
		queue_delayed_work(ts->himax_boot_upgrade_wq, &ts->work_boot_upgrade,
				   msecs_to_jiffies(HX_DELAY_BOOT_UPDATE));
	}

	ts->himax_hid_debug_wq =
		create_singlethread_workqueue("HX_hid_debug");
	if (!ts->himax_hid_debug_wq) {
		E("allocate himax_hid_debug_wq failed");
		err = -ENOMEM;
		goto err_hid_debug_wq_failed;
	}
	INIT_DELAYED_WORK(&ts->work_self_test, hx_self_test);
	INIT_DELAYED_WORK(&ts->work_hid_update, hx_hid_update);

	ts->himax_resume_delayed_work_wq =
		create_singlethread_workqueue("HX_resume_delayed_work");
	if (!ts->himax_resume_delayed_work_wq) {
		E("allocate himax_resume_delayed_work_wq failed");
		err = -ENOMEM;
		goto err_resume_delayed_work_wq_failed;
	}
	INIT_DELAYED_WORK(&ts->work_resume_delayed_work, himax_resume_work_func);

	g_core_fp.fp_calc_touch_data_size(ts);

#if defined(CONFIG_OF)
	pdata->cable_config[0]             = 0xF0;
	pdata->cable_config[1]             = 0x00;
#endif

	ts->suspended                      = false;
	ts->usb_connected = 0x00;
	ts->cable_config = pdata->cable_config;
	ts->initialized = true;
	return 0;

	cancel_delayed_work_sync(&ts->work_resume_delayed_work);
	destroy_workqueue(ts->himax_resume_delayed_work_wq);
err_resume_delayed_work_wq_failed:
	cancel_delayed_work_sync(&ts->work_self_test);
	destroy_workqueue(ts->himax_hid_debug_wq);
err_hid_debug_wq_failed:
	cancel_delayed_work_sync(&ts->work_boot_upgrade);
	destroy_workqueue(ts->himax_boot_upgrade_wq);
err_boot_upgrade_wq_failed:
	kfree(ts->zf_update_cfg_buffer);
err_update_cfg_buf_adlled:
	himax_ts_unregister_interrupt(ts);
err_register_interrupt_failed:
error_ic_init_failed:
	ts->probe_fail_flag = 1;
	return err;
}

void himax_chip_deinit(struct himax_ts_data *ts)
{
	kfree(ts->zf_update_cfg_buffer);
	ts->zf_update_cfg_buffer = NULL;

	himax_ts_unregister_interrupt(ts);

	himax_report_data_deinit(ts);

	cancel_delayed_work_sync(&ts->work_resume_delayed_work);
	destroy_workqueue(ts->himax_resume_delayed_work_wq);

	cancel_delayed_work_sync(&ts->work_self_test);
	destroy_workqueue(ts->himax_hid_debug_wq);

	if (!ts->ic_data->has_flash) {
		cancel_delayed_work_sync(&ts->work_boot_upgrade);
		destroy_workqueue(ts->himax_boot_upgrade_wq);
	}
	ts->probe_fail_flag = 0;

	I("Common section deinited!");
}

static void himax_platform_deinit(struct himax_ts_data *ts)
{
	struct himax_platform_data *pdata = NULL;

	I("entering");

	if (!ts) {
		E("ts is NULL");
		return;
	}

	pdata = ts->pdata;
	if (!pdata) {
		E("pdata is NULL");
		return;
	}

	himax_gpio_power_deconfig(pdata);

	kfree(ts->ic_data);
	ts->ic_data = NULL;

	kfree(pdata);
	pdata = NULL;
	ts->pdata = NULL;

	kfree(ts->xfer_buff);
	ts->xfer_buff = NULL;

	I("exit");
}

static bool himax_platform_init(struct himax_ts_data *ts,
				struct himax_platform_data *local_pdata)
{
	int err = PROBE_FAIL;
	struct himax_platform_data *pdata;

	I("entering");
	ts->xfer_buff = kcalloc
		(HX_FULL_STACK_RAWDATA_SIZE, sizeof(u8), GFP_KERNEL);
	if (!ts->xfer_buff) {
		err = -ENOMEM;
		goto err_xfer_buff_fail;
	}

	I("PDATA START");
	pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
	if (!pdata) { /*Allocate Platform data space*/
		err = -ENOMEM;
		goto err_dt_platform_data_fail;
	}

	I("ts->ic_data START");
	ts->ic_data = kzalloc(sizeof(*ts->ic_data), GFP_KERNEL);
	if (!ts->ic_data) { /*Allocate IC data space*/
		err = -ENOMEM;
		goto err_dt_ic_data_fail;
	}
	memset(ts->ic_data, 0xFF, sizeof(struct himax_ic_data));
	/* default 128k, different size please follow HX83121A style */
	ts->ic_data->flash_size = 131072;

	memcpy(pdata, local_pdata, sizeof(struct himax_platform_data));
	ts->pdata = pdata;
	pdata->ts = ts;
	ts->rst_gpio = pdata->gpio_reset;

	if (himax_gpio_power_config(ts, pdata) < 0) {
		E("gpio config fail, exit!");
		goto err_power_config_failed;
	}

	I("Completed.");

	return true;

	himax_gpio_power_deconfig(pdata);
err_power_config_failed:
	kfree(ts->ic_data);
	ts->ic_data = NULL;
err_dt_ic_data_fail:
	kfree(pdata);
	pdata = NULL;
err_dt_platform_data_fail:
	kfree(ts->xfer_buff);
	ts->xfer_buff = NULL;
err_xfer_buff_fail:
	return false;
}

static struct himax_ts_data *get_ts(struct device *dev)
{
	struct list_head *listptr = NULL;
	struct himax_ts_data *ts = NULL;
	struct himax_ts_data *tmp_ts = NULL;

	if (!g_himax_ts->dev ||
	    g_himax_ts->dev == dev) {
		D("Found 1st device : %p", dev);
		return g_himax_ts;
	}

	D("Matching for device %p", dev);
	list_for_each(listptr, &g_himax_ts->list) {
		tmp_ts = list_entry(listptr, struct himax_ts_data, list);
		if (tmp_ts->dev == dev) {
			ts = tmp_ts;
			break;
		}
	}
	if (!ts)
		D("No matching device found");

	return ts;
}

int himax_spi_drv_probe(struct spi_device *spi)
{
	struct himax_ts_data *ts = NULL;
	int ret = 0;
	bool bret = false;
	static struct himax_platform_data pdata = {0};
#if !defined(__HIMAX_MOD__)
	struct time_var current_time;
#endif

	ts = get_ts(&spi->dev);
	if (!ts) {
		// non exist, create new one
		ts = kzalloc(sizeof(*ts), GFP_KERNEL);
		if (!ts) {
			E("allocate himax_ts_data failed");
			ret = -ENOMEM;
			goto err_alloc_data_failed;
		}
		list_add_tail(&ts->list, &g_himax_ts->list);
		I("Allocated himax_ts_data for new device %p", &spi->dev);
		ts->dev = &spi->dev;
	}
	if (ts == g_himax_ts)
		ts->dev = &spi->dev;
#if !defined(__HIMAX_MOD__)
	if ((ts->deferred_start.tv_sec != 0 ||
	     ts->deferred_start.tv_nsec != 0) && ts->ic_det_delay) {
		time_func(&current_time);
		if (time_diff(&ts->deferred_start, &current_time) <
			ts->ic_det_delay) {
			D("delay time not reach, defer probe");
			return -EPROBE_DEFER;
		}
		I("delay time reach, probe again!");
	}
#endif
	D("Enter");
	if (spi->master->flags & SPI_MASTER_HALF_DUPLEX) {
		E("Full duplex not supported by host");
		return -EIO;
	}
	pdata.ts = ts;
	ts->dev = &spi->dev;

#if defined(CONFIG_OF)
	if (himax_parse_dt(spi->dev.of_node, &pdata) < 0) {
		E(" parse OF data failed!");
		if (ts != g_himax_ts) {
			list_del(&ts->list);
			kfree(ts);
			D("free ts %p of dev %p", ts, &spi->dev);
		} else {
			ts->dev = NULL;
		}
		return -ENODEV;
	}
#elif defined(CONFIG_ACPI)
	if (himax_parse_acpi(&spi->dev, &pdata) < 0) {
		E(" parse acpi data failed!");
		if (ts != g_himax_ts) {
			list_del(&ts->list);
			kfree(ts);
			D("free ts %p of dev %p", ts, &spi->dev);
		} else {
			ts->dev = NULL;
		}
		return -ENODEV;
	}
#endif

#if !defined(__HIMAX_MOD__)
	if (pdata.ic_det_delay > 0) {
		if (ts->deferred_start.tv_sec == 0 &&
		    ts->deferred_start.tv_nsec == 0) {
			I("delay %d ms for IC detect",
			  pdata.ic_det_delay);
			ts->ic_det_delay = pdata.ic_det_delay;
			time_func(&ts->deferred_start);
			return -EPROBE_DEFER;
		}
	}
#endif

	ts->xfer_data = kzalloc(BUS_RW_MAX_LEN, GFP_KERNEL);
	if (!ts->xfer_data) {
		E("allocate xfer_data failed");
		ret = -ENOMEM;
		goto err_alloc_xfer_data_failed;
	}

	spi->bits_per_word = 8;
	spi->mode = SPI_MODE_3;
	spi->chip_select = 0;

	ts->spi = spi;
	mutex_init(&ts->rw_lock);
	mutex_init(&ts->reg_lock);
	mutex_init(&ts->hid_ioctl_lock);
	dev_set_drvdata(&spi->dev, ts);
	spi_set_drvdata(spi, ts);

	ts->probe_finish = false;
	ts->initialized = false;
	ts->ic_boot_done = false;
	bret = himax_platform_init(ts, &pdata);
	if (!bret) {
		E("platform init failed");
		ret = -ENODEV;
		goto error_platform_init_failed;
	}
	ts->ic_data->has_flash = !pdata.is_zf;

	bret = g_core_fp.fp_chip_detect(ts);
	if (!bret) {
		E("IC detect failed");
		ret = -ENODEV;
		goto error_ic_detect_failed;
	}

	ret = himax_chip_init(ts);
	if (ret < 0)
		goto err_init_failed;

#if defined(CONFIG_FB)
	ts->himax_att_wq = create_singlethread_workqueue("HMX_ATT_request");
	if (!ts->himax_att_wq) {
		E(" allocate himax_att_wq failed");
		ret = -ENOMEM;
		goto err_get_intr_bit_failed;
	}

	INIT_DELAYED_WORK(&ts->work_att, himax_fb_register);
	queue_delayed_work(ts->himax_att_wq, &ts->work_att,
			   msecs_to_jiffies(0));
#endif

	ts->himax_pwr_wq = create_singlethread_workqueue("HMX_PWR_request");
	if (!ts->himax_pwr_wq) {
		E(" allocate himax_pwr_wq failed");
		ret = -ENOMEM;
		goto err_create_pwr_wq_failed;
	}

	INIT_DELAYED_WORK(&ts->work_pwr, himax_pwr_register);
	queue_delayed_work(ts->himax_pwr_wq, &ts->work_pwr,
			   msecs_to_jiffies(0));

	ts->probe_finish = true;

	return ret;

err_create_pwr_wq_failed:
#if defined(CONFIG_FB)
	cancel_delayed_work_sync(&ts->work_att);
	destroy_workqueue(ts->himax_att_wq);
err_get_intr_bit_failed:
#endif
	himax_chip_deinit(ts);
err_init_failed:
error_ic_detect_failed:
	himax_platform_deinit(ts);
error_platform_init_failed:
	kfree(ts->xfer_data);
	ts->xfer_data = NULL;
err_alloc_xfer_data_failed:
	if (ts != g_himax_ts)
		list_del(&ts->list);
	kfree(ts);
	ts = NULL;
err_alloc_data_failed:

	return ret;
}

static void himax_spi_drv_remove(struct spi_device *spi)
{
	struct himax_ts_data *ts = spi_get_drvdata(spi);

	if (ts->probe_finish) {
		if (ts->ic_boot_done) {
			himax_int_enable(ts, false);

			if (ts->hid_probe) {
				hx_hid_remove(ts);
				ts->hid_probe = false;
			}

			kfree(ts->hid_rd_data.rd_data);
			ts->hid_rd_data.rd_data = NULL;

			ts->ic_boot_done = false;
		}
		power_supply_unreg_notifier(&ts->power_notif);
		cancel_delayed_work_sync(&ts->work_pwr);
		destroy_workqueue(ts->himax_pwr_wq);
	#if defined(CONFIG_FB)
		if (fb_unregister_client(&ts->fb_notif))
			E("Err occur while unregister fb_noti.");
		cancel_delayed_work_sync(&ts->work_att);
		destroy_workqueue(ts->himax_att_wq);
	#endif
		himax_chip_deinit(ts);
		himax_platform_deinit(ts);
		ts->pdata = NULL;
		kfree(ts->ovl_idx);
		ts->ovl_idx = NULL;
		kfree(ts->xfer_data);
		ts->xfer_data = NULL;
		ts->probe_fail_flag = 0;
		if (ts != g_himax_ts) {
			list_del(&ts->list);
			kfree(ts);
			ts = NULL;
		}
	}
	spi_set_drvdata(spi, NULL);

	I("completed.");
}

static struct spi_driver himax_hid_over_spi_driver = {
	.driver = {
		.name =		himax_dev_name,
		.owner =	THIS_MODULE,
		.pm	= HX_HID_PM,
		.of_match_table = himax_match_table,
		.acpi_match_table = hx_acpi_spi_table,
	},
	.probe =	himax_spi_drv_probe,
	.remove =	himax_spi_drv_remove,
};

int himax_spi_drv_init(struct himax_ts_data *ts)
{
	int ret;

	I("Himax touch panel driver for HID init");
	print_config();
	ret = spi_register_driver(&himax_hid_over_spi_driver);

	return ret;
}

void himax_spi_drv_exit(void)
{
	if (g_himax_ts) {
		spi_unregister_driver(&himax_hid_over_spi_driver);
		kfree(g_himax_ts);
		g_himax_ts = NULL;
		I("Free g_himax_ts");
	}
}
