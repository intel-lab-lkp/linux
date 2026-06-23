// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2026 Semiconductor Components Industries, LLC ("onsemi").
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/unaligned.h>

#include "pmbus.h"

enum chips { chip_fd5121, chip_fd5123, chip_fd5125 };

#define CTLR_ID_UNKNOWN				0
#define CTLR_ID_FD5121				0xFD5121
#define CTLR_ID_FD5123				0xFD5123
#define CTLR_ID_FD5125				0xFD5125

#define FD5121_NUM_PAGES			2

/* Custom PMBUS commands */
#define PMBUS_REG_VOUT_MIN			0x2B
#define PMBUS_REG_POWER_MODE			0x34
#define PMBUS_REG_VIN_ON			0x35
#define PMBUS_REG_VIN_OFF			0x36
#define PMBUS_REG_VIN_UV_FAULT_RESPONSE		0x5A
#define PMBUS_REG_IIN_OC_FAULT_RESPONSE		0x5C
#define PMBUS_REG_TON_DELAY			0x60
#define PMBUS_REG_POUT_OP_FAULT_RESPONSE	0x69
#define PMBUS_REG_READ_VAUX			0x85

#define PMBUS_REG_IKNEE_SET			0x2D
#define PMBUS_REG_PIN_COUNTER			0x2E
#define PMBUS_REG_VMIN_AWARE			0x2F
#define PMBUS_REG_VAUX_UV_FAULT_LIMIT		0x6C
#define PMBUS_REG_VAUX_OV_FAULT_LIMIT		0x6D
#define PMBUS_REG_VAUX_UV_FAULT_RESPONSE	0x6E
#define PMBUS_REG_VAUX_OV_FAULT_RESPONSE	0x6F
#define PMBUS_REG_VAUX_UV_WARNING		0x75
#define PMBUS_REG_VAUX_OV_WARNING		0x76
#define PMBUS_REG_MFR_FREE_USER_CONFIG_TABLES	0xCF
#define PMBUS_REG_MFR_ADDRESS_TABLE		0xD0
#define PMBUS_REG_MFR_STATUS_ONSEMI		0xD1
#define PMBUS_REG_MFR_UNLOCK			0xD2
#define PMBUS_REG_MFR_FAULTY_SPS		0xD3
#define PMBUS_REG_TLVR_FAULTS			0xD4
#define PMBUS_REG_MFR_USER_STORE_CONFIG_TAB	0xD5
#define PMBUS_REG_MFR_USER_CONFIG_INDEX		0xD6
#define PMBUS_REG_MFR_PWM_DISCONNECTION		0xD7
#define PMBUS_REG_MFR_VR_DISCONNECTION		0xD8
#define PMBUS_REG_MFR_TON_SLEW			0xD9
#define PMBUS_REG_MFR_TOFF_SLEW			0xDA
#define PMBUS_REG_MFR_RAIL_NAME			0xDB
#define PMBUS_REG_MFR_VOUT_DROOP		0xDC
#define PMBUS_REG_MFR_USER_RESTORE_CONFIG_TAB	0xDD
#define PMBUS_REG_MFR_SVR_GO			0xDE
#define PMBUS_REG_MFR_SET_PWD			0xDF
#define PMBUS_REG_MFR_CONFIG_ACTIVATE		0xE0
#define PMBUS_REG_MFR_CONFIG_RECOVER		0xE1
#define PMBUS_REG_MFR_OTP_DUMP			0xE2
#define PMBUS_REG_MFR_BBR_EN			0xE3
#define PMBUS_REG_MFR_DPM_MIN			0xE4
#define PMBUS_REG_MFR_VBOOT			0xE5
#define PMBUS_REG_MFR_PRECLAMP_OFFSET		0xE6
#define PMBUS_REG_MFR_TLVR_DIAGN		0xE7
#define PMBUS_REG_MFR_READ_VSYS			0xE8
#define PMBUS_REG_MFR_SPECIFIC_E9		0xE9
#define PMBUS_REG_MFR_SPECIFIC_EA		0xEA
#define PMBUS_REG_MFR_SS_CBC			0xEB
#define PMBUS_REG_MFR_AMD_STATUS		0xEC
#define PMBUS_REG_MFR_CHECKSUM			0xEE
#define PMBUS_REG_CSE_INDEX			0xF0
#define PMBUS_REG_COUT_MEASURE			0xF1
#define PMBUS_REG_VR_COUT			0xF2
#define PMBUS_REG_BBR_RAM			0xF3
#define PMBUS_REG_BBR_OTP			0xF4
#define PMBUS_REG_READ_PSYS			0xF5
#define PMBUS_REG_POSTCLAMP_OFFSET		0xF6
#define PMBUS_REG_PGOOD_DELAY			0xF7
#define PMBUS_REG_MFR_SPECIFIC_F8		0xF8
#define PMBUS_REG_MFR_SPECIFIC_F9		0xF9
#define PMBUS_REG_MFR_PWD_PROGRAM_RAM		0xFA
#define PMBUS_REG_MFR_PWD_PROGRAM_I2C		0xFB
#define PMBUS_REG_MFR_PWD_ENABLE_OTP_STORE	0xFC

/* List of recognized commands */
static const u8 cc_list[] = {
	PMBUS_PAGE,
	PMBUS_OPERATION,
	PMBUS_ON_OFF_CONFIG,
	PMBUS_CLEAR_FAULTS,
	PMBUS_WRITE_PROTECT,
	PMBUS_CAPABILITY,
	PMBUS_VOUT_MODE,
	PMBUS_VOUT_COMMAND,
	PMBUS_VOUT_MAX,
	PMBUS_VOUT_MARGIN_HIGH,
	PMBUS_VOUT_MARGIN_LOW,
	PMBUS_VOUT_TRANSITION_RATE,
	PMBUS_REG_VOUT_MIN,
	PMBUS_REG_IKNEE_SET,
	PMBUS_REG_PIN_COUNTER,
	PMBUS_REG_VMIN_AWARE,
	PMBUS_REG_POWER_MODE,
	PMBUS_REG_VIN_ON,
	PMBUS_REG_VIN_OFF,
	PMBUS_VOUT_OV_FAULT_LIMIT,
	PMBUS_VOUT_OV_FAULT_RESPONSE,
	PMBUS_VOUT_UV_FAULT_LIMIT,
	PMBUS_VOUT_UV_FAULT_RESPONSE,
	PMBUS_IOUT_OC_FAULT_LIMIT,
	PMBUS_IOUT_OC_FAULT_RESPONSE,
	PMBUS_IOUT_OC_WARN_LIMIT,
	PMBUS_OT_FAULT_LIMIT,
	PMBUS_OT_FAULT_RESPONSE,
	PMBUS_OT_WARN_LIMIT,
	PMBUS_VIN_OV_FAULT_LIMIT,
	PMBUS_VIN_OV_FAULT_RESPONSE,
	PMBUS_VIN_OV_WARN_LIMIT,
	PMBUS_VIN_UV_WARN_LIMIT,
	PMBUS_VIN_UV_FAULT_LIMIT,
	PMBUS_REG_VIN_UV_FAULT_RESPONSE,
	PMBUS_IIN_OC_FAULT_LIMIT,
	PMBUS_REG_IIN_OC_FAULT_RESPONSE,
	PMBUS_IIN_OC_WARN_LIMIT,
	PMBUS_REG_TON_DELAY,
	PMBUS_POUT_OP_FAULT_LIMIT,
	PMBUS_REG_POUT_OP_FAULT_RESPONSE,
	PMBUS_POUT_OP_WARN_LIMIT,
	PMBUS_PIN_OP_WARN_LIMIT,
	PMBUS_REG_VAUX_UV_FAULT_LIMIT,
	PMBUS_REG_VAUX_OV_FAULT_LIMIT,
	PMBUS_REG_VAUX_UV_FAULT_RESPONSE,
	PMBUS_REG_VAUX_OV_FAULT_RESPONSE,
	PMBUS_REG_VAUX_UV_WARNING,
	PMBUS_REG_VAUX_OV_WARNING,
	PMBUS_STATUS_BYTE,
	PMBUS_STATUS_WORD,
	PMBUS_STATUS_VOUT,
	PMBUS_STATUS_IOUT,
	PMBUS_STATUS_INPUT,
	PMBUS_STATUS_TEMPERATURE,
	PMBUS_STATUS_CML,
	PMBUS_STATUS_OTHER,
	PMBUS_STATUS_MFR_SPECIFIC,
	PMBUS_REG_READ_VAUX,
	PMBUS_READ_VIN,
	PMBUS_READ_IIN,
	PMBUS_READ_VOUT,
	PMBUS_READ_IOUT,
	PMBUS_READ_TEMPERATURE_1,
	PMBUS_READ_POUT,
	PMBUS_READ_PIN,
	PMBUS_REVISION,
	PMBUS_MFR_ID,
	PMBUS_MFR_MODEL,
	PMBUS_MFR_REVISION,
	PMBUS_IC_DEVICE_ID,
	PMBUS_REG_MFR_FREE_USER_CONFIG_TABLES,
	PMBUS_REG_MFR_ADDRESS_TABLE,
	PMBUS_REG_MFR_STATUS_ONSEMI,
	PMBUS_REG_MFR_UNLOCK,
	PMBUS_REG_MFR_FAULTY_SPS,
	PMBUS_REG_TLVR_FAULTS,
	PMBUS_REG_MFR_USER_STORE_CONFIG_TAB,
	PMBUS_REG_MFR_USER_CONFIG_INDEX,
	PMBUS_REG_MFR_PWM_DISCONNECTION,
	PMBUS_REG_MFR_VR_DISCONNECTION,
	PMBUS_REG_MFR_TON_SLEW,
	PMBUS_REG_MFR_TOFF_SLEW,
	PMBUS_REG_MFR_RAIL_NAME,
	PMBUS_REG_MFR_VOUT_DROOP,
	PMBUS_REG_MFR_USER_RESTORE_CONFIG_TAB,
	PMBUS_REG_MFR_SVR_GO,
	PMBUS_REG_MFR_SET_PWD,
	PMBUS_REG_MFR_CONFIG_ACTIVATE,
	PMBUS_REG_MFR_CONFIG_RECOVER,
	PMBUS_REG_MFR_OTP_DUMP,
	PMBUS_REG_MFR_BBR_EN,
	PMBUS_REG_MFR_DPM_MIN,
	PMBUS_REG_MFR_VBOOT,
	PMBUS_REG_MFR_PRECLAMP_OFFSET,
	PMBUS_REG_MFR_TLVR_DIAGN,
	PMBUS_REG_MFR_READ_VSYS,
	PMBUS_REG_MFR_SPECIFIC_E9,
	PMBUS_REG_MFR_SPECIFIC_EA,
	PMBUS_REG_MFR_SS_CBC,
	PMBUS_REG_MFR_AMD_STATUS,
	PMBUS_REG_MFR_CHECKSUM,
	PMBUS_REG_CSE_INDEX,
	PMBUS_REG_COUT_MEASURE,
	PMBUS_REG_VR_COUT,
	PMBUS_REG_BBR_RAM,
	PMBUS_REG_BBR_OTP,
	PMBUS_REG_READ_PSYS,
	PMBUS_REG_POSTCLAMP_OFFSET,
	PMBUS_REG_PGOOD_DELAY,
	PMBUS_REG_MFR_SPECIFIC_F8,
	PMBUS_REG_MFR_SPECIFIC_F9,
	PMBUS_REG_MFR_PWD_PROGRAM_RAM,
	PMBUS_REG_MFR_PWD_PROGRAM_I2C,
	PMBUS_REG_MFR_PWD_ENABLE_OTP_STORE,
};

/* Following registers expect block read */
static const u8 blk_rd_cc[] = {
	PMBUS_SMBALERT_MASK,
	PMBUS_MFR_DATE,
	PMBUS_IC_DEVICE_REV,
};

struct fd5121_data {
	struct attribute_group *groups[3];
	struct pmbus_driver_info info;
	struct device *dev;
	u32 id;
};

static s32 fd5121_read_block_data(const struct i2c_client *client,
				  u8 cmd_code, u8 len, u8 *pbuf)
{
	s32 ret = 0;

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_READ_BLOCK_DATA)) {

		/* Payload length is in the first byte. */
		ret = i2c_smbus_read_i2c_block_data(client, cmd_code,
						    len, pbuf);
		if (ret < 0)
			return ret;
		ret = pbuf[0];
		if (ret > len)
			ret = len;
		for (int idx = 0; idx < ret; idx++)
			pbuf[idx] = pbuf[idx + 1];
		return ret;
	}
	ret = i2c_smbus_read_block_data(client, cmd_code, pbuf);
	if (ret < 0)
		return ret;
	return min_t(s32, ret, len);
}

/* Command code that expects block read, not word read */
static bool fd5121_blk_rd_reg(u8 cmd_code)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(blk_rd_cc); i++) {
		if (cmd_code == blk_rd_cc[i])
			return true;
	}
	return false;
}

static ssize_t fd5121_send_byte_store(struct device *dev,
				      struct device_attribute *da,
				      const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev->parent);
	u8 val = 0;
	int ret;

	ret = kstrtou8(buf, 10, &val);
	if (ret < 0)
		return ret;
	ret = i2c_smbus_write_byte(client, val);
	if (ret < 0)
		return ret;
	return count;
}

static int fd5121_config_activate(struct i2c_client *client)
{
	return i2c_smbus_write_byte_data(client,
					 PMBUS_REG_MFR_CONFIG_ACTIVATE, 0xAA);
}

static ssize_t fd5121_byte_store(struct device *dev,
				 struct device_attribute *da,
				 const char *buf, size_t count)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct i2c_client *client = to_i2c_client(dev->parent);
	u8 reg = attr->index;
	int ret = 0;
	u8 val = 0;

	switch (reg) {
	case PMBUS_REG_MFR_CONFIG_ACTIVATE:
		ret = fd5121_config_activate(client);
		if (ret < 0)
			return ret;
		return count;
	default:
		ret = kstrtou8(buf, 10, &val);
		if (ret < 0)
			return ret;
		break;
	}
	if (reg == PMBUS_PAGE && ((val != 0 && val != 1 &&
	    val != GENMASK(7, 0))))
		return -EINVAL;
	ret = i2c_smbus_write_byte_data(client, reg, val);
	if (ret < 0)
		return ret;
	return count;
}

static ssize_t fd5121_byte_show(struct device *dev,
				struct device_attribute *da, char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct i2c_client *client = to_i2c_client(dev->parent);
	u8 reg = attr->index;
	s32 ret;

	ret = i2c_smbus_read_byte_data(client, reg);
	if (ret < 0)
		return ret;
	return scnprintf(buf, PAGE_SIZE, "%d\n", ret & 0xFF);
}

static ssize_t fd5121_word_store(struct device *dev,
				 struct device_attribute *da,
				 const char *buf, size_t count)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct i2c_client *client = to_i2c_client(dev->parent);
	u8 reg = attr->index;
	s16 val = 0;
	int ret = 0;

	switch (reg) {
	case PMBUS_REG_MFR_PWD_PROGRAM_RAM:
		val = 0xC93F;
		break;
	default:
		ret = kstrtos16(buf, 10, &val);
		if (ret)
			return ret;
		break;
	}
	ret = i2c_smbus_write_word_data(client, reg, val);
	if (ret < 0)
		return ret;
	return count;
}

static ssize_t fd5121_word_show(struct device *dev,
				struct device_attribute *da, char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct i2c_client *client = to_i2c_client(dev->parent);
	u8 data[I2C_SMBUS_BLOCK_MAX] = { 0 };
	u8 reg = attr->index;
	s32 ret = 0;

	if (fd5121_blk_rd_reg(reg)) {
		ret = fd5121_read_block_data(client, reg, 2, data);
		if (ret >= 0)
			ret = get_unaligned_le16(data);
	} else
		ret = i2c_smbus_read_word_data(client, reg);
	if (ret < 0)
		return ret;
	return scnprintf(buf, PAGE_SIZE, "%d\n", ret & 0xFFFF);
}

static s32 fd5121_write_block_data(const struct i2c_client *client,
				   u8 cmd_code, u8 len, u8 *pbuf)
{
	s32 ret = 0;

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_WRITE_BLOCK_DATA))
		ret = i2c_smbus_write_i2c_block_data(client, cmd_code,
						     len, pbuf);
	else
		ret = i2c_smbus_write_block_data(client, cmd_code,
						 len, pbuf);
	return ret;
}

static s32 fd5121_read_long(struct i2c_client *client, u8 cmd_code, u32 *pval)
{
	u8 buffer[I2C_SMBUS_BLOCK_MAX] = { 0 };
	s32 ret;

	ret = fd5121_read_block_data(client, cmd_code, 4, buffer);
	if (ret < 0)
		return ret;
	if (ret < 4)
		return -EIO;

	*pval = get_unaligned_le32(buffer);
	return 0;
}

static ssize_t fd5121_long_store(struct device *dev,
				 struct device_attribute *da,
				 const char *buf, size_t count)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct i2c_client *client = to_i2c_client(dev->parent);
	u8 reg = attr->index;
	u8 buffer[4];
	u32 val = 0;
	int ret = 0;
	u8 len;

	ret = kstrtou32(buf, 10, &val);
	if (ret)
		return ret;

	len = (u8) sizeof(buffer);
	for (u8 i = 0; i < len; i++) {
		buffer[i] = val & 0xFF;
		val >>= 8;
	}
	ret = fd5121_write_block_data(client, reg, len, buffer);
	if (ret < 0)
		return ret;
	return count;
}

static ssize_t fd5121_long_show(struct device *dev,
				struct device_attribute *da, char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	struct i2c_client *client = to_i2c_client(dev->parent);
	u8 reg = attr->index;
	u32 val = 0;
	s32 ret = 0;

	ret = fd5121_read_long(client, reg, &val);
	if (ret < 0)
		return ret;
	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t fd5121_block_show(struct device *dev,
				 struct device_attribute *da, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev->parent);
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	u8 buffer[I2C_SMBUS_BLOCK_MAX] = { 0 };
	u8 reg = attr->index;
	int printed = 0;
	s32 ret = 0;
	u8 len = 0;
	int i = 0;

	if (reg == PMBUS_REG_MFR_FAULTY_SPS) {
		int to_print = 0;

		len = 7;
		ret = fd5121_read_block_data(client, reg, len, buffer);
		if (ret < 0)
			return ret;
		printed = 0;
		to_print = (ret < len) ? ret : len;
		for (i = 0; i < to_print; i++)
			printed += scnprintf(buf + printed,
					     PAGE_SIZE - printed,
					     "%02x", buffer[i]);
		printed += scnprintf(buf + printed,
				     PAGE_SIZE - printed, "\n");
		return printed;
	} else if (reg == PMBUS_REG_BBR_RAM ||
		   reg == PMBUS_REG_BBR_OTP) {
		u32 len = (reg == PMBUS_REG_BBR_OTP) ? 165 : 164;

		/* Extra byte may be needed in case we need to store
		 * the length of the data
		 */
		u8 *tmp_in = kcalloc(len+1, sizeof(u8), GFP_KERNEL);

		if (tmp_in == NULL)
			return -ENOMEM;
		ret = fd5121_read_block_data(client, reg, len, tmp_in);
		if (ret < 0) {
			kfree(tmp_in);
			return ret;
		}

		printed = 0;
		for (i = 0; i < ret; i++)
			printed += scnprintf(buf + printed,
					     PAGE_SIZE - printed, "%02x",
					     tmp_in[i]);
		printed += scnprintf(buf + printed,
				     PAGE_SIZE - printed, "\n");

		kfree(tmp_in);
		return printed;
	} else
		return -ENODATA;
}

static SENSOR_DEVICE_ATTR_RW(page, fd5121_byte,
			     PMBUS_PAGE);
static SENSOR_DEVICE_ATTR_RO(vout_raw, fd5121_word,
			     PMBUS_READ_VOUT);
static SENSOR_DEVICE_ATTR_RW(operation, fd5121_byte,
			     PMBUS_OPERATION);
static SENSOR_DEVICE_ATTR_RW(on_off_config, fd5121_byte,
			     PMBUS_ON_OFF_CONFIG);
static SENSOR_DEVICE_ATTR_WO(clear_faults, fd5121_byte,
			     PMBUS_CLEAR_FAULTS);
static SENSOR_DEVICE_ATTR_RW(write_protect, fd5121_byte,
			     PMBUS_WRITE_PROTECT);
static SENSOR_DEVICE_ATTR_RO(capability, fd5121_byte,
			     PMBUS_CAPABILITY);
static SENSOR_DEVICE_ATTR_RW(smbalert_mask, fd5121_word,
			     PMBUS_SMBALERT_MASK);
static SENSOR_DEVICE_ATTR_RO(vout_mode, fd5121_byte,
			     PMBUS_VOUT_MODE);
static SENSOR_DEVICE_ATTR_RW(vout_command, fd5121_word,
			     PMBUS_VOUT_COMMAND);
static SENSOR_DEVICE_ATTR_RW(vout_max, fd5121_word,
			     PMBUS_VOUT_MAX);
static SENSOR_DEVICE_ATTR_RW(vout_margin_high, fd5121_word,
			     PMBUS_VOUT_MARGIN_HIGH);
static SENSOR_DEVICE_ATTR_RW(vout_margin_low, fd5121_word,
			     PMBUS_VOUT_MARGIN_LOW);
static SENSOR_DEVICE_ATTR_RW(vout_transition_rate, fd5121_word,
			     PMBUS_VOUT_TRANSITION_RATE);
static SENSOR_DEVICE_ATTR_RW(vout_min, fd5121_word,
			     PMBUS_REG_VOUT_MIN);
static SENSOR_DEVICE_ATTR_RW(power_mode, fd5121_byte,
			     PMBUS_REG_POWER_MODE);
static SENSOR_DEVICE_ATTR_RW(vin_on, fd5121_word,
			     PMBUS_REG_VIN_ON);
static SENSOR_DEVICE_ATTR_RW(vin_off, fd5121_word,
			     PMBUS_REG_VIN_OFF);
static SENSOR_DEVICE_ATTR_RW(vin_uv_fault_response, fd5121_byte,
			     PMBUS_REG_VIN_UV_FAULT_RESPONSE);
static SENSOR_DEVICE_ATTR_RW(iin_oc_fault_response, fd5121_byte,
			     PMBUS_REG_IIN_OC_FAULT_RESPONSE);
static SENSOR_DEVICE_ATTR_RW(ton_delay, fd5121_word,
			     PMBUS_REG_TON_DELAY);
static SENSOR_DEVICE_ATTR_RW(pout_op_fault_response, fd5121_byte,
			     PMBUS_REG_POUT_OP_FAULT_RESPONSE);
static SENSOR_DEVICE_ATTR_RO(read_vaux, fd5121_word,
			     PMBUS_REG_READ_VAUX);
static SENSOR_DEVICE_ATTR_RW(iknee_set, fd5121_word,
			     PMBUS_REG_IKNEE_SET);
static SENSOR_DEVICE_ATTR_RW(pin_counter, fd5121_byte,
			     PMBUS_REG_PIN_COUNTER);
static SENSOR_DEVICE_ATTR_RW(vmin_aware, fd5121_word,
			     PMBUS_REG_VMIN_AWARE);
static SENSOR_DEVICE_ATTR_RW(vout_ov_fault_response, fd5121_byte,
			     PMBUS_VOUT_OV_FAULT_RESPONSE);
static SENSOR_DEVICE_ATTR_RW(vout_uv_fault_response, fd5121_byte,
			     PMBUS_VOUT_UV_FAULT_RESPONSE);
static SENSOR_DEVICE_ATTR_RW(iout_oc_fault_response, fd5121_byte,
			     PMBUS_IOUT_OC_FAULT_RESPONSE);
static SENSOR_DEVICE_ATTR_RW(ot_fault_response, fd5121_byte,
			     PMBUS_OT_FAULT_RESPONSE);
static SENSOR_DEVICE_ATTR_RW(vin_ov_fault_response, fd5121_byte,
			     PMBUS_VIN_OV_FAULT_RESPONSE);
static SENSOR_DEVICE_ATTR_RW(vaux_uv_fault_limit, fd5121_word,
			     PMBUS_REG_VAUX_UV_FAULT_LIMIT);
static SENSOR_DEVICE_ATTR_RW(vaux_ov_fault_limit, fd5121_word,
			     PMBUS_REG_VAUX_OV_FAULT_LIMIT);
static SENSOR_DEVICE_ATTR_RW(vaux_uv_fault_response, fd5121_byte,
			     PMBUS_REG_VAUX_UV_FAULT_RESPONSE);
static SENSOR_DEVICE_ATTR_RW(vaux_ov_fault_response, fd5121_byte,
			     PMBUS_REG_VAUX_OV_FAULT_RESPONSE);
static SENSOR_DEVICE_ATTR_RW(vaux_uv_warning, fd5121_word,
			     PMBUS_REG_VAUX_UV_WARNING);
static SENSOR_DEVICE_ATTR_RW(vaux_ov_warning, fd5121_word,
			     PMBUS_REG_VAUX_OV_WARNING);
static SENSOR_DEVICE_ATTR_RO(free_user_config_tables, fd5121_byte,
			     PMBUS_REG_MFR_FREE_USER_CONFIG_TABLES);
static SENSOR_DEVICE_ATTR_RW(address_table, fd5121_byte,
			     PMBUS_REG_MFR_ADDRESS_TABLE);
static SENSOR_DEVICE_ATTR_RW(status_onsemi, fd5121_word,
			     PMBUS_REG_MFR_STATUS_ONSEMI);
static SENSOR_DEVICE_ATTR_RO(status_byte, fd5121_byte,
			     PMBUS_STATUS_BYTE);
static SENSOR_DEVICE_ATTR_RO(status_cml, fd5121_byte,
			     PMBUS_STATUS_CML);
static SENSOR_DEVICE_ATTR_RO(status_other, fd5121_byte,
			     PMBUS_STATUS_OTHER);
static SENSOR_DEVICE_ATTR_RO(status_mfr_specific, fd5121_byte,
			     PMBUS_STATUS_MFR_SPECIFIC);
static SENSOR_DEVICE_ATTR_RO(revision, fd5121_byte,
			     PMBUS_REVISION);
static SENSOR_DEVICE_ATTR_RO(id, fd5121_long,
			     PMBUS_MFR_ID);
static SENSOR_DEVICE_ATTR_RO(model, fd5121_long,
			     PMBUS_MFR_MODEL);
static SENSOR_DEVICE_ATTR_RO(mfr_revision, fd5121_long,
			     PMBUS_MFR_REVISION);
static SENSOR_DEVICE_ATTR_RW(date, fd5121_word,
			     PMBUS_MFR_DATE);
static SENSOR_DEVICE_ATTR_RO(ic_device_id, fd5121_long,
			     PMBUS_IC_DEVICE_ID);
static SENSOR_DEVICE_ATTR_RO(ic_device_rev, fd5121_word,
			     PMBUS_IC_DEVICE_REV);
static SENSOR_DEVICE_ATTR_WO(unlock, fd5121_byte,
			     PMBUS_REG_MFR_UNLOCK);
static SENSOR_DEVICE_ATTR_RO(faulty_sps, fd5121_block,
			     PMBUS_REG_MFR_FAULTY_SPS);
static SENSOR_DEVICE_ATTR_RO(tlvr_faults, fd5121_word,
			     PMBUS_REG_TLVR_FAULTS);
static SENSOR_DEVICE_ATTR_RW(user_store_config_tab, fd5121_byte,
			     PMBUS_REG_MFR_USER_STORE_CONFIG_TAB);
static SENSOR_DEVICE_ATTR_RO(user_config_index, fd5121_byte,
			     PMBUS_REG_MFR_USER_CONFIG_INDEX);
static SENSOR_DEVICE_ATTR_RO(pwm_disconnection, fd5121_word,
			     PMBUS_REG_MFR_PWM_DISCONNECTION);
static SENSOR_DEVICE_ATTR_RO(vr_disconnection, fd5121_byte,
			     PMBUS_REG_MFR_VR_DISCONNECTION);
static SENSOR_DEVICE_ATTR_RW(ton_slew, fd5121_byte,
			     PMBUS_REG_MFR_TON_SLEW);
static SENSOR_DEVICE_ATTR_RW(toff_slew, fd5121_byte,
			     PMBUS_REG_MFR_TOFF_SLEW);
static SENSOR_DEVICE_ATTR_RW(rail_name, fd5121_word,
			     PMBUS_REG_MFR_RAIL_NAME);
static SENSOR_DEVICE_ATTR_RW(vout_droop, fd5121_byte,
			     PMBUS_REG_MFR_VOUT_DROOP);
static SENSOR_DEVICE_ATTR_WO(svr_go, fd5121_send_byte,
			     PMBUS_REG_MFR_SVR_GO);
static SENSOR_DEVICE_ATTR_RW(user_restore_config_tab, fd5121_byte,
			     PMBUS_REG_MFR_USER_RESTORE_CONFIG_TAB);
static SENSOR_DEVICE_ATTR_WO(set_pwd, fd5121_byte,
			     PMBUS_REG_MFR_SET_PWD);
static SENSOR_DEVICE_ATTR_RW(config_activate, fd5121_byte,
			     PMBUS_REG_MFR_CONFIG_ACTIVATE);
static SENSOR_DEVICE_ATTR_RW(config_recover, fd5121_byte,
			     PMBUS_REG_MFR_CONFIG_RECOVER);
static SENSOR_DEVICE_ATTR_RW(otp_dump, fd5121_byte,
			     PMBUS_REG_MFR_OTP_DUMP);
static SENSOR_DEVICE_ATTR_RW(bbr_en, fd5121_byte,
			     PMBUS_REG_MFR_BBR_EN);
static SENSOR_DEVICE_ATTR_RW(dpm_min, fd5121_byte,
			     PMBUS_REG_MFR_DPM_MIN);
static SENSOR_DEVICE_ATTR_RW(vboot, fd5121_word,
			     PMBUS_REG_MFR_VBOOT);
static SENSOR_DEVICE_ATTR_RW(preclamp_offset, fd5121_word,
			     PMBUS_REG_MFR_PRECLAMP_OFFSET);
static SENSOR_DEVICE_ATTR_RW(tlvr_diagn, fd5121_word,
			     PMBUS_REG_MFR_TLVR_DIAGN);
static SENSOR_DEVICE_ATTR_RO(vsys, fd5121_word,
			     PMBUS_REG_MFR_READ_VSYS);
static SENSOR_DEVICE_ATTR_RW(specific_e9, fd5121_word,
			     PMBUS_REG_MFR_SPECIFIC_E9);
static SENSOR_DEVICE_ATTR_RW(specific_ea, fd5121_long,
			     PMBUS_REG_MFR_SPECIFIC_EA);
static SENSOR_DEVICE_ATTR_RO(ss_cbc, fd5121_word,
			     PMBUS_REG_MFR_SS_CBC);
static SENSOR_DEVICE_ATTR_RO(amd_status, fd5121_byte,
			     PMBUS_REG_MFR_AMD_STATUS);
static SENSOR_DEVICE_ATTR_RO(checksum, fd5121_word,
			     PMBUS_REG_MFR_CHECKSUM);
static SENSOR_DEVICE_ATTR_RO(cse_index, fd5121_word,
			     PMBUS_REG_CSE_INDEX);
static SENSOR_DEVICE_ATTR_RW(cout_measure, fd5121_word,
			     PMBUS_REG_COUT_MEASURE);
static SENSOR_DEVICE_ATTR_RO(vr_cout, fd5121_word,
			     PMBUS_REG_VR_COUT);
static SENSOR_DEVICE_ATTR_RO(bbr_ram, fd5121_block,
			     PMBUS_REG_BBR_RAM);
static SENSOR_DEVICE_ATTR_RO(bbr_otp, fd5121_block,
			     PMBUS_REG_BBR_OTP);
static SENSOR_DEVICE_ATTR_RO(psys, fd5121_word,
			     PMBUS_REG_READ_PSYS);
static SENSOR_DEVICE_ATTR_RW(postclamp_offset, fd5121_word,
			     PMBUS_REG_POSTCLAMP_OFFSET);
static SENSOR_DEVICE_ATTR_RW(pgood_delay, fd5121_byte,
			     PMBUS_REG_PGOOD_DELAY);
static SENSOR_DEVICE_ATTR_RW(specific_f8, fd5121_word,
			     PMBUS_REG_MFR_SPECIFIC_F8);
static SENSOR_DEVICE_ATTR_RW(specific_f9, fd5121_long,
			     PMBUS_REG_MFR_SPECIFIC_F9);
static SENSOR_DEVICE_ATTR_RW(pwd_program_ram, fd5121_word,
			     PMBUS_REG_MFR_PWD_PROGRAM_RAM);
static SENSOR_DEVICE_ATTR_RW(pwd_program_i2c, fd5121_word,
			     PMBUS_REG_MFR_PWD_PROGRAM_I2C);
static SENSOR_DEVICE_ATTR_RW(pwd_enable_otp_store, fd5121_word,
			     PMBUS_REG_MFR_PWD_ENABLE_OTP_STORE);

static struct attribute *fd5121_non_paged_attrs[] = {
	&sensor_dev_attr_page.dev_attr.attr,
	&sensor_dev_attr_capability.dev_attr.attr,
	&sensor_dev_attr_pin_counter.dev_attr.attr,
	&sensor_dev_attr_vaux_uv_fault_limit.dev_attr.attr,
	&sensor_dev_attr_vaux_ov_fault_limit.dev_attr.attr,
	&sensor_dev_attr_vaux_uv_warning.dev_attr.attr,
	&sensor_dev_attr_vaux_ov_warning.dev_attr.attr,
	&sensor_dev_attr_free_user_config_tables.dev_attr.attr,
	&sensor_dev_attr_address_table.dev_attr.attr,
	&sensor_dev_attr_unlock.dev_attr.attr,
	&sensor_dev_attr_faulty_sps.dev_attr.attr,
	&sensor_dev_attr_tlvr_faults.dev_attr.attr,
	&sensor_dev_attr_user_store_config_tab.dev_attr.attr,
	&sensor_dev_attr_user_config_index.dev_attr.attr,
	&sensor_dev_attr_pwm_disconnection.dev_attr.attr,
	&sensor_dev_attr_vr_disconnection.dev_attr.attr,
	&sensor_dev_attr_user_restore_config_tab.dev_attr.attr,
	&sensor_dev_attr_svr_go.dev_attr.attr,
	&sensor_dev_attr_set_pwd.dev_attr.attr,
	&sensor_dev_attr_config_activate.dev_attr.attr,
	&sensor_dev_attr_config_recover.dev_attr.attr,
	&sensor_dev_attr_otp_dump.dev_attr.attr,
	&sensor_dev_attr_bbr_en.dev_attr.attr,
	&sensor_dev_attr_vboot.dev_attr.attr,
	&sensor_dev_attr_vsys.dev_attr.attr,
	&sensor_dev_attr_specific_e9.dev_attr.attr,
	&sensor_dev_attr_specific_ea.dev_attr.attr,
	&sensor_dev_attr_ss_cbc.dev_attr.attr,
	&sensor_dev_attr_checksum.dev_attr.attr,
	&sensor_dev_attr_cse_index.dev_attr.attr,
	&sensor_dev_attr_cout_measure.dev_attr.attr,
	&sensor_dev_attr_vr_cout.dev_attr.attr,
	&sensor_dev_attr_bbr_ram.dev_attr.attr,
	&sensor_dev_attr_bbr_otp.dev_attr.attr,
	&sensor_dev_attr_psys.dev_attr.attr,
	&sensor_dev_attr_specific_f8.dev_attr.attr,
	&sensor_dev_attr_specific_f9.dev_attr.attr,
	&sensor_dev_attr_pwd_program_ram.dev_attr.attr,
	&sensor_dev_attr_pwd_program_i2c.dev_attr.attr,
	&sensor_dev_attr_pwd_enable_otp_store.dev_attr.attr,
	&sensor_dev_attr_revision.dev_attr.attr,
	&sensor_dev_attr_id.dev_attr.attr,
	&sensor_dev_attr_model.dev_attr.attr,
	&sensor_dev_attr_mfr_revision.dev_attr.attr,
	&sensor_dev_attr_date.dev_attr.attr,
	&sensor_dev_attr_ic_device_id.dev_attr.attr,
	&sensor_dev_attr_ic_device_rev.dev_attr.attr,
	NULL
};

static struct attribute *fd5121_paged_attrs[] = {
	&sensor_dev_attr_operation.dev_attr.attr,
	&sensor_dev_attr_vout_raw.dev_attr.attr,
	&sensor_dev_attr_on_off_config.dev_attr.attr,
	&sensor_dev_attr_clear_faults.dev_attr.attr,
	&sensor_dev_attr_write_protect.dev_attr.attr,
	&sensor_dev_attr_smbalert_mask.dev_attr.attr,
	&sensor_dev_attr_vout_mode.dev_attr.attr,
	&sensor_dev_attr_vout_command.dev_attr.attr,
	&sensor_dev_attr_vout_margin_high.dev_attr.attr,
	&sensor_dev_attr_vout_margin_low.dev_attr.attr,
	&sensor_dev_attr_vout_min.dev_attr.attr,
	&sensor_dev_attr_vin_on.dev_attr.attr,
	&sensor_dev_attr_vin_off.dev_attr.attr,
	&sensor_dev_attr_vout_ov_fault_response.dev_attr.attr,
	&sensor_dev_attr_vout_uv_fault_response.dev_attr.attr,
	&sensor_dev_attr_iout_oc_fault_response.dev_attr.attr,
	&sensor_dev_attr_ot_fault_response.dev_attr.attr,
	&sensor_dev_attr_vin_ov_fault_response.dev_attr.attr,
	&sensor_dev_attr_status_byte.dev_attr.attr,
	&sensor_dev_attr_iknee_set.dev_attr.attr,
	&sensor_dev_attr_vmin_aware.dev_attr.attr,
	&sensor_dev_attr_power_mode.dev_attr.attr,
	&sensor_dev_attr_vin_uv_fault_response.dev_attr.attr,
	&sensor_dev_attr_iin_oc_fault_response.dev_attr.attr,
	&sensor_dev_attr_ton_delay.dev_attr.attr,
	&sensor_dev_attr_pout_op_fault_response.dev_attr.attr,
	&sensor_dev_attr_vaux_uv_fault_response.dev_attr.attr,
	&sensor_dev_attr_vaux_ov_fault_response.dev_attr.attr,
	&sensor_dev_attr_status_onsemi.dev_attr.attr,
	&sensor_dev_attr_status_cml.dev_attr.attr,
	&sensor_dev_attr_status_other.dev_attr.attr,
	&sensor_dev_attr_status_mfr_specific.dev_attr.attr,
	&sensor_dev_attr_read_vaux.dev_attr.attr,
	&sensor_dev_attr_ton_slew.dev_attr.attr,
	&sensor_dev_attr_toff_slew.dev_attr.attr,
	&sensor_dev_attr_rail_name.dev_attr.attr,
	&sensor_dev_attr_vout_droop.dev_attr.attr,
	&sensor_dev_attr_dpm_min.dev_attr.attr,
	&sensor_dev_attr_preclamp_offset.dev_attr.attr,
	&sensor_dev_attr_tlvr_diagn.dev_attr.attr,
	&sensor_dev_attr_amd_status.dev_attr.attr,
	&sensor_dev_attr_postclamp_offset.dev_attr.attr,
	&sensor_dev_attr_pgood_delay.dev_attr.attr,
	&sensor_dev_attr_vout_max.dev_attr.attr,
	&sensor_dev_attr_vout_transition_rate.dev_attr.attr,
	NULL
};

static struct attribute_group fd5121_groups[2] = {
	{ .name = "global", .attrs = fd5121_non_paged_attrs },
	{ .name = "paged", .attrs = fd5121_paged_attrs }
};

/* Regulator descriptors for VOUT rails (VID encoded) */
static struct regulator_desc fd5121_reg_desc[] = {
	PMBUS_REGULATOR_STEP_ONE("vout1", 3001, 1000, 200000),
	PMBUS_REGULATOR_STEP_ONE("vout2", 3001, 1000, 200000),
};

static int fd5121_valid_reg(struct i2c_client *client, int reg)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(cc_list); i++) {
		if (reg == cc_list[i])
			return 0;
	}

	if (fd5121_blk_rd_reg(reg))
		return 0;
	return -ENXIO;
}

static int fd5121_read_word_data(struct i2c_client *client, int page,
				 int phase, int reg)
{
	int ret;

	ret = fd5121_valid_reg(client, reg);
	if (ret < 0)
		return ret;

	ret = pmbus_read_word_data(client, page, phase, reg);
	if (ret < 0)
		return ret;

	/* Chip reports VOUT_MODE as vid. But gives raw value 1mV per bit.
	 * So, encode the READ_VOUT value so that it gets decoded and
	 * reported correctly.
	 */
	if (reg == PMBUS_READ_VOUT)
		ret = DIV_ROUND_CLOSEST(155000 - ret * 100, 625);
	return ret;
}

static int fd5121_read_byte_data(struct i2c_client *client, int page, int reg)
{
	int ret;

	ret = fd5121_valid_reg(client, reg);
	if (ret < 0)
		return ret;

	return pmbus_read_byte_data(client, page, reg);
}

static int fd5121_write_byte_data(struct i2c_client *client, int page,
				  int reg, u8 value)
{
	int ret;

	ret = fd5121_valid_reg(client, reg);
	if (ret < 0)
		return ret;
	return pmbus_write_byte_data(client, page, reg, value);
}

static int fd5121_write_byte(struct i2c_client *client, int page, u8 byte)
{
	return pmbus_write_byte(client, page, byte);
}

static int fd5121_write_word_data(struct i2c_client *client, int page,
				    int reg, u16 word)
{
	int ret;

	ret = fd5121_valid_reg(client, reg);
	if (ret < 0)
		return ret;
	ret = pmbus_write_word_data(client, page, reg, word);
	return ret;
}

static u32 fd5121_get_dev_id(struct i2c_client *client)
{
	u32 dev_id = 0;
	s32 ret = 0;

	ret = fd5121_read_long(client, PMBUS_IC_DEVICE_ID, &dev_id);
	if (ret < 0)
		return CTLR_ID_UNKNOWN;

	switch (dev_id) {
	case CTLR_ID_FD5121:
	case CTLR_ID_FD5123:
	case CTLR_ID_FD5125:
		break;
	default:
		if (dev_id != 0)
			dev_err(&client->dev, "Unknown device 0x%x",
				dev_id);
		return CTLR_ID_UNKNOWN;
	}
	return dev_id;
}

static int fd5121_probe(struct i2c_client *client)
{
	struct pmbus_driver_info *info;
	struct fd5121_data *pdata;
	u32 id;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	pdata = devm_kzalloc(&client->dev, sizeof(struct fd5121_data),
			     GFP_KERNEL);
	if (pdata == NULL)
		return -ENOMEM;

	pdata->dev = &client->dev;
	pdata->groups[0] = &fd5121_groups[0];
	pdata->groups[1] = &fd5121_groups[1];

	id = fd5121_get_dev_id(client);
	if (id == CTLR_ID_UNKNOWN)
		return -ENODEV;

	pdata->id = id;

	switch (id) {
	case CTLR_ID_FD5121:
	case CTLR_ID_FD5123:
	case CTLR_ID_FD5125:
		break;
	default:
		dev_err(&client->dev, "Failed to read device ID");
		return -ENODEV;
	}

	info = &pdata->info;
	info->groups = (const struct attribute_group **)&pdata->groups[0];
	info->write_word_data = fd5121_write_word_data;
	info->write_byte = fd5121_write_byte;
	info->write_byte_data = fd5121_write_byte_data;
	info->read_word_data = fd5121_read_word_data;
	info->read_byte_data = fd5121_read_byte_data;

	info->pages = FD5121_NUM_PAGES;
	info->format[PSC_VOLTAGE_IN] = linear;
	info->format[PSC_VOLTAGE_OUT] = vid;

	fd5121_reg_desc[0].id = 0;
	fd5121_reg_desc[1].id = 1;

	/* Device implements VID coding with 1 mV steps from 0.200 V
	 * up to 3.200 V
	 */
	info->num_regulators = FD5121_NUM_PAGES;
	info->reg_desc = fd5121_reg_desc;
	info->format[PSC_CURRENT_IN] = linear;
	info->format[PSC_CURRENT_OUT] = linear;
	info->format[PSC_POWER] = linear;
	info->format[PSC_TEMPERATURE] = linear;
	for (u8 idx = 0; idx < info->pages; idx++) {
		info->func[idx] = PMBUS_HAVE_IOUT | PMBUS_HAVE_STATUS_IOUT;
		info->func[idx] |= PMBUS_HAVE_VOUT | PMBUS_HAVE_STATUS_VOUT;
		info->func[idx] |= PMBUS_HAVE_TEMP | PMBUS_HAVE_STATUS_TEMP;
		info->func[idx] |= PMBUS_HAVE_PIN | PMBUS_HAVE_POUT;
		info->func[idx] |= PMBUS_HAVE_VIN | PMBUS_HAVE_IIN;
		info->func[idx] |= PMBUS_HAVE_STATUS_INPUT;
		info->vrm_version[idx] = amd625mv;
	}
	return pmbus_do_probe(client, info);
}

#ifdef CONFIG_OF
static const struct of_device_id fd5121_of_match[] = {
	{ .compatible = "onnn,fd5121" },
	{ }
};
MODULE_DEVICE_TABLE(of, fd5121_of_match);
#endif

static const struct i2c_device_id fd5121_id[] = {
	{ "fd5121", chip_fd5121 },
	{ "fd5123", chip_fd5123 },
	{ "fd5125", chip_fd5125 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, fd5121_id);

static struct i2c_driver fd5121_driver = {
	.driver = {
		.name = "fd5121",
		.of_match_table = of_match_ptr(fd5121_of_match),
	},
	.probe = fd5121_probe,
	.id_table = fd5121_id,
};

module_i2c_driver(fd5121_driver);

MODULE_AUTHOR("Selva Rajagopal <selvamani.rajagopal@onsemi.com>");
MODULE_DESCRIPTION("PMBus driver for FD5121");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("PMBUS");

