// SPDX-License-Identifier: GPL-2.0
/*
 * Nuvoton MA35D1 EADC driver
 *
 * Copyright (c) 2026 Nuvoton Technology Corp.
 */

#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/cleanup.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/time.h>
#include <linux/types.h>
#include <linux/units.h>

#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>

#define MA35D1_EADC_RESOLUTION		12
#define MA35D1_EADC_DIFF_SCALE_BITS	(MA35D1_EADC_RESOLUTION - 1)

#define MA35D1_EADC_DAT(n)		(0x000 + (n) * 0x004)
#define   MA35D1_EADC_DAT_RESULT_12BIT	GENMASK(MA35D1_EADC_RESOLUTION - 1, 0)
#define   MA35D1_EADC_DAT_OV		BIT(16)
#define   MA35D1_EADC_DAT_VALID		BIT(17)

#define MA35D1_EADC_CURDAT		0x04c

#define MA35D1_EADC_CTL			0x050
#define   MA35D1_EADC_CTL_ADCEN		BIT(0)
#define   MA35D1_EADC_CTL_EADCRST	BIT(1)
#define   MA35D1_EADC_CTL_ADCIEN0	BIT(2)
#define   MA35D1_EADC_CTL_DIFFEN	BIT(8)
#define   MA35D1_EADC_CTL_DMOF		BIT(9)
#define   MA35D1_EADC_CTL_VREFSEL_MASK	GENMASK(11, 10)
#define     MA35D1_EADC_CTL_VREFSEL_1V6	FIELD_PREP(MA35D1_EADC_CTL_VREFSEL_MASK, 0)

#define MA35D1_EADC_SWTRG		0x054
#define   MA35D1_EADC_SWTRG_SWTRG0	BIT(0)

#define MA35D1_EADC_PENDSTS		0x058
#define MA35D1_EADC_OVSTS		0x05c

#define MA35D1_EADC_SCTL(n)		(0x080 + (n) * 0x004)
#define   MA35D1_EADC_SCTL_CHSEL_MASK	GENMASK(3, 0)
#define   MA35D1_EADC_SCTL_TRGSEL_MASK	GENMASK(21, 16)
#define   MA35D1_EADC_SCTL_INTPOS	BIT(22)

#define MA35D1_EADC_INTSRC0		0x0d0
#define   MA35D1_EADC_INTSRC0_SPLIEN0	BIT(0)
#define   MA35D1_EADC_INTSRC0_SPLIEN1	BIT(1)
#define   MA35D1_EADC_INTSRC0_SPLIEN2	BIT(2)
#define   MA35D1_EADC_INTSRC0_SPLIEN3	BIT(3)
#define   MA35D1_EADC_INTSRC0_SPLIEN4	BIT(4)
#define   MA35D1_EADC_INTSRC0_SPLIEN5	BIT(5)
#define   MA35D1_EADC_INTSRC0_SPLIEN6	BIT(6)
#define   MA35D1_EADC_INTSRC0_SPLIEN7	BIT(7)
#define   MA35D1_EADC_INTSRC0_SPLIEN8	BIT(8)
#define   MA35D1_EADC_INTSRC0_SPLIEN_MASK \
	(MA35D1_EADC_INTSRC0_SPLIEN0 | MA35D1_EADC_INTSRC0_SPLIEN1 | \
	 MA35D1_EADC_INTSRC0_SPLIEN2 | MA35D1_EADC_INTSRC0_SPLIEN3 | \
	 MA35D1_EADC_INTSRC0_SPLIEN4 | MA35D1_EADC_INTSRC0_SPLIEN5 | \
	 MA35D1_EADC_INTSRC0_SPLIEN6 | MA35D1_EADC_INTSRC0_SPLIEN7 | \
	 MA35D1_EADC_INTSRC0_SPLIEN8)

#define MA35D1_EADC_CMP(n)		(0x0e0 + (n) * 0x004)
#define MA35D1_EADC_STATUS0		0x0f0

#define MA35D1_EADC_STATUS2		0x0f8
#define   MA35D1_EADC_STATUS2_ADIF0	BIT(0)

#define MA35D1_EADC_DDAT(n)		(0x100 + (n) * 0x004)

#define MA35D1_EADC_PWRM		0x110
#define   MA35D1_EADC_PWRM_PWUPRDY	BIT(0)

#define MA35D1_EADC_CALCTL		0x114

#define MA35D1_EADC_PDMACTL		0x130

#define MA35D1_EADC_SELSMP0		0x140
#define   MA35D1_EADC_SELSMP0_SMPT0_MASK GENMASK(1, 0)
#define     MA35D1_EADC_SELSMP_LONG	FIELD_PREP(MA35D1_EADC_SELSMP0_SMPT0_MASK, 3)

#define MA35D1_EADC_SELSMP1		0x144

#define MA35D1_EADC_REFADJCTL		0x150
#define   MA35D1_EADC_REFADJCTL_PDREF	BIT(0)

#define MA35D1_EADC_MAX_EXT_CHANNELS	8
#define MA35D1_EADC_NUM_DIFF_PAIRS	4
#define MA35D1_EADC_NUM_SAMPLE_MODULES	9
/* Sample module 8 is fixed to VBAT and has no SCTL register. */
#define MA35D1_EADC_NUM_SCTL		8
#define MA35D1_EADC_NUM_CMP		4
#define MA35D1_EADC_NUM_DDAT		4
#define MA35D1_EADC_INTERNAL_VREF_mV	1600
#define MA35D1_EADC_TIMEOUT		msecs_to_jiffies(1000)
#define MA35D1_EADC_RESET_DELAY_US	10
#define MA35D1_EADC_REF_STABLE_US	(1 * USEC_PER_MSEC)

struct ma35d1_adc {
	struct regmap *regmap;
	struct clk *clk;
	struct reset_control *rst;
	struct completion completion;
	/* Protect conversion state and register accesses from PM transitions. */
	struct mutex lock;
	const struct iio_chan_spec *scan_chan;
	int irq;
	unsigned int vref_mV;
	bool external_vref;
	/* Set while the clock is gated; EADC registers must not be accessed. */
	bool suspended;
};

static const struct regmap_range ma35d1_adc_readable_ranges[] = {
	regmap_reg_range(MA35D1_EADC_DAT(0),
			 MA35D1_EADC_DAT(MA35D1_EADC_NUM_SAMPLE_MODULES - 1)),
	regmap_reg_range(MA35D1_EADC_CURDAT, MA35D1_EADC_CTL),
	regmap_reg_range(MA35D1_EADC_PENDSTS, MA35D1_EADC_OVSTS),
	regmap_reg_range(MA35D1_EADC_SCTL(0),
			 MA35D1_EADC_SCTL(MA35D1_EADC_NUM_SCTL - 1)),
	regmap_reg_range(MA35D1_EADC_INTSRC0, MA35D1_EADC_STATUS0),
	regmap_reg_range(MA35D1_EADC_STATUS2, MA35D1_EADC_CALCTL),
	regmap_reg_range(MA35D1_EADC_PDMACTL, MA35D1_EADC_PDMACTL),
	regmap_reg_range(MA35D1_EADC_SELSMP0, MA35D1_EADC_SELSMP1),
	regmap_reg_range(MA35D1_EADC_REFADJCTL, MA35D1_EADC_REFADJCTL),
};

static const struct regmap_access_table ma35d1_adc_readable_table = {
	.yes_ranges = ma35d1_adc_readable_ranges,
	.n_yes_ranges = ARRAY_SIZE(ma35d1_adc_readable_ranges),
};

static const struct regmap_range ma35d1_adc_writable_ranges[] = {
	regmap_reg_range(MA35D1_EADC_CTL, MA35D1_EADC_OVSTS),
	regmap_reg_range(MA35D1_EADC_SCTL(0),
			 MA35D1_EADC_SCTL(MA35D1_EADC_NUM_SCTL - 1)),
	regmap_reg_range(MA35D1_EADC_INTSRC0,
			 MA35D1_EADC_CMP(MA35D1_EADC_NUM_CMP - 1)),
	regmap_reg_range(MA35D1_EADC_STATUS2, MA35D1_EADC_STATUS2),
	regmap_reg_range(MA35D1_EADC_PWRM, MA35D1_EADC_CALCTL),
	regmap_reg_range(MA35D1_EADC_PDMACTL, MA35D1_EADC_PDMACTL),
	regmap_reg_range(MA35D1_EADC_SELSMP0, MA35D1_EADC_SELSMP1),
	regmap_reg_range(MA35D1_EADC_REFADJCTL, MA35D1_EADC_REFADJCTL),
};

static const struct regmap_access_table ma35d1_adc_writable_table = {
	.yes_ranges = ma35d1_adc_writable_ranges,
	.n_yes_ranges = ARRAY_SIZE(ma35d1_adc_writable_ranges),
};

/*
 * Reading DATn or DDATn clears the VALID and OV flags of the result. CURDAT
 * is the PDMA shadow of DATn, so treat it as read-sensitive as well.
 */
static const struct regmap_range ma35d1_adc_precious_ranges[] = {
	regmap_reg_range(MA35D1_EADC_DAT(0),
			 MA35D1_EADC_DAT(MA35D1_EADC_NUM_SAMPLE_MODULES - 1)),
	regmap_reg_range(MA35D1_EADC_CURDAT, MA35D1_EADC_CURDAT),
	regmap_reg_range(MA35D1_EADC_DDAT(0),
			 MA35D1_EADC_DDAT(MA35D1_EADC_NUM_DDAT - 1)),
};

static const struct regmap_access_table ma35d1_adc_precious_table = {
	.yes_ranges = ma35d1_adc_precious_ranges,
	.n_yes_ranges = ARRAY_SIZE(ma35d1_adc_precious_ranges),
};

/*
 * Keep the register map uncached because status and conversion data are
 * updated by hardware, and some status bits use write-one-to-clear semantics.
 */
static const struct regmap_config ma35d1_adc_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = MA35D1_EADC_REFADJCTL,
	.rd_table = &ma35d1_adc_readable_table,
	.wr_table = &ma35d1_adc_writable_table,
	.precious_table = &ma35d1_adc_precious_table,
};

static int ma35d1_adc_enable_irq(struct ma35d1_adc *adc)
{
	return regmap_set_bits(adc->regmap, MA35D1_EADC_CTL,
			       MA35D1_EADC_CTL_ADCIEN0);
}

static int ma35d1_adc_disable_irq(struct ma35d1_adc *adc)
{
	return regmap_clear_bits(adc->regmap, MA35D1_EADC_CTL,
				 MA35D1_EADC_CTL_ADCIEN0);
}

static int ma35d1_adc_setup_reference(struct ma35d1_adc *adc)
{
	struct regmap *map = adc->regmap;
	int ret;

	/*
	 * PDREF powers down the internal reference, so an external reference
	 * must then be supplied through the VREF pin. With the internal
	 * reference, VREFSEL = 0 selects 1.6 V.
	 */
	if (adc->external_vref) {
		ret = regmap_set_bits(map, MA35D1_EADC_REFADJCTL,
				      MA35D1_EADC_REFADJCTL_PDREF);
		if (ret)
			return ret;
	} else {
		ret = regmap_clear_bits(map, MA35D1_EADC_REFADJCTL,
					MA35D1_EADC_REFADJCTL_PDREF);
		if (ret)
			return ret;

		ret = regmap_update_bits(map, MA35D1_EADC_CTL,
					 MA35D1_EADC_CTL_VREFSEL_MASK,
					 MA35D1_EADC_CTL_VREFSEL_1V6);
		if (ret)
			return ret;
	}

	fsleep(MA35D1_EADC_REF_STABLE_US);

	return 0;
}

static int ma35d1_adc_reset_control(struct ma35d1_adc *adc)
{
	struct regmap *map = adc->regmap;
	unsigned int val;
	int ret;

	ret = regmap_set_bits(map, MA35D1_EADC_CTL, MA35D1_EADC_CTL_EADCRST);
	if (ret)
		return ret;

	return regmap_read_poll_timeout(map, MA35D1_EADC_CTL, val,
					!(val & MA35D1_EADC_CTL_EADCRST),
					1, 1 * USEC_PER_MSEC);
}

static int ma35d1_adc_reset(struct ma35d1_adc *adc)
{
	int ret;

	ret = reset_control_assert(adc->rst);
	if (ret)
		return ret;

	fsleep(MA35D1_EADC_RESET_DELAY_US);

	return reset_control_deassert(adc->rst);
}

static int ma35d1_adc_hw_init(struct ma35d1_adc *adc)
{
	struct regmap *map = adc->regmap;
	unsigned int val;
	int ret;

	/*
	 * Reset the EADC at probe and on resume. The register reset values
	 * disable all interrupt, PDMA and hardware trigger sources and clear
	 * ADIF0 and the VALID and OV flags of the conversion results.
	 */
	ret = ma35d1_adc_reset(adc);
	if (ret)
		return ret;

	ret = ma35d1_adc_setup_reference(adc);
	if (ret)
		return ret;

	ret = regmap_update_bits(map, MA35D1_EADC_SELSMP0,
				 MA35D1_EADC_SELSMP0_SMPT0_MASK,
				 MA35D1_EADC_SELSMP_LONG);
	if (ret)
		return ret;

	ret = regmap_set_bits(map, MA35D1_EADC_CTL, MA35D1_EADC_CTL_ADCEN);
	if (ret)
		return ret;

	ret = regmap_read_poll_timeout(map, MA35D1_EADC_PWRM, val,
				       val & MA35D1_EADC_PWRM_PWUPRDY,
				       10, 1 * USEC_PER_SEC);
	if (ret) {
		int err;

		/* ADCEN must stay set until PWUPRDY, so abort through module reset. */
		err = ma35d1_adc_reset(adc);
		if (err)
			dev_err(regmap_get_device(map),
				"failed to reset ADC after power-up failure: %d\n",
				err);
	}

	return ret;
}

static int ma35d1_adc_hw_disable(struct ma35d1_adc *adc)
{
	struct regmap *map = adc->regmap;
	int ret;

	ret = ma35d1_adc_disable_irq(adc);
	if (ret)
		return ret;

	return regmap_clear_bits(map, MA35D1_EADC_CTL, MA35D1_EADC_CTL_ADCEN);
}

static void ma35d1_adc_hw_disable_action(void *data)
{
	struct ma35d1_adc *adc = data;

	if (!adc->suspended)
		ma35d1_adc_hw_disable(adc);
}

static int ma35d1_adc_config_channel(struct ma35d1_adc *adc,
				     const struct iio_chan_spec *chan)
{
	struct regmap *map = adc->regmap;
	unsigned int mask;
	int ret;

	mask = MA35D1_EADC_CTL_DIFFEN | MA35D1_EADC_CTL_DMOF;
	ret = regmap_assign_bits(map, MA35D1_EADC_CTL, mask, chan->differential);
	if (ret)
		return ret;

	/*
	 * INTPOS = 0 generates ADIF0 at end of conversion. TRGSEL = 0 disables
	 * the hardware trigger; conversions are started through SWTRG0.
	 */
	mask = MA35D1_EADC_SCTL_CHSEL_MASK | MA35D1_EADC_SCTL_TRGSEL_MASK |
	       MA35D1_EADC_SCTL_INTPOS;
	ret = regmap_update_bits(map, MA35D1_EADC_SCTL(0), mask,
				 FIELD_PREP(MA35D1_EADC_SCTL_CHSEL_MASK,
					    chan->channel));
	if (ret)
		return ret;

	return regmap_update_bits(map, MA35D1_EADC_INTSRC0,
				  MA35D1_EADC_INTSRC0_SPLIEN_MASK,
				  MA35D1_EADC_INTSRC0_SPLIEN0);
}

static void ma35d1_adc_abort_conversion(struct ma35d1_adc *adc)
{
	struct regmap *map = adc->regmap;
	struct device *dev = regmap_get_device(map);
	unsigned int val;
	int ret;

	/*
	 * Stop ADC interrupt generation before resetting the conversion state,
	 * then wait for any running IRQ handler before discarding the result
	 * and clearing ADIF0.
	 */
	ret = ma35d1_adc_disable_irq(adc);
	if (ret)
		dev_err(dev, "failed to disable ADC interrupt: %d\n", ret);

	/*
	 * Reset the EADC control circuits to terminate a conversion which may
	 * still be in progress. EADCRST is cleared automatically by hardware
	 * when the reset completes and the EADC register values are preserved.
	 */
	ret = ma35d1_adc_reset_control(adc);
	if (ret)
		dev_err(dev, "failed to reset timed out conversion: %d\n", ret);

	synchronize_irq(adc->irq);

	ret = regmap_read(map, MA35D1_EADC_DAT(0), &val);
	if (ret)
		dev_err(dev, "failed to discard timed out ADC data: %d\n", ret);

	ret = regmap_write(map, MA35D1_EADC_STATUS2, MA35D1_EADC_STATUS2_ADIF0);
	if (ret)
		dev_err(dev, "failed to clear ADC interrupt status: %d\n", ret);
}

static int ma35d1_adc_read_conversion(struct ma35d1_adc *adc,
				      const struct iio_chan_spec *chan, u16 *raw)
{
	struct regmap *map = adc->regmap;
	unsigned int val;
	long timeout;
	int ret;

	reinit_completion(&adc->completion);

	ret = regmap_write(map, MA35D1_EADC_STATUS2, MA35D1_EADC_STATUS2_ADIF0);
	if (ret)
		return ret;

	ret = ma35d1_adc_config_channel(adc, chan);
	if (ret)
		return ret;

	ret = ma35d1_adc_enable_irq(adc);
	if (ret)
		return ret;

	ret = regmap_write(map, MA35D1_EADC_SWTRG, MA35D1_EADC_SWTRG_SWTRG0);
	if (ret) {
		ma35d1_adc_disable_irq(adc);
		return ret;
	}

	timeout = wait_for_completion_timeout(&adc->completion, MA35D1_EADC_TIMEOUT);
	if (!timeout) {
		ma35d1_adc_abort_conversion(adc);
		return -ETIMEDOUT;
	}

	ret = ma35d1_adc_disable_irq(adc);
	if (ret)
		return ret;

	ret = regmap_read(map, MA35D1_EADC_DAT(0), &val);
	if (ret)
		return ret;

	if (!(val & MA35D1_EADC_DAT_VALID))
		return -EIO;

	if (val & MA35D1_EADC_DAT_OV)
		return -EOVERFLOW;

	*raw = FIELD_GET(MA35D1_EADC_DAT_RESULT_12BIT, val);

	return 0;
}

static irqreturn_t ma35d1_adc_isr(int irq, void *data)
{
	struct iio_dev *indio_dev = data;
	struct ma35d1_adc *adc = iio_priv(indio_dev);
	struct regmap *map = adc->regmap;
	unsigned int status;
	int ret;

	ret = regmap_read(map, MA35D1_EADC_STATUS2, &status);
	if (ret)
		return IRQ_NONE;

	if (!(status & MA35D1_EADC_STATUS2_ADIF0))
		return IRQ_NONE;

	ret = regmap_write(map, MA35D1_EADC_STATUS2, MA35D1_EADC_STATUS2_ADIF0);
	if (ret)
		dev_err_ratelimited(regmap_get_device(map),
				    "failed to clear ADC interrupt status: %d\n", ret);

	complete(&adc->completion);

	return IRQ_HANDLED;
}

static int ma35d1_adc_read_raw(struct iio_dev *indio_dev,
			       const struct iio_chan_spec *chan,
			       int *val, int *val2, long mask)
{
	struct ma35d1_adc *adc = iio_priv(indio_dev);
	u16 raw;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW: {
		IIO_DEV_ACQUIRE_DIRECT_MODE(indio_dev, claim);
		if (IIO_DEV_ACQUIRE_FAILED(claim))
			return -EBUSY;

		guard(mutex)(&adc->lock);

		if (adc->suspended)
			return -EBUSY;

		ret = ma35d1_adc_read_conversion(adc, chan, &raw);
		if (ret)
			return ret;

		if (chan->differential)
			*val = sign_extend32(raw, MA35D1_EADC_RESOLUTION - 1);
		else
			*val = raw;

		return IIO_VAL_INT;
	}
	case IIO_CHAN_INFO_SCALE:
		*val = adc->vref_mV;
		/* Differential mode has a VREF / 2048 LSB; single-ended uses /4096. */
		*val2 = chan->differential ? MA35D1_EADC_DIFF_SCALE_BITS : MA35D1_EADC_RESOLUTION;
		return IIO_VAL_FRACTIONAL_LOG2;
	default:
		return -EINVAL;
	}
}

static int ma35d1_adc_update_scan_mode(struct iio_dev *indio_dev,
				       const unsigned long *scan_mask)
{
	struct ma35d1_adc *adc = iio_priv(indio_dev);
	const struct iio_chan_spec *scan_chan = NULL;
	int bit;

	if (!iio_validate_scan_mask_onehot(indio_dev, scan_mask))
		return -EINVAL;

	iio_for_each_active_channel(indio_dev, bit) {
		const struct iio_chan_spec *chan = &indio_dev->channels[bit];

		if (chan->type != IIO_VOLTAGE)
			return -EINVAL;

		scan_chan = chan;
		break;
	}

	if (!scan_chan)
		return -EINVAL;

	adc->scan_chan = scan_chan;

	return 0;
}

static int ma35d1_adc_buffer_postenable(struct iio_dev *indio_dev)
{
	struct ma35d1_adc *adc = iio_priv(indio_dev);

	guard(mutex)(&adc->lock);

	if (adc->suspended)
		return -EBUSY;

	if (!adc->scan_chan)
		return -EINVAL;

	return 0;
}

static void ma35d1_adc_trigger_capture(struct iio_poll_func *pf)
{
	struct iio_dev *indio_dev = pf->indio_dev;
	struct ma35d1_adc *adc = iio_priv(indio_dev);
	struct {
		u16 channel;
		aligned_s64 timestamp;
	} scan = { };
	int ret;

	guard(mutex)(&adc->lock);

	if (adc->suspended || !adc->scan_chan)
		return;

	ret = ma35d1_adc_read_conversion(adc, adc->scan_chan,
					 &scan.channel);
	if (ret)
		return;

	iio_push_to_buffers_with_ts(indio_dev, &scan, sizeof(scan), pf->timestamp);
}

static irqreturn_t ma35d1_adc_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;

	ma35d1_adc_trigger_capture(pf);
	iio_trigger_notify_done(pf->indio_dev->trig);

	return IRQ_HANDLED;
}

static const struct iio_buffer_setup_ops ma35d1_adc_buffer_ops = {
	.postenable = ma35d1_adc_buffer_postenable,
	.validate_scan_mask = iio_validate_scan_mask_onehot,
};

static int ma35d1_adc_fwnode_xlate(struct iio_dev *indio_dev,
				   const struct fwnode_reference_args *iiospec)
{
	if (iiospec->nargs != 1)
		return -EINVAL;

	for (unsigned int i = 0; i < indio_dev->num_channels; i++) {
		const struct iio_chan_spec *chan = &indio_dev->channels[i];

		if (chan->type == IIO_VOLTAGE &&
		    chan->address == iiospec->args[0])
			return i;
	}

	return -EINVAL;
}

static const struct iio_info ma35d1_adc_info = {
	.read_raw = ma35d1_adc_read_raw,
	.update_scan_mode = ma35d1_adc_update_scan_mode,
	.fwnode_xlate = ma35d1_adc_fwnode_xlate,
};

static int ma35d1_adc_parse_diff_channel(struct device *dev,
					 struct fwnode_handle *child,
					 struct iio_chan_spec *chan,
					 unsigned long *used_diff_pairs,
					 u32 reg, unsigned int scan_index)
{
	u32 inputs[2];
	int ret;

	ret = fwnode_property_read_u32_array(child, "diff-channels", inputs,
					     ARRAY_SIZE(inputs));
	if (ret)
		return dev_err_probe(dev, ret,
				     "invalid diff-channels for index %u\n", reg);

	if (inputs[0] >= MA35D1_EADC_NUM_DIFF_PAIRS || inputs[1] != inputs[0] + 4)
		return dev_err_probe(dev, -ECHRNG,
				     "invalid differential inputs %u-%u\n",
				     inputs[0], inputs[1]);

	if (test_and_set_bit(inputs[0], used_diff_pairs))
		return dev_err_probe(dev, -EEXIST,
				     "duplicate differential inputs %u-%u\n",
				     inputs[0], inputs[1]);

	*chan = (struct iio_chan_spec) {
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.differential = 1,
		.channel = inputs[0],
		.channel2 = inputs[1],
		.address = reg,
		.scan_index = scan_index,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.scan_type = {
			.format = IIO_SCAN_FORMAT_SIGNED_INT,
			.realbits = MA35D1_EADC_RESOLUTION,
			.storagebits = 16,
			.endianness = IIO_CPU,
		},
	};

	return 0;
}

static int ma35d1_adc_parse_single_channel(struct device *dev,
					   struct fwnode_handle *child,
					   struct iio_chan_spec *chan,
					   unsigned long *used_single_inputs,
					   bool has_single, u32 reg,
					   unsigned int scan_index)
{
	u32 input = reg;
	int ret;

	if (has_single) {
		ret = fwnode_property_read_u32(child, "single-channel", &input);
		if (ret)
			return dev_err_probe(dev, ret,
					     "invalid single-channel for index %u\n",
					     reg);
	}

	if (input >= MA35D1_EADC_MAX_EXT_CHANNELS)
		return dev_err_probe(dev, -ECHRNG, "invalid ADC input %u\n", input);

	if (test_and_set_bit(input, used_single_inputs))
		return dev_err_probe(dev, -EEXIST, "duplicate ADC input %u\n", input);

	*chan = (struct iio_chan_spec) {
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.channel = input,
		.address = reg,
		.scan_index = scan_index,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.scan_type = {
			.format = IIO_SCAN_FORMAT_UNSIGNED_INT,
			.realbits = MA35D1_EADC_RESOLUTION,
			.storagebits = 16,
			.endianness = IIO_CPU,
		},
	};

	return 0;
}

static int ma35d1_adc_parse_channels(struct iio_dev *indio_dev,
				     struct device *dev)
{
	DECLARE_BITMAP(used_single_inputs, MA35D1_EADC_MAX_EXT_CHANNELS) = { };
	DECLARE_BITMAP(used_diff_pairs, MA35D1_EADC_NUM_DIFF_PAIRS) = { };
	DECLARE_BITMAP(used_regs, MA35D1_EADC_MAX_EXT_CHANNELS) = { };
	struct iio_chan_spec *channels;
	unsigned int scan_index = 0;
	int num_channels;
	int ret;

	num_channels = device_get_child_node_count(dev);
	if (!num_channels)
		return dev_err_probe(dev, -ENOENT, "no ADC channels configured\n");

	if (num_channels > MA35D1_EADC_MAX_EXT_CHANNELS)
		return dev_err_probe(dev, -ENOSPC, "too many ADC channels\n");

	channels = devm_kcalloc(dev, size_add(num_channels, 1),
				sizeof(*channels), GFP_KERNEL);
	if (!channels)
		return -ENOMEM;

	device_for_each_child_node_scoped(dev, child) {
		bool has_diff, has_single;
		u32 reg;

		ret = fwnode_property_read_u32(child, "reg", &reg);
		if (ret)
			return dev_err_probe(dev, ret,
					     "missing channel reg property\n");

		if (reg >= MA35D1_EADC_MAX_EXT_CHANNELS)
			return dev_err_probe(dev, -ENOSPC,
					     "invalid ADC channel index %u\n", reg);

		if (test_and_set_bit(reg, used_regs))
			return dev_err_probe(dev, -EEXIST,
					     "duplicate ADC channel index %u\n", reg);

		has_single = fwnode_property_present(child, "single-channel");
		has_diff = fwnode_property_present(child, "diff-channels");
		if (has_single && has_diff)
			return dev_err_probe(dev, -EINVAL,
					     "channel %u has conflicting input properties\n",
					     reg);

		if (has_diff)
			ret = ma35d1_adc_parse_diff_channel(dev, child,
							    &channels[scan_index],
							    used_diff_pairs, reg,
							    scan_index);
		else
			ret = ma35d1_adc_parse_single_channel(dev, child,
							      &channels[scan_index],
							      used_single_inputs,
							      has_single, reg,
							      scan_index);
		if (ret)
			return ret;

		scan_index++;
	}

	channels[scan_index] = IIO_CHAN_SOFT_TIMESTAMP(scan_index);
	indio_dev->channels = channels;
	indio_dev->num_channels = scan_index + 1;

	return 0;
}

static int ma35d1_adc_init_vref(struct ma35d1_adc *adc, struct device *dev)
{
	int ret;

	ret = devm_regulator_get_enable_read_voltage(dev, "vref");
	if (ret == -ENODEV) {
		adc->vref_mV = MA35D1_EADC_INTERNAL_VREF_mV;
	} else if (ret < 0) {
		return dev_err_probe(dev, ret, "failed to get VREF supply\n");
	} else if (ret > 0) {
		adc->external_vref = true;
		adc->vref_mV = ret / (MICRO / MILLI);
	} else {
		return dev_err_probe(dev, -EINVAL, "invalid VREF voltage\n");
	}

	return 0;
}

static void ma35d1_adc_clk_disable_action(void *data)
{
	struct ma35d1_adc *adc = data;

	/* A failed resume leaves the clock gated. */
	if (adc->suspended)
		return;

	clk_disable_unprepare(adc->clk);
}

static int ma35d1_adc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct iio_dev *indio_dev;
	struct ma35d1_adc *adc;
	void __iomem *regs;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*adc));
	if (!indio_dev)
		return -ENOMEM;

	adc = iio_priv(indio_dev);
	init_completion(&adc->completion);

	ret = devm_mutex_init(dev, &adc->lock);
	if (ret)
		return ret;

	regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(regs))
		return dev_err_probe(dev, PTR_ERR(regs),
				     "failed to map registers\n");

	adc->regmap = devm_regmap_init_mmio(dev, regs, &ma35d1_adc_regmap_config);
	if (IS_ERR(adc->regmap))
		return dev_err_probe(dev, PTR_ERR(adc->regmap),
				     "failed to initialize regmap\n");

	adc->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(adc->clk))
		return dev_err_probe(dev, PTR_ERR(adc->clk),
				     "failed to get ADC clock\n");

	ret = clk_prepare_enable(adc->clk);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable ADC clock\n");

	ret = devm_add_action_or_reset(dev, ma35d1_adc_clk_disable_action, adc);
	if (ret)
		return ret;

	adc->rst = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(adc->rst))
		return dev_err_probe(dev, PTR_ERR(adc->rst),
				     "failed to get reset control\n");

	ret = ma35d1_adc_init_vref(adc, dev);
	if (ret)
		return ret;

	indio_dev->name = "ma35d1-eadc";
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &ma35d1_adc_info;

	ret = ma35d1_adc_parse_channels(indio_dev, dev);
	if (ret)
		return ret;

	ret = ma35d1_adc_hw_init(adc);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize ADC\n");

	ret = devm_add_action_or_reset(dev, ma35d1_adc_hw_disable_action, adc);
	if (ret)
		return ret;

	adc->irq = platform_get_irq(pdev, 0);
	if (adc->irq < 0)
		return adc->irq;

	ret = devm_request_irq(dev, adc->irq, ma35d1_adc_isr, 0,
			       dev_name(dev), indio_dev);
	if (ret)
		return ret;

	ret = devm_iio_triggered_buffer_setup(dev, indio_dev,
					      iio_pollfunc_store_time,
					      ma35d1_adc_trigger_handler,
					      &ma35d1_adc_buffer_ops);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, indio_dev);

	return devm_iio_device_register(dev, indio_dev);
}

static int ma35d1_adc_suspend_hw(struct ma35d1_adc *adc)
{
	struct regmap *map = adc->regmap;
	int ret;

	guard(mutex)(&adc->lock);

	if (adc->suspended)
		return 0;

	ret = ma35d1_adc_disable_irq(adc);
	if (ret)
		return ret;

	synchronize_irq(adc->irq);

	ret = regmap_clear_bits(map, MA35D1_EADC_CTL, MA35D1_EADC_CTL_ADCEN);
	if (ret) {
		/*
		 * ADCIEN0 is enabled only while a conversion is active. Holding
		 * adc->lock here guarantees that no conversion is in progress, so
		 * leave it disabled. The next conversion enables it again.
		 */
		return ret;
	}

	adc->suspended = true;
	clk_disable_unprepare(adc->clk);

	return 0;
}

static int ma35d1_adc_resume_hw(struct ma35d1_adc *adc)
{
	int ret;

	guard(mutex)(&adc->lock);

	if (!adc->suspended)
		return 0;

	ret = clk_prepare_enable(adc->clk);
	if (ret)
		return ret;

	ret = ma35d1_adc_hw_init(adc);
	if (ret) {
		clk_disable_unprepare(adc->clk);
		return ret;
	}

	adc->suspended = false;

	return 0;
}

static int ma35d1_adc_suspend(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct ma35d1_adc *adc = iio_priv(indio_dev);
	bool buffer_enabled;
	int ret;

	/* A failed resume leaves the device and its triggering suspended. */
	scoped_guard(mutex, &adc->lock) {
		if (adc->suspended)
			return 0;
	}

	/*
	 * pollfunc->irq is not initialized until the pollfunc is first
	 * attached to a trigger, so only suspend and resume triggering
	 * while the buffer is enabled.
	 */
	buffer_enabled = iio_buffer_enabled(indio_dev);
	if (buffer_enabled) {
		ret = iio_device_suspend_triggering(indio_dev);
		if (ret)
			return ret;
	}

	ret = ma35d1_adc_suspend_hw(adc);
	if (ret && buffer_enabled)
		iio_device_resume_triggering(indio_dev);

	return ret;
}

static int ma35d1_adc_resume(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct ma35d1_adc *adc = iio_priv(indio_dev);
	int ret;

	scoped_guard(mutex, &adc->lock) {
		if (!adc->suspended)
			return 0;
	}

	ret = ma35d1_adc_resume_hw(adc);
	if (ret)
		return ret;

	if (!iio_buffer_enabled(indio_dev))
		return 0;

	return iio_device_resume_triggering(indio_dev);
}

static DEFINE_SIMPLE_DEV_PM_OPS(ma35d1_adc_pm_ops,
				ma35d1_adc_suspend, ma35d1_adc_resume);

static const struct of_device_id ma35d1_adc_of_match[] = {
	{ .compatible = "nuvoton,ma35d1-eadc" },
	{ }
};
MODULE_DEVICE_TABLE(of, ma35d1_adc_of_match);

static struct platform_driver ma35d1_adc_driver = {
	.probe = ma35d1_adc_probe,
	.driver = {
		.name = "ma35d1-eadc",
		.of_match_table = ma35d1_adc_of_match,
		.pm = pm_sleep_ptr(&ma35d1_adc_pm_ops),
	},
};
module_platform_driver(ma35d1_adc_driver);

MODULE_AUTHOR("Chi-Wen Weng <cwweng@nuvoton.com>");
MODULE_DESCRIPTION("Nuvoton MA35D1 EADC driver");
MODULE_LICENSE("GPL");
