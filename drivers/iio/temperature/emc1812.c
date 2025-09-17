// SPDX-License-Identifier: GPL-2.0+
/*
 * IIO driver for Microchip EMC1812/13/14/15/33 Multichannel high-accuracy 2-wire
 * low-voltage remote diode temperature monitor family.
 *
 * Copyright (C) 2025 Microchip Technology Inc. and its subsidiaries
 *
 * Author: Marius Cristea <marius.cristea@microchip.com>
 *
 * Datasheet can be found here:
 * https://ww1.microchip.com/downloads/aemDocuments/documents/MSLD/ProductDocuments/DataSheets/EMC1812-3-4-5-33-Data-Sheet-DS20005751.pdf
 */

#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/math64.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/string.h>
#include <linux/units.h>

/* EMC1812 Registers Addresses */
#define EMC1812_STATUS_ADDR				0x02
#define EMC1812_CONFIG_LO_ADDR				0x03

#define EMC1812_CFG_ADDR				0x09
#define EMC1812_CONV_ADDR				0x0A
#define EMC1812_INT_DIODE_HIGH_LIMIT_ADDR		0x0B
#define EMC1812_INT_DIODE_LOW_LIMIT_ADDR		0x0C
#define EMC1812_EXT1_HIGH_LIMIT_HIGH_BYTE_ADDR		0x0D
#define EMC1812_EXT1_LOW_LIMIT_HIGH_BYTE_ADDR		0x0E
#define EMC1812_ONE_SHOT_ADDR				0x0F

#define EMC1812_EXT1_HIGH_LIMIT_LOW_BYTE_ADDR		0x13
#define EMC1812_EXT1_LOW_LIMIT_LOW_BYTE_ADDR		0x14
#define EMC1812_EXT2_HIGH_LIMIT_HIGH_BYTE_ADDR		0x15
#define EMC1812_EXT2_LOW_LIMIT_HIGH_BYTE_ADDR		0x16
#define EMC1812_EXT2_HIGH_LIMIT_LOW_BYTE_ADDR		0x17
#define EMC1812_EXT2_LOW_LIMIT_LOW_BYTE_ADDR		0x18
#define EMC1812_EXT1_THERM_LIMIT_ADDR			0x19
#define EMC1812_EXT2_THERM_LIMIT_ADDR			0x1A
#define EMC1812_EXT_DIODE_FAULT_STATUS_ADDR		0x1B

#define EMC1812_DIODE_FAULT_MASK_ADDR			0x1F
#define EMC1812_INT_DIODE_THERM_LIMIT_ADDR		0x20
#define EMC1812_THRM_HYS_ADDR				0x21
#define EMC1812_CONSEC_ALERT_ADDR			0x22

#define EMC1812_EXT1_BETA_CONFIG_ADDR			0x25
#define EMC1812_EXT2_BETA_CONFIG_ADDR			0x26
#define EMC1812_EXT1_IDEALITY_FACTOR_ADDR		0x27
#define EMC1812_EXT2_IDEALITY_FACTOR_ADDR		0x28

#define EMC1812_EXT3_HIGH_LIMIT_HIGH_BYTE_ADDR		0x2C
#define EMC1812_EXT3_LOW_LIMIT_HIGH_BYTE_ADDR		0x2D
#define EMC1812_EXT3_HIGH_LIMIT_LOW_BYTE_ADDR		0x2E
#define EMC1812_EXT3_LOW_LIMIT_LOW_BYTE_ADDR		0x2F
#define EMC1812_EXT3_THERM_LIMIT_ADDR			0x30
#define EMC1812_EXT3_IDEALITY_FACTOR_ADDR		0x31

#define EMC1812_EXT4_HIGH_LIMIT_HIGH_BYTE_ADDR		0x34
#define EMC1812_EXT4_LOW_LIMIT_HIGH_BYTE_ADDR		0x35
#define EMC1812_EXT4_HIGH_LIMIT_LOW_BYTE_ADDR		0x36
#define EMC1812_EXT4_LOW_LIMIT_LOW_BYTE_ADDR		0x37
#define EMC1812_EXT4_THERM_LIMIT_ADDR			0x38
#define EMC1812_EXT4_IDEALITY_FACTOR_ADDR		0x39
#define EMC1812_HIGH_LIMIT_STATUS_ADDR			0x3A
#define EMC1812_LOW_LIMIT_STATUS_ADDR			0x3B
#define EMC1812_THERM_LIMIT_STATUS_ADDR			0x3C
#define EMC1812_ROC_GAIN_ADDR				0x3D
#define EMC1812_ROC_CONFIG_ADDR				0x3E
#define EMC1812_ROC_STATUS_ADDR				0x3F
#define EMC1812_R1_RESH_ADDR				0x40
#define EMC1812_R1_LIMH_ADDR				0x41
#define EMC1812_R1_LIML_ADDR				0x42
#define EMC1812_R1_SMPL_ADDR				0x43
#define EMC1812_R2_RESH_ADDR				0x44
#define EMC1812_R2_3_RESL_ADDR				0x45
#define EMC1812_R2_LIMH_ADDR				0x46
#define EMC1812_R2_LIML_ADDR				0x47
#define EMC1812_R2_SMPL_ADDR				0x48
#define EMC1812_PER_MAXTH_1_ADDR			0x49
#define EMC1812_PER_MAXT1L_ADDR				0x4A
#define EMC1812_PER_MAXTH_2_ADDR			0x4B
#define EMC1812_PER_MAXT2_3L_ADDR			0x4C
#define EMC1812_GBL_MAXT1H_ADDR				0x4D
#define EMC1812_GBL_MAXT1L_ADDR				0x4E
#define EMC1812_GBL_MAXT2H_ADDR				0x4F
#define EMC1812_GBL_MAXT2L_ADDR				0x50
#define EMC1812_FILTER_SEL_ADDR				0x51

#define EMC1812_INT_HIGH_BYTE_ADDR		0x60
#define EMC1812_INT_LOW_BYTE_ADDR		0x61
#define EMC1812_EXT1_HIGH_BYTE_ADDR		0x62
#define EMC1812_EXT1_LOW_BYTE_ADDR		0x63
#define EMC1812_EXT2_HIGH_BYTE_ADDR		0x64
#define EMC1812_EXT2_LOW_BYTE_ADDR		0x65
#define EMC1812_EXT3_HIGH_BYTE_ADDR		0x66
#define EMC1812_EXT3_LOW_BYTE_ADDR		0x67
#define EMC1812_EXT4_HIGH_BYTE_ADDR		0x68
#define EMC1812_EXT4_LOW_BYTE_ADDR		0x69
#define EMC1812_HOTTEST_DIODE_HIGH_BYTE_ADDR	0x6A
#define EMC1812_HOTTEST_DIODE_LOW_BYTE_ADDR	0x6B
#define EMC1812_HOTTEST_STATUS_ADDR		0x6C
#define EMC1812_HOTTEST_CFG_ADDR		0x6D

#define EMC1812_PRODUCT_ID_ADDR			0xFD
#define EMC1812_MANUFACTURER_ID_ADDR		0xFE
#define EMC1812_REVISION_ADDR			0xFF

/* EMC1812 Config Bits */
#define EMC1812_CFG_MSKAL			BIT(7)
#define EMC1812_CFG_RS				BIT(6)
#define EMC1812_CFG_ATTHM			BIT(5)
#define EMC1812_CFG_RECD12			BIT(4)
#define EMC1812_CFG_RECD34			BIT(3)
#define EMC1812_CFG_RANGE			BIT(2)
#define EMC1812_CFG_DA_ENA			BIT(1)
#define EMC1812_CFG_APDD			BIT(0)

/* EMC1812 Status Bits */
#define EMC1812_STATUS_ROCF			BIT(7)
#define EMC1812_STATUS_HOTCHG			BIT(6)
#define EMC1812_STATUS_BUSY			BIT(5)
#define EMC1812_STATUS_HIGH			BIT(4)
#define EMC1812_STATUS_LOW			BIT(3)
#define EMC1812_STATUS_FAULT			BIT(2)
#define EMC1812_STATUS_ETHRM			BIT(1)
#define EMC1812_STATUS_ITHRM			BIT(0)

#define EMC1812_BETA_CFG_ADDR(index)		(0x25 + (index))

#define EMC1812_CH_ADDR(index)			(0x60 + 2 * (index))

#define EMC1812_FILTER_MASK_LEN			2

#define EMC1812_PID				0x81
#define EMC1813_PID				0x87
#define EMC1814_PID				0x84
#define EMC1815_PID				0x85
#define EMC1833_PID				0x83

/* The maximum number of channels a member of the family can have */
#define EMC1812_MAX_NUM_CHANNELS		5
#define EMC1812_TEMP_OFFSET			-64

#define EMC1812_CHAN(index, si, __address) ({						\
	(struct iio_chan_spec) {                                                        \
		.type = IIO_TEMP,							\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),				\
		.info_mask_shared_by_all_available = BIT(IIO_CHAN_INFO_SAMP_FREQ) |	\
		BIT(IIO_CHAN_INFO_LOW_PASS_FILTER_3DB_FREQUENCY),			\
		.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ) |		\
		BIT(IIO_CHAN_INFO_LOW_PASS_FILTER_3DB_FREQUENCY) |			\
		BIT(IIO_CHAN_INFO_OFFSET) |						\
		BIT(IIO_CHAN_INFO_SCALE),						\
		.channel = index,							\
		.address = __address,							\
		.scan_index = si,							\
		.scan_type = {								\
			.sign = 'u',							\
			.realbits = 8,							\
			.storagebits = 8,						\
		},									\
		.indexed = 1,								\
	};										\
})

/**
 * struct emc1812_features - features of a emc1812 instance
 * @name:		chip's name
 * @phys_channels:	number of physical channels supported by the chip
 * @lock_beta1:		lock beta compensation on channel 1 when APDD is enabled
 * @lock_beta2:		lock beta compensation on channel 2 when APDD is enabled
 * @allow_apdd		whether the chip supports enabling APDD
 */
struct emc1812_features {
	const char	*name;
	u8		phys_channels;
	bool		lock_beta1;
	bool		lock_beta2;
};

static const struct emc1812_features emc1833_chip_config = {
	.name = "emc1833",
	.phys_channels = 3,
	.lock_beta1 = true,
	.lock_beta2 = false,
};

static const struct emc1812_features emc1812_chip_config = {
	.name = "emc1812",
	.phys_channels = 2,
	.lock_beta1 = false,
	.lock_beta2 = false,
};

static const struct emc1812_features emc1813_chip_config = {
	.name = "emc1813",
	.phys_channels = 3,
	.lock_beta1 = false,
	.lock_beta2 = false,
};

static const struct emc1812_features emc1814_chip_config = {
	.name = "emc1814",
	.phys_channels = 4,
	.lock_beta1 = false,
	.lock_beta2 = true,
};

static const struct emc1812_features emc1815_chip_config = {
	.name = "emc1815",
	.phys_channels = 5,
	.lock_beta1 = true,
	.lock_beta2 = true,
};

static const unsigned int emc1812_freq[][2] = {
	{  0,  62500 },
	{  0, 125000 },
	{  0, 250000 },
	{  0, 500000 },
	{  1,      0 },
	{  2,      0 },
	{  4,      0 },
	{  8,      0 },
	{ 16,      0 },
	{ 32,      0 },
	{ 64,      0 },
};

/*
 * For moving average filter with window size 4 and 8, constants were calculated
 * using: F(@-3db) = Sampling_Frequency(Hz) / (Window_Size * 2)
 * In case the moving average filter is disabled we will print the sampling frequency.
 */
static const unsigned int emc1812_3db_values_map_tbl[11][3][2] = {
	{ {  0,  62500 }, { 0,   7813 }, { 0,   3906 }, },
	{ {  0, 125000 }, { 0,  15625 }, { 0,   7813 }, },
	{ {  0, 250000 }, { 0,  31250 }, { 0,  15625 }, },
	{ {  0, 500000 }, { 0,  62500 }, { 0,  31250 }, },
	{ {  1,      0 }, { 0, 125000 }, { 0,  62500 }, },
	{ {  2,      0 }, { 0, 250000 }, { 0, 125000 }, },
	{ {  4,      0 }, { 0, 500000 }, { 0, 250000 }, },
	{ {  8,      0 }, { 1,      0 }, { 0, 500000 }, },
	{ { 16,      0 }, { 2,      0 }, { 1,      0 }, },
	{ { 32,      0 }, { 4,      0 }, { 2,      0 }, },
	{ { 64,      0 }, { 8,      0 }, { 4,      0 }, },
};

static const unsigned int emc1812_window_size[3] = { 1, 4, 8 };

/**
 * struct emc1812_priv - information about chip parameters
 * @labels:		labels of the channels
 * @chip:		pointer to structure holding chip features
 * @ideality_value:	ideality factor value for each external channel
 * @iio_chan:		specifications for each channel
 * @beta_values:	beta compensation value for external channel 1 and 2
 * @freq_idx:		index representing the current sampling frequency
 * @regmap:		device register map
 * @recd34_en:		state of Resistance Error Correction (REC) on channels 3 and 4
 * @recd12_en:		state of Resistance Error Correction (REC) on channels 1 and 2
 * @lock:		synchronize access to driver's state members
 * @apdd_en:		state of anti-parallel diode mode
 * @num_channels:	number of active physical channels
 */
struct emc1812_priv {
	const char *labels[EMC1812_MAX_NUM_CHANNELS];
	const struct emc1812_features *chip;
	unsigned int ideality_value[4];
	struct iio_chan_spec iio_chan[EMC1812_MAX_NUM_CHANNELS];
	unsigned int beta_values[2];
	unsigned int freq_idx;
	struct regmap *regmap;
	bool recd34_en;
	bool recd12_en;
	struct mutex lock;	 /* Synchronize access to driver's state members */
	bool apdd_en;
	u8 num_channels;
};

/* emc1812 regmap configuration */
static const struct regmap_range emc1812_regmap_writable_ranges[] = {
	regmap_reg_range(EMC1812_CFG_ADDR, EMC1812_ONE_SHOT_ADDR),
	regmap_reg_range(EMC1812_EXT1_HIGH_LIMIT_LOW_BYTE_ADDR,
			 EMC1812_EXT_DIODE_FAULT_STATUS_ADDR),
	regmap_reg_range(EMC1812_DIODE_FAULT_MASK_ADDR, EMC1812_CONSEC_ALERT_ADDR),
	regmap_reg_range(EMC1812_EXT1_BETA_CONFIG_ADDR, EMC1812_FILTER_SEL_ADDR),
	regmap_reg_range(EMC1812_HOTTEST_STATUS_ADDR, EMC1812_HOTTEST_CFG_ADDR),
};

static const struct regmap_access_table emc1812_regmap_wr_table = {
	.yes_ranges = emc1812_regmap_writable_ranges,
	.n_yes_ranges = ARRAY_SIZE(emc1812_regmap_writable_ranges),
};

static const struct regmap_range emc1812_regmap_rd_ranges[] = {
	regmap_reg_range(EMC1812_STATUS_ADDR, EMC1812_CONFIG_LO_ADDR),
	regmap_reg_range(EMC1812_CFG_ADDR, EMC1812_ONE_SHOT_ADDR),
	regmap_reg_range(EMC1812_EXT1_HIGH_LIMIT_LOW_BYTE_ADDR,
			 EMC1812_EXT_DIODE_FAULT_STATUS_ADDR),
	regmap_reg_range(EMC1812_DIODE_FAULT_MASK_ADDR, EMC1812_CONSEC_ALERT_ADDR),
	regmap_reg_range(EMC1812_EXT1_BETA_CONFIG_ADDR, EMC1812_FILTER_SEL_ADDR),
	regmap_reg_range(EMC1812_INT_HIGH_BYTE_ADDR, EMC1812_HOTTEST_CFG_ADDR),
	regmap_reg_range(EMC1812_PRODUCT_ID_ADDR, EMC1812_REVISION_ADDR),
};

static const struct regmap_access_table emc1812_regmap_rd_table = {
	.yes_ranges = emc1812_regmap_rd_ranges,
	.n_yes_ranges = ARRAY_SIZE(emc1812_regmap_rd_ranges),
};

static bool emc1812_is_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case EMC1812_STATUS_ADDR:
	case EMC1812_EXT_DIODE_FAULT_STATUS_ADDR:
	case EMC1812_DIODE_FAULT_MASK_ADDR:

	case EMC1812_EXT1_BETA_CONFIG_ADDR:
	case EMC1812_EXT2_BETA_CONFIG_ADDR:

	case EMC1812_HIGH_LIMIT_STATUS_ADDR:
	case EMC1812_LOW_LIMIT_STATUS_ADDR:
	case EMC1812_THERM_LIMIT_STATUS_ADDR:
	case EMC1812_ROC_STATUS_ADDR:
	case EMC1812_PER_MAXTH_1_ADDR:
	case EMC1812_PER_MAXT1L_ADDR:
	case EMC1812_PER_MAXTH_2_ADDR:
	case EMC1812_PER_MAXT2_3L_ADDR:
	case EMC1812_GBL_MAXT1H_ADDR:
	case EMC1812_GBL_MAXT1L_ADDR:
	case EMC1812_GBL_MAXT2H_ADDR:
	case EMC1812_GBL_MAXT2L_ADDR:
	case EMC1812_INT_HIGH_BYTE_ADDR:
	case EMC1812_INT_LOW_BYTE_ADDR:
	case EMC1812_EXT1_HIGH_BYTE_ADDR:
	case EMC1812_EXT1_LOW_BYTE_ADDR:
	case EMC1812_EXT2_HIGH_BYTE_ADDR:
	case EMC1812_EXT2_LOW_BYTE_ADDR:
	case EMC1812_EXT3_HIGH_BYTE_ADDR:
	case EMC1812_EXT3_LOW_BYTE_ADDR:
	case EMC1812_EXT4_HIGH_BYTE_ADDR:
	case EMC1812_EXT4_LOW_BYTE_ADDR:
	case EMC1812_HOTTEST_DIODE_HIGH_BYTE_ADDR:
	case EMC1812_HOTTEST_DIODE_LOW_BYTE_ADDR:
	case EMC1812_HOTTEST_STATUS_ADDR:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config emc1812_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.rd_table = &emc1812_regmap_rd_table,
	.wr_table = &emc1812_regmap_wr_table,
	.volatile_reg = emc1812_is_volatile_reg,
	.max_register = EMC1812_REVISION_ADDR,
};

static int emc1812_read_avail(struct iio_dev *indio_dev, struct iio_chan_spec const *chan,
			      const int **vals, int *type, int *length, long mask)
{
	struct emc1812_priv *priv = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		*vals = emc1812_freq[0];
		*length = ARRAY_SIZE(emc1812_freq) * 2;
		*type = IIO_VAL_INT_PLUS_MICRO;
		return IIO_AVAIL_LIST;
	case IIO_CHAN_INFO_LOW_PASS_FILTER_3DB_FREQUENCY:
		*vals = emc1812_3db_values_map_tbl[priv->freq_idx][0];
		*length = ARRAY_SIZE(emc1812_3db_values_map_tbl[priv->freq_idx]) * 2;
		*type = IIO_VAL_INT_PLUS_MICRO;
		return IIO_AVAIL_LIST;
	default:
		return -EINVAL;
	}
}

static int emc1812_read_raw(struct iio_dev *indio_dev, struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct emc1812_priv *priv = iio_priv(indio_dev);
	unsigned int idx, tmp_reg;
	__be16 tmp_be16;
	int ret;

	guard(mutex)(&priv->lock);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = regmap_bulk_read(priv->regmap, EMC1812_CH_ADDR(chan->channel),
				       &tmp_be16, sizeof(tmp_be16));
		if (ret)
			return ret;

		*val = be16_to_cpu(tmp_be16);
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = 0;
		*val2 = 3906250;
		return IIO_VAL_INT_PLUS_NANO;
	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = emc1812_freq[priv->freq_idx][0];
		*val2 = emc1812_freq[priv->freq_idx][1];
		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_CHAN_INFO_LOW_PASS_FILTER_3DB_FREQUENCY:
		ret = regmap_read(priv->regmap, EMC1812_FILTER_SEL_ADDR, &tmp_reg);
		if (ret)
			return ret;

		idx = hweight32(tmp_reg);
		*val = emc1812_3db_values_map_tbl[priv->freq_idx][idx][0];
		*val2 = emc1812_3db_values_map_tbl[priv->freq_idx][idx][1];
		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_CHAN_INFO_OFFSET:
		*val = EMC1812_TEMP_OFFSET;
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static int emc1812_read_label(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan, char *label)
{
	struct emc1812_priv *priv = iio_priv(indio_dev);

	if (chan->channel >= EMC1812_MAX_NUM_CHANNELS)
		return -EINVAL;

	return sysfs_emit(label, "%s\n", priv->labels[chan->channel]);
}

static int emc1812_write_raw_get_fmt(struct iio_dev *indio_dev,
				     struct iio_chan_spec const *chan, long info)
{
	switch (info) {
	case IIO_CHAN_INFO_SAMP_FREQ:
	case IIO_CHAN_INFO_LOW_PASS_FILTER_3DB_FREQUENCY:
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
}

static int emc1812_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan, int val,
			     int val2, long mask)
{
	struct emc1812_priv *priv = iio_priv(indio_dev);
	unsigned int i;
	int ret;

	guard(mutex)(&priv->lock);

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		for (i = 0; i < ARRAY_SIZE(emc1812_freq); i++)
			if (val == emc1812_freq[i][0] && val2 == emc1812_freq[i][1])
				break;

		if (i == ARRAY_SIZE(emc1812_freq))
			return -EINVAL;

		ret = regmap_write(priv->regmap, EMC1812_CONV_ADDR, i);
		if (ret)
			return ret;

		priv->freq_idx = i;
		break;
	case IIO_CHAN_INFO_LOW_PASS_FILTER_3DB_FREQUENCY:
		for (i = 0; i < ARRAY_SIZE(emc1812_3db_values_map_tbl[priv->freq_idx]); i++)
			if (val == emc1812_3db_values_map_tbl[priv->freq_idx][i][0] &&
			    val2 == emc1812_3db_values_map_tbl[priv->freq_idx][i][1])
				break;

		if (i == ARRAY_SIZE(emc1812_3db_values_map_tbl[priv->freq_idx]))
			return -EINVAL;

		/*
		 * In emc1812_3db_values_map_tbl the second index maps:
		 * 0 for filter off
		 * 1 for filter at level 1
		 * 2 for filter at level 2
		 */
		if (i == 2)
			i = 3;

		ret = regmap_write(priv->regmap, EMC1812_FILTER_SEL_ADDR, i);
		if (ret)
			return ret;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct iio_info emc1812_info = {
	.read_raw = emc1812_read_raw,
	.read_label = emc1812_read_label,
	.read_avail = emc1812_read_avail,
	.write_raw_get_fmt = emc1812_write_raw_get_fmt,
	.write_raw = emc1812_write_raw,
};

static int emc1812_init(struct emc1812_priv *priv)
{
	unsigned int i;
	int ret;
	u8 val;

	/*
	 * Depending on the chip, lock channel beta 1 and/or 2 to Diode Mode
	 * when APDD is enabled.
	 */
	if (priv->chip->lock_beta1 && priv->apdd_en)
		priv->beta_values[0] = 0x0F;
	if (priv->chip->lock_beta2 && priv->apdd_en)
		priv->beta_values[1] = 0x0F;

	/*
	 * Set default values in registers. APDD, RECD12 and RECD34 are active
	 * on 0.
	 * Set the device to be in Run (Active) state and converting on all
	 * channels.
	 * The temperature measurement range is -64°C to +191.875°C.
	 */
	val = FIELD_PREP(EMC1812_CFG_MSKAL, 1) |
	      FIELD_PREP(EMC1812_CFG_RS, 0) |
	      FIELD_PREP(EMC1812_CFG_ATTHM, 1) |
	      FIELD_PREP(EMC1812_CFG_RECD12, !priv->recd12_en) |
	      FIELD_PREP(EMC1812_CFG_RECD34, !priv->recd34_en) |
	      FIELD_PREP(EMC1812_CFG_RANGE, 1) |
	      FIELD_PREP(EMC1812_CFG_DA_ENA, 0) |
	      FIELD_PREP(EMC1812_CFG_APDD, !priv->apdd_en);

	ret = regmap_write(priv->regmap, EMC1812_CFG_ADDR, val);
	if (ret)
		return ret;

	/* Default is 4 conversions/seconds */
	ret = regmap_write(priv->regmap, EMC1812_CONV_ADDR, 6);
	if (ret)
		return ret;
	priv->freq_idx = 6;

	ret = regmap_write(priv->regmap, EMC1812_THRM_HYS_ADDR, 0x0A);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, EMC1812_CONSEC_ALERT_ADDR, 0x70);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, EMC1812_FILTER_SEL_ADDR, 0);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, EMC1812_HOTTEST_CFG_ADDR, 0);
	if (ret)
		return ret;

	/* Set beta1 and beta2 compensation parameters */
	for (i = 0; i < ARRAY_SIZE(priv->beta_values); i++) {
		ret = regmap_write(priv->regmap, EMC1812_BETA_CFG_ADDR(i),
				   priv->beta_values[i]);
		if (ret)
			return ret;
	}

	/* Set ideality factor for all external channels */
	ret = regmap_write(priv->regmap, EMC1812_EXT1_IDEALITY_FACTOR_ADDR,
			   priv->ideality_value[0]);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, EMC1812_EXT2_IDEALITY_FACTOR_ADDR,
			   priv->ideality_value[1]);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, EMC1812_EXT3_IDEALITY_FACTOR_ADDR,
			   priv->ideality_value[2]);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, EMC1812_EXT4_IDEALITY_FACTOR_ADDR,
			   priv->ideality_value[3]);
	if (ret)
		return ret;

	return 0;
}

static int emc1812_parse_fw_config(struct emc1812_priv *priv, struct device *dev,
				   int device_nr_channels)
{
	unsigned int reg_nr, iio_idx, tmp;
	int ret;

	priv->apdd_en = device_property_read_bool(dev, "microchip,enable-anti-parallel");
	priv->recd12_en = device_property_read_bool(dev, "microchip,parasitic-res-on-channel1-2");
	priv->recd34_en = device_property_read_bool(dev, "microchip,parasitic-res-on-channel3-4");

	memset32(priv->beta_values, 16, ARRAY_SIZE(priv->beta_values));
	device_property_read_u32(dev, "microchip,beta1", &priv->beta_values[0]);
	device_property_read_u32(dev, "microchip,beta2", &priv->beta_values[1]);
	if (priv->beta_values[0] > 16 || priv->beta_values[1] > 16)
		return dev_err_probe(dev, -EINVAL, "Invalid beta value\n");

	priv->num_channels = device_get_child_node_count(dev) + 1;

	if (priv->num_channels > priv->chip->phys_channels)
		return dev_err_probe(dev, -E2BIG, "More channels than the chip supports\n");

	priv->iio_chan[0] = EMC1812_CHAN(0, 0, EMC1812_CH_ADDR(0));

	priv->labels[0] = "internal_diode";
	iio_idx = 1;
	device_for_each_child_node_scoped(dev, child) {
		ret = fwnode_property_read_u32(child, "reg", &reg_nr);
		if (ret || reg_nr >= priv->chip->phys_channels)
			return dev_err_probe(dev, -EINVAL,
				     "The index of the channels does not match the chip\n");

		ret = fwnode_property_read_u32(child, "microchip,ideality-factor", &tmp);
		if (ret == 0) {
			if (tmp < 8  || tmp > 63)
				return dev_err_probe(dev, ret, "Invalid ideality value\n");
			priv->ideality_value[reg_nr - 1] = tmp;
		} else {
			priv->ideality_value[reg_nr - 1] = 18;
		}

		fwnode_property_read_string(child, "label", &priv->labels[reg_nr]);

		priv->iio_chan[iio_idx++] = EMC1812_CHAN(reg_nr, reg_nr, EMC1812_CH_ADDR(reg_nr));
	}

	return 0;
}

static int emc1812_chip_identify(struct emc1812_priv *priv, struct i2c_client *client)
{
	int ret, tmp;

	ret = regmap_read(priv->regmap, EMC1812_PRODUCT_ID_ADDR, &tmp);
	if (ret)
		return ret;

	switch (tmp) {
	case EMC1812_PID:
		priv->chip = &emc1812_chip_config;
		break;
	case EMC1813_PID:
		priv->chip = &emc1813_chip_config;
		break;
	case EMC1814_PID:
		priv->chip = &emc1814_chip_config;
		break;
	case EMC1815_PID:
		priv->chip = &emc1815_chip_config;
		break;
	case EMC1833_PID:
		priv->chip = &emc1833_chip_config;
		break;
	default:
		/*
		 * If failed to identify the hardware based on internal registers,
		 * try using fallback compatible in device tree to deal with some
		 * newer part number.
		 */
		priv->chip = i2c_get_match_data(client);
		if (!priv->chip)
			return -EINVAL;
		break;
	}
	return 0;
}

static int emc1812_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct emc1812_priv *priv;
	struct iio_dev *indio_dev;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*priv));
	if (!indio_dev)
		return -ENOMEM;

	priv = iio_priv(indio_dev);
	priv->regmap = devm_regmap_init_i2c(client, &emc1812_regmap_config);
	if (IS_ERR(priv->regmap))
		return dev_err_probe(dev, PTR_ERR(priv->regmap),
				     "Cannot initialize register map\n");

	ret = devm_mutex_init(dev, &priv->lock);
	if (ret)
		return ret;

	ret = emc1812_chip_identify(priv, client);
	if (ret)
		return dev_err_probe(dev, ret, "Chip identification fails\n");

	dev_info(dev, "Device name: %s\n", priv->chip->name);

	ret = emc1812_parse_fw_config(priv, dev, priv->chip->phys_channels);
	if (ret)
		return ret;

	ret = emc1812_init(priv);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot initialize device\n");

	indio_dev->name = priv->chip->name;
	indio_dev->info = &emc1812_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = &priv->iio_chan[0];
	indio_dev->num_channels = priv->num_channels;

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot register IIO device\n");

	return 0;
}

static const struct i2c_device_id emc1812_id[] = {
	{ .name = "emc1812", .driver_data = (kernel_ulong_t)&emc1812_chip_config },
	{ .name = "emc1813", .driver_data = (kernel_ulong_t)&emc1813_chip_config },
	{ .name = "emc1814", .driver_data = (kernel_ulong_t)&emc1814_chip_config },
	{ .name = "emc1815", .driver_data = (kernel_ulong_t)&emc1815_chip_config },
	{ .name = "emc1833", .driver_data = (kernel_ulong_t)&emc1833_chip_config },
	{ }
};
MODULE_DEVICE_TABLE(i2c, emc1812_id);

static const struct of_device_id emc1812_of_match[] = {
	{
		.compatible = "microchip,emc1812",
		.data = &emc1812_chip_config
	},
	{
		.compatible = "microchip,emc1813",
		.data = &emc1813_chip_config
	},
	{
		.compatible = "microchip,emc1814",
		.data = &emc1814_chip_config
	},
	{
		.compatible = "microchip,emc1815",
		.data = &emc1815_chip_config
	},
	{
		.compatible = "microchip,emc1833",
		.data = &emc1833_chip_config
	},
	{ }
};
MODULE_DEVICE_TABLE(of, emc1812_of_match);

static struct i2c_driver emc1812_driver = {
	.driver	 = {
		.name = "emc1812",
		.of_match_table = emc1812_of_match,
	},
	.probe = emc1812_probe,
	.id_table = emc1812_id,
};
module_i2c_driver(emc1812_driver);

MODULE_AUTHOR("Marius Cristea <marius.cristea@microchip.com>");
MODULE_DESCRIPTION("EMC1812/13/14/15/33 high-accuracy remote diode temperature monitor Driver");
MODULE_LICENSE("GPL");
