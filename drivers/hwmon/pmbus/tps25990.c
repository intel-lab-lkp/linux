// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2024 BayLibre, SAS.
// Author: Jerome Brunet <jbrunet@baylibre.com>

#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/hwmon-sysfs.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "pmbus.h"

#define TPS25990_READ_VAUX		0xd0
#define TPS25990_READ_VIN_MIN		0xd1
#define TPS25990_READ_VIN_PEAK		0xd2
#define TPS25990_READ_IIN_PEAK		0xd4
#define TPS25990_READ_PIN_PEAK		0xd5
#define TPS25990_READ_TEMP_AVG		0xd6
#define TPS25990_READ_TEMP_PEAK		0xd7
#define TPS25990_READ_VOUT_MIN		0xda
#define TPS25990_READ_VIN_AVG		0xdc
#define TPS25990_READ_VOUT_AVG		0xdd
#define TPS25990_READ_IIN_AVG		0xde
#define TPS25990_READ_PIN_AVG		0xdf
#define TPS25990_VIREF			0xe0
#define TPS25990_PK_MIN_AVG		0xea
#define  PK_MIN_AVG_RST_PEAK		BIT(7)
#define  PK_MIN_AVG_RST_AVG		BIT(6)
#define  PK_MIN_AVG_RST_MIN		BIT(5)
#define  PK_MIN_AVG_AVG_CNT		GENMASK(2, 0)
#define TPS25990_MFR_WRITE_PROTECT	0xf8
#define  TPS25990_UNLOCKED		BIT(7)

#define TPS25990_8B_SHIFT		2
#define TPS25990_VIN_OVF_NUM		525100
#define TPS25990_VIN_OVF_DIV		10163
#define TPS25990_VIN_OVF_OFF		155
#define TPS25990_IIN_OCF_NUM		953800
#define TPS25990_IIN_OCF_DIV		129278
#define TPS25990_IIN_OCF_OFF		157

#define TPS25990_DEFAULT_RIMON		910000

static int tps25990_mfr_write_protect(struct i2c_client *client, bool protect)
{
	return pmbus_write_byte_data(client, -1, TPS25990_MFR_WRITE_PROTECT,
				     protect ? 0x0 : 0xa2);
}

static int tps25990_mfr_write_protect_active(struct i2c_client *client)
{
	int ret = pmbus_read_byte_data(client, -1, TPS25990_MFR_WRITE_PROTECT);

	if (ret < 0)
		return ret;

	return !(ret & TPS25990_UNLOCKED);
}

#if IS_ENABLED(CONFIG_DEBUG_FS)
static int tps25990_write_protect_get(void *data, u64 *val)
{
	struct i2c_client *client = data;

	return tps25990_mfr_write_protect_active(client);
}

static int tps25990_write_protect_set(void *data, u64 val)
{
	struct i2c_client *client = data;

	if (val > 1)
		return -EINVAL;

	return tps25990_mfr_write_protect(client, val);
}

DEFINE_DEBUGFS_ATTRIBUTE(tps25990_write_protect_fops,
			 tps25990_write_protect_get,
			 tps25990_write_protect_set,
			 "%llu\n");

static int tps25990_init_debugfs(struct i2c_client *client)
{
	struct dentry *dir;

	dir = pmbus_get_debugfs_dir(client);
	if (!dir)
		return -ENOENT;

	debugfs_create_file("write_protect", 0644, dir,
			    client, &tps25990_write_protect_fops);

	return 0;
}

#else
static inline int tps25990_init_debugfs(struct i2c_client *client)
{
	return 0;
}
#endif

/*
 * TPS25990 has history reset based on MIN/AVG/PEAK instead of per sensor type
 * Emulate the behaviour a pmbus limit_attr would have for consistency
 *  - Read: Do nothing and emit 0
 *  - Write: Check the input is a number and reset
 */
static ssize_t tps25990_history_reset_show(struct device *dev,
					   struct device_attribute *devattr,
					   char *buf)
{
	return sysfs_emit(buf, "0\n");
}

static ssize_t tps25990_history_reset_store(struct device *dev,
					    struct device_attribute *devattr,
					    const char *buf, size_t count)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct i2c_client *client = to_i2c_client(dev->parent);
	s64 val;
	int ret;

	if (kstrtos64(buf, 10, &val) < 0)
		return -EINVAL;

	ret = pmbus_update_byte_data(client, 0, TPS25990_PK_MIN_AVG,
				     BIT(attr->index), BIT(attr->index));
	if (ret < 0)
		return ret;

	return count;
}

static SENSOR_DEVICE_ATTR_RW(highest_history_reset, tps25990_history_reset, 7);
static SENSOR_DEVICE_ATTR_RW(average_history_reset, tps25990_history_reset, 6);
static SENSOR_DEVICE_ATTR_RW(lowest_history_reset,  tps25990_history_reset, 5);

static struct attribute *tps25990_attrs[] = {
	&sensor_dev_attr_highest_history_reset.dev_attr.attr,
	&sensor_dev_attr_average_history_reset.dev_attr.attr,
	&sensor_dev_attr_lowest_history_reset.dev_attr.attr,
	NULL,
};

ATTRIBUTE_GROUPS(tps25990);

static int tps25990_get_addr(int reg)
{
	switch (reg) {
	case PMBUS_SMBALERT_MASK:
		/*
		 * Note: PMBUS_SMBALERT_MASK is not implemented on this chip
		 * Writing to this address raises CML errors.
		 * Instead it provides ALERT_MASK which allows to set the mask
		 * for each of the status registers, but not the specific bits
		 * in them.
		 * The default setup assert SMBA# if any bit is set in any of the
		 * status registers the chip has. This is as close as we can get
		 * to what pmbus_irq_setup() would set, sooo ... do nothing.
		 */
		return -ENXIO;
	case PMBUS_IIN_OC_FAULT_LIMIT:
		/*
		 * VIREF directly sets the over-current limit at which the eFuse
		 * will turn the FET off and trigger a fault. Expose it through
		 * this generic property instead of a manufacturer specific one.
		 */
		return TPS25990_VIREF;
	case PMBUS_VIRT_READ_VIN_MAX:
		return TPS25990_READ_VIN_PEAK;
	case PMBUS_VIRT_READ_VIN_MIN:
		return TPS25990_READ_VIN_MIN;
	case PMBUS_VIRT_READ_VIN_AVG:
		return TPS25990_READ_VIN_AVG;
	case PMBUS_VIRT_READ_VOUT_MIN:
		return TPS25990_READ_VOUT_MIN;
	case PMBUS_VIRT_READ_VOUT_AVG:
		return TPS25990_READ_VOUT_AVG;
	case PMBUS_VIRT_READ_IIN_AVG:
		return TPS25990_READ_IIN_AVG;
	case PMBUS_VIRT_READ_IIN_MAX:
		return TPS25990_READ_IIN_PEAK;
	case PMBUS_VIRT_READ_TEMP_AVG:
		return TPS25990_READ_TEMP_AVG;
	case PMBUS_VIRT_READ_TEMP_MAX:
		return TPS25990_READ_TEMP_PEAK;
	case PMBUS_VIRT_READ_PIN_AVG:
		return TPS25990_READ_PIN_AVG;
	case PMBUS_VIRT_READ_PIN_MAX:
		return TPS25990_READ_PIN_PEAK;
	case PMBUS_VIRT_READ_VMON:
		return TPS25990_READ_VAUX;
	case PMBUS_VIRT_SAMPLES:
		return TPS25990_PK_MIN_AVG;
	}

	/* Let the register check do its job */
	if (reg < PMBUS_VIRT_BASE)
		return reg;

	return -ENXIO;
}

/*
 * Some registers use a different scale than the one registered with
 * pmbus_driver_info. An extra conversion step is necessary to adapt
 * the register value to the conversion on the sensor type
 */
static int tps25990_read_adapt_value(int reg, int val)
{
	switch (reg) {
	case PMBUS_VIN_UV_WARN_LIMIT:
	case PMBUS_VIN_UV_FAULT_LIMIT:
	case PMBUS_VIN_OV_WARN_LIMIT:
	case PMBUS_VOUT_UV_WARN_LIMIT:
	case PMBUS_IIN_OC_WARN_LIMIT:
	case PMBUS_OT_WARN_LIMIT:
	case PMBUS_OT_FAULT_LIMIT:
	case PMBUS_PIN_OP_WARN_LIMIT:
	case PMBUS_POWER_GOOD_OFF:
		/*
		 * These registers provide an 8 bits value instead of a
		 * 10bits one. Just shifting twice the register value is
		 * enough to make the sensor type conversion work, even
		 * if the datasheet provides different m, b and R for
		 * those.
		 */
		val <<= TPS25990_8B_SHIFT;
		break;

	case PMBUS_VIN_OV_FAULT_LIMIT:
		val = DIV_ROUND_CLOSEST(val * TPS25990_VIN_OVF_NUM, TPS25990_VIN_OVF_DIV);
		val += TPS25990_VIN_OVF_OFF;
		break;

	case PMBUS_IIN_OC_FAULT_LIMIT:
		val = DIV_ROUND_CLOSEST(val * TPS25990_IIN_OCF_NUM, TPS25990_IIN_OCF_DIV);
		val += TPS25990_IIN_OCF_OFF;
		break;

	case PMBUS_VIRT_SAMPLES:
		val = 1 << val;
		break;
	}

	return val;
}

static int tps25990_read_word(struct i2c_client *client,
			      int page, int phase, int reg)
{
	int ret, addr;

	addr = tps25990_get_addr(reg);
	if (addr < 0)
		return addr;

	switch (reg) {
	case PMBUS_VIRT_SAMPLES:
		ret = pmbus_read_byte_data(client, page, addr);
		ret = FIELD_GET(PK_MIN_AVG_AVG_CNT, ret);
		break;

	case PMBUS_IIN_OC_FAULT_LIMIT:
		ret = pmbus_read_byte_data(client, page, addr);
		break;

	default:
		ret = pmbus_read_word_data(client, page, -1, addr);
		break;
	}

	if (ret >= 0)
		ret = tps25990_read_adapt_value(reg, ret);

	return ret;
}

static int tps25990_write_adapt_value(int reg, int val)
{
	switch (reg) {
	case PMBUS_VIN_UV_WARN_LIMIT:
	case PMBUS_VIN_UV_FAULT_LIMIT:
	case PMBUS_VIN_OV_WARN_LIMIT:
	case PMBUS_VOUT_UV_WARN_LIMIT:
	case PMBUS_IIN_OC_WARN_LIMIT:
	case PMBUS_OT_WARN_LIMIT:
	case PMBUS_OT_FAULT_LIMIT:
	case PMBUS_PIN_OP_WARN_LIMIT:
	case PMBUS_POWER_GOOD_OFF:
		val >>= TPS25990_8B_SHIFT;
		val = clamp(val, 0, 0xff);
		break;

	case PMBUS_VIN_OV_FAULT_LIMIT:
		val -= TPS25990_VIN_OVF_OFF;
		val = DIV_ROUND_CLOSEST(val * TPS25990_VIN_OVF_DIV, TPS25990_VIN_OVF_NUM);
		val = clamp_val(val, 0, 0xf);
		break;

	case PMBUS_IIN_OC_FAULT_LIMIT:
		val -= TPS25990_IIN_OCF_OFF;
		val = DIV_ROUND_CLOSEST(val * TPS25990_IIN_OCF_DIV, TPS25990_IIN_OCF_NUM);
		val = clamp_val(val, 0, 0x3f);
		break;

	case PMBUS_VIRT_SAMPLES:
		val = clamp_val(val, 1, 1 << PK_MIN_AVG_AVG_CNT);
		val = ilog2(val);
		break;
	}

	return val;
}

static int tps25990_write_word(struct i2c_client *client,
			       int page, int reg, u16 value)
{
	int addr, ret;

	addr = tps25990_get_addr(reg);
	if (addr < 0)
		return addr;

	value = tps25990_write_adapt_value(reg, value);

	switch (reg) {
	case PMBUS_VIRT_SAMPLES:
		ret = pmbus_update_byte_data(client, page, addr,
					     PK_MIN_AVG_AVG_CNT,
					     FIELD_PREP(PK_MIN_AVG_AVG_CNT, value));
		break;

	case PMBUS_IIN_OC_FAULT_LIMIT:
		ret = pmbus_write_byte_data(client, page, addr,
					    value);
		break;

	default:
		ret = pmbus_write_word_data(client, page, addr, value);
		break;
	}

	return ret;
}

#if IS_ENABLED(CONFIG_SENSORS_TPS25990_REGULATOR)
static const struct regulator_desc tps25990_reg_desc[] = {
	PMBUS_REGULATOR_ONE("vout"),
};
#endif

static const struct pmbus_driver_info tps25990_base_info = {
	.pages = 1,
	.format[PSC_VOLTAGE_IN] = direct,
	.m[PSC_VOLTAGE_IN] = 5251,
	.b[PSC_VOLTAGE_IN] = 0,
	.R[PSC_VOLTAGE_IN] = -2,
	.format[PSC_VOLTAGE_OUT] = direct,
	.m[PSC_VOLTAGE_OUT] = 5251,
	.b[PSC_VOLTAGE_OUT] = 0,
	.R[PSC_VOLTAGE_OUT] = -2,
	.format[PSC_TEMPERATURE] = direct,
	.m[PSC_TEMPERATURE] = 140,
	.b[PSC_TEMPERATURE] = 32100,
	.R[PSC_TEMPERATURE] = -2,
	/*
	 * Current and Power measurement depends on the ohm value
	 * of Rimon. m is multiplied by 1000 below to have an integer
	 * and -3 is added to R to compensate.
	 */
	.format[PSC_CURRENT_IN] = direct,
	.m[PSC_CURRENT_IN] = 9538,
	.b[PSC_CURRENT_IN] = 0,
	.R[PSC_CURRENT_IN] = -6,
	.format[PSC_POWER] = direct,
	.m[PSC_POWER] = 4901,
	.b[PSC_POWER] = 0,
	.R[PSC_POWER] = -7,
	.func[0] = (PMBUS_HAVE_VIN |
		    PMBUS_HAVE_VOUT |
		    PMBUS_HAVE_VMON |
		    PMBUS_HAVE_IIN |
		    PMBUS_HAVE_PIN |
		    PMBUS_HAVE_TEMP |
		    PMBUS_HAVE_STATUS_VOUT |
		    PMBUS_HAVE_STATUS_IOUT |
		    PMBUS_HAVE_STATUS_INPUT |
		    PMBUS_HAVE_STATUS_TEMP |
		    PMBUS_HAVE_SAMPLES),
	.read_word_data = tps25990_read_word,
	.write_word_data = tps25990_write_word,
	.groups = tps25990_groups,

#if IS_ENABLED(CONFIG_SENSORS_TPS25990_REGULATOR)
	.reg_desc = tps25990_reg_desc,
	.num_regulators = ARRAY_SIZE(tps25990_reg_desc),
#endif
};

static const struct i2c_device_id tps25990_i2c_id[] = {
	{ "tps25990" },
	{}
};
MODULE_DEVICE_TABLE(i2c, tps25990_i2c_id);

static const struct of_device_id tps25990_of_match[] = {
	{ .compatible = "ti,tps25990" },
	{}
};
MODULE_DEVICE_TABLE(of, tps25990_of_match);

static int tps25990_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct pmbus_driver_info *info;
	u32 rimon = TPS25990_DEFAULT_RIMON;
	int ret;

	ret = device_property_read_u32(dev, "ti,rimon-milli-ohms", &rimon);
	if (ret == -EINVAL) {
		dev_warn(dev,
			 "using default rimon: current and power scale possibly wrong\n");
	} else if (ret < 0) {
		return dev_err_probe(dev, ret, "failed get rimon\n");
	}

	/*
	 * TPS25990 may be stacked with several TPS25895, allowing a higher
	 * current. The higher the allowed current is, the lower rimon
	 * will be. How low it can realistically get is unknown.
	 * To avoid problems with precision later on, rimon is provided in
	 * milli Ohms. This is a precaution to keep a stable ABI.
	 * At the moment, doing the calculation with rimon in milli Ohms
	 * would overflow the s32 'm' in the direct conversion. Convert it
	 * back to Ohms until greater precision is actually needed.
	 */
	rimon /= 1000;

	info = devm_kmemdup(dev, &tps25990_base_info, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	/* Adapt the current and power scale for each instance */
	info->m[PSC_CURRENT_IN] *= rimon;
	info->m[PSC_POWER] *= rimon;

	ret = pmbus_do_probe(client, info);
	if (ret < 0)
		return ret;

	return tps25990_init_debugfs(client);
}

static struct i2c_driver tps25990_driver = {
	.driver = {
		.name = "tps25990",
		.of_match_table = tps25990_of_match,
	},
	.probe = tps25990_probe,
	.id_table = tps25990_i2c_id,
};
module_i2c_driver(tps25990_driver);

MODULE_AUTHOR("Jerome Brunet <jbrunet@baylibre.com>");
MODULE_DESCRIPTION("PMBUS driver for TPS25990 eFuse");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(PMBUS);
