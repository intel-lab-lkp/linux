// SPDX-License-Identifier: GPL-2.0
/*
 * Nuvoton MA35D1 EADC driver
 *
 * Copyright (c) 2026 Nuvoton Technology Corp.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/bitmap.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/property.h>

#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>

#define MA35D1_EADC_DAT(n)		(0x00 + (n) * 0x04)
#define MA35D1_EADC_CTL			0x50
#define MA35D1_EADC_SWTRG		0x54
#define MA35D1_EADC_SCTL(n)		(0x80 + (n) * 0x04)
#define MA35D1_EADC_INTSRC0		0xd0
#define MA35D1_EADC_STATUS2		0xf8
#define MA35D1_EADC_SELSMP0		0x140
#define MA35D1_EADC_REFADJCTL		0x150

#define MA35D1_EADC_CTL_ADCEN		BIT(0)
#define MA35D1_EADC_CTL_ADCIEN0		BIT(2)
#define MA35D1_EADC_CTL_DIFFEN		BIT(8)

#define MA35D1_EADC_SCTL_CHSEL_MASK	GENMASK(3, 0)
#define MA35D1_EADC_SCTL_TRGDLY_MASK	GENMASK(15, 8)
#define MA35D1_EADC_SCTL_TRGSEL_MASK	GENMASK(21, 16)
#define MA35D1_EADC_SCTL_TRGSEL_ADINT0	\
	FIELD_PREP(MA35D1_EADC_SCTL_TRGSEL_MASK, 2)

#define MA35D1_EADC_DAT_MASK		GENMASK(11, 0)
#define MA35D1_EADC_STATUS2_ADIF0	BIT(0)
#define MA35D1_EADC_INTSRC0_ADINT0	BIT(0)
#define MA35D1_EADC_REFADJCTL_EXT_VREF	BIT(0)

#define MA35D1_EADC_MAX_CHANNELS	9
#define MA35D1_EADC_MAX_SAMPLE_MODULES	16
#define MA35D1_EADC_CHAN_NAME_LEN	16
#define MA35D1_EADC_TIMEOUT		msecs_to_jiffies(1000)

struct ma35d1_adc {
	struct device *dev;
	void __iomem *regs;
	struct clk *clk;
	struct completion completion;
	/* Protects direct conversions against concurrent register access. */
	struct mutex lock;
	struct iio_trigger *trig;
	unsigned int scan_chancnt;
	bool scan_differential;
	char chan_name[MA35D1_EADC_MAX_CHANNELS][MA35D1_EADC_CHAN_NAME_LEN];
	struct {
		u16 channels[MA35D1_EADC_MAX_SAMPLE_MODULES];
		aligned_s64 timestamp;
	} scan;
};

static inline u32 ma35d1_adc_read(struct ma35d1_adc *adc, u32 reg)
{
	return readl(adc->regs + reg);
}

static inline void ma35d1_adc_write(struct ma35d1_adc *adc, u32 reg, u32 val)
{
	writel(val, adc->regs + reg);
}

static void ma35d1_adc_rmw(struct ma35d1_adc *adc, u32 reg, u32 mask, u32 val)
{
	u32 tmp;

	tmp = ma35d1_adc_read(adc, reg);
	tmp &= ~mask;
	tmp |= val;
	ma35d1_adc_write(adc, reg, tmp);
}

static void ma35d1_adc_set_diff(struct ma35d1_adc *adc, bool differential)
{
	ma35d1_adc_rmw(adc, MA35D1_EADC_CTL, MA35D1_EADC_CTL_DIFFEN,
		       differential ? MA35D1_EADC_CTL_DIFFEN : 0);
}

static void ma35d1_adc_config_sample(struct ma35d1_adc *adc,
				     unsigned int sample, unsigned int channel)
{
	u32 reg = MA35D1_EADC_SCTL(sample);

	ma35d1_adc_rmw(adc, reg,
		       MA35D1_EADC_SCTL_CHSEL_MASK |
		       MA35D1_EADC_SCTL_TRGSEL_MASK,
		       FIELD_PREP(MA35D1_EADC_SCTL_CHSEL_MASK, channel) |
		       MA35D1_EADC_SCTL_TRGSEL_ADINT0);
}

static void ma35d1_adc_disable_irq(struct ma35d1_adc *adc)
{
	ma35d1_adc_rmw(adc, MA35D1_EADC_CTL, MA35D1_EADC_CTL_ADCIEN0, 0);
}

static void ma35d1_adc_hw_init(struct ma35d1_adc *adc)
{
	ma35d1_adc_disable_irq(adc);
	ma35d1_adc_rmw(adc, MA35D1_EADC_CTL,
		       MA35D1_EADC_CTL_ADCEN, MA35D1_EADC_CTL_ADCEN);
	ma35d1_adc_write(adc, MA35D1_EADC_STATUS2, MA35D1_EADC_STATUS2_ADIF0);
	ma35d1_adc_rmw(adc, MA35D1_EADC_INTSRC0,
		       MA35D1_EADC_INTSRC0_ADINT0,
		       MA35D1_EADC_INTSRC0_ADINT0);
	ma35d1_adc_rmw(adc, MA35D1_EADC_REFADJCTL,
		       MA35D1_EADC_REFADJCTL_EXT_VREF,
		       MA35D1_EADC_REFADJCTL_EXT_VREF);
	ma35d1_adc_rmw(adc, MA35D1_EADC_SELSMP0, GENMASK(1, 0), 3);
}

static void ma35d1_adc_hw_disable(void *data)
{
	struct ma35d1_adc *adc = data;

	ma35d1_adc_disable_irq(adc);
	ma35d1_adc_rmw(adc, MA35D1_EADC_CTL, MA35D1_EADC_CTL_ADCEN, 0);
}

static irqreturn_t ma35d1_adc_isr(int irq, void *data)
{
	struct iio_dev *indio_dev = data;
	struct ma35d1_adc *adc = iio_priv(indio_dev);
	u32 status;

	status = ma35d1_adc_read(adc, MA35D1_EADC_STATUS2);
	if (!(status & MA35D1_EADC_STATUS2_ADIF0))
		return IRQ_NONE;

	ma35d1_adc_write(adc, MA35D1_EADC_STATUS2, MA35D1_EADC_STATUS2_ADIF0);

	if (iio_buffer_enabled(indio_dev)) {
		ma35d1_adc_disable_irq(adc);
		iio_trigger_poll(adc->trig);
	} else {
		complete(&adc->completion);
	}

	return IRQ_HANDLED;
}

static irqreturn_t ma35d1_adc_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct ma35d1_adc *adc = iio_priv(indio_dev);
	int i;

	for (i = 0; i < adc->scan_chancnt; i++)
		adc->scan.channels[i] =
			ma35d1_adc_read(adc, MA35D1_EADC_DAT(i)) &
			MA35D1_EADC_DAT_MASK;

	iio_push_to_buffers_with_timestamp(indio_dev, &adc->scan, pf->timestamp);
	iio_trigger_notify_done(adc->trig);

	ma35d1_adc_rmw(adc, MA35D1_EADC_CTL, MA35D1_EADC_CTL_ADCIEN0,
		       MA35D1_EADC_CTL_ADCIEN0);
	ma35d1_adc_write(adc, MA35D1_EADC_SWTRG, 1);

	return IRQ_HANDLED;
}

static int ma35d1_adc_read_conversion(struct iio_dev *indio_dev,
				      const struct iio_chan_spec *chan,
				      int *val)
{
	struct ma35d1_adc *adc = iio_priv(indio_dev);
	long timeout;

	reinit_completion(&adc->completion);

	ma35d1_adc_write(adc, MA35D1_EADC_STATUS2, MA35D1_EADC_STATUS2_ADIF0);
	ma35d1_adc_rmw(adc, MA35D1_EADC_SCTL(0),
		       MA35D1_EADC_SCTL_CHSEL_MASK |
		       MA35D1_EADC_SCTL_TRGSEL_MASK,
		       FIELD_PREP(MA35D1_EADC_SCTL_CHSEL_MASK,
				  chan->channel));
	ma35d1_adc_set_diff(adc, chan->differential);
	ma35d1_adc_rmw(adc, MA35D1_EADC_CTL, MA35D1_EADC_CTL_ADCIEN0,
		       MA35D1_EADC_CTL_ADCIEN0);
	ma35d1_adc_write(adc, MA35D1_EADC_SWTRG, 1);

	timeout = wait_for_completion_interruptible_timeout(&adc->completion,
							    MA35D1_EADC_TIMEOUT);
	ma35d1_adc_disable_irq(adc);

	if (timeout < 0)
		return timeout;
	if (!timeout)
		return -ETIMEDOUT;

	*val = ma35d1_adc_read(adc, MA35D1_EADC_DAT(0)) & MA35D1_EADC_DAT_MASK;

	return 0;
}

static int ma35d1_adc_read_raw(struct iio_dev *indio_dev,
			       const struct iio_chan_spec *chan,
			       int *val, int *val2, long mask)
{
	struct ma35d1_adc *adc = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (!iio_device_claim_direct(indio_dev))
			return -EBUSY;

		mutex_lock(&adc->lock);
		ret = ma35d1_adc_read_conversion(indio_dev, chan, val);
		mutex_unlock(&adc->lock);

		iio_device_release_direct(indio_dev);
		if (ret)
			return ret;

		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static int ma35d1_adc_validate_scan(struct iio_dev *indio_dev,
				    const unsigned long *scan_mask)
{
	const struct iio_chan_spec *chan;
	bool have_single = false;
	bool have_diff = false;
	unsigned int count = 0;
	unsigned long bit;

	for_each_set_bit(bit, scan_mask, indio_dev->masklength) {
		chan = &indio_dev->channels[bit];

		if (chan->type == IIO_TIMESTAMP)
			continue;
		count++;
		if (chan->differential)
			have_diff = true;
		else
			have_single = true;
	}

	if (!count || count > MA35D1_EADC_MAX_SAMPLE_MODULES)
		return -EINVAL;

	if (have_single && have_diff)
		return -EINVAL;

	return 0;
}

static int ma35d1_adc_update_scan_mode(struct iio_dev *indio_dev,
				       const unsigned long *scan_mask)
{
	struct ma35d1_adc *adc = iio_priv(indio_dev);
	const struct iio_chan_spec *chan;
	unsigned int sample = 0;
	unsigned long bit;
	bool differential = false;
	int ret;

	ret = ma35d1_adc_validate_scan(indio_dev, scan_mask);
	if (ret)
		return ret;

	for_each_set_bit(bit, scan_mask, indio_dev->masklength) {
		chan = &indio_dev->channels[bit];
		if (chan->type == IIO_TIMESTAMP)
			continue;

		if (!sample)
			differential = chan->differential;

		ma35d1_adc_config_sample(adc, sample, chan->channel);
		sample++;
	}

	adc->scan_chancnt = sample;
	adc->scan_differential = differential;

	return 0;
}

static int ma35d1_adc_buffer_postenable(struct iio_dev *indio_dev)
{
	struct ma35d1_adc *adc = iio_priv(indio_dev);
	int i;

	if (!adc->scan_chancnt)
		return -EINVAL;

	ma35d1_adc_write(adc, MA35D1_EADC_STATUS2, MA35D1_EADC_STATUS2_ADIF0);
	ma35d1_adc_rmw(adc, MA35D1_EADC_INTSRC0,
		       MA35D1_EADC_INTSRC0_ADINT0,
		       MA35D1_EADC_INTSRC0_ADINT0);
	ma35d1_adc_rmw(adc, MA35D1_EADC_REFADJCTL,
		       MA35D1_EADC_REFADJCTL_EXT_VREF,
		       MA35D1_EADC_REFADJCTL_EXT_VREF);
	ma35d1_adc_rmw(adc, MA35D1_EADC_SELSMP0, GENMASK(1, 0), 3);
	ma35d1_adc_set_diff(adc, adc->scan_differential);

	for (i = 0; i < adc->scan_chancnt; i++)
		ma35d1_adc_rmw(adc, MA35D1_EADC_SCTL(i),
			       MA35D1_EADC_SCTL_TRGDLY_MASK,
			       MA35D1_EADC_SCTL_TRGDLY_MASK);

	ma35d1_adc_rmw(adc, MA35D1_EADC_CTL, MA35D1_EADC_CTL_ADCIEN0,
		       MA35D1_EADC_CTL_ADCIEN0);
	ma35d1_adc_write(adc, MA35D1_EADC_SWTRG, 1);

	return 0;
}

static int ma35d1_adc_buffer_predisable(struct iio_dev *indio_dev)
{
	struct ma35d1_adc *adc = iio_priv(indio_dev);
	int i;

	ma35d1_adc_disable_irq(adc);
	for (i = 0; i < adc->scan_chancnt; i++)
		ma35d1_adc_rmw(adc, MA35D1_EADC_SCTL(i),
			       MA35D1_EADC_SCTL_TRGSEL_MASK, 0);

	return 0;
}

static const struct iio_buffer_setup_ops ma35d1_adc_buffer_ops = {
	.postenable = ma35d1_adc_buffer_postenable,
	.predisable = ma35d1_adc_buffer_predisable,
};

static const struct iio_info ma35d1_adc_info = {
	.read_raw = ma35d1_adc_read_raw,
	.update_scan_mode = ma35d1_adc_update_scan_mode,
};

static const struct iio_trigger_ops ma35d1_adc_trigger_ops = {
	.validate_device = iio_trigger_validate_own_device,
};

static void ma35d1_adc_init_channel(struct ma35d1_adc *adc,
				    struct iio_chan_spec *chan, u32 vinp,
				    u32 vinn, int scan_index, bool differential)
{
	char *name = adc->chan_name[vinp];

	chan->type = IIO_VOLTAGE;
	chan->indexed = 1;
	chan->channel = vinp;
	chan->address = vinp;
	chan->scan_index = scan_index;
	chan->info_mask_separate = BIT(IIO_CHAN_INFO_RAW);
	chan->scan_type.sign = 'u';
	chan->scan_type.realbits = 12;
	chan->scan_type.storagebits = 16;
	chan->scan_type.endianness = IIO_CPU;

	if (differential) {
		chan->differential = 1;
		chan->channel2 = vinn;
		snprintf(name, MA35D1_EADC_CHAN_NAME_LEN, "in%d-in%d", vinp,
			 vinn);
	} else {
		snprintf(name, MA35D1_EADC_CHAN_NAME_LEN, "in%d", vinp);
	}

	chan->datasheet_name = name;
}

static int ma35d1_adc_parse_channels(struct iio_dev *indio_dev,
				     struct device *dev)
{
	struct ma35d1_adc *adc = iio_priv(indio_dev);
	DECLARE_BITMAP(used_channels, MA35D1_EADC_MAX_CHANNELS);
	struct fwnode_handle *child;
	struct iio_chan_spec *channels;
	int num_channels;
	int scan_index = 0;
	int ret;

	bitmap_zero(used_channels, MA35D1_EADC_MAX_CHANNELS);

	num_channels = device_get_child_node_count(dev);
	if (!num_channels)
		return dev_err_probe(dev, -ENODATA,
				     "no ADC channels configured\n");

	if (num_channels > MA35D1_EADC_MAX_CHANNELS)
		return dev_err_probe(dev, -EINVAL, "too many ADC channels\n");

	channels = devm_kcalloc(dev, num_channels + 1, sizeof(*channels),
				GFP_KERNEL);
	if (!channels)
		return -ENOMEM;

	device_for_each_child_node(dev, child) {
		u32 diff[2];
		u32 reg;
		bool differential = false;

		ret = fwnode_property_read_u32(child, "reg", &reg);
		if (ret) {
			fwnode_handle_put(child);
			return dev_err_probe(dev, ret,
					     "missing channel reg property\n");
		}

		if (reg >= MA35D1_EADC_MAX_CHANNELS) {
			fwnode_handle_put(child);
			return dev_err_probe(dev, -EINVAL,
					     "invalid ADC channel %u\n", reg);
		}

		if (test_and_set_bit(reg, used_channels)) {
			fwnode_handle_put(child);
			return dev_err_probe(dev, -EINVAL,
					     "duplicate ADC channel %u\n", reg);
		}

		if (fwnode_property_present(child, "diff-channels")) {
			ret = fwnode_property_read_u32_array(child,
							     "diff-channels",
							     diff,
							     ARRAY_SIZE(diff));
			if (ret) {
				fwnode_handle_put(child);
				return dev_err_probe(dev, ret,
						     "invalid diff-channels for channel %u\n",
						     reg);
			}

			if (diff[0] != reg ||
			    diff[1] >= MA35D1_EADC_MAX_CHANNELS ||
			    diff[0] == diff[1]) {
				fwnode_handle_put(child);
				return dev_err_probe(dev, -EINVAL,
						     "invalid differential ADC channel %u-%u\n",
						     diff[0], diff[1]);
			}

			if (test_and_set_bit(diff[1], used_channels)) {
				fwnode_handle_put(child);
				return dev_err_probe(dev, -EINVAL,
						     "ADC channel %u already used\n",
						     diff[1]);
			}

			differential = true;
		}

		ma35d1_adc_init_channel(adc, &channels[scan_index], reg,
					differential ? diff[1] : 0,
					scan_index, differential);
		scan_index++;
	}

	channels[scan_index] = (struct iio_chan_spec)
		IIO_CHAN_SOFT_TIMESTAMP(scan_index);

	indio_dev->channels = channels;
	indio_dev->num_channels = scan_index + 1;
	indio_dev->masklength = indio_dev->num_channels;

	return 0;
}

static int ma35d1_adc_setup_trigger(struct iio_dev *indio_dev,
				    struct device *dev)
{
	struct ma35d1_adc *adc = iio_priv(indio_dev);
	int ret;

	adc->trig = devm_iio_trigger_alloc(dev, "%s-trigger", dev_name(dev));
	if (!adc->trig)
		return -ENOMEM;

	adc->trig->ops = &ma35d1_adc_trigger_ops;
	iio_trigger_set_drvdata(adc->trig, indio_dev);

	ret = devm_iio_trigger_register(dev, adc->trig);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register trigger\n");

	ret = iio_trigger_set_immutable(indio_dev, adc->trig);
	if (ret)
		return dev_err_probe(dev, ret, "failed to set trigger\n");

	return 0;
}

static int ma35d1_adc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct iio_dev *indio_dev;
	struct ma35d1_adc *adc;
	int irq;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*adc));
	if (!indio_dev)
		return -ENOMEM;
	adc = iio_priv(indio_dev);
	adc->dev = dev;
	mutex_init(&adc->lock);
	init_completion(&adc->completion);

	adc->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(adc->regs))
		return dev_err_probe(dev, PTR_ERR(adc->regs),
				     "failed to map registers\n");

	adc->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(adc->clk))
		return dev_err_probe(dev, PTR_ERR(adc->clk),
				     "failed to get and enable ADC clock\n");

	indio_dev->name = "ma35d1-eadc";
	indio_dev->modes = INDIO_DIRECT_MODE | INDIO_BUFFER_TRIGGERED;
	indio_dev->info = &ma35d1_adc_info;

	ret = ma35d1_adc_parse_channels(indio_dev, dev);
	if (ret)
		return ret;

	ma35d1_adc_hw_init(adc);

	ret = devm_add_action_or_reset(dev, ma35d1_adc_hw_disable, adc);
	if (ret)
		return ret;

	ret = ma35d1_adc_setup_trigger(indio_dev, dev);
	if (ret)
		return ret;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_irq(dev, irq, ma35d1_adc_isr, 0, dev_name(dev),
			       indio_dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request IRQ %d\n", irq);

	ret = devm_iio_triggered_buffer_setup(dev, indio_dev,
					      iio_pollfunc_store_time,
					      ma35d1_adc_trigger_handler,
					      &ma35d1_adc_buffer_ops);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to setup triggered buffer\n");

	platform_set_drvdata(pdev, indio_dev);

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register IIO device\n");

	return 0;
}

static int ma35d1_adc_suspend(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct ma35d1_adc *adc = iio_priv(indio_dev);

	if (iio_buffer_enabled(indio_dev))
		return -EBUSY;

	ma35d1_adc_hw_disable(adc);
	clk_disable_unprepare(adc->clk);

	return 0;
}

static int ma35d1_adc_resume(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct ma35d1_adc *adc = iio_priv(indio_dev);
	int ret;

	ret = clk_prepare_enable(adc->clk);
	if (ret)
		return ret;

	ma35d1_adc_hw_init(adc);

	return 0;
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
