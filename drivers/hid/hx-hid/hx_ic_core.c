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

/* CORE_IC */
/* IC side start*/
static void himax_mcu_burst_enable(struct himax_ts_data *ts,
				   u8 auto_add_4_byte)
{
	u8 tmp_data[DATA_LEN_4];
	int ret;

	/*I("Entering");*/
	tmp_data[0] = IC_GET_VAL(data_conti);

	ret = himax_bus_write(ts, IC_GET_VAL(addr_conti), NULL, tmp_data, 1);
	if (ret < 0) {
		E("bus access fail!");
		return;
	}

	tmp_data[0] = (IC_GET_VAL(data_incr4) | auto_add_4_byte);

	ret = himax_bus_write(ts, IC_GET_VAL(addr_incr4), NULL, tmp_data, 1);
	if (ret < 0) {
		E("bus access fail!");
		return;
	}
}

static int himax_mcu_register_read(struct himax_ts_data *ts, u8 *addr,
				   u8 *buf, u32 len)
{
	int ret = -1;

	mutex_lock(&ts->reg_lock);

	if (memcmp(addr, FLASH_GET_ARRAY(addr_spi200_data), 4) == 0)
		g_core_fp.fp_burst_enable(ts, 0);
	else if (len > DATA_LEN_4)
		g_core_fp.fp_burst_enable(ts, 1);
	else
		g_core_fp.fp_burst_enable(ts, 0);

	ret = himax_bus_write(ts, IC_GET_VAL(addr_ahb_addr_byte_0), addr, NULL, 4);
	if (ret < 0) {
		E("bus access fail!");
		mutex_unlock(&ts->reg_lock);
		return BUS_FAIL;
	}

	ret = himax_bus_write(ts, IC_GET_VAL(addr_ahb_access_direction), NULL,
			      IC_GET_ARRAY(data_ahb_access_direction_read), 1);
	if (ret < 0) {
		E("bus access fail!");
		mutex_unlock(&ts->reg_lock);
		return BUS_FAIL;
	}

	ret = himax_bus_read(ts, IC_GET_VAL(addr_ahb_rdata_byte_0), buf, len);
	if (ret < 0) {
		E("bus access fail!");
		mutex_unlock(&ts->reg_lock);
		return BUS_FAIL;
	}

	mutex_unlock(&ts->reg_lock);

	return NO_ERR;
}

static int himax_mcu_reg_read(struct himax_ts_data *ts, struct hx_reg_t *addr,
			      struct data_pack_t *data)
{
	return g_core_fp.fp_register_read(ts,
		addr->data.byte, data->data.byte, data->len);
}

static int himax_mcu_register_write(struct himax_ts_data *ts, u8 *addr,
				    u8 *val, u32 len)
{
	int ret = -1;
	const u32 max_trans_sz = 4 * 1024;
	int i = 0;
	u32 orig_addr = 0;
	union hx_dword_data_t temp_addr = {0};
	u32 temp_len = 0;

	mutex_lock(&ts->reg_lock);

	if (memcmp(addr, FLASH_GET_ARRAY(addr_spi200_data), 4) == 0)
		g_core_fp.fp_burst_enable(ts, 0);
	else if (len > DATA_LEN_4)
		g_core_fp.fp_burst_enable(ts, 1);
	else
		g_core_fp.fp_burst_enable(ts, 0);

	if (len > max_trans_sz) {
		orig_addr = le32_to_cpup((__le32 *)addr);
		for (i = 0; i < len; i += max_trans_sz) {
			if ((len - i) > max_trans_sz)
				temp_len = max_trans_sz;
			else
				temp_len = len % max_trans_sz;
			temp_addr.dword = cpu_to_le32(orig_addr + i);
			ret = himax_bus_write(ts, IC_GET_VAL(addr_ahb_addr_byte_0),
					      temp_addr.byte, val + i, temp_len + ADDR_LEN_4);
			if (ret < 0) {
				E("xfer fail!");
				mutex_unlock(&ts->reg_lock);
				return BUS_FAIL;
			}
		}
	} else {
		ret = himax_bus_write(ts, IC_GET_VAL(addr_ahb_addr_byte_0), addr, val,
				      len + ADDR_LEN_4);
		if (ret < 0) {
			E("xfer fail!");
			mutex_unlock(&ts->reg_lock);
			return BUS_FAIL;
		}
	}

	mutex_unlock(&ts->reg_lock);

	return NO_ERR;
}

static int himax_mcu_reg_write(struct himax_ts_data *ts,
			       struct hx_reg_t *addr, struct data_pack_t *data)
{
	return g_core_fp.fp_register_write(ts,
		addr->data.byte, data->data.byte, data->len);
}

static int himax_write_read_reg(struct himax_ts_data *ts, u8 *tmp_addr,
				u8 *tmp_data, u8 hb, u8 lb)
{
	u16 retry = 0;
	union hx_dword_data_t r_data = {0};
	union hx_dword_data_t t_addr = {0};
	union hx_dword_data_t t_data = {0};

	memcpy(t_addr.byte, tmp_addr, ADDR_LEN_4);
	memcpy(t_data.byte, tmp_data, DATA_LEN_4);

	while (retry++ < 40) { /* ceil[16.6*2] */
		g_core_fp.fp_register_read(ts, t_addr.byte, r_data.byte, DATA_LEN_4);
		D("Read handshake = 0x%08X:0x%08X", le32_to_cpu(t_addr.dword),
		  le32_to_cpu(r_data.dword));
		if (r_data.byte[1] == lb && r_data.byte[0] == hb)
			break;
		else if (r_data.byte[1] == hb && r_data.byte[0] == lb)
			return NO_ERR;

		g_core_fp.fp_register_write(ts, t_addr.byte, t_data.byte, DATA_LEN_4);
		D("Write handshake = 0x%08X:0x%08X", le32_to_cpu(t_addr.dword),
		  le32_to_cpu(t_data.dword));
		usleep_range(1000, 1100);
	}

	if (retry >= 40)
		goto FAIL;

	retry = 0;
	while (retry++ < 200) { /* self test item might take long time */
		g_core_fp.fp_register_read(ts, t_addr.byte, r_data.byte, DATA_LEN_4);
		if (r_data.byte[1] == hb && r_data.byte[0] == lb)
			return NO_ERR;

		D("wait data ready %d times, handshake = 0x%08X:0x%08X", retry,
		  le32_to_cpu(t_addr.dword), le32_to_cpu(r_data.dword));
		usleep_range(10000, 10100);
	}

FAIL:
	E("failed to handshaking with DSRAM");
	E("addr = 0x%02X%02X%02X%02X; data = 0x%02X%02X%02X%02X",
	  tmp_addr[3], tmp_addr[2], tmp_addr[1], tmp_addr[0],
	  tmp_data[3], tmp_data[2], tmp_data[1], tmp_data[0]);
	E("target = %02X%02X; r_data = %02X%02X",
	  hb, lb, r_data.byte[1], r_data.byte[0]);

	return HX_RW_REG_FAIL;
}

static void himax_mcu_interface_on(struct himax_ts_data *ts)
{
	u8 tmp_data[DATA_LEN_4];
	u8 tmp_data2[DATA_LEN_4];
	int cnt = 0;
	int ret = 0;

	/* Read a dummy register to wake up BUS.*/
	ret = himax_bus_read(ts, IC_GET_VAL(addr_ahb_rdata_byte_0), tmp_data,
			     DATA_LEN_4);
	if (ret < 0) {/* to knock BUS*/
		E("bus access fail!");
		return;
	}

	do {
		tmp_data[0] = IC_GET_VAL(data_conti);

		ret = himax_bus_write(ts, IC_GET_VAL(addr_conti), NULL, tmp_data, 1);
		if (ret < 0) {
			E("bus access fail!");
			return;
		}

		tmp_data[0] = IC_GET_VAL(data_incr4);

		ret = himax_bus_write(ts, IC_GET_VAL(addr_incr4), NULL, tmp_data, 1);
		if (ret < 0) {
			E("bus access fail!");
			return;
		}

		/*Check cmd*/
		himax_bus_read(ts, IC_GET_VAL(addr_conti), tmp_data, 1);
		himax_bus_read(ts, IC_GET_VAL(addr_incr4), tmp_data2, 1);

		if (tmp_data[0] == IC_GET_VAL(data_conti) &&
		    tmp_data2[0] == IC_GET_VAL(data_incr4))
			break;

		usleep_range(1000, 1100);
	} while (++cnt < 10);

	if (cnt > 0)
		I("Polling burst mode: %d times", cnt);
}

static void himax_mcu_sense_on(struct himax_ts_data *ts, u8 flash_mode)
{
	u8 tmp_data[DATA_LEN_4];
	int retry = 0;
	int ret = 0;

	I("Enter");
	ts->notouch_frame = ts->ic_notouch_frame;
	g_core_fp.fp_interface_on(ts);
	g_core_fp.fp_register_write(ts, FW_GET_ARRAY(addr_ctrl_fw_isr),
		FW_GET_ARRAY(data_clear), FW_GET_SZ(data_clear));
	usleep_range(10000, 11000);
	if (!flash_mode) {
		g_core_fp.fp_ic_reset(ts, false, false);
	} else {
		do {
			g_core_fp.fp_register_read(ts,
				FW_GET_ARRAY(addr_flag_reset_event),
				tmp_data, DATA_LEN_4);
			I("Read status from IC = %X,%X", tmp_data[0], tmp_data[1]);
		} while ((tmp_data[1] != 0x01 ||
			tmp_data[0] != 0x00) &&
			retry++ < 5);

		if (retry >= 5) {
			E("Fail");
			g_core_fp.fp_ic_reset(ts, false, false);
		} else {
			I("OK and Read status from IC = %X,%X",
			  tmp_data[0], tmp_data[1]);
			/* reset code*/
			tmp_data[0] = 0x00;

			ret = himax_bus_write(ts, IC_GET_VAL(adr_i2c_psw_lb), NULL,
					      tmp_data, 1);
			if (ret < 0) {
				E("cmd=%x bus access fail!",
				  IC_GET_VAL(adr_i2c_psw_lb));
			}
			ret = himax_bus_write(ts, IC_GET_VAL(adr_i2c_psw_ub), NULL,
					      tmp_data, 1);
			if (ret < 0) {
				E("cmd=%x bus access fail!",
				  IC_GET_VAL(adr_i2c_psw_ub));
			}
		}
	}
}

static bool himax_mcu_sense_off(struct himax_ts_data *ts, bool check_en)
{
	u8 cnt = 0;
	u8 tmp_data[DATA_LEN_4];
	int ret = 0;

	do {
		tmp_data[0] = IC_GET_VAL(data_i2c_psw_lb);
		tmp_data[1] = IC_GET_VAL(data_i2c_psw_ub);

		ret = himax_bus_write(ts, IC_GET_VAL(adr_i2c_psw_lb), NULL, tmp_data,
				      2);
		if (ret < 0) {
			E("bus access fail!");
			return false;
		}

		g_core_fp.fp_register_read(ts, IC_GET_ARRAY(addr_cs_central_state),
			tmp_data, ADDR_LEN_4);
		I("Check enter_save_mode data[0]=%X", tmp_data[0]);

		if (tmp_data[0] == 0x0C) {
			g_core_fp.fp_register_write(ts, IC_GET_ARRAY(addr_tcon_on_rst),
				IC_GET_ARRAY(data_rst), DATA_LEN_4);
			usleep_range(1000, 1100);

			g_core_fp.fp_register_write(ts, IC_GET_ARRAY(addr_adc_on_rst),
				IC_GET_ARRAY(data_rst), DATA_LEN_4);
			usleep_range(1000, 1100);
			tmp_data[3] = IC_GET_ARRAY(data_rst)[3];
			tmp_data[2] = IC_GET_ARRAY(data_rst)[2];
			tmp_data[1] = IC_GET_ARRAY(data_rst)[1];
			tmp_data[0] = IC_GET_ARRAY(data_rst)[0] | 0x01;
			g_core_fp.fp_register_write(ts, IC_GET_ARRAY(addr_adc_on_rst),
				tmp_data, DATA_LEN_4);
			goto TRUE_END;
		} else {
			/* usleep_range(10000, 10001); */
			g_core_fp.fp_ic_reset(ts, false, false);
		}
	} while (cnt++ < 15);

	return false;
TRUE_END:
	return true;
}

#define WIP_PRT_LOG "retry:%d, bf[0]=%d, bf[1]=%d,bf[2]=%d, bf[3]=%d"
static bool himax_mcu_wait_wip(struct himax_ts_data *ts, int timing)
{
	u8 tmp_data[DATA_LEN_4];
	int retry_cnt = 0;

	g_core_fp.fp_register_write(ts, FLASH_GET_ARRAY(addr_spi200_trans_fmt),
		FLASH_GET_ARRAY(data_spi200_trans_fmt), DATA_LEN_4);
	tmp_data[0] = 0x01;

	do {
		g_core_fp.fp_register_write(ts,
			FLASH_GET_ARRAY(addr_spi200_trans_ctrl),
			FLASH_GET_ARRAY(data_spi200_trans_ctrl_1), DATA_LEN_4);

		g_core_fp.fp_register_write(ts, FLASH_GET_ARRAY(addr_spi200_cmd),
			FLASH_GET_ARRAY(data_spi200_cmd_1), DATA_LEN_4);
		memset(tmp_data, 0xFF, DATA_LEN_4);
		g_core_fp.fp_register_read(ts, FLASH_GET_ARRAY(addr_spi200_data),
			tmp_data, 4);

		if ((tmp_data[0] & 0x01) == 0x00)
			return true;

		retry_cnt++;

		if (tmp_data[0] != 0x00 ||
		    tmp_data[1] != 0x00 ||
		    tmp_data[2] != 0x00 ||
		    tmp_data[3] != 0x00)
			I(WIP_PRT_LOG,
			  retry_cnt, tmp_data[0],
			  tmp_data[1], tmp_data[2], tmp_data[3]);

		if (retry_cnt > 100) {
			E("Wait wip error!");
			return false;
		}

		usleep_range(timing * 1000, timing * 1000 + 1);
	} while ((tmp_data[0] & 0x01) == 0x01);

	return true;
}

/*power saving level*/
static void himax_mcu_init_psl(struct himax_ts_data *ts)
{
	g_core_fp.fp_register_write(ts, IC_GET_ARRAY(addr_psl),
		IC_GET_ARRAY(data_rst), IC_GET_SZ(data_rst));
	I("power saving level reset OK!");
}

static void himax_mcu_resume_ic_action(struct himax_ts_data *ts)
{
	/* Nothing to do */
}

static void himax_mcu_suspend_ic_action(struct himax_ts_data *ts)
{
	/* Nothing to do */
}

static void himax_mcu_power_on_init(struct himax_ts_data *ts)
{
	union hx_dword_data_t tmp_data = { .byte = {0x01, 0x00, 0x00, 0x00} };
	u8 retry = 0;

	/*RawOut select initial*/
	g_core_fp.fp_register_write(ts, FW_GET_ARRAY(addr_raw_out_sel),
		FW_GET_ARRAY(data_clear), FW_GET_SZ(data_clear));
	/*DSRAM func initial*/
	g_core_fp.fp_assign_sorting_mode(ts, FW_GET_ARRAY(data_clear));
	/*N frame initial*/
	/* reset N frame back to default value 1 for normal mode */
	g_core_fp.fp_register_write(ts, FW_GET_ARRAY(addr_set_frame_addr),
		tmp_data.byte, 4);
	/*FW reload done initial*/
	g_core_fp.fp_register_write(ts,
		DRV_GET_ARRAY(addr_fw_define_2nd_flash_reload),
		FW_GET_ARRAY(data_clear), FW_GET_SZ(data_clear));

	g_core_fp.fp_sense_on(ts, 0x00);

	I("waiting for FW reload data");

	while (retry++ < 30) {
		g_core_fp.fp_register_read(ts,
			DRV_GET_ARRAY(addr_fw_define_2nd_flash_reload), tmp_data.byte,
			DATA_LEN_4);

		/* use all 4 bytes to compare */
		if (le32_to_cpu(tmp_data.dword) == 0x72C0) {
			I("FW reload done");
			break;
		}
		I("wait FW reload %d times", retry);
		g_core_fp.fp_read_FW_status(ts);
		usleep_range(10000, 11000);
	}
}

/* IC side end*/
/* CORE_IC */

/* CORE_FW */
/* FW side start*/
static void himax_mcu_system_reset(struct himax_ts_data *ts)
{
	int ret = 0;
	u8 tmp_data[DATA_LEN_4];
	int retry = 0;

	g_core_fp.fp_interface_on(ts);
	g_core_fp.fp_register_write(ts, FW_GET_ARRAY(addr_ctrl_fw_isr),
		FW_GET_ARRAY(data_clear), FW_GET_SZ(data_clear));
	do {
		/* reset code*/
		/**
		 * I2C_password[7:0] set Enter safe mode : 0x31 ==> 0x27
		 */
		tmp_data[0] = IC_GET_VAL(data_i2c_psw_lb);

		ret = himax_bus_write(ts, IC_GET_VAL(adr_i2c_psw_lb), NULL, tmp_data,
				      1);
		if (ret < 0)
			E("bus access fail!");

		/**
		 * I2C_password[15:8] set Enter safe mode :0x32 ==> 0x95
		 */
		tmp_data[0] = IC_GET_VAL(data_i2c_psw_ub);

		ret = himax_bus_write(ts, IC_GET_VAL(adr_i2c_psw_ub), NULL, tmp_data,
				      1);
		if (ret < 0)
			E("bus access fail!");

		/**
		 * I2C_password[7:0] set Enter safe mode : 0x31 ==> 0x00
		 */
		tmp_data[0] = 0x00;

		ret = himax_bus_write(ts, IC_GET_VAL(adr_i2c_psw_lb), NULL, tmp_data,
				      1);
		if (ret < 0)
			E("bus access fail!");

		usleep_range(10000, 11000);

		g_core_fp.fp_register_read(ts, FW_GET_ARRAY(addr_flag_reset_event),
			tmp_data, DATA_LEN_4);
		I("Read status from IC = %X,%X", tmp_data[0], tmp_data[1]);
	} while ((tmp_data[1] != 0x02 || tmp_data[0] != 0x00) && retry++ < 5);
}

static int himax_mcu_calculate_crc_with_ap(const unsigned char *FW_content,
					   int CRC_from_FW, int len)
{
	int i, j, length = 0;
	int fw_data;
	int fw_data_2;
	int CRC = 0xFFFFFFFF;
	int polynomial = 0x82F63B78;

	length = len / 4;

	for (i = 0; i < length; i++) {
		fw_data = FW_content[i * 4];

		for (j = 1; j < 4; j++) {
			fw_data_2 = FW_content[i * 4 + j];
			fw_data += (fw_data_2) << (8 * j);
		}
		CRC = fw_data ^ CRC;
		for (j = 0; j < 32; j++) {
			if ((CRC % 2) != 0)
				CRC = ((CRC >> 1) & 0x7FFFFFFF) ^ polynomial;
			else
				CRC = (((CRC >> 1) & 0x7FFFFFFF));
		}
	}

	return CRC;
}

static u32 himax_mcu_check_crc(struct himax_ts_data *ts,
			       u8 *start_addr, int reload_length)
{
	u32 result = 0;
	union {
		u8 byte[4];
		u16 word[2];
		u32 dword;
	} tmp_data = {0};
	int cnt = 0, ret = 0;
	int length = reload_length / DATA_LEN_4;

	ret = g_core_fp.fp_register_write(ts, FW_GET_ARRAY(addr_reload_addr_from),
		start_addr, DATA_LEN_4);
	if (ret < NO_ERR) {
		E("bus access fail!");
		return HW_CRC_FAIL;
	}

	tmp_data.word[1] = 0x0099;
	tmp_data.word[1] = cpu_to_le16(tmp_data.word[1]);
	tmp_data.word[0] = length;
	tmp_data.word[0] = cpu_to_le16(tmp_data.word[0]);
	ret = g_core_fp.fp_register_write(ts,
		FW_GET_ARRAY(addr_reload_addr_cmd_beat), tmp_data.byte, DATA_LEN_4);
	if (ret < NO_ERR) {
		E("bus access fail!");
		return HW_CRC_FAIL;
	}
	tmp_data.word[0] = 0x0000;

	ret = g_core_fp.fp_register_read(ts,
		FW_GET_ARRAY(addr_reload_addr_cmd_beat), tmp_data.byte, DATA_LEN_4);

	if (le16_to_cpu(tmp_data.word[0]) != length) {
		E("Flash content is Wrong");
		return HW_CRC_FAIL;
	}

	cnt = 0;

	do {
		ret = g_core_fp.fp_register_read(ts, FW_GET_ARRAY(addr_reload_status),
			tmp_data.byte, DATA_LEN_4);
		if (ret < NO_ERR) {
			E("bus access fail!");
			return HW_CRC_FAIL;
		}

		if ((tmp_data.byte[0] & 0x01) != 0x01) {
			ret = g_core_fp.fp_register_read(ts,
				FW_GET_ARRAY(addr_reload_crc32_result), tmp_data.byte,
				DATA_LEN_4);
			if (ret < NO_ERR) {
				E("bus access fail!");
				return HW_CRC_FAIL;
			}
			I("data[3]=%X,data[2]=%X,data[1]=%X,data[0]=%X",
			  tmp_data.byte[3],
			  tmp_data.byte[2],
			  tmp_data.byte[1],
			  tmp_data.byte[0]);
			result = tmp_data.dword;
			goto END;
		} else {
			I("Waiting for HW ready!");
			usleep_range(1000, 1100);
			if (cnt >= 100)
				g_core_fp.fp_read_FW_status(ts);
		}

	} while (cnt++ < 100);
END:
	return result;
}

static void himax_mcu_set_reload_cmd(u8 *write_data, int idx,
				     u32 cmd_from, u32 cmd_to, u32 cmd_beat)
{
	int index = idx * 12;
	int i;

	for (i = 3; i >= 0; i--) {
		write_data[index + i] = (cmd_from >> (8 * i));
		write_data[index + 4 + i] = (cmd_to >> (8 * i));
		write_data[index + 8 + i] = (cmd_beat >> (8 * i));
	}
}

static bool himax_mcu_program_reload(void)
{
	return true;
}

static void himax_mcu_usb_detect_set(struct himax_ts_data *ts,
				     const u8 *cable_config)
{
	struct hx_reg_t tmp_reg;
	struct hx_reg_t back_reg;
	u8 retry_cnt = 0;

	do {
		if (cable_config[1] == 0x01) {
			WORD_REG(tmp_reg, fw_func_handshaking_pwd);
			g_core_fp.fp_register_write(ts, FW_GET_ARRAY(addr_usb_detect),
				REG_GET_ARRAY(tmp_reg), REG_GET_SZ(tmp_reg));
			WORD_REG(back_reg, fw_func_handshaking_pwd);
			I("USB detect status IN!");
		} else {
			WORD_REG(tmp_reg, fw_data_safe_mode_release_pw_reset);
			g_core_fp.fp_register_write(ts, FW_GET_ARRAY(addr_usb_detect),
				REG_GET_ARRAY(tmp_reg), REG_GET_SZ(tmp_reg));
			WORD_REG(back_reg, fw_data_safe_mode_release_pw_reset);
			I("USB detect status OUT!");
		}

		g_core_fp.fp_register_read(ts, FW_GET_ARRAY(addr_usb_detect),
			REG_GET_ARRAY(tmp_reg), REG_GET_SZ(tmp_reg));
		/*I("tmp_data[0]=%d, USB detect=%d, retry_cnt=%d",
		 *	tmp_data[0], cable_config[1], retry_cnt);
		 */
		retry_cnt++;
	} while ((REG_GET_ARRAY(tmp_reg)[3] != REG_GET_ARRAY(back_reg)[3] ||
		REG_GET_ARRAY(tmp_reg)[2] != REG_GET_ARRAY(back_reg)[2] ||
		REG_GET_ARRAY(tmp_reg)[1] != REG_GET_ARRAY(back_reg)[1] ||
		REG_GET_ARRAY(tmp_reg)[0] != REG_GET_ARRAY(back_reg)[0]) &&
		retry_cnt < HIMAX_REG_RETRY_TIMES);
}

#define PRT_DATA "[3]=0x%02X, [2]=0x%02X, [1]=0x%02X, [0]=0x%02X"
static void himax_mcu_diag_register_set(struct himax_ts_data *ts,
					u8 diag_command)
{
	u8 tmp_data[DATA_LEN_4];
	u8 back_data[DATA_LEN_4];
	u8 cnt = 50;

	tmp_data[0] = diag_command;
	I("diag_command = %d, tmp_data[0] = %X", diag_command, tmp_data[0]);
	g_core_fp.fp_interface_on(ts);
	tmp_data[3] = 0x00; tmp_data[2] = 0x00; tmp_data[1] = 0x00;
	do {
		g_core_fp.fp_register_write(ts, FW_GET_ARRAY(addr_raw_out_sel),
			tmp_data, DATA_LEN_4);
		g_core_fp.fp_register_read(ts, FW_GET_ARRAY(addr_raw_out_sel), back_data,
			DATA_LEN_4);
		I(PRT_DATA, back_data[3], back_data[2], back_data[1], back_data[0]);
		cnt--;
	} while (tmp_data[0] != back_data[0] && cnt > 0);
}

static int himax_mcu_diag_register_get(struct himax_ts_data *ts,
				       u32 *diag_value)
{
	int ret = 0;
	union hx_dword_data_t *tmp_data = (union hx_dword_data_t *)diag_value;

	if (tmp_data) {
		ret = g_core_fp.fp_register_read(ts, FW_GET_ARRAY(addr_raw_out_sel),
			tmp_data->byte, DATA_LEN_4);
	} else {
		E("diag_value is NULL");
		ret = -EINVAL;
	}

	return ret;
}

#define PRT_TMP_DATA "[0]=0x%2X,[1]=0x%2X,	[2]=0x%2X,[3]=0x%2X"
static void himax_mcu_idle_mode(struct himax_ts_data *ts, int disable)
{
	int retry = 20;
	u8 tmp_data[DATA_LEN_4];
	u8 switch_cmd = 0x00;

	I("entering");

	do {
		I("now %d times!", retry);
		g_core_fp.fp_register_read(ts, FW_GET_ARRAY(addr_fw_mode_status),
			tmp_data, DATA_LEN_4);

		if (disable)
			switch_cmd = FW_GET_ARRAY(data_idle_dis_pwd)[0];
		else
			switch_cmd = FW_GET_ARRAY(data_idle_en_pwd)[0];

		tmp_data[0] = switch_cmd;
		g_core_fp.fp_register_write(ts, FW_GET_ARRAY(addr_fw_mode_status),
			tmp_data, DATA_LEN_4);
		g_core_fp.fp_register_read(ts, FW_GET_ARRAY(addr_fw_mode_status),
			tmp_data, DATA_LEN_4);

		I(PRT_TMP_DATA,	tmp_data[0], tmp_data[1], tmp_data[2], tmp_data[3]);

		retry--;
		usleep_range(10000, 11000);
	} while ((tmp_data[0] != switch_cmd) && retry > 0);

	I("setting OK!");
}

static void himax_mcu_reload_disable(struct himax_ts_data *ts, int disable)
{
	I("entering");

	if (disable) { /*reload disable*/
		g_core_fp.fp_register_write(ts,
			DRV_GET_ARRAY(addr_fw_define_flash_reload),
			DRV_GET_ARRAY(data_fw_define_flash_reload_dis),
			DATA_LEN_4);
	} else { /*reload enable*/
		g_core_fp.fp_register_write(ts,
			DRV_GET_ARRAY(addr_fw_define_flash_reload),
			DRV_GET_ARRAY(data_fw_define_flash_reload_en),
			DATA_LEN_4);
	}

	I("setting OK!");
}

static int himax_mcu_read_ic_trigger_type(struct himax_ts_data *ts)
{
	u8 tmp_data[DATA_LEN_4];
	int trigger_type = false;

	g_core_fp.fp_register_read(ts, DRV_GET_ARRAY(addr_fw_define_int_is_edge),
		tmp_data, DATA_LEN_4);

	if ((tmp_data[1] & 0x01) == 1)
		trigger_type = true;

	return trigger_type;
}

/* Please call this function after FW finish reload done */
static void himax_mcu_read_FW_ver(struct himax_ts_data *ts)
{
	u8 data[12] = {0};

	g_core_fp.fp_register_read(ts, FW_GET_ARRAY(addr_fw_ver_addr), data,
		DATA_LEN_4);
	ts->ic_data->vendor_panel_ver =  data[0];
	ts->ic_data->vendor_fw_ver = data[1] << 8 | data[2];
	I("PANEL_VER : %X", ts->ic_data->vendor_panel_ver);
	I("FW_VER : %X", ts->ic_data->vendor_fw_ver);
	g_core_fp.fp_register_read(ts, FW_GET_ARRAY(addr_fw_cfg_addr), data,
		DATA_LEN_4);
	ts->ic_data->vendor_config_ver = data[2] << 8 | data[3];
	/*I("CFG_VER : %X",ts->ic_data->vendor_config_ver);*/
	ts->ic_data->vendor_touch_cfg_ver = data[2];
	I("TOUCH_VER : %X", ts->ic_data->vendor_touch_cfg_ver);
	ts->ic_data->vendor_display_cfg_ver = data[3];
	I("DISPLAY_VER : %X", ts->ic_data->vendor_display_cfg_ver);
	g_core_fp.fp_register_read(ts, FW_GET_ARRAY(addr_fw_vendor_addr), data,
		DATA_LEN_4);
	ts->ic_data->vendor_cid_maj_ver = data[2];
	ts->ic_data->vendor_cid_min_ver = data[3];
	I("CID_VER : %X", (ts->ic_data->vendor_cid_maj_ver << 8
			| ts->ic_data->vendor_cid_min_ver));
	g_core_fp.fp_register_read(ts, FW_GET_ARRAY(addr_cus_info), data, 12);
	memcpy(ts->ic_data->vendor_cus_info, data, 12);
	I("Cusomer ID = %s", ts->ic_data->vendor_cus_info);
	g_core_fp.fp_register_read(ts, FW_GET_ARRAY(addr_proj_info), data, 12);
	memcpy(ts->ic_data->vendor_proj_info, data, 12);
	I("Project ID = %s", ts->ic_data->vendor_proj_info);
	hx_hid_update_info(ts);
}

static bool himax_mcu_read_event_stack(struct himax_ts_data *ts, u8 *buf,
				       u32 length)
{
	u8 cmd[DATA_LEN_4];
	struct time_var t_start, t_end, t_delta;
	int len = length;
	int i2c_speed = 0;
	int ret = 0;

	/*  AHB_I2C Burst Read Off */
	cmd[0] = FW_GET_VAL(data_ahb_dis);

	ret = himax_bus_write(ts, FW_GET_VAL(addr_ahb_addr), NULL, cmd, 1);
	if (ret < 0) {
		E("bus access fail!");
		return 0;
	}
	if (ts->debug_log_level & BIT(2))
		time_func(&t_start);

	himax_bus_read(ts, FW_GET_VAL(addr_event_addr), buf, length);

	if (ts->debug_log_level & BIT(2)) {
		time_func(&t_end);
		t_delta.tv_nsec = (t_end.tv_sec * 1000000000 + t_end.tv_nsec)
			- (t_start.tv_sec * 1000000000 + t_start.tv_nsec);

		i2c_speed = (len * 9 * 1000000
			/ (int)t_delta.tv_nsec) * 13 / 10;
		ts->bus_speed = (int)i2c_speed;
	}

	/*  AHB_I2C Burst Read On */
	cmd[0] = FW_GET_VAL(data_ahb_en);

	ret = himax_bus_write(ts, FW_GET_VAL(addr_ahb_addr), NULL, cmd, 1);
	if (ret < 0) {
		E("bus access fail!");
		return 0;
	}

	return 1;
}

static void himax_mcu_return_event_stack(struct himax_ts_data *ts)
{
	int retry = 20, i;
	u8 tmp_data[DATA_LEN_4];

	I("entering");

	do {
		I("now %d times!", retry);

		for (i = 0; i < DATA_LEN_4; i++)
			tmp_data[i] = SRAM_GET_ARRAY(addr_rawdata_end)[i];

		g_core_fp.fp_register_write(ts, SRAM_GET_ARRAY(addr_rawdata_addr),
			tmp_data, DATA_LEN_4);
		g_core_fp.fp_register_read(ts, SRAM_GET_ARRAY(addr_rawdata_addr),
			tmp_data, DATA_LEN_4);
		retry--;
		usleep_range(10000, 11000);
	} while ((tmp_data[1] != SRAM_GET_ARRAY(addr_rawdata_end)[1] &&
		tmp_data[0] != SRAM_GET_ARRAY(addr_rawdata_end)[0]) &&
		retry > 0);

	I("End of setting!");
}

static bool himax_mcu_calculate_checksum(struct himax_ts_data *ts,
					 bool change_iref, u32 size)
{
	u32 CRC_result = 0xFFFFFFFF;
	u8 i;
	u8 tmp_data[DATA_LEN_4];

	I("Now size=%d", size);
	for (i = 0; i < DATA_LEN_4; i++)
		tmp_data[i] = SRAM_GET_ARRAY(addr_rawdata_end)[i];

	CRC_result = g_core_fp.fp_check_crc(ts, tmp_data, size);
	usleep_range(50000, 50001);

	if (CRC_result != 0)
		I("CRC Fail=%d", CRC_result);

	return (CRC_result == 0) ? true : false;
}

static u32 dbg_reg_ary[4] = {fw_addr_fw_dbg_msg_addr,
	fw_addr_chk_fw_status, fw_addr_chk_dd_status, fw_addr_flag_reset_event};

static void himax_mcu_read_FW_status(struct himax_ts_data *ts)
{
	u8 len = 0;
	u8 i = 0;
	struct hx_reg_t addr_reg;
	u8 data[4] = {0};

	len = (u8)(sizeof(dbg_reg_ary) / sizeof(u32));

	for (i = 0; i < len; i++) {
		WORD_REG(addr_reg, dbg_reg_ary[i]);
		g_core_fp.fp_register_read(ts, REG_GET_ARRAY(addr_reg),
			data, DATA_LEN_4);

		I("reg[0-3] : 0x%08X = 0x%02X, 0x%02X, 0x%02X, 0x%02X",
		  dbg_reg_ary[i], data[0], data[1], data[2], data[3]);
	}
}

static void himax_mcu_irq_switch(struct himax_ts_data *ts, int switch_on)
{
	if (switch_on) {
		if (ts->use_irq)
			himax_int_enable(ts, switch_on);
		else
			hrtimer_start(&ts->timer, ktime_set(1, 0),
				      HRTIMER_MODE_REL);
	} else {
		if (ts->use_irq) {
			himax_int_enable(ts, switch_on);
		} else {
			hrtimer_cancel(&ts->timer);
			cancel_work_sync(&ts->work);
		}
	}
}

static int himax_mcu_assign_sorting_mode(struct himax_ts_data *ts,
					 u8 *tmp_data_in)
{
	u8 retry = 0;
	union hx_dword_data_t rdata = {0};
	u32 sorting_addr = FW_GET_VAL(addr_sorting_mode_en);
	union hx_dword_data_t *tmp_data = (union hx_dword_data_t *)tmp_data_in;

	I("addr: 0x%08X, write: 0x%08X",
	  sorting_addr, le32_to_cpu(tmp_data->dword));

	while (retry++ < 3) {
		g_core_fp.fp_register_write(ts, FW_GET_ARRAY(addr_sorting_mode_en),
			tmp_data->byte, DATA_LEN_4);
		usleep_range(1000, 1100);
		g_core_fp.fp_register_read(ts, FW_GET_ARRAY(addr_sorting_mode_en),
			rdata.byte, DATA_LEN_4);

		if (rdata.dword == tmp_data->dword) {
			I("success to write sorting mode");
			return NO_ERR;
		}
		E("fail to write sorting mode");
	}

	return BUS_FAIL;
}

static int himax_mcu_check_sorting_mode(struct himax_ts_data *ts,
					u8 *tmp_data_in)
{
	int ret = NO_ERR;
	u32 sorting_addr = FW_GET_VAL(addr_sorting_mode_en);
	union hx_dword_data_t *tmp_data = (union hx_dword_data_t *)tmp_data_in;

	ret = g_core_fp.fp_register_read(ts, FW_GET_ARRAY(addr_sorting_mode_en),
		tmp_data->byte, DATA_LEN_4);
	I("addr: 0x%08X, Now is:0x%08X",
	  sorting_addr, le32_to_cpu(tmp_data->dword));
	if (tmp_data->dword == 0xFFFFFFFF) {
		ret = BUS_FAIL;
		I("All 0xFF, Fail!");
	}

	return ret;
}

static u8 himax_mcu_read_DD_status(struct himax_ts_data *ts,
				   u8 *cmd_set, u8 *tmp_data)
{
	int cnt = 0;
	u8 req_size = cmd_set[0];

	cmd_set[3] = FW_GET_ARRAY(data_dd_request)[0];
	g_core_fp.fp_register_write(ts, FW_GET_ARRAY(addr_dd_handshak_addr),
		cmd_set, DATA_LEN_4);
	I("cmd set[0]=0x%2X,set[1]=0x%2X,set[2]=0x%2X,set[3]=0x%2X",
	  cmd_set[0], cmd_set[1], cmd_set[2], cmd_set[3]);

	/* Doing hand shaking 0xAA -> 0xBB */
	for (cnt = 0; cnt < 100; cnt++) {
		g_core_fp.fp_register_read(ts, FW_GET_ARRAY(addr_dd_handshak_addr),
			tmp_data, DATA_LEN_4);
		usleep_range(10000, 11000);

		if (tmp_data[3] == FW_GET_ARRAY(data_dd_ack)[0]) {
			I("Data ready goto moving data");
			goto FINALIZE;
		} else {
			if (cnt >= 99) {
				I("Data not ready in FW");
				return FW_NOT_READY;
			}
		}
	}
FINALIZE:
	g_core_fp.fp_register_read(ts, FW_GET_ARRAY(addr_dd_data_addr), tmp_data,
		req_size);
	return NO_ERR;
}

static void hx_clr_fw_reord_dd_sts(struct himax_ts_data *ts)
{
	union hx_dword_data_t tmp_data = {0};

	g_core_fp.fp_register_read(ts, IC_GET_ARRAY(addr_cs_central_state),
		tmp_data.byte, ADDR_LEN_4);
	I("Check enter_save_mode data[0]=%02X", tmp_data.byte[0]);

	if (tmp_data.byte[0] == 0x0C) {
		I("Enter safe mode, OK!");
	} else {
		E("It doen't enter safe mode, please check it again");
		return;
	}
	g_core_fp.fp_register_read(ts, FW_GET_ARRAY(addr_clr_fw_record_dd_sts),
		tmp_data.byte, DATA_LEN_4);
	I("Before Write :Now 10007FCC=0x%08X",
	  le32_to_cpu(tmp_data.dword));
	usleep_range(10000, 10001);

	tmp_data.byte[2] = 0x00;
	tmp_data.byte[3] = 0x00;
	g_core_fp.fp_register_write(ts, FW_GET_ARRAY(addr_clr_fw_record_dd_sts),
		tmp_data.byte, DATA_LEN_4);
	usleep_range(10000, 10001);

	g_core_fp.fp_register_read(ts, FW_GET_ARRAY(addr_clr_fw_record_dd_sts),
		tmp_data.byte, DATA_LEN_4);
	I("After Write :Now 10007FCC=0x%08X",
	  le32_to_cpu(tmp_data.dword));
}

static void hx_ap_notify_fw_sus(struct himax_ts_data *ts, int suspend)
{
	int retry = 0;
	int read_sts = 0;
	union hx_dword_data_t read_tmp = {0};
	union hx_dword_data_t addr_tmp = {0};
	union hx_dword_data_t data_tmp = {0};

	addr_tmp.dword = FW_GET_VAL(addr_ap_notify_fw_sus);

	if (suspend) {
		I("Suspend mode!");
		data_tmp.dword = FW_GET_VAL(data_ap_notify_fw_sus_en);
	} else {
		I("Non-Suspend mode!");
		data_tmp.dword = FW_GET_VAL(data_ap_notify_fw_sus_dis);
	}

	I("R%08XH<-0x%08X",	addr_tmp.dword,	data_tmp.dword);
	addr_tmp.dword = cpu_to_le32(addr_tmp.dword);
	data_tmp.dword = cpu_to_le32(data_tmp.dword);
	do {
		g_core_fp.fp_register_write(ts, addr_tmp.byte, data_tmp.byte,
			sizeof(data_tmp.byte));
		usleep_range(1000, 1001);
		read_sts = g_core_fp.fp_register_read(ts, addr_tmp.byte, read_tmp.byte,
			sizeof(read_tmp.byte));
		I("read bus status=%d", read_sts);
		I("Now retry=%d, data=0x%08X", retry,
		  le32_to_cpu(read_tmp.dword));
	} while ((retry++ < 10) && (read_sts != NO_ERR) &&
		(read_tmp.dword != data_tmp.dword));
}

/* FW side end*/
/* CORE_FW */

/* CORE_FLASH */
/* FLASH side start*/
static void himax_mcu_chip_erase(struct himax_ts_data *ts)
{
	g_core_fp.fp_interface_on(ts);

	/* Reset power saving level */
	if (g_core_fp.fp_init_psl)
		g_core_fp.fp_init_psl(ts);

	g_core_fp.fp_register_write(ts, FLASH_GET_ARRAY(addr_spi200_trans_fmt),
		FLASH_GET_ARRAY(data_spi200_trans_fmt), DATA_LEN_4);

	g_core_fp.fp_register_write(ts, FLASH_GET_ARRAY(addr_spi200_trans_ctrl),
		FLASH_GET_ARRAY(data_spi200_trans_ctrl_2), DATA_LEN_4);
	g_core_fp.fp_register_write(ts, FLASH_GET_ARRAY(addr_spi200_cmd),
		FLASH_GET_ARRAY(data_spi200_cmd_2), DATA_LEN_4);

	g_core_fp.fp_register_write(ts, FLASH_GET_ARRAY(addr_spi200_cmd),
		FLASH_GET_ARRAY(data_spi200_cmd_3), DATA_LEN_4);
	usleep_range(2000000, 2000001);

	if (!g_core_fp.fp_wait_wip(ts, 100))
		E("Chip_Erase Fail");
	else
		I("Chip_Erase Success");
}

static bool himax_mcu_block_erase(struct himax_ts_data *ts, int start_addr,
				  int length)
{
	u32 page_prog_start = 0;
	u32 block_size = 0x10000;

	DEF_WORD_DATA(tmp_data);

	g_core_fp.fp_interface_on(ts);

	g_core_fp.fp_init_psl(ts);

	g_core_fp.fp_register_write(ts, FLASH_GET_ARRAY(addr_spi200_trans_fmt),
		FLASH_GET_ARRAY(data_spi200_trans_fmt), DATA_LEN_4);

	for (page_prog_start = start_addr;
		page_prog_start < start_addr + length;
		page_prog_start = page_prog_start + block_size) {
		g_core_fp.fp_register_write(ts,
			FLASH_GET_ARRAY(addr_spi200_trans_ctrl),
			FLASH_GET_ARRAY(data_spi200_trans_ctrl_2), DATA_LEN_4);
		g_core_fp.fp_register_write(ts,
			FLASH_GET_ARRAY(addr_spi200_cmd),
			FLASH_GET_ARRAY(data_spi200_cmd_2), DATA_LEN_4);

		VAL_SET(tmp_data, page_prog_start);
		g_core_fp.fp_register_write(ts, FLASH_GET_ARRAY(addr_spi200_addr),
			tmp_data.data.byte, DATA_LEN_4);

		g_core_fp.fp_register_write(ts,
			FLASH_GET_ARRAY(addr_spi200_trans_ctrl),
			FLASH_GET_ARRAY(data_spi200_trans_ctrl_3), DATA_LEN_4);
		g_core_fp.fp_register_write(ts,
			FLASH_GET_ARRAY(addr_spi200_cmd),
			FLASH_GET_ARRAY(data_spi200_cmd_4), DATA_LEN_4);
		usleep_range(1000000, 1000001);

		if (!g_core_fp.fp_wait_wip(ts, 100)) {
			E("Erase Fail");
			return false;
		}
	}

	I("END");
	return true;
}

static bool himax_mcu_sector_erase(int start_addr)
{
	return true;
}

static bool himax_mcu_flash_programming(struct himax_ts_data *ts,
					u8 *FW_content, int fw_size)
{
	int page_prog_start = 0;
	union hx_dword_data_t tmp_data = {0};
	int ret = 0;
	/* 4 bytes for padding*/
	g_core_fp.fp_interface_on(ts);

	g_core_fp.fp_register_write(ts,
		FLASH_GET_ARRAY(addr_spi200_trans_fmt),
		FLASH_GET_ARRAY(data_spi200_trans_fmt), DATA_LEN_4);

	for (page_prog_start = 0; page_prog_start < fw_size;
		page_prog_start += FLASH_RW_MAX_LEN) {
		/* ===Flash Write Enable ===*/

		g_core_fp.fp_register_write(ts,
				FLASH_GET_ARRAY(addr_spi200_trans_ctrl),
				FLASH_GET_ARRAY(data_spi200_trans_ctrl_2), DATA_LEN_4);

		g_core_fp.fp_register_write(ts, FLASH_GET_ARRAY(addr_spi200_cmd),
				FLASH_GET_ARRAY(data_spi200_cmd_2),	DATA_LEN_4);

		/* ===WEL Write Control ===*/
		g_core_fp.fp_register_write(ts,
				FLASH_GET_ARRAY(addr_spi200_trans_ctrl),
				FLASH_GET_ARRAY(data_spi200_trans_ctrl_6), DATA_LEN_4);

		g_core_fp.fp_register_write(ts,
				FLASH_GET_ARRAY(addr_spi200_cmd),
				FLASH_GET_ARRAY(data_spi200_cmd_1),	DATA_LEN_4);

		g_core_fp.fp_register_read(ts, FLASH_GET_ARRAY(addr_spi200_data),
				tmp_data.byte, DATA_LEN_4);

		/* === Check WEL Fail ===*/
		if (((tmp_data.byte[0] & 0x02) >> 1) == 0) {
			I("SPI 0x8000002c = %d, Check WEL Fail",
			  tmp_data.byte[0]);
			return false;
		}

		g_core_fp.fp_register_write(ts,
			FLASH_GET_ARRAY(addr_spi200_trans_ctrl),
			FLASH_GET_ARRAY(data_spi200_trans_ctrl_2), DATA_LEN_4);
		g_core_fp.fp_register_write(ts,
			FLASH_GET_ARRAY(addr_spi200_cmd),
			FLASH_GET_ARRAY(data_spi200_cmd_2), DATA_LEN_4);

		 /*Programmable size = 1 page = 256 bytes,*/
		 /*word_number = 256 byte / 4 = 64*/
		g_core_fp.fp_register_write(ts,
			FLASH_GET_ARRAY(addr_spi200_trans_ctrl),
			FLASH_GET_ARRAY(data_spi200_trans_ctrl_4), DATA_LEN_4);

		/* Flash start address 1st : 0x0000_0000*/
		tmp_data.dword = cpu_to_le32(page_prog_start);
		g_core_fp.fp_register_write(ts,
			FLASH_GET_ARRAY(addr_spi200_addr), tmp_data.byte, DATA_LEN_4);

		ret = g_core_fp.fp_register_write(ts,
			FLASH_GET_ARRAY(addr_spi200_data),
			&FW_content[page_prog_start], 16);
		if (ret < 0) {
			E("bus access fail!");
			return false;
		}

		g_core_fp.fp_register_write(ts, FLASH_GET_ARRAY(addr_spi200_cmd),
			FLASH_GET_ARRAY(data_spi200_cmd_6), DATA_LEN_4);

		ret = g_core_fp.fp_register_write(ts,
			FLASH_GET_ARRAY(addr_spi200_data),
			&FW_content[page_prog_start + 16], 240);
		if (ret < 0) {
			E("bus access fail!");
			return false;
		}

		if (!g_core_fp.fp_wait_wip(ts, 1)) {
			E("Flash_Programming Fail");
			return false;
		}
	}
	return true;
}

static void himax_mcu_flash_page_write(u8 *write_addr, int length,
				       u8 *write_data)
{
}

static void himax_flash_speed_set(struct himax_ts_data *ts, u8 speed)
{
	struct hx_reg_t reg_addr;
	struct hx_reg_t reg_data;

	reg_data.data.word = 0;
	reg_data.data.byte[1] = 0x02; /*extand cs high to 100ns*/
	reg_data.data.byte[0] = speed;
	WORD_REG(reg_addr, flash_clk_setup_addr);
	g_core_fp.fp_register_write(ts, REG_GET_ARRAY(reg_addr),
		REG_GET_ARRAY(reg_data), 4);
}

static int himax_mcu_fts_ctpm_fw_upgrade_with_sys_fs_32k
		(struct himax_ts_data *ts, unsigned char *fw,
		 int len, bool change_iref)
{
	/* Not use */
	return 0;
}

static int himax_mcu_fts_ctpm_fw_upgrade_with_sys_fs_60k
		(struct himax_ts_data *ts, unsigned char *fw,
		 int len, bool change_iref)
{
	/* Not use */
	return 0;
}

static int himax_mcu_fts_ctpm_fw_upgrade_with_sys_fs_64k
		(struct himax_ts_data *ts, unsigned char *fw,
		 int len, bool change_iref)
{
	int burn_firmware_success = 0;
	int counter = 0;

	if (len != FW_SIZE_64k) {
		E("The file size is not 64K bytes");
		return false;
	}

	g_core_fp.fp_ic_reset(ts, false, false);
	for (counter = 0; counter < 3; counter++) {
		g_core_fp.fp_sense_off(ts, true);
		himax_flash_speed_set(ts, HX_FLASH_SPEED_12p5M);
		g_core_fp.fp_block_erase(ts, 0x00, FW_SIZE_64k);
		if (g_core_fp.fp_flash_programming(ts, fw, FW_SIZE_64k) == false) {
			g_core_fp.fp_ic_reset(ts, false, false);
			continue;
		}

		if (g_core_fp.fp_check_crc(ts, FW_GET_ARRAY(addr_program_reload_from),
					   FW_SIZE_64k) == 0) {
			burn_firmware_success = 1;
			break;
		}
	}
	/*RawOut select initial*/
	g_core_fp.fp_register_write(ts, FW_GET_ARRAY(addr_raw_out_sel),
		FW_GET_ARRAY(data_clear), FW_GET_SZ(data_clear));
	/*DSRAM func initial*/
	g_core_fp.fp_assign_sorting_mode(ts, FW_GET_ARRAY(data_clear));
	g_core_fp.fp_ic_reset(ts, false, false);

	return burn_firmware_success;
}

static int himax_mcu_fts_ctpm_fw_upgrade_with_sys_fs_124k
		(struct himax_ts_data *ts, unsigned char *fw,
		 int len, bool change_iref)
{
	/* Not use */
	return 0;
}

static int himax_mcu_fts_ctpm_fw_upgrade_with_sys_fs_128k
		(struct himax_ts_data *ts, unsigned char *fw,
		 int len, bool change_iref)
{
	int burn_firmware_success = 0;
	int counter = 0;

	if (len != FW_SIZE_128k) {
		E("The file size is not 128K bytes");
		return false;
	}

	g_core_fp.fp_ic_reset(ts, false, false);
	for (counter = 0; counter < 3; counter++) {
		g_core_fp.fp_sense_off(ts, true);
		himax_flash_speed_set(ts, HX_FLASH_SPEED_12p5M);
		g_core_fp.fp_block_erase(ts, 0x00, FW_SIZE_128k);
		if (g_core_fp.fp_flash_programming(ts, fw, FW_SIZE_128k) == false) {
			g_core_fp.fp_ic_reset(ts, false, false);
			continue;
		}

		if (g_core_fp.fp_check_crc(ts, FW_GET_ARRAY(addr_program_reload_from),
					   FW_SIZE_128k) == 0) {
			burn_firmware_success = 1;
			break;
		}
	}
	/*RawOut select initial*/
	g_core_fp.fp_register_write(ts, FW_GET_ARRAY(addr_raw_out_sel),
		FW_GET_ARRAY(data_clear), FW_GET_SZ(data_clear));
	/*DSRAM func initial*/
	g_core_fp.fp_assign_sorting_mode(ts, FW_GET_ARRAY(data_clear));

	return burn_firmware_success;
}

static int himax_mcu_fts_ctpm_fw_upgrade_with_sys_fs_255k
		(struct himax_ts_data *ts, unsigned char *fw,
		 int len, bool change_iref)
{
	int burn_firmware_success = 0;
	int counter = 0;

	if (len != FW_SIZE_255k) {
		E("The file size is not 255K bytes");
		return false;
	}

	g_core_fp.fp_ic_reset(ts, false, false);
	for (counter = 0; counter < 3; counter++) {
		g_core_fp.fp_sense_off(ts, true);
		himax_flash_speed_set(ts, HX_FLASH_SPEED_12p5M);
		g_core_fp.fp_block_erase(ts, 0x00, FW_SIZE_255k);
		if (g_core_fp.fp_flash_programming(ts, fw, FW_SIZE_255k) == false) {
			g_core_fp.fp_ic_reset(ts, false, false);
			continue;
		}
		if (g_core_fp.fp_check_crc(ts, FW_GET_ARRAY(addr_program_reload_from),
					   FW_SIZE_255k) == 0) {
			burn_firmware_success = 1;
			break;
		}
	}
	/*RawOut select initial*/
	g_core_fp.fp_register_write(ts, FW_GET_ARRAY(addr_raw_out_sel),
		FW_GET_ARRAY(data_clear), FW_GET_SZ(data_clear));
	/*DSRAM func initial*/
	g_core_fp.fp_assign_sorting_mode(ts, FW_GET_ARRAY(data_clear));

	return burn_firmware_success;
}

static void himax_mcu_flash_dump_func
		(struct himax_ts_data *ts, u8 local_flash_command,
		 int flash_size, u8 *flash_buffer)
{
	struct hx_reg_t tmp_addr;
	int page_prog_start = 0;

	g_core_fp.fp_sense_off(ts, true);

	for (page_prog_start = 0; page_prog_start < flash_size;
		page_prog_start += 128) {
		WORD_REG(tmp_addr, page_prog_start);
		g_core_fp.fp_register_read(ts, REG_GET_ARRAY(tmp_addr),
			flash_buffer + page_prog_start, 128);
	}

	g_core_fp.fp_sense_on(ts, 0x01);
}

static bool himax_mcu_flash_lastdata_check(struct himax_ts_data *ts,
					   u32 size)
{
	struct hx_reg_t tmp_addr;
	/* 64K - 0x80, which is the address of
	 * the last 128bytes in 64K, default value
	 */
	u32 start_addr = 0xFFFFFFFF;
	u32 temp_addr = 0;
	u32 flash_page_len = 0x80;
	u8 flash_tmp_buffer[128];

	if (size < flash_page_len) {
		E("flash size is wrong, terminated");
		E("flash size = %08X; flash page len = %08X",
		  size, flash_page_len);
		goto FAIL;
	}

	/* In order to match other size of fw */
	start_addr = size - flash_page_len;
	I("Now size is %d, the start_addr is 0x%08X",
	  size, start_addr);
	for (temp_addr = start_addr; temp_addr < (start_addr + flash_page_len);
		temp_addr = temp_addr + flash_page_len) {
		/*I("temp_addr=%d,tmp_addr[0]=0x%2X, tmp_addr[1]=0x%2X,
		 *	tmp_addr[2]=0x%2X,tmp_addr[3]=0x%2X",
		 *	temp_addr,tmp_addr[0], tmp_addr[1],
		 *	tmp_addr[2],tmp_addr[3]);
		 */
		WORD_REG(tmp_addr, temp_addr);
		g_core_fp.fp_register_read(ts, REG_GET_ARRAY(tmp_addr),
			&flash_tmp_buffer[0], flash_page_len);
	}

	I("FLASH[%08X] ~ FLASH[%08X] = %02X%02X%02X%02X", size - 4, size - 1,
	  flash_tmp_buffer[flash_page_len - 4],
	  flash_tmp_buffer[flash_page_len - 3],
	  flash_tmp_buffer[flash_page_len - 2],
	  flash_tmp_buffer[flash_page_len - 1]);

	if (!flash_tmp_buffer[flash_page_len - 4] &&
	    !flash_tmp_buffer[flash_page_len - 3] &&
		!flash_tmp_buffer[flash_page_len - 2] &&
		!flash_tmp_buffer[flash_page_len - 1]) {
		I("Fail, Last four Bytes are all 0x00:");
		goto FAIL;
	} else if (flash_tmp_buffer[flash_page_len - 4] == 0xFF &&
			  (flash_tmp_buffer[flash_page_len - 3] == 0xFF) &&
			  (flash_tmp_buffer[flash_page_len - 2] == 0xFF) &&
			  (flash_tmp_buffer[flash_page_len - 1] == 0xFF)) {
		I("Fail, Last four Bytes are all 0xFF:");
		goto FAIL;
	} else {
		return 0;
	}

FAIL:
	return 1;
}

static bool hx_bin_desc_data_get(struct himax_ts_data *ts,
				 u32 addr, u8 *flash_buf,
				 const u8 *fw_all_data)
{
	u8 data_sz = 0x10;
	u32 i = 0, j = 0;
	u16 chk_end = 0;
	u16 chk_sum = 0;
	u32 map_code = 0;
	unsigned long flash_addr = 0;
	u32 hid_table_addr = 0;
	union {
		u8 *buf;
		u32 *word;
	} map_data;

	for (i = 0; i < FW_PAGE_SZ; i = i + data_sz) {
		for (j = i; j < (i + data_sz); j++) {
			chk_end |= flash_buf[j];
			chk_sum += flash_buf[j];
		}
		if (!chk_end) { /*1. Check all zero*/
			I("End in %X", i + addr);
			return false;
		} else if (chk_sum % 0x100) { /*2. Check sum*/
			I("chk sum failed in %X", i + addr);
		} else { /*3. get data*/
			map_data.buf = &flash_buf[i];
			map_code = le32_to_cpup(map_data.word);
			map_data.buf = &flash_buf[i + 4];
			flash_addr = le32_to_cpup(map_data.word);
			switch (map_code) {
			case FW_CID:
				WORD_REG(g_core_regs.flash_ver_info.addr_cid_ver_major,
					 flash_addr);
				WORD_REG(g_core_regs.flash_ver_info.addr_cid_ver_minor,
					 flash_addr + 1);
				I("CID_VER in %X", FLASH_VER_GET_VAL(addr_cid_ver_major));
				memcpy(&ts->fw_bin_desc, &fw_all_data
					   [flash_addr - sizeof(ts->hid_info.fw_bin_desc.passwd)],
				       sizeof(struct hx_bin_desc_t));
				break;
			case FW_VER:
				WORD_REG(g_core_regs.flash_ver_info.addr_fw_ver_major,
					 flash_addr);
				WORD_REG(g_core_regs.flash_ver_info.addr_fw_ver_minor,
					 flash_addr + 1);
				I("FW_VER in %X", FLASH_VER_GET_VAL(addr_fw_ver_major));
				break;
			case CFG_VER:
				WORD_REG(g_core_regs.flash_ver_info.addr_cfg_ver_major,
					 flash_addr);
				WORD_REG(g_core_regs.flash_ver_info.addr_cfg_ver_minor,
					 flash_addr + 1);
				I("CFG_VER in = %08X",	FLASH_VER_GET_VAL(addr_cfg_ver_major));
				break;
			case TP_CONFIG_TABLE:
				WORD_REG(g_core_regs.flash_ver_info.addr_cfg_table,
					 flash_addr);
				I("CONFIG_TABLE in %X", FLASH_VER_GET_VAL(addr_cfg_table));
				break;
			case HID_TABLE:
				WORD_REG(g_core_regs.flash_ver_info.addr_hid_table,
					 flash_addr);
				I("HID_TABLE in %X", FLASH_VER_GET_VAL(addr_hid_table));
				hid_table_addr = FLASH_VER_GET_VAL(addr_hid_table);
				WORD_REG(g_core_regs.flash_ver_info.addr_hid_desc,
					 hid_table_addr);
				I("HID_DESC in %X", FLASH_VER_GET_VAL(addr_hid_desc));
				WORD_REG(g_core_regs.flash_ver_info.addr_hid_rd_desc,
					 (hid_table_addr + 24));
				I("HID_RD_DESC in %X", FLASH_VER_GET_VAL(addr_hid_rd_desc));
				break;
			}
		}
		chk_end = 0;
		chk_sum = 0;
	}

	return true;
}

static bool hx_mcu_bin_desc_get(unsigned char *fw, struct himax_ts_data *ts,
				u32 max_sz)
{
	u32 addr_t = 0;
	unsigned char *fw_buf = NULL;
	bool keep_on_flag = false;
	bool bin_desc_flag = false;

	do {
		fw_buf = &fw[addr_t];

		/*Check bin is with description table or not*/
		if (!bin_desc_flag) {
			if (fw_buf[0x00] == 0x00 && fw_buf[0x01] == 0x00 &&
			    fw_buf[0x02] == 0x00 && fw_buf[0x03] == 0x00 &&
			    fw_buf[0x04] == 0x00 && fw_buf[0x05] == 0x00 &&
			    fw_buf[0x06] == 0x00 && fw_buf[0x07] == 0x00 &&
			    fw_buf[0x0E] == 0x87)
				bin_desc_flag = true;
		}
		if (!bin_desc_flag) {
			I("fw_buf[0x00] = %2X, fw_buf[0x0E] = %2X",
			  fw_buf[0x00], fw_buf[0x0E]);
			I("No description table");
			break;
		}

		/*Get related data*/
		keep_on_flag = hx_bin_desc_data_get(ts, addr_t, fw_buf, fw);
		if (FLASH_VER_GET_VAL(addr_hid_table) >= ts->hxfw->size ||
		    (FLASH_VER_GET_VAL(addr_hid_rd_desc) +
			ts->hid_rd_data.rd_length) >= ts->hxfw->size) {
			W("hid_table_addr = %d, ts->hxfw->size = %lu!",
			  FLASH_VER_GET_VAL(addr_hid_table), ts->hxfw->size);
			W("hid_rd_desc_addr = %d, rd_len = %d, ts->hxfw->size = %lu!",
			  FLASH_VER_GET_VAL(addr_hid_rd_desc),
			  ts->hid_rd_data.rd_length,
			  ts->hxfw->size);
			WORD_REG(g_core_regs.flash_ver_info.addr_hid_table, 0);
			WORD_REG(g_core_regs.flash_ver_info.addr_hid_desc, 0);
			WORD_REG(g_core_regs.flash_ver_info.addr_hid_rd_desc, 0);
		}

		addr_t = addr_t + FW_PAGE_SZ;
	} while (max_sz > addr_t && keep_on_flag);

	return bin_desc_flag;
}

static int hx_mcu_diff_overlay_flash(struct himax_ts_data *ts)
{
	int rslt = 0;
	int diff_val = 0;

	diff_val = (ts->ic_data->vendor_fw_ver);
	I("Now fw ID is 0x%04X", diff_val);
	diff_val = (diff_val >> 12);
	I("Now diff value=0x%04X", diff_val);

	if (diff_val == 1)
		I("Now size should be 128K!");
	else
		I("Now size should be 64K!");
	rslt = diff_val;
	return rslt;
}

/* FLASH side end*/
/* CORE_FLASH */

/* CORE_SRAM */
/* SRAM side start*/
static void himax_mcu_sram_write(struct himax_ts_data *ts, u8 *fw_content)
{
}

static bool himax_mcu_sram_verify(struct himax_ts_data *ts, u8 *fw_file,
				  int fw_size)
{
	return true;
}

static bool himax_mcu_get_DSRAM_data(struct himax_ts_data *ts,
				     u8 *info_data, bool dsram_flag)
{
	unsigned int i = 0;
	struct hx_reg_t tmp_addr;
	union hx_dword_data_t tmp_data = {0};
	unsigned int max_bus_size = MAX_I2C_TRANS_SZ;
	const u32 x_num = ts->ic_data->HX_RX_NUM;
	const u32 y_num = ts->ic_data->HX_TX_NUM;
	unsigned int total_size = (x_num * y_num + x_num + y_num) * 2 + 4;
	unsigned int remain_size;
	u32 retry = 0;
	unsigned int addr = 0;
	u8  *temp_info_data = NULL; /*max mkey size = 8*/
	u32 checksum = 0;
	s32 fw_run_flag = -1;

#if defined(BUS_R_DLEN)
	max_bus_size = BUS_R_DLEN;
#endif

	if (strcmp(ts->chip_name, HX83121A_ID) == 0) {
		if (max_bus_size > 4096)
			max_bus_size = 4096;
	}

	temp_info_data = kcalloc((total_size + 8), sizeof(u8), GFP_KERNEL);
	if (!temp_info_data) {
		E("Failed to allocate memory");
		return false;
	}
	/* 1. Read number of MKey R100070E8H to determin data size */
	/* m_key_num = g_hx_ic_data->HX_BT_NUM; */
	/* I("m_key_num=%d", m_key_num); */
	/* total_size += m_key_num * 2; */

	/* 2. Start DSRAM Rawdata and Wait Data Ready */
	tmp_data.dword = SRAM_GET_VAL(passwrd_start);
	tmp_data.dword = cpu_to_le32(tmp_data.dword);
	fw_run_flag = himax_write_read_reg(ts, SRAM_GET_ARRAY(addr_rawdata_addr),
					   tmp_data.byte,
					   SRAM_GET_ARRAY(passwrd_end)[1],
					   SRAM_GET_ARRAY(passwrd_end)[0]);

	if (fw_run_flag < 0) {
		E("Data NOT ready => bypass");
		kfree(temp_info_data);
		return false;
	}

	/* 3. Read RawData */
	while (retry++ < 5) {
		remain_size = total_size;
		while (remain_size > 0) {
			i = total_size - remain_size;
			addr = sram_adr_rawdata_addr + i;

			WORD_REG(tmp_addr, addr);

			if (remain_size >= max_bus_size) {
				g_core_fp.fp_register_read(ts, REG_GET_ARRAY(tmp_addr),
					&temp_info_data[i], max_bus_size);
				remain_size -= max_bus_size;
			} else {
				g_core_fp.fp_register_read(ts, REG_GET_ARRAY(tmp_addr),
					&temp_info_data[i], remain_size);
				remain_size = 0;
			}
		}

		/* 5. Data Checksum Check */
		/* 2 is meaning PASSWORD NOT included */
		checksum = 0;
		for (i = 2; i < total_size; i += 2)
			checksum += temp_info_data[i + 1] << 8 | temp_info_data[i];

		if (checksum % 0x10000 != 0) {
			E("check_sum_cal fail=%08X", checksum);

		} else {
			memcpy(info_data, temp_info_data,
			       total_size * sizeof(u8));
			break;
		}
	}

	/* 4. FW stop outputing */
	tmp_data.dword = 0;
	tmp_data.byte[3] = temp_info_data[3];
	tmp_data.byte[2] = temp_info_data[2];
	g_core_fp.fp_register_write(ts, SRAM_GET_ARRAY(addr_rawdata_addr),
		tmp_data.byte, DATA_LEN_4);

	kfree(temp_info_data);
	if (retry >= 5)
		return false;
	else
		return true;
}

/* SRAM side end*/
/* CORE_SRAM */

/* CORE_DRIVER */
static void himax_mcu_init_ic(struct himax_ts_data *ts)
{
	I("use default incell init.");
}

static void himax_suspend_proc(struct himax_ts_data *ts, bool suspended)
{
	I("himax suspend.");
}

static void himax_resume_proc(struct himax_ts_data *ts, bool suspended)
{
	int result = 0;

	I("himax resume.");
	if (!ts->ic_data->has_flash) {
		if (g_core_fp.fp_0f_op_file_dirly) {
			result = g_core_fp.fp_0f_op_file_dirly(g_fw_boot_upgrade_name,
				ts);
			if (result)
				E("update FW fail, code[%d]!!", result);
			else
				ts->resume_success = true;
		}
	} else {
		if (g_core_fp.fp_resend_cmd_func) {
			g_core_fp.fp_resend_cmd_func(ts, suspended);
			ts->resume_success = true;
		}
	}

	if (g_core_fp.fp_ap_notify_fw_sus && result == 0)
		g_core_fp.fp_ap_notify_fw_sus(ts, 0);
}

static void himax_mcu_pin_reset(struct himax_ts_data *ts)
{
	const int rst_low_period_s = RST_LOW_PERIOD_S;
	const int rst_low_period_e = RST_LOW_PERIOD_E;
	int rst_high_period_s;
	int rst_high_period_e;

	I("Now reset the Touch chip.");
	if (!ts->ic_data->has_flash) {
		rst_high_period_s = RST_HIGH_PERIOD_ZF_S;
		rst_high_period_e = RST_HIGH_PERIOD_ZF_E;
	} else {
		rst_high_period_s = RST_HIGH_PERIOD_S;
		rst_high_period_e = RST_HIGH_PERIOD_E;
	}
	himax_rst_gpio_set(ts->rst_gpio, 0);
	usleep_range(rst_low_period_s, rst_low_period_e);
	himax_rst_gpio_set(ts->rst_gpio, 1);
	usleep_range(rst_high_period_s, rst_high_period_e);
}

static void himax_mcu_ic_reset(struct himax_ts_data *ts, u8 loadconfig,
			       u8 int_off)
{
	I("status: loadconfig=%d,int_off=%d", loadconfig, int_off);

	if (ts->rst_gpio >= 0) {
		if (int_off)
			g_core_fp.fp_irq_switch(ts, 0);

		g_core_fp.fp_pin_reset(ts);

		if (int_off)
			g_core_fp.fp_irq_switch(ts, 1);
	}
}

static u8 himax_mcu_tp_info_check(struct himax_ts_data *ts)
{
	union hx_dword_data_t addr;
	char data[DATA_LEN_4] = {0};
	u32 rx_num;
	u32 tx_num;
	u32 bt_num;
	u32 max_pt;
	u32 y_res;
	u32 x_res;
	u8 int_is_edge;
	u8 stylus_func;
	u8 stylus_id_v2;
	u8 stylus_ratio;

	g_core_fp.fp_register_read(ts,
		DRV_GET_ARRAY(addr_fw_define_rxnum_txnum), data, DATA_LEN_4);
	rx_num = data[2];
	tx_num = data[3];

	g_core_fp.fp_register_read(ts,
		DRV_GET_ARRAY(addr_fw_define_maxpt_xyrvs), data, DATA_LEN_4);
	max_pt = data[0];

	g_core_fp.fp_register_read(ts,
		DRV_GET_ARRAY(addr_fw_define_x_y_res), data, DATA_LEN_4);
	y_res = be16_to_cpup((u16 *)&data[0]);
	x_res = be16_to_cpup((u16 *)&data[2]);
	/* I("rx_num=%d, tx_num=%d, max_pt=%d, y_res=%d, x_res=%d",
	 *	rx_num, tx_num, max_pt, y_res, x_res);
	 */

	g_core_fp.fp_register_read(ts,
		DRV_GET_ARRAY(addr_fw_define_int_is_edge), data, DATA_LEN_4);
	if ((data[1] & 0x01) == 1)
		int_is_edge = true;
	else
		int_is_edge = false;

	/*1. Read number of MKey R100070E8H to determin data size*/
	g_core_fp.fp_register_read(ts, SRAM_GET_ARRAY(addr_mkey), data,
		DATA_LEN_4);
	bt_num = data[0] & 0x03;
	ts->ic_data->HX_BT_NUM = bt_num;

	addr.dword = 0x1000719C;
	addr.dword = cpu_to_le32(addr.dword);

	g_core_fp.fp_register_read(ts, addr.byte, data, DATA_LEN_4);
	stylus_func = data[3];
	ts->ic_data->HX_STYLUS_FUNC = stylus_func;

	ts->ic_data->HX_RX_NUM = rx_num;

	ts->ic_data->HX_TX_NUM = tx_num;

	ts->ic_data->HX_X_RES = x_res;
	ts->ic_data->HX_Y_RES = y_res;
	ts->ic_data->HX_MAX_PT = max_pt;
	ts->ic_data->HX_INT_IS_EDGE = int_is_edge;

	if (ts->ic_data->HX_STYLUS_FUNC) {
		addr.dword = 0x100071FC;
		addr.dword = cpu_to_le32(addr.dword);
		g_core_fp.fp_register_read(ts, addr.byte, data, DATA_LEN_4);
		stylus_id_v2 = data[2];/* 0x100071FE 0=off 1=on */
		stylus_ratio = data[3];
		/* 0x100071FF 0=ratio_1 10=ratio_10 */
		ts->ic_data->HX_STYLUS_ID_V2 = stylus_id_v2;
		ts->ic_data->HX_STYLUS_RATIO = stylus_ratio;
	}

	I("TOUCH_INFO updated");

	return 0;
}

static void himax_mcu_touch_information(struct himax_ts_data *ts)
{
	if (ts->ic_data->HX_RX_NUM == 0xFFFFFFFF)
		ts->ic_data->HX_RX_NUM = FIX_HX_RX_NUM;

	if (ts->ic_data->HX_TX_NUM == 0xFFFFFFFF)
		ts->ic_data->HX_TX_NUM = FIX_HX_TX_NUM;

	if (ts->ic_data->HX_BT_NUM == 0xFFFFFFFF)
		ts->ic_data->HX_BT_NUM = FIX_HX_BT_NUM;

	if (ts->ic_data->HX_MAX_PT == 0xFFFFFFFF)
		ts->ic_data->HX_MAX_PT = FIX_HX_MAX_PT;

	if (ts->ic_data->HX_INT_IS_EDGE == 0xFF)
		ts->ic_data->HX_INT_IS_EDGE = FIX_HX_INT_IS_EDGE;

	if (ts->ic_data->HX_STYLUS_FUNC == 0xFF)
		ts->ic_data->HX_STYLUS_FUNC = FIX_HX_STYLUS_FUNC;

	if (ts->ic_data->HX_STYLUS_ID_V2 == 0xFF)
		ts->ic_data->HX_STYLUS_ID_V2 = FIX_HX_STYLUS_ID_V2;

	if (ts->ic_data->HX_STYLUS_RATIO == 0xFF)
		ts->ic_data->HX_STYLUS_RATIO = FIX_HX_STYLUS_RATIO;

	I("HX_RX_NUM = %d,HX_TX_NUM = %d",
	  ts->ic_data->HX_RX_NUM, ts->ic_data->HX_TX_NUM);
	I("HX_MAX_PT = %d", ts->ic_data->HX_MAX_PT);
	I("HX_INT_IS_EDGE = %d,HX_STYLUS_FUNC = %d",
	  ts->ic_data->HX_INT_IS_EDGE, ts->ic_data->HX_STYLUS_FUNC);
	I("HX_STYLUS_ID_V2 = %d,HX_STYLUS_RATIO = %d",
	  ts->ic_data->HX_STYLUS_ID_V2, ts->ic_data->HX_STYLUS_RATIO);
}

static void himax_mcu_calc_touch_data_size(struct himax_ts_data *ts)
{
	ts->x_channel = ts->ic_data->HX_RX_NUM;
	ts->y_channel = ts->ic_data->HX_TX_NUM;
	ts->n_finger_support = ts->ic_data->HX_MAX_PT;
}

static int himax_mcu_get_touch_data_size(void)
{
	return HIMAX_TOUCH_DATA_SIZE;
}

static int himax_mcu_hand_shaking(void)
{
	/* 0:Running, 1:Stop, 2:I2C Fail */
	int result = 0;
	return result;
}

static int himax_mcu_determin_diag_rawdata(int diag_command)
{
	return diag_command % 10;
}

static int himax_mcu_determin_diag_storage(int diag_command)
{
	return diag_command / 10;
}

static int himax_mcu_cal_data_len(int raw_cnt_rmd, int HX_MAX_PT,
				  int raw_cnt_max)
{
	int raw_data_len;
	/* rawdata checksum is 2 bytes */
	if (raw_cnt_rmd != 0x00)
		raw_data_len = MAX_I2C_TRANS_SZ
			- ((HX_MAX_PT + raw_cnt_max + 3) * 4) - 2;
	else
		raw_data_len = MAX_I2C_TRANS_SZ
			- ((HX_MAX_PT + raw_cnt_max + 2) * 4) - 2;

	return raw_data_len;
}

static bool himax_mcu_diag_check_sum(struct himax_ts_data *ts)
{
	u16 check_sum_cal = 0;
	int i;

	/* Check 128th byte CRC */
	for (i = 0, check_sum_cal = 0;
		i < (ts->touch_all_size
		- ts->touch_info_size);
		i += 2) {
		check_sum_cal += (ts->hx_rawdata_buf[i + 1]
			* FLASH_RW_MAX_LEN
			+ ts->hx_rawdata_buf[i]);
	}

	if (check_sum_cal % HX64K != 0) {
		I("fail = %2X", check_sum_cal);
		return 0;
	}

	return 1;
}

static int himax_mcu_ic_excp_recovery
		(struct himax_ts_data *ts,
		 u32 hx_excp_event,
		 u32 hx_zero_event,
		 u32 length)
{
	int ret_val = NO_ERR;

	if (hx_excp_event == length) {
		ts->excp_zero_event_count = 0;
		ret_val = HX_EXCP_EVENT;
	} else if (hx_zero_event == length) {
		if (ts->excp_zero_event_count > 5) {
			ts->excp_zero_event_count = 0;
			I("EXCEPTION event checked - ALL Zero.");
			ret_val = HX_EXCP_EVENT;
		} else {
			ts->excp_zero_event_count++;
			I("ALL Zero event is %d times.",
			  ts->excp_zero_event_count);
			ret_val = HX_ZERO_EVENT_COUNT;
		}
	}

	return ret_val;
}

static void himax_mcu_excp_ic_reset(struct himax_ts_data *ts)
{
	ts->excp_reset_active = 0;
	himax_mcu_pin_reset(ts);
	I("reset!");
}

static void himax_mcu_resend_cmd_func(struct himax_ts_data *ts, bool suspended)
{
	himax_cable_detect_func(ts, true);
}

/* CORE_DRIVER */

static int hx_turn_on_mp_func(struct himax_ts_data *ts, int on)
{
	int rslt = 0;
	int retry = 3;
	struct hx_reg_t tmp_addr;
	struct hx_reg_t tmp_data;
	u8 tmp_read[4] = {0};

	if (strcmp(HX83102D_ID, ts->chip_name) == 0) {
		WORD_REG(tmp_addr, fw_addr_ctrl_mpap_ovl);
		if (on) {
			I("Turn on MPAP mode!");
			WORD_REG(tmp_data, fw_data_ctrl_mpap_ovl_on);
			do {
				g_core_fp.fp_register_write(ts, REG_GET_ARRAY(tmp_addr),
					REG_GET_ARRAY(tmp_data), REG_GET_SZ(tmp_data));
				usleep_range(10000, 10001);
				g_core_fp.fp_register_read(ts, REG_GET_ARRAY(tmp_addr),
					tmp_read, 4);

				I("read2=0x%02X,read1=0x%02X,read0=0x%02X",
				  tmp_read[2], tmp_read[1],
				  tmp_read[0]);

				retry--;
			} while (((retry > 0) &&
			(tmp_read[2] != REG_GET_ARRAY(tmp_data)[2] &&
			tmp_read[1] != REG_GET_ARRAY(tmp_data)[1] &&
			tmp_read[0] != REG_GET_ARRAY(tmp_data)[0])));
		} else {
			I("Turn off MPAP mode!");
			WORD_REG(tmp_data, fw_data_clear);
			do {
				g_core_fp.fp_register_write(ts, REG_GET_ARRAY(tmp_addr),
					REG_GET_ARRAY(tmp_data), REG_GET_SZ(tmp_data));
				usleep_range(10000, 10001);
				g_core_fp.fp_register_read(ts, REG_GET_ARRAY(tmp_addr),
					tmp_read, sizeof(tmp_read));

				I("read2=0x%02X,read1=0x%02X,read0=0x%02X",
				  tmp_read[2], tmp_read[1], tmp_read[0]);

				retry--;
			} while ((retry > 0) &&
			(tmp_read[2] != REG_GET_ARRAY(tmp_data)[2] &&
			tmp_read[1] != REG_GET_ARRAY(tmp_data)[1] &&
			tmp_read[0] != REG_GET_ARRAY(tmp_data)[0]));
		}
	} else {
		I("Nothing to be done!");
	}

	return rslt;
}

static void hx_dis_rload_0f(struct himax_ts_data *ts, int disable)
{
	/*Disable Flash Reload*/
	g_core_fp.fp_register_write(ts,
		DRV_GET_ARRAY(addr_fw_define_flash_reload),
		ZF_GET_ARRAY(data_dis_flash_reload), DATA_LEN_4);
}

static void himax_mcu_clean_sram_0f(struct himax_ts_data *ts, u8 *addr,
				    int write_len, int type)
{
	int total_read_times = 0;
	int max_bus_size = MAX_I2C_TRANS_SZ;
	int total_size_temp = 0;
	int address = 0;
	int i = 0;

	u8 fix_data = 0x00;
	union hx_dword_data_t tmp_addr;
	u8 tmp_data[MAX_I2C_TRANS_SZ] = {0};

	I("Entering");

	total_size_temp = write_len;

	tmp_addr.dword = le32_to_cpup((__le32 *)addr);
	I("addr = 0x%08X", tmp_addr.dword);

	switch (type) {
	case 0:
		fix_data = 0x00;
		break;
	case 1:
		fix_data = 0xAA;
		break;
	case 2:
		fix_data = 0xBB;
		break;
	}

	for (i = 0; i < MAX_I2C_TRANS_SZ; i++)
		tmp_data[i] = fix_data;

	I("total size=%d", total_size_temp);

	if (total_size_temp % max_bus_size == 0)
		total_read_times = total_size_temp / max_bus_size;
	else
		total_read_times = total_size_temp / max_bus_size + 1;

	tmp_addr.dword = cpu_to_le32(tmp_addr.dword);
	for (i = 0; i < (total_read_times); i++) {
		I("[log]write %d time start!", i);
		if (total_size_temp >= max_bus_size) {
			g_core_fp.fp_register_write(ts, tmp_addr.byte, tmp_data,
				max_bus_size);
			total_size_temp = total_size_temp - max_bus_size;
		} else {
			I("last total_size_temp=%d", total_size_temp);
			g_core_fp.fp_register_write(ts, tmp_addr.byte, tmp_data,
				total_size_temp % max_bus_size);
		}
		address = ((i + 1) * max_bus_size);
		tmp_addr.dword = cpu_to_le32(address);

		usleep_range(10000, 11000);
	}

	I("END");
}

static void himax_mcu_write_sram_0f(struct himax_ts_data *ts,
				    u8 *addr, const u8 *data, u32 len)
{
	int max_bus_size = MAX_I2C_TRANS_SZ;
	u32 remain_len = 0;
	u32 address = 0;
	u32 i;
	struct hx_reg_t tmp_addr;

	I("Entering - total write size = %d", len);

#if defined(BUS_W_DLEN)
	max_bus_size = BUS_W_DLEN - ADDR_LEN_4;
#endif

	if (strcmp(ts->chip_name, HX83121A_ID) == 0) {
		if (max_bus_size > 4096)
			max_bus_size = 4096;
	}

	remain_len = len;
	address = le32_to_cpup((__le32 *)addr);

	while (remain_len > 0) {
		i = len - remain_len;
		WORD_REG(tmp_addr, address + i);

		if (remain_len > max_bus_size) {
			g_core_fp.fp_register_write(ts, REG_GET_ARRAY(tmp_addr),
				(u8 *)data + i, max_bus_size);
			remain_len -= max_bus_size;
		} else {
			g_core_fp.fp_register_write(ts, REG_GET_ARRAY(tmp_addr),
				(u8 *)data + i, remain_len);
			remain_len = 0;
		}
	}

	I("End");
}

static int himax_sram_write_crc_check(struct himax_ts_data *ts,
				      u8 *addr, const u8 *data, u32 len)
{
	int retry = 0;
	int crc = -1;

	do {
		g_core_fp.fp_write_sram_0f(ts, addr, data, len);
		crc = g_core_fp.fp_check_crc(ts, addr, len);
		retry++;
		I("HW CRC %s in %d time",
		  (crc == 0) ? "OK" : "Fail", retry);
	} while (crc != 0 && retry < 3);

	return crc;
}

static int code_overlay(struct zf_info *info, const struct firmware *fw,
			struct himax_ts_data *ts, int type)
{
	int ret = 0;
	int retry = 0;
	union hx_dword_data_t tmp_addr = { .dword = 0x10007FFC };
	union hx_dword_data_t rdata = {0};
	u8 code_idx_t = 0;
	union hx_dword_data_t code_sdata = {0};

	/* ovl_idx[0] - sorting */
	/* ovl_idx[1] - gesture */
	/* ovl_idx[2] - border	*/

	code_idx_t = ts->ovl_idx[0];
	code_sdata.dword = ovl_sorting_reply;

	if (type == 0) {
		code_idx_t = ts->ovl_idx[2];
		code_sdata.dword = ovl_border_reply;
	}
	if (code_idx_t == 0 || info[code_idx_t].write_size == 0) {
		E("wrong code overlay section[%d, %d]!",
		  code_idx_t, info[code_idx_t].write_size);
		ret = FW_NOT_READY;
		goto ALOC_CFG_BUF_FAIL;
	}

	I("upgrade code overlay section[%d]", code_idx_t);
	if (himax_sram_write_crc_check(ts, info[code_idx_t].sram_addr,
				       &fw->data[info[code_idx_t].fw_addr],
				       info[code_idx_t].write_size) != 0) {
		E("code overlay HW CRC FAIL");
		code_sdata.dword = ovl_fault;
		ret = 2;
	}

	retry = 0;
	tmp_addr.dword = cpu_to_le32(tmp_addr.dword);
	code_sdata.dword = cpu_to_le32(code_sdata.dword);
	do {
		g_core_fp.fp_register_write(ts, tmp_addr.byte, code_sdata.byte,
			DATA_LEN_4);
		usleep_range(1000, 1100);
		g_core_fp.fp_register_read(ts, tmp_addr.byte, rdata.byte,
			DATA_LEN_4);
		retry++;
	} while ((code_sdata.dword != rdata.dword) &&
		(retry < HIMAX_REG_RETRY_TIMES));

	if (retry >= HIMAX_REG_RETRY_TIMES)
		E("fail code rpl data = 0x%08X", rdata.dword);

ALOC_CFG_BUF_FAIL:
	return ret;
}

static int alg_overlay(struct himax_ts_data *ts, u8 alg_idx_t,
		       struct zf_info *info, const struct firmware *fw)
{
	int ret = 0;
	int retry = 0;
	union hx_dword_data_t tmp_addr = { .dword = 0x10007FFC };
	union hx_dword_data_t rdata = {0};
	u8 i = 0;
	union hx_dword_data_t alg_sdata = { .dword = 0xA55A5AA5 };
	union hx_dword_data_t data = { .dword = 0x00000001 };

	if (alg_idx_t == 0 || info[alg_idx_t].write_size == 0) {
		E("wrong alg overlay section[%d, %d]!",
		  alg_idx_t, info[alg_idx_t].write_size);
		ret = FW_NOT_READY;
		goto ALOC_CFG_BUF_FAIL;
	}

	// clear handshaking to 0xA55A5AA5

	retry = 0;
	tmp_addr.dword = cpu_to_le32(tmp_addr.dword);
	alg_sdata.dword = cpu_to_le32(alg_sdata.dword);
	do {
		g_core_fp.fp_register_write(ts, tmp_addr.byte, alg_sdata.byte,
			DATA_LEN_4);
		usleep_range(1000, 1100);
		g_core_fp.fp_register_read(ts, tmp_addr.byte, rdata.byte,
			DATA_LEN_4);
	} while ((rdata.dword != alg_sdata.dword) &&
	retry++ < HIMAX_REG_RETRY_TIMES);

	if (retry > HIMAX_REG_RETRY_TIMES) {
		E("init handshaking data FAIL[0x%08X]!!",
		  le32_to_cpu(rdata.dword));
	}

	alg_sdata.dword = ovl_alg_reply;

	g_core_fp.fp_reload_disable(ts, 0);

	/*Rawout Sel initial*/
	g_core_fp.fp_register_write(ts, FW_GET_ARRAY(addr_raw_out_sel),
		FW_GET_ARRAY(data_clear), FW_GET_SZ(data_clear));
	/*DSRAM func initial*/
	g_core_fp.fp_assign_sorting_mode(ts, FW_GET_ARRAY(data_clear));
	/* reset N frame back to default for normal mode */
	g_core_fp.fp_register_write(ts, FW_GET_ARRAY(addr_set_frame_addr),
		data.byte, 4);
	/*FW reload done initial*/
	g_core_fp.fp_register_write(ts,
		DRV_GET_ARRAY(addr_fw_define_2nd_flash_reload),
		FW_GET_ARRAY(data_clear), FW_GET_SZ(data_clear));

	g_core_fp.fp_sense_on(ts, 0x00);

	retry = 0;
	do {
		usleep_range(3000, 3100);
		g_core_fp.fp_register_read(ts, tmp_addr.byte, rdata.byte, DATA_LEN_4);
	} while ((rdata.dword != ovl_alg_request) && retry++ < 30);

	if (retry > 30) {
		E("fail req data = 0x%08X", le32_to_cpu(rdata.dword));
		/* monitor FW status for debug */
		for (i = 0; i < 10; i++) {
			usleep_range(10000, 10100);
			g_core_fp.fp_register_read(ts, tmp_addr.byte, rdata.byte,
				DATA_LEN_4);
			I("req data = 0x%08X", le32_to_cpu(rdata.dword));
			g_core_fp.fp_read_FW_status(ts);
		}
		ret = 3;
		goto BURN_OVL_FAIL;
	}

	I("upgrade alg overlay section[%d]", alg_idx_t);

	if (himax_sram_write_crc_check(ts, info[alg_idx_t].sram_addr,
				       &fw->data[info[alg_idx_t].fw_addr],
				       info[alg_idx_t].write_size) != 0) {
		E("Alg Overlay HW CRC FAIL");
		ret = 2;
	}

	retry = 0;
	do {
		g_core_fp.fp_register_write(ts, tmp_addr.byte, alg_sdata.byte,
			DATA_LEN_4);
		usleep_range(1000, 1100);
		g_core_fp.fp_register_read(ts, tmp_addr.byte, rdata.byte, DATA_LEN_4);
	} while ((alg_sdata.dword != rdata.dword) &&
	retry++ < HIMAX_REG_RETRY_TIMES);

	if (retry > HIMAX_REG_RETRY_TIMES) {
		E("fail rpl data = 0x%08X",	le32_to_cpu(rdata.dword));
		// maybe need to reset
	} else {
		I("waiting for FW reload data");

		retry = 0;
		while (retry++ < 30) {
			g_core_fp.fp_register_read(ts,
				DRV_GET_ARRAY(addr_fw_define_2nd_flash_reload),
				data.byte, DATA_LEN_4);

			/* use all 4 bytes to compare */
			if (le32_to_cpu(data.dword) == 0x72C0) {
				I("FW reload done");
					break;
			}
			I("wait FW reload %d times", retry);
			g_core_fp.fp_read_FW_status(ts);
			usleep_range(10000, 11000);
		}
	}

BURN_OVL_FAIL:
ALOC_CFG_BUF_FAIL:
	return ret;
}

int himax_zf_part_info(const struct firmware *fw, struct himax_ts_data *ts,
		       int type)
{
	u32 table_addr = FLASH_VER_GET_VAL(addr_cfg_table);
	int pnum = 0;
	int ret = 0;
	u8 buf[16];
	struct zf_info *info;
	u8 sram_min[4];
	int cfg_sz = 0;
	int cfg_crc_sw = 0;
	int cfg_crc_hw = 0;
	s32 i = 0;
	int i_max = 0;
	int i_min = 0;
	u32 dsram_base = 0xFFFFFFFF;
	u32 dsram_max = 0;
	int retry = 0;
	int allovlidx = 0;
	s32 alg_idx_t = 0;
	s32 j = 0;
	bool has_code_overlay = false;

	ts->has_alg_overlay = false;

	/* 1. initial check */
	if (g_core_fp._en_hw_crc)
		g_core_fp._en_hw_crc(ts, 1);
	pnum = fw->data[table_addr + 12];
	if (pnum < 2) {
		E("partition number is not correct");
		return FW_NOT_READY;
	}

	info = kcalloc(pnum, sizeof(struct zf_info), GFP_KERNEL);
	if (!info) {
		E("memory allocation fail[info]!!");
		return 1;
	}
	memset(info, 0, pnum * sizeof(struct zf_info));
	if (!ts->ovl_idx) {
		ts->ovl_idx = kcalloc(ovl_section_num, sizeof(u8), GFP_KERNEL);
		if (!ts->ovl_idx) {
			E("memory allocation fail[ovl_idx]!!");
			ret = 1;
			goto ALOC_CFG_BUF_FAIL;
		}
	}
	memset(ts->ovl_idx, 0, ovl_section_num);

	/* 2. record partition information */
	memcpy(buf, &fw->data[table_addr], 16);
	memcpy(info[0].sram_addr, buf, 4);
	info[0].write_size = le32_to_cpup((u32 *)&buf[4]);
	info[0].fw_addr = le32_to_cpup((u32 *)&buf[8]);
	I("[%d]SRAM addr=%08X, fw_addr=%08X, write_size=%d",
	  0, info[0].cfg_addr, info[0].fw_addr,
	  info[0].write_size);

	for (i = 1; i < pnum; i++) {
		memcpy(buf, &fw->data[i * 0x10 + table_addr], 16);

		memcpy(info[i].sram_addr, buf, 4);
		info[i].write_size = le32_to_cpup((u32 *)&buf[4]);
		info[i].fw_addr = le32_to_cpup((u32 *)&buf[8]);
		info[i].cfg_addr = le32_to_cpup((u32 *)&info[i].sram_addr[0]);

		if (info[i].cfg_addr % 4 != 0)
			info[i].cfg_addr -= (info[i].cfg_addr % 4);

		I("[%d]SRAM addr=%08X, fw_addr=%08X, write_size=%d",
		  i, info[i].cfg_addr, info[i].fw_addr,
		  info[i].write_size);

		/* alg overlay section */
		if ((buf[15] == 0x77 && buf[14] == 0x88)) {
			I("find alg overlay section in index %d", i);
			/* record index of alg overlay section */
			allovlidx |= 1 << i;
			alg_idx_t = i;
			ts->has_alg_overlay = true;
			continue;
		}

		/* code overlay section */
		if ((buf[15] == 0x55 && buf[14] == 0x66) ||
		    le32_to_cpup((u32 *)&buf[0]) == 0x20008CE0) {
			I("find code overlay section in index %d", i);
			has_code_overlay = true;
			/* record index of code overlay section */
			allovlidx |= 1 << i;
			if (buf[15] == 0x55 && buf[14] == 0x66) {
				/* current mechanism */
				j = buf[13];
				if (j < ovl_section_num)
					ts->ovl_idx[j] = i;
			} else {
				/* previous mechanism */
				if (j < ovl_section_num)
					ts->ovl_idx[j++] = i;
			}
			continue;
		}

		if (dsram_base > info[i].cfg_addr) {
			dsram_base = info[i].cfg_addr;
			i_min = i;
		}
		if (dsram_max < info[i].cfg_addr) {
			dsram_max = info[i].cfg_addr;
			i_max = i;
		}
	}

	/* 3. prepare data to update */
	for (i = 0; i < ADDR_LEN_4; i++)
		sram_min[i] = (info[i_min].cfg_addr >> (8 * i)) & 0xFF;

	D("dsram_max: %d, dsram_base: %d, write_size: %d",
	  dsram_max, dsram_base, info[i_max].write_size);
	cfg_sz = (dsram_max - dsram_base) + info[i_max].write_size;
	if (cfg_sz % 16 != 0)
		cfg_sz = cfg_sz + 16 - (cfg_sz % 16);

	I("cfg_sz = %d!, dsram_base = %X, dsram_max = %X",
	  cfg_sz, dsram_base, dsram_max);
	/* config size should be smaller than DSRAM size */
	if (cfg_sz > ts->chip_max_dsram_size) {
		E("config size error[%d, %d]!!",
		  cfg_sz, ts->chip_max_dsram_size);
		ret = LENGTH_FAIL;
		goto ALOC_CFG_BUF_FAIL;
	}

	memset(ts->zf_update_cfg_buffer, 0x00,
	       ts->chip_max_dsram_size * sizeof(u8));

	for (i = 1; i < pnum; i++) {
		/* overlay section */
		if (allovlidx & (1 << i)) {
			I("skip overlay section %d", i);
			continue;
		}

		memcpy(&ts->zf_update_cfg_buffer[info[i].cfg_addr - dsram_base],
		       &fw->data[info[i].fw_addr], info[i].write_size);
	}

	/* 4. write to sram */
	/* FW entity */
	if (himax_sram_write_crc_check(ts, info[0].sram_addr,
				       &fw->data[info[0].fw_addr], info[0].write_size) != 0) {
		E("HW CRC FAIL");
		ret = 2;
		goto BURN_SRAM_FAIL;
	}

	cfg_crc_sw = g_core_fp.fp_calculate_crc_with_ap(ts->zf_update_cfg_buffer,
		0, cfg_sz);
	do {
		g_core_fp.fp_write_sram_0f(ts, sram_min, ts->zf_update_cfg_buffer,
			cfg_sz);
		cfg_crc_hw = g_core_fp.fp_check_crc(ts, sram_min, cfg_sz);
		if (cfg_crc_hw != cfg_crc_sw) {
			E("Cfg CRC FAIL,HWCRC=%X,SWCRC=%X,retry=%d",
			  cfg_crc_hw, cfg_crc_sw, retry);
		}
	} while (cfg_crc_hw != cfg_crc_sw && retry++ < 3);

	if (retry > 3) {
		ret = 2;
		goto BURN_SRAM_FAIL;
	}

	/*write back system config*/
	if (type == 0)
		g_core_fp.fp_resend_cmd_func(ts, ts->suspended);

	if (ts->has_alg_overlay)
		ret = alg_overlay(ts, alg_idx_t, info, fw);
	if (has_code_overlay)
		ret = code_overlay(info, fw, ts, type);

BURN_SRAM_FAIL:
ALOC_CFG_BUF_FAIL:
	kfree(info);

	return ret;
/* ret = 1, memory allocation fail
 *     = 2, crc fail
 *     = 3, flow control error
 */
}

int himax_mcu_firmware_update_0f(const struct firmware *fw,
				 struct himax_ts_data *ts, int type)
{
	int ret = 0;
	bool bret;

	I("Entering - total FW size=%d", (int)fw->size);

	g_core_fp.fp_register_write(ts, ZF_GET_ARRAY(addr_system_reset),
		ZF_GET_ARRAY(data_system_reset), 4);

	bret = g_core_fp.fp_sense_off(ts, false);
	if (!bret) {
		E("sense off fail");
		return -1;
	}

	if ((int)fw->size > HX64K) {
		ret = himax_zf_part_info(fw, ts, type);
	} else {
		/* first 48K */
		ret = himax_sram_write_crc_check
			(ts, ZF_GET_ARRAY(data_sram_start_addr), &fw->data[0], HX_48K_SZ);
		if (ret != 0)
			E("HW CRC FAIL - Main SRAM 48K");

		/*config info*/
		if (!ts->ic_data->has_flash) {
			ret = himax_sram_write_crc_check(ts, ZF_GET_ARRAY(data_cfg_info),
							 &fw->data[0xc000], 128);
			if (ret != 0)
				E("Config info CRC Fail!");
		} else {
			g_core_fp.fp_clean_sram_0f(ts, ZF_GET_ARRAY(data_cfg_info),
				128, 2);
		}

		if (!ts->ic_data->has_flash) {
			ret = himax_sram_write_crc_check(ts, ZF_GET_ARRAY(data_fw_cfg_1),
							 &fw->data[0xc0fe], 528);
			if (ret != 0)
				E("FW config 1 CRC Fail!");
		} else {
			g_core_fp.fp_clean_sram_0f(ts, ZF_GET_ARRAY(data_fw_cfg_1),
				528, 1);
		}

		if (!ts->ic_data->has_flash) {
			ret = himax_sram_write_crc_check(ts, ZF_GET_ARRAY(data_fw_cfg_3),
							 &fw->data[0xca00], 128);
			if (ret != 0)
				E("FW config 3 CRC Fail!");
		} else {
			g_core_fp.fp_clean_sram_0f(ts, ZF_GET_ARRAY(data_fw_cfg_3),
				128, 2);
		}

		/*ADC config*/
		if (!ts->ic_data->has_flash) {
			ret = himax_sram_write_crc_check(ts, ZF_GET_ARRAY(data_adc_cfg_1),
							 &fw->data[0xd640], 1200);
			if (ret != 0)
				E("ADC config 1 CRC Fail!");
		} else {
			g_core_fp.fp_clean_sram_0f(ts, ZF_GET_ARRAY(data_adc_cfg_1),
				1200, 2);
		}

		if (!ts->ic_data->has_flash) {
			ret = himax_sram_write_crc_check(ts, ZF_GET_ARRAY(data_adc_cfg_2),
							 &fw->data[0xd320], 800);
			if (ret != 0)
				E("ADC config 2 CRC Fail!");
		} else {
			g_core_fp.fp_clean_sram_0f(ts, ZF_GET_ARRAY(data_adc_cfg_2),
				800, 2);
		}

		/*mapping table*/
		if (!ts->ic_data->has_flash) {
			ret = himax_sram_write_crc_check(ts, ZF_GET_ARRAY(data_map_table),
							 &fw->data[0xe000], 1536);
			if (ret != 0)
				E("Mapping table CRC Fail!");
		} else {
			g_core_fp.fp_clean_sram_0f(ts, ZF_GET_ARRAY(data_map_table),
				1536, 2);
		}
	}

	I("End");

	return ret;
}

int hx_0f_op_file_dirly(char *file_name, struct himax_ts_data *ts)
{
	const struct firmware *fw = NULL;
	int reqret = -1;
	int ret = -1;
	int type = 0; /* FW type: 0, normal; 1, MPAP */

	if (ts->zf_update_flag) {
		W("Other thread is updating now!");
		return ret;
	}
	ts->zf_update_flag = true;
	I("Preparing to update %s!", file_name);

	reqret = request_firmware(&fw, file_name, ts->dev);
	if (reqret < 0) {
		ret = reqret;
		E("request firmware fail, code[%d]!!", ret);
		goto END;
	}

	if (strcmp(file_name, MPAP_FWNAME) == 0)
		type = 1;

	ret = g_core_fp.fp_firmware_update_0f(fw, ts, type);

	if (reqret >= 0)
		release_firmware(fw);

	if (ret < 0)
		goto END;

	if (!ts->has_alg_overlay) {
		if (type == 1)
			g_core_fp.fp_turn_on_mp_func(ts, 1);
		else
			g_core_fp.fp_turn_on_mp_func(ts, 0);
		g_core_fp.fp_reload_disable(ts, 0);
		g_core_fp.fp_power_on_init(ts);
	}

END:
	ts->zf_update_flag = false;

	I("END");
	return ret;
}

static int himax_mcu_0f_excp_check(void)
{
	return NO_ERR;
}

void himax_mcu_read_sram_0f(struct himax_ts_data *ts,
			    const struct firmware *fw_entry,
			    u8 *addr, int start_index, int read_len)
{
	int total_read_times = 0;
	int max_bus_size = MAX_I2C_TRANS_SZ;
	int total_size_temp = 0;
	int total_size = 0;
	int address = 0;
	int i = 0, j = 0;
	int not_same = 0;
	struct hx_reg_t in_addr = { .data = {0}, .len = 4 };
	struct hx_reg_t tmp_addr = { .data = {0}, .len = 4 };
	u8 *temp_info_data = NULL;
	int *not_same_buff = NULL;

	I("Entering");
	PTR_SET(in_addr, addr, 4);
	total_size = read_len;
	total_size_temp = read_len;

	if (read_len > 2048)
		max_bus_size = 2048;
	else
		max_bus_size = read_len;

	if (total_size % max_bus_size == 0)
		total_read_times = total_size / max_bus_size;
	else
		total_read_times = total_size / max_bus_size + 1;

	I("total size=%d, bus size=%d, read time=%d",
	  total_size, max_bus_size,	total_read_times);

	memcpy(REG_GET_ARRAY(tmp_addr), addr, 4);
	I("addr[3]=0x%2X,addr[2]=0x%2X,addr[1]=0x%2X,addr[0]=0x%2X",
	  REG_GET_ARRAY(tmp_addr)[3], REG_GET_ARRAY(tmp_addr)[2],
	  REG_GET_ARRAY(tmp_addr)[1], REG_GET_ARRAY(tmp_addr)[0]);

	temp_info_data = kcalloc(total_size, sizeof(u8), GFP_KERNEL);
	if (!temp_info_data) {
		E("Failed to allocate temp_info_data");
		goto err_malloc_temp_info_data;
	}

	not_same_buff = kcalloc(total_size, sizeof(int), GFP_KERNEL);
	if (!not_same_buff) {
		E("Failed to allocate not_same_buff");
		goto err_malloc_not_same_buff;
	}

	for (i = 0; i < (total_read_times); i++) {
		if (total_size_temp >= max_bus_size) {
			g_core_fp.fp_register_read(ts, REG_GET_ARRAY(tmp_addr),
				&temp_info_data[i * max_bus_size], max_bus_size);
			total_size_temp = total_size_temp - max_bus_size;
		} else {
			g_core_fp.fp_register_read(ts, REG_GET_ARRAY(tmp_addr),
				&temp_info_data[i * max_bus_size],
				total_size_temp % max_bus_size);
		}

		address = ((i + 1) * max_bus_size);
		WORD_REG(tmp_addr, address + REG_GET_VAL(in_addr));
	}
	I("READ Start, start_index = %d", start_index);

	j = start_index;
	for (i = 0; i < read_len; i++, j++) {
		if (fw_entry->data[j] != temp_info_data[i]) {
			not_same++;
			not_same_buff[i] = 1;
		}

		I("0x%2.2X, ", temp_info_data[i]);

		if (i > 0 && i % 16 == 15)
			pr_info("\n");
	}
	I("READ END,Not Same count=%d", not_same);

	if (not_same != 0) {
		j = start_index;
		for (i = 0; i < read_len; i++, j++) {
			if (not_same_buff[i] == 1)
				I("bin=[%d] 0x%2.2X", i, fw_entry->data[j]);
		}
		for (i = 0; i < read_len; i++, j++) {
			if (not_same_buff[i] == 1)
				I("sram=[%d] 0x%2.2X", i, temp_info_data[i]);
		}
	}
	I("READ END, Not Same count=%d", not_same);

	kfree(not_same_buff);
err_malloc_not_same_buff:
	kfree(temp_info_data);
err_malloc_temp_info_data:
	return;
}

void himax_mcu_read_all_sram(struct himax_ts_data *ts, u8 *addr,
			     int read_len)
{
	int total_read_times = 0;
	int max_bus_size = MAX_I2C_TRANS_SZ;
	int total_size_temp = 0;
	int total_size = 0;
	int address = 0;
	int i = 0;
	struct hx_reg_t tmp_addr = { .data = {0}, .len = 4 };
	u8 *temp_info_data;

	I("Entering");

	total_size = read_len;
	total_size_temp = read_len;

	if (total_size % max_bus_size == 0)
		total_read_times = total_size / max_bus_size;
	else
		total_read_times = total_size / max_bus_size + 1;

	I("total size=%d", total_size);

	PTR_SET(tmp_addr, addr, 4);
	I("addr[3]=0x%2X,addr[2]=0x%2X,addr[1]=0x%2X,addr[0]=0x%2X",
	  REG_GET_ARRAY(tmp_addr)[3], REG_GET_ARRAY(tmp_addr)[2],
	  REG_GET_ARRAY(tmp_addr)[1], REG_GET_ARRAY(tmp_addr)[0]);

	temp_info_data = kcalloc(total_size, sizeof(u8), GFP_KERNEL);
	if (!temp_info_data) {
		E("Failed to allocate temp_info_data");
		return;
	}

	for (i = 0; i < (total_read_times); i++) {
		if (total_size_temp >= max_bus_size) {
			g_core_fp.fp_register_read(ts, REG_GET_ARRAY(tmp_addr),
				&temp_info_data[i * max_bus_size], max_bus_size);
			total_size_temp = total_size_temp - max_bus_size;
		} else {
			g_core_fp.fp_register_read(ts, REG_GET_ARRAY(tmp_addr),
				&temp_info_data[i * max_bus_size],
				total_size_temp % max_bus_size);
		}

		address = ((i + 1) * max_bus_size);
		WORD_REG(tmp_addr, address + REG_GET_VAL(tmp_addr));
	}
	I("addr[3]=0x%2X,addr[2]=0x%2X,addr[1]=0x%2X,addr[0]=0x%2X",
	  REG_GET_ARRAY(tmp_addr)[3], REG_GET_ARRAY(tmp_addr)[2],
	  REG_GET_ARRAY(tmp_addr)[1], REG_GET_ARRAY(tmp_addr)[0]);

	kfree(temp_info_data);

	I("END");
}

void himax_mcu_firmware_read_0f(struct himax_ts_data *ts,
				const struct firmware *fw_entry, int type)
{
	u8 tmp_addr[4];

	I("Entering");
	if (type == 0) { /* first 48K */
		g_core_fp.fp_read_sram_0f(ts, fw_entry,
			ZF_GET_ARRAY(data_sram_start_addr),
			0,
			HX_48K_SZ);
		g_core_fp.fp_read_all_sram(ts, tmp_addr, 0xC000);
	} else { /*last 16k*/
		g_core_fp.fp_read_sram_0f(ts, fw_entry, ZF_GET_ARRAY(data_cfg_info),
			0xC000, 132);

		/*FW config*/
		g_core_fp.fp_read_sram_0f(ts, fw_entry, ZF_GET_ARRAY(data_fw_cfg_1),
			0xC0FE, 484);
		g_core_fp.fp_read_sram_0f(ts, fw_entry, ZF_GET_ARRAY(data_fw_cfg_2),
			0xC9DE, 36);
		g_core_fp.fp_read_sram_0f(ts, fw_entry, ZF_GET_ARRAY(data_fw_cfg_3),
			0xCA00, 72);

		/*ADC config*/

		g_core_fp.fp_read_sram_0f(ts, fw_entry, ZF_GET_ARRAY(data_adc_cfg_1),
			0xD630, 1188);
		g_core_fp.fp_read_sram_0f(ts, fw_entry, ZF_GET_ARRAY(data_adc_cfg_2),
			0xD318, 792);

		/*mapping table*/
		g_core_fp.fp_read_sram_0f(ts, fw_entry, ZF_GET_ARRAY(data_map_table),
			0xE000, 1536);

		/* set n frame=0*/
		g_core_fp.fp_read_sram_0f(ts, fw_entry,
			FW_GET_ARRAY(addr_set_frame_addr), 0xC30C, 4);
	}

	I("END");
}

void himax_ic_reg_init(struct himax_core_command_regs *reg_data)
{
	I("Entering!");
/* CORE_IC -start- */
	BYTE_REG(reg_data->ic_op.addr_ahb_addr_byte_0, ic_adr_ahb_addr_byte_0);
	BYTE_REG(reg_data->ic_op.addr_ahb_rdata_byte_0, ic_adr_ahb_rdata_byte_0);
	BYTE_REG(reg_data->ic_op.addr_ahb_access_direction, ic_adr_ahb_access_direction);
	BYTE_REG(reg_data->ic_op.addr_conti, ic_adr_conti);
	BYTE_REG(reg_data->ic_op.addr_incr4, ic_adr_incr4);
	BYTE_REG(reg_data->ic_op.adr_i2c_psw_lb, ic_adr_i2c_psw_lb);
	BYTE_REG(reg_data->ic_op.adr_i2c_psw_ub, ic_adr_i2c_psw_ub);
	BYTE_REG(reg_data->ic_op.data_ahb_access_direction_read, ic_cmd_ahb_access_direction_read);
	BYTE_REG(reg_data->ic_op.data_conti, ic_cmd_conti);
	BYTE_REG(reg_data->ic_op.data_incr4, ic_cmd_incr4);
	BYTE_REG(reg_data->ic_op.data_i2c_psw_lb, ic_cmd_i2c_psw_lb);
	BYTE_REG(reg_data->ic_op.data_i2c_psw_ub, ic_cmd_i2c_psw_ub);
	WORD_REG(reg_data->ic_op.addr_tcon_on_rst, ic_adr_tcon_on_rst);
	WORD_REG(reg_data->ic_op.addr_adc_on_rst, ic_addr_adc_on_rst);
	WORD_REG(reg_data->ic_op.addr_psl, ic_adr_psl);
	WORD_REG(reg_data->ic_op.addr_cs_central_state, ic_adr_cs_central_state);
	WORD_REG(reg_data->ic_op.data_rst, ic_cmd_rst);
	WORD_REG(reg_data->ic_op.adr_osc_en, ic_adr_osc_en);
	WORD_REG(reg_data->ic_op.adr_osc_pw, ic_adr_osc_pw);
/* CORE_IC -end- */
/* CORE_FW -start- */
	WORD_REG(reg_data->fw_op.addr_system_reset, fw_addr_system_reset);
	WORD_REG(reg_data->fw_op.addr_ctrl_fw_isr, fw_addr_ctrl_fw);
	WORD_REG(reg_data->fw_op.addr_flag_reset_event, fw_addr_flag_reset_event);
	WORD_REG(reg_data->fw_op.addr_hsen_enable, fw_addr_hsen_enable);
	WORD_REG(reg_data->fw_op.addr_program_reload_from,
		 fw_addr_program_reload_from);
	WORD_REG(reg_data->fw_op.addr_program_reload_to,
		 fw_addr_program_reload_to);
	WORD_REG(reg_data->fw_op.addr_program_reload_page_write,
		 fw_addr_program_reload_page_write);
	WORD_REG(reg_data->fw_op.addr_raw_out_sel, fw_addr_raw_out_sel);
	WORD_REG(reg_data->fw_op.addr_reload_status, fw_addr_reload_status);
	WORD_REG(reg_data->fw_op.addr_reload_crc32_result,
		 fw_addr_reload_crc32_result);
	WORD_REG(reg_data->fw_op.addr_reload_addr_from, fw_addr_reload_addr_from);
	WORD_REG(reg_data->fw_op.addr_reload_addr_cmd_beat,
		 fw_addr_reload_addr_cmd_beat);
	WORD_REG(reg_data->fw_op.addr_selftest_addr_en, fw_addr_selftest_addr_en);
	WORD_REG(reg_data->fw_op.addr_criteria_addr, fw_addr_criteria_addr);
	WORD_REG(reg_data->fw_op.addr_set_frame_addr, fw_addr_set_frame_addr);
	WORD_REG(reg_data->fw_op.addr_selftest_result_addr,
		 fw_addr_selftest_result_addr);
	WORD_REG(reg_data->fw_op.addr_sorting_mode_en, fw_addr_sorting_mode_en);
	WORD_REG(reg_data->fw_op.addr_fw_mode_status, fw_addr_fw_mode_status);
	WORD_REG(reg_data->fw_op.addr_icid_addr, fw_addr_icid_addr);
	WORD_REG(reg_data->fw_op.addr_fw_ver_addr, fw_addr_fw_ver_addr);
	WORD_REG(reg_data->fw_op.addr_fw_cfg_addr, fw_addr_fw_cfg_addr);
	WORD_REG(reg_data->fw_op.addr_fw_vendor_addr, fw_addr_fw_vendor_addr);
	WORD_REG(reg_data->fw_op.addr_cus_info, fw_addr_cus_info);
	WORD_REG(reg_data->fw_op.addr_proj_info, fw_addr_proj_info);
	WORD_REG(reg_data->fw_op.addr_fw_state_addr, fw_addr_fw_state_addr);
	WORD_REG(reg_data->fw_op.addr_fw_dbg_msg_addr, fw_addr_fw_dbg_msg_addr);
	WORD_REG(reg_data->fw_op.addr_chk_fw_status, fw_addr_chk_fw_status);
	WORD_REG(reg_data->fw_op.addr_dd_handshak_addr, fw_addr_dd_handshak_addr);
	WORD_REG(reg_data->fw_op.addr_dd_data_addr, fw_addr_dd_data_addr);
	WORD_REG(reg_data->fw_op.addr_clr_fw_record_dd_sts,
		 fw_addr_clr_fw_record_dd_sts);
	WORD_REG(reg_data->fw_op.addr_ap_notify_fw_sus, fw_addr_ap_notify_fw_sus);
	WORD_REG(reg_data->fw_op.data_ap_notify_fw_sus_en,
		 fw_data_ap_notify_fw_sus_en);
	WORD_REG(reg_data->fw_op.data_ap_notify_fw_sus_dis,
		 fw_data_ap_notify_fw_sus_dis);
	WORD_REG(reg_data->fw_op.data_system_reset, fw_data_system_reset);
	WORD_REG(reg_data->fw_op.data_safe_mode_release_pw_active,
		 fw_data_safe_mode_release_pw_active);
	WORD_REG(reg_data->fw_op.data_safe_mode_release_pw_reset,
		 fw_data_safe_mode_release_pw_reset);
	WORD_REG(reg_data->fw_op.data_clear, fw_data_clear);
	WORD_REG(reg_data->fw_op.data_fw_stop, fw_data_fw_stop);
	WORD_REG(reg_data->fw_op.data_program_reload_start,
		 fw_data_program_reload_start);
	WORD_REG(reg_data->fw_op.data_program_reload_compare,
		 fw_data_program_reload_compare);
	WORD_REG(reg_data->fw_op.data_program_reload_break,
		 fw_data_program_reload_break);
	WORD_REG(reg_data->fw_op.data_selftest_request, fw_data_selftest_request);
	BYTE_REG(reg_data->fw_op.data_criteria_aa_top, fw_data_criteria_aa_top);
	BYTE_REG(reg_data->fw_op.data_criteria_aa_bot, fw_data_criteria_aa_bot);
	BYTE_REG(reg_data->fw_op.data_criteria_key_top, fw_data_criteria_key_top);
	BYTE_REG(reg_data->fw_op.data_criteria_key_bot, fw_data_criteria_key_bot);
	BYTE_REG(reg_data->fw_op.data_criteria_avg_top, fw_data_criteria_avg_top);
	BYTE_REG(reg_data->fw_op.data_criteria_avg_bot, fw_data_criteria_avg_bot);
	WORD_REG(reg_data->fw_op.data_set_frame, fw_data_set_frame);
	BYTE_REG(reg_data->fw_op.data_selftest_ack_hb, fw_data_selftest_ack_hb);
	BYTE_REG(reg_data->fw_op.data_selftest_ack_lb, fw_data_selftest_ack_lb);
	BYTE_REG(reg_data->fw_op.data_selftest_pass, fw_data_selftest_pass);
	BYTE_REG(reg_data->fw_op.data_normal_cmd, fw_data_normal_cmd);
	BYTE_REG(reg_data->fw_op.data_normal_status, fw_data_normal_status);
	BYTE_REG(reg_data->fw_op.data_sorting_cmd, fw_data_sorting_cmd);
	BYTE_REG(reg_data->fw_op.data_sorting_status, fw_data_sorting_status);
	BYTE_REG(reg_data->fw_op.data_dd_request, fw_data_dd_request);
	BYTE_REG(reg_data->fw_op.data_dd_ack, fw_data_dd_ack);
	BYTE_REG(reg_data->fw_op.data_idle_dis_pwd, fw_data_idle_dis_pwd);
	BYTE_REG(reg_data->fw_op.data_idle_en_pwd, fw_data_idle_en_pwd);
	BYTE_REG(reg_data->fw_op.data_rawdata_ready_hb, fw_data_rawdata_ready_hb);
	BYTE_REG(reg_data->fw_op.data_rawdata_ready_lb, fw_data_rawdata_ready_lb);
	BYTE_REG(reg_data->fw_op.addr_ahb_addr, fw_addr_ahb_addr);
	BYTE_REG(reg_data->fw_op.data_ahb_dis, fw_data_ahb_dis);
	BYTE_REG(reg_data->fw_op.data_ahb_en, fw_data_ahb_en);
	BYTE_REG(reg_data->fw_op.addr_event_addr, fw_addr_event_addr);
	WORD_REG(reg_data->fw_op.addr_usb_detect, fw_usb_detect_addr);
/* CORE_FW -end- */
/* CORE_FLASH -start- */
	WORD_REG(reg_data->flash_op.addr_spi200_trans_fmt,
		 flash_addr_spi200_trans_fmt);
	WORD_REG(reg_data->flash_op.addr_spi200_trans_ctrl,
		 flash_addr_spi200_trans_ctrl);
	WORD_REG(reg_data->flash_op.addr_spi200_fifo_rst,
		 flash_addr_spi200_fifo_rst);
	WORD_REG(reg_data->flash_op.addr_spi200_flash_speed,
		 flash_addr_spi200_flash_speed);
	WORD_REG(reg_data->flash_op.addr_spi200_rst_status,
		 flash_addr_spi200_rst_status);
	WORD_REG(reg_data->flash_op.addr_spi200_cmd, flash_addr_spi200_cmd);
	WORD_REG(reg_data->flash_op.addr_spi200_addr, flash_addr_spi200_addr);
	WORD_REG(reg_data->flash_op.addr_spi200_data, flash_addr_spi200_data);
	WORD_REG(reg_data->flash_op.addr_spi200_bt_num, flash_addr_spi200_bt_num);
	WORD_REG(reg_data->flash_op.data_spi200_trans_fmt,
		 flash_data_spi200_trans_fmt);
	WORD_REG(reg_data->flash_op.data_spi200_txfifo_rst,
		 flash_data_spi200_txfifo_rst);
	WORD_REG(reg_data->flash_op.data_spi200_rxfifo_rst,
		 flash_data_spi200_rxfifo_rst);
	WORD_REG(reg_data->flash_op.data_spi200_trans_ctrl_1,
		 flash_data_spi200_trans_ctrl_1);
	WORD_REG(reg_data->flash_op.data_spi200_trans_ctrl_2,
		 flash_data_spi200_trans_ctrl_2);
	WORD_REG(reg_data->flash_op.data_spi200_trans_ctrl_3,
		 flash_data_spi200_trans_ctrl_3);
	WORD_REG(reg_data->flash_op.data_spi200_trans_ctrl_4,
		 flash_data_spi200_trans_ctrl_4);
	WORD_REG(reg_data->flash_op.data_spi200_trans_ctrl_5,
		 flash_data_spi200_trans_ctrl_5);
	WORD_REG(reg_data->flash_op.data_spi200_trans_ctrl_6,
		 flash_data_spi200_trans_ctrl_6);
	WORD_REG(reg_data->flash_op.data_spi200_trans_ctrl_7,
		 flash_data_spi200_trans_ctrl_7);
	WORD_REG(reg_data->flash_op.data_spi200_cmd_1, flash_data_spi200_cmd_1);
	WORD_REG(reg_data->flash_op.data_spi200_cmd_2, flash_data_spi200_cmd_2);
	WORD_REG(reg_data->flash_op.data_spi200_cmd_3, flash_data_spi200_cmd_3);
	WORD_REG(reg_data->flash_op.data_spi200_cmd_4, flash_data_spi200_cmd_4);
	WORD_REG(reg_data->flash_op.data_spi200_cmd_5, flash_data_spi200_cmd_5);
	WORD_REG(reg_data->flash_op.data_spi200_cmd_6, flash_data_spi200_cmd_6);
	WORD_REG(reg_data->flash_op.data_spi200_cmd_7, flash_data_spi200_cmd_7);
	WORD_REG(reg_data->flash_op.data_spi200_cmd_8, flash_data_spi200_cmd_8);
	WORD_REG(reg_data->flash_op.data_spi200_addr, flash_data_spi200_addr);
/* CORE_FLASH -end- */
/* CORE_SRAM */
	/* sram start*/
	WORD_REG(reg_data->sram_op.addr_mkey, sram_adr_mkey);
	WORD_REG(reg_data->sram_op.addr_rawdata_addr, sram_adr_rawdata_addr);
	WORD_REG(reg_data->sram_op.addr_rawdata_end, sram_adr_rawdata_end);
	HALF_REG(reg_data->sram_op.passwrd_start, sram_passwrd_start);
	HALF_REG(reg_data->sram_op.passwrd_end, sram_passwrd_end);
	/* sram end*/
/* CORE_SRAM */
/* CORE_DRIVER -start- */
	WORD_REG(reg_data->driver_op.addr_fw_define_flash_reload,
		 driver_addr_fw_define_flash_reload);
	WORD_REG(reg_data->driver_op.addr_fw_define_2nd_flash_reload,
		 driver_addr_fw_define_2nd_flash_reload);
	WORD_REG(reg_data->driver_op.addr_fw_define_int_is_edge,
		 driver_addr_fw_define_int_is_edge);
	WORD_REG(reg_data->driver_op.addr_fw_define_rxnum_txnum,
		 driver_addr_fw_define_rxnum_txnum);
	WORD_REG(reg_data->driver_op.addr_fw_define_maxpt_xyrvs,
		 driver_addr_fw_define_maxpt_xyrvs);
	WORD_REG(reg_data->driver_op.addr_fw_define_x_y_res,
		 driver_addr_fw_define_x_y_res);
	BYTE_REG(reg_data->driver_op.data_df_rx, driver_data_df_rx);
	BYTE_REG(reg_data->driver_op.data_df_tx, driver_data_df_tx);
	BYTE_REG(reg_data->driver_op.data_df_pt, driver_data_df_pt);
	WORD_REG(reg_data->driver_op.data_fw_define_flash_reload_dis,
		 driver_data_fw_define_flash_reload_dis);
	WORD_REG(reg_data->driver_op.data_fw_define_flash_reload_en,
		 driver_data_fw_define_flash_reload_en);
	WORD_REG(reg_data->driver_op.data_fw_define_rxnum_txnum_maxpt_sorting,
		 driver_data_fw_define_rxnum_txnum_maxpt_sorting);
	WORD_REG(reg_data->driver_op.data_fw_define_rxnum_txnum_maxpt_normal,
		 driver_data_fw_define_rxnum_txnum_maxpt_normal);
/* CORE_DRIVER -end- */
	WORD_REG(reg_data->zf_op.data_dis_flash_reload, zf_data_dis_flash_reload);
	WORD_REG(reg_data->zf_op.addr_system_reset, zf_addr_system_reset);
	BYTE_REG(reg_data->zf_op.data_system_reset, zf_data_system_reset);
	WORD_REG(reg_data->zf_op.data_sram_start_addr, zf_data_sram_start_addr);
	WORD_REG(reg_data->zf_op.data_cfg_info, zf_data_cfg_info);
	WORD_REG(reg_data->zf_op.data_fw_cfg_1, zf_data_fw_cfg_1);
	WORD_REG(reg_data->zf_op.data_fw_cfg_2, zf_data_fw_cfg_2);
	WORD_REG(reg_data->zf_op.data_fw_cfg_3, zf_data_fw_cfg_3);
	WORD_REG(reg_data->zf_op.data_adc_cfg_1, zf_data_adc_cfg_1);
	WORD_REG(reg_data->zf_op.data_adc_cfg_2, zf_data_adc_cfg_2);
	WORD_REG(reg_data->zf_op.data_adc_cfg_3, zf_data_adc_cfg_3);
	WORD_REG(reg_data->zf_op.data_map_table, zf_data_map_table);
	WORD_REG(reg_data->zf_op.addr_sts_chk, zf_addr_sts_chk);
	BYTE_REG(reg_data->zf_op.data_activ_sts, zf_data_activ_sts);
	WORD_REG(reg_data->zf_op.addr_activ_relod, zf_addr_activ_relod);
	BYTE_REG(reg_data->zf_op.data_activ_in, zf_data_activ_in);
}

/* CORE_INIT */
/* init start */
void himax_ic_fp_init(void)
{
/* CORE_IC */
	g_core_fp.fp_burst_enable = himax_mcu_burst_enable;
	g_core_fp.fp_register_read = himax_mcu_register_read;
	g_core_fp.fp_reg_read = himax_mcu_reg_read;
	/*
	 * g_core_fp.fp_flash_write_burst = himax_mcu_flash_write_burst;
	 */
	/*
	 * g_core_fp.fp_flash_write_burst_length =
	 *	himax_mcu_flash_write_burst_length;
	 */
	g_core_fp.fp_register_write = himax_mcu_register_write;
	g_core_fp.fp_reg_write = himax_mcu_reg_write;
	g_core_fp.fp_interface_on = himax_mcu_interface_on;
	g_core_fp.fp_sense_on = himax_mcu_sense_on;
	g_core_fp.fp_sense_off = himax_mcu_sense_off;
	g_core_fp.fp_wait_wip = himax_mcu_wait_wip;
	g_core_fp.fp_init_psl = himax_mcu_init_psl;
	g_core_fp.fp_resume_ic_action = himax_mcu_resume_ic_action;
	g_core_fp.fp_suspend_ic_action = himax_mcu_suspend_ic_action;
	g_core_fp.fp_power_on_init = himax_mcu_power_on_init;
/* CORE_IC */
/* CORE_FW */
	g_core_fp.fp_system_reset = himax_mcu_system_reset;
	g_core_fp.fp_calculate_crc_with_ap = himax_mcu_calculate_crc_with_ap;
	g_core_fp.fp_check_crc = himax_mcu_check_crc;
	g_core_fp.fp_set_reload_cmd = himax_mcu_set_reload_cmd;
	g_core_fp.fp_program_reload = himax_mcu_program_reload;
	g_core_fp.fp_usb_detect_set = himax_mcu_usb_detect_set;
	g_core_fp.fp_diag_register_set = himax_mcu_diag_register_set;
	g_core_fp.fp_diag_register_get = himax_mcu_diag_register_get;
	g_core_fp.fp_idle_mode = himax_mcu_idle_mode;
	g_core_fp.fp_reload_disable = himax_mcu_reload_disable;
	g_core_fp.fp_read_ic_trigger_type = himax_mcu_read_ic_trigger_type;
	g_core_fp.fp_read_FW_ver = himax_mcu_read_FW_ver;
	g_core_fp.fp_read_event_stack = himax_mcu_read_event_stack;
	g_core_fp.fp_return_event_stack = himax_mcu_return_event_stack;
	g_core_fp.fp_calculate_checksum = himax_mcu_calculate_checksum;
	g_core_fp.fp_read_FW_status = himax_mcu_read_FW_status;
	g_core_fp.fp_irq_switch = himax_mcu_irq_switch;
	g_core_fp.fp_assign_sorting_mode = himax_mcu_assign_sorting_mode;
	g_core_fp.fp_check_sorting_mode = himax_mcu_check_sorting_mode;
	g_core_fp.fp_read_DD_status = himax_mcu_read_DD_status;
	g_core_fp.fp_clr_fw_reord_dd_sts = hx_clr_fw_reord_dd_sts;
	g_core_fp.fp_ap_notify_fw_sus = hx_ap_notify_fw_sus;
/* CORE_FW */
/* CORE_FLASH */
	g_core_fp.fp_chip_erase = himax_mcu_chip_erase;
	g_core_fp.fp_block_erase = himax_mcu_block_erase;
	g_core_fp.fp_sector_erase = himax_mcu_sector_erase;
	g_core_fp.fp_flash_programming = himax_mcu_flash_programming;
	g_core_fp.fp_flash_page_write = himax_mcu_flash_page_write;
	g_core_fp.fp_fts_ctpm_fw_upgrade_with_sys_fs_32k =
			himax_mcu_fts_ctpm_fw_upgrade_with_sys_fs_32k;
	g_core_fp.fp_fts_ctpm_fw_upgrade_with_sys_fs_60k =
			himax_mcu_fts_ctpm_fw_upgrade_with_sys_fs_60k;
	g_core_fp.fp_fts_ctpm_fw_upgrade_with_sys_fs_64k =
			himax_mcu_fts_ctpm_fw_upgrade_with_sys_fs_64k;
	g_core_fp.fp_fts_ctpm_fw_upgrade_with_sys_fs_124k =
			himax_mcu_fts_ctpm_fw_upgrade_with_sys_fs_124k;
	g_core_fp.fp_fts_ctpm_fw_upgrade_with_sys_fs_128k =
			himax_mcu_fts_ctpm_fw_upgrade_with_sys_fs_128k;
	g_core_fp.fp_fts_ctpm_fw_upgrade_with_sys_fs_255k =
			himax_mcu_fts_ctpm_fw_upgrade_with_sys_fs_255k;
	g_core_fp.fp_flash_dump_func = himax_mcu_flash_dump_func;
	g_core_fp.fp_flash_lastdata_check = himax_mcu_flash_lastdata_check;
	g_core_fp.fp_bin_desc_get = hx_mcu_bin_desc_get;
	g_core_fp.fp_diff_overlay_flash = hx_mcu_diff_overlay_flash;
/* CORE_FLASH */
/* CORE_SRAM */
	g_core_fp.fp_sram_write = himax_mcu_sram_write;
	g_core_fp.fp_sram_verify = himax_mcu_sram_verify;
	g_core_fp.fp_get_DSRAM_data = himax_mcu_get_DSRAM_data;
/* CORE_SRAM */
/* CORE_DRIVER */
	g_core_fp.fp_chip_init = himax_mcu_init_ic;
	g_core_fp.fp_pin_reset = himax_mcu_pin_reset;
	g_core_fp.fp_ic_reset = himax_mcu_ic_reset;
	g_core_fp.fp_tp_info_check = himax_mcu_tp_info_check;
	g_core_fp.fp_touch_information = himax_mcu_touch_information;
	g_core_fp.fp_calc_touch_data_size = himax_mcu_calc_touch_data_size;
	g_core_fp.fp_get_touch_data_size = himax_mcu_get_touch_data_size;
	g_core_fp.fp_hand_shaking = himax_mcu_hand_shaking;
	g_core_fp.fp_determin_diag_rawdata = himax_mcu_determin_diag_rawdata;
	g_core_fp.fp_determin_diag_storage = himax_mcu_determin_diag_storage;
	g_core_fp.fp_cal_data_len = himax_mcu_cal_data_len;
	g_core_fp.fp_diag_check_sum = himax_mcu_diag_check_sum;
	g_core_fp.fp_ic_excp_recovery = himax_mcu_ic_excp_recovery;
	g_core_fp.fp_excp_ic_reset = himax_mcu_excp_ic_reset;
	g_core_fp.fp_resend_cmd_func = himax_mcu_resend_cmd_func;
/* CORE_DRIVER */
	g_core_fp.fp_turn_on_mp_func = hx_turn_on_mp_func;
	g_core_fp.fp_reload_disable = hx_dis_rload_0f;
	g_core_fp.fp_clean_sram_0f = himax_mcu_clean_sram_0f;
	g_core_fp.fp_write_sram_0f = himax_mcu_write_sram_0f;
	g_core_fp.fp_write_sram_0f_crc = himax_sram_write_crc_check;
	g_core_fp.fp_firmware_update_0f = himax_mcu_firmware_update_0f;
	g_core_fp.fp_0f_op_file_dirly = hx_0f_op_file_dirly;
	g_core_fp.fp_0f_excp_check = himax_mcu_0f_excp_check;
	g_core_fp.fp_read_sram_0f = himax_mcu_read_sram_0f;
	g_core_fp.fp_read_all_sram = himax_mcu_read_all_sram;
	g_core_fp.fp_firmware_read_0f = himax_mcu_firmware_read_0f;
	g_core_fp.fp_suspend_proc = himax_suspend_proc;
	g_core_fp.fp_resume_proc = himax_resume_proc;
}
