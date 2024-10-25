// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Analog Devices Inc.
 *
 * Driver for the LTC4296_1 SPoE PSE.
 *
 * Author: Antoniu Miclaus <antoniu.miclaus@analog.com>
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/hwmon-sysfs.h>
#include <linux/hwmon.h>
#include <linux/spi/spi.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/regmap.h>

#include <linux/unaligned.h>

#define LTC4296_1_REG_GFLTEV			0x02
#define LTC4296_1_REG_GFLTMSK			0x03
#define LTC4296_1_REG_GCAP			0x06
#define LTC4296_1_REG_GIOST			0x07
#define LTC4296_1_REG_GCMD			0x08
#define LTC4296_1_REG_GCFG			0x09
#define LTC4296_1_REG_GADCCFG			0x0A
#define LTC4296_1_REG_GADCDAT			0x0B
#define LTC4296_1_REG_P0EV			0x10
#define LTC4296_1_REG_P0ST			0x12
#define LTC4296_1_REG_P0CFG0			0x13
#define LTC4296_1_REG_P0CFG1			0x14
#define LTC4296_1_REG_P0ADCCFG			0x15
#define LTC4296_1_REG_P0ADCDAT			0x16
#define LTC4296_1_REG_P0SELFTEST		0x17
#define LTC4296_1_REG_P1EV			0x20
#define LTC4296_1_REG_P1ST			0x22
#define LTC4296_1_REG_P1CFG0			0x23
#define LTC4296_1_REG_P1CFG1			0x24
#define LTC4296_1_REG_P1ADCCFG			0x25
#define LTC4296_1_REG_P1ADCDAT			0x26
#define LTC4296_1_REG_P1SELFTEST		0x27
#define LTC4296_1_REG_P2EV			0x30
#define LTC4296_1_REG_P2ST			0x32
#define LTC4296_1_REG_P2CFG0			0x33
#define LTC4296_1_REG_P2CFG1			0x34
#define LTC4296_1_REG_P2ADCCFG			0x35
#define LTC4296_1_REG_P2ADCDAT			0x36
#define LTC4296_1_REG_P2SELFTEST		0x37
#define LTC4296_1_REG_P3EV			0x40
#define LTC4296_1_REG_P3ST			0x42
#define LTC4296_1_REG_P3CFG0			0x43
#define LTC4296_1_REG_P3CFG1			0x44
#define LTC4296_1_REG_P3ADCCFG			0x45
#define LTC4296_1_REG_P3ADCDAT			0x46
#define LTC4296_1_REG_P3SELFTEST		0x47
#define LTC4296_1_REG_P4EV			0x50
#define LTC4296_1_REG_P4ST			0x52
#define LTC4296_1_REG_P4CFG0			0x53
#define LTC4296_1_REG_P4CFG1			0x54
#define LTC4296_1_REG_P4ADCCFG			0x55
#define LTC4296_1_REG_P4ADCDAT			0x56
#define LTC4296_1_REG_P4SELFTEST		0x57

/* LTC4296_1_REG_GFLTEV */
#define LTC4296_1_UVLO_DIGITAL_MSK		BIT(4)
#define LTC4296_1_COMMAND_FAULT_MSK		BIT(3)
#define LTC4296_1_PEC_FAULT_MSK			BIT(2)
#define LTC4296_1_MEMORY_FAULT_MSK		BIT(1)
#define LTC4296_1_LOW_CKT_BRK_FAULT_MSK		BIT(0)

/* LTC4296_1_REG_GCAP */
#define LTC4296_1_SCCP_SUPPORT_MSK		BIT(6)
#define LTC4296_1_WAKE_FWD_SUPPORT_MSK		BIT(5)
#define LTC4296_1_NUMPORTS_MSK			GENMASK(4, 0)

/* LTC4296_1_REG_GIOST */
#define LTC4296_1_PG_OUT4_MSK			BIT(8)
#define LTC4296_1_PG_OUT3_MSK			BIT(7)
#define LTC4296_1_PG_OUT2_MSK			BIT(6)
#define LTC4296_1_PG_OUT1_MSK			BIT(5)
#define LTC4296_1_PG_OUT0_MSK			BIT(4)
#define LTC4296_1_PAD_AUTO_MSK			BIT(3)
#define LTC4296_1_PAD_WAKEUP_MSK		BIT(2)
#define LTC4296_1_PAD_WAKEUP_DRIVE_MSK		BIT(1)

/* LTC4296_1_REG_GCMD */
#define LTC4296_1_SW_RESET_MSK			GENMASK(15, 8)
#define LTC4296_1_WRITE_PROTECT_MSK		GENMASK(7, 0)

/* LTC4296_1_REG_GCFG */
#define LTC4296_1_MASK_LOWFAULT_MSK		BIT(5)
#define LTC4296_1_TLIM_DISABLE_MSK		BIT(4)
#define LTC4296_1_TLIM_TIMER_SLEEP_MSK		GENMASK(3, 2)
#define LTC4296_1_REFRESH_MSK			BIT(1)
#define LTC4296_1_SW_VIN_PGOOD_MSK		BIT(0)

/* LTC4296_1_REG_GADCCFG */
#define LTC4296_1_GADC_SAMPLE_MODE_MSK		GENMASK(6, 5)
#define LTC4296_1_GADC_SEL_MSK			GENMASK(4, 0)

/* LTC4296_1_REG_GADCDAT */
#define LTC4296_1_GADC_MISSED_MSK		BIT(13)
#define LTC4296_1_GADC_NEW_MSK			BIT(12)
#define LTC4296_1_GADC_MSK			GENMASK(11, 0)

/* LTC4296_1_REG_PXEV */
#define LTC4296_1_VALID_SIGNATURE_MSK		BIT(9)
#define LTC4296_1_INVALID_SIGNATURE_MSK		BIT(8)
#define LTC4296_1_TOFF_TIMER_DONE_MSK		BIT(7)
#define LTC4296_1_OVERLOAD_DETECTED_ISLEEP_MSK	BIT(6)
#define LTC4296_1_OVERLOAD_DETECTED_IPOWER_MSK	BIT(5)
#define LTC4296_1_MFVS_TIMEOUT_MSK		BIT(4)
#define LTC4296_1_TINRUSH_TIMER_DONE_MSK	BIT(3)
#define LTC4296_1_PD_WAKEUP_MSK			BIT(2)
#define LTC4296_1_LSNS_FORWARD_FAULT_MSK	BIT(1)
#define LTC4296_1_LSNS_REVERSE_FAULT_MSK	BIT(0)

/* LTC4296_1_REG_PXST */
#define LTC4296_1_DET_VHIGH_MSK			BIT(13)
#define LTC4296_1_DET_VLOW_MSK			BIT(12)
#define LTC4296_1_POWER_STABLE_HI_MSK		BIT(11)
#define LTC4296_1_POWER_STABLE_LO_MSK		BIT(10)
#define LTC4296_1_POWER_STABLE_MSK		BIT(9)
#define LTC4296_1_OVERLOAD_HELD_MSK		BIT(8)
#define LTC4296_1_PI_SLEEPING_MSK		BIT(7)
#define LTC4296_1_PI_PREBIASED_MSK		BIT(6)
#define LTC4296_1_PI_DETECTING_MSK		BIT(5)
#define LTC4296_1_PI_POWERED_MSK		BIT(4)
#define LTC4296_1_PI_DISCHARGE_EN_MSK		BIT(3)
#define LTC4296_1_PSE_STATUS_MSK		GENMASK(2, 0)

/* LTC4296_1_REG_PXCFG0 */
#define LTC4296_1_SW_INRUSH_MSK			BIT(15)
#define LTC4296_1_END_CLASSIFICATION_MSK	BIT(14)
#define LTC4296_1_SET_CLASSIFICATION_MODE_MSK	BIT(13)
#define LTC4296_1_DISABLE_DETECTION_PULLUP_MSK	BIT(12)
#define LTC4296_1_TDET_DISABLE_MSK		BIT(11)
#define LTC4296_1_FOLDBACK_DISABLE_MSK		BIT(10)
#define LTC4296_1_SOFT_START_DISABLE_MSK	BIT(9)
#define LTC4296_1_TOFF_TIMER_DISABLE_MSK	BIT(8)
#define LTC4296_1_TMFVDO_TIMER_DISABLE_MSK	BIT(7)
#define LTC4296_1_SW_PSE_READY_MSK		BIT(6)
#define LTC4296_1_SW_POWER_AVAILABLE_MSK	BIT(5)
#define LTC4296_1_UPSTREAM_WAKEUP_DISABLE_MSK	BIT(4)
#define LTC4296_1_DOWNSTREAM_WAKEUP_DISABLE_MSK	BIT(3)
#define LTC4296_1_SW_PSE_WAKEUP_MSK		BIT(2)
#define LTC4296_1_HW_EN_MASK_MSK		BIT(1)
#define LTC4296_1_SW_EN_MSK			BIT(0)

/* LTC4296_1_REG_PXCFG1 */
#define LTC4296_1_PREBIAS_OVERRIDE_GOOD_MSK	BIT(8)
#define LTC4296_1_TLIM_TIMER_TOP_MSK		GENMASK(7, 6)
#define LTC4296_1_TOD_TRESTART_TIMER_MSK	GENMASK(5, 4)
#define LTC4296_1_TINRUSH_TIMER_MSK		GENMASK(3, 2)
#define LTC4296_1_SIG_OVERRIDE_BAD_MSK		BIT(1)
#define LTC4296_1_SIG_OVERRIDE_GOOD_MSK		BIT(0)

/* LTC4296_1_REG_PXADCCFG */
#define LTC4296_1_MFVS_THRESHOLD_MSK		GENMASK(7, 0)

/* LTC4296_1_REG_PXADCDAT */
#define LTC4296_1_MISSED_MSK			BIT(13)
#define LTC4296_1_NEW_MSK			BIT(12)
#define LTC4296_1_SOURCE_CURRENT_MSK		GENMASK(11, 0)

/* Miscellaneous Definitions*/
#define LTC4296_1_RESET_CODE			0x73
#define LTC4296_1_UNLOCK_KEY			0x05
#define LTC4296_1_LOCK_KEY			0xA0

#define LTC4296_1_ADC_OFFSET			2049

#define LTC4296_1_VGAIN				(35230 / 1000)
#define LTC4296_1_IGAIN				(1 / 10)

#define LTC4296_1_VMAX				1
#define LTC4296_1_VMIN				0

#define LTC4296_1_MAX_PORTS			5

enum ltc4296_1_port {
	LTC_PORT0,
	LTC_PORT1,
	LTC_PORT2,
	LTC_PORT3,
	LTC_PORT4,
	LTC_NO_PORT
};

enum ltc4296_1_port_status {
	LTC_PORT_DISABLED,
	LTC_PORT_ENABLED
};

enum ltc4296_1_port_reg_offset_e {
	LTC_PORT_EVENTS   = 0,
	LTC_PORT_STATUS   = 2,
	LTC_PORT_CFG0     = 3,
	LTC_PORT_CFG1     = 4,
	LTC_PORT_ADCCFG   = 5,
	LTC_PORT_ADCDAT   = 6,
	LTC_PORT_SELFTEST = 7
};

static u8 set_port_vout[LTC4296_1_MAX_PORTS] = {0x04, 0x06, 0x08, 0x0A, 0x0C};

struct ltc4296_1_state {
	struct spi_device *spi;
	u32 r_sense_mohm[LTC4296_1_MAX_PORTS];
	u8 data[5];
};

static u8 ltc4296_1_get_pec_byte(u8 data, u8 seed)
{
	u8 pec = seed;
	u8 din, in0, in1, in2;
	int bit;

	for (bit = 7; bit >= 0; bit--) {
		din = (data >> bit) & 0x01;
		in0 = din ^ ((pec >> 7) & 0x01);
		in1 = in0 ^ (pec & 0x01);
		in2 = in0 ^ ((pec >> 1) & 0x01);
		pec = (pec << 1);
		pec &= ~(0x07);
		pec = pec | in0 | (in1 << 1) | (in2 << 2);
	}

	return pec;
}

static int ltc4296_1_spi_write(struct ltc4296_1_state *st, unsigned int reg, u16 val)
{
	st->data[0] = reg << 1 | 0x0;
	st->data[1] = ltc4296_1_get_pec_byte(st->data[0], 0x41);
	st->data[2] = val >> 8;
	st->data[3] = val & 0xFF;
	st->data[4] = ltc4296_1_get_pec_byte(st->data[3],
					     ltc4296_1_get_pec_byte(st->data[2], 0x41));

	return spi_write(st->spi, &st->data[0], 5);
}

static int ltc4296_1_spi_read(struct ltc4296_1_state *st, unsigned int reg,
			      u16 *val)
{
	int ret;
	struct spi_transfer t = {0};

	t.tx_buf = &st->data[0];
	t.rx_buf = &st->data[0];
	t.len = 5;

	st->data[0] = reg << 1 | 0x1;
	st->data[1] = ltc4296_1_get_pec_byte(st->data[0], 0x41);

	ret = spi_sync_transfer(st->spi, &t, 1);
	if (ret)
		return ret;

	*val = get_unaligned_be16(&st->data[2]);

	return 0;
}

static int ltc4296_1_reset(struct ltc4296_1_state *st)
{
	int ret;

	ret = ltc4296_1_spi_write(st, LTC4296_1_REG_GCMD, LTC4296_1_RESET_CODE);
	if (ret)
		return ret;

	fsleep(10000);

	return 0;
}

static int ltc4296_1_get_port_addr(enum ltc4296_1_port port_no,
				   enum ltc4296_1_port_reg_offset_e port_offset,
				   u8 *port_addr)
{
	if (!port_addr)
		return -EINVAL;

	*port_addr = (((port_no + 1) << 4) + port_offset);

	return 0;
}

static int ltc4296_1_read_gadc(struct ltc4296_1_state *st, int *port_voltage_mv)
{
	int ret;
	u16 val16;

	if (!st || !port_voltage_mv)
		return -EINVAL;

	ret = ltc4296_1_spi_read(st, LTC4296_1_REG_GADCDAT, &val16);
	if (ret)
		return ret;
	if ((val16 & LTC4296_1_GADC_NEW_MSK) != LTC4296_1_GADC_NEW_MSK)
		return -EINVAL;

	/* A new ADC value is available */
	*port_voltage_mv = (((val16 & LTC4296_1_GADC_MSK) - LTC4296_1_ADC_OFFSET) *
			    LTC4296_1_VGAIN);

	return 0;
}

static int ltc4296_1_read_port_current(struct ltc4296_1_state *st, enum ltc4296_1_port port_no,
				       int *port_i_out_ma)
{
	int ret;
	u8 port_addr = 0;
	u16 val16;

	if (!st || !port_i_out_ma || !st->r_sense_mohm[port_no])
		return -EINVAL;

	ret = ltc4296_1_get_port_addr(port_no, LTC_PORT_ADCDAT, &port_addr);
	if (ret)
		return ret;

	ret = ltc4296_1_spi_read(st, port_addr, &val16);
	if (ret)
		return ret;

	if ((val16 & LTC4296_1_NEW_MSK) == LTC4296_1_NEW_MSK)
		/* A new ADC value is available */
		*port_i_out_ma = (FIELD_GET(LTC4296_1_SOURCE_CURRENT_MSK, val16) - LTC4296_1_ADC_OFFSET)
				  * 100 / st->r_sense_mohm[port_no];
	else
		return -EINVAL;

	return 0;
}

static int ltc4296_1_port_prebias(struct ltc4296_1_state *st, enum ltc4296_1_port port_no)
{
	int ret;
	u8 port_addr = 0;

	ret = ltc4296_1_get_port_addr(port_no, LTC_PORT_CFG1, &port_addr);
	if (ret)
		return ret;

	return ltc4296_1_spi_write(st, port_addr, LTC4296_1_PREBIAS_OVERRIDE_GOOD_MSK |
						FIELD_PREP(LTC4296_1_TINRUSH_TIMER_MSK, 2) |
						LTC4296_1_SIG_OVERRIDE_GOOD_MSK);
}

static int ltc4296_1_port_en(struct ltc4296_1_state *st, enum ltc4296_1_port port_no)
{
	int ret;
	u8 port_addr = 0;

	ret = ltc4296_1_get_port_addr(port_no, LTC_PORT_CFG0, &port_addr);
	if (ret)
		return ret;

	return ltc4296_1_spi_write(st, port_addr, (LTC4296_1_SW_EN_MSK |
				 LTC4296_1_SW_POWER_AVAILABLE_MSK |
				 LTC4296_1_SW_PSE_READY_MSK |
				 LTC4296_1_TMFVDO_TIMER_DISABLE_MSK));
}

static int ltc4296_1_port_dis(struct ltc4296_1_state *st, enum ltc4296_1_port port_no)
{
	int ret;
	u8 port_addr = 0;

	ret = ltc4296_1_get_port_addr(port_no, LTC_PORT_CFG0, &port_addr);
	if (ret)
		return ret;

	return ltc4296_1_spi_write(st, port_addr, 0);
}

static int ltc4296_1_force_port_pwr(struct ltc4296_1_state *st, enum ltc4296_1_port port_no)
{
	int ret;
	u8 port_addr = 0;

	if (!st)
		return -EINVAL;

	ret = ltc4296_1_get_port_addr(port_no, LTC_PORT_CFG0, &port_addr);
	if (ret)
		return ret;

	return ltc4296_1_spi_write(st, port_addr,
				 (LTC4296_1_SW_EN_MSK | LTC4296_1_SW_POWER_AVAILABLE_MSK |
				  LTC4296_1_SW_PSE_READY_MSK | LTC4296_1_TMFVDO_TIMER_DISABLE_MSK));
}

static int ltc4296_1_set_gadc_vout(struct ltc4296_1_state *st, enum ltc4296_1_port port_no)
{
	u16 val16;

	if (!st)
		return -EINVAL;

	/* LTC4296_1-1 Set global ADC to measure Port Vout GADCCFG=ContModeLowGain|VoutPort */
	val16 = FIELD_PREP(LTC4296_1_GADC_SEL_MSK, set_port_vout[port_no]);
	/* Set Continuous mode with LOW GAIN bit */
	val16 = val16 | FIELD_PREP(LTC4296_1_GADC_SAMPLE_MODE_MSK, 2);

	return ltc4296_1_spi_write(st, LTC4296_1_REG_GADCCFG, val16);
}

static int ltc4296_1_init(struct ltc4296_1_state *st)
{
	int ret;
	u16 value;

	ret = ltc4296_1_reset(st);
	if (ret)
		return ret;

	ret = ltc4296_1_spi_write(st, LTC4296_1_REG_GCMD, LTC4296_1_UNLOCK_KEY);
	if (ret)
		return ret;

	ret = ltc4296_1_spi_read(st, LTC4296_1_REG_GCMD, &value);
	if (ret)
		return ret;

	if (value != LTC4296_1_UNLOCK_KEY) {
		dev_err_probe(&st->spi->dev, -EINVAL, "Device locked. Write Access is disabled\n");
		return -EINVAL;
	}

	return 0;
}

static ssize_t input_enable_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	int channel = to_sensor_dev_attr(attr)->index;
	struct ltc4296_1_state *st = dev_get_drvdata(dev);
	int ret;
	bool val;

	ret = kstrtobool(buf, &val);
	if (ret)
		return ret;

	if (!val) {
		ret = ltc4296_1_port_dis(st, channel);
	} else {
		ret = ltc4296_1_port_prebias(st, channel);
		if (ret)
			return ret;

		ret = ltc4296_1_port_en(st, channel);
	}

	if (ret)
		return ret;

	return count;
}

static ssize_t input_enable_show(struct device *dev, struct device_attribute *attr,
				 char *buf)
{
	int channel = to_sensor_dev_attr(attr)->index;
	struct ltc4296_1_state *st = dev_get_drvdata(dev);
	int ret;
	u8 port_addr = 0;
	u16 val = 0;

	ret = ltc4296_1_get_port_addr(channel, LTC_PORT_CFG0, &port_addr);
	if (ret)
		return ret;

	ret = ltc4296_1_spi_read(st, port_addr, &val);
	if (ret)
		return ret;

	if (val & LTC4296_1_SW_EN_MSK)
		return sprintf(buf, "Port %d enabled.\n", channel);

	return sprintf(buf, "Port %d disabled.\n", channel);
}

static ssize_t input_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	int channel = to_sensor_dev_attr(attr)->index;
	struct ltc4296_1_state *st = dev_get_drvdata(dev);
	int ret, data;

	ret = ltc4296_1_port_prebias(st, channel);
	if (ret)
		return ret;

	ret = ltc4296_1_force_port_pwr(st, channel);
	if (ret)
		return ret;

	ret = ltc4296_1_set_gadc_vout(st, channel);
	if (ret)
		return ret;

	fsleep(10000);

	ret = ltc4296_1_read_gadc(st, &data);
	if (ret)
		return ret;

	return sprintf(buf, "%d mV\n", data);
}

static ssize_t curr_input_show(struct device *dev, struct device_attribute *attr,
			       char *buf)
{
	int channel = to_sensor_dev_attr(attr)->index;
	struct ltc4296_1_state *st = dev_get_drvdata(dev);
	int ret, data;

	ret = ltc4296_1_read_port_current(st, channel, &data);
	if (ret)
		return ret;

	return sprintf(buf, "%d mA\n", data);
}

static SENSOR_DEVICE_ATTR_RW(in0_input_enable, input_enable, 0);
static SENSOR_DEVICE_ATTR_RW(in1_input_enable, input_enable, 1);
static SENSOR_DEVICE_ATTR_RW(in2_input_enable, input_enable, 2);
static SENSOR_DEVICE_ATTR_RW(in3_input_enable, input_enable, 3);
static SENSOR_DEVICE_ATTR_RW(in4_input_enable, input_enable, 4);

static SENSOR_DEVICE_ATTR_RO(in0_input, input, 0);
static SENSOR_DEVICE_ATTR_RO(in1_input, input, 1);
static SENSOR_DEVICE_ATTR_RO(in2_input, input, 2);
static SENSOR_DEVICE_ATTR_RO(in3_input, input, 3);
static SENSOR_DEVICE_ATTR_RO(in4_input, input, 4);

static SENSOR_DEVICE_ATTR_RO(curr0_input, curr_input, 0);
static SENSOR_DEVICE_ATTR_RO(curr1_input, curr_input, 1);
static SENSOR_DEVICE_ATTR_RO(curr2_input, curr_input, 2);
static SENSOR_DEVICE_ATTR_RO(curr3_input, curr_input, 3);
static SENSOR_DEVICE_ATTR_RO(curr4_input, curr_input, 4);

static struct attribute *ltc4296_1_attrs[] = {
	&sensor_dev_attr_in0_input_enable.dev_attr.attr,
	&sensor_dev_attr_in1_input_enable.dev_attr.attr,
	&sensor_dev_attr_in2_input_enable.dev_attr.attr,
	&sensor_dev_attr_in3_input_enable.dev_attr.attr,
	&sensor_dev_attr_in4_input_enable.dev_attr.attr,
	&sensor_dev_attr_in0_input.dev_attr.attr,
	&sensor_dev_attr_in1_input.dev_attr.attr,
	&sensor_dev_attr_in2_input.dev_attr.attr,
	&sensor_dev_attr_in3_input.dev_attr.attr,
	&sensor_dev_attr_in4_input.dev_attr.attr,
	&sensor_dev_attr_curr0_input.dev_attr.attr,
	&sensor_dev_attr_curr1_input.dev_attr.attr,
	&sensor_dev_attr_curr2_input.dev_attr.attr,
	&sensor_dev_attr_curr3_input.dev_attr.attr,
	&sensor_dev_attr_curr4_input.dev_attr.attr,
	NULL
};
ATTRIBUTE_GROUPS(ltc4296_1);

static int ltc4296_1_probe(struct spi_device *spi)
{
	struct ltc4296_1_state *st;
	struct device *hwmon_dev;
	u32 val, addr;
	int ret;

	st = devm_kzalloc(&spi->dev, sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	st->spi = spi;

	ret = devm_regulator_get_enable(dev, "vin");
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to enable regulator\n");

	device_for_each_child_node_scoped(&spi->dev, child) {
		ret = fwnode_property_read_u32(child, "reg", &addr);
		if (ret < 0)
			return ret;

		if (addr > 4)
			return -EINVAL;

		ret = fwnode_property_read_u32(child,
					       "shunt-resistor-micro-ohms",
					       &val);

		if (!ret) {
			if (!val)
				return dev_err_probe(&spi->dev, -EINVAL,
						     "shunt resistor value cannot be zero\n");

			st->r_sense_mohm[addr] = val;
		}
	}

	ret = ltc4296_1_init(st);
	if (ret)
		return ret;

	hwmon_dev = devm_hwmon_device_register_with_groups(&spi->dev,
							   spi->modalias,
							   st, ltc4296_1_groups);
	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct spi_device_id ltc4296_1_id[] = {
	{ "ltc4296-1", 0 },
	{}
};
MODULE_DEVICE_TABLE(spi, ltc4296_1_id);

static const struct of_device_id ltc4296_1_of_match[] = {
	{ .compatible = "adi,ltc4296-1", },
	{ },
};
MODULE_DEVICE_TABLE(of, ltc4296_1_of_match);

static struct spi_driver ltc4296_1_driver = {
	.probe = ltc4296_1_probe,
	.driver = {
		.name = "ltc4296-1",
		.of_match_table	= ltc4296_1_of_match,
	},
	.id_table = ltc4296_1_id,
};
module_spi_driver(ltc4296_1_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Antoniu Miclaus <antoniu.miclaus@analog.com>");
MODULE_DESCRIPTION("LTC4296_1 SPoE PSE");
