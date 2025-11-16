/* SPDX-License-Identifier: GPL-2.0 */
/*
 * This file is part of ILITEK CommonFlow
 *
 * Copyright (c) 2022 ILI Technology Corp.
 * Copyright (c) 2022 Luca Hsu <luca_hsu@ilitek.com>
 * Copyright (c) 2022 Joe Hung <joe_hung@ilitek.com>
 */

#ifndef __ILITEK_PROTOCOL_H__
#define __ILITEK_PROTOCOL_H__

#include "ilitek_def.h"

/* quirks definition */
#define QUIRK_WAIT_ACK_DELAY		0x1
#define QUIRK_BRIDGE			0x2
#define QUIRK_DAEMON_I2C		0x4
#define QUIRK_WIFI_ITS_I2C		0x8
#define QUIRK_LIBUSB			0x10

#define START_ADDR_LEGO			0x3000
#define START_ADDR_29XX			0x4000
#define END_ADDR_LEGO			0x40000

#define MM_ADDR_LEGO			0x3020
#define MM_ADDR_29XX			0x4020
#define MM_ADDR_2501X			0x4038

#define DF_START_ADDR_LEGO		0x3C000
#define DF_START_ADDR_29XX		0x2C000

#define ILITEK_TP_SYSTEM_READY		0x50

#define CRC_CALCULATE			0
#define CRC_GET				1

#define ILTIEK_MAX_BLOCK_NUM		20

#define PTL_ANY				0x00
#define PTL_V3				0x03
#define PTL_V6				0x06

#define BL_PROTOCOL_V1_8		0x10800
#define BL_PROTOCOL_V1_7		0x10700
#define BL_PROTOCOL_V1_6		0x10600

#define TOUT_CF_BLOCK_0			2500
#define TOUT_CF_BLOCK_N			500
#define TOUT_F1_SHORT			1600
#define TOUT_F1_OPEN			12
#define TOUT_F1_FREQ_MC			2
#define TOUT_F1_FREQ_SC			1
#define TOUT_F1_CURVE			13
#define TOUT_F1_KEY			400
#define TOUT_F1_OTHER			27
#define TOUT_F2				7
#define TOUT_CD				27
#define TOUT_C3				100
#define TOUT_65_WRITE			135
#define TOUT_65_READ			3
#define TOUT_68				24
#define TOUT_CC_SLAVE			16000

#define TOUT_F1_SHORT_RATIO		2
#define TOUT_F1_OPEN_RATIO		3
#define TOUT_F1_FREQ_RATIO		3
#define TOUT_F1_CURVE_RATIO		3
#define TOUT_F1_OTHER_RATIO		3
#define TOUT_F2_RATIO			3
#define TOUT_CD_RATIO			3
#define TOUT_C3_RATIO			3
#define TOUT_65_WRITE_RATIO		3
#define TOUT_65_READ_RATIO		3
#define TOUT_68_RATIO			3
#define TOUT_CC_SLAVE_RATIO		2

#define AP_MODE		0x5A
#define BL_MODE		0x55

#define STYLUS_MODES			\
	X(STYLUS_WGP,	0x1,	"WGP")	\
	X(STYLUS_USI,	0x2,	"USI")	\
	X(STYLUS_MPP,	0x4,	"MPP")

#define ILITEK_CMD_MAP							\
	X(0x20, PTL_ANY, GET_TP_INFO, api_protocol_get_tp_info)		\
	X(0x21, PTL_ANY, GET_SCRN_RES, api_protocol_get_scrn_res)	\
	X(0x22, PTL_ANY, GET_KEY_INFO, api_protocol_get_key_info)	\
	X(0x30, PTL_ANY, SET_IC_SLEEP, api_protocol_set_sleep)		\
	X(0x31, PTL_ANY, SET_IC_WAKE, api_protocol_set_wakeup)		\
	X(0x34, PTL_ANY, SET_MCU_IDLE, api_protocol_set_idle)		\
	X(0x40, PTL_ANY, GET_FW_VER, api_protocol_get_fw_ver)		\
	X(0x42, PTL_ANY, GET_PTL_VER, api_protocol_get_ptl_ver)		\
	X(0x43, PTL_ANY, GET_CORE_VER, api_protocol_get_core_ver)	\
	X(0x60, PTL_ANY, SET_SW_RST, api_protocol_set_sw_reset)		\
	X(0x61, PTL_ANY, GET_MCU_VER, api_protocol_get_mcu_ver)		\
	X(0x68, PTL_ANY, SET_FUNC_MOD, api_protocol_set_func_mode)	\
	X(0x80, PTL_ANY, GET_SYS_BUSY, api_protocol_get_sys_busy)	\
	X(0xC0, PTL_ANY, GET_MCU_MOD, api_protocol_get_mcu_mode)	\
	X(0xC1, PTL_ANY, SET_AP_MODE, api_protocol_set_ap_mode)		\
	X(0xC2, PTL_ANY, SET_BL_MODE, api_protocol_set_bl_mode)		\
	X(0xC5, PTL_ANY, READ_FLASH, api_protocol_read_flash)		\
	X(0xC7, PTL_ANY, GET_AP_CRC, api_protocol_get_ap_crc)		\
	X(0xC8, PTL_ANY, SET_ADDR, api_protocol_set_flash_addr)		\
									\
	/* v3 only cmds */						\
	X(0x25, PTL_V3, GET_CDC_INFO_V3, api_protocol_get_cdc_info_v3)	\
	X(0x63, PTL_V3, TUNING_PARA_V3, api_protocol_tuning_para_v3)	\
	X(0xC3, PTL_V3, WRITE_DATA_V3, api_protocol_write_data_v3)	\
	X(0xC4, PTL_V3, WRITE_ENABLE, api_protocol_write_enable)	\
	X(0xCA, PTL_V3, GET_DF_CRC, api_protocol_get_df_crc)		\
	X(0xF2, PTL_V3, SET_TEST_MOD, api_protocol_set_mode_v3)		\
	X(0xF3, PTL_V3, INIT_CDC_V3, api_protocol_set_cdc_init_v3)	\
									\
	/* v6 only cmds */						\
	X(0x24, PTL_V6, POWER_STATUS, api_protocol_power_status)	\
	X(0x27, PTL_V6, GET_SENSOR_ID, api_protocol_get_sensor_id)	\
	X(0x44, PTL_V6, GET_TUNING_VER, api_protocol_get_tuning_ver)	\
	X(0x45, PTL_V6, GET_PRODUCT_INFO, api_protocol_get_product_info)\
	X(0x46, PTL_V6, GET_FWID, api_protocol_get_fwid)		\
	X(0x47, PTL_V6, GET_CRYPTO_INFO, api_protocol_get_crypto_info)	\
	X(0x48, PTL_V6, GET_HID_INFO, api_protocol_get_hid_info)	\
	X(0x62, PTL_V6, GET_MCU_INFO, api_protocol_get_mcu_info)	\
	X(0x65, PTL_V6, TUNING_PARA_V6, api_protocol_tuning_para_v6)	\
	X(0x69, PTL_V6, SET_FS_INFO, api_protocol_set_fs_info)		\
	X(0x6A, PTL_V6, SET_SHORT_INFO, api_protocol_set_short_info)	\
	X(0x6B, PTL_V6, C_MODEL_INFO, api_protocol_c_model_info)	\
	X(0x6C, PTL_V6, SET_P2P_INFO, api_protocol_set_p2p_info)	\
	X(0x6D, PTL_V6, SET_OPEN_INFO, api_protocol_set_open_info)	\
	X(0x6E, PTL_V6, SET_CHARGE_INFO, api_protocol_set_charge_info)	\
	X(0x6F, PTL_V6, SET_PEN_FS_INFO, api_protocol_set_pen_fs_info)	\
	X(0xB0, PTL_V6, WRITE_DATA_M2V, api_protocol_write_data_m2v)	\
	X(0xC3, PTL_V6, WRITE_DATA_V6, api_protocol_write_data_v6)	\
	X(0xC9, PTL_V6, SET_DATA_LEN, api_protocol_set_data_len)	\
	X(0xCB, PTL_V6, ACCESS_SLAVE, api_protocol_access_slave)	\
	X(0xCC, PTL_V6, SET_FLASH_EN, api_protocol_set_flash_enable)	\
	X(0xCD, PTL_V6, GET_BLK_CRC_ADDR, api_protocol_get_crc_by_addr)	\
	X(0xCF, PTL_V6, GET_BLK_CRC_NUM, api_protocol_get_crc_by_num)	\
	X(0xF0, PTL_V6, SET_MOD_CTRL, api_protocol_set_mode_v6)		\
	X(0xF1, PTL_V6, INIT_CDC_V6, api_protocol_set_cdc_init_v6)	\
	X(0xF2, PTL_V6, GET_CDC_V6, api_protocol_get_cdc_v6)


#define X(_cmd, _protocol, _cmd_id, _api)	_cmd_id,
enum ilitek_cmd_ids {
	ILITEK_CMD_MAP
	/* ALWAYS keep at the end */
	MAX_CMD_CNT
};
#undef X

#define X(_cmd, _protocol, _cmd_id, _api)	CMD_##_cmd_id = _cmd,
enum ilitek_cmds { ILITEK_CMD_MAP };
#undef X

enum ilitek_hw_interfaces {
	interface_i2c = 0,
	interface_hid_over_i2c,
	interface_usb,
};

enum ilitek_fw_modes {
	mode_unknown = -1,
	mode_normal = 0,
	mode_test,
	mode_debug,
	mode_suspend,
};

enum ilitek_key_modes {
	key_disable = 0,
	key_hw = 1,
	key_hsw = 2,
	key_vitual = 3,
	key_fw_disable = 0xff,
};

#define ILITEK_TOUCH_REPORT_FORMAT			\
	X(touch_fmt_0x0,	0x0, 5, 10)	\
	X(touch_fmt_0x1,	0x1, 6, 10)	\
	X(touch_fmt_0x2,	0x2, 10, 5)	\
	X(touch_fmt_0x3,	0x3, 10, 5)	\
	X(touch_fmt_0x4,	0x4, 10, 5)	\
	X(touch_fmt_0x10,	0x10, 10, 6)	\
	X(touch_fmt_0x11,	0x11, 5, 10)

#define X(_enum, _id, _size, _cnt)	_enum = _id,
enum ilitek_touch_fmts {
	ILITEK_TOUCH_REPORT_FORMAT
	touch_fmt_max = 0x100,
};
#undef X

#define ILITEK_PEN_REPORT_FORMAT		\
	X(pen_fmt_0x0, 0x0, 12, 1)	\
	X(pen_fmt_0x1, 0x1, 18, 1)	\
	X(pen_fmt_0x2, 0x2, 22, 1)

#define X(_enum, _id, _size, _cnt)	_enum = _id,
enum ilitek_pen_fmts {
	ILITEK_PEN_REPORT_FORMAT
	pen_fmt_max = 0x100,
};
#undef X

struct ilitek_slave_access {
	u8 slave_id;
	u8 func;
	void *data;
};

struct tuning_para_settings {
	u8 func;
	u8 ctrl;
	u8 type;

	u8 *buf;
	u32 len;
};

struct reports {
	bool touch_need_update;
	bool pen_need_update;

	u8 touch[64];
	u8 pen[64];
};

struct grid_data {
	bool need_update;
	unsigned int X, Y;

	s32 *data;
};

struct grids {
	struct grid_data mc;
	struct grid_data sc_x;
	struct grid_data sc_y;
	struct grid_data pen_x;
	struct grid_data pen_y;

	struct grid_data key_mc;
	struct grid_data key_x;
	struct grid_data key_y;

	struct grid_data self;

	/* touch/pen debug message along with frame update */
	struct reports dmsg;
};

enum ilitek_enum_type {
	enum_ap_bl = 0,
	enum_sw_reset,
};

typedef void (*update_grid_t)(u32, u32, struct grids *, void *);
typedef void (*update_report_rate_t)(unsigned int);

typedef int (*write_then_read_t)(u8 *, int, u8 *, int, void *);
typedef int (*read_ctrl_in_t)(u8 *, int, unsigned int, void *);
typedef int (*read_interrupt_in_t)(u8 *, int, unsigned int, void *);
typedef void (*init_ack_t)(unsigned int, void *);
typedef int (*wait_ack_t)(u8, unsigned int, void *);
typedef int (*hw_reset_t)(unsigned int, void *);
typedef int (*re_enum_t)(u8, void *);
typedef void (*delay_ms_t)(unsigned int);


typedef int (*write_then_read_direct_t)(u8 *, int, u8 *, int, void *);
typedef void (*mode_switch_notify_t)(bool, bool, void *);

#ifdef _WIN32
/* packed below structures by 1 byte */
#pragma pack(1)
#endif

struct __PACKED__ touch_fmt {
	u8 id : 6;
	u8 status : 1;
	u8 reserve : 1;
	u16 x;
	u16 y;
	u8 pressure;
	u16 width;
	u16 height;

	u8 algo;
};

struct __PACKED__ touch_iwb_fmt {
	u8 status : 3;
	u8 reserve : 5;
	u8 id : 6;
	u8 reserve_1 : 2;
	u16 x;
	u16 y;
	u16 width;
	u16 height;

	u8 algo;
};

struct __PACKED__ pen_fmt {
	union __PACKED__ {
		u8 modes;
		struct __PACKED__ {
			u8 tip_sw : 1;
			u8 barrel_sw : 1;
			u8 eraser : 1;
			u8 invert : 1;
			u8 in_range : 1;
			u8 reserve : 3;
		};
	};
	u16 x;
	u16 y;
	u16 pressure;
	s16 x_tilt;
	s16 y_tilt;

	u8 battery;

	union __PACKED__ {
		/* usi v1.0 */
		struct __PACKED__ {
			u16 barrel_pressure;
			u8 idx;
			u8 color;
			u8 width;
			u8 style;
		} usi_1;

		/* usi v2.0 */
		struct __PACKED__ {
			u16 barrel_pressure;
			u8 idx;
			u8 color;
			u8 color_24[3];
			u8 no_color;
			u8 width;
			u8 style;
		} usi_2;
	};
};

struct __PACKED__ ilitek_report_fmt_info {
	u32 touch_size;
	u32 touch_max_cnt;

	u32 pen_size;
	u32 pen_max_cnt;
};

struct __PACKED__ ilitek_screen_info {
	u16 x_min;
	u16 y_min;
	u16 x_max;
	u16 y_max;
	u16 pressure_min;
	u16 pressure_max;
	s16 x_tilt_min;
	s16 x_tilt_max;
	s16 y_tilt_min;
	s16 y_tilt_max;
	u16 pen_x_min;
	u16 pen_y_min;
	u16 pen_x_max;
	u16 pen_y_max;
};

struct __PACKED__ ilitek_tp_info_v6 {
	u16 x_resolution;
	u16 y_resolution;
	u16 x_ch;
	u16 y_ch;
	u8 max_fingers;
	u8 key_num;
	u8 ic_num;
	u8 support_modes;
	u8 format;
	u8 die_num;
	u8 block_num;
	u8 pen_modes;
	u8 pen_format;
	u16 pen_x_resolution;
	u16 pen_y_resolution;
};

struct __PACKED__ ilitek_tp_info_v3 {
	u16 x_resolution;
	u16 y_resolution;
	u8 x_ch;
	u8 y_ch;
	u8 max_fingers;
	u8 reserve;
	u8 key_num;
	u8 reserve_1;
	u8 touch_start_y;
	u8 touch_end_y;
	u8 touch_start_x;
	u8 touch_end_x;
	u8 support_modes;
};

struct __PACKED__ ilitek_key_info_v6 {
	u8 mode;
	u16 x_len;
	u16 y_len;

	struct __PACKED__ _ilitek_key_info_v6 {
		u8 id;
		u16 x;
		u16 y;
	} keys[50];
};

struct __PACKED__ ilitek_key_info_v3 {
	u8 x_len[2];
	u8 y_len[2];

	struct __PACKED__ _ilitek_key_info_v3 {
		u8 id;
		u8 x[2];
		u8 y[2];
	} keys[20];
};

struct __PACKED__ ilitek_ts_kernel_info {
	char ic_name[6];
	char mask_ver[2];
	u32 mm_addr;
	u32 min_addr;
	u32 max_addr;
	char module_name[32];

	char ic_full_name[16];
};

struct __PACKED__ ilitek_key_info {
	struct ilitek_key_info_v6 info;
	bool clicked[50];
};

struct __PACKED__ ilitek_power_status {
	u16 header;
	u8 vdd33_lvd_flag;
	u8 vdd33_lvd_level_sel;
};

struct __PACKED__ ilitek_sensor_id {
	u16 header;
	u8 id;
};

struct __PACKED__ ilitek_func_mode {
	u16 header;
	u8 mode;
};

struct __PACKED__ ilitek_ts_protocol {
	u32 ver;
	u8 flag;
};

struct __PACKED__ ilitek_ts_ic {
	u8 mode;
	u32 crc[ILTIEK_MAX_BLOCK_NUM];

	char mode_str[32];
};

struct __PACKED__ ilitek_hid_info {
	u16 pid;
	u16 vid;
	u16 rev;
};

struct __PACKED__ freq_category {
	u32 start;
	u32 end;
	u32 step;
	u32 steps;

		u32 size;
	char limit[1024];

	u8 mode;

	s32 *data;
};

struct __PACKED__ freq_settings {
	bool prepared;

	unsigned int frame_cnt;

	/* add from v6.0.A */
	unsigned int mc_frame_cnt;
	unsigned int dump_frame_cnt;

	unsigned int scan_type;

	struct freq_category sine;
	struct freq_category mc_swcap;
	struct freq_category sc_swcap;
	struct freq_category pen;

	struct freq_category dump1;
	struct freq_category dump2;
	u8 dump1_val;
	u8 dump2_val;

	u16 packet_steps;
};

struct __PACKED__ short_settings {
	bool prepared;

	u8 dump_1;
	u8 dump_2;
	u8 v_ref_L;
	u16 post_idle;
};

struct __PACKED__ open_settings {
	bool prepared;

	u16 freq;
	u8 gain;
	u8 gain_rfb;
	u8 afe_res_sel;
	u8 mc_fsel;
	u16 frame;
};

struct __PACKED__ p2p_settings {
	bool prepared;

	u16 frame_cnt;
	u8 type;

	/* add from v6.0.A */
	u16 freq;
};

struct __PACKED__ charge_curve_sweep {
	u16 start;
	u16 end;
	u8 step;
	u16 post_idle;
	u16 fix_val;

	u16 steps;
};

struct __PACKED__ charge_curve_settings {
	bool prepared;

	u8 scan_mode;

	struct charge_curve_sweep dump;
	struct charge_curve_sweep charge;

	u16 c_sub;
	u16 frame_cnt;

	struct __PACKED__ charge_curve_point {
		u16 x;
		u16 y;
		u16 *dump_max;
		u16 *dump_avg;
		u16 *charge_max;
		u16 *charge_avg;
	} pt[9];

	u16 packet_steps;
};

struct __PACKED__ cdc_settings {
	u8 cmd;
	u16 config;

	bool skip_checksum;

	/* freq. */
	struct freq_settings freq;
	/* short */
	struct short_settings _short;
	/* open */
	struct open_settings open;
	/* p2p */
	struct p2p_settings p2p;
	/* charge curve */
	struct charge_curve_settings curve;

	/* status only writable by CDC commonflow */
	bool is_key;
	bool is_p2p;
	bool is_freq;
	bool is_curve;
	bool is_short;
	bool is_open;
	bool is_16bit;
	bool is_sign;
	bool is_fast_mode;
	unsigned int total_bytes;

	/* error code during cdc data collection */
	s32 error;
};

struct __PACKED__ mp_station_old {
	struct __PACKED__ {
		u8 week;
		u8 year;
		u8 fw_ver[8];
		char module[19];

		u8 short_test:2;
		u8 open_test:2;
		u8 self_test:2;
		u8 uniform_test:2;

		u8 dac_test:2;
		u8 key_test:2;
		u8 final_result:2;
		u8 paint_test:2;

		u8 mopen_test:2;
		u8 gpio_test:2;
		u8 reserve_1:4;

		char bar_code[28];
		u8 reserve_2[35];

		u16 custom_id;
		u16 fwid;
		u8 idx;
	} station[10];
};

struct __PACKED__ mp_station {
	struct __PACKED__ {
		u8 week;
		u8 year;
		u8 fw_ver[8];
		char module[19];

		u8 short_test : 2;
		u8 open_test : 2;
		u8 self_test : 2;
		u8 uniform_test : 2;

		u8 dac_test : 2;
		u8 key_test : 2;
		u8 final_result : 2;
		u8 paint_test : 2;

		u8 mopen_test : 2;
		u8 gpio_test : 2;
		u8 reserve : 4;

		u8 tool_ver[8];
		char bar_code[135];

		u16 custom_id;
		u16 fwid;
		u8 idx;
	} station[5];

	struct __PACKED__ {
		u8 reserve_1[91];
		u32 mp_result_ver;
		u16 customer_id;
		u16 fwid;
		u8 reserve_2;
	} info;

	u16 crc;
};

struct __PACKED__ ilitek_ts_settings {
	bool no_retry;
	bool no_INT_ack;

	bool sw_reset_at_last;

	u8 sensor_id_mask;

	/* only used for QUIRK_WAIT_ACK_DELAY */
	u32 wait_ack_delay;

	/*
	 * engineer mode would likely report default format
	 * ex. IWB-format
	 */
	bool default_format_enabled;
};

struct __PACKED__ ilitek_sys_info {
	u16 pid;
};

struct __PACKED__ ilitek_ts_callback {
	/* Please don't use "repeated start" for I2C interface */
	write_then_read_t write_then_read;
	read_ctrl_in_t read_ctrl_in;
	read_interrupt_in_t read_interrupt_in;
	init_ack_t init_ack;
	wait_ack_t wait_ack;
	hw_reset_t hw_reset;
	re_enum_t re_enum;
	delay_ms_t delay_ms;
	msg_t msg;

	/* write cmd without adding any hid header */
	write_then_read_direct_t write_then_read_direct;
	/* notify caller after AP/BL mode switch command */
	mode_switch_notify_t mode_switch_notify;
};

struct __PACKED__ ilitek_common_info {
	u32 quirks;
	u8 _interface;

	u16 customer_id;
	u16 fwid;

	char pen_mode[64];
	u8 fw_ver[8];
	u8 core_ver[8];
	u8 tuning_ver[4];
	u8 product_info[8];

	struct ilitek_sys_info sys;
	struct ilitek_ts_protocol protocol;
	struct ilitek_func_mode func;
	struct ilitek_sensor_id sensor;
	struct ilitek_ts_ic ic[32];
	struct ilitek_screen_info screen;
	struct ilitek_tp_info_v6 tp;
	struct ilitek_key_info key;
	struct ilitek_ts_kernel_info mcu;
	struct ilitek_hid_info hid;
	struct ilitek_report_fmt_info fmt;
	struct ilitek_power_status pwr;
};

struct __PACKED__ ilitek_ts_device {
	void *_private;
	char id[64];
	u32 reset_time;

	struct ilitek_ts_settings setting;

	u32 quirks;
	u8 _interface;

	u16 customer_id;
	u16 fwid;

	char pen_mode[64];
	u8 fw_ver[8];
	u8 core_ver[8];
	u8 tuning_ver[4];
	u8 product_info[8];

	struct ilitek_sys_info sys;
	struct ilitek_ts_protocol protocol;
	struct ilitek_func_mode func;
	struct ilitek_sensor_id sensor;
	struct ilitek_ts_ic ic[32];
	struct ilitek_screen_info screen_info;
	struct ilitek_tp_info_v6 tp_info;
	struct ilitek_key_info key;
	struct ilitek_ts_kernel_info mcu_info;
	struct ilitek_hid_info hid_info;
	struct ilitek_report_fmt_info fmt;
	struct ilitek_power_status pwr;

	u8 fw_mode;
	struct mp_station mp;

	u8 wbuf[4096];
	u8 rbuf[4096];
	struct ilitek_ts_callback cb;
};

#ifdef _WIN32
#pragma pack()
#endif

#ifdef __cplusplus
extern "C" {
#endif

u16 __DLL le16(const u8 *p);
u16 __DLL be16(const u8 *p);
u32 __DLL le32(const u8 *p, int bytes);
u32 __DLL be32(const u8 *p, int bytes);

bool __DLL is_29xx(void *handle);

bool __DLL _is_231x(char *ic_name);
bool __DLL is_231x(void *handle);

bool __DLL has_hw_key(void *handle);

u8 __DLL get_protocol_ver_flag(u32 ver);

int __DLL grid_alloc(void *handle, struct grids *grid);
void __DLL grid_free(struct grids *grid);
void __DLL grid_reset(struct grids *grid);

u16 __DLL get_crc(u32 start, u32 end,
		  u8 *buf, u32 buf_size);

u32 __DLL get_checksum(u32 start, u32 end,
		       u8 *buf, u32 buf_size);

bool __DLL is_checksum_matched(u8 checksum, int start, int end,
			       u8 *buf, int buf_size);

bool __DLL support_sensor_id(void *handle);
bool __DLL support_production_info(void *handle);
bool __DLL support_fwid(void *handle);
bool __DLL support_mcu_info(void *handle);
bool __DLL support_power_status(void *handle);
int __DLL write_then_wait_ack(void *handle, u8 *cmd, int wlen, int timeout_ms);

int __DLL bridge_set_int_monitor(void *handle, bool enable);
int __DLL bridge_set_test_mode(void *handle, bool enable);

int __DLL reset_helper(void *handle);

int __DLL write_then_read(void *handle, u8 *cmd, int wlen,
			  u8 *buf, int rlen);
int __DLL write_then_read_direct(void *handle, u8 *cmd, int wlen,
				 u8 *buf, int rlen);
int __DLL read_interrupt_in(void *handle, u8 *buf, int rlen,
			    unsigned int timeout_ms);
int __DLL read_ctrl_in(void *handle, u8 cmd, u8 *buf, int rlen,
		       unsigned int timeout_ms);

void __DLL __ilitek_get_info(void *handle,
			     struct ilitek_common_info *info);

void __DLL ilitek_dev_set_quirks(void *handle, u32 quirks);
void __DLL ilitek_dev_set_sys_info(void *handle, struct ilitek_sys_info *sys);
void __DLL ilitek_dev_setting(void *handle,
			      struct ilitek_ts_settings *setting);

void __DLL ilitek_dev_bind_callback(void *handle,
				    struct ilitek_ts_callback *callback);

void __DLL *ilitek_dev_init(u8 _interface, const char *id,
			    bool need_update_ts_info,
			    struct ilitek_ts_callback *callback,
			    void *_private);
void __DLL ilitek_dev_exit(void *handle);

void __DLL api_print_ts_info(void *handle);
void __DLL api_read_then_print_m2v_info(void *handle);

int __DLL api_update_ts_info(void *handle);

int __DLL api_protocol_set_cmd(void *handle, u8 idx, void *data);
int __DLL api_set_ctrl_mode(void *handle, u8 mode, bool eng, bool force);

u16 __DLL api_get_block_crc_by_addr(void *handle, u8 type,
				    u32 start, u32 end);
u16 __DLL api_get_block_crc_by_num(void *handle, u8 type,
				   u8 block_num);

int __DLL api_set_data_len(void *handle, u16 data_len);
int __DLL api_write_enable_v6(void *handle, bool in_ap, bool is_slave,
			      u32 start, u32 end);
int __DLL api_write_data_v6(void *handle, int wlen);
int __DLL api_access_slave(void *handle, u8 id, u8 func, void *data);
int __DLL api_check_busy(void *handle, int timeout_ms, int delay_ms);
int __DLL api_write_enable_v3(void *handle, bool in_ap,
			      bool write_ap, u32 end, u32 checksum);
int __DLL api_write_data_v3(void *handle);

int __DLL api_to_bl_mode(void *handle, bool bl, u32 start, u32 end);

int __DLL api_write_data_m2v(void *handle, int wlen);
int __DLL api_to_bl_mode_m2v(void *handle, bool to_bl);

int __DLL api_set_idle(void *handle, bool enable);
int __DLL api_set_func_mode(void *handle, u8 mode);
int __DLL api_get_func_mode(void *handle);

int __DLL api_erase_data_v3(void *handle);

int __DLL api_read_flash(void *handle, u8 *buf,
			 u32 start_addr, u32 len);

int __DLL _api_read_mp_result(void *handle);
int __DLL api_read_mp_result(void *handle);
int __DLL _api_write_mp_result(void *handle, struct mp_station *mp);
int __DLL api_write_mp_result(void *handle, struct mp_station *mp);
void __DLL api_decode_mp_result(void *handle);

int __DLL api_read_tuning(void *handle, u8 *buf, int rlen);

int __DLL api_get_ic_crc(void *handle, u8 final_fw_mode);

#ifdef __cplusplus
}
#endif

#endif
