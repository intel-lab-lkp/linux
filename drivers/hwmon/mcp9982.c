// SPDX-License-Identifier: GPL-2.0+
/*
 * HWMON driver for MCP998X/33 and MCP998XD/33D Multichannel Automotive
 * Temperature Monitor Family
 *
 * Copyright (C) 2025 Microchip Technology Inc. and its subsidiaries
 *
 * Author: Victor Duicu <victor.duicu@microchip.com>
 *
 * Datasheet can be found here:
 * https://ww1.microchip.com/downloads/aemDocuments/documents/MSLD/ProductDocuments/DataSheets/MCP998X-Family-Data-Sheet-DS20006827.pdf
 */

#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/device/devres.h>
#include <linux/device.h>
#include <linux/dev_printk.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/time64.h>
#include <linux/unaligned.h>

/* MCP9982 Registers */
#define MCP9982_HIGH_BYTE_ADDR(index)		(2 * (index))
#define MCP9982_ONE_SHOT_ADDR			0x0A
#define MCP9982_INTERNAL_HIGH_LIMIT_ADDR	0x0B
#define MCP9982_INTERNAL_LOW_LIMIT_ADDR		0x0C
#define MCP9982_EXT1_HIGH_LIMIT_HIGH_BYTE_ADDR	0x0D
#define MCP9982_EXT1_HIGH_LIMIT_LOW_BYTE_ADDR	0x0E
#define MCP9982_EXT1_LOW_LIMIT_HIGH_BYTE_ADDR	0x0F
#define MCP9982_EXT1_LOW_LIMIT_LOW_BYTE_ADDR	0x10
#define MCP9982_INTERNAL_THERM_LIMIT_ADDR	0x1D
#define MCP9982_EXT1_THERM_LIMIT_ADDR		0x1E
#define MCP9982_CFG_ADDR			0x22
#define MCP9982_CONV_ADDR			0x24
#define MCP9982_HYS_ADDR			0x25
#define MCP9982_CONSEC_ALRT_ADDR		0x26
#define MCP9982_ALRT_CFG_ADDR			0x27
#define MCP9982_RUNNING_AVG_ADDR		0x28
#define MCP9982_HOTTEST_CFG_ADDR		0x29
#define MCP9982_STATUS_ADDR			0x2A
#define MCP9982_EXT_FAULT_STATUS_ADDR		0x2B
#define MCP9982_HIGH_LIMIT_STATUS_ADDR		0x2C
#define MCP9982_LOW_LIMIT_STATUS_ADDR		0x2D
#define MCP9982_THERM_LIMIT_STATUS_ADDR		0x2E
#define MCP9982_HOTTEST_HIGH_BYTE_ADDR		0x2F
#define MCP9982_HOTTEST_LOW_BYTE_ADDR		0x30
#define MCP9982_HOTTEST_STATUS_ADDR		0x31
#define MCP9982_THERM_SHTDWN_CFG_ADDR		0x32
#define MCP9982_HRDW_THERM_SHTDWN_LIMIT_ADDR	0x33
#define MCP9982_EXT_BETA1_CFG_ADDR		0x34
#define MCP9982_EXT_BETA2_CFG_ADDR		0x35
#define MCP9982_EXT_IDEAL1_ADDR			0x36
#define MCP9982_EXT_IDEAL2_ADDR			0x37
#define MCP9982_EXT_IDEAL3_ADDR			0x38
#define MCP9982_EXT_IDEAL4_ADDR			0x39
/* 80h is the start address for temperature memory block */
#define MCP9982_TEMP_MEM_BLOCK_ADDR(index)	(2 * (index) + 0x80)
/* Addresses in the STATUS MEMORY BLOCK */
#define MCP9982_STATUS_BLOCK_MEMORY		0x90
#define MCP9982_STATUS_BLOCK_DIODE_FAULT	0x91
#define MCP9982_STATUS_BLOCK_HIGH_LIMIT		0x92
#define MCP9982_STATUS_BLOCK_LOW_LIMIT		0x93
#define MCP9982_STATUS_BLOCK_THERM_LIMIT	0x94
#define MCP9982_STATUS_BLOCK_HOTTEST_HIGH_BYTE	0x95
#define MCP9982_STATUS_BLOCK_HOTTEST_LOW_BYTE	0x96
#define MCP9982_STATUS_BLOCK_HOTTEST		0x97

/* MCP9982 Bits */
#define MCP9982_CFG_MSKAL			BIT(7)
#define MCP9982_CFG_RS				BIT(6)
#define MCP9982_CFG_ATTHM			BIT(5)
#define MCP9982_CFG_RECD12			BIT(4)
#define MCP9982_CFG_RECD34			BIT(3)
#define MCP9982_CFG_RANGE			BIT(2)
#define MCP9982_CFG_DA_ENA			BIT(1)
#define MCP9982_CFG_APDD			BIT(0)

#define MCP9982_STATUS_BUSY			BIT(5)

/* The maximum number of channels a member of the family can have */
#define MCP9982_MAX_NUM_CHANNELS		5
#define MCP9982_BETA_AUTODETECT			16
#define MCP9982_IDEALITY_DEFAULT		18
#define MCP9982_OFFSET				64
#define MCP9982_SCALE				256
#define MCP9982_DEFAULT_CONSEC_ALRT_VAL		112
#define MCP9982_DEFAULT_HYS_VAL			10
#define MCP9982_DEFAULT_CONV_VAL		6
#define MCP9982_WAKE_UP_TIME_MS			125
#define MCP9982_CONVERSION_TIME_MS		125

static const struct hwmon_channel_info * const mcp9985_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(chip,
			   HWMON_C_UPDATE_INTERVAL),
	NULL
};

/**
 * struct mcp9982_features - features of a mcp9982 instance
 * @name:			chip's name
 * @phys_channels:		number of physical channels supported by the chip
 * @hw_thermal_shutdown:	presence of hardware thermal shutdown circuitry
 * @allow_apdd:			whether the chip supports enabling APDD
 */
struct mcp9982_features {
	const char	*name;
	u8		phys_channels;
	bool		hw_thermal_shutdown;
	bool		allow_apdd;
};

static const struct mcp9982_features mcp9933_chip_config = {
	.name = "mcp9933",
	.phys_channels = 3,
	.hw_thermal_shutdown = false,
	.allow_apdd = true,
};

static const struct mcp9982_features mcp9933d_chip_config = {
	.name = "mcp9933d",
	.phys_channels = 3,
	.hw_thermal_shutdown = true,
	.allow_apdd = true,
};

static const struct mcp9982_features mcp9982_chip_config = {
	.name = "mcp9982",
	.phys_channels = 2,
	.hw_thermal_shutdown = false,
	.allow_apdd = false,
};

static const struct mcp9982_features mcp9982d_chip_config = {
	.name = "mcp9982d",
	.phys_channels = 2,
	.hw_thermal_shutdown = true,
	.allow_apdd = false,
};

static const struct mcp9982_features mcp9983_chip_config = {
	.name = "mcp9983",
	.phys_channels = 3,
	.hw_thermal_shutdown = false,
	.allow_apdd = false,
};

static const struct mcp9982_features mcp9983d_chip_config = {
	.name = "mcp9983d",
	.phys_channels = 3,
	.hw_thermal_shutdown = true,
	.allow_apdd = false,
};

static const struct mcp9982_features mcp9984_chip_config = {
	.name = "mcp9984",
	.phys_channels = 4,
	.hw_thermal_shutdown = false,
	.allow_apdd = true,
};

static const struct mcp9982_features mcp9984d_chip_config = {
	.name = "mcp9984d",
	.phys_channels = 4,
	.hw_thermal_shutdown = true,
	.allow_apdd = true,
};

static const struct mcp9982_features mcp9985_chip_config = {
	.name = "mcp9985",
	.phys_channels = 5,
	.hw_thermal_shutdown = false,
	.allow_apdd = true,
};

static const struct mcp9982_features mcp9985d_chip_config = {
	.name = "mcp9985d",
	.phys_channels = 5,
	.hw_thermal_shutdown = true,
	.allow_apdd = true,
};

static const unsigned int mcp9982_update_interval[11] = {
	16000,
	8000,
	4000,
	2000,
	1000,
	500,
	250,
	125,
	64,
	32,
	16,
};

/* MCP9982 regmap configuration */
static const struct regmap_range mcp9982_regmap_wr_ranges[] = {
	regmap_reg_range(MCP9982_ONE_SHOT_ADDR, MCP9982_EXT1_LOW_LIMIT_LOW_BYTE_ADDR),
	regmap_reg_range(MCP9982_INTERNAL_THERM_LIMIT_ADDR, MCP9982_EXT1_THERM_LIMIT_ADDR),
	regmap_reg_range(MCP9982_CFG_ADDR, MCP9982_CFG_ADDR),
	regmap_reg_range(MCP9982_CONV_ADDR, MCP9982_HOTTEST_CFG_ADDR),
	regmap_reg_range(MCP9982_THERM_SHTDWN_CFG_ADDR, MCP9982_THERM_SHTDWN_CFG_ADDR),
	regmap_reg_range(MCP9982_EXT_BETA1_CFG_ADDR, MCP9982_EXT_IDEAL4_ADDR),
};

static const struct regmap_access_table mcp9982_regmap_wr_table = {
	.yes_ranges = mcp9982_regmap_wr_ranges,
	.n_yes_ranges = ARRAY_SIZE(mcp9982_regmap_wr_ranges),
};

static const struct regmap_range mcp9982_regmap_rd_ranges[] = {
	regmap_reg_range(MCP9982_HIGH_BYTE_ADDR(0), MCP9982_EXT1_LOW_LIMIT_LOW_BYTE_ADDR),
	regmap_reg_range(MCP9982_INTERNAL_THERM_LIMIT_ADDR, MCP9982_EXT1_THERM_LIMIT_ADDR),
	regmap_reg_range(MCP9982_CFG_ADDR, MCP9982_CFG_ADDR),
	regmap_reg_range(MCP9982_CONV_ADDR, MCP9982_EXT_IDEAL4_ADDR),
	regmap_reg_range(MCP9982_TEMP_MEM_BLOCK_ADDR(0), MCP9982_STATUS_BLOCK_HOTTEST),
};

static const struct regmap_access_table mcp9982_regmap_rd_table = {
	.yes_ranges = mcp9982_regmap_rd_ranges,
	.n_yes_ranges = ARRAY_SIZE(mcp9982_regmap_rd_ranges),
};

static bool mcp9982_is_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case MCP9982_ONE_SHOT_ADDR:
	case MCP9982_INTERNAL_HIGH_LIMIT_ADDR:
	case MCP9982_INTERNAL_LOW_LIMIT_ADDR:
	case MCP9982_EXT1_HIGH_LIMIT_HIGH_BYTE_ADDR:
	case MCP9982_EXT1_HIGH_LIMIT_LOW_BYTE_ADDR:
	case MCP9982_EXT1_LOW_LIMIT_HIGH_BYTE_ADDR:
	case MCP9982_EXT1_LOW_LIMIT_LOW_BYTE_ADDR:
	case MCP9982_INTERNAL_THERM_LIMIT_ADDR:
	case MCP9982_EXT1_THERM_LIMIT_ADDR:
	case MCP9982_CFG_ADDR:
	case MCP9982_CONV_ADDR:
	case MCP9982_HYS_ADDR:
	case MCP9982_CONSEC_ALRT_ADDR:
	case MCP9982_ALRT_CFG_ADDR:
	case MCP9982_RUNNING_AVG_ADDR:
	case MCP9982_HOTTEST_CFG_ADDR:
	case MCP9982_THERM_SHTDWN_CFG_ADDR:
		return false;
	default:
		return true;
	}
}

static const struct regmap_config mcp9982_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.rd_table = &mcp9982_regmap_rd_table,
	.wr_table = &mcp9982_regmap_wr_table,
	.volatile_reg = mcp9982_is_volatile_reg,
	.max_register = MCP9982_STATUS_BLOCK_HOTTEST,
	.cache_type = REGCACHE_MAPLE,
};

/**
 * struct mcp9992_priv - information about chip parameters
 * @regmap:			device register map
 * @chip:			pointer to structure holding chip features
 * @labels:			labels of the channels
 * @sampl_idx:			index representing the current sampling frequency
 * @time_limit:			time when it is safe to read
 * @enabled_channel_mask:	mask containing which channels should be enabled
 * @num_channels:		number of active physical channels
 * @recd34_enable:		state of Resistance Error Correction(REC) on channels 3 and 4
 * @recd12_enable:		state of Resistance Error Correction(REC) on channels 1 and 2
 * @apdd_enable:		state of anti-parallel diode mode
 * @run_state:			chip is in run state, otherwise is in standby state
 * @wait_before_read:		whether we need to wait a delay before reading a new value
 */
struct mcp9982_priv {
	struct regmap *regmap;
	const struct mcp9982_features *chip;
	const char *labels[MCP9982_MAX_NUM_CHANNELS];
	unsigned int sampl_idx;
	unsigned long  time_limit;
	unsigned long enabled_channel_mask;
	u8 num_channels;
	bool recd34_enable;
	bool recd12_enable;
	bool apdd_enable;
	bool run_state;
	bool wait_before_read;
};

static int mcp9982_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	unsigned int reg_status;
	struct mcp9982_priv *priv = dev_get_drvdata(dev);
	int ret;
	u8 bulk_read[3];

	if (priv->run_state) {
		/*
		 * When working in Run mode, after modifying a parameter (like update
		 * interval) we have to wait a delay before reading the new values.
		 * We can't determine when the conversion is done based on the BUSY bit.
		 */
		if (priv->wait_before_read) {
			if (!time_after(jiffies, priv->time_limit))
				mdelay(jiffies_to_msecs(priv->time_limit - jiffies));
			priv->wait_before_read = false;
		}
	} else {
		ret = regmap_write(priv->regmap, MCP9982_ONE_SHOT_ADDR, 1);
		if (ret)
			return ret;
		/*
		 * In Standby state after writing in OneShot register wait for
		 * the start of conversion and then poll the BUSY bit.
		 */
		mdelay(MCP9982_WAKE_UP_TIME_MS);
		ret = regmap_read_poll_timeout(priv->regmap, MCP9982_STATUS_ADDR,
					       reg_status, !(reg_status & MCP9982_STATUS_BUSY),
					       (mcp9982_update_interval[priv->sampl_idx]) *
					       USEC_PER_MSEC, 0);
		if (ret)
			return ret;
	}

	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
			/*
			 * The Block Read Protocol first returns the number of user readable
			 * bytes, held in bulk_read[0], followed by the data.
			 */
			ret = regmap_bulk_read(priv->regmap, MCP9982_TEMP_MEM_BLOCK_ADDR(channel),
					       &bulk_read, sizeof(bulk_read));

			if (ret)
				return ret;

			*val = ((get_unaligned_be16(bulk_read + 1) >> 5) -
			       (MCP9982_OFFSET << 3)) * 125;

			return 0;
		default:
			return -EINVAL;
		}

	case hwmon_chip:
		switch (attr) {
		case hwmon_chip_update_interval:
			*val = mcp9982_update_interval[priv->sampl_idx];
			return 0;
		default:
			return -EINVAL;
		}
	default:
		return -EINVAL;
	}
}

static int mcp9982_read_label(struct device *dev,
			      enum hwmon_sensor_types type,
			      u32 attr, int channel, const char **str)
{
	struct mcp9982_priv *priv = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_label:
			if (priv->labels[channel]) {
				*str = priv->labels[channel];
				return 0;
			} else {
				return -EOPNOTSUPP;
			}
		default:
			return -EOPNOTSUPP;
		}
	default:
		return -EOPNOTSUPP;
	}
}

static int mcp9982_write(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long val)
{
	unsigned int i, previous_sampl_idx;
	struct mcp9982_priv *priv = dev_get_drvdata(dev);
	unsigned long new_time_limit;
	bool use_previous_freq = false;
	int ret;

	switch (type) {
	case hwmon_chip:
		switch (attr) {
		case hwmon_chip_update_interval:
			previous_sampl_idx = priv->sampl_idx;

			/*
			 * For MCP998XD and MCP9933D update interval
			 * can't be slower than 1 second.
			 */
			i = priv->chip->hw_thermal_shutdown ? 4 : 0;
			for (; i < ARRAY_SIZE(mcp9982_update_interval); i++)
				if (val == mcp9982_update_interval[i])
					break;

			if (i == ARRAY_SIZE(mcp9982_update_interval))
				return -EINVAL;

			ret = regmap_write(priv->regmap, MCP9982_CONV_ADDR, i);
			if (ret)
				return ret;

			priv->sampl_idx = i;

			/*
			 * When changing the frequency in Run mode, wait a delay based
			 * on the previous value to ensure the new value becomes active.
			 */
			if (priv->run_state)
				use_previous_freq = true;
			else
				return 0;
			break;
		default:
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}

	/* update conversion delay in runmode */
	if (use_previous_freq) {
		new_time_limit = msecs_to_jiffies(mcp9982_update_interval[previous_sampl_idx]);
		use_previous_freq = false;
	} else {
		new_time_limit = msecs_to_jiffies(mcp9982_update_interval[priv->sampl_idx]);
	}

	new_time_limit += jiffies + msecs_to_jiffies(MCP9982_CONVERSION_TIME_MS);

	if (time_after(new_time_limit, priv->time_limit)) {
		priv->time_limit = new_time_limit;
		priv->wait_before_read = true;
	}

	return 0;
}

static umode_t mcp9982_is_visible(const void *_data,
				  enum hwmon_sensor_types type,
				  u32 attr, int channel)
{
	const struct mcp9982_priv *priv = _data;

	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_label:
			if (priv->labels[channel])
				return 0444;
			else
				return 0;
		case hwmon_temp_input:
			if (test_bit(channel, &priv->enabled_channel_mask))
				return 0444;
			else
				return 0;
		default:
			return 0;
		}
	case hwmon_chip:
		switch (attr) {
		case hwmon_chip_update_interval:
			return 0644;
		default:
			return 0;
		}
	default:
		return 0;
	}
}

static const struct hwmon_ops mcp9982_hwmon_ops = {
	.is_visible = mcp9982_is_visible,
	.read = mcp9982_read,
	.read_string = mcp9982_read_label,
	.write = mcp9982_write,
};

static int mcp9982_init(struct device *dev, struct mcp9982_priv *priv)
{
	int ret;
	u8 val;

	/* Chips 82/83 and 82D/83D do not support anti-parallel diode mode. */
	if (!priv->chip->allow_apdd && priv->apdd_enable == 1)
		return dev_err_probe(dev, -EINVAL, "Incorrect setting of APDD.\n");

	/* Chips with "D" work in Run state and those without work in Standby state. */
	if (priv->chip->hw_thermal_shutdown)
		priv->run_state = true;

	/*
	 * For chips with "D" in the name, resistance error correction must be on
	 * so that hardware shutdown feature can't be overridden.
	 */
	if (priv->chip->hw_thermal_shutdown)
		if (!priv->recd34_enable || !priv->recd12_enable)
			return dev_err_probe(dev, -EINVAL, "Incorrect setting of RECD.\n");
	/*
	 * Set default values in registers.
	 * APDD, RECD12 and RECD34 are active on 0.
	 */
	val = FIELD_PREP(MCP9982_CFG_MSKAL, 1) |
	      FIELD_PREP(MCP9982_CFG_RS, !priv->run_state) |
	      FIELD_PREP(MCP9982_CFG_ATTHM, 1) |
	      FIELD_PREP(MCP9982_CFG_RECD12, !priv->recd12_enable) |
	      FIELD_PREP(MCP9982_CFG_RECD34, !priv->recd34_enable) |
	      FIELD_PREP(MCP9982_CFG_RANGE, 1) | FIELD_PREP(MCP9982_CFG_DA_ENA, 0) |
	      FIELD_PREP(MCP9982_CFG_APDD, !priv->apdd_enable);

	ret = regmap_write(priv->regmap, MCP9982_CFG_ADDR, val);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, MCP9982_CONV_ADDR, MCP9982_DEFAULT_CONV_VAL);
	if (ret)
		return ret;
	priv->sampl_idx = MCP9982_DEFAULT_CONV_VAL;

	ret = regmap_write(priv->regmap, MCP9982_HYS_ADDR, MCP9982_DEFAULT_HYS_VAL);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, MCP9982_CONSEC_ALRT_ADDR,
			   MCP9982_DEFAULT_CONSEC_ALRT_VAL);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, MCP9982_RUNNING_AVG_ADDR, 0);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, MCP9982_HOTTEST_CFG_ADDR, 0);
	if (ret)
		return ret;

	/*
	 * Only external channels 1 and 2 support beta compensation.
	 * Set beta auto-detection.
	 */
	ret = regmap_write(priv->regmap, MCP9982_EXT_BETA1_CFG_ADDR, MCP9982_BETA_AUTODETECT);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, MCP9982_EXT_BETA2_CFG_ADDR, MCP9982_BETA_AUTODETECT);
	if (ret)
		return ret;

	/* Set ideality factor to default for all external channels. */
	ret = regmap_write(priv->regmap, MCP9982_EXT_IDEAL1_ADDR, MCP9982_IDEALITY_DEFAULT);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, MCP9982_EXT_IDEAL2_ADDR, MCP9982_IDEALITY_DEFAULT);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, MCP9982_EXT_IDEAL3_ADDR, MCP9982_IDEALITY_DEFAULT);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, MCP9982_EXT_IDEAL4_ADDR, MCP9982_IDEALITY_DEFAULT);
	if (ret)
		return ret;

	priv->wait_before_read = false;
	priv->time_limit = jiffies;

	return 0;
}

static int mcp9982_parse_fw_config(struct device *dev, int device_nr_channels)
{
	unsigned int reg_nr;
	struct mcp9982_priv *priv = dev_get_drvdata(dev);

	/* For unit tests */
	if (!dev_fwnode(dev)) {
		priv->num_channels = device_nr_channels;
		priv->enabled_channel_mask = BIT(priv->num_channels) - 1;
		priv->apdd_enable = false;
		priv->recd12_enable = true;
		priv->recd34_enable = true;
		return 0;
	}

	priv->apdd_enable =
		device_property_read_bool(dev, "microchip,enable-anti-parallel");

	priv->recd12_enable =
		device_property_read_bool(dev, "microchip,parasitic-res-on-channel1-2");

	priv->recd34_enable =
		device_property_read_bool(dev, "microchip,parasitic-res-on-channel3-4");

	priv->num_channels = device_get_child_node_count(dev) + 1;

	if (priv->num_channels > device_nr_channels)
		return dev_err_probe(dev, -E2BIG,
				     "More channels than the chip supports.\n");

	/* Initialise internal channel( which is always present ). */
	priv->labels[0] = "internal diode";
	priv->enabled_channel_mask = 1;

	device_for_each_child_node_scoped(dev, child) {
		reg_nr = 0;
		fwnode_property_read_u32(child, "reg", &reg_nr);
		if (!reg_nr || reg_nr >= device_nr_channels)
			return dev_err_probe(dev, -EINVAL,
			  "The index of the channels does not match the chip.\n");

		fwnode_property_read_string(child, "label", &priv->labels[reg_nr]);
		set_bit(reg_nr, &priv->enabled_channel_mask);
	}

	return 0;
}

static int mcp9982_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct hwmon_chip_info mcp998x_chip_info;
	struct mcp9982_priv *priv;
	const struct mcp9982_features *chip;
	struct device *hwmon_dev;
	int ret;

	priv = devm_kzalloc(dev, sizeof(struct mcp9982_priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->regmap = devm_regmap_init_i2c(client, &mcp9982_regmap_config);

	if (IS_ERR(priv->regmap))
		return dev_err_probe(dev, PTR_ERR(priv->regmap),
				     "Cannot initialize register map.\n");

	dev_set_drvdata(dev, priv);

	chip = i2c_get_match_data(client);
	if (!chip)
		return -EINVAL;
	priv->chip = chip;

	ret = mcp9982_parse_fw_config(dev, chip->phys_channels);
	if (ret)
		return ret;

	ret = mcp9982_init(dev, priv);
	if (ret)
		return ret;

	mcp998x_chip_info.ops = &mcp9982_hwmon_ops;
	mcp998x_chip_info.info = mcp9985_info;

	hwmon_dev = devm_hwmon_device_register_with_info(dev, chip->name, priv,
							 &mcp998x_chip_info, NULL);

	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct i2c_device_id mcp9982_id[] = {
	{ .name = "mcp9933", .driver_data = (kernel_ulong_t)&mcp9933_chip_config },
	{ .name = "mcp9933d", .driver_data = (kernel_ulong_t)&mcp9933d_chip_config },
	{ .name = "mcp9982", .driver_data = (kernel_ulong_t)&mcp9982_chip_config },
	{ .name = "mcp9982d", .driver_data = (kernel_ulong_t)&mcp9982d_chip_config },
	{ .name = "mcp9983", .driver_data = (kernel_ulong_t)&mcp9983_chip_config },
	{ .name = "mcp9983d", .driver_data = (kernel_ulong_t)&mcp9983d_chip_config },
	{ .name = "mcp9984", .driver_data = (kernel_ulong_t)&mcp9984_chip_config },
	{ .name = "mcp9984d", .driver_data = (kernel_ulong_t)&mcp9984d_chip_config },
	{ .name = "mcp9985", .driver_data = (kernel_ulong_t)&mcp9985_chip_config },
	{ .name = "mcp9985d", .driver_data = (kernel_ulong_t)&mcp9985d_chip_config },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mcp9982_id);

static const struct of_device_id mcp9982_of_match[] = {
	{
		.compatible = "microchip,mcp9933",
		.data = &mcp9933_chip_config,
	}, {
		.compatible = "microchip,mcp9933d",
		.data = &mcp9933d_chip_config,
	}, {
		.compatible = "microchip,mcp9982",
		.data = &mcp9982_chip_config,
	}, {
		.compatible = "microchip,mcp9982d",
		.data = &mcp9982d_chip_config,
	}, {
		.compatible = "microchip,mcp9983",
		.data = &mcp9983_chip_config,
	}, {
		.compatible = "microchip,mcp9983d",
		.data = &mcp9983d_chip_config,
	}, {
		.compatible = "microchip,mcp9984",
		.data = &mcp9984_chip_config,
	}, {
		.compatible = "microchip,mcp9984d",
		.data = &mcp9984d_chip_config,
	}, {
		.compatible = "microchip,mcp9985",
		.data = &mcp9985_chip_config,
	}, {
		.compatible = "microchip,mcp9985d",
		.data = &mcp9985d_chip_config,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, mcp9982_of_match);

static struct i2c_driver mcp9982_driver = {
	.driver	 = {
		.name = "mcp9982",
		.of_match_table = mcp9982_of_match,
	},
	.probe = mcp9982_probe,
	.id_table = mcp9982_id,
};
module_i2c_driver(mcp9982_driver);

MODULE_AUTHOR("Victor Duicu <victor.duicu@microchip.com>");
MODULE_DESCRIPTION("MCP998X/33 and MCP998XD/33D Multichannel Automotive Temperature Monitor Driver");
MODULE_LICENSE("GPL");
