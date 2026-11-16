// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * rmi-hwmon.c - hwmon sensor support for side band RMI
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/property.h>
#include <uapi/misc/amd-apml.h>
#include "rmi-core.h"

/* Do not allow setting negative power limit */
#define SBRMI_PWR_MIN  0

/* DIMM temperature (SB-RMI Mode 1 DIMM_ADDRESS) */
#define DIMM_BASE_ID		0x80	/* Mode 1: Bit[7]=1 */
#define DIMM_TS_SELECT_BIT	BIT(6)	/* 0 = TS0, 1 = TS1 */
#define DIMM_RSVD_BIT		BIT(5)
#define DIMM_SLOT_BIT		BIT(4)
#define DIMM_CHAN_MASK		GENMASK(3, 0)
#define DIMM_TEMP_MASK		GENMASK(31, 21)
#define DIMM_CHANNELS_PER_TS	16
#define SBRMI_MAX_DIMMS		DIMM_CHANNELS_PER_TS
#define SBRMI_NUM_DIMM_CHANNELS	(DIMM_CHANNELS_PER_TS * 2)	/* TS0 + TS1 */

static bool sbrmi_dimm_channel_valid(const struct sbrmi_data *data, int channel)
{
	return data->dimm.dimm_ids && data->dimm.dimm_ids[channel];
}

/* Fill dimm_ids[] from the "amd,dimm-ids" entries, one sensor per entry */
static int sbrmi_read_dimm_ids(struct device *dev, struct sbrmi_data *data)
{
	u8 raw_ids[SBRMI_NUM_DIMM_CHANNELS];
	int count, i, ret, ch;
	u8 addr;

	count = device_property_count_u8(dev, "amd,dimm-ids");
	if (count <= 0 || count > SBRMI_NUM_DIMM_CHANNELS) {
		dev_err(dev, "amd,dimm-ids must have 1..%d entries, got %d\n",
			SBRMI_NUM_DIMM_CHANNELS, count);
		return -EINVAL;
	}

	ret = device_property_read_u8_array(dev, "amd,dimm-ids", raw_ids, count);
	if (ret) {
		dev_err(dev, "failed to read amd,dimm-ids: %d\n", ret);
		return ret;
	}

	for (i = 0; i < count; i++) {
		addr = raw_ids[i];
		/* Board DIMM n = memory channel * dpc + slot */
		ch = (addr & DIMM_CHAN_MASK) * data->dimm.dpc + FIELD_GET(DIMM_SLOT_BIT, addr);

		if (!(addr & DIMM_BASE_ID) || (addr & DIMM_RSVD_BIT) ||
		    ch >= SBRMI_MAX_DIMMS ||
		    (data->dimm.dpc == 1 && (addr & DIMM_SLOT_BIT))) {
			dev_err(dev, "invalid amd,dimm-ids[%d] = 0x%x for %u DIMM(s) per channel\n",
				i, addr, data->dimm.dpc);
			return -EINVAL;
		}

		if (addr & DIMM_TS_SELECT_BIT)
			ch += DIMM_CHANNELS_PER_TS;
		if (data->dimm.dimm_ids[ch]) {
			dev_err(dev, "amd,dimm-ids[%d] = 0x%x duplicates 0x%x\n",
				i, addr, data->dimm.dimm_ids[ch]);
			return -EINVAL;
		}

		data->dimm.dimm_ids[ch] = addr;
	}

	return 0;
}

/*
 * Build dimm_ids[], indexed by hwmon channel (TS0 0-15, TS1 16-31), from
 * "amd,dimm-ids" or, when absent, for all 16 DIMMs.
 */
static int sbrmi_init_dimm_config(struct device *dev, struct sbrmi_data *data)
{
	int i;
	u8 addr;

	data->dimm.dimm_ids = devm_kcalloc(dev, SBRMI_NUM_DIMM_CHANNELS,
					   sizeof(*data->dimm.dimm_ids), GFP_KERNEL);
	if (!data->dimm.dimm_ids)
		return -ENOMEM;

	if (device_property_present(dev, "amd,dimm-ids"))
		return sbrmi_read_dimm_ids(dev, data);

	for (i = 0; i < SBRMI_MAX_DIMMS; i++) {
		addr = DIMM_BASE_ID | i;
		if (data->dimm.dpc == 2)
			addr = DIMM_BASE_ID | (i & 1 ? DIMM_SLOT_BIT : 0) | (i / 2);

		data->dimm.dimm_ids[i] = addr;
		data->dimm.dimm_ids[i + DIMM_CHANNELS_PER_TS] = addr | DIMM_TS_SELECT_BIT;
	}

	return 0;
}

static int sbrmi_read(struct device *dev, enum hwmon_sensor_types type,
		      u32 attr, int channel, long *val)
{
	struct sbrmi_data *data = dev_get_drvdata(dev);
	struct apml_mbox_msg msg = { 0 };
	int ret;

	if (!data)
		return -ENODEV;

	if (type != hwmon_power && type != hwmon_temp)
		return -EINVAL;

	if (type == hwmon_power) {
		switch (attr) {
		case hwmon_power_input:
			msg.cmd = SBRMI_READ_PKG_PWR_CONSUMPTION;
			ret = rmi_mailbox_xfer(data, &msg);
			break;
		case hwmon_power_cap:
			msg.cmd = SBRMI_READ_PKG_PWR_LIMIT;
			ret = rmi_mailbox_xfer(data, &msg);
			break;
		case hwmon_power_cap_max:
			msg.mb_in_out = data->pwr_limit_max;
			ret = 0;
			break;
		default:
			return -EINVAL;
		}
	} else {
		switch (attr) {
		case hwmon_temp_input:
			if (!sbrmi_dimm_channel_valid(data, channel))
				return -EINVAL;
			msg.cmd = SBRMI_READ_DIMM_THERMAL_SENSOR;
			msg.mb_in_out = data->dimm.dimm_ids[channel];
			ret = rmi_mailbox_xfer(data, &msg);
			if (ret == -EPROTOTYPE)
				dev_dbg(dev, "FW error code:0x%x\n",
					msg.fw_ret_code);
			break;
		default:
			return -EINVAL;
		}
	}
	if (ret < 0)
		return ret;

	if (type == hwmon_power) {
		/* hwmon power attributes are in microWatt */
		*val = (long)msg.mb_in_out * 1000;
	} else {
		/* 11-bit two's complement in bits 31:21, 0.25 degC per LSB */
		*val = sign_extend32(FIELD_GET(DIMM_TEMP_MASK, msg.mb_in_out), 10) * 250;
	}
	return ret;
}

static int sbrmi_write(struct device *dev, enum hwmon_sensor_types type,
		       u32 attr, int channel, long val)
{
	struct sbrmi_data *data = dev_get_drvdata(dev);
	struct apml_mbox_msg msg = { 0 };

	if (!data)
		return -ENODEV;

	if (type != hwmon_power && attr != hwmon_power_cap)
		return -EINVAL;
	/*
	 * hwmon power attributes are in microWatt
	 * mailbox read/write is in mWatt
	 */
	val /= 1000;

	val = clamp_val(val, SBRMI_PWR_MIN, data->pwr_limit_max);

	msg.cmd = SBRMI_WRITE_PKG_PWR_LIMIT;
	msg.mb_in_out = val;

	return rmi_mailbox_xfer(data, &msg);
}

static umode_t sbrmi_is_visible(const void *data,
				enum hwmon_sensor_types type,
				u32 attr, int channel)
{
	switch (type) {
	case hwmon_power:
		switch (attr) {
		case hwmon_power_input:
		case hwmon_power_cap_max:
			return 0444;
		case hwmon_power_cap:
			return 0644;
		}
		break;
	case hwmon_temp:
		if (!sbrmi_dimm_channel_valid(data, channel))
			return 0;
		switch (attr) {
		case hwmon_temp_input:
		case hwmon_temp_label:
			return 0444;
		}
		break;
	default:
		break;
	}
	return 0;
}

static const u32 sbrmi_temp_config[SBRMI_NUM_DIMM_CHANNELS + 1] = {
	[0 ... (SBRMI_NUM_DIMM_CHANNELS - 1)] = HWMON_T_INPUT | HWMON_T_LABEL,
};

static const struct hwmon_channel_info sbrmi_temp_channel_info = {
	.type = hwmon_temp,
	.config = sbrmi_temp_config,
};

static const struct hwmon_channel_info * const sbrmi_info[] = {
	HWMON_CHANNEL_INFO(power,
			   HWMON_P_INPUT | HWMON_P_CAP | HWMON_P_CAP_MAX),
	&sbrmi_temp_channel_info,
	NULL
};

static int sbrmi_read_string(struct device *dev,
			     enum hwmon_sensor_types type,
			     u32 attr, int channel, const char **str)
{
	struct sbrmi_data *data = dev_get_drvdata(dev);

	if (type != hwmon_temp || attr != hwmon_temp_label)
		return -EOPNOTSUPP;
	if (!sbrmi_dimm_channel_valid(data, channel))
		return -EINVAL;
	*str = data->dimm.labels[channel];
	return 0;
}

static const struct hwmon_ops sbrmi_hwmon_ops = {
	.is_visible = sbrmi_is_visible,
	.read = sbrmi_read,
	.read_string = sbrmi_read_string,
	.write = sbrmi_write,
};

static const struct hwmon_chip_info sbrmi_chip_info = {
	.ops = &sbrmi_hwmon_ops,
	.info = sbrmi_info,
};

/* Label each channel by its DIMM number, not by the address it queries */
static int sbrmi_init_dimm_labels(struct device *dev, struct sbrmi_data *data)
{
	int ch;

	data->dimm.labels = devm_kcalloc(dev, SBRMI_NUM_DIMM_CHANNELS,
					 sizeof(*data->dimm.labels), GFP_KERNEL);
	if (!data->dimm.labels)
		return -ENOMEM;

	for (ch = 0; ch < SBRMI_NUM_DIMM_CHANNELS; ch++) {
		if (!data->dimm.dimm_ids[ch])
			continue;
		data->dimm.labels[ch] =
			devm_kasprintf(dev, GFP_KERNEL, "DIMM_TS%d_UMC%d",
				       ch / DIMM_CHANNELS_PER_TS,
				       ch % DIMM_CHANNELS_PER_TS);
		if (!data->dimm.labels[ch])
			return -ENOMEM;
	}

	return 0;
}

int create_hwmon_sensor_device(struct device *dev, struct sbrmi_data *data)
{
	struct device *hwmon_dev;
	int ret;

	/* amd,dpc: dimms-per-channel, default 1 */
	data->dimm.dpc = 1;
	if (device_property_present(dev, "amd,dpc")) {
		ret = device_property_read_u8(dev, "amd,dpc", &data->dimm.dpc);
		if (ret) {
			dev_err(dev, "failed to read amd,dpc: %d\n", ret);
			return ret;
		}
	}
	if (data->dimm.dpc != 1 && data->dimm.dpc != 2) {
		dev_err(dev, "amd,dpc must be 1 or 2, got %u\n",
			data->dimm.dpc);
		return -EINVAL;
	}

	ret = sbrmi_init_dimm_config(dev, data);
	if (ret)
		return ret;

	ret = sbrmi_init_dimm_labels(dev, data);
	if (ret)
		return ret;

	hwmon_dev = devm_hwmon_device_register_with_info(dev, "sbrmi", data,
							 &sbrmi_chip_info, NULL);
	return PTR_ERR_OR_ZERO(hwmon_dev);
}
