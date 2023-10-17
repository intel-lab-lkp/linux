/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __HX_HID_H__
#define __HX_HID_H__
// #include "hx_core.h"

enum HID_ID_FUNCT {
	ID_CONTACT_COUNT = 0x03,
	ID_CFG = 0x05,
	ID_REG_RW = 0x06,
	ID_TOUCH_MONITOR_SEL = 0x07,
	ID_TOUCH_MONITOR = 0x08,
	// ID_TOUCH_MONITOR_PARTIAL = 0x09,
	ID_FW_UPDATE = 0x0A,
	ID_FW_UPDATE_HANDSHAKING = 0x0B,
	ID_SELF_TEST = 0x0C,
	ID_INPUT_RD_DE = 0x31,
};

enum HID_SELF_TEST_TYPE {
	HID_SELF_TEST_SHORT = 0x11,
	HID_SELF_TEST_OPEN = 0x12,
	HID_SELF_TEST_MICRO_OPEN = 0x13,
	HID_SELF_TEST_RAWDATA = 0x21,
	HID_SELF_TEST_NOISE = 0x22,

	HID_SELF_TEST_RESET = 0x01,
};

enum HID_SELF_TEST_STATUS {
	HID_SELF_TEST_INIT = 0xF1,
	HID_SELF_TEST_START = 0xF2,
	HID_SELF_TEST_ONGOINIG = 0xF3,
	HID_SELF_TEST_FINISH = 0xFF,

	HID_SELF_TEST_NOT_SUPPORT = 0xE1,
	HID_SELF_TEST_ERROR = 0xEF
};

#define HID_RAW_DATA_TYPE_DELTA     (0x09)
#define HID_RAW_DATA_TYPE_RAW       (0x0A)
#define HID_RAW_DATA_TYPE_BASELINE  (0x0B)
#define HID_RAW_DATA_TYPE_NORMAL	(0x00)

enum HID_RAW_DATA_TYPE {
	HX_HID_RAW_DATA_TYPE_DELTA,
	HX_HID_RAW_DATA_TYPE_RAW,
	HX_HID_RAW_DATA_TYPE_BASELINE,
	HX_HID_RAW_DATA_TYPE_NORMAL,
	HX_HID_RAW_DATA_TYPE_MAX
};

enum HID_FW_UPDATE_TYPE {
	HID_FW_UPDATE_TYPE_BL = 0x77,
	HID_FW_UPDATE_TYPE_MAIN = 0x55,
};

enum HID_FW_UPDATE_STATUS_CODE {
	FWUP_ERROR_NO_ERROR = 0x77,
	FWUP_ERROR_MCU_00 = 0x00,
	FWUP_ERROR_MCU_A0 = 0xA0,
	FWUP_ERROR_NO_BL = 0xC1,
	FWUP_ERROR_NO_MAIN = 0xC2,
	FWUP_ERROR_BL_COMPLETE = 0xB1,
	FWUP_ERROR_BL = 0xB2,
	FWUP_ERROR_PW = 0xB3,
	FWUP_ERROR_ERASE_FLASH = 0xB4,
	FWUP_ERROR_FLASH_PROGRAMMING = 0xB5,
	FWUP_ERROR_NO_DEVICE = 0xFFFFFF00,
	FWUP_ERROR_LOAD_FW_BIN = 0xFFFFFF01,
	FWUP_ERROR_INITIAL = 0xFFFFFF02,
	FWUP_ERROR_POLLING_TIMEOUT = 0xFFFFFF03,
	FWUP_ERROR_FW_TRANSFER = 0xFFFFFF04
};

struct himax_ts_data;
union host_ext_rd_t;
union heatmap_rd_t;

extern const unsigned int host_ext_report_desc_sz;
extern const unsigned int host_heatmap_report_desc_sz;

extern union host_ext_rd_t g_host_ext_rd;

extern union heatmap_rd_t g_heatmap_rd;

#define HID_FW_UPDATE_BL_CMD    (0x77)
#define HID_FW_UPDATE_MAIN_CMD  (0x55)

int hx_hid_probe(struct himax_ts_data *ts);
void hx_hid_remove(struct himax_ts_data *ts);

void hx_hid_update_info(struct himax_ts_data *ts);
int hx_hid_report(const struct himax_ts_data *ts, u8 *data, s32 len);

#endif
