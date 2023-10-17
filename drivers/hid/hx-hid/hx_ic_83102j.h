/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __HX_IC_83102J_H__
#define __HX_IC_83102J_H__

#include "hx_ic_core.h"

extern struct himax_core_fp g_core_fp;

#define hx83102d_fw_addr_raw_out_sel      0x800204f4
#define hx83102d_zf_data_adc_cfg_1        0x10007B00
#define hx83102d_zf_data_adc_cfg_2        0x10006A00
#define hx83102d_zf_data_adc_cfg_3        0x10007500
#define hx83102d_zf_data_bor_prevent_info 0x10007268
#define hx83102d_zf_data_notch_info       0x10007300
#define hx83102d_zf_func_info_en          0x10007FD0
#define hx83102d_zf_po_sub_func           0x10005A00
#define hx83102d_zf_data_sram_start_addr  0x20000000
#define hx83102d_adr_osc_en               0x9000009C
#define hx83102d_adr_osc_pw               0x90000280
#define hx83102d_data_adc_num             48
#define hx83102d_notouch_frame            0

#define hx83102e_fw_addr_raw_out_sel 0x100072EC
#define hx83102e_ic_adr_tcon_rst     0x80020004
#define hx83102e_data_df_rx          48
#define hx83102e_data_df_tx          30
#define hx83102e_data_adc_num        400 /* 200x2 */
#define hx83102e_notouch_frame       0

#define hx83102j_fw_addr_raw_out_sel 0x100072EC
#define hx83102j_ic_adr_tcon_rst     0x80020004
#define hx83102j_data_df_rx          48
#define hx83102j_data_df_tx          30
#define hx83102j_data_adc_num        400 /* 200x2 */
#define hx83102j_notouch_frame       0
#define HX83102J_FLASH_SIZE        261120

#define HX83102j_ic_addr_hw_crc 0x80010000
#define HX83102j_data_hw_crc 0x0000ECCE

#endif
