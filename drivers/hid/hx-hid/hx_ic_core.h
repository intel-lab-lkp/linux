/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __HIMAX_IC_CORE_H__
#define __HIMAX_IC_CORE_H__

#include "hx_core.h"
#include <linux/slab.h>

#define DATA_LEN_8				8
#define DATA_LEN_4				4
#define ADDR_LEN_4				4
#define FLASH_RW_MAX_LEN		256
#define FLASH_WRITE_BURST_SZ	8
#define MAX_I2C_TRANS_SZ		128
#define HIMAX_REG_RETRY_TIMES	5
#define FW_BIN_16K_SZ			0x4000
#define HIMAX_TOUCH_DATA_SIZE	128
#define MASK_BIT_0				0x01
#define MASK_BIT_1				0x02
#define MASK_BIT_2				0x04

#define FW_SECTOR_PER_BLOCK		8
#define FW_PAGE_PER_SECTOR		64
#define FW_PAGE_SZ			128
#define HX256B					0x100
#define HX1K					0x400
#define HX4K					0x1000
#define HX_32K_SZ				0x8000
#define HX_40K_SZ				0xA000
#define HX_48K_SZ				0xC000
#define HX64K					0x10000
#define HX124K					0x1f000
#define HX4000K					0x1000000

#define HX_NORMAL_MODE			1
#define HX_SORTING_MODE			2
#define HX_CHANGE_MODE_FAIL		(-1)
#define HX_RW_REG_FAIL			(-1)
#define HX_DRIVER_MAX_IC_NUM	12

/* CORE_INIT */
/* CORE_IC */
/* CORE_FW */
/* CORE_FLASH */
/* CORE_SRAM */
/* CORE_DRIVER */

void himax_mcu_in_cmd_struct_free(void);
void himax_rst_gpio_set(int pinnum, u8 value);
void himax_cable_detect_func(struct himax_ts_data *ts, bool force_renew);
int himax_report_data_init(struct himax_ts_data *ts);

enum HX_FLASH_SPEED_ENUM {
	HX_FLASH_SPEED_25M = 0,
	HX_FLASH_SPEED_12p5M = 0x01,
	HX_FLASH_SPEED_8p3M = 0x02,
	HX_FLASH_SPEED_6p25M = 0x03,
};

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
	#define ic_addr_adc_on_rst               0x80020094
	#define ic_adr_psl                       0x900000A0
	#define ic_adr_cs_central_state          0x900000A8
	#define ic_cmd_rst                       0x00000000
	#define ic_adr_osc_en                    0x900880A8
	#define ic_adr_osc_pw                    0x900880E0
/* CORE_IC */

/* CORE_FW */
	#define fw_addr_system_reset                0x90000018
	#define fw_addr_ctrl_fw                     0x9000005c
	#define fw_addr_flag_reset_event            0x900000e4
	#define fw_addr_hsen_enable                 0x10007F14
	#define fw_usb_detect_addr                  0x10007F38
	#define fw_addr_program_reload_from         0x00000000
	#define fw_addr_program_reload_to           0x08000000
	#define fw_addr_program_reload_page_write   0x0000fb00
	#define fw_addr_raw_out_sel                 0x800204b4
	#define fw_addr_reload_status               0x80050000
	#define fw_addr_reload_crc32_result         0x80050018
	#define fw_addr_reload_addr_from            0x80050020
	#define fw_addr_reload_addr_cmd_beat        0x80050028
	#define fw_data_system_reset                0x00000055
	#define fw_data_safe_mode_release_pw_active 0x00000053
	#define fw_data_safe_mode_release_pw_reset  0x00000000
	#define fw_data_clear                       0x00000000
	#define fw_data_fw_stop                     0x000000A5
	#define fw_data_program_reload_start        0x0A3C3000
	#define fw_data_program_reload_compare      0x04663000
	#define fw_data_program_reload_break        0x15E75678
	#define fw_addr_selftest_addr_en            0x10007F18
	#define fw_addr_selftest_result_addr        0x10007f24
	#define fw_data_selftest_request            0x00006AA6
	#define fw_addr_criteria_addr               0x10007f1c
	#define fw_data_criteria_aa_top             0x64
	#define fw_data_criteria_aa_bot             0x00
	#define fw_data_criteria_key_top            0x64
	#define fw_data_criteria_key_bot            0x00
	#define fw_data_criteria_avg_top            0x64
	#define fw_data_criteria_avg_bot            0x00
	#define fw_addr_set_frame_addr              0x10007294
	#define fw_data_set_frame                   0x0000000A
	#define fw_data_selftest_ack_hb             0xa6
	#define fw_data_selftest_ack_lb             0x6a
	#define fw_data_selftest_pass               0xaa
	#define fw_data_normal_cmd                  0x00
	#define fw_data_normal_status               0x99
	#define fw_data_sorting_cmd                 0xaa
	#define fw_data_sorting_status              0xcc
	#define fw_data_idle_dis_pwd                0x17
	#define fw_data_idle_en_pwd                 0x1f
	#define fw_addr_sorting_mode_en             0x10007f04
	#define fw_addr_fw_mode_status              0x10007088
	#define fw_addr_icid_addr                   0x900000d0
	#define fw_addr_fw_ver_addr                 0x10007004
	#define fw_addr_fw_cfg_addr                 0x10007084
	#define fw_addr_fw_vendor_addr              0x10007000
	#define fw_addr_cus_info                    0x10007008
	#define fw_addr_proj_info                   0x10007014
	#define fw_addr_fw_state_addr               0x900000f8
	#define fw_addr_fw_dbg_msg_addr             0x10007f40
	#define fw_addr_chk_fw_status               0x900000a8
	#define fw_addr_chk_dd_status               0x900000E8
	#define fw_addr_dd_handshak_addr            0x900000fc
	#define fw_addr_dd_data_addr                0x10007f80
	#define fw_addr_clr_fw_record_dd_sts        0x10007FCC
	#define fw_addr_ap_notify_fw_sus            0x10007FD0
	#define fw_data_ap_notify_fw_sus_en         0xA55AA55A
	#define fw_data_ap_notify_fw_sus_dis        0x00000000
	#define fw_data_dd_request                  0xaa
	#define fw_data_dd_ack                      0xbb
	#define fw_data_rawdata_ready_hb            0xa3
	#define fw_data_rawdata_ready_lb            0x3a
	#define fw_addr_ahb_addr                    0x11
	#define fw_data_ahb_dis                     0x00
	#define fw_data_ahb_en                      0x01
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
	#define flash_addr_spi200_trans_fmt    (flash_addr_ctrl_base + 0x10)
	#define flash_addr_spi200_trans_ctrl   (flash_addr_ctrl_base + 0x20)
	#define flash_addr_spi200_cmd          (flash_addr_ctrl_base + 0x24)
	#define flash_addr_spi200_addr         (flash_addr_ctrl_base + 0x28)
	#define flash_addr_spi200_data         (flash_addr_ctrl_base + 0x2c)
	#define flash_addr_spi200_fifo_rst   (flash_addr_ctrl_base + 0x30)
	#define flash_addr_spi200_rst_status   (flash_addr_ctrl_base + 0x34)
	#define flash_addr_spi200_flash_speed  (flash_addr_ctrl_base + 0x40)
	#define flash_addr_spi200_bt_num       (flash_addr_ctrl_base + 0xe8)
	#define flash_data_spi200_txfifo_rst   0x00000004
	#define flash_data_spi200_rxfifo_rst   0x00000002
	#define flash_data_spi200_trans_fmt    0x00020780
	#define flash_data_spi200_trans_ctrl_1 0x42000003
	#define flash_data_spi200_trans_ctrl_2 0x47000000
	#define flash_data_spi200_trans_ctrl_3 0x67000000
	#define flash_data_spi200_trans_ctrl_4 0x610ff000
	#define flash_data_spi200_trans_ctrl_5 0x694002ff
	#define flash_data_spi200_trans_ctrl_6 0x42000000
	#define flash_data_spi200_trans_ctrl_7 0x6940020f
	#define flash_data_spi200_cmd_1        0x00000005
	#define flash_data_spi200_cmd_2        0x00000006
	#define flash_data_spi200_cmd_3        0x000000C7
	#define flash_data_spi200_cmd_4        0x000000D8
	#define flash_data_spi200_cmd_5        0x00000020
	#define flash_data_spi200_cmd_6        0x00000002
	#define flash_data_spi200_cmd_7        0x0000003b
	#define flash_data_spi200_cmd_8        0x00000003
	#define flash_data_spi200_addr         0x00000000
	#define flash_clk_setup_addr           0x80000040
/* CORE_FLASH */

/* CORE_SRAM */
	#define sram_adr_mkey         0x100070E8
	#define sram_adr_rawdata_addr 0x10000000
	#define sram_adr_rawdata_end  0x00000000
	#define	sram_passwrd_start    0x5AA5
	#define	sram_passwrd_end      0xA55A
/* CORE_SRAM */

/* CORE_DRIVER */
	#define driver_addr_fw_define_flash_reload              0x10007f00
	#define driver_addr_fw_define_2nd_flash_reload          0x100072c0
	#define driver_data_fw_define_flash_reload_dis          0x0000a55a
	#define driver_data_fw_define_flash_reload_en           0x00000000
	#define driver_addr_fw_define_int_is_edge               0x10007088
	#define driver_addr_fw_define_rxnum_txnum               0x100070f4
	#define driver_data_fw_define_rxnum_txnum_maxpt_sorting 0x00000008
	#define driver_data_fw_define_rxnum_txnum_maxpt_normal  0x00000014
	#define driver_addr_fw_define_maxpt_xyrvs               0x100070f8
	#define driver_addr_fw_define_x_y_res                   0x100070fc
	#define driver_data_df_rx                               36
	#define driver_data_df_tx                               18
	#define driver_data_df_pt                               10
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
	#define zf_addr_sts_chk          0x900000A8
	#define zf_data_activ_sts        0x05
	#define zf_addr_activ_relod      0x90000048
	#define zf_data_activ_in         0xEC

	#define ovl_section_num      3
	#define ovl_gesture_request  0x11
	#define ovl_gesture_reply    0x22
	#define ovl_border_request   0x55
	#define ovl_border_reply     0x66
	#define ovl_sorting_request  0x99
	#define ovl_sorting_reply    0xAA
	#define ovl_fault            0xFF

	#define ovl_alg_request  0x11111111
	#define ovl_alg_reply    0x22222222
	#define ovl_alg_fault    0xFF

struct zf_info {
	u8 sram_addr[4];
	int write_size;
	u32 fw_addr;
	u32 cfg_addr;
};

/* New Version 1K*/
enum bin_desc_map_table {
	TP_CONFIG_TABLE = 0x00000A00,
	FW_CID = 0x10000000,
	FW_VER = 0x10000100,
	CFG_VER = 0x10000600,
	HID_TABLE = 0x30000100,
	FW_BIN_DESC = 0x10000000
};

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
	struct hx_reg_t addr_adc_on_rst;
	struct hx_reg_t addr_psl;
	struct hx_reg_t addr_cs_central_state;
	struct hx_reg_t data_rst;
	struct hx_reg_t adr_osc_en;
	struct hx_reg_t adr_osc_pw;
};

struct fw_operation {
	struct hx_reg_t addr_system_reset;
	struct hx_reg_t addr_ctrl_fw_isr;
	struct hx_reg_t addr_flag_reset_event;
	struct hx_reg_t addr_hsen_enable;
	struct hx_reg_t addr_program_reload_from;
	struct hx_reg_t addr_program_reload_to;
	struct hx_reg_t addr_program_reload_page_write;
	struct hx_reg_t addr_raw_out_sel;
	struct hx_reg_t addr_reload_status;
	struct hx_reg_t addr_reload_crc32_result;
	struct hx_reg_t addr_reload_addr_from;
	struct hx_reg_t addr_reload_addr_cmd_beat;
	struct hx_reg_t addr_selftest_addr_en;
	struct hx_reg_t addr_criteria_addr;
	struct hx_reg_t addr_set_frame_addr;
	struct hx_reg_t addr_selftest_result_addr;
	struct hx_reg_t addr_sorting_mode_en;
	struct hx_reg_t addr_fw_mode_status;
	struct hx_reg_t addr_icid_addr;
	struct hx_reg_t addr_fw_ver_addr;
	struct hx_reg_t addr_fw_cfg_addr;
	struct hx_reg_t addr_fw_vendor_addr;
	struct hx_reg_t addr_cus_info;
	struct hx_reg_t addr_proj_info;
	struct hx_reg_t addr_fw_state_addr;
	struct hx_reg_t addr_fw_dbg_msg_addr;
	struct hx_reg_t addr_chk_fw_status;
	struct hx_reg_t addr_dd_handshak_addr;
	struct hx_reg_t addr_dd_data_addr;
	struct hx_reg_t addr_clr_fw_record_dd_sts;
	struct hx_reg_t addr_ap_notify_fw_sus;
	struct hx_reg_t data_ap_notify_fw_sus_en;
	struct hx_reg_t data_ap_notify_fw_sus_dis;
	struct hx_reg_t data_system_reset;
	struct hx_reg_t data_safe_mode_release_pw_active;
	struct hx_reg_t data_safe_mode_release_pw_reset;
	struct hx_reg_t data_clear;
	struct hx_reg_t data_fw_stop;
	struct hx_reg_t data_program_reload_start;
	struct hx_reg_t data_program_reload_compare;
	struct hx_reg_t data_program_reload_break;
	struct hx_reg_t data_selftest_request;
	struct hx_reg_t data_criteria_aa_top;
	struct hx_reg_t data_criteria_aa_bot;
	struct hx_reg_t data_criteria_key_top;
	struct hx_reg_t data_criteria_key_bot;
	struct hx_reg_t data_criteria_avg_top;
	struct hx_reg_t data_criteria_avg_bot;
	struct hx_reg_t data_set_frame;
	struct hx_reg_t data_selftest_ack_hb;
	struct hx_reg_t data_selftest_ack_lb;
	struct hx_reg_t data_selftest_pass;
	struct hx_reg_t data_normal_cmd;
	struct hx_reg_t data_normal_status;
	struct hx_reg_t data_sorting_cmd;
	struct hx_reg_t data_sorting_status;
	struct hx_reg_t data_dd_request;
	struct hx_reg_t data_dd_ack;
	struct hx_reg_t data_idle_dis_pwd;
	struct hx_reg_t data_idle_en_pwd;
	struct hx_reg_t data_rawdata_ready_hb;
	struct hx_reg_t data_rawdata_ready_lb;
	struct hx_reg_t addr_ahb_addr;
	struct hx_reg_t data_ahb_dis;
	struct hx_reg_t data_ahb_en;
	struct hx_reg_t addr_event_addr;
	struct hx_reg_t addr_usb_detect;
};

struct flash_operation {
	struct hx_reg_t addr_spi200_trans_fmt;
	struct hx_reg_t addr_spi200_trans_ctrl;
	struct hx_reg_t addr_spi200_fifo_rst;
	struct hx_reg_t addr_spi200_rst_status;
	struct hx_reg_t addr_spi200_flash_speed;
	struct hx_reg_t addr_spi200_cmd;
	struct hx_reg_t addr_spi200_addr;
	struct hx_reg_t addr_spi200_data;
	struct hx_reg_t addr_spi200_bt_num;

	struct hx_reg_t data_spi200_txfifo_rst;
	struct hx_reg_t data_spi200_rxfifo_rst;
	struct hx_reg_t data_spi200_trans_fmt;
	struct hx_reg_t data_spi200_trans_ctrl_1;
	struct hx_reg_t data_spi200_trans_ctrl_2;
	struct hx_reg_t data_spi200_trans_ctrl_3;
	struct hx_reg_t data_spi200_trans_ctrl_4;
	struct hx_reg_t data_spi200_trans_ctrl_5;
	struct hx_reg_t data_spi200_trans_ctrl_6;
	struct hx_reg_t data_spi200_trans_ctrl_7;
	struct hx_reg_t data_spi200_cmd_1;
	struct hx_reg_t data_spi200_cmd_2;
	struct hx_reg_t data_spi200_cmd_3;
	struct hx_reg_t data_spi200_cmd_4;
	struct hx_reg_t data_spi200_cmd_5;
	struct hx_reg_t data_spi200_cmd_6;
	struct hx_reg_t data_spi200_cmd_7;
	struct hx_reg_t data_spi200_cmd_8;
	struct hx_reg_t data_spi200_addr;
};

struct sram_operation {
	struct hx_reg_t addr_mkey;
	struct hx_reg_t addr_rawdata_addr;
	struct hx_reg_t addr_rawdata_end;
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
	struct hx_reg_t data_df_rx;
	struct hx_reg_t data_df_tx;
	struct hx_reg_t data_df_pt;
	struct hx_reg_t data_fw_define_flash_reload_dis;
	struct hx_reg_t data_fw_define_flash_reload_en;
	struct hx_reg_t data_fw_define_rxnum_txnum_maxpt_sorting;
	struct hx_reg_t data_fw_define_rxnum_txnum_maxpt_normal;
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
	struct hx_reg_t addr_sts_chk;
	struct hx_reg_t data_activ_sts;
	struct hx_reg_t addr_activ_relod;
	struct hx_reg_t data_activ_in;
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
	struct flash_operation flash_op;
	struct sram_operation sram_op;
	struct driver_operation driver_op;
	struct zf_operation zf_op;
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
	void (*fp_tcon_on)(struct himax_ts_data *ts);
	bool (*fp_watch_dog_off)(struct himax_ts_data *ts);
	bool (*fp_sense_off)(struct himax_ts_data *ts, bool check_en);
	void (*fp_sleep_in)(struct himax_ts_data *ts);
	bool (*fp_wait_wip)(struct himax_ts_data *ts, int timing);
	void (*fp_init_psl)(struct himax_ts_data *ts);
	void (*fp_resume_ic_action)(struct himax_ts_data *ts);
	void (*fp_suspend_ic_action)(struct himax_ts_data *ts);
	void (*fp_power_on_init)(struct himax_ts_data *ts);
	bool (*fp_slave_tcon_reset)(struct himax_ts_data *ts);
	bool (*fp_slave_adc_reset_slave)(struct himax_ts_data *ts);
	bool (*fp_slave_wdt_off_slave)(struct himax_ts_data *ts);
/* CORE_IC */

/* CORE_FW */
	void (*fp_system_reset)(struct himax_ts_data *ts);
	int (*fp_calculate_crc_with_ap)(const unsigned char *fw_content,
					int crc_from_fw,
			int len);
	u32 (*fp_check_crc)(struct himax_ts_data *ts, u8 *start_addr,
			    int reload_length);
	void (*fp_set_reload_cmd)(u8 *write_data,
				  int idx,
			u32 cmd_from,
			u32 cmd_to,
			u32 cmd_beat);
	bool (*fp_program_reload)(void);
	void (*fp_diag_register_set)(struct himax_ts_data *ts,
				     u8 diag_command);
	int (*fp_diag_register_get)(struct himax_ts_data *ts,
				    u32 *diag_value);
	void (*fp_clr_fw_reord_dd_sts)(struct himax_ts_data *ts);
	void (*fp_ap_notify_fw_sus)(struct himax_ts_data *ts, int suspend);
	int (*fp_chip_self_test)(struct seq_file *s, void *v);
	void (*fp_idle_mode)(struct himax_ts_data *ts, int disable);
	void (*fp_reload_disable)(struct himax_ts_data *ts, int disable);
	int (*fp_read_ic_trigger_type)(struct himax_ts_data *ts);
	void (*fp_read_FW_ver)(struct himax_ts_data *ts);
	bool (*fp_read_event_stack)(struct himax_ts_data *ts, u8 *buf,
				    u32 length);
	void (*fp_return_event_stack)(struct himax_ts_data *ts);
	bool (*fp_calculate_checksum)(struct himax_ts_data *ts, bool change_iref,
				      u32 size);
	void (*fp_read_FW_status)(struct himax_ts_data *ts);
	void (*fp_irq_switch)(struct himax_ts_data *ts, int switch_on);
	int (*fp_assign_sorting_mode)(struct himax_ts_data *ts, u8 *tmp_data);
	int (*fp_check_sorting_mode)(struct himax_ts_data *ts, u8 *tmp_data);
	int (*fp_get_max_dc)(void);
	u8 (*fp_read_DD_status)(struct himax_ts_data *ts, u8 *cmd_set,
				u8 *tmp_data);
	int (*fp_ulpm_in)(void);
	int (*fp_black_gest_ctrl)(bool enable);
	int	(*fp_diff_overlay_bin)(void);
/* CORE_FW */

/* CORE_FLASH */
	void (*fp_chip_erase)(struct himax_ts_data *ts);
	bool (*fp_block_erase)(struct himax_ts_data *ts, int start_addr,
			       int length);
	bool (*fp_sector_erase)(int start_addr);
	bool (*fp_flash_programming)(struct himax_ts_data *ts,
				     u8 *fw_content, int fw_size);
	void (*fp_flash_page_write)(u8 *write_addr, int length,
				    u8 *write_data);
	int (*fp_fts_ctpm_fw_upgrade_with_sys_fs_32k)(struct himax_ts_data *ts,
						      unsigned char *fw, int len, bool change_iref);
	int (*fp_fts_ctpm_fw_upgrade_with_sys_fs_60k)(struct himax_ts_data *ts,
						      unsigned char *fw, int len, bool change_iref);
	int (*fp_fts_ctpm_fw_upgrade_with_sys_fs_64k)(struct himax_ts_data *ts,
						      unsigned char *fw, int len, bool change_iref);
	int (*fp_fts_ctpm_fw_upgrade_with_sys_fs_124k)
		(struct himax_ts_data *ts, unsigned char *fw,
		 int len, bool change_iref);
	int (*fp_fts_ctpm_fw_upgrade_with_sys_fs_128k)
		(struct himax_ts_data *ts, unsigned char *fw,
		 int len, bool change_iref);
	int (*fp_fts_ctpm_fw_upgrade_with_sys_fs_255k)
		(struct himax_ts_data *ts, unsigned char *fw,
		 int len, bool change_iref);
	void (*fp_flash_dump_func)(struct himax_ts_data *ts,
				   u8 local_flash_command, int flash_size,
				   u8 *flash_buffer);
	bool (*fp_flash_lastdata_check)(struct himax_ts_data *ts, u32 size);
	bool (*fp_bin_desc_get)(unsigned char *fw, struct himax_ts_data *ts,
				u32 max_sz);
	bool (*fp_ahb_squit)(void);
	void (*fp_flash_read)(u8 *r_data, int start_addr, int length);
	bool (*fp_sfr_rw)(u8 *addr, int length,
			  u8 *data, u8 rw_ctrl);
	bool (*fp_lock_flash)(void);
	bool (*fp_unlock_flash)(void);
	void (*fp_init_auto_func)(void);
	int (*fp_diff_overlay_flash)(struct himax_ts_data *ts);
/* CORE_FLASH */

/* CORE_SRAM */
	void (*fp_sram_write)(struct himax_ts_data *ts, u8 *FW_content);
	bool (*fp_sram_verify)(struct himax_ts_data *ts, u8 *fw_file,
			       int fw_size);
	bool (*fp_get_DSRAM_data)(struct himax_ts_data *ts, u8 *info_data,
				  bool dsram_flag);
/* CORE_SRAM */

/* CORE_DRIVER */
	bool (*fp_chip_detect)(struct himax_ts_data *ts);
	void (*fp_chip_init)(struct himax_ts_data *ts);
	void (*fp_pin_reset)(struct himax_ts_data *ts);
	u8 (*fp_tp_info_check)(struct himax_ts_data *ts);
	void (*fp_touch_information)(struct himax_ts_data *ts);
	void (*fp_calc_touch_data_size)(struct himax_ts_data *ts);
	void (*fp_reload_config)(void);
	int (*fp_get_touch_data_size)(void);
	void (*fp_usb_detect_set)(struct himax_ts_data *ts,
				  const u8 *cable_config);
	int (*fp_hand_shaking)(void);
	int (*fp_determin_diag_rawdata)(int diag_command);
	int (*fp_determin_diag_storage)(int diag_command);
	int (*fp_cal_data_len)(int raw_cnt_rmd, int HX_MAX_PT, int raw_cnt_max);
	bool (*fp_diag_check_sum)(struct himax_ts_data *ts);

	void (*fp_ic_reset)(struct himax_ts_data *ts,
			    u8 loadconfig, u8 int_off);
	int (*fp_ic_excp_recovery)
			(struct himax_ts_data *ts,
			u32 hx_excp_event,
			u32 hx_zero_event, u32 length);
	void (*fp_excp_ic_reset)(struct himax_ts_data *ts);
	void (*fp_resend_cmd_func)(struct himax_ts_data *ts, bool suspended);
/* CORE_DRIVER */
	int (*fp_turn_on_mp_func)(struct himax_ts_data *ts, int on);
	void (*fp_clean_sram_0f)(struct himax_ts_data *ts, u8 *addr,
				 int write_len, int type);
	void (*fp_write_sram_0f)(struct himax_ts_data *ts, u8 *addr,
				 const u8 *data, u32 len);
	int (*fp_write_sram_0f_crc)(struct himax_ts_data *ts, u8 *addr,
				    const u8 *data, u32 len);
	int (*fp_firmware_update_0f)(const struct firmware *fw_entry,
				     struct himax_ts_data *ts, int type);
	int (*fp_0f_op_file_dirly)(char *file_name, struct himax_ts_data *ts);
	int (*fp_0f_excp_check)(void);
	void (*fp_0f_reload_to_active)(struct himax_ts_data *ts);
	void (*_en_hw_crc)(struct himax_ts_data *ts, int en);
	void (*fp_read_sram_0f)(struct himax_ts_data *ts,
				const struct firmware *fw_entry,
				u8 *addr, int start_index, int read_len);
	void (*fp_read_all_sram)(struct himax_ts_data *ts,
				 u8 *addr, int read_len);
	void (*fp_firmware_read_0f)(struct himax_ts_data *ts,
				    const struct firmware *fw_entry, int type);
	int (*fp_0f_overlay)(struct himax_ts_data *ts, int ovl_type, int mode);
	void (*fp_suspend_proc)(struct himax_ts_data *ts, bool suspended);
	void (*fp_resume_proc)(struct himax_ts_data *ts, bool suspended);
};

extern struct himax_core_command_regs g_core_regs;

void himax_ic_reg_init(struct himax_core_command_regs *reg_data);
void himax_ic_fp_init(void);

#endif
