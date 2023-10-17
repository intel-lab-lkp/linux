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

#include "hx_ic_83102j.h"
#include "hx_plat.h"

struct himax_core_command_regs g_core_regs;

static void hx83102j_reload_to_active(struct himax_ts_data *ts)
{
	struct hx_reg_t addr = {0};
	u8 retry_cnt = 0;

	DEF_WORD_DATA(data);
	WORD_REG(addr, 0x90000048);

	do {
		VAL_SET(data, 0xEC);
		g_core_fp.fp_reg_write(ts, &addr, &data);
		usleep_range(1000, 1100);
		g_core_fp.fp_reg_read(ts, &addr, &data);
		I("data[1]=%d, data[0]=%d, retry_cnt=%d",
		  data.data.byte[1], data.data.byte[0], retry_cnt);
		retry_cnt++;
	} while ((data.data.byte[1] != 0x01 ||
		data.data.byte[0] != 0xEC) &&
		retry_cnt < HIMAX_REG_RETRY_TIMES);
}

static void hx83102j_en_hw_crc(struct himax_ts_data *ts, int en)
{
	struct hx_reg_t addr = {0};
	u8 retry_cnt = 0;

	DEF_WORD_DATA(data);
	DEF_WORD_DATA(wrt_data);
	WORD_REG(addr, HX83102j_ic_addr_hw_crc);

	if (en)
		VAL_SET(data, HX83102j_data_hw_crc);
	else
		VAL_SET(data, fw_data_clear);

	do {
		PTR_SET(wrt_data, data.data.byte, data.len);
		g_core_fp.fp_reg_write(ts, &addr, &data);
		usleep_range(1000, 1100);
		g_core_fp.fp_reg_read(ts, &addr, &data);
		I("ECC data[1]=%d, data[0]=%d, retry_cnt=%d",
		  data.data.byte[1], data.data.byte[0], retry_cnt);
		retry_cnt++;
	} while ((data.data.byte[1] != wrt_data.data.byte[1] ||
		data.data.byte[0] != wrt_data.data.byte[0]) &&
		retry_cnt < HIMAX_REG_RETRY_TIMES);
}

static void hx83102j_resume_ic_action(struct himax_ts_data *ts)
{
	hx83102j_reload_to_active(ts);
}

static bool hx83102j_chip_detect(struct himax_ts_data *ts)
{
	DEF_WORD_DATA(tmp_data);
	struct hx_reg_t tmp_addr;
	bool ret_data = false;
	int ret = 0;
	int i = 0;
	bool check_flash;

	g_core_fp.fp_pin_reset(ts);
	ret = himax_bus_read(ts, 0x13, tmp_data.data.byte, 1);
	if (ret < 0) {
		E("bus access fail!");
		return false;
	}

	if (!ts->ic_data->has_flash)
		check_flash = false;
	else
		check_flash = true;

	if (g_core_fp.fp_sense_off(ts, check_flash) == false) {
		ret_data = false;
		E("hx83102_sense_off Fail!");
		return ret_data;
	}

	for (i = 0; i < 5; i++) {
		WORD_REG(tmp_addr, 0x900000D0);
		ret = g_core_fp.fp_reg_read(ts, &tmp_addr, &tmp_data);
		if (ret != 0) {
			ret_data = false;
			E("read ic id Fail");
			return ret_data;
		}

		if (((*tmp_data.data.word) & 0x83102900) == 0x83102900) {
			strscpy(ts->chip_name,
				HX83102J_ID, 30);
			(ts->ic_data)->ic_adc_num =
				hx83102j_data_adc_num;
			ts->ic_data->flash_size = HX83102J_FLASH_SIZE;
			ts->ic_data->icid = *tmp_data.data.word;
			I("detect IC HX83102J successfully");
			ret_data = true;
			break;
		}
		E("Read driver IC ID = %X,%X,%X",
		  tmp_data_array[3],
		  tmp_data_array[2], tmp_data_array[1]); /*83,10,2X*/
		ret_data = false;
		E("Read driver ID register Fail!");
		E("Could NOT find Himax Chipset");
		E("Please check:\n1.VCCD,VCCA,VSP,VSN");
		E("2. LCM_RST,TP_RST");
		E("3. Power On Sequence");
	}

	return ret_data;
}

static void hx83102j_sense_on(struct himax_ts_data *ts, u8 flash_mode)
{
	int ret = 0;

	DEF_WORD_DATA(tmp_data);

	I("Enter");
	ts->notouch_frame = ts->ic_notouch_frame;
	g_core_fp.fp_interface_on(ts);
	g_core_fp.fp_register_write(ts, FW_GET_ARRAY(addr_ctrl_fw_isr),
		FW_GET_ARRAY(data_clear), FW_GET_SZ(data_clear));
	usleep_range(10000, 11000);
	if (!flash_mode) {
		g_core_fp.fp_ic_reset(ts, false, false);
	} else {
		tmp_data.data.half[0] = 0;
		ret = himax_bus_write(ts, IC_GET_ARRAY(adr_i2c_psw_lb)[0], NULL,
				      tmp_data.data.byte, 2);
		if (ret < 0) {
			E("cmd=%x bus access fail!",
			  IC_GET_ARRAY(adr_i2c_psw_lb)[0]);
		}
	}
	if (!ts->ic_data->has_flash) {
		if (g_core_fp._en_hw_crc)
			g_core_fp._en_hw_crc(ts, 1);
		hx83102j_reload_to_active(ts);
	}
}

static bool hx83102j_sense_off(struct himax_ts_data *ts, bool check_en)
{
	u32 cnt = 0;
	struct hx_reg_t tmp_addr = {0};
	int ret = 0;

	DEF_WORD_DATA(tmp_data);

	do {
		if (cnt == 0 ||
		    (tmp_data.data.byte[0] != 0xA5 &&
		    tmp_data.data.byte[0] != 0x00 &&
		    tmp_data.data.byte[0] != 0x87))
			g_core_fp.fp_register_write(ts, FW_GET_ARRAY(addr_ctrl_fw_isr),
				FW_GET_ARRAY(data_fw_stop), FW_GET_SZ(data_fw_stop));
		usleep_range(10000, 10001);

		/* check fw status */
		g_core_fp.fp_register_read(ts, IC_GET_ARRAY(addr_cs_central_state),
			tmp_data.data.byte, tmp_data.len);

		if (tmp_data.data.byte[0] != 0x05) {
			I("Do not need wait FW, Status = 0x%02X!", tmp_data.data.byte[0]);
			break;
		}

		g_core_fp.fp_register_read(ts, FW_GET_ARRAY(addr_ctrl_fw_isr),
			tmp_data.data.byte, tmp_data.len);
		I("cnt = %d, data[0] = 0x%02X!", cnt, tmp_data.data.byte[0]);
	} while (tmp_data.data.byte[0] != 0x87 && ++cnt < 35 && check_en);

	cnt = 0;

	do {
		/**
		 * set Enter safe mode : 0x31 ==> 0x9527
		 */
		tmp_data.data.half[0] = 0x9527;
		ret = himax_bus_write(ts, 0x31, NULL, tmp_data.data.byte, 2);
		if (ret < 0) {
			E("bus access fail!");
			return false;
		}

		/**
		 *Check enter_save_mode
		 */
		WORD_REG(tmp_addr, 0x900000A8);
		g_core_fp.fp_reg_read(ts, &tmp_addr, &tmp_data);
		I("Check enter_save_mode data[0]=%X", tmp_data.data.byte[0]);

		if (tmp_data.data.byte[0] == 0x0C) {
			/**
			 *Reset TCON
			 */
			WORD_REG(tmp_addr, 0x80020004);
			VAL_SET(tmp_data, 0x00000000);
			g_core_fp.fp_reg_write(ts, &tmp_addr, &tmp_data);
			usleep_range(1000, 1001);
			return true;
		}
		usleep_range(5000, 5001);
		g_core_fp.fp_pin_reset(ts);
	} while (cnt++ < 5);

	return false;
}

static bool hx83102j_read_event_stack(struct himax_ts_data *ts,
				      u8 *buf, u32 length)
{
	int ret = 0;

	ret = himax_bus_read(ts, FW_GET_ARRAY(addr_event_addr)[0], buf, length);

	return (ret == NO_ERR) ? true : false;
}

static void hx83102j_chip_init(struct himax_ts_data *ts)
{
	ts->chip_cell_type = CHIP_IS_IN_CELL;
	ts->chip_max_dsram_size = 73728;
	I("IC cell type = %d", ts->chip_cell_type);
	ts->ic_checksum_type = HX_TP_BIN_CHECKSUM_CRC;
	/*Himax: Set FW and CFG Flash Address*/
	WORD_REG(g_core_regs.flash_ver_info.addr_fw_ver_major, 59397);	/*0x00E805*/
	WORD_REG(g_core_regs.flash_ver_info.addr_fw_ver_minor, 59398);	/*0x00E806*/
	WORD_REG(g_core_regs.flash_ver_info.addr_cfg_ver_major, 59648);	/*0x00E900*/
	WORD_REG(g_core_regs.flash_ver_info.addr_cfg_ver_minor, 59649);	/*0x00E901*/
	WORD_REG(g_core_regs.flash_ver_info.addr_cid_ver_major, 59394);	/*0x00E802*/
	WORD_REG(g_core_regs.flash_ver_info.addr_cid_ver_minor, 59395);	/*0x00E803*/
	WORD_REG(g_core_regs.flash_ver_info.addr_cfg_table, 0x10000);
	g_core_regs.flash_ver_info.addr_cfg_table_t.data.word =
		g_core_regs.flash_ver_info.addr_cfg_table.data.word;
	ts->ic_data->enc16bits = false;
}

static void himax_reg_overlay(struct himax_ts_data *ts)
{
	I("Entering!");
	ts->ic_notouch_frame = hx83102j_notouch_frame;
	WORD_REG(g_core_regs.fw_op.addr_raw_out_sel, hx83102j_fw_addr_raw_out_sel);
	WORD_REG(g_core_regs.driver_op.data_df_rx, hx83102j_data_df_rx);
	WORD_REG(g_core_regs.driver_op.data_df_tx, hx83102j_data_df_tx);
	WORD_REG(g_core_regs.ic_op.addr_tcon_on_rst, hx83102j_ic_adr_tcon_rst);
}

static void hx83102j_pin_reset(struct himax_ts_data *ts)
{
	I("Now reset the Touch chip.");
	himax_rst_gpio_set(ts->rst_gpio, 0);
	usleep_range(100 * 100, 101 * 100);
	himax_rst_gpio_set(ts->rst_gpio, 1);
	usleep_range(200 * 100, 201 * 100);
}

static void himax_fp_overlay(void)
{
	I("Entering!");
	g_core_fp.fp_chip_detect = hx83102j_chip_detect;
	g_core_fp.fp_chip_init = hx83102j_chip_init;
	g_core_fp.fp_sense_on = hx83102j_sense_on;
	g_core_fp.fp_sense_off = hx83102j_sense_off;
	g_core_fp.fp_read_event_stack = hx83102j_read_event_stack;
	g_core_fp.fp_pin_reset = hx83102j_pin_reset;
	g_core_fp.fp_resume_ic_action = hx83102j_resume_ic_action;
	g_core_fp.fp_0f_reload_to_active = hx83102j_reload_to_active;
	g_core_fp._en_hw_crc = hx83102j_en_hw_crc;
}

static int __init himax_ic_init(void)
{
	int ret = 0;
	// set default regs
	himax_ic_reg_init(&g_core_regs);
	himax_ic_fp_init();
	// basic struct init
	g_himax_ts = kzalloc(sizeof(*g_himax_ts), GFP_KERNEL);
	if (!g_himax_ts) {
		E("allocate himax_ts_data failed");
		ret = -ENOMEM;
		goto err_alloc_data_failed;
	}
	INIT_LIST_HEAD(&g_himax_ts->list);
	// init ic specific params
	himax_reg_overlay(g_himax_ts);
	himax_fp_overlay();

	// add to ic module list
	ret = himax_spi_drv_init(g_himax_ts);
	if (ret) {
		E("himax_spi_drv_init failed");
		kfree(g_himax_ts);
		g_himax_ts = NULL;
	}

err_alloc_data_failed:
	return ret;
}

static void __exit himax_ic_exit(void)
{
	himax_spi_drv_exit();
}

#if defined(__HIMAX_MOD__)
module_init(himax_ic_init);
#else
late_initcall(himax_ic_init);
#endif
module_exit(himax_ic_exit);

MODULE_DESCRIPTION("Himax SPI driver for HID simulator for " HX83102J_ID);
MODULE_AUTHOR("Himax, Inc.");
MODULE_LICENSE("GPL");
MODULE_VERSION(HIMAX_DRIVER_VER);
