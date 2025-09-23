// SPDX-License-Identifier: GPL-2.0

#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/iio/adc-helpers.h>
#include <linux/iio/iio.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>

#define RZT2H_NAME			"rzt2h-adc"

#define RZT2H_ADCSR_REG			0x00
#define RZT2H_ADCSR_ADIE_MASK		BIT(12)
#define RZT2H_ADCSR_ADCS_MASK		GENMASK(14, 13)
#define RZT2H_ADCSR_ADCS_SINGLE		0b00
#define RZT2H_ADCSR_ADST_MASK		BIT(15)

#define RZT2H_ADANSA0_REG		0x04
#define RZT2H_ADANSA0_CH_MASK(x)	BIT(x)

#define RZT2H_ADDR_REG(x)		(0x20 + 0x2 * (x))

#define RZT2H_ADCALCTL_REG		0x1f0
#define RZT2H_ADCALCTL_CAL_MASK		BIT(0)
#define RZT2H_ADCALCTL_CAL_RDY_MASK	BIT(1)
#define RZT2H_ADCALCTL_CAL_ERR_MASK	BIT(2)

#define RZT2H_ADC_MAX_CHANNELS		16
#define RZT2H_ADC_VREF_MV		1800
#define RZT2H_ADC_RESOLUTION		12

struct rzt2h_adc {
	void __iomem *base;
	struct device *dev;

	struct completion completion;
	/* lock to protect against multiple access to the device */
	struct mutex lock;

	const struct iio_chan_spec *channels;
	unsigned int num_channels;

	u16 data[RZT2H_ADC_MAX_CHANNELS];
};

static void rzt2h_adc_start_stop(struct rzt2h_adc *adc, bool start,
				 unsigned int conversion_type)
{
	u16 mask;
	u16 reg;

	reg = readw(adc->base + RZT2H_ADCSR_REG);

	if (start) {
		/* Set conversion type */
		reg &= ~RZT2H_ADCSR_ADCS_MASK;
		reg |= FIELD_PREP(RZT2H_ADCSR_ADCS_MASK, conversion_type);
	}

	/* Toggle end of conversion interrupt and start bit. */
	mask = RZT2H_ADCSR_ADIE_MASK | RZT2H_ADCSR_ADST_MASK;
	if (start)
		reg |= mask;
	else
		reg &= ~mask;

	writew(reg, adc->base + RZT2H_ADCSR_REG);
}

static void rzt2h_adc_start(struct rzt2h_adc *adc, unsigned int conversion_type)
{
	rzt2h_adc_start_stop(adc, true, conversion_type);
}

static void rzt2h_adc_stop(struct rzt2h_adc *adc)
{
	rzt2h_adc_start_stop(adc, false, 0);
}

static int rzt2h_adc_read_single(struct rzt2h_adc *adc, unsigned int ch, int *val)
{
	int ret;

	ret = pm_runtime_resume_and_get(adc->dev);
	if (ret)
		return ret;

	guard(mutex)(&adc->lock);

	reinit_completion(&adc->completion);

	/* Enable a single channel */
	writew(RZT2H_ADANSA0_CH_MASK(ch), adc->base + RZT2H_ADANSA0_REG);

	rzt2h_adc_start(adc, RZT2H_ADCSR_ADCS_SINGLE);

	/*
	 * Datasheet Page 2770, Table 41.1:
	 * 0.32us per channel when sample-and-hold circuits are not in use.
	 */
	ret = wait_for_completion_timeout(&adc->completion, usecs_to_jiffies(1));
	if (!ret) {
		ret = -ETIMEDOUT;
		goto disable;
	}

	*val = adc->data[ch];
	ret = IIO_VAL_INT;

disable:
	rzt2h_adc_stop(adc);

	pm_runtime_put_autosuspend(adc->dev);

	return ret;
}

static void rzt2h_adc_set_cal(struct rzt2h_adc *adc, bool cal)
{
	u16 val;

	val = readw(adc->base + RZT2H_ADCALCTL_REG);
	if (cal)
		val |= RZT2H_ADCALCTL_CAL_MASK;
	else
		val &= ~RZT2H_ADCALCTL_CAL_MASK;

	writew(val, adc->base + RZT2H_ADCALCTL_REG);
}

static int rzt2h_adc_calibrate(struct rzt2h_adc *adc)
{
	u16 val;
	int ret;

	rzt2h_adc_set_cal(adc, true);

	ret = read_poll_timeout(readw, val, val & RZT2H_ADCALCTL_CAL_RDY_MASK,
				200, 1000, true, adc->base + RZT2H_ADCALCTL_REG);
	if (ret) {
		dev_err(adc->dev, "Calibration timed out: %d\n", ret);
		return ret;
	}

	if (val & RZT2H_ADCALCTL_CAL_ERR_MASK) {
		dev_err(adc->dev, "Calibration failed\n");
		return -EINVAL;
	}

	rzt2h_adc_set_cal(adc, false);

	return 0;
}

static int rzt2h_adc_read_raw(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan,
			      int *val, int *val2, long mask)
{
	struct rzt2h_adc *adc = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		return rzt2h_adc_read_single(adc, chan->channel, val);
	case IIO_CHAN_INFO_SCALE:
		*val = RZT2H_ADC_VREF_MV;
		*val2 = RZT2H_ADC_RESOLUTION;
		return IIO_VAL_FRACTIONAL_LOG2;
	default:
		return -EINVAL;
	}
}

static const struct iio_info rzt2h_adc_iio_info = {
	.read_raw = rzt2h_adc_read_raw,
};

static irqreturn_t rzt2h_adc_isr(int irq, void *private)
{
	struct rzt2h_adc *adc = private;
	unsigned long enabled_channels;
	unsigned int ch;

	enabled_channels = readw(adc->base + RZT2H_ADANSA0_REG);
	if (!enabled_channels)
		return IRQ_NONE;

	for_each_set_bit(ch, &enabled_channels, adc->num_channels)
		adc->data[ch] = readw(adc->base + RZT2H_ADDR_REG(ch));

	complete(&adc->completion);

	return IRQ_HANDLED;
}

static const struct iio_chan_spec rzt2h_adc_chan_template = {
	.indexed = 1,
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
			      BIT(IIO_CHAN_INFO_SCALE),
	.type = IIO_VOLTAGE,
};

static int rzt2h_adc_parse_properties(struct rzt2h_adc *adc)
{
	struct iio_chan_spec *chan_array;
	u32 max_channels;
	int ret;

	ret = device_property_read_u32(adc->dev, "renesas,max-channels",
				       &max_channels);
	if (ret)
		return dev_err_probe(adc->dev, ret,
				     "Failed to find max-channels property");

	ret = devm_iio_adc_device_alloc_chaninfo_se(adc->dev,
						    &rzt2h_adc_chan_template,
						    max_channels - 1,
						    &chan_array);
	if (ret < 0)
		return dev_err_probe(adc->dev, ret, "Failed to read channel info");

	adc->num_channels = ret;
	adc->channels = chan_array;

	return 0;
}

static int rzt2h_adc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct iio_dev *indio_dev;
	struct rzt2h_adc *adc;
	int ret;
	int irq;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*adc));
	if (!indio_dev)
		return -ENOMEM;

	adc = iio_priv(indio_dev);
	adc->dev = dev;
	init_completion(&adc->completion);

	ret = devm_mutex_init(dev, &adc->lock);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, indio_dev);

	ret = rzt2h_adc_parse_properties(adc);
	if (ret)
		return ret;

	adc->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(adc->base))
		return PTR_ERR(adc->base);

	pm_runtime_set_autosuspend_delay(dev, 300);
	pm_runtime_use_autosuspend(dev);
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	irq = platform_get_irq_byname(pdev, "adi");
	if (irq < 0)
		return irq;

	ret = devm_request_irq(dev, irq, rzt2h_adc_isr, 0, dev_name(dev), adc);
	if (ret)
		return ret;

	indio_dev->name = RZT2H_NAME;
	indio_dev->info = &rzt2h_adc_iio_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = adc->channels;
	indio_dev->num_channels = adc->num_channels;

	return devm_iio_device_register(dev, indio_dev);
}

static const struct of_device_id rzt2h_adc_match[] = {
	{ .compatible = "renesas,r9a09g077-adc" },
	{ .compatible = "renesas,r9a09g087-adc" },
	{ }
};
MODULE_DEVICE_TABLE(of, rzt2h_adc_match);

static int rzt2h_adc_pm_runtime_resume(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct rzt2h_adc *adc = iio_priv(indio_dev);

	/*
	 * Datasheet Page 2810, Section 41.5.6:
	 * After release from the module-stop state, wait for at least
	 * 0.5 µs before starting A/D conversion.
	 */
	fsleep(1);

	return rzt2h_adc_calibrate(adc);
}

static const struct dev_pm_ops rzt2h_adc_pm_ops = {
	RUNTIME_PM_OPS(NULL, rzt2h_adc_pm_runtime_resume, NULL)
};

static struct platform_driver rzt2h_adc_driver = {
	.probe		= rzt2h_adc_probe,
	.driver		= {
		.name		= RZT2H_NAME,
		.of_match_table = rzt2h_adc_match,
		.pm		= pm_ptr(&rzt2h_adc_pm_ops),
	},
};

module_platform_driver(rzt2h_adc_driver);

MODULE_AUTHOR("Cosmin Tanislav <cosmin-gabriel.tanislav.xa@renesas.com>");
MODULE_DESCRIPTION("Renesas RZ/T2H / RZ/N2H ADC driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("IIO_DRIVER");
