// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Hardware monitoring driver for MPS Multi-phase Digital VR Controllers(MP2891)
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include "pmbus.h"

/* Vendor specific registers, the register READ_PIN_EST(0x94),
 * MFR_VOUT_LOOP_CTRL(0xBD) and READ_IIN_EST(0x95) redefine
 * the standard PMBUS register.
 */
#define MFR_VOUT_LOOP_CTRL      0xBD
#define READ_PIN_EST            0x94
#define READ_IIN_EST            0x95

#define MP2891_PAGE_NUM			2

#define MP2891_RAIL1_FUNC	(PMBUS_HAVE_VIN | PMBUS_HAVE_VOUT | \
							PMBUS_HAVE_IOUT | PMBUS_HAVE_TEMP | \
							PMBUS_HAVE_POUT | PMBUS_HAVE_PIN | \
							PMBUS_HAVE_IIN | PMBUS_PHASE_VIRTUAL)

#define MP2891_RAIL2_FUNC	(PMBUS_HAVE_VOUT | PMBUS_HAVE_IOUT | \
							PMBUS_HAVE_TEMP | PMBUS_HAVE_POUT | \
							PMBUS_HAVE_IIN | PMBUS_PHASE_VIRTUAL)

struct mp2891_data {
	struct pmbus_driver_info info;
};

#define to_mp2891_data(x) container_of(x, struct mp2891_data, info)

static int mp2891_read_byte_data(struct i2c_client *client, int page, int reg)
{
	int ret;

	switch (reg) {
	case PMBUS_VOUT_MODE:
		ret = PB_VOUT_MODE_DIRECT;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int mp2891_read_word_data(struct i2c_client *client, int page, int phase,
			      int reg)
{
	int ret;

	switch (reg) {
	case PMBUS_READ_VIN:
	case PMBUS_READ_IOUT:
	case PMBUS_READ_POUT:
	case PMBUS_READ_VOUT:
	case PMBUS_READ_TEMPERATURE_1:
		ret = pmbus_read_word_data(client, page, phase, reg);
		break;
	case PMBUS_READ_IIN:
		ret = pmbus_read_word_data(client, page, phase, READ_IIN_EST);
		break;
	case PMBUS_READ_PIN:
		ret = pmbus_read_word_data(client, page, phase, READ_PIN_EST);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int
mp2891_identify_vout_scale(struct i2c_client *client, struct mp2891_data *data,
							u32 reg, int page)
{
	int ret;

	ret = i2c_smbus_write_byte_data(client, PMBUS_PAGE, page);
	if (ret < 0)
		return ret;

	ret = i2c_smbus_read_word_data(client, reg);
	if (ret < 0)
		return ret;

	/*
	 * Obtain vout scale from the register MFR_VOUT_LOOP_CTRL, bits 15-14,bit 13.
	 * If MFR_VOUT_LOOP_CTRL[13] = 1, the vout scale is below:
	 * 2.5mV/LSB
	 * If MFR_VOUT_LOOP_CTRL[13] = 0, the vout scale is decided by
	 * MFR_VOUT_LOOP_CTRL[15:14]:
	 * 00b - 6.25mV/LSB, 01b - 5mV/LSB, 10b - 2mV/LSB, 11b - 1mV
	 */
	if (ret & GENMASK(13, 13)) {
		data->info.m[PSC_VOLTAGE_OUT] = 4;
		data->info.R[PSC_VOLTAGE_OUT] = -1;
		data->info.b[PSC_VOLTAGE_OUT] = 0;
	} else {
		ret = (ret & GENMASK(15, 14)) >> 14;
		if (ret == 0) {
			data->info.m[PSC_VOLTAGE_OUT] = 16;
			data->info.R[PSC_VOLTAGE_OUT] = -2;
			data->info.b[PSC_VOLTAGE_OUT] = 0;
		} else if (ret == 1) {
			data->info.m[PSC_VOLTAGE_OUT] = 2;
			data->info.R[PSC_VOLTAGE_OUT] = -1;
			data->info.b[PSC_VOLTAGE_OUT] = 0;
		} else if (ret == 2) {
			data->info.m[PSC_VOLTAGE_OUT] = 5;
			data->info.R[PSC_VOLTAGE_OUT] = -1;
			data->info.b[PSC_VOLTAGE_OUT] = 0;
		} else {
			data->info.m[PSC_VOLTAGE_OUT] = 1;
			data->info.R[PSC_VOLTAGE_OUT] = 0;
			data->info.b[PSC_VOLTAGE_OUT] = 0;
		}
	}

	return 0;
}

static int
mp2891_identify_rails_vout_scale(struct i2c_client *client, struct mp2891_data *data)
{
	int ret;

	/* Identify vout scale from register  MFR_VOUT_LOOP_CTRL. */
	/* Identify vout scale for rail 1. */
	ret = mp2891_identify_vout_scale(client, data, MFR_VOUT_LOOP_CTRL, 0);
	if (ret < 0)
		return ret;

	/* Identify vout scale for rail 2. */
	ret = mp2891_identify_vout_scale(client, data, MFR_VOUT_LOOP_CTRL, 1);

	return ret;
}

static struct pmbus_driver_info mp2891_info = {
	.pages = MP2891_PAGE_NUM,
	.format[PSC_VOLTAGE_IN] = linear,
	.format[PSC_CURRENT_IN] = linear,
	.format[PSC_CURRENT_OUT] = linear,
	.format[PSC_TEMPERATURE] = linear,
	.format[PSC_POWER] = linear,
	.format[PSC_VOLTAGE_OUT] = direct,

	.func[0] = MP2891_RAIL1_FUNC,
	.func[1] = MP2891_RAIL2_FUNC,
	.read_word_data = mp2891_read_word_data,
	.read_byte_data = mp2891_read_byte_data,
};

static int mp2891_probe(struct i2c_client *client)
{
	struct pmbus_driver_info *info;
	struct mp2891_data *data;
	int ret;

	data = devm_kzalloc(&client->dev, sizeof(struct mp2891_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	memcpy(&data->info, &mp2891_info, sizeof(*info));
	info = &data->info;

	/* Identify vout scale per rail. */
	ret = mp2891_identify_rails_vout_scale(client, data);
	if (ret < 0)
		return ret;

	return pmbus_do_probe(client, info);
}

static const struct i2c_device_id mp2891_id[] = {
	{"mp2891", 0},
	{}
};
MODULE_DEVICE_TABLE(i2c, mp2891_id);

static const struct of_device_id __maybe_unused mp2891_of_match[] = {
	{.compatible = "mps,mp2891"},
	{}
};
MODULE_DEVICE_TABLE(of, mp2891_of_match);

static struct i2c_driver mp2891_driver = {
	.driver = {
		.name = "mp2891",
		.of_match_table = mp2891_of_match,
	},
	.probe = mp2891_probe,
	.id_table = mp2891_id,
};

module_i2c_driver(mp2891_driver);

MODULE_AUTHOR("Noah Wang <noahwang.wang@outlook.com>");
MODULE_DESCRIPTION("PMBus driver for MPS MP2891 device");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(PMBUS);
