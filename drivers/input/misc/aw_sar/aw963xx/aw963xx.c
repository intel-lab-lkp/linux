// SPDX-License-Identifier: GPL-2.0
/*
 * AWINIC sar sensor driver (aw963xx)
 *
 * Author: Shuaijie Wang<wangshuaijie@awinic.com>
 *
 * Copyright (c) 2024 awinic Technology CO., LTD
 */
#include "aw963xx.h"
#include "../aw_sar.h"

#define AW963XX_I2C_NAME "aw963xx_sar"

static void aw963xx_set_cs_as_irq(struct aw_sar *p_sar, int flag);
static void aw963xx_get_ref_ch_enable(struct aw_sar *p_sar);

static int32_t aw963xx_read_init_over_irq(void *load_bin_para)
{
	struct aw_sar *p_sar = (struct aw_sar *)load_bin_para;
	uint32_t cnt = 1000;
	uint32_t reg;
	int32_t ret;

	while (cnt--) {
		ret = aw_sar_i2c_read(p_sar->i2c, REG_IRQSRC, &reg);
		if (ret != 0) {
			dev_err(p_sar->dev, "i2c error %d", ret);
			return ret;
		}
		if ((reg & 0x01) == 0x01) {
			aw_sar_i2c_read(p_sar->i2c, REG_FWVER, &reg);
			return 0;
		}
		mdelay(1);
	}

	aw_sar_i2c_read(p_sar->i2c, REG_FWVER, &reg);

	return -EINVAL;
}

static void aw963xx_convert_little_endian_2_big_endian(struct aw_bin *aw_bin)
{
	uint32_t start_index = aw_bin->header_info[0].valid_data_addr;
	uint32_t fw_len = aw_bin->header_info[0].reg_num;
	uint32_t uints = fw_len / AW963XX_SRAM_UPDATE_ONE_UINT_SIZE;
	uint8_t tmp1;
	uint8_t tmp2;
	uint8_t tmp3;
	uint8_t tmp4;
	int i;

	for (i = 0; i < uints; i++) {
		tmp1 = aw_bin->info.data[start_index + i * AW963XX_SRAM_UPDATE_ONE_UINT_SIZE + 3];
		tmp2 = aw_bin->info.data[start_index + i * AW963XX_SRAM_UPDATE_ONE_UINT_SIZE + 2];
		tmp3 = aw_bin->info.data[start_index + i * AW963XX_SRAM_UPDATE_ONE_UINT_SIZE + 1];
		tmp4 = aw_bin->info.data[start_index + i * AW963XX_SRAM_UPDATE_ONE_UINT_SIZE];
		aw_bin->info.data[start_index + i * AW963XX_SRAM_UPDATE_ONE_UINT_SIZE]     = tmp1;
		aw_bin->info.data[start_index + i * AW963XX_SRAM_UPDATE_ONE_UINT_SIZE + 1] = tmp2;
		aw_bin->info.data[start_index + i * AW963XX_SRAM_UPDATE_ONE_UINT_SIZE + 2] = tmp3;
		aw_bin->info.data[start_index + i * AW963XX_SRAM_UPDATE_ONE_UINT_SIZE + 3] = tmp4;
	}
}

/**
 * @aw963xx_sram_fill_not_wrote_area()
 *         |----------------code ram-----------------|
 *       0x2000                                    0x4fff
 *         |--- app wrote here ---|--fill with 0xff--|
 *
 *         if the size of app is less than the size of code ram, the rest of the
 *         ram is filled with 0xff.
 * @load_bin_para
 * @offset the rear addr of app
 * @return int32_t
 */
static int32_t aw963xx_sram_fill_not_wrote_area(void *load_bin_para, uint32_t offset)
{
	uint32_t last_pack_len = (AW963XX_SRAM_END_ADDR - offset) %
						AW963XX_SRAM_UPDATE_ONE_PACK_SIZE;
	uint32_t pack_cnt = last_pack_len == 0 ?
			((AW963XX_SRAM_END_ADDR - offset) / AW963XX_SRAM_UPDATE_ONE_PACK_SIZE) :
			((AW963XX_SRAM_END_ADDR - offset) / AW963XX_SRAM_UPDATE_ONE_PACK_SIZE) + 1;
	uint8_t buf[AW963XX_SRAM_UPDATE_ONE_PACK_SIZE + 2] = { 0 };
	struct aw_sar *p_sar = (struct aw_sar *)load_bin_para;
	uint32_t download_addr_with_ofst;
	uint8_t *r_buf;
	int32_t ret;
	uint32_t i;

	r_buf = devm_kzalloc(p_sar->dev, AW963XX_SRAM_UPDATE_ONE_PACK_SIZE, GFP_KERNEL);
	if (!r_buf)
		return -ENOMEM;

	memset(buf, 0xff, sizeof(buf));
	for (i = 0; i < pack_cnt; i++) {
		memset(r_buf, 0, AW963XX_SRAM_UPDATE_ONE_PACK_SIZE);
		download_addr_with_ofst = offset + i * AW963XX_SRAM_UPDATE_ONE_PACK_SIZE;
		buf[0] = (uint8_t)(download_addr_with_ofst >> OFFSET_BIT_8);
		buf[1] = (uint8_t)(download_addr_with_ofst);
		if (i != (pack_cnt - 1)) {
			ret = aw_sar_i2c_write_seq(p_sar->i2c, buf,
					AW963XX_SRAM_UPDATE_ONE_PACK_SIZE + 2);
			if (ret != 0) {
				dev_err(p_sar->dev, "cnt%d, write_seq error!", i);
				devm_kfree(p_sar->dev, r_buf);
				return ret;
			}
			ret = aw_sar_i2c_read_seq(p_sar->i2c, buf, 2, r_buf,
					AW963XX_SRAM_UPDATE_ONE_PACK_SIZE);
			if (ret != 0) {
				dev_err(p_sar->dev, "cnt%d, read_seq error!", i);
				devm_kfree(p_sar->dev, r_buf);
				return ret;
			}
			if (memcmp(&buf[2], r_buf, AW963XX_SRAM_UPDATE_ONE_PACK_SIZE) != 0) {
				dev_err(p_sar->dev, "read is not equal to write ");
				devm_kfree(p_sar->dev, r_buf);
				return -EINVAL;
			}
		} else {
			ret = aw_sar_i2c_write_seq(p_sar->i2c, buf, last_pack_len + 2);
			if (ret != 0) {
				dev_err(p_sar->dev, "cnt%d, write_seq error!", i);
				devm_kfree(p_sar->dev, r_buf);
				return ret;
			}
			ret = aw_sar_i2c_read_seq(p_sar->i2c, buf, 2, r_buf, last_pack_len);
			if (ret != 0) {
				dev_err(p_sar->dev, "cnt%d, read_seq error!", i);
				devm_kfree(p_sar->dev, r_buf);
				return ret;
			}
			if (memcmp(&buf[2], r_buf, last_pack_len) != 0) {
				dev_err(p_sar->dev, "read is not equal to write ");
				devm_kfree(p_sar->dev, r_buf);
				return -EINVAL;
			}
		}
	}

	devm_kfree(p_sar->dev, r_buf);

	return 0;
}

static int32_t aw963xx_sram_data_write(struct aw_bin *aw_bin, void *load_bin_para)
{
	uint8_t buf[AW963XX_SRAM_UPDATE_ONE_PACK_SIZE + 2] = { 0 };
	uint32_t start_index = aw_bin->header_info[0].valid_data_addr;
	uint32_t fw_bin_version = aw_bin->header_info[0].app_version;
	uint32_t download_addr = AW963XX_RAM_START_ADDR;
	uint32_t fw_len = aw_bin->header_info[0].reg_num;
	uint32_t last_pack_len = fw_len % AW963XX_SRAM_UPDATE_ONE_PACK_SIZE;
	struct aw_sar *p_sar = (struct aw_sar *)load_bin_para;
	uint32_t download_addr_with_ofst = 0;
	uint32_t pack_cnt;
	uint8_t *r_buf;
	int32_t ret = -EINVAL;
	uint32_t i;

	r_buf = devm_kzalloc(p_sar->dev, AW963XX_SRAM_UPDATE_ONE_PACK_SIZE, GFP_KERNEL);
	if (!r_buf)
		return -ENOMEM;

	pack_cnt = ((fw_len % AW963XX_SRAM_UPDATE_ONE_PACK_SIZE) == 0) ?
			(fw_len / AW963XX_SRAM_UPDATE_ONE_PACK_SIZE) :
			(fw_len / AW963XX_SRAM_UPDATE_ONE_PACK_SIZE) + 1;

	dev_info(p_sar->dev, "fw_bin_version = 0x%x", fw_bin_version);
	for (i = 0; i < pack_cnt; i++) {
		memset(r_buf, 0, AW963XX_SRAM_UPDATE_ONE_PACK_SIZE);
		download_addr_with_ofst = download_addr + i * AW963XX_SRAM_UPDATE_ONE_PACK_SIZE;
		buf[0] = (uint8_t)(download_addr_with_ofst >> OFFSET_BIT_8);
		buf[1] = (uint8_t)(download_addr_with_ofst);
		if (i != (pack_cnt - 1)) {
			memcpy(&buf[2], &aw_bin->info.data[start_index +
					i * AW963XX_SRAM_UPDATE_ONE_PACK_SIZE],
					AW963XX_SRAM_UPDATE_ONE_PACK_SIZE);
			ret = aw_sar_i2c_write_seq(p_sar->i2c, buf,
					AW963XX_SRAM_UPDATE_ONE_PACK_SIZE + 2);
			if (ret != 0) {
				dev_err(p_sar->dev, "cnt%d, write_seq error!", i);
				goto err_out;
			}
			ret = aw_sar_i2c_read_seq(p_sar->i2c, buf, 2, r_buf,
					AW963XX_SRAM_UPDATE_ONE_PACK_SIZE);
			if (ret != 0) {
				dev_err(p_sar->dev, "cnt%d, read_seq error!", i);
				goto err_out;
			}
			if (memcmp(&buf[2], r_buf, AW963XX_SRAM_UPDATE_ONE_PACK_SIZE) != 0) {
				dev_err(p_sar->dev, "read is not equal to write ");
				ret = -EIO;
				goto err_out;
			}
		} else { // last pack process
			memcpy(&buf[2], &aw_bin->info.data[start_index +
					i * AW963XX_SRAM_UPDATE_ONE_PACK_SIZE], last_pack_len);
			ret = aw_sar_i2c_write_seq(p_sar->i2c, buf, last_pack_len + 2);
			if (ret != 0) {
				dev_err(p_sar->dev, "cnt%d, write_seq error!", i);
				goto err_out;
			}
			ret = aw_sar_i2c_read_seq(p_sar->i2c, buf, 2, r_buf, last_pack_len);
			if (ret != 0) {
				dev_err(p_sar->dev, "cnt%d, read_seq error!", i);
				goto err_out;
			}
			if (memcmp(&buf[2], r_buf, last_pack_len) != 0) {
				dev_err(p_sar->dev, "read is not equal to write ");
				ret = -EIO;
				goto err_out;
			}
			/* fill 0xff in the area that not worte. */
			ret = aw963xx_sram_fill_not_wrote_area(load_bin_para,
					download_addr_with_ofst + last_pack_len);
			if (ret != 0) {
				dev_err(p_sar->dev, "cnt%d, sram_fill_not_wrote_area error!", i);
				goto err_out;
			}
		}
	}

err_out:
	devm_kfree(p_sar->dev, r_buf);

	return ret;
}

static int32_t aw963xx_update_firmware(struct aw_bin *aw_bin, void *load_bin_para)
{
	struct aw_sar *p_sar = (struct aw_sar *)load_bin_para;
	struct aw963xx *aw963xx = (struct aw963xx *)p_sar->priv_data;
	struct i2c_client *i2c = p_sar->i2c;
	int32_t ret;

	if (aw963xx->start_mode == AW963XX_ROM_MODE) {
		dev_info(p_sar->dev, "no need to update fw.");
		return 0;
	}

	//step1: close coderam shutdown mode
	aw_sar_i2c_write(i2c, 0xfff4, 0x3c00d11f);
	aw_sar_i2c_write(i2c, 0xc400, 0x21660000);

	// step 2: reset mcu only and set boot mode to 1. (0xf800 0x00010100)
	aw_sar_i2c_write(i2c, REG_CPU_MODE_SET, AW963XX_RESET_CPU_SET_BOOT_SATRT);

	// step 3: enable data ram. (0xFFE4 0x3C000000)
	aw_sar_i2c_write(i2c, REG_RAM_PASSWORD, AW963XX_NO_ENCRYPTION);

	// setp 4: convert LD to BD
	aw963xx_convert_little_endian_2_big_endian(aw_bin);

	// step 5: write ram data.
	ret = aw963xx_sram_data_write(aw_bin, load_bin_para);
	if (ret == 0)
		dev_info(p_sar->dev, "sram_data_write OK");
	else
		dev_err(p_sar->dev, "sram_data_write error");

	mdelay(3);

	// step 6: exit reset mcu and boot cpu in ram. (0xf800 0x00000100)
	aw_sar_i2c_write(i2c, REG_CPU_MODE_SET, AW963XX_EXIT_RESET_CPU_SET_BOOT_SATRT);

	// step 7: reset cpu (0xFF0C 0x0)
	aw_sar_i2c_write(i2c, REG_CPU_RESET, AW963XX_RESET_SET);

	//step 8: Wait for chip initialization to complete
	msleep(AW963XX_CHIP_INIT_MAX_TIME_MS);

	return aw963xx_read_init_over_irq(load_bin_para);
}

static int32_t aw963xx_load_reg_bin(struct aw_bin *aw_bin, void *load_bin_para)
{
	struct aw_sar *p_sar = (struct aw_sar *)load_bin_para;
	struct aw963xx *aw963xx = (struct aw963xx *)p_sar->priv_data;
	int32_t ret;

	dev_dbg(p_sar->dev, "reg chip name: %s, soc chip name: %s, len = %d",
			p_sar->chip_name, aw_bin->header_info[0].chip_type, aw_bin->info.len);

	ret = strncmp(p_sar->chip_name, aw_bin->header_info[0].chip_type,
			sizeof(aw_bin->header_info[0].chip_type));
	if (ret != 0)
		dev_err(p_sar->dev, "load_binname(%s) incompatible with chip type(%s)",
			p_sar->chip_name, aw_bin->header_info[0].chip_type);

	p_sar->load_bin.bin_data_ver = aw_bin->header_info[0].bin_data_ver;

	ret = aw_sar_load_reg(aw_bin, p_sar->i2c);

	if (!strncmp(p_sar->chip_name, AW96308, sizeof(AW96308)) ||
		!strncmp(p_sar->chip_name, AW96305BFOR, sizeof(AW96305BFOR))) {
		dev_dbg(p_sar->dev, "set cs%d as irq", aw963xx->irq_mux);
		aw963xx_set_cs_as_irq(p_sar, aw963xx->irq_mux);
	}

	return ret;
}

static void aw963xx_irq_handle_func(uint32_t irq_status, void *data)
{
	struct aw_sar *p_sar = (struct aw_sar *)data;
	uint32_t ch_th[AW963XX_CHANNEL_NUM_MAX] = { 0 };
	uint32_t curr_status_val[4] = { 0 };
	int8_t i;
	int8_t j;

	if (((irq_status & 0x01) == 1) && (p_sar->driver_code_initover_flag == 1)) {
		dev_dbg(p_sar->dev, "not healthy!");
		p_sar->fault_flag = AW_SAR_UNHEALTHY;
	}

	for (i = 0; i < AW963XX_VALID_TH; i++)
		aw_sar_i2c_read(p_sar->i2c, REG_STAT0 + i * (REG_STAT1 - REG_STAT0),
				&curr_status_val[i]);

	for (j = 0; j < AW963XX_CHANNEL_NUM_MAX; j++) {
		if (!p_sar->channels_arr[j].input)
			continue;

		for (i = 0; i < AW963XX_VALID_TH; i++)
			ch_th[j] |= ((curr_status_val[i] >> j) & 0x01) << i;

		if (p_sar->channels_arr[j].last_channel_info != ch_th[j]) {
			if ((ch_th[j] >> 3 & 0x01) == 1) {	//th3
				input_report_abs(p_sar->channels_arr[j].input, ABS_DISTANCE, 4);
			} else if ((ch_th[j] >> 2 & 0x01) == 1) { //th2
				input_report_abs(p_sar->channels_arr[j].input, ABS_DISTANCE, 3);
			} else if ((ch_th[j] >> 1 & 0x01) == 1) { //th1
				input_report_abs(p_sar->channels_arr[j].input, ABS_DISTANCE, 2);
			} else if ((ch_th[j] >> 0 & 0x01) == 1) { //th0
				input_report_abs(p_sar->channels_arr[j].input, ABS_DISTANCE, 1);
			} else {	//far
				input_report_abs(p_sar->channels_arr[j].input, ABS_DISTANCE, 0);
			}
			input_sync(p_sar->channels_arr[j].input);
			p_sar->channels_arr[j].last_channel_info = ch_th[j];
		}
	}
}

static ssize_t aw963xx_operation_mode_get(void *data, char *buf)
{
	struct aw_sar *p_sar = (struct aw_sar *)data;
	ssize_t len = 0;

	if (p_sar->last_mode == AW963XX_ACTIVE_MODE)
		len += snprintf(buf + len, PAGE_SIZE - len, "operation mode: Active\n");
	else if (p_sar->last_mode == AW963XX_SLEEP_MODE)
		len += snprintf(buf + len, PAGE_SIZE - len, "operation mode: Sleep\n");
	else if (p_sar->last_mode == AW963XX_DEEPSLEEP_MODE)
		len += snprintf(buf + len, PAGE_SIZE - len, "operation mode: DeepSleep\n");
	else
		len += snprintf(buf + len, PAGE_SIZE - len, "operation mode: Unconfirmed\n");

	return len;
}

static void aw963xx_sar_chip_info_get(void *data, char *buf, ssize_t *p_len)
{
	struct aw_sar *p_sar = (struct aw_sar *)data;
	uint32_t reg_data;

	*p_len += snprintf(buf + *p_len, PAGE_SIZE - *p_len,
			"sar%u\n", p_sar->dts_info.sar_num);
	*p_len += snprintf(buf + *p_len, PAGE_SIZE - *p_len, "The driver supports UI\n");

	aw_sar_i2c_read(p_sar->i2c, REG_CHIP_ID0, &reg_data);
	*p_len += snprintf(buf + *p_len, PAGE_SIZE - *p_len, "chipid is 0x%08x\n", reg_data);

	aw_sar_i2c_read(p_sar->i2c, REG_IRQEN, &reg_data);
	*p_len += snprintf(buf + *p_len, PAGE_SIZE - *p_len, "REG_HOSTIRQEN is 0x%08x\n", reg_data);

	*p_len += snprintf(buf + *p_len, PAGE_SIZE - *p_len, "aw963xx Bin data version:0x%08x\n",
			p_sar->load_bin.bin_data_ver);
}

static int32_t aw963xx_get_signed_cap(void *data, uint16_t reg_addr)
{
	struct aw_sar *p_sar = (struct aw_sar *)data;
	int32_t s_ofst_c = 0;
	uint32_t off_m_bit;
	uint32_t off_c_bit;
	uint32_t reg_data;
	uint32_t off_c;
	uint32_t off_m;
	int32_t off_f;
	uint32_t i;

	aw_sar_i2c_read(p_sar->i2c, reg_addr, &reg_data);

	off_f = ((reg_data >> AW_BIT16) & ONE_WORD) * AW963XX_STEP_LEN_UNSIGNED_CAP_FINE_ADJ;
	off_c = (reg_data >> AW_BIT8) & ONE_WORD;
	off_m = reg_data & ONE_WORD;

	for (i = 0; i < 8; i++) {
		off_m_bit = (off_m >> i) & 0x01;
		off_c_bit = (off_c >> i) & 0x01;
		s_ofst_c += ((1 - 2 * off_m_bit) * off_c_bit * aw_sar_pow2(i)) *
			AW963XX_STEP_LEN_UNSIGNED_CAP_ROUGH_ADJ;
	}

	return (s_ofst_c + off_f);
}

static uint32_t aw963xx_get_unsigned_cap(void *data, uint16_t reg_addr)
{
	struct aw_sar *p_sar = (struct aw_sar *)data;
	uint32_t reg_data;
	uint32_t rough;
	uint32_t fine;

	aw_sar_i2c_read(p_sar->i2c, reg_addr, &reg_data);

	rough = ((reg_data >> AW_BIT8) & ONE_WORD) * AW963XX_STEP_LEN_UNSIGNED_CAP_ROUGH_ADJ;
	fine = ((reg_data >> AW_BIT16) & ONE_WORD) * AW963XX_STEP_LEN_UNSIGNED_CAP_FINE_ADJ;

	return (rough + fine);
}

static void aw963xx_get_ref_ch_enable(struct aw_sar *p_sar)
{
	struct aw963xx *aw963xx = (struct aw963xx *)p_sar->priv_data;
	uint32_t refa_ch;
	uint32_t refb_ch;
	uint32_t reg_data;
	int32_t i;

	for (i = 0; i < AW963XX_CHANNEL_NUM_MAX; i++) {
		aw_sar_i2c_read(p_sar->i2c,
				REG_FILTCTRL0_CH0 +
				i * (REG_FILTCTRL0_CH1 - REG_FILTCTRL0_CH0),
				&reg_data);
		if ((reg_data >> AW963XX_FILTCTRL0_CHX_REFAEN) & 0x01) {
			refa_ch = (reg_data >> AW963XX_FILTCTRL0_CHX_REFASEL) & 0x1f;
			aw963xx->ref_ch_en[refa_ch] = AW963XX_REF_EN;
		}
		if ((reg_data >> AW963XX_FILTCTRL0_CHX_REFBEN) & 0x01) {
			refb_ch = (reg_data >> AW963XX_FILTCTRL0_CHX_REFBSEL) & 0x1f;
			aw963xx->ref_ch_en[refb_ch] = AW963XX_REF_EN;
		}
	}
}

//Note: Because the kernel cannot handle floating-point types, it expands mul by 10 times
static uint8_t aw963xx_get_offset_multiple(struct aw_sar *p_sar, uint8_t ch)
{
	uint32_t reg_data;
	uint8_t mul = 1;

	aw_sar_i2c_read(p_sar->i2c, REG_AFECFG2_CH0 + ch * (REG_AFECFG2_CH1 - REG_AFECFG2_CH0),
			&reg_data);
	if ((reg_data >> 27) & 0x1) {
		if (((reg_data >> 29) & 0x3) == 0)
			mul = 16;
		else if (((reg_data >> 29) & 0x3) == 1)
			mul = 20;
		else if (((reg_data >> 29) & 0x3) == 2)
			mul = 26;
		else if (((reg_data >> 29) & 0x3) == 3)
			mul = 40;
		return mul;
	}

	aw_sar_i2c_read(p_sar->i2c, REG_AFECFG3_CH0 +
			ch * (REG_AFECFG3_CH1 - REG_AFECFG3_CH0),
			&reg_data);
	if ((reg_data >> 11) & 0x1)
		mul = 20;
	else
		mul = 10;

	return mul;
}

static ssize_t aw963xx_get_cap_offset(void *data, char *buf)
{
	struct aw_sar *p_sar = (struct aw_sar *)data;
	struct aw963xx *aw963xx = (struct aw963xx *)p_sar->priv_data;
	int32_t signed_cap_ofst;
	uint32_t mode = 0xff;
	uint32_t reg_data;
	uint32_t cap_ofst;
	uint8_t mul = 10;
	ssize_t len = 0;
	uint32_t tmp;
	uint32_t i;

	aw963xx_get_ref_ch_enable(p_sar);

	for (i = 0; i < AW963XX_CHANNEL_NUM_MAX; i++) {
		aw_sar_i2c_read(p_sar->i2c,
				REG_AFESOFTCFG0_CH0 +
				i * (REG_AFESOFTCFG0_CH1 - REG_AFESOFTCFG0_CH0),
				&reg_data);
		mul = aw963xx_get_offset_multiple(p_sar, i);
		mode = reg_data & 0x0ff;
		switch (mode) {
		case AW963XX_UNSIGNED_CAP:	//self-capacitance mode unsigned cail
			cap_ofst = aw963xx_get_unsigned_cap(p_sar,
				REG_AFECFG1_CH0 + i * (REG_AFECFG1_CH1 - REG_AFECFG1_CH0));
		//Because it has been expanded by 1000 times before,
		//the accuracy of removing mul's expansion loss can be ignored
			cap_ofst = cap_ofst * mul / 10;
			len += snprintf(buf + len, PAGE_SIZE - len,
					"unsigned cap ofst ch%u: %u.%u pf\r\n",
					i,
					cap_ofst / AW963XX_STEP_LEN_UNSIGNED_CAP_ENLARGE,
					cap_ofst % AW963XX_STEP_LEN_UNSIGNED_CAP_ENLARGE);
			break;
		case AW963XX_SIGNED_CAP:	//self-capacitance mode signed cail
			signed_cap_ofst = aw963xx_get_signed_cap(p_sar,
					REG_AFECFG1_CH0 + i * (REG_AFECFG1_CH1 - REG_AFECFG1_CH0));
			signed_cap_ofst = signed_cap_ofst * mul / 10;
			if (signed_cap_ofst < 0) {
				tmp = -signed_cap_ofst;
				dev_info(p_sar->dev, "cap_ofst2 = 0x%x", signed_cap_ofst);
				len += snprintf(buf + len, PAGE_SIZE - len,
						"signed cap ofst ch%u: -%u.%upf\r\n",
						i,
						tmp / AW963XX_STEP_LEN_UNSIGNED_CAP_ENLARGE,
						tmp % AW963XX_STEP_LEN_UNSIGNED_CAP_ENLARGE);
			} else {
				len += snprintf(buf + len, PAGE_SIZE - len,
						"signed cap ofst ch%u: %d.%dpf\r\n",
						i,
						signed_cap_ofst /
						AW963XX_STEP_LEN_UNSIGNED_CAP_ENLARGE,
						signed_cap_ofst %
						AW963XX_STEP_LEN_UNSIGNED_CAP_ENLARGE);
			}
			break;
		case AW963XX_MUTUAL_CAP:	//mutual-capacitance mode
			if (aw963xx->ref_ch_en[i] == AW963XX_REF_EN) {
				cap_ofst = aw963xx_get_unsigned_cap(p_sar,
					REG_AFECFG1_M_CH0 +
					i * (REG_AFECFG1_M_CH1 - REG_AFECFG1_M_CH0));
				cap_ofst = cap_ofst * mul / 10;
				len += snprintf(buf + len, PAGE_SIZE - len,
						"ref unsigned cap ofst ch%u: %u.%udpf\r\n",
						i,
						cap_ofst / AW963XX_STEP_LEN_UNSIGNED_CAP_ENLARGE,
						cap_ofst % AW963XX_STEP_LEN_UNSIGNED_CAP_ENLARGE);
			} else {
				signed_cap_ofst = aw963xx_get_signed_cap(p_sar,
							REG_AFECFG1_CH0 +
							i * (REG_AFECFG1_CH1 - REG_AFECFG1_CH0));
				signed_cap_ofst = signed_cap_ofst * mul / 10;
				if (signed_cap_ofst < 0) {
					tmp = -signed_cap_ofst;
					dev_info(p_sar->dev, "cap_ofst2 = 0x%x", signed_cap_ofst);
					len += snprintf(buf + len, PAGE_SIZE - len,
							"mutual cap ofst ch%u: -%u.%udpf\r\n",
							i,
							tmp / AW963XX_STEP_LEN_UNSIGNED_CAP_ENLARGE,
							tmp % AW963XX_STEP_LEN_UNSIGNED_CAP_ENLARGE);
				} else {
					len += snprintf(buf + len, PAGE_SIZE - len,
							"mutual cap ofst ch%u: %d.%dpf\r\n",
							i,
							signed_cap_ofst /
							AW963XX_STEP_LEN_UNSIGNED_CAP_ENLARGE,
							signed_cap_ofst %
							AW963XX_STEP_LEN_UNSIGNED_CAP_ENLARGE);
				}
			}
			break;
		default:
			dev_info(p_sar->dev, "aw963xx ofst error 0x%x", reg_data & 0x0f);
			break;
		}
	}

	return len;
}

static void aw963xx_set_cs_as_irq(struct aw_sar *p_sar, int flag)
{
	if (flag == AW963XX_CS2_IRQ) {
		aw_sar_i2c_write(p_sar->i2c, 0xfff4, 0x3c00d013);
		aw_sar_i2c_write(p_sar->i2c, 0xc100, 0x00000020);
		aw_sar_i2c_write(p_sar->i2c, 0xe018, 0x00000004);
	} else if (flag == AW963XX_CS5_IRQ) {
		aw_sar_i2c_write(p_sar->i2c, 0xfff4, 0x3c00d013);
		aw_sar_i2c_write(p_sar->i2c, 0xc100, 0x00000800);
		aw_sar_i2c_write(p_sar->i2c, 0xe018, 0x00000020);
	} else {
		aw_sar_i2c_write(p_sar->i2c, 0xfff4, 0x3c00d013);
		aw_sar_i2c_write(p_sar->i2c, 0xc100, 0x00000000);
		aw_sar_i2c_write(p_sar->i2c, 0xe018, 0x00000000);
	}
}

int32_t aw963xx_check_chipid(void *data)
{
	struct aw_sar *p_sar = (struct aw_sar *)data;
	uint32_t reg_val;
	int32_t ret;

	if (!p_sar)
		return -EINVAL;

	ret = aw_sar_i2c_read(p_sar->i2c, REG_CHIP_ID0, &reg_val);
	if (ret < 0) {
		dev_err(p_sar->dev, "read CHIP ID failed: %d", ret);
		return ret;
	}

	switch (reg_val) {
	case AW96303_CHIP_ID:
		dev_info(p_sar->dev, "aw96303 detected, 0x%04x", reg_val);
		memcpy(p_sar->chip_name, AW96303, 8);
		ret = 0;
		break;
	case AW96305_CHIP_ID:
		dev_info(p_sar->dev, "aw96305 detected, 0x%04x", reg_val);
		memcpy(p_sar->chip_name, AW96305, 8);
		ret = 0;
		break;
	case AW96305BFOR_CHIP_ID:
		dev_info(p_sar->dev, "aw96305bfor detected, 0x%04x", reg_val);
		memcpy(p_sar->chip_name, AW96305BFOR, 8);
		ret = 0;
		break;
	case AW96308_CHIP_ID:
		dev_info(p_sar->dev, "aw96308 detected, 0x%04x", reg_val);
		memcpy(p_sar->chip_name, AW96308, 8);
		ret = 0;
		break;
	case AW96310_CHIP_ID:
		dev_info(p_sar->dev, "aw96310 detected, 0x%04x", reg_val);
		memcpy(p_sar->chip_name, AW96310, 8);
		ret = 0;
		break;
	default:
		dev_info(p_sar->dev, "chip id error, 0x%04x", reg_val);
		ret =  -EIO;
		break;
	}

	return ret;
}

/**********************mode operation start*******************************/
static void aw963xx_enable_clock(void *i2c)
{
	aw_sar_i2c_write_bits(i2c, REG_CHIPSTAT, ~AW963XX_CPU_OSC_CTRL_MASK,
			AW963XX_CPU_OSC_CTRL_MASK);
}

static uint32_t aw963xx_rc_irqscr(void *i2c)
{
	uint32_t val;

	aw_sar_i2c_read(i2c, REG_IRQSRC, &val);

	return val;
}

static void aw963xx_set_active_cmd(void *i2c)
{
	aw_sar_i2c_write(i2c, REG_CMD, AW963XX_ACTIVE_MODE);
}

static void aw963xx_set_sleep_cmd(void *i2c)
{
	aw_sar_i2c_write(i2c, REG_CMD, AW963XX_SLEEP_MODE);
}

static void aw963xx_set_deepsleep_cmd(void *i2c)
{
	aw_sar_i2c_write(i2c, REG_CMD, AW963XX_DEEPSLEEP_MODE);
}

static const struct aw_sar_mode_set_t g_aw963xx_mode_set[] = {
	{
		.chip_id = AW_SAR_NONE_CHECK_CHIP,
		.chip_mode = {
			.curr_mode = AW963XX_ACTIVE_MODE,
			.last_mode = AW963XX_DEEPSLEEP_MODE,
		},
		.mode_switch_ops = {
			.enable_clock = aw963xx_enable_clock,
			.rc_irqscr = NULL,
			.mode_update = aw963xx_set_active_cmd,
		},
	},
	{
		.chip_id = AW_SAR_NONE_CHECK_CHIP,
		.chip_mode = {
			.curr_mode = AW963XX_ACTIVE_MODE,
			.last_mode = AW963XX_SLEEP_MODE,
		},
		.mode_switch_ops = {
			.enable_clock = NULL,
			.rc_irqscr = NULL,
			.mode_update = aw963xx_set_active_cmd,
		},
	},
	{
		.chip_id = AW_SAR_NONE_CHECK_CHIP,
		.chip_mode = {
			.curr_mode = AW963XX_ACTIVE_MODE,
			.last_mode = AW963XX_ACTIVE_MODE,
		},
		.mode_switch_ops = {
			.enable_clock = NULL,
			.rc_irqscr = NULL,
			.mode_update = aw963xx_set_active_cmd,
		},
	},
	{
		.chip_id = AW_SAR_NONE_CHECK_CHIP,
		.chip_mode = {
			.curr_mode = AW963XX_SLEEP_MODE,
			.last_mode = AW963XX_DEEPSLEEP_MODE,
		},
		.mode_switch_ops = {
			.enable_clock = aw963xx_enable_clock,
			.rc_irqscr = aw963xx_rc_irqscr,
			.mode_update = aw963xx_set_sleep_cmd,
		},
	},
	{
		.chip_id = AW_SAR_NONE_CHECK_CHIP,
		.chip_mode = {
			.curr_mode = AW963XX_SLEEP_MODE,
			.last_mode = AW963XX_ACTIVE_MODE,
		},
		.mode_switch_ops = {
			.enable_clock = NULL,
			.rc_irqscr = aw963xx_rc_irqscr,
			.mode_update = aw963xx_set_sleep_cmd,
		},
	},
	{
		.chip_id = AW_SAR_NONE_CHECK_CHIP,
		.chip_mode = {
			.curr_mode = AW963XX_DEEPSLEEP_MODE,
			.last_mode = AW963XX_SLEEP_MODE,
		},
		.mode_switch_ops = {
			.enable_clock = NULL,
			.rc_irqscr = aw963xx_rc_irqscr,
			.mode_update = aw963xx_set_deepsleep_cmd,
		},
	},
	{
		.chip_id = AW_SAR_NONE_CHECK_CHIP,
		.chip_mode = {
			.curr_mode = AW963XX_DEEPSLEEP_MODE,
			.last_mode = AW963XX_ACTIVE_MODE,
		},
		.mode_switch_ops = {
			.enable_clock = NULL,
			.rc_irqscr = aw963xx_rc_irqscr,
			.mode_update = aw963xx_set_deepsleep_cmd,
		},
	},
};

static void aw963xx_sar_get_firmware_info(void *data, char *buf, ssize_t *p_len)
{
	struct aw_sar *p_sar = (struct aw_sar *)data;
	uint32_t reg_data;

	aw_sar_i2c_read(p_sar->i2c, REG_FWVER, &reg_data);
	*p_len += snprintf(buf + *p_len, PAGE_SIZE - *p_len, "firmware is 0x%08x\n", reg_data);
}

static int32_t aw963xx_parse_dts(void *data)
{
	struct aw_sar *p_sar = (struct aw_sar *)data;
	struct aw963xx *aw963xx = (struct aw963xx *)p_sar->priv_data;
	struct device_node *np = p_sar->i2c->dev.of_node;
	int32_t val;

	val = of_property_read_u32(np, "awinic,irq-mux", &aw963xx->irq_mux);
	if (val != 0)
		dev_err(p_sar->dev, "irq-mux not detected");
	else
		dev_info(p_sar->dev, "irq-mux =  %d", aw963xx->irq_mux);

	val = of_property_read_u32(np, "awinic,start-mode", &aw963xx->start_mode);
	if (val != 0)
		dev_err(p_sar->dev, "start-mode not detected");
	else
		dev_info(p_sar->dev, "start-mode =  %d", aw963xx->start_mode);

	return 0;
}

static const struct aw_sar_mode_t g_aw963xx_mode = {
	.mode_set_arr = &g_aw963xx_mode_set[0],
	.mode_set_arr_len = ARRAY_SIZE(g_aw963xx_mode_set),
	.p_set_mode_node_fn = NULL,
	.p_get_mode_node_fn = aw963xx_operation_mode_get,
};

static const struct aw_sar_diff_t g_aw963xx_diff = {
	.diff0_reg = REG_DIFF_CH0,
	.diff_step = REG_DIFF_CH1 - REG_DIFF_CH0,
	.rm_float = AW963XX_DATA_PROCESS_FACTOR,
	.p_get_diff_node_fn = NULL,
};

static const struct aw_sar_offset_t g_aw963xx_offset = {
	.p_get_offset_node_fn = aw963xx_get_cap_offset,
};

static const struct aw_sar_aot_t g_aw963xx_aot = {
	.aot_reg = REG_SCANCTRL1,
	.aot_mask = ~0xfff,
	.aot_flag = 0xfff,
};

static const struct aw_sar_para_load_t g_aw963xx_reg_arr_para = {
	.reg_arr = aw963xx_reg_default,
	.reg_arr_len = ARRAY_SIZE(aw963xx_reg_default),
};

static const struct aw_sar_regulator_config_t g_regulator_config = {
	.vcc_name = "vcc",
	.min_uV = AW963XX_SAR_VCC_MIN_UV,
	.max_uV = AW963XX_SAR_VCC_MAX_UV,
};

static const struct aw_sar_reg_list_t g_aw963xx_reg_list = {
	.reg_none_access = REG_NONE_ACCESS,
	.reg_rd_access = REG_RD_ACCESS,
	.reg_wd_access = REG_WR_ACCESS,
	.reg_perm = (struct aw_sar_reg_data *)&g_aw963xx_reg_access[0],
	.reg_num = ARRAY_SIZE(g_aw963xx_reg_access),
};

static const struct aw_sar_chip_mode_t g_aw963xx_chip_mode = {
	.init_mode = AW963XX_ACTIVE_MODE,
	.active = AW963XX_ACTIVE_MODE,
	.pre_init_mode = AW963XX_SLEEP_MODE,
};

static const struct aw_sar_load_bin_t g_aw963xx_load_reg_bin = {
	.bin_name = "aw963xx_reg",
	.bin_opera_func = aw963xx_load_reg_bin,
	.p_update_fn = NULL,
};

static const struct aw_sar_load_bin_t g_aw963xx_load_fw_bin = {
	.bin_name = "aw963xx_fw",
	.bin_opera_func = aw963xx_update_firmware,
	.p_get_prot_update_fw_node_fn = aw963xx_sar_get_firmware_info,
	.bin_load_fail_opera = NULL,
};

static const struct aw_sar_get_chip_info_t g_aw963xx_get_chip_info = {
	.p_get_chip_info_node_fn = aw963xx_sar_chip_info_get,
};

static const struct aw_sar_check_chipid_t g_aw963xx_check_chipid = {
	.p_check_chipid_fn = aw963xx_check_chipid,
};

static const struct aw_sar_irq_init_t g_aw963xx_irq_init = {
	.flags = GPIOF_DIR_IN | GPIOF_INIT_HIGH,
	.irq_flags = IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
	.handler = NULL,
	.thread_fn = NULL,
	.rc_irq_fn = aw963xx_rc_irqscr,
	.irq_spec_handler_fn = aw963xx_irq_handle_func,

	.p_irq_init_fn = NULL,
	.p_irq_deinit_fn = NULL,
};

static const struct aw_sar_soft_rst_t g_aw963xx_soft_rst = {
	.reg_rst = REG_SA_RSTNALL,
	.reg_rst_val = AW963XX_SOFT_RST_EN,
	.delay_ms = AW963XX_CHIP_INIT_MAX_TIME_MS,
	.p_soft_reset_fn = NULL,
};

static const struct aw_sar_init_over_irq_t g_aw963xx_init_over_irq = {
	.wait_times = 100,
	.daley_step = 1,
	.reg_irqsrc = REG_IRQSRC,
	.irq_offset_bit = 0,
	.irq_mask = 0x1,
	.irq_flag = 0x1,

	.p_check_init_over_irq_fn = NULL,
	.p_get_err_type_fn = NULL,
};

static const struct aw_sar_pm_t g_aw963xx_pm_chip_mode = {
	.suspend_set_mode = AW963XX_SLEEP_MODE,
	.resume_set_mode = AW963XX_ACTIVE_MODE,
	.shutdown_set_mode = AW963XX_SLEEP_MODE,
};

static const struct aw_sar_platform_config g_aw963xx_platform_config = {
	.p_add_parse_dts_fn = &aw963xx_parse_dts,
	.p_regulator_config = &g_regulator_config,
	.p_irq_init = &g_aw963xx_irq_init,
	.p_pm_chip_mode = &g_aw963xx_pm_chip_mode,
};

static void aw963xx_power_on_prox_detection(void *data, uint8_t en_flag)
{

}

static const struct aw_sar_power_on_prox_detection_t g_aw933xx_power_on_prox_detection = {
	.p_power_on_prox_detection_en_fn = aw963xx_power_on_prox_detection,
	.irq_en_cali_bit = 3,
	.power_on_prox_en_flag = true,
};

static const struct aw_sar_chip_config g_aw963xx_chip_config = {
	.ch_num_max = AW963XX_CHANNEL_NUM_MAX,
	.p_platform_config = &g_aw963xx_platform_config,

	.p_check_chipid = &g_aw963xx_check_chipid,
	.p_soft_rst = &g_aw963xx_soft_rst,
	.p_init_over_irq = &g_aw963xx_init_over_irq,
	.p_fw_bin = &g_aw963xx_load_fw_bin,
	.p_reg_bin = &g_aw963xx_load_reg_bin,
	.p_chip_mode = &g_aw963xx_chip_mode,

	//Node usage parameters
	.p_reg_list = &g_aw963xx_reg_list,
	.p_reg_arr = &g_aw963xx_reg_arr_para,
	.p_aot = &g_aw963xx_aot,
	.p_diff = &g_aw963xx_diff,
	.p_offset = &g_aw963xx_offset,
	.p_mode = &g_aw963xx_mode,
	.p_prox_fw = &g_aw963xx_load_fw_bin,
	.p_get_chip_info = &g_aw963xx_get_chip_info,
	.p_aw_sar_awrw = NULL,
	.p_boot_bin = NULL,

	.p_other_operation = NULL,
	.p_other_opera_free = NULL,

	.power_on_prox_detection = &g_aw933xx_power_on_prox_detection,
};

int32_t aw963xx_init(struct aw_sar *p_sar)
{
	if (!p_sar)
		return -EINVAL;

	p_sar->priv_data = devm_kzalloc(p_sar->dev, sizeof(struct aw963xx), GFP_KERNEL);
	if (!p_sar->priv_data)
		return -ENOMEM;

	//Chip private function operation
	p_sar->p_sar_para = &g_aw963xx_chip_config;

	return 0;
}

void aw963xx_deinit(struct aw_sar *p_sar)
{
	if ((!p_sar) || (!p_sar->priv_data))
		return;

	if (p_sar->priv_data != NULL)
		devm_kfree(p_sar->dev, p_sar->priv_data);
}
