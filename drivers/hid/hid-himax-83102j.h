/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __HX_IC_83102J_H__
#define __HX_IC_83102J_H__

#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/acpi.h>
#include <linux/spi/spi.h>
#include <linux/hid.h>
#include <linux/sizes.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/types.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/proc_fs.h>
#include <linux/version.h>
#include <linux/firmware.h>
#include <linux/stddef.h>
#include <linux/power_supply.h>

#define HIMAX_DRIVER_VER "1.0.0"

#define HIMAX_BUS_RETRY_TIMES 3
#define BUS_RW_MAX_LEN 0x20006
#define BUS_R_HLEN 3
#define BUS_R_DLEN ((BUS_RW_MAX_LEN - BUS_R_HLEN) - ((BUS_RW_MAX_LEN - BUS_R_HLEN) % 4))
#define BUS_W_HLEN 2
#define BUS_W_DLEN ((BUS_RW_MAX_LEN - BUS_W_HLEN) - ((BUS_RW_MAX_LEN - BUS_W_HLEN) % 4))
#define FIX_HX_INT_IS_EDGE	(false)

#define HX_DELAY_BOOT_UPDATE			(2000)
#define HID_REG_SZ_MAX					(1 + 4 + 1 + 4 + 256)

enum HID_ID_FUNCT {
	ID_CONTACT_COUNT = 0x03,
};

#define HID_RAW_DATA_TYPE_DELTA     (0x09)
#define HID_RAW_DATA_TYPE_RAW       (0x0A)
#define HID_RAW_DATA_TYPE_BASELINE  (0x0B)
#define HID_RAW_DATA_TYPE_NORMAL	(0x00)

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

#define HID_FW_UPDATE_BL_CMD    (0x77)
#define HID_FW_UPDATE_MAIN_CMD  (0x55)

int hx_hid_probe(struct himax_ts_data *ts);
void hx_hid_remove(struct himax_ts_data *ts);

void hx_hid_update_info(struct himax_ts_data *ts);
int hx_hid_report(const struct himax_ts_data *ts, u8 *data, s32 len);

enum fix_touch_info {
	FIX_HX_RX_NUM = 48,
	FIX_HX_TX_NUM = 32,
	FIX_HX_BT_NUM = 0,
	FIX_HX_MAX_PT = 10,
	FIX_HX_STYLUS_FUNC = 1,
	FIX_HX_STYLUS_ID_V2 = 0,
	FIX_HX_STYLUS_RATIO = 1,
	HX_STACK_ORG_LEN = 128
};

#define himax_dev_name "hx_spi_hid_tp"

#if defined(CONFIG_FB)
#include <linux/notifier.h>
#include <linux/fb.h>
#endif

/* log macro */
#define I(fmt, arg...) pr_info("[HXTP][%s]: " fmt "\n", __func__, ##arg)
#define W(fmt, arg...) pr_warn("[HXTP][WARNING][%s]: " fmt "\n", __func__, ##arg)
#define E(fmt, arg...) pr_err("[HXTP][ERROR][%s]: " fmt "\n", __func__, ##arg)
#define D(fmt, arg...) pr_debug("[HXTP][DEBUG][%s]: " fmt "\n", __func__, ##arg)

#define himax_dev_name "hx_spi_hid_tp"

#define BOOT_UPGRADE_FWNAME "himax_i2chid"

#define MPAP_FWNAME "himax_mpfw.bin"

#define UNUSED(x) ((void)(x))
static const char default_fw_name[] = BOOT_UPGRADE_FWNAME;

#define DATA_LEN_4				4
#define ADDR_LEN_4				4
#define MAX_I2C_TRANS_SZ		128
#define HIMAX_REG_RETRY_TIMES	5
#define FW_PAGE_SZ			128
#define HX1K					0x400
#define HX_48K_SZ				0xC000
#define HX64K					0x10000

#define HX_RW_REG_FAIL			(-1)

#define hx83102j_fw_addr_raw_out_sel 0x100072EC
#define hx83102j_ic_adr_tcon_rst     0x80020004
#define hx83102j_data_adc_num        400 /* 200x2 */
#define hx83102j_notouch_frame       0
#define HX83102J_FLASH_SIZE        261120
#define HX83102j_ic_addr_hw_crc 0x80010000
#define HX83102j_data_hw_crc 0x0000ECCE

/* CORE_IC */
	#define ic_adr_ahb_addr_byte_0           0x00
	#define ic_adr_ahb_rdata_byte_0          0x08
	#define ic_adr_ahb_access_direction      0x0c
	#define ic_adr_conti                     0x13
	#define ic_adr_incr4                     0x0D
	#define ic_adr_i2c_psw_lb                0x31
	#define ic_adr_i2c_psw_ub                0x32
	#define ic_cmd_ahb_access_direction_read 0x00
	#define ic_cmd_conti                     0x31
	#define ic_cmd_incr4                     0x10
	#define ic_cmd_i2c_psw_lb                0x27
	#define ic_cmd_i2c_psw_ub                0x95
	#define ic_adr_tcon_on_rst               0x80020020
	#define ic_adr_cs_central_state          0x900000A8
/* CORE_IC */
/* CORE_FW */
	#define fw_addr_system_reset                0x90000018
	#define fw_addr_ctrl_fw                     0x9000005c
	#define fw_addr_flag_reset_event            0x900000e4
	#define fw_usb_detect_addr                  0x10007F38
	#define fw_addr_raw_out_sel                 0x800204b4
	#define fw_addr_reload_status               0x80050000
	#define fw_addr_reload_crc32_result         0x80050018
	#define fw_addr_reload_addr_from            0x80050020
	#define fw_addr_reload_addr_cmd_beat        0x80050028
	#define fw_data_system_reset                0x00000055
    #define fw_data_safe_mode_release_pw_reset  0x00000000
	#define fw_data_clear                       0x00000000
	#define fw_data_fw_stop                     0x000000A5
	#define fw_addr_set_frame_addr              0x10007294
	#define fw_addr_sorting_mode_en             0x10007f04
	#define fw_addr_fw_ver_addr                 0x10007004
	#define fw_addr_fw_cfg_addr                 0x10007084
	#define fw_addr_fw_vendor_addr              0x10007000
	#define fw_addr_cus_info                    0x10007008
	#define fw_addr_proj_info                   0x10007014
	#define fw_addr_fw_dbg_msg_addr             0x10007f40
	#define fw_addr_chk_fw_status               0x900000a8
	#define fw_addr_chk_dd_status               0x900000E8
	#define fw_addr_ap_notify_fw_sus            0x10007FD0
	#define fw_data_ap_notify_fw_sus_en         0xA55AA55A
	#define fw_data_ap_notify_fw_sus_dis        0x00000000
	#define fw_addr_event_addr                  0x30
	#define fw_func_handshaking_pwd             0xA55AA55A
	#define fw_func_handshaking_end             0x77887788
	#define fw_addr_ulpm_33                     0x33
	#define fw_addr_ulpm_34                     0x34
	#define fw_data_ulpm_11                     0x11
	#define fw_data_ulpm_22                     0x22
	#define fw_data_ulpm_33                     0x33
	#define fw_data_ulpm_aa                     0xAA
	#define fw_addr_ctrl_mpap_ovl               0x100073EC
	#define fw_data_ctrl_mpap_ovl_on            0x107380
/* CORE_FW */
/* CORE_FLASH */
	#define flash_addr_ctrl_base           0x80000000
	#define flash_addr_spi200_data         (flash_addr_ctrl_base + 0x2c)
/* CORE_FLASH */
/* CORE_SRAM */
	#define sram_adr_mkey         0x100070E8
	#define sram_adr_rawdata_addr 0x10000000
	#define	sram_passwrd_start    0x5AA5
	#define	sram_passwrd_end      0xA55A
/* CORE_SRAM */
/* CORE_DRIVER */
	#define driver_addr_fw_define_flash_reload              0x10007f00
	#define driver_addr_fw_define_2nd_flash_reload          0x100072c0
	#define driver_addr_fw_define_int_is_edge               0x10007088
	#define driver_addr_fw_define_rxnum_txnum               0x100070f4
	#define driver_addr_fw_define_maxpt_xyrvs               0x100070f8
	#define driver_addr_fw_define_x_y_res                   0x100070fc
/* CORE_DRIVER */
	#define zf_data_dis_flash_reload 0x00009AA9
	#define zf_addr_system_reset     0x90000018
	#define zf_data_system_reset     0x00000055
	#define zf_data_sram_start_addr  0x08000000
	#define zf_data_cfg_info         0x10007000
	#define zf_data_fw_cfg_1         0x10007084
	#define zf_data_fw_cfg_2         0x10007264
	#define zf_data_fw_cfg_3         0x10007300
	#define zf_data_adc_cfg_1        0x10006A00
	#define zf_data_adc_cfg_2        0x10007B28
	#define zf_data_adc_cfg_3        0x10007AF0
	#define zf_data_map_table        0x10007500
	#define ovl_section_num      3
	#define ovl_border_reply     0x66
	#define ovl_sorting_reply    0xAA
	#define ovl_fault            0xFF
	#define ovl_alg_request  0x11111111
	#define ovl_alg_reply    0x22222222

	#define time_var timespec64
	#define time_var_fine tv_nsec
	#define time_var_fine_unit (1000 * 1000)
	#define time_func ktime_get_real_ts64
	#define owner_line

	#define HX_TP_BIN_CHECKSUM_CRC		3

	#define FW_SIZE_255k    261120

	#define HX83102J_ID		"HX83102J"

	/* origin is 20/50 */
	#define RST_LOW_PERIOD_S 5000
	#define RST_LOW_PERIOD_E 5100
	#define RST_HIGH_PERIOD_ZF_S 5000
	#define RST_HIGH_PERIOD_ZF_E 5100
	#define RST_HIGH_PERIOD_S 50000
	#define RST_HIGH_PERIOD_E 50100
enum data_type {
	HX_REG = 0xA5,
	HX_DATA
};

struct hx_reg_t {
	union {
		u32 word;
		u16 half[2];
		u8 byte[4];
	} data;
	u32 len;
	u32 data_type;
};

struct data_pack_t {
	union {
		u32 *word;
		u16 *half;
		u8 *byte;
		void *ptr;
	} data;
	/* length in byte unit */
	u32 len;
	u32 data_type;
};

#define BYTE_REG(_reg, _data) \
	{ \
		_reg.data.byte[0] = (_data) & 0xFF; \
		_reg.len = 1; \
		_reg.data_type = HX_REG; \
	}
#define HALF_REG(_reg, _data) \
	{ \
		_reg.data.half[0] = cpu_to_le16((_data) & 0xFFFF); \
		_reg.len = 2; \
		_reg.data_type = HX_REG; \
	}
#define WORD_REG(_reg, _data) \
	{ \
		_reg.data.word = cpu_to_le32(_data); \
		_reg.len = 4; \
		_reg.data_type = HX_REG; \
	}

// set val to already defined reg/data
#define VAL_SET(_var, _val) \
	({ \
		bool _ret = true; \
		do { \
			if (_var.data_type == HX_DATA) { \
				memset(_var.data.byte, 0, _var.len); \
				do { \
					switch (_var.len) { \
					case 1: \
						_var.data.byte[0] = (_val) & 0xFF; \
						break; \
					case 2: \
						_var.data.half[0] = cpu_to_le16((_val) & 0xFFFF); \
						break; \
					case 3: \
						_var.data.half[0] = cpu_to_le16((_val) & 0xFFFF); \
						_var.data.byte[2] = ((_val) >> 16) & 0xFF; \
						break; \
					case 4: \
						_var.data.word[0] =\
						cpu_to_le32((_val) & 0xFFFFFFFF); \
						break; \
					default: \
						_ret = false; \
						break; \
					};\
				} while (0); \
			} else { \
				_ret = false; \
			} \
		} while (0); \
		_ret; \
	})

// set ptr/array to already defined reg/data
#define PTR_SET(_var, _ptr, _len) \
	({ \
		bool _ret = true; \
		do { \
			if ((_len) > (_var).len) { \
				_ret = false; \
				break; \
			} \
			memcpy((_var).data.byte, _ptr, (_len)); \
			(_var).len = (_len); \
		} while (0); \
		_ret; \
	})

#define DEF_WORD_DATA(_data_name) \
	u8 _data_name##_array[4] = {0}; \
	struct data_pack_t _data_name = { \
		.data.byte = _data_name##_array, \
		.len = 4, \
		.data_type = HX_DATA \
	}

#define ARRAY_DATA(_data, _byte_len) { \
		_data.data.byte = (uint8_t *)_data, \
		_data.len = _byte_len, \
		.data_type = HX_DATA \
	}

#define REG_GET_VAL(_reg) \
	({ \
		u32 _val = 0; \
		do { \
			switch (_reg.len) { \
			case 1: \
				_val = _reg.data.byte[0]; \
				break; \
			case 2: \
				_val = le16_to_cpu(_reg.data.half[0]); \
				break; \
			case 3: \
				_val = le16_to_cpu(_reg.data.half[0]) | (_reg.data.byte[2] << 16); \
				break; \
			case 4: \
				_val = le32_to_cpu(_reg.data.word); \
				break; \
			} \
		} while (0); \
		_val; \
	})

struct ic_operation {
	struct hx_reg_t addr_ahb_addr_byte_0;
	struct hx_reg_t addr_ahb_rdata_byte_0;
	struct hx_reg_t addr_ahb_access_direction;
	struct hx_reg_t addr_conti;
	struct hx_reg_t addr_incr4;
	struct hx_reg_t adr_i2c_psw_lb;
	struct hx_reg_t adr_i2c_psw_ub;
	struct hx_reg_t data_ahb_access_direction_read;
	struct hx_reg_t data_conti;
	struct hx_reg_t data_incr4;
	struct hx_reg_t data_i2c_psw_lb;
	struct hx_reg_t data_i2c_psw_ub;
	struct hx_reg_t addr_tcon_on_rst;
	struct hx_reg_t addr_cs_central_state;
};

struct fw_operation {
	struct hx_reg_t addr_system_reset;
	struct hx_reg_t addr_ctrl_fw_isr;
	struct hx_reg_t addr_flag_reset_event;
	struct hx_reg_t addr_raw_out_sel;
	struct hx_reg_t addr_reload_status;
	struct hx_reg_t addr_reload_crc32_result;
	struct hx_reg_t addr_reload_addr_from;
	struct hx_reg_t addr_reload_addr_cmd_beat;
	struct hx_reg_t addr_set_frame_addr;
	struct hx_reg_t addr_sorting_mode_en;
	struct hx_reg_t addr_fw_ver_addr;
	struct hx_reg_t addr_fw_cfg_addr;
	struct hx_reg_t addr_fw_vendor_addr;
	struct hx_reg_t addr_cus_info;
	struct hx_reg_t addr_proj_info;
	struct hx_reg_t addr_ap_notify_fw_sus;
	struct hx_reg_t data_ap_notify_fw_sus_en;
	struct hx_reg_t data_ap_notify_fw_sus_dis;
	struct hx_reg_t data_system_reset;
	struct hx_reg_t data_clear;
	struct hx_reg_t data_fw_stop;
	struct hx_reg_t addr_event_addr;
	struct hx_reg_t addr_usb_detect;
};

struct sram_operation {
	struct hx_reg_t addr_mkey;
	struct hx_reg_t addr_rawdata_addr;
	struct hx_reg_t passwrd_start;
	struct hx_reg_t passwrd_end;
};

struct driver_operation {
	struct hx_reg_t addr_fw_define_flash_reload;
	struct hx_reg_t addr_fw_define_2nd_flash_reload;
	struct hx_reg_t addr_fw_define_int_is_edge;
	struct hx_reg_t addr_fw_define_rxnum_txnum;
	struct hx_reg_t addr_fw_define_maxpt_xyrvs;
	struct hx_reg_t addr_fw_define_x_y_res;

};

struct zf_operation {
	struct hx_reg_t data_dis_flash_reload;
	struct hx_reg_t addr_system_reset;
	struct hx_reg_t data_system_reset;
	struct hx_reg_t data_sram_start_addr;
	struct hx_reg_t data_sram_clean;
	struct hx_reg_t data_cfg_info;
	struct hx_reg_t data_fw_cfg_1;
	struct hx_reg_t data_fw_cfg_2;
	struct hx_reg_t data_fw_cfg_3;
	struct hx_reg_t data_adc_cfg_1;
	struct hx_reg_t data_adc_cfg_2;
	struct hx_reg_t data_adc_cfg_3;
	struct hx_reg_t data_map_table;
};

struct flash_version_info {
	struct hx_reg_t addr_fw_ver_major;
	struct hx_reg_t addr_fw_ver_minor;
	struct hx_reg_t addr_cfg_ver_major;
	struct hx_reg_t addr_cfg_ver_minor;
	struct hx_reg_t addr_cid_ver_major;
	struct hx_reg_t addr_cid_ver_minor;
	struct hx_reg_t addr_cfg_table;
	struct hx_reg_t addr_cfg_table_t;
	struct hx_reg_t addr_hid_table;
	struct hx_reg_t addr_hid_desc;
	struct hx_reg_t addr_hid_rd_desc;
};

struct himax_core_command_regs {
	struct flash_version_info flash_ver_info;
	struct ic_operation ic_op;
	struct fw_operation fw_op;
	struct sram_operation sram_op;
	struct driver_operation driver_op;
	struct zf_operation zf_op;
};
struct hx_hid_rd_data_t {
	u8 *rd_data;
	u32 rd_length;
};
union hx_dword_data_t {
	u32 dword;
	u8 byte[4];
};

enum hid_reg_action {
	REG_READ = 0,
	REG_WRITE = 1
};

enum hid_reg_types {
	REG_TYPE_EXT_AHB,
	REG_TYPE_EXT_SRAM,
	REG_TYPE_EXT_TYPE = 0xFFFFFFFF
};

struct rd_feature_unit_t {
	u8 id_tag;
	u8 id;
	u8 usage_tag;
	u8 usage;
	u8 report_cnt_tag;
	u16 report_cnt;
	u8 feature_tag[2];
} __packed;
struct hx_hid_req_cfg_t {
	u32 processing_id;
	u32 data_type;
	u32 self_test_type;
	u32 handshake_set;
	u32 handshake_get;
	struct firmware *fw;
	u32 current_size;
	// HID REG READ/WRITE format:
	// STANDARD TYPE
	// [ID:1][READ/WRITE:1][REG_ADDR:4][REG_DATA:4] : 10 bytes
	//  0     1             2~5         6~9
	// EXT TYPE
	// [ID:1][READ/WRITE:1][0xFFFFFFFF][REG_TYPE:1][REG_ADDR:1|4][REG_DATA:1~256]
	//  0	  1             2~5         6           7|7~10        8~263|11~266
	union hx_dword_data_t reg_addr;
	u32 reg_addr_sz;
	u8 reg_data[HID_REG_SZ_MAX - 1 - 4];
	u32 reg_data_sz;
	u32 input_RD_de;
};

enum cell_type {
	CHIP_IS_ON_CELL,
	CHIP_IS_IN_CELL
};

#define HX_FULL_STACK_RAWDATA_SIZE \
	(HX_STACK_ORG_LEN +\
	(2 + FIX_HX_RX_NUM * FIX_HX_TX_NUM + FIX_HX_TX_NUM + FIX_HX_RX_NUM)\
	* 2)

struct himax_ic_data {
	int vendor_fw_ver;
	int vendor_config_ver;
	int vendor_touch_cfg_ver;
	int vendor_display_cfg_ver;
	int vendor_cid_maj_ver;
	int vendor_cid_min_ver;
	int vendor_panel_ver;
	int vendor_sensor_id;
	int ic_adc_num;
	u8 vendor_cus_info[12];
	u8 vendor_proj_info[12];
	u32 flash_size;
	u32 HX_RX_NUM;
	u32 HX_TX_NUM;
	u32 HX_BT_NUM;
	u32 HX_X_RES;
	u32 HX_Y_RES;
	u32 HX_MAX_PT;
	u8 HX_INT_IS_EDGE;
	u8 HX_STYLUS_FUNC;
	u8 HX_STYLUS_ID_V2;
	u8 HX_STYLUS_RATIO;
	u32 icid;
	bool enc16bits;
	bool has_flash;
};

enum HX_TS_PATH {
	HX_REPORT_COORD = 1,
	HX_REPORT_COORD_RAWDATA,
};

enum HX_TS_STATUS {
	HX_TS_GET_DATA_FAIL = -4,
	HX_EXCP_EVENT,
	HX_CHKSUM_FAIL,
	HX_PATH_FAIL,
	HX_TS_NORMAL_END = 0,
	HX_EXCP_REC_OK,
	HX_READY_SERVE,
	HX_REPORT_DATA,
	HX_EXCP_WARNING,
	HX_IC_RUNNING,
	HX_ZERO_EVENT_COUNT,
	HX_RST_OK,
};

enum HX_ERROR_CODE {
	NO_ERR = 0,
	READY_TO_SERVE = 1,
	WORK_OUT = 2,
	HX_EMBEDDED_FW = 3,
	BUS_FAIL = -1,
	HX_INIT_FAIL = -1,
	MEM_ALLOC_FAIL = -2,
	CHECKSUM_FAIL = -3,
	GESTURE_DETECT_FAIL = -4,
	INPUT_REGISTER_FAIL = -5,
	FW_NOT_READY = -6,
	LENGTH_FAIL = -7,
	OPEN_FILE_FAIL = -8,
	PROBE_FAIL = -9,
	ERR_WORK_OUT = -10,
	ERR_STS_WRONG = -11,
	ERR_TEST_FAIL = -12,
	HW_CRC_FAIL = 1
};

struct himax_platform_data {
	struct himax_ts_data *ts;
	u16 pid;
	bool power_off_3v3;
	u8 cable_config[2];
	int gpio_irq;
	int of_irq;
	int gpio_reset;
	int ic_det_delay;
	int ic_resume_delay;
	int panel_id;
	bool is_zf;
	struct regulator *vccd_supply;
	struct regulator *vcca_supply;
};

struct hx_hid_fw_unit_t {
	u8 cmd;
	u16 bin_start_offset;
	u16 unit_sz;
} __packed;

struct hx_bin_desc_t {
	u16 passwd;
	u16 cid;
	u8 panel_ver;
	u16 fw_ver;
	u8 ic_sign;
	char customer[12];
	char project[12];
	char fw_major[12];
	char fw_minor[12];
	char date[12];
	char ic_sign_2[12];
} __packed;

struct hx_hid_desc_t {
	u16 desc_length;
	u16 bcd_version;
	u16 report_desc_length;
	u16 max_input_length;
	u16 max_output_length;
	u16 max_fragment_length;
	u16 vendor_id;
	u16 product_id;
	u16 version_id;
	u16 flags;
	u32 reserved;
} __packed;
struct hx_hid_info_t {
	struct hx_hid_fw_unit_t main_mapping[9];
	struct hx_hid_fw_unit_t bl_mapping;
	struct hx_bin_desc_t fw_bin_desc;
	u16 vid;
	u16 pid;
	u8 cfg_info[32];
	u8 cfg_version;
	u8 disp_version;
	u8 rx;
	u8 tx;
	u16 yres;
	u16 xres;
	u8 pt_num;
	u8 mkey_num;
	u8 debug_info[78];
} __packed;

struct himax_ts_data {
	bool initialized;
	bool probe_finish;
	bool suspended;
	s32 notouch_frame;
	s32 ic_notouch_frame;
	atomic_t suspend_mode;
	u8 x_channel;
	u8 y_channel;
	char chip_name[30];
	u8 chip_cell_type;
	u32 chip_max_dsram_size;
	u32 ic_checksum_type;
	bool ic_boot_done;
	u32 probe_fail_flag;
	u8 *xfer_data;
	struct himax_ic_data *ic_data;

	int touch_all_size;
	int touch_info_size;

	u8 *hx_rawdata_buf;
	bool boot_upgrade_flag;
	const struct firmware *hxfw;
	bool has_alg_overlay;
	u8 *ovl_idx;
	bool zf_update_flag;
	u8 *zf_update_cfg_buffer;
#if defined(CONFIG_HID_HIMAX)
	struct time_var deferred_start;
	unsigned int ic_det_delay;
#endif

	u8 n_finger_support;
	u8 irq_enabled;

	u32 debug_log_level;

	s32 rst_gpio;
	s32 use_irq;
	s32 (*power)(s32 on);

	struct device *dev;
	struct workqueue_struct *himax_wq;
	struct work_struct work;

	struct hrtimer timer;
	struct i2c_client *client;
	struct himax_platform_data *pdata;
	/* mutex lock for reg access */
	struct mutex reg_lock;
	/* mutex lock for read/write action */
	struct mutex rw_lock;
	/* mutex lock for hid ioctl action */
	struct mutex hid_ioctl_lock;
	atomic_t irq_state;
	/* spin lock for irq */
	spinlock_t irq_lock;

/******* SPI-start *******/
	struct spi_device	*spi;
	s32 hx_irq;
	u8 *xfer_buff;
/******* SPI-end *******/

	struct hid_device *hid;
	struct hx_hid_desc_t hid_desc;
	struct hx_hid_rd_data_t hid_rd_data;
	struct hx_hid_info_t hid_info;
	struct hx_hid_req_cfg_t hid_req_cfg;
	struct hx_bin_desc_t fw_bin_desc;
	bool hid_probe;
	bool resume_success;

	s32 in_self_test;
	s32 suspend_resume_done;
	s32 bus_speed;

	s32 excp_reset_active;
	s32 excp_eb_event_flag;
	s32 excp_ec_event_flag;
	s32 excp_ed_event_flag;
	s32 excp_zero_event_count;

#if defined(CONFIG_FB)
	struct notifier_block fb_notif;
	struct workqueue_struct *himax_att_wq;
	struct delayed_work work_att;
#endif

	struct notifier_block power_notif;
	struct workqueue_struct *himax_pwr_wq;
	struct delayed_work work_pwr;

	struct workqueue_struct *himax_boot_upgrade_wq;
	struct delayed_work work_boot_upgrade;

	struct workqueue_struct *himax_hid_debug_wq;
	struct delayed_work work_hid_update;
	u8 usb_connected;
	u8 *cable_config;
	u8 latest_power_status;

	struct workqueue_struct *himax_resume_delayed_work_wq;
	struct delayed_work work_resume_delayed_work;

	u8 slave_write_reg;
	u8 slave_read_reg;
	bool acc_slave_reg;
	bool select_slave_reg;
	struct list_head list;
};
struct himax_core_fp {
/* CORE_IC */
	void (*fp_burst_enable)(struct himax_ts_data *ts, u8 auto_add_4_byte);
	int (*fp_register_read)(struct himax_ts_data *ts, u8 *addr,
			u8 *buf, u32 len);
	int (*fp_reg_read)(struct himax_ts_data *ts, struct hx_reg_t *addr,
			struct data_pack_t *data);
	int (*fp_register_write)(struct himax_ts_data *ts, u8 *addr,
			u8 *val, u32 len);
	int (*fp_reg_write)(struct himax_ts_data *ts, struct hx_reg_t *addr,
			struct data_pack_t *data);
	void (*fp_interface_on)(struct himax_ts_data *ts);
	void (*fp_sense_on)(struct himax_ts_data *ts, u8 flash_mode);
	bool (*fp_sense_off)(struct himax_ts_data *ts, bool check_en);
	void (*fp_power_on_init)(struct himax_ts_data *ts);
/* CORE_IC */

/* CORE_FW */
	void (*fp_system_reset)(struct himax_ts_data *ts);
	void (*fp_usb_detect_set)(struct himax_ts_data *ts,
			const u8 *cable_config);
	void (*fp_diag_register_set)(struct himax_ts_data *ts,
			u8 diag_command);
	void (*fp_reload_disable)(struct himax_ts_data *ts, int disable);
	void (*fp_read_FW_ver)(struct himax_ts_data *ts);
	void (*fp_read_FW_status)(struct himax_ts_data *ts);
	void (*fp_irq_switch)(struct himax_ts_data *ts, int switch_on);
	int (*fp_assign_sorting_mode)(struct himax_ts_data *ts, u8 *tmp_data);
	void (*fp_ap_notify_fw_sus)(struct himax_ts_data *ts, int suspend);
/* CORE_FW */
/* CORE_FLASH */
	int (*fp_diff_overlay_flash)(struct himax_ts_data *ts);
/* CORE_FLASH */
/* CORE_DRIVER */
	bool (*fp_chip_detect)(struct himax_ts_data *ts);
	void (*fp_chip_init)(struct himax_ts_data *ts);
	void (*fp_pin_reset)(struct himax_ts_data *ts);
	void (*fp_ic_reset)(struct himax_ts_data *ts,
		    u8 loadconfig, u8 int_off);
	u8 (*fp_tp_info_check)(struct himax_ts_data *ts);
	void (*fp_touch_information)(struct himax_ts_data *ts);
	void (*fp_calc_touch_data_size)(struct himax_ts_data *ts);
	int (*fp_ic_excp_recovery)(struct himax_ts_data *ts,
		u32 hx_excp_event,
		u32 hx_zero_event, u32 length);
	void (*fp_excp_ic_reset)(struct himax_ts_data *ts);
	void (*fp_resend_cmd_func)(struct himax_ts_data *ts, bool suspended);
	bool (*fp_read_event_stack)(struct himax_ts_data *ts, u8 *buf,
				    u32 length);
	void (*fp_suspend_proc)(struct himax_ts_data *ts, bool suspended);
	void (*fp_resume_proc)(struct himax_ts_data *ts, bool suspended);
/* CORE_DRIVER */
/* INSPECTION*/
/* INSPECTION*/
};

#define FLASH_VER_GET_VAL(_reg) REG_GET_VAL(g_core_regs.flash_ver_info._reg)
#define IC_GET_VAL(_reg) REG_GET_VAL(g_core_regs.ic_op._reg)
#define FW_GET_VAL(_reg) REG_GET_VAL(g_core_regs.fw_op._reg)
#define FLASH_GET_VAL(_reg) REG_GET_VAL(g_core_regs.flash_op._reg)
#define SRAM_GET_VAL(_reg) REG_GET_VAL(g_core_regs.sram_op._reg)
#define DRV_GET_VAL(_reg) REG_GET_VAL(g_core_regs.driver_op._reg)
#define ZF_GET_VAL(_reg) REG_GET_VAL(g_core_regs.zf_op._reg)
#define REG_GET_ARRAY(_reg) \
	({ \
		_reg.data.byte; \
	})
#define FLASH_VER_GET_ARRAY(_reg) REG_GET_ARRAY(g_core_regs.flash_ver_info._reg)
#define IC_GET_ARRAY(_reg) REG_GET_ARRAY(g_core_regs.ic_op._reg)
#define FW_GET_ARRAY(_reg) REG_GET_ARRAY(g_core_regs.fw_op._reg)
#define FLASH_GET_ARRAY(_reg) REG_GET_ARRAY(g_core_regs.flash_op._reg)
#define SRAM_GET_ARRAY(_reg) REG_GET_ARRAY(g_core_regs.sram_op._reg)
#define DRV_GET_ARRAY(_reg) REG_GET_ARRAY(g_core_regs.driver_op._reg)
#define ZF_GET_ARRAY(_reg) REG_GET_ARRAY(g_core_regs.zf_op._reg)
#define REG_GET_SZ(_reg) \
	({ \
		_reg.len; \
	})
#define FLASH_VER_GET_SZ(_reg) REG_GET_SZ(g_core_regs.flash_ver_info._reg)
#define IC_GET_SZ(_reg) REG_GET_SZ(g_core_regs.ic_op._reg)
#define FW_GET_SZ(_reg) REG_GET_SZ(g_core_regs.fw_op._reg)
#define FLASH_GET_SZ(_reg) REG_GET_SZ(g_core_regs.flash_op._reg)
#define SRAM_GET_SZ(_reg) REG_GET_SZ(g_core_regs.sram_op._reg)
#define DRV_GET_SZ(_reg) REG_GET_SZ(g_core_regs.driver_op._reg)
#define ZF_GET_SZ(_reg) REG_GET_SZ(g_core_regs.zf_op._reg)
void himax_rst_gpio_set(int pinnum, u8 value);
void himax_cable_detect_func(struct himax_ts_data *ts, bool force_renew);
int himax_report_data_init(struct himax_ts_data *ts);

int himax_bus_read(struct himax_ts_data *ts, u8 cmd, u8 *buf,
		   u32 len);
int himax_bus_write(struct himax_ts_data *ts, u8 cmd, u8 *addr,
		    u8 *data, u32 len);

void himax_int_enable(struct himax_ts_data *ts, int enable);

union host_ext_rd_t {
	struct __packed rd_struct_t {
		u8 header[14];
		struct rd_feature_unit_t cfg;// ID_CFG
		struct rd_feature_unit_t reg_rw;// ID_REG_RW
		struct rd_feature_unit_t monitor_sel;// ID_TOUCH_MONITOR_SEL
		struct rd_feature_unit_t monitor;// ID_TOUCH_MONITOR
		// rd_feature_unit_t monitor_partial;// ID_TOUCH_MONITOR_PARTIAL
		struct rd_feature_unit_t fw_update;// ID_FW_UPDATE
		struct rd_feature_unit_t fw_update_handshaking;// ID_FW_UPDATE_HANDSHAKING
		struct rd_feature_unit_t self_test;// ID_SELF_TEST
		struct rd_feature_unit_t input_rd_en;// ID_INPUT_RD_DE
		u8 end_collection;
	} rd_struct;
	u8 host_report_descriptor[sizeof(struct rd_struct_t)];
};

union heatmap_rd_t {
	struct __packed heatmap_struct_t {
		u8 header[17];
		u8 heatmap_info_desc[29];
		u8 heatmap_data_hdr[9];
		u8 heatmap_data_cnt_tag;
		u16 heatmap_data_cnt;
		u8 heatmap_input_desc[2];
		u8 end_collection;
	} heatmap_struct;
	u8 host_report_descriptor[sizeof(struct heatmap_struct_t)];
};

/* FW Auto upgrade case, you need to setup the fix_touch_info of module
 */

#define HID_REPORT_HDR_SZ (2)
#if defined(CONFIG_OF)
int himax_parse_dt(struct device_node *dt, struct himax_platform_data *pdata);
#endif
void himax_ts_work(struct himax_ts_data *ts);
enum hrtimer_restart himax_ts_timer_func(struct hrtimer *timer);
int himax_resume(struct device *dev);
int himax_suspend(struct device *dev);

// int himax_spi_drv_init(struct himax_ts_data *ts);
void himax_spi_drv_exit(void);
int himax_chip_init(struct himax_ts_data *ts);
int himax_report_data_init(struct himax_ts_data *ts);
void himax_cable_detect_func(struct himax_ts_data *ts, bool force_renew);
#endif
