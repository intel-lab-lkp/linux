// SPDX-License-Identifier: GPL-2.0+
/*
 * AD7172-2/AD7173-8/AD7175-2/AD7176-2 SPI ADC driver
 * Copyright (C) 2023 Analog Devices, Inc.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/sysfs.h>
#include <linux/units.h>
#include <linux/gpio/driver.h>
#include <linux/regulator/consumer.h>

#include <linux/iio/iio.h>
#include <linux/iio/buffer.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/adc/ad_sigma_delta.h>

#define AD717X_REG_COMMS		0x00
#define AD717X_REG_ADC_MODE		0x01
#define AD717X_REG_INTERFACE_MODE	0x02
#define AD717X_REG_CRC			0x03
#define AD717X_REG_DATA			0x04
#define AD717X_REG_GPIO			0x06
#define AD717X_REG_ID			0x07
#define AD717X_REG_CH(x)		(0x10 + (x))
#define AD717X_REG_SETUP(x)		(0x20 + (x))
#define AD717X_REG_FILTER(x)		(0x28 + (x))
#define AD717X_REG_OFFSET(x)		(0x30 + (x))
#define AD717X_REG_GAIN(x)		(0x38 + (x))

#define AD717X_CH_ENABLE		BIT(15)
#define AD717X_CH_SETUP_SEL(x)		((x) << 12)
#define AD717X_CH_SETUP_AINPOS(x)	((x) << 5)
#define AD717X_CH_SETUP_AINNEG(x)	(x)

#define AD717X_CH_ADDRESS(pos, neg) \
	(AD717X_CH_SETUP_AINPOS(pos) | AD717X_CH_SETUP_AINNEG(neg))

#define AD7172_ID			0x00d0
#define AD7173_ID			0x30d0
#define AD7175_ID			0x0cd0
#define AD7176_ID			0x0c90
#define AD717X_ID_MASK			0xfff0

#define AD717X_ADC_MODE_REF_EN		BIT(15)
#define AD717X_ADC_MODE_SING_CYC	BIT(13)
#define AD717X_ADC_MODE_MODE_MASK	0x70
#define AD717X_ADC_MODE_MODE(x)		((x) << 4)
#define AD717X_ADC_MODE_CLOCKSEL_MASK	0xc
#define AD717X_ADC_MODE_CLOCKSEL(x)	((x) << 2)

#define AD717X_GPIO_PDSW	BIT(14)
#define AD717X_GPIO_OP_EN2_3	BIT(13)
#define AD717X_GPIO_MUX_IO	BIT(12)
#define AD717X_GPIO_SYNC_EN	BIT(11)
#define AD717X_GPIO_ERR_EN	BIT(10)
#define AD717X_GPIO_ERR_DAT	BIT(9)
#define AD717X_GPIO_GP_DATA3	BIT(7)
#define AD717X_GPIO_GP_DATA2	BIT(6)
#define AD717X_GPIO_IP_EN1	BIT(5)
#define AD717X_GPIO_IP_EN0	BIT(4)
#define AD717X_GPIO_OP_EN1	BIT(3)
#define AD717X_GPIO_OP_EN0	BIT(2)
#define AD717X_GPIO_GP_DATA1	BIT(1)
#define AD717X_GPIO_GP_DATA0	BIT(0)

#define AD717X_INTERFACE_DATA_STAT	BIT(6)
#define AD717X_INTERFACE_DATA_STAT_EN(x)\
	FIELD_PREP(AD717X_INTERFACE_DATA_STAT, x)

#define AD717X_SETUP_BIPOLAR		BIT(12)
#define AD717X_SETUP_AREF_BUF		(0x3 << 10)
#define AD717X_SETUP_AIN_BUF		(0x3 << 8)
#define AD717X_SETUP_REF_SEL_MASK	0x30
#define AD717X_SETUP_REF_SEL_AVDD1_AVSS	0x30
#define AD717X_SETUP_REF_SEL_INT_REF	0x20
#define AD717X_SETUP_REF_SEL_EXT_REF2	0x10
#define AD717X_SETUP_REF_SEL_EXT_REF	0x00

#define AD717X_FILTER_ODR0_MASK		0x1f
#define AD717X_MAX_CONFIGS		8

enum ad717x_ids {
	ID_AD7172_2,
	ID_AD7173_8,
	ID_AD7175_2,
	ID_AD7176_2,
};

struct ad717x_device_info {
	unsigned int id;
	unsigned int num_inputs;
	unsigned int num_channels;
	unsigned int num_configs;
	bool has_gp23;
	bool has_temp;
	unsigned int clock;

	const unsigned int *sinc5_data_rates;
	unsigned int num_sinc5_data_rates;
};

struct ad717x_channel_config {
	bool live;
	unsigned char cfg_slot;
	/* Fields from this point are used to compare equality of configs */
	unsigned char odr;
	bool bipolar;
	bool input_buf;
};

struct ad717x_channel {
	unsigned int chan_reg;
	struct ad717x_channel_config cfg;
	unsigned int ain;
};

struct ad717x_state {
	const struct ad717x_device_info *info;
	struct ad_sigma_delta sd;
	struct ad717x_channel *channels;
	struct regulator *reg;
	unsigned int adc_mode;
	unsigned int interface_mode;
	unsigned int num_channels;
	struct mutex cfgs_lock; /* lock for configs access */
	unsigned long cfg_slots_status; /* slots usage status bitmap*/
	unsigned long long config_usage_counter;
	unsigned long long *config_cnts;

#ifdef CONFIG_GPIOLIB
	struct gpio_chip gpiochip;
	unsigned int gpio_reg;
	unsigned int gpio_23_mask;
#endif
};

static const unsigned int ad7173_sinc5_data_rates[] = {
	6211000, 6211000, 6211000, 6211000, 6211000, 6211000, 5181000, 4444000,
	3115000, 2597000, 1007000, 503800,  381000,  200300,  100500,  59520,
	49680,	 20010,	  16333,   10000,   5000,    2500,    1250,
};

static const unsigned int ad7175_sinc5_data_rates[] = {
	50000000, 41667000, 31250000, 27778000, 20833000, 17857000, 12500000,
	10000000, 5000000,  2500000,  1000000,	500000,	  397500,   200000,
	100000,	  59920,    49960,    20000,	16666,	  10000,    5000,
};

static const struct ad717x_device_info ad717x_device_info[] = {
	[ID_AD7172_2] = {
		.id = AD7172_ID,
		.num_inputs = 5,
		.num_channels = 4,
		.num_configs = 4,
		.has_gp23 = false,
		.has_temp = true,
		.clock = 2000000,
		.sinc5_data_rates = ad7173_sinc5_data_rates,
		.num_sinc5_data_rates = ARRAY_SIZE(ad7173_sinc5_data_rates),
	},
	[ID_AD7173_8] = {
		.id = AD7173_ID,
		.num_inputs = 17,
		.num_channels = 16,
		.num_configs = 8,
		.has_gp23 = true,
		.has_temp = true,
		.clock = 2000000,
		.sinc5_data_rates = ad7173_sinc5_data_rates,
		.num_sinc5_data_rates = ARRAY_SIZE(ad7173_sinc5_data_rates),
	},
	[ID_AD7175_2] = {
		.id = AD7175_ID,
		.num_inputs = 5,
		.num_channels = 4,
		.num_configs = 4,
		.has_gp23 = false,
		.has_temp = true,
		.clock = 16000000,
		.sinc5_data_rates = ad7175_sinc5_data_rates,
		.num_sinc5_data_rates = ARRAY_SIZE(ad7175_sinc5_data_rates),
	},
	[ID_AD7176_2] = {
		.id = AD7176_ID,
		.num_inputs = 5,
		.num_channels = 4,
		.num_configs = 4,
		.has_gp23 = false,
		.has_temp = false,
		.clock = 16000000,
		.sinc5_data_rates = ad7175_sinc5_data_rates,
		.num_sinc5_data_rates = ARRAY_SIZE(ad7175_sinc5_data_rates),
	},
};

#ifdef CONFIG_GPIOLIB

static struct ad717x_state *gpiochip_to_ad717x(struct gpio_chip *chip)
{
	return container_of(chip, struct ad717x_state, gpiochip);
}

static int ad717x_gpio_get(struct gpio_chip *chip, unsigned offset)
{
	struct ad717x_state *st = gpiochip_to_ad717x(chip);
	unsigned int mask;
	unsigned int value;
	int ret;

	switch (offset) {
	case 0:
		mask = AD717X_GPIO_GP_DATA0;
		break;
	case 1:
		mask = AD717X_GPIO_GP_DATA1;
		break;
	default:
		return -EINVAL;
	}

	ret = ad_sd_read_reg(&st->sd, AD717X_REG_GPIO, 2, &value);
	if (ret)
		return ret;

	return (bool)(value & mask);
}

static int ad717x_gpio_update(struct ad717x_state *st, unsigned int set_mask,
			      unsigned int clr_mask)
{
	st->gpio_reg |= set_mask;
	st->gpio_reg &= ~clr_mask;

	return ad_sd_write_reg(&st->sd, AD717X_REG_GPIO, 2, st->gpio_reg);
}

static void ad717x_gpio_set(struct gpio_chip *chip, unsigned offset, int value)
{
	struct ad717x_state *st = gpiochip_to_ad717x(chip);
	unsigned int mask, set_mask, clr_mask;

	switch (offset) {
	case 0:
		mask = AD717X_GPIO_GP_DATA0;
		break;
	case 1:
		mask = AD717X_GPIO_GP_DATA1;
		break;
	case 2:
		mask = AD717X_GPIO_GP_DATA2;
		break;
	case 3:
		mask = AD717X_GPIO_GP_DATA3;
		break;
	default:
		return;
	}

	if (value) {
		set_mask = mask;
		clr_mask = 0;
	} else {
		set_mask = 0;
		clr_mask = mask;
	}

	ad717x_gpio_update(st, set_mask, clr_mask);
}

static int ad717x_gpio_direction_input(struct gpio_chip *chip, unsigned offset)
{
	struct ad717x_state *st = gpiochip_to_ad717x(chip);
	unsigned int mask;

	switch (offset) {
	case 0:
		mask = AD717X_GPIO_IP_EN0;
		break;
	case 1:
		mask = AD717X_GPIO_IP_EN1;
		break;
	default:
		return -EINVAL;
	}

	return ad717x_gpio_update(st, mask, 0);
}

static int ad717x_gpio_direction_output
	(struct gpio_chip *chip, unsigned offset, int value)
{
	struct ad717x_state *st = gpiochip_to_ad717x(chip);
	unsigned int set_mask, clr_mask, val_mask;

	switch (offset) {
	case 0:
		set_mask = AD717X_GPIO_OP_EN0;
		val_mask = AD717X_GPIO_GP_DATA0;
		break;
	case 1:
		set_mask = AD717X_GPIO_OP_EN1;
		val_mask = AD717X_GPIO_GP_DATA1;
		break;
	/* GP2 and GP3 can not be enabled independently */
	case 2:
		st->gpio_23_mask |= (1 << 2);
		set_mask = AD717X_GPIO_OP_EN2_3;
		val_mask = AD717X_GPIO_GP_DATA2;
		break;
	case 3:
		st->gpio_23_mask |= (1 << 3);
		set_mask = AD717X_GPIO_OP_EN2_3;
		val_mask = AD717X_GPIO_GP_DATA3;
		break;
	default:
		return -EINVAL;
	}

	if (value) {
		set_mask |= val_mask;
		clr_mask = 0;
	} else {
		clr_mask = val_mask;
	}

	return ad717x_gpio_update(st, set_mask, clr_mask);
}

static void ad717x_gpio_free(struct gpio_chip *chip, unsigned offset)
{
	struct ad717x_state *st = gpiochip_to_ad717x(chip);
	unsigned int mask;

	switch (offset) {
	case 0:
		mask = AD717X_GPIO_OP_EN0 | AD717X_GPIO_IP_EN0;
		break;
	case 1:
		mask = AD717X_GPIO_OP_EN1 | AD717X_GPIO_IP_EN1;
		break;
	case 2:
		st->gpio_23_mask &= ~(1 << offset);
		if (st->gpio_23_mask != 0)
			return;
		mask = AD717X_GPIO_OP_EN2_3;
		break;
	default:
		return;
	}

	ad717x_gpio_update(st, 0, mask);
}

static int ad717x_gpio_init(struct ad717x_state *st)
{
	st->gpiochip.label = dev_name(&st->sd.spi->dev);
	st->gpiochip.base = -1;
	if (st->info->has_gp23)
		st->gpiochip.ngpio = 4;
	else
		st->gpiochip.ngpio = 2;
	st->gpiochip.parent = &st->sd.spi->dev;
	st->gpiochip.can_sleep = true;
	st->gpiochip.direction_input = ad717x_gpio_direction_input;
	st->gpiochip.direction_output = ad717x_gpio_direction_output;
	st->gpiochip.get = ad717x_gpio_get;
	st->gpiochip.set = ad717x_gpio_set;
	st->gpiochip.free = ad717x_gpio_free;
	st->gpiochip.owner = THIS_MODULE;

	return devm_gpiochip_add_data(&st->sd.spi->dev, &st->gpiochip, NULL);
}

#else

static int ad717x_gpio_init(struct ad717x_state *st) { return 0 };
static void ad717x_gpio_cleanup(struct ad717x_state *st) {};

#endif

static struct ad717x_state *ad_sigma_delta_to_ad717x(struct ad_sigma_delta *sd)
{
	return container_of(sd, struct ad717x_state, sd);
}

static void ad717x_reset_usage_cnts(struct ad717x_state *st)
{
	int i;

	for (i = 0; i < st->info->num_configs; i++)
		(st->config_cnts)[i] = 0;

	st->config_usage_counter = 0;
}

static struct ad717x_channel_config *ad717x_find_live_config
	(struct ad717x_state *st, struct ad717x_channel_config *cfg)
{
	struct ad717x_channel_config *cfg_aux;
	ptrdiff_t cmp_size, offset;
	int i;

	offset = offsetof(struct ad717x_channel_config, cfg_slot) +
		 sizeof(cfg->cfg_slot);
	cmp_size = sizeof(*cfg) - offset;

	for (i = 0; i < st->num_channels; i++) {
		cfg_aux = &st->channels[i].cfg;

		if (cfg_aux->live && !memcmp(((void *)cfg) + offset,
					    ((void *)cfg_aux) + offset, cmp_size))
			return cfg_aux;
	}
	return NULL;
}

static int ad717x_free_config_slot_lru(struct ad717x_state *st)
{
	int i, lru_position = 0;

	for (i = 1; i < st->info->num_configs; i++)
		if (st->config_cnts[i] < st->config_cnts[lru_position])
			lru_position = i;

	for (i = 0; i < st->num_channels; i++)
		if (st->channels[i].cfg.cfg_slot == lru_position)
			st->channels[i].cfg.live = false;

	assign_bit(lru_position, &st->cfg_slots_status, 0);
	return lru_position;
}

static int ad717x_load_config(struct ad717x_state *st,
			      struct ad717x_channel_config *cfg)
{
	unsigned int config;
	int free_cfg_slot, ret;

	free_cfg_slot = find_first_zero_bit(&st->cfg_slots_status,
					    st->info->num_configs);
	if (free_cfg_slot == st->info->num_configs)
		free_cfg_slot = ad717x_free_config_slot_lru(st);

	assign_bit(free_cfg_slot, &st->cfg_slots_status, 1);
	cfg->cfg_slot = free_cfg_slot;

	config = AD717X_SETUP_REF_SEL_INT_REF;

	if (cfg->bipolar)
		config |= AD717X_SETUP_BIPOLAR;

	if (cfg->input_buf)
		config |= AD717X_SETUP_AIN_BUF;

	ret = ad_sd_write_reg(&st->sd, AD717X_REG_SETUP(free_cfg_slot), 2, config);
	if (ret)
		return ret;

	config = AD717X_FILTER_ODR0_MASK & cfg->odr;
	return ad_sd_write_reg(&st->sd, AD717X_REG_FILTER(free_cfg_slot), 2, config);
}

static int ad717x_config_channel(struct ad717x_state *st, int addr)
{
	struct ad717x_channel_config *cfg = &st->channels[addr].cfg;
	struct ad717x_channel_config *live_cfg;
	int ret = 0;

	if (!cfg->live) {
		live_cfg = ad717x_find_live_config(st, cfg);
		if (!live_cfg) {
			ret = ad717x_load_config(st, cfg);
			if (ret < 0)
				return ret;
		} else {
			cfg->cfg_slot = live_cfg->cfg_slot;
		}
	}

	cfg->live = true;
	if (st->config_usage_counter == U64_MAX)
		ad717x_reset_usage_cnts(st);

	st->config_usage_counter++;
	st->config_cnts[cfg->cfg_slot] = st->config_usage_counter;

	return 0;
}

static int ad717x_set_channel(struct ad_sigma_delta *sd, unsigned int channel)
{
	struct ad717x_state *st = ad_sigma_delta_to_ad717x(sd);
	unsigned int val;
	int ret;

	ret = ad717x_config_channel(st, channel);
	if (ret < 0)
		return ret;

	val = AD717X_CH_ENABLE |
	      AD717X_CH_SETUP_SEL(st->channels[channel].cfg.cfg_slot) |
	      st->channels[channel].ain;

	return ad_sd_write_reg(&st->sd, AD717X_REG_CH(channel), 2, val);
}

static int ad717x_set_mode(struct ad_sigma_delta *sd,
			   enum ad_sigma_delta_mode mode)
{
	struct ad717x_state *st = ad_sigma_delta_to_ad717x(sd);

	st->adc_mode &= ~AD717X_ADC_MODE_MODE_MASK;
	st->adc_mode |= AD717X_ADC_MODE_MODE(mode);

	return ad_sd_write_reg(&st->sd, AD717X_REG_ADC_MODE, 2, st->adc_mode);
}

static int ad717x_append_status(struct ad_sigma_delta *sd, bool append)
{
	struct ad717x_state *st = container_of(sd, struct ad717x_state, sd);
	unsigned int interface_mode = st->interface_mode;
	int ret;

	interface_mode |= AD717X_INTERFACE_DATA_STAT_EN(append);
	ret = ad_sd_write_reg(&st->sd, AD717X_REG_INTERFACE_MODE, 2, interface_mode);
	if (ret < 0)
		return ret;

	st->interface_mode = interface_mode;

	return 0;
}

static int ad717x_disable_all(struct ad_sigma_delta *sd)
{
	struct ad717x_state *st = container_of(sd, struct ad717x_state, sd);
	int ret;
	int i;

	for (i = 0; i < st->num_channels; i++) {
		ret = ad_sd_write_reg(sd, AD717X_REG_CH(i), 2, 0);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static struct ad_sigma_delta_info ad717x_sigma_delta_info = {
	.set_channel = ad717x_set_channel,
	.append_status = ad717x_append_status,
	.disable_all = ad717x_disable_all,
	.set_mode = ad717x_set_mode,
	.has_registers = true,
	.addr_shift = 0,
	.read_mask = BIT(6),
	.status_ch_mask = GENMASK(3, 0),
	.data_reg = AD717X_REG_DATA,
	.irq_flags = IRQF_TRIGGER_FALLING
};

static int ad717x_setup(struct iio_dev *indio_dev)
{
	struct ad717x_state *st = iio_priv(indio_dev);
	unsigned int id;
	u8 *buf;
	int ret;

	/* reset the serial interface */
	buf = kcalloc(8, sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	memset(buf, 0xff, 8);
	ret = spi_write(st->sd.spi, buf, 8);
	kfree(buf);
	if (ret < 0)
		return ret;

	/* datasheet recommends a delay of at least 500us after reset */
	usleep_range(500, 1000);

	ret = ad_sd_read_reg(&st->sd, AD717X_REG_ID, 2, &id);
	if (ret)
		return ret;

	id &= AD717X_ID_MASK;
	if (id != st->info->id)
		dev_warn(&st->sd.spi->dev, "Unexpected device id: %x, expected: %x\n",
					    id, st->info->id);

	mutex_init(&st->cfgs_lock);
	st->adc_mode |= AD717X_ADC_MODE_REF_EN | AD717X_ADC_MODE_SING_CYC;
	st->interface_mode = 0x0;

	st->config_usage_counter = 0;
	st->config_cnts = devm_kzalloc(&indio_dev->dev,
				       st->info->num_configs * sizeof(u64),
				       GFP_KERNEL);
	if (!st->config_cnts)
		return -ENOMEM;

	/* All channels are enabled by default after a reset */
	ret = ad717x_disable_all(&st->sd);
	if (ret < 0)
		return ret;

	return 0;
}

static int ad717x_read_raw(struct iio_dev *indio_dev,
	struct iio_chan_spec const *chan, int *val, int *val2, long info)
{
	struct ad717x_state *st = iio_priv(indio_dev);
	unsigned int reg;
	int ret;

	switch (info) {
	case IIO_CHAN_INFO_RAW:
		ret = ad_sigma_delta_single_conversion(indio_dev, chan, val);
		if (ret < 0)
			return ret;

		/* disable channel after single conversion */
		ret = ad_sd_write_reg(&st->sd, AD717X_REG_CH(chan->address), 2,
				      0);
		if (ret < 0)
			return ret;

		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		if (chan->type == IIO_TEMP) {
			*val = 250000000;
			*val2 = 800273203; /* (2**24 * 477) / 10 */
			return IIO_VAL_FRACTIONAL;
		} else {
			*val = 2500;
			if (chan->differential)
				*val2 = 23;
			else
				*val2 = 24;
			return IIO_VAL_FRACTIONAL_LOG2;
		}
	case IIO_CHAN_INFO_OFFSET:
		if (chan->type == IIO_TEMP) {
			*val = -874379;
		} else {
			if (chan->differential)
				*val = -(1 << (chan->scan_type.realbits - 1));
			else
				*val = 0;
		}
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SAMP_FREQ:
		reg = st->channels[chan->address].cfg.odr;

		*val = st->info->sinc5_data_rates[reg] / MILLI;
		*val2 = (st->info->sinc5_data_rates[reg] % MILLI) * MILLI;

		return IIO_VAL_INT_PLUS_MICRO;
	}
	return -EINVAL;
}

static int ad717x_write_raw(struct iio_dev *indio_dev,
	struct iio_chan_spec const *chan, int val, int val2, long info)
{
	struct ad717x_state *st = iio_priv(indio_dev);
	struct ad717x_channel_config *cfg;
	unsigned int freq, i, reg;
	int ret = 0;

	mutex_lock(&st->cfgs_lock);
	if (iio_buffer_enabled(indio_dev)) {
		mutex_unlock(&st->cfgs_lock);
		return -EBUSY;
	}

	switch (info) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		freq = val * MILLI + val2 / MILLI;

		for (i = 0; i < st->info->num_sinc5_data_rates - 1; i++) {
			if (freq >= st->info->sinc5_data_rates[i])
				break;
		}

		cfg = &st->channels[chan->address].cfg;
		cfg->odr = i;

		if (cfg->live) {
			ret = ad_sd_read_reg(&st->sd, AD717X_REG_FILTER(cfg->cfg_slot), 2, &reg);
			if (ret)
				break;
			reg &= ~AD717X_FILTER_ODR0_MASK;
			reg |= i;
			ret = ad_sd_write_reg(&st->sd, AD717X_REG_FILTER(cfg->cfg_slot), 2, reg);
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}

	mutex_unlock(&st->cfgs_lock);
	return ret;
}

static int ad717x_write_raw_get_fmt(struct iio_dev *indio_dev,
				    struct iio_chan_spec const *chan, long mask)
{
	return IIO_VAL_INT_PLUS_MICRO;
}

static int ad717x_update_scan_mode(struct iio_dev *indio_dev,
				   const unsigned long *scan_mask)
{
	struct ad717x_state *st = iio_priv(indio_dev);
	bool bit_set;
	int ret;
	int i;

	mutex_lock(&st->cfgs_lock);
	for (i = 0; i < st->num_channels; i++) {
		bit_set = test_bit(i, scan_mask);
		if (bit_set)
			ret = ad717x_set_channel(&st->sd, i);
		else
			ret = ad_sd_write_reg(&st->sd, AD717X_REG_CH(i), 2, 0);

		if (ret < 0) {
			mutex_unlock(&st->cfgs_lock);
			return ret;
		}
	}

	mutex_unlock(&st->cfgs_lock);

	return 0;
}

static int ad717x_debug(struct iio_dev *indio_dev, unsigned int reg,
			unsigned int writeval, unsigned int *readval)
{
	struct ad717x_state *st = iio_priv(indio_dev);
	u8 reg_size = 2;

	if (reg == 0)
		reg_size = 1;
	else if (reg == AD717X_REG_CRC || reg == AD717X_REG_DATA ||
		 reg >= AD717X_REG_OFFSET(0))
		reg_size = 3;

	if (readval)
		return ad_sd_read_reg(&st->sd, reg, reg_size, readval);

	return ad_sd_write_reg(&st->sd, reg, reg_size, writeval);
}

static const struct iio_info ad717x_info = {
	.read_raw = &ad717x_read_raw,
	.write_raw = &ad717x_write_raw,
	.write_raw_get_fmt = &ad717x_write_raw_get_fmt,
	.debugfs_reg_access = &ad717x_debug,
	.validate_trigger = ad_sd_validate_trigger,
	.update_scan_mode = ad717x_update_scan_mode,
};

static const struct iio_chan_spec ad717x_channel_template = {
	.type = IIO_VOLTAGE,
	.indexed = 1,
	.channel = 0,
	.address = AD717X_CH_ADDRESS(0, 0),
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
		BIT(IIO_CHAN_INFO_SCALE),
	.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ),
	.scan_index = 0,
	.scan_type = {
		.sign = 'u',
		.realbits = 24,
		.storagebits = 32,
		.shift = 0,
		.endianness = IIO_BE,
	},
};

static const struct iio_chan_spec ad717x_temp_iio_channel_template = {
	.type = IIO_TEMP,
	.indexed = 1,
	.channel = 17,
	.channel2 = 18,
	.address = 0,
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
		BIT(IIO_CHAN_INFO_SCALE) | BIT(IIO_CHAN_INFO_OFFSET),
	.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ),
	.scan_index = 0,
	.scan_type = {
		.sign = 'u',
		.realbits = 24,
		.storagebits = 32,
		.shift = 0,
		.endianness = IIO_BE,
	},
};

static int ad717x_of_parse_channel_config(struct iio_dev *indio_dev)
{
	struct ad717x_state *st = iio_priv(indio_dev);
	struct ad717x_channel *channels_st_priv;
	struct fwnode_handle *child;
	struct device *dev = indio_dev->dev.parent;
	struct iio_chan_spec *chan;
	unsigned int num_channels = 0;
	unsigned int ain[2], chan_index = 0;
	bool use_temp = false;
	int ret;

	num_channels = device_get_child_node_count(dev);

	if (device_property_read_bool(dev, "adi,temp-channel")) {
		if (!st->info->has_temp) {
			dev_err(indio_dev->dev.parent,
				"Current chip does not support temperature channel");
			return -EINVAL;
		}

		num_channels++;
		use_temp = true;
	}

	st->num_channels = num_channels;

	if (num_channels == 0)
		return 0;

	chan = devm_kcalloc(indio_dev->dev.parent, sizeof(*chan), num_channels,
			    GFP_KERNEL);
	if (!chan)
		return -ENOMEM;

	channels_st_priv = devm_kcalloc(indio_dev->dev.parent, num_channels,
					sizeof(*channels_st_priv), GFP_KERNEL);
	if (!channels_st_priv)
		return -ENOMEM;

	indio_dev->channels = chan;
	indio_dev->num_channels = num_channels;
	st->channels = channels_st_priv;

	if (use_temp) {
		chan[chan_index] = ad717x_temp_iio_channel_template;
		channels_st_priv[chan_index].ain =
			AD717X_CH_ADDRESS(chan[chan_index].channel, chan[chan_index].channel2);
		channels_st_priv[chan_index].cfg.bipolar = false;
		channels_st_priv[chan_index].cfg.input_buf = true;
		chan_index++;
	}

	device_for_each_child_node(dev, child) {
		ret = fwnode_property_read_u32_array(child, "diff-channels", ain, 2);
		if (ret) {
			fwnode_handle_put(child);
			return ret;
		}

		if (ain[0] >= st->info->num_inputs ||
		    ain[1] >= st->info->num_inputs) {
			dev_err(indio_dev->dev.parent,
				"Input pin number out of range for pair (%d %d).", ain[0], ain[1]);
			fwnode_handle_put(child);
			return -EINVAL;
		}

		chan[chan_index] = ad717x_channel_template;
		chan[chan_index].address = chan_index;
		chan[chan_index].scan_index = chan_index;
		chan[chan_index].channel = ain[0];
		chan[chan_index].channel2 = ain[1];

		channels_st_priv[chan_index].ain =
			AD717X_CH_ADDRESS(ain[0], ain[1]);
		channels_st_priv[chan_index].chan_reg = chan_index;
		channels_st_priv[chan_index].cfg.input_buf = true;
		channels_st_priv[chan_index].cfg.odr = 0;

		chan[chan_index].differential = fwnode_property_read_bool(child, "adi,bipolar");
		if (chan[chan_index].differential) {
			chan[chan_index].info_mask_separate |= BIT(IIO_CHAN_INFO_OFFSET);
			channels_st_priv[chan_index].cfg.bipolar = true;
		}

		chan_index++;
	}

	return 0;
}

static int ad717x_probe(struct spi_device *spi)
{
	struct ad717x_state *st;
	struct iio_dev *indio_dev;
	int ret;

	if (!spi->irq) {
		dev_err(&spi->dev, "No IRQ specified\n");
		return -ENODEV;
	}

	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->info = device_get_match_data(&spi->dev);
	if (!st->info)
		return -ENODEV;

	indio_dev->dev.parent = &spi->dev;
	indio_dev->name = spi_get_device_id(spi)->name;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &ad717x_info;

	ad717x_sigma_delta_info.num_slots = st->info->num_configs;
	ret = ad_sd_init(&st->sd, indio_dev, spi, &ad717x_sigma_delta_info);
	if (ret < 0)
		return ret;

	spi_set_drvdata(spi, indio_dev);

	ret = ad717x_of_parse_channel_config(indio_dev);
	if (ret)
		return ret;

	ret = devm_ad_sd_setup_buffer_and_trigger(&spi->dev, indio_dev);
	if (ret < 0)
		return ret;

	ret = ad717x_setup(indio_dev);
	if (ret)
		return ret;

	ret = devm_iio_device_register(&spi->dev, indio_dev);
	if (ret)
		return ret;

	return ad717x_gpio_init(st);
}

static const struct of_device_id ad717x_of_match[] = {
	{ .compatible = "adi,ad7172-2",
	  .data = &ad717x_device_info[ID_AD7172_2] },
	{ .compatible = "adi,ad7173-8",
	  .data = &ad717x_device_info[ID_AD7173_8] },
	{ .compatible = "adi,ad7175-2",
	  .data = &ad717x_device_info[ID_AD7175_2] },
	{ .compatible = "adi,ad7176-2",
	  .data = &ad717x_device_info[ID_AD7176_2] },
	{}
}
MODULE_DEVICE_TABLE(of, ad717x_of_match);

static const struct spi_device_id ad717x_id_table[] = {
	{ "ad7172-2", },
	{ "ad7173-8", },
	{ "ad7175-2", },
	{ "ad7176-2", },
	{}
};
MODULE_DEVICE_TABLE(spi, ad717x_id_table);

static struct spi_driver ad717x_driver = {
	.driver = {
		.name	= "ad717x",
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(ad717x_of_match),
	},
	.probe		= ad717x_probe,
	.id_table	= ad717x_id_table,
};
module_spi_driver(ad717x_driver);

MODULE_AUTHOR("Lars-Peter Clausen <lars@metafo.de>");
MODULE_AUTHOR("Dumitru Ceclan <dumitru.ceclan@analog.com>");
MODULE_DESCRIPTION("Analog Devices AD7172/AD7173/AD7175/AD7176 ADC driver");
MODULE_LICENSE("GPL v2");
