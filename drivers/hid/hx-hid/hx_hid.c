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

#include <linux/hid.h>
#include "hx_core.h"
#include "hx_inspect.h"
#include "hx_plat.h"
#include "hx_ic_core.h"

union host_ext_rd_t g_host_ext_rd = {
	.host_report_descriptor = {
		0x06, 0x00, 0xFF,// Usage Page (Vendor-defined)
		0x09, 0x01,// Usage (0x1)
		0xA1, 0x01,// Collection (Application)
		0x75, 0x08,// Report Size (8)
		0x15, 0x00,// Logical Minimum (0)
		0x26, 0xFF, 0x00,// Logical Maximum (255)
		0x85, 0x05,// Report ID (5)
		0x09, 0x02,// Usage (0x2)
		0x96, 0xFF, 0x00,// Report Count (255)
		0xB1, 0x02,// Feature (ID: 5, sz: 2040 bits(255 bytes))
		0x85, 0x06,// Report ID (6)
		0x09, 0x02,// Usage (0x2)
		0x96, (HID_REG_SZ_MAX & 0xFF), (HID_REG_SZ_MAX >> 8),
		0xB1, 0x02,// Feature (ID: 6, sz: 72 bits(9 bytes))
		0x85, 0x07,// Report ID (7)
		0x09, 0x02,// Usage (0x2)
		0x96, 0x04, 0x00,// Report Count (4)
		0xB1, 0x02,// Feature (ID: 7, sz: 32 bits(4 bytes))
		0x85, 0x08,// Report ID (8)
		0x09, 0x02,// Usage (0x2)
		0x96, 0x8D, 0x13,// Report Count (5005)
		0xB1, 0x02,// Feature (ID: 8, sz: 40040 bits(5005 bytes))
		// 0x85, 0x09,// Report ID (9)
		// 0x09, 0x02,// Usage (0x2)
		// 0x96, 0x4F, 0x03,// Report Count (847)
		// 0xB1, 0x02,// Feature (ID: 9, sz: 6776 bits(847 bytes))
		0x85, 0x0A,// Report ID (10)
		0x09, 0x02,// Usage (0x2)
		0x96, 0x00, 0x04,// Report Count (1024)
		0x91, 0x02,// Output (ID: 10, sz: 8192 bits(1024 bytes))
		0x85, 0x0B,// Report ID (11)
		0x09, 0x02,// Usage (0x2)
		0x96, 0x01, 0x00,// Report Count (1)
		0xB1, 0x02,// Feature (ID: 11, sz: 8 bits(1 bytes))
		0x85, 0x0C,// Report ID (12)
		0x09, 0x02,// Usage (0x2)
		0x96, 0x01, 0x00,// Report Count (1)
		0xB1, 0x02,// Feature (ID: 12, sz: 8 bits(1 bytes))
		0x85, 0x31,// Report ID (49)
		0x09, 0x02,// Usage (0x2)
		0x96, 0x01, 0x00,// Report Count (1)
		0xB1, 0x02,// Feature (ID: 49, sz: 8 bits(1 bytes))
		0xC0,// End Collection
	},
};

const unsigned int host_ext_report_desc_sz =
	sizeof(g_host_ext_rd.host_report_descriptor);

union heatmap_rd_t g_heatmap_rd = {
	.host_report_descriptor = {
		0x05, 0x0D,// Usage Page (Digitizers)
		0x09, 0x0F,// Usage (0xF)
		0xA1, 0x01,// Collection (Application)
		0x85, 0x61,// Report ID (97)
		0x05, 0x0D,// Usage Page (Digitizers)
		0x15, 0x00,// Logical Minimum (0)
		0x27, 0xFF, 0xFF, 0x00, 0x00,// Logical Maximum (65535)
		0x75, 0x10,// Report Size (16)
		0x95, 0x01,// Report Count (1)
		0x09, 0x6A,// Usage (0x6A)
		0x81, 0x02,// Input (ID: 97, sz: 16 bits(2 bytes))
		0x09, 0x6B,// Usage (0x6B)
		0x81, 0x02,// Input (ID: 97, sz: 16 bits(2 bytes))
		0x27, 0xFF, 0xFF, 0xFF, 0xFF,// Logical Maximum (-1)
		0x75, 0x20,// Report Size (32)
		0x09, 0x56,// Usage (0x56)
		0x81, 0x02,// Input (ID: 97, sz: 32 bits(4 bytes))
		0x05, 0x01,// Usage Page (Generic Desktop)
		0x09, 0x3B,// Usage (0x3B)
		0x81, 0x02,// Input (ID: 97, sz: 32 bits(4 bytes))
		0x05, 0x0D,// Usage Page (Digitizers)
		0x26, 0xFF, 0x00,// Logical Maximum (255)
		0x09, 0x6C,// Usage (0x6C)
		0x75, 0x08,// Report Size (8)
		0x96, 0x00, 0x0C,// Report Count (3072)
		0x81, 0x02,// Input (ID: 97, sz: 24576 bits(3072 bytes))
		0xC0,// End Collection
	},
};

const unsigned int host_heatmap_report_desc_sz =
	sizeof(g_heatmap_rd.host_report_descriptor);

static const struct hx_hid_fw_unit_t default_main_121A[9] = {
	{
		.cmd = 0xA1,
		.bin_start_offset = 0,
		.unit_sz = 127,
	},
	{
		.cmd = 0xA2,
		.bin_start_offset = 129,
		.unit_sz = 111,
	},
};

static const u16 g_hx_hid_raw_data_type[HX_HID_RAW_DATA_TYPE_MAX] = {
	HID_RAW_DATA_TYPE_DELTA,
	HID_RAW_DATA_TYPE_RAW,
	HID_RAW_DATA_TYPE_BASELINE,
	HID_RAW_DATA_TYPE_NORMAL
};

static int hx_hid_parse(struct hid_device *hid)
{
	struct himax_ts_data *ts = NULL;
	int ret;

	if (!hid) {
		E("hid is NULL");
		return -EINVAL;
	}

	ts = hid->driver_data;
	if (!ts) {
		E("hid->driver_data is NULL");
		return -EINVAL;
	}

	ret = hid_parse_report(hid, ts->hid_rd_data.rd_data,
			       ts->hid_rd_data.rd_length);
	if (ret) {
		E("failed parse report");
		return	ret;
	}
	I("rdesc parse success");
	return 0;
}

static int hx_hid_start(struct hid_device *hid)
{
	D("enter");
	return 0;
}

static void hx_hid_stop(struct hid_device *hid)
{
	D("enter");
}

static int hx_hid_open(struct hid_device *hid)
{
	D("enter");
	return 0;
}

static void hx_hid_close(struct hid_device *hid)
{
	D("enter");
}

void hx_hid_update_info(struct himax_ts_data *ts)
{
	memcpy(&ts->hid_info.fw_bin_desc, &ts->fw_bin_desc, sizeof(struct hx_bin_desc_t));
	ts->hid_info.vid = cpu_to_be16(ts->hid_desc.vendor_id);
	ts->hid_info.pid = cpu_to_be16(ts->hid_desc.product_id);
	ts->hid_info.cfg_version = ts->ic_data->vendor_touch_cfg_ver;
	ts->hid_info.disp_version = ts->ic_data->vendor_display_cfg_ver;
	ts->hid_info.rx = ts->ic_data->HX_RX_NUM;
	ts->hid_info.tx = ts->ic_data->HX_TX_NUM;
	ts->hid_info.yres = cpu_to_be16(ts->ic_data->HX_Y_RES);
	ts->hid_info.xres = cpu_to_be16(ts->ic_data->HX_X_RES);
	ts->hid_info.pt_num = ts->ic_data->HX_MAX_PT;
	ts->hid_info.mkey_num = ts->ic_data->HX_BT_NUM;

	// firmware table parameters, use only bl part.
	ts->hid_info.bl_mapping.cmd = HID_FW_UPDATE_BL_CMD;
	ts->hid_info.bl_mapping.bin_start_offset = 0;
	ts->hid_info.bl_mapping.unit_sz = ts->ic_data->flash_size / 1024;
}

static void free_firmware(struct firmware *fw)
{
	if (fw) {
		kfree(fw->data);
		kfree(fw->priv);
		kfree(fw);
	}
}

static int hx_hid_load_user_firmware(struct himax_ts_data *ts,
				     u8 *fwdata, size_t sz)
{
	int ret = 0;

	if (ts->hid_req_cfg.fw) {
		if (ts->hid_req_cfg.fw->size == ts->ic_data->flash_size) {
			I("free old fw");
			free_firmware(ts->hid_req_cfg.fw);
			ts->hid_req_cfg.fw = NULL;
		}
	}

	if (!ts->hid_req_cfg.fw) {
		ts->hid_req_cfg.fw = kzalloc(sizeof(*ts->hid_req_cfg.fw), GFP_KERNEL);
		if (!ts->hid_req_cfg.fw) {
			E("Allocate firmware failed");
			ret = -ENOMEM;
			goto ERR_OUT;
		}
		ts->hid_req_cfg.fw->data = kzalloc(ts->ic_data->flash_size, GFP_KERNEL);
		if (!ts->hid_req_cfg.fw->data) {
			E("kzalloc failed");
			kfree(ts->hid_req_cfg.fw);
			ts->hid_req_cfg.fw = NULL;
			ret = -ENOMEM;
			goto ERR_OUT;
		}
	}

	memcpy((u8 *)ts->hid_req_cfg.fw->data + ts->hid_req_cfg.fw->size,
	       (u8 *)fwdata + 1, sz - 1);
	ts->hid_req_cfg.fw->size += sz - 1;
	if (ts->hid_req_cfg.fw->size == ts->ic_data->flash_size) {
		I("load firmware done");
		ret = 2;
	} else {
		I("still loading firmware...");
		ret = 1;
	}

ERR_OUT:
	return ret;
}

static int hx_hid_get_raw_report(const struct hid_device *hid, unsigned char reportnum,
				 __u8 *buf, size_t len, unsigned char report_type)
{
	struct himax_ts_data *ts = NULL;
	int ret = 0;
	u32 tmp_data = 0;
	union hx_dword_data_t *tmp = NULL;

	ts = hid->driver_data;
	if (!ts) {
		E("hid->driver_data is NULL");
		return -EINVAL;
	}

	D("reportnum:%d, len:%lu, report_type:%d", reportnum, len, report_type);

	switch (reportnum) {
	case ID_CONTACT_COUNT:
		if (!ts->ic_data) {
			E("ts->ic_data is NULL");
			return -EINVAL;
		}
		buf[1] = ts->ic_data->HX_MAX_PT;
		ret = len;
		break;
	case ID_CFG:
		memcpy(buf + 1, &ts->hid_info, sizeof(struct hx_hid_info_t));
		ret = len;
		break;
	case ID_FW_UPDATE_HANDSHAKING:
		I("id: %X, cmd: %X", buf[0], buf[1]);
		if (ts->hid_req_cfg.processing_id == ID_FW_UPDATE_HANDSHAKING) {
			if (ts->hid_req_cfg.handshake_set == ts->hid_info.bl_mapping.cmd) {
				ts->hid_req_cfg.handshake_get = ts->hid_req_cfg.handshake_set;
			} else if (ts->hid_req_cfg.handshake_set == HID_FW_UPDATE_MAIN_CMD) {
				ts->hid_req_cfg.handshake_get = default_main_121A[0].cmd;
				ts->hid_req_cfg.current_size = 0;
			} else if (ts->hid_req_cfg.handshake_set == default_main_121A[0].cmd) {
				if (ts->hid_req_cfg.current_size >= default_main_121A[0].unit_sz) {
					ts->hid_req_cfg.handshake_get = default_main_121A[1].cmd;
					ts->hid_req_cfg.current_size = 0;
				}
			} else if (ts->hid_req_cfg.handshake_set == default_main_121A[1].cmd) {
				if (ts->hid_req_cfg.current_size >= default_main_121A[1].unit_sz) {
					ts->hid_req_cfg.handshake_get = FWUP_ERROR_BL_COMPLETE;
					ts->hid_req_cfg.current_size = 0;
				}
			} else {
				// ts->hid_req_cfg.handshake_get = FWUP_ERROR_NO_MAIN;
				ts->hid_req_cfg.handshake_get = FWUP_ERROR_NO_ERROR;
			}
			buf[1] = ts->hid_req_cfg.handshake_get;
		} else if (ts->hid_req_cfg.processing_id == ID_FW_UPDATE) {
			mutex_lock(&ts->hid_ioctl_lock);
			buf[1] = ts->hid_req_cfg.handshake_get;
			mutex_unlock(&ts->hid_ioctl_lock);
		} else {
			buf[1] = FWUP_ERROR_NO_ERROR;
		}
		ret = len;
		break;
	case ID_SELF_TEST:
		mutex_lock(&ts->hid_ioctl_lock);
		buf[1] = ts->hid_req_cfg.handshake_get;
		ret = len;
		himax_int_enable(ts, true);
		mutex_unlock(&ts->hid_ioctl_lock);
		break;
	case ID_TOUCH_MONITOR:
		ret = hx_get_data(ts, &buf[2], len - 2);
		if (ret == HX_INSP_OK)
			ret = len;
		else
			ret = 0;

		// dummy byte for hx_util
		buf[1] = 0;
		break;
	case ID_TOUCH_MONITOR_SEL:
		ret = g_core_fp.fp_diag_register_get(ts, &tmp_data);
		if (ret == NO_ERR) {
			buf[1] = tmp_data & 0xFF;
			ret = len;
		} else {
			ret = 0;
		}

		break;
	case ID_REG_RW:
		if (len == 10 &&
		    ((union hx_dword_data_t *)&buf[2])->dword != REG_TYPE_EXT_TYPE) {
			// standard REG RW
			ts->hid_req_cfg.reg_addr_sz = 4;
			ts->hid_req_cfg.reg_data_sz = 4;
			ts->hid_req_cfg.reg_addr.dword =
				((union hx_dword_data_t *)&buf[2])->dword;
			ts->hid_req_cfg.reg_addr.dword =
				cpu_to_le32(ts->hid_req_cfg.reg_addr.dword);
			ret = g_core_fp.fp_register_read(ts,
				ts->hid_req_cfg.reg_addr.byte,
				ts->hid_req_cfg.reg_data,
				ts->hid_req_cfg.reg_data_sz);
			if (ret == NO_ERR) {
				tmp = (union hx_dword_data_t *)ts->hid_req_cfg.reg_data;
				tmp->dword = le32_to_cpu(tmp->dword);
				memcpy(&buf[6], ts->hid_req_cfg.reg_data,
				       ts->hid_req_cfg.reg_data_sz);
				ret = len;
			} else {
				ret = 0;
			}
		} else if ((len >= 9) && (len <= (1 + HID_REG_SZ_MAX)) &&
			(((union hx_dword_data_t *)&buf[2])->dword == REG_TYPE_EXT_TYPE)) {
			// ext type REG RW
			switch (buf[6]) {
			case REG_TYPE_EXT_AHB:
				ts->hid_req_cfg.reg_addr_sz = 1;
				ts->hid_req_cfg.reg_data_sz = len - 1 - 1 - 4 - 1 - 1;
				ts->hid_req_cfg.reg_addr.dword = buf[7];
				ret = himax_bus_read(ts, ts->hid_req_cfg.reg_addr.dword,
						     ts->hid_req_cfg.reg_data,
						     ts->hid_req_cfg.reg_data_sz);
				if (ret == 0) {
					memcpy(&buf[8], ts->hid_req_cfg.reg_data,
					       ts->hid_req_cfg.reg_data_sz);
					ret = len;
				}
				break;
			case REG_TYPE_EXT_SRAM:
				ts->hid_req_cfg.reg_addr_sz = 4;
				ts->hid_req_cfg.reg_data_sz = len - 1 - 1 - 4 - 1 - 4;
				ts->hid_req_cfg.reg_addr.dword =
					((union hx_dword_data_t *)&buf[7])->dword;
				ts->hid_req_cfg.reg_addr.dword =
					cpu_to_le32(ts->hid_req_cfg.reg_addr.dword);
				ret = g_core_fp.fp_register_read(ts,
					ts->hid_req_cfg.reg_addr.byte,
					ts->hid_req_cfg.reg_data,
					ts->hid_req_cfg.reg_data_sz);
				if (ret == NO_ERR) {
					memcpy(&buf[11], ts->hid_req_cfg.reg_data,
					       ts->hid_req_cfg.reg_data_sz);
					ret = len;
				} else {
					ret = 0;
				}
				break;
			default:
				E("Invalid ext type");
				return -EINVAL;
			}
		} else {
			E("Invalid reg format!");
			return -EINVAL;
		}
		break;
	case ID_INPUT_RD_DE:
		buf[1] = ts->hid_req_cfg.input_RD_de;
	    ret = len;
		break;
	// case ID_TOUCH_MONITOR_PARTIAL:
	case ID_FW_UPDATE:
	    ret = 0;
		break;
	default:
		ret = -EINVAL;
	};

	if (ret > 0)
		D("ret:%d", ret);

	return ret;
}

static int hx_hid_set_raw_report(const struct hid_device *hid, unsigned char reportnum,
				 __u8 *buf, size_t len,	unsigned char report_type)
{
	int ret = 0;
	struct himax_ts_data *ts = NULL;
	unsigned int i = 0;
	union hx_dword_data_t *tmp_data = NULL;

	ts = hid->driver_data;
	if (!ts) {
		E("hid->driver_data is NULL");
		return -EINVAL;
	}
	D("reportnum:%d, len:%lu, report_type:%d", reportnum, len, report_type);

	switch (reportnum) {
	case ID_FW_UPDATE:
		if (ts->hid_req_cfg.processing_id == ID_FW_UPDATE_HANDSHAKING) {
			if (ts->hid_req_cfg.handshake_get == default_main_121A[0].cmd) {
				ts->hid_req_cfg.handshake_set = default_main_121A[0].cmd;
				ts->hid_req_cfg.current_size += len - 1;
				return 0;
			} else if (ts->hid_req_cfg.handshake_get == default_main_121A[1].cmd) {
				ts->hid_req_cfg.handshake_set = default_main_121A[1].cmd;
				ts->hid_req_cfg.current_size += len - 1;
				return 0;
			}
		}
		ret = hx_hid_load_user_firmware(ts, buf, len);
		if (ret < 0) {
			E("load user firmware failed");
			goto END;
		} else if (ret == 1) {
			I("Still loading firmware...");
			ret = 0;
			goto END;
		} else {
			I("load user firmware succeeded");
		}

		ts->hid_req_cfg.processing_id = ID_FW_UPDATE;
		ts->hid_req_cfg.handshake_get = FWUP_ERROR_FLASH_PROGRAMMING;
		mutex_lock(&ts->hid_ioctl_lock);
		queue_delayed_work(ts->himax_boot_upgrade_wq, &ts->work_boot_upgrade,
				   msecs_to_jiffies(0));
		break;
	case ID_FW_UPDATE_HANDSHAKING:
		I("id: %X, cmd: %X", buf[0], buf[1]);
		ts->hid_req_cfg.processing_id = ID_FW_UPDATE_HANDSHAKING;
		ts->hid_req_cfg.handshake_set = buf[1];
		break;
	case ID_SELF_TEST:
		ts->hid_req_cfg.processing_id = ID_SELF_TEST;
		ts->hid_req_cfg.handshake_set = buf[1];
		I("id: %X, cmd: %X", buf[0], buf[1]);
		switch (buf[1]) {
		case HID_SELF_TEST_SHORT:
			ts->hid_req_cfg.self_test_type = HX_SHORT;
			break;
		case HID_SELF_TEST_OPEN:
			ts->hid_req_cfg.self_test_type = HX_OPEN;
			break;
		case HID_SELF_TEST_MICRO_OPEN:
			ts->hid_req_cfg.self_test_type = HX_MICRO_OPEN;
			break;
		case HID_SELF_TEST_RAWDATA:
			ts->hid_req_cfg.self_test_type = HX_RAWDATA;
			break;
		case HID_SELF_TEST_NOISE:
			ts->hid_req_cfg.self_test_type = HX_ABS_NOISE;
			break;
		case HID_SELF_TEST_RESET:
			ts->hid_req_cfg.self_test_type = HX_BACK_NORMAL;
			break;
		default:
			I("Not support self test type, set to default(HX_BACK_NORMAL)");
			ts->hid_req_cfg.self_test_type = HX_BACK_NORMAL;
		}
		if (ts->hid_req_cfg.self_test_type == HX_BACK_NORMAL) {
			hx_switch_data_type(ts, HX_BACK_NORMAL);
			himax_int_enable(ts, false);
			g_core_fp.fp_reload_disable(ts, 0);
			g_core_fp.fp_power_on_init(ts);
			himax_int_enable(ts, true);
			break;
		}
		mutex_lock(&ts->hid_ioctl_lock);
		himax_int_enable(ts, false);
		queue_delayed_work(ts->himax_hid_debug_wq, &ts->work_self_test,
				   msecs_to_jiffies(0));
		break;
	case ID_TOUCH_MONITOR_SEL:
		I("id: %X, cmd: %X", buf[0], buf[1]);
		for (i = 0; i < HX_HID_RAW_DATA_TYPE_MAX; i++) {
			if (buf[1] == g_hx_hid_raw_data_type[i]) {
				g_core_fp.fp_diag_register_set(ts, buf[1]);
				break;
			}
		}
		if (i == HX_HID_RAW_DATA_TYPE_MAX) {
			E("Not support data type");
			return -EINVAL;
		}
		ts->hid_req_cfg.processing_id = ID_TOUCH_MONITOR_SEL;
		ts->hid_req_cfg.handshake_set = buf[1];
		break;
	case ID_REG_RW:
		if (len == 10 &&
		    ((union hx_dword_data_t *)&buf[2])->dword != REG_TYPE_EXT_TYPE) {
			// standard REG RW
			if (buf[1] == REG_READ)
				return 0;
			ts->hid_req_cfg.reg_addr_sz = 4;
			ts->hid_req_cfg.reg_data_sz = 4;
			ts->hid_req_cfg.reg_addr.dword =
				((union hx_dword_data_t *)&buf[2])->dword;
			ts->hid_req_cfg.reg_addr.dword =
				cpu_to_le32(ts->hid_req_cfg.reg_addr.dword);
			memcpy(ts->hid_req_cfg.reg_data, &buf[6], 4);
			tmp_data = (union hx_dword_data_t *)(ts->hid_req_cfg.reg_data);
			tmp_data->dword = cpu_to_le32(tmp_data->dword);
			ret = g_core_fp.fp_register_write(ts,
				ts->hid_req_cfg.reg_addr.byte,
				ts->hid_req_cfg.reg_data, 4);
		} else if ((len >= 9) && (len <= (1 + HID_REG_SZ_MAX)) &&
			(((union hx_dword_data_t *)&buf[2])->dword == REG_TYPE_EXT_TYPE)) {
			// ext type REG RW
			if (buf[1] == REG_READ)
				return 0;
			switch (buf[6]) {
			case REG_TYPE_EXT_AHB:
				ts->hid_req_cfg.reg_addr_sz = 1;
				ts->hid_req_cfg.reg_data_sz = len - 1 - 1 - 4 - 1 - 1;
				ts->hid_req_cfg.reg_addr.dword = buf[7];
				memcpy(ts->hid_req_cfg.reg_data, &buf[8],
				       ts->hid_req_cfg.reg_data_sz);
				ret = himax_bus_write(ts, ts->hid_req_cfg.reg_addr.dword, NULL,
						      ts->hid_req_cfg.reg_data,
						      ts->hid_req_cfg.reg_data_sz);
				break;
			case REG_TYPE_EXT_SRAM:
				ts->hid_req_cfg.reg_addr_sz = 4;
				ts->hid_req_cfg.reg_data_sz = len - 1 - 1 - 4 - 1 - 4;
				ts->hid_req_cfg.reg_addr.dword =
					((union hx_dword_data_t *)&buf[7])->dword;
				ts->hid_req_cfg.reg_addr.dword =
					cpu_to_le32(ts->hid_req_cfg.reg_addr.dword);
				memcpy(ts->hid_req_cfg.reg_data, &buf[11],
				       ts->hid_req_cfg.reg_data_sz);
				ret = g_core_fp.fp_register_write(ts,
					ts->hid_req_cfg.reg_addr.byte,
					ts->hid_req_cfg.reg_data,
					ts->hid_req_cfg.reg_data_sz);
				break;
			default:
				E("Invalid ext type");
				return -EINVAL;
			}
		} else {
			E("Invalid reg format!");
			return -EINVAL;
		}
		ts->hid_req_cfg.processing_id = ID_REG_RW;
		ts->hid_req_cfg.handshake_set = ts->hid_req_cfg.reg_addr.dword;
		break;
	case ID_INPUT_RD_DE:
		ts->hid_req_cfg.processing_id = ID_INPUT_RD_DE;
		ts->hid_req_cfg.handshake_set = !!buf[1];
		if (ts->hid_req_cfg.input_RD_de != (!!buf[1])) {
			ts->hid_req_cfg.input_RD_de = !!buf[1];

			queue_delayed_work(ts->himax_hid_debug_wq, &ts->work_hid_update,
					   msecs_to_jiffies(0));
		}
		break;
	case ID_CONTACT_COUNT:
	case ID_CFG:
	case ID_TOUCH_MONITOR:
	// case ID_TOUCH_MONITOR_PARTIAL:
		ret = 0;
		break;
	default:
		ret = -EINVAL;
	};

END:
	return ret;
}

static int hx_raw_request(struct hid_device *hid, unsigned char reportnum,
			  __u8 *buf, size_t len, unsigned char rtype, int reqtype)
{
	if (!hid) {
		E("hid is NULL");
		return -EINVAL;
	}

	D("report num %d, len %lu, rtype %d, reqtype %d", reportnum, len, rtype, reqtype);
	switch (reqtype) {
	case HID_REQ_GET_REPORT:
		return hx_hid_get_raw_report(hid, reportnum, buf, len, rtype);
	case HID_REQ_SET_REPORT:
		if (buf[0] != reportnum)
			return -EINVAL;
		return hx_hid_set_raw_report(hid, reportnum, buf, len, rtype);
	default:
		return -EIO;
	}

	return -EINVAL;
}

static struct hid_ll_driver hx_hid_ll_driver = {
	.parse = hx_hid_parse,
	.start = hx_hid_start,
	.stop = hx_hid_stop,
	.open = hx_hid_open,
	.close = hx_hid_close,
	.raw_request = hx_raw_request
};

#define HX_HID_HEADER_LEN	3
#define HX_HID_COOR_LEN	84
#define HX_HID_DIFF_LEN	(4083 + 12)
#define HX_HID_MAX_INPUT_LEN  (HX_HID_HEADER_LEN + HX_HID_COOR_LEN + HX_HID_DIFF_LEN)

int hx_hid_report(const struct himax_ts_data *ts, u8 *data, s32 len)
{
	int ret = 0;

	if (ts->hid)
		ret = hid_input_report(ts->hid, HID_INPUT_REPORT, data, len, 1);

	return ret;
}

static int hx_hid_desc_fetch(struct himax_ts_data *ts)
{
	if (!ts)
		return -EINVAL;

	I("desc_length:           %d", ts->hid_desc.desc_length);
	I("bcd_version:           0x%x", ts->hid_desc.bcd_version);
	I("report_desc_length:    %d", ts->hid_desc.report_desc_length);
	I("max_input_length:      %d", ts->hid_desc.max_input_length);
	I("max_output_length:     %d", ts->hid_desc.max_output_length);
	I("max fragment length:   %d", ts->hid_desc.max_fragment_length);
	I("vendor_id:             0x%x", ts->hid_desc.vendor_id);
	I("product_id:            0x%x", ts->hid_desc.product_id);
	I("version_id:            0x%x", ts->hid_desc.version_id);
	I("flags:                 0x%x", ts->hid_desc.flags);

	return 0;
}

int hx_hid_probe(struct himax_ts_data *ts)
{
	int ret;
	struct hid_device *hid = NULL;

	if (!ts) {
		E("ts is NULL");
		return -EINVAL;
	}

	ret = hx_hid_desc_fetch(ts);
	if (ret) {
		E("failed get hid desc");
		return ret;
	}

	hid = ts->hid;
	if (hid) {
		hid_destroy_device(hid);
		hid = NULL;
	}

	hid = hid_allocate_device();
	if (IS_ERR(hid)) {
		ret = PTR_ERR(hid);
		return ret;
	}

	hid->driver_data = ts;
	hid->ll_driver = &hx_hid_ll_driver;
	hid->bus = BUS_SPI;
	hid->dev.parent = &ts->spi->dev;

	hid->version = ts->hid_desc.bcd_version;
	hid->vendor = ts->hid_desc.vendor_id;
	hid->product = ts->hid_desc.product_id;
	snprintf(hid->name, sizeof(hid->name), "%s %04X:%04X", "hid-hxtp",
		 hid->vendor, hid->product);

	ret = hid_add_device(hid);
	if (ret) {
		E("failed add hid device");
		goto err_hid_data;
	}
	I("hid init success");
	ts->hid = hid;
	mutex_unlock(&ts->hid_ioctl_lock);
	return 0;

err_hid_data:
	hid_destroy_device(hid);
	return ret;
}

void hx_hid_remove(struct himax_ts_data *ts)
{
	mutex_lock(&ts->hid_ioctl_lock);
	if (ts && ts->hid) {
		hid_destroy_device(ts->hid);
	} else {
		D("ts or hid is NULL");
		goto OUT;
	}
	ts->hid = NULL;

	if (ts->hid_req_cfg.fw) {
		I("free fw");
		kfree(ts->hid_req_cfg.fw->data);
		kfree(ts->hid_req_cfg.fw->priv);
		kfree(ts->hid_req_cfg.fw);
		ts->hid_req_cfg.fw = NULL;
	}
OUT:
	mutex_unlock(&ts->hid_ioctl_lock);
}
