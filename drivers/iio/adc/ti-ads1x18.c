// SPDX-License-Identifier: GPL-2.0-only
/*
 * Texas Instruments ADS1X18 ADC driver
 *
 * Copyright (C) 2025 Kurt Borja <kuurtb@gmail.com>
 */

#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/bitmap.h>
#include <linux/byteorder/generic.h>
#include <linux/cleanup.h>
#include <linux/completion.h>
#include <linux/dev_printk.h>
#include <linux/device.h>
#include <linux/device/devres.h>
#include <linux/errno.h>
#include <linux/find.h>
#include <linux/gfp_types.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>
#include <linux/spi/spi.h>
#include <linux/units.h>

#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/trigger.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/trigger_consumer.h>

#define ADS1X18_CFG_DEFAULT		0x058b

#define ADS1X18_CFG_OS_TRIG		BIT_U16(15)
#define ADS1X18_CFG_TS_MODE_EN		BIT_U16(4)
#define ADS1X18_CFG_PULL_UP		BIT_U16(3)
#define ADS1X18_CFG_NOP			BIT_U16(1)
#define ADS1X18_CFG_VALID		(ADS1X18_CFG_PULL_UP | ADS1X18_CFG_NOP)

#define ADS1X18_CFG_MUX_MASK		GENMASK_U16(14, 12)
#define ADS1X18_AIN0_AIN1_ADDR		0
#define ADS1X18_AIN0_AIN3_ADDR		1
#define ADS1X18_AIN1_AIN3_ADDR		2
#define ADS1X18_AIN2_AIN3_ADDR		3
#define ADS1X18_AIN0_GND_ADDR		4
#define ADS1X18_AIN1_GND_ADDR		5
#define ADS1X18_AIN2_GND_ADDR		6
#define ADS1X18_AIN3_GND_ADDR		7
#define ADS1X18_TEMP_ADDR		8
#define ADS1X18_TIMESTAMP_ADDR		9

#define ADS1X18_CFG_PGA_MASK		GENMASK_U16(11, 9)
#define ADS1X18_PGA_MODE_0		0
#define ADS1X18_PGA_MODE_1		1
#define ADS1X18_PGA_MODE_2		2
#define ADS1X18_PGA_MODE_3		3
#define ADS1X18_PGA_MODE_4		4
#define ADS1X18_PGA_MODE_5		5
#define ADS1X18_PGA_DEFAULT		ADS1X18_PGA_MODE_2

#define ADS1X18_CFG_MODE_MASK		GENMASK_U16(8, 8)
#define ADS1X18_MODE_CONTINUOUS		0
#define ADS1X18_MODE_ONESHOT		1

#define ADS1X18_CFG_DRATE_MASK		GENMASK_U16(7, 5)
#define ADS1X18_DRATE_MODE_0		0
#define ADS1X18_DRATE_MODE_1		1
#define ADS1X18_DRATE_MODE_2		2
#define ADS1X18_DRATE_MODE_3		3
#define ADS1X18_DRATE_MODE_4		4
#define ADS1X18_DRATE_MODE_5		5
#define ADS1X18_DRATE_MODE_6		6
#define ADS1X18_DRATE_MODE_7		7
#define ADS1X18_DRATE_DEFAULT		ADS1X18_DRATE_MODE_4

#define ADS1X18_MAX_ADC_ADDR		7
#define ADS1X18_MAX_CHANNELS		9

struct ads1x18_chan_data {
	unsigned int pga_mode:3;
	unsigned int drate_mode:3;
};

struct ads1x18_chip_info {
	const char *name;

	const struct iio_chan_spec *channels;
	unsigned long channels_sz;

	const int *sps_table;
	unsigned long sps_table_sz;
	const int (*fsr_table)[2];
	unsigned long fsr_table_sz;
	const int temp_scale[2];
};

struct ads1x18 {
	struct spi_device *spi;
	struct iio_dev *indio_dev;
	struct iio_trigger *indio_trig;

	struct gpio_desc *drdy_gpiod;
	int drdy_irq;

	u16 tx_buf[2] __aligned(IIO_DMA_MINALIGN);
	u16 rx_buf[2];
	struct spi_transfer xfer;
	struct spi_message message;
	struct completion data_ready;
	struct mutex msg_lock; /* Protects message transfers */

	unsigned int restore_mode:1;

	unsigned long bufidx_to_addr[ADS1X18_MAX_CHANNELS];
	struct ads1x18_chan_data channels[ADS1X18_MAX_CHANNELS];

	const struct ads1x18_chip_info *chip_info;
};

#define ADS1X18_VOLT_CHANNEL(_addr, _chan, _chan2, _diff, _realbits) {		\
	.type = IIO_VOLTAGE,							\
	.channel = _chan,							\
	.channel2 = _chan2,							\
	.address = _addr,							\
	.scan_type = {								\
		.sign = 's',							\
		.realbits = _realbits,						\
		.storagebits = 16,						\
		.shift = 16 - _realbits,					\
	},									\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |				\
			      BIT(IIO_CHAN_INFO_SCALE) |			\
			      BIT(IIO_CHAN_INFO_SAMP_FREQ),			\
	.info_mask_shared_by_type_available = BIT(IIO_CHAN_INFO_SCALE),		\
	.info_mask_shared_by_all_available = BIT(IIO_CHAN_INFO_SAMP_FREQ),	\
	.modified = _diff,							\
	.indexed = true,							\
	.differential = _diff,							\
}

#define ADS1X18_TEMP_CHANNEL(_realbits)	{					\
	.type = IIO_TEMP,							\
	.channel = 0,								\
	.address = ADS1X18_TEMP_ADDR,						\
	.scan_type = {								\
		.sign = 's',							\
		.realbits = _realbits,						\
		.storagebits = 16,						\
		.shift = 16 - _realbits,					\
	},									\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |				\
			      BIT(IIO_CHAN_INFO_SCALE) |			\
			      BIT(IIO_CHAN_INFO_SAMP_FREQ),			\
	.info_mask_shared_by_all_available = BIT(IIO_CHAN_INFO_SAMP_FREQ),	\
}

static const struct iio_chan_spec ads1118_iio_channels[] = {
	ADS1X18_VOLT_CHANNEL(ADS1X18_AIN0_AIN1_ADDR, 0, 1, true, 16),
	ADS1X18_VOLT_CHANNEL(ADS1X18_AIN0_AIN3_ADDR, 0, 3, true, 16),
	ADS1X18_VOLT_CHANNEL(ADS1X18_AIN1_AIN3_ADDR, 1, 3, true, 16),
	ADS1X18_VOLT_CHANNEL(ADS1X18_AIN2_AIN3_ADDR, 2, 3, true, 16),
	ADS1X18_VOLT_CHANNEL(ADS1X18_AIN0_GND_ADDR, 0, 0, false, 16),
	ADS1X18_VOLT_CHANNEL(ADS1X18_AIN1_GND_ADDR, 1, 0, false, 16),
	ADS1X18_VOLT_CHANNEL(ADS1X18_AIN2_GND_ADDR, 2, 0, false, 16),
	ADS1X18_VOLT_CHANNEL(ADS1X18_AIN3_GND_ADDR, 3, 0, false, 16),
	ADS1X18_TEMP_CHANNEL(14),
	IIO_CHAN_SOFT_TIMESTAMP(ADS1X18_TIMESTAMP_ADDR),
};

static const struct iio_chan_spec ads1018_iio_channels[] = {
	ADS1X18_VOLT_CHANNEL(ADS1X18_AIN0_AIN1_ADDR, 0, 1, true, 12),
	ADS1X18_VOLT_CHANNEL(ADS1X18_AIN0_AIN3_ADDR, 0, 3, true, 12),
	ADS1X18_VOLT_CHANNEL(ADS1X18_AIN1_AIN3_ADDR, 1, 3, true, 12),
	ADS1X18_VOLT_CHANNEL(ADS1X18_AIN2_AIN3_ADDR, 2, 3, true, 12),
	ADS1X18_VOLT_CHANNEL(ADS1X18_AIN0_GND_ADDR, 0, 0, false, 12),
	ADS1X18_VOLT_CHANNEL(ADS1X18_AIN1_GND_ADDR, 1, 0, false, 12),
	ADS1X18_VOLT_CHANNEL(ADS1X18_AIN2_GND_ADDR, 2, 0, false, 12),
	ADS1X18_VOLT_CHANNEL(ADS1X18_AIN3_GND_ADDR, 3, 0, false, 12),
	ADS1X18_TEMP_CHANNEL(12),
	IIO_CHAN_SOFT_TIMESTAMP(ADS1X18_TIMESTAMP_ADDR),
};

static unsigned int ads1x18_get_drate_mode(struct ads1x18 *ads1x18,
					   unsigned int address)
{
	return ads1x18->channels[address].drate_mode;
}

static unsigned int ads1x18_get_pga_mode(struct ads1x18 *ads1x18,
					 unsigned int address)
{
	return ads1x18->channels[address].pga_mode;
}

static void ads1x18_set_drate_mode(struct ads1x18 *ads1x18, unsigned int address,
				   unsigned int val)
{
	ads1x18->channels[address].drate_mode = val;
}

static void ads1x18_set_pga_mode(struct ads1x18 *ads1x18, unsigned int address,
				 unsigned int val)
{
	ads1x18->channels[address].pga_mode = val;
}

static unsigned long ads1x18_calc_timeout(struct ads1x18 *ads1x18,
					  unsigned int drate_mode)
{
	const struct ads1x18_chip_info *chip_info = ads1x18->chip_info;
	unsigned long timeout;
	unsigned int sps;

	sps = chip_info->sps_table[drate_mode];
	timeout = DIV_ROUND_UP(MICROHZ_PER_HZ, sps);

	return usecs_to_jiffies(timeout * 2);
}

static int __ads1x18_read_conver(struct ads1x18 *ads1x18, u16 *cnv)
{
	int ret;

	ads1x18->tx_buf[0] = 0;
	ads1x18->tx_buf[1] = 0;
	ret = spi_sync_locked(ads1x18->spi, &ads1x18->message);
	if (ret)
		return ret;

	*cnv = be16_to_cpu(ads1x18->rx_buf[0]);

	return 0;
}

static int __ads1x18_write_config(struct ads1x18 *ads1x18, u16 cfg)
{
	ads1x18->tx_buf[0] = cpu_to_be16(cfg);
	ads1x18->tx_buf[1] = 0;

	return spi_sync_locked(ads1x18->spi, &ads1x18->message);
}

static int ads1x18_read_conver(struct ads1x18 *ads1x18, u16 *cnv)
{
	int ret;

	spi_bus_lock(ads1x18->spi->controller);
	ret = __ads1x18_read_conver(ads1x18, cnv);
	spi_bus_unlock(ads1x18->spi->controller);

	return ret;
}

static int ads1x18_write_config(struct ads1x18 *ads1x18, u16 cfg)
{
	int ret;

	spi_bus_lock(ads1x18->spi->controller);
	ret = __ads1x18_write_config(ads1x18, cfg);
	spi_bus_unlock(ads1x18->spi->controller);

	return ret;
}

static int ads1x18_oneshot(struct ads1x18 *ads1x18,
			   struct iio_chan_spec const *chan, int *val)
{
	unsigned int drate = ads1x18_get_drate_mode(ads1x18, chan->address);
	unsigned int pga = ads1x18_get_pga_mode(ads1x18, chan->address);
	unsigned long timeout = ads1x18_calc_timeout(ads1x18, drate);
	u16 cnv, cfg = 0;
	int ret;

	reinit_completion(&ads1x18->data_ready);

	cfg |= ADS1X18_CFG_VALID;
	cfg |= ADS1X18_CFG_OS_TRIG;
	cfg |= FIELD_PREP(ADS1X18_CFG_MUX_MASK, chan->address);
	cfg |= FIELD_PREP(ADS1X18_CFG_PGA_MASK, pga);
	cfg |= FIELD_PREP(ADS1X18_CFG_MODE_MASK, ADS1X18_MODE_ONESHOT);
	cfg |= FIELD_PREP(ADS1X18_CFG_DRATE_MASK, drate);
	if (chan->type == IIO_TEMP)
		cfg |= ADS1X18_CFG_TS_MODE_EN;

	ret = __ads1x18_write_config(ads1x18, cfg);
	if (ret)
		return ret;
	enable_irq(ads1x18->drdy_irq);

	if (!wait_for_completion_timeout(&ads1x18->data_ready, timeout)) {
		disable_irq(ads1x18->drdy_irq);
		return -ETIMEDOUT;
	}
	disable_irq(ads1x18->drdy_irq);

	ret = __ads1x18_read_conver(ads1x18, &cnv);
	if (ret)
		return ret;

	cnv >>= chan->scan_type.shift;
	*val = sign_extend32(cnv, chan->scan_type.realbits - 1);

	return ret;
}

static int
ads1x18_read_raw(struct iio_dev *indio_dev, struct iio_chan_spec const *chan,
		 int *val, int *val2, long mask)
{
	struct ads1x18 *ads1x18 = iio_priv(indio_dev);
	struct ads1x18_chan_data *chan_info = &ads1x18->channels[chan->address];
	const struct ads1x18_chip_info *chip_info = ads1x18->chip_info;
	int ret;

	guard(mutex)(&ads1x18->msg_lock);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (!iio_device_claim_direct(indio_dev))
			return -EBUSY;
		/* CS needs to remain asserted until dataready IRQ */
		spi_bus_lock(ads1x18->spi->controller);

		ret = ads1x18_oneshot(ads1x18, chan, val);

		spi_bus_unlock(ads1x18->spi->controller);
		iio_device_release_direct(indio_dev);

		if (ret)
			return ret;

		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_VOLTAGE:
			*val = chip_info->fsr_table[chan_info->pga_mode][0];
			*val2 = chip_info->fsr_table[chan_info->pga_mode][1];
			return IIO_VAL_INT_PLUS_NANO;

		case IIO_TEMP:
			*val = chip_info->temp_scale[0];
			*val2 = chip_info->temp_scale[1];
			return IIO_VAL_INT_PLUS_MICRO;

		default:
			return -EOPNOTSUPP;
		}

	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = chip_info->sps_table[chan_info->drate_mode];
		return IIO_VAL_INT;

	default:
		return -EOPNOTSUPP;
	}
}

static int
ads1x18_read_avail(struct iio_dev *indio_dev, struct iio_chan_spec const *chan,
		   const int **vals, int *type, int *length, long mask)
{
	struct ads1x18 *ads1x18 = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		*type = IIO_VAL_INT_PLUS_NANO;
		*vals = (const int *)ads1x18->chip_info->fsr_table;
		*length = ads1x18->chip_info->fsr_table_sz * 2;
		return IIO_AVAIL_LIST;

	case IIO_CHAN_INFO_SAMP_FREQ:
		*type = IIO_VAL_INT;
		*vals = ads1x18->chip_info->sps_table;
		*length = ads1x18->chip_info->sps_table_sz;
		return IIO_AVAIL_LIST;

	default:
		return -EOPNOTSUPP;
	}
}

static int
ads1x18_write_raw(struct iio_dev *indio_dev, struct iio_chan_spec const *chan,
		  int val, int val2, long mask)
{
	struct ads1x18 *ads1x18 = iio_priv(indio_dev);
	const struct ads1x18_chip_info *info = ads1x18->chip_info;
	unsigned int i = 0;

	guard(mutex)(&ads1x18->msg_lock);

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		for (; i < info->fsr_table_sz; i++) {
			if (val != info->fsr_table[i][0] ||
			    val2 != info->fsr_table[i][1])
				continue;

			ads1x18_set_pga_mode(ads1x18, chan->address, i);
			return 0;
		}

		return -EINVAL;

	case IIO_CHAN_INFO_SAMP_FREQ:
		for (; i < info->sps_table_sz; i++) {
			if (val != info->sps_table[i])
				continue;

			ads1x18_set_drate_mode(ads1x18, chan->address, i);
			return 0;
		}

		return -EINVAL;

	default:
		return -EOPNOTSUPP;
	}
}

static int
ads1x18_write_raw_get_fmt(struct iio_dev *indio_dev,
			  struct iio_chan_spec const *chan, long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		return IIO_VAL_INT_PLUS_NANO;
	default:
		return IIO_VAL_INT_PLUS_MICRO;
	}
}

static const struct iio_info ads1x18_iio_info = {
	.read_raw = ads1x18_read_raw,
	.read_avail = ads1x18_read_avail,
	.write_raw = ads1x18_write_raw,
	.write_raw_get_fmt = ads1x18_write_raw_get_fmt,
};

static int ads1x18_set_trigger_state(struct iio_trigger *trig, bool state)
{
	struct ads1x18 *ads1x18 = iio_trigger_get_drvdata(trig);
	u16 cnv;

	guard(mutex)(&ads1x18->msg_lock);

	/*
	 * We need to lock the SPI bus when enabling the trigger to prevent
	 * another device from taking the CS and DOUT/DRDY lines.
	 */

	if (state) {
		spi_bus_lock(ads1x18->spi->controller);
		enable_irq(ads1x18->drdy_irq);

		/*
		 * Read once to ensure we are holding the CS line after locking
		 */
		return __ads1x18_read_conver(ads1x18, &cnv);
	}

	disable_irq(ads1x18->drdy_irq);
	spi_bus_unlock(ads1x18->spi->controller);

	return 0;
}

static const struct iio_trigger_ops ads1x18_trigger_ops = {
	.set_trigger_state = ads1x18_set_trigger_state,
	.validate_device = iio_trigger_validate_own_device,
};

static int ads1x18_buffer_preenable(struct iio_dev *indio_dev)
{
	struct ads1x18 *ads1x18 = iio_priv(indio_dev);
	unsigned int pga, drate, addr, idx;
	u16 cfg = 0;

	guard(mutex)(&ads1x18->msg_lock);

	idx = find_first_bit(indio_dev->active_scan_mask,
			     iio_get_masklength(indio_dev));
	addr = ads1x18->bufidx_to_addr[idx];
	pga = ads1x18_get_pga_mode(ads1x18, addr);
	drate = ads1x18_get_drate_mode(ads1x18, addr);

	cfg |= ADS1X18_CFG_VALID;
	cfg |= FIELD_PREP(ADS1X18_CFG_MUX_MASK, addr);
	cfg |= FIELD_PREP(ADS1X18_CFG_PGA_MASK, pga);
	cfg |= FIELD_PREP(ADS1X18_CFG_MODE_MASK, ADS1X18_MODE_CONTINUOUS);
	cfg |= FIELD_PREP(ADS1X18_CFG_DRATE_MASK, drate);
	if (addr == ADS1X18_TEMP_ADDR)
		cfg |= ADS1X18_CFG_TS_MODE_EN;

	return ads1x18_write_config(ads1x18, cfg);
}

static int ads1x18_buffer_postdisable(struct iio_dev *indio_dev)
{
	struct ads1x18 *ads1x18 = iio_priv(indio_dev);

	guard(mutex)(&ads1x18->msg_lock);

	return ads1x18_write_config(ads1x18, ADS1X18_CFG_DEFAULT);
}

static bool ads1x18_validate_scan_mask(struct iio_dev *indio_dev,
				       const unsigned long *mask)
{
	return bitmap_weight(mask, iio_get_masklength(indio_dev)) == 1;
}

static const struct iio_buffer_setup_ops ads1x18_buffer_ops = {
	.preenable = ads1x18_buffer_preenable,
	.postdisable = ads1x18_buffer_postdisable,
	.validate_scan_mask = ads1x18_validate_scan_mask,
};

static irqreturn_t ads1x18_irq_handler(int irq, void *dev_id)
{
	struct ads1x18 *ads1x18 = dev_id;

	/*
	 * We need to check if the "drdy" pin is actually active or if it's a
	 * latched pending interrupt.
	 */
	if (!gpiod_get_value(ads1x18->drdy_gpiod))
		return IRQ_HANDLED;

	complete(&ads1x18->data_ready);
	iio_trigger_poll(ads1x18->indio_trig);

	return IRQ_HANDLED;
}

static int ads1x18_interrupt_init(struct ads1x18 *ads1x18)
{
	const struct ads1x18_chip_info *info = ads1x18->chip_info;
	struct spi_device *spi = ads1x18->spi;

	ads1x18->drdy_gpiod = devm_gpiod_get(&spi->dev, "drdy", GPIOD_IN);
	if (IS_ERR(ads1x18->drdy_gpiod))
		return dev_err_probe(&spi->dev, PTR_ERR(ads1x18->drdy_gpiod),
				     "Failed to get 'drdy' GPIO.\n");

	ads1x18->drdy_irq = gpiod_to_irq(ads1x18->drdy_gpiod);
	if (ads1x18->drdy_irq < 0)
		return dev_err_probe(&spi->dev, ads1x18->drdy_irq,
				     "Failed to get 'drdy IRQ.\n'");

	/*
	 * The "data-ready" IRQ line is shared with the MOSI pin, thus we need
	 * to keep it disabled until we actually request data.
	 */
	return devm_request_irq(&spi->dev, ads1x18->drdy_irq,
				ads1x18_irq_handler, IRQF_NO_AUTOEN,
				info->name, ads1x18);
}

static irqreturn_t ads1x18_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct ads1x18 *ads1x18 = iio_priv(indio_dev);
	struct {
		u16 conv;
		aligned_s64 ts;
	} scan;
	int ret;

	guard(mutex)(&ads1x18->msg_lock);

	if (iio_trigger_using_own(indio_dev)) {
		disable_irq(ads1x18->drdy_irq);
		ret = __ads1x18_read_conver(ads1x18, &scan.conv);
		enable_irq(ads1x18->drdy_irq);
	} else {
		ret = ads1x18_read_conver(ads1x18, &scan.conv);
	}

	if (ret)
		return IRQ_HANDLED;

	ret = iio_push_to_buffers_with_ts(indio_dev, &scan, sizeof(scan),
					  pf->timestamp);
	if (!ret)
		iio_trigger_notify_done(ads1x18->indio_trig);

	return IRQ_HANDLED;
}

static int ads1x18_triggered_buffer_init(struct ads1x18 *ads1x18)
{
	struct iio_dev *indio_dev = ads1x18->indio_dev;
	struct spi_device *spi = ads1x18->spi;
	int ret;

	ads1x18->indio_trig = devm_iio_trigger_alloc(&spi->dev, "%s-dev%d-drdy",
						     indio_dev->name,
						     iio_device_id(indio_dev));
	if (!ads1x18->indio_trig)
		return -ENOMEM;

	iio_trigger_set_drvdata(ads1x18->indio_trig, ads1x18);
	ads1x18->indio_trig->ops = &ads1x18_trigger_ops;

	ret = devm_iio_trigger_register(&spi->dev, ads1x18->indio_trig);
	if (ret)
		return ret;

	return devm_iio_triggered_buffer_setup(&spi->dev, indio_dev,
					       iio_pollfunc_store_time,
					       ads1x18_trigger_handler,
					       &ads1x18_buffer_ops);
}

static int ads1x18_message_init(struct ads1x18 *ads1x18)
{
	struct spi_device *spi = ads1x18->spi;

	/*
	 * We need to keep CS asserted to catch "data-ready" interrupts.
	 * Otherwise the DOUT/DRDY line enters a Hi-Z state and it can't be
	 * driven by the ADC.
	 */
	ads1x18->xfer.cs_change = 1;
	ads1x18->xfer.tx_buf = ads1x18->tx_buf;
	ads1x18->xfer.rx_buf = ads1x18->rx_buf;
	ads1x18->xfer.len = sizeof(ads1x18->tx_buf);
	spi_message_init_no_memset(&ads1x18->message);
	spi_message_add_tail(&ads1x18->xfer, &ads1x18->message);

	return devm_spi_optimize_message(&spi->dev, spi, &ads1x18->message);
}

static int ads1x18_fill_properties(struct ads1x18 *ads1x18,
				   struct fwnode_handle *handle,
				   struct iio_chan_spec *chan)
{
	const struct ads1x18_chip_info *info = ads1x18->chip_info;
	struct ads1x18_chan_data *chan_data;
	u32 val, reg;
	int ret;

	ret = fwnode_property_read_u32(handle, "reg", &reg);
	if (ret)
		return ret;
	if (reg > ADS1X18_MAX_ADC_ADDR)
		return dev_err_probe(&ads1x18->spi->dev, -ENXIO,
				     "%s: Invalid channel address %u.\n",
				     fwnode_get_name(handle), reg);

	*chan = info->channels[reg];

	chan_data = &ads1x18->channels[reg];
	chan_data->pga_mode = ADS1X18_PGA_DEFAULT;
	chan_data->drate_mode = ADS1X18_DRATE_DEFAULT;

	if (fwnode_property_present(handle, "ti,gain")) {
		ret = fwnode_property_read_u32(handle, "ti,gain", &val);
		if (ret)
			return ret;
		if (val >= info->fsr_table_sz)
			return dev_err_probe(&ads1x18->spi->dev, -ENXIO,
					     "%s: ti,gain not in range.",
					     fwnode_get_name(handle));

		chan_data->pga_mode = val;
	}

	if (fwnode_property_present(handle, "ti,datarate")) {
		ret = fwnode_property_read_u32(handle, "ti,datarate", &val);
		if (ret)
			return ret;
		if (val >= info->sps_table_sz)
			return dev_err_probe(&ads1x18->spi->dev, -ENXIO,
					     "%s: ti,datarate not in range.",
					     fwnode_get_name(handle));

		chan_data->drate_mode = val;
	}

	return 0;
}

static int ads1x18_channels_init(struct ads1x18 *ads1x18,
				 const struct ads1x18_chip_info *info,
				 struct iio_chan_spec **cs)
{
	struct device *dev = &ads1x18->spi->dev;
	struct iio_chan_spec *channels;
	int ret, nchans, index = 0;

	nchans = device_get_named_child_node_count(dev, "channel");
	if (!nchans)
		return dev_err_probe(dev, -ENODEV,
				     "No ADC channels described.\n");

	channels = devm_kcalloc(dev, nchans + 2, sizeof(*channels), GFP_KERNEL);
	if (!channels)
		return -ENOMEM;

	device_for_each_named_child_node_scoped(dev, child, "channel") {
		ret = ads1x18_fill_properties(ads1x18, child, &channels[index]);
		if (ret)
			return ret;

		channels[index].scan_index = index;
		ads1x18->bufidx_to_addr[index] = channels[index].address;
		index++;
	}

	ads1x18->channels[ADS1X18_TEMP_ADDR].drate_mode = ADS1X18_DRATE_DEFAULT;
	channels[index] = info->channels[ADS1X18_TEMP_ADDR];
	channels[index].scan_index = index;
	ads1x18->bufidx_to_addr[index] = channels[index].address;
	index++;

	channels[index] = info->channels[ADS1X18_TIMESTAMP_ADDR];
	ads1x18->bufidx_to_addr[index] = channels[index].address;

	*cs = channels;

	return index;
}

static int ads1x18_spi_probe(struct spi_device *spi)
{
	const struct ads1x18_chip_info *info = spi_get_device_match_data(spi);
	struct iio_chan_spec *channels;
	struct iio_dev *indio_dev;
	struct ads1x18 *ads1x18;
	int num_channels, ret;

	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*ads1x18));
	if (!indio_dev)
		return -ENOMEM;

	ads1x18 = iio_priv(indio_dev);
	ads1x18->spi = spi;
	ads1x18->indio_dev = indio_dev;
	ads1x18->chip_info = info;
	mutex_init(&ads1x18->msg_lock);
	init_completion(&ads1x18->data_ready);
	spi_set_drvdata(spi, ads1x18);

	num_channels = ads1x18_channels_init(ads1x18, info, &channels);
	if (num_channels < 0)
		return num_channels;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->name = info->name;
	indio_dev->info = &ads1x18_iio_info;
	indio_dev->channels = channels;
	indio_dev->num_channels = num_channels;

	ret = ads1x18_message_init(ads1x18);
	if (ret)
		return ret;

	ret = ads1x18_triggered_buffer_init(ads1x18);
	if (ret)
		return ret;

	ret = ads1x18_interrupt_init(ads1x18);
	if (ret)
		return ret;

	return devm_iio_device_register(&spi->dev, indio_dev);
}

static int ads1x18_suspend(struct device *dev)
{
	struct ads1x18 *ads1x18 = dev_get_drvdata(dev);
	u16 cfg;
	int ret;

	guard(mutex)(&ads1x18->msg_lock);

	/* Current config is readback into rx_buf[1] */
	cfg = be16_to_cpu(ads1x18->rx_buf[1]);
	if (FIELD_GET(ADS1X18_CFG_MODE_MASK, cfg) == ADS1X18_MODE_ONESHOT)
		return 0;

	cfg |= FIELD_PREP(ADS1X18_CFG_MODE_MASK, ADS1X18_MODE_ONESHOT);
	ret = ads1x18_write_config(ads1x18, cfg);
	if (ret)
		return ret;

	ads1x18->restore_mode = 1;

	return 0;
}

static int ads1x18_resume(struct device *dev)
{
	struct ads1x18 *ads1x18 = dev_get_drvdata(dev);
	u16 cfg;
	int ret;

	guard(mutex)(&ads1x18->msg_lock);

	if (!ads1x18->restore_mode)
		return 0;

	cfg = be16_to_cpu(ads1x18->rx_buf[1]);
	FIELD_MODIFY(ADS1X18_CFG_MODE_MASK, &cfg, ADS1X18_MODE_CONTINUOUS);
	ret = ads1x18_write_config(ads1x18, cfg);
	if (ret)
		return ret;

	ads1x18->restore_mode = 0;

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(ads1x18_pm_ops, ads1x18_suspend, ads1x18_resume);

static const int ads1118_fsr_table[][2] = {
	[ADS1X18_PGA_MODE_0] = { 0, 187500 },
	[ADS1X18_PGA_MODE_1] = { 0, 125000 },
	[ADS1X18_PGA_MODE_2] = { 0, 62500 },
	[ADS1X18_PGA_MODE_3] = { 0, 31250 },
	[ADS1X18_PGA_MODE_4] = { 0, 15625 },
	[ADS1X18_PGA_MODE_5] = { 0, 7812 }
};

static const int ads1018_fsr_table[][2] = {
	[ADS1X18_PGA_MODE_0] = { 0, 3000000 },
	[ADS1X18_PGA_MODE_1] = { 0, 2000000 },
	[ADS1X18_PGA_MODE_2] = { 0, 1000000 },
	[ADS1X18_PGA_MODE_3] = { 0, 500000 },
	[ADS1X18_PGA_MODE_4] = { 0, 250000 },
	[ADS1X18_PGA_MODE_5] = { 0, 125000 }
};

static const unsigned int ads1018_drate_table[] = {
	[ADS1X18_DRATE_MODE_0] = 128,
	[ADS1X18_DRATE_MODE_1] = 250,
	[ADS1X18_DRATE_MODE_2] = 490,
	[ADS1X18_DRATE_MODE_3] = 920,
	[ADS1X18_DRATE_MODE_4] = 1600,
	[ADS1X18_DRATE_MODE_5] = 2400,
	[ADS1X18_DRATE_MODE_6] = 3300
};

static const unsigned int ads1118_drate_table[] = {
	[ADS1X18_DRATE_MODE_0] = 8,
	[ADS1X18_DRATE_MODE_1] = 16,
	[ADS1X18_DRATE_MODE_2] = 32,
	[ADS1X18_DRATE_MODE_3] = 64,
	[ADS1X18_DRATE_MODE_4] = 128,
	[ADS1X18_DRATE_MODE_5] = 250,
	[ADS1X18_DRATE_MODE_6] = 475,
	[ADS1X18_DRATE_MODE_7] = 860
};

static const struct ads1x18_chip_info ads1018_chip_info = {
	.name = "ads1018",

	.channels = ads1018_iio_channels,
	.channels_sz = ARRAY_SIZE(ads1018_iio_channels),

	.fsr_table = ads1018_fsr_table,
	.fsr_table_sz = ARRAY_SIZE(ads1018_fsr_table),
	.sps_table = ads1018_drate_table,
	.sps_table_sz = ARRAY_SIZE(ads1018_drate_table),
	.temp_scale = { 0, 125000 },
};

static const struct ads1x18_chip_info ads1118_chip_info = {
	.name = "ads1118",

	.channels = ads1118_iio_channels,
	.channels_sz = ARRAY_SIZE(ads1118_iio_channels),

	.fsr_table = ads1118_fsr_table,
	.fsr_table_sz = ARRAY_SIZE(ads1118_fsr_table),
	.sps_table = ads1118_drate_table,
	.sps_table_sz = ARRAY_SIZE(ads1118_drate_table),
	.temp_scale = { 0, 31250 },
};

static const struct of_device_id ads1x18_of_match[] = {
	{ .compatible = "ti,ads1018", .data = &ads1018_chip_info },
	{ .compatible = "ti,ads1118", .data = &ads1118_chip_info },
	{ }
};
MODULE_DEVICE_TABLE(of, ads1x18_of_match);

static const struct spi_device_id ads1x18_spi_match[] = {
	{ "ads1018", (kernel_ulong_t)&ads1018_chip_info },
	{ "ads1118", (kernel_ulong_t)&ads1118_chip_info },
	{ }
};
MODULE_DEVICE_TABLE(spi, ads1x18_spi_match);

static struct spi_driver ads1x18_spi_driver = {
	.driver = {
		.name = "ads1x18",
		.of_match_table = ads1x18_of_match,
		.pm = pm_sleep_ptr(&ads1x18_pm_ops),
	},
	.probe = ads1x18_spi_probe,
	.id_table = ads1x18_spi_match,
};

module_spi_driver(ads1x18_spi_driver);

MODULE_DESCRIPTION("Texas Instruments ADS1X18 ADC Driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kurt Borja <kuurtb@gmail.com>");
