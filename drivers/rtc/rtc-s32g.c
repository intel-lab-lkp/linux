// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2024 NXP
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/rtc.h>

#define RTCC_OFFSET	0x4ul
#define RTCS_OFFSET	0x8ul
#define RTCCNT_OFFSET	0xCul
#define APIVAL_OFFSET	0x10ul
#define RTCVAL_OFFSET	0x14ul

/* RTCC fields */
#define RTCC_CNTEN				BIT(31)
#define RTCC_RTCIE_SHIFT		30
#define RTCC_RTCIE				BIT(RTCC_RTCIE_SHIFT)
#define RTCC_APIEN				BIT(15)
#define RTCC_APIIE				BIT(14)
#define RTCC_CLKSEL_OFFSET		12
#define RTCC_CLKSEL_MASK		GENMASK(13, 12)
#define RTCC_CLKSEL(n)			(((n) << 12) & RTCC_CLKSEL_MASK)
#define RTCC_DIV512EN			BIT(11)
#define RTCC_DIV32EN			BIT(10)

/* RTCS fields */
#define RTCS_RTCF		BIT(29)
#define RTCS_INV_RTC		BIT(18)
#define RTCS_APIF		BIT(13)

#define RTCCNT_MAX_VAL		GENMASK(31, 0)
#define RTC_SYNCH_TIMEOUT	(100 * USEC_PER_MSEC)

#define RTC_CLK_MUX_SIZE	4

/*
 * S32G2 and S32G3 SoCs have RTC clock source 1 reserved and
 * should not be used.
 */
#define RTC_QUIRK_SRC1_RESERVED			BIT(2)

enum {
	RTC_CLK_SRC0,
	RTC_CLK_SRC1,
	RTC_CLK_SRC2,
	RTC_CLK_SRC3
};

enum {
	DIV1 = 1,
	DIV32 = 32,
	DIV512 = 512,
	DIV512_32 = 16384
};

static const char *rtc_clk_src[RTC_CLK_MUX_SIZE] = {
	"source0",
	"source1",
	"source2",
	"source3"
};

struct rtc_time_base {
	s64 sec;
	u64 cycles;
	struct rtc_time tm;
};

struct rtc_priv {
	struct rtc_device *rdev;
	void __iomem *rtc_base;
	struct clk *ipg;
	struct clk *clk_src;
	const struct rtc_soc_data *rtc_data;
	struct rtc_time_base base;
	u64 rtc_hz;
	int dt_irq_id;
	int clk_src_idx;
};

struct rtc_soc_data {
	u32 clk_div;
	u32 quirks;
};

static const struct rtc_soc_data rtc_s32g2_data = {
	.clk_div = DIV512,
	.quirks = RTC_QUIRK_SRC1_RESERVED,
};

static int is_src1_reserved(struct rtc_priv *priv)
{
	return priv->rtc_data->quirks & RTC_QUIRK_SRC1_RESERVED;
}

static u64 cycles_to_sec(u64 hz, u64 cycles)
{
	return div_u64(cycles, hz);
}

/**
 * Convert a number of seconds to a value suitable for RTCVAL in our clock's
 * current configuration.
 * @rtcval: The value to go into RTCVAL[RTCVAL]
 * Returns: 0 for success, -EINVAL if @seconds push the counter past the
 *          32bit register range
 */
static int sec_to_rtcval(const struct rtc_priv *priv,
			 unsigned long seconds, u32 *rtcval)
{
	u32 delta_cnt;

	if (!seconds || seconds > cycles_to_sec(priv->rtc_hz, RTCCNT_MAX_VAL))
		return -EINVAL;

	/*
	 * RTCCNT is read-only; we must return a value relative to the
	 * current value of the counter (and hope we don't linger around
	 * too much before we get to enable the interrupt)
	 */
	delta_cnt = seconds * priv->rtc_hz;
	*rtcval = delta_cnt + ioread32(priv->rtc_base + RTCCNT_OFFSET);

	return 0;
}

static irqreturn_t s32g_rtc_handler(int irq, void *dev)
{
	struct rtc_priv *priv = platform_get_drvdata(dev);
	u32 status;

	status = ioread32(priv->rtc_base + RTCS_OFFSET);

	if (status & RTCS_RTCF) {
		iowrite32(0x0, priv->rtc_base + RTCVAL_OFFSET);
		rtc_update_irq(priv->rdev, 1, RTC_AF);
	}

	if (status & RTCS_APIF)
		rtc_update_irq(priv->rdev, 1, RTC_PF);

	iowrite32(status, priv->rtc_base + RTCS_OFFSET);

	return IRQ_HANDLED;
}

static s64 s32g_rtc_get_time_or_alrm(struct rtc_priv *priv,
				     u32 offset)
{
	u32 counter;

	counter = ioread32(priv->rtc_base + offset);

	if (counter < priv->base.cycles)
		return -EINVAL;

	counter -= priv->base.cycles;

	return priv->base.sec + cycles_to_sec(priv->rtc_hz, counter);
}

static int s32g_rtc_read_time(struct device *dev,
			      struct rtc_time *tm)
{
	struct rtc_priv *priv = dev_get_drvdata(dev);
	s64 sec;

	sec = s32g_rtc_get_time_or_alrm(priv, RTCCNT_OFFSET);
	if (sec < 0)
		return -EINVAL;

	rtc_time64_to_tm(sec, tm);

	return 0;
}

static int s32g_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct rtc_priv *priv = dev_get_drvdata(dev);
	u32 rtcc, rtccnt, rtcval;
	s64 sec;

	sec = s32g_rtc_get_time_or_alrm(priv, RTCVAL_OFFSET);
	if (sec < 0)
		return -EINVAL;

	rtc_time64_to_tm(sec, &alrm->time);

	rtcc = ioread32(priv->rtc_base + RTCC_OFFSET);
	alrm->enabled = sec && (rtcc & RTCC_RTCIE);

	alrm->pending = 0;
	if (alrm->enabled) {
		rtccnt = ioread32(priv->rtc_base + RTCCNT_OFFSET);
		rtcval = ioread32(priv->rtc_base + RTCVAL_OFFSET);

		if (rtccnt < rtcval)
			alrm->pending = 1;
	}

	return 0;
}

static int s32g_rtc_alarm_irq_enable(struct device *dev, unsigned int enabled)
{
	struct rtc_priv *priv = dev_get_drvdata(dev);
	u32 rtcc;

	if (!priv->dt_irq_id)
		return -EIO;

	rtcc = ioread32(priv->rtc_base + RTCC_OFFSET);
	if (enabled)
		rtcc |= RTCC_RTCIE;

	iowrite32(rtcc, priv->rtc_base + RTCC_OFFSET);

	return 0;
}

static int s32g_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct rtc_priv *priv = dev_get_drvdata(dev);
	struct rtc_time time_crt;
	long long t_crt, t_alrm;
	u32 rtcval, rtcs;
	int ret = 0;

	iowrite32(0x0, priv->rtc_base + RTCVAL_OFFSET);

	t_alrm = rtc_tm_to_time64(&alrm->time);

	/*
	 * Assuming the alarm is being set relative to the same time
	 * returned by our s32g_rtc_read_time callback
	 */
	ret = s32g_rtc_read_time(dev, &time_crt);
	if (ret)
		return ret;

	t_crt = rtc_tm_to_time64(&time_crt);
	ret = sec_to_rtcval(priv, t_alrm - t_crt, &rtcval);
	if (ret) {
		dev_warn(dev, "Alarm is set too far in the future\n");
		return -ERANGE;
	}

	ret = read_poll_timeout(ioread32, rtcs, !(rtcs & RTCS_INV_RTC),
				0, RTC_SYNCH_TIMEOUT, false, priv->rtc_base + RTCS_OFFSET);
	if (ret)
		return ret;

	iowrite32(rtcval, priv->rtc_base + RTCVAL_OFFSET);

	return 0;
}

static int s32g_rtc_set_time(struct device *dev,
			     struct rtc_time *time)
{
	struct rtc_priv *priv = dev_get_drvdata(dev);

	priv->base.cycles = ioread32(priv->rtc_base + RTCCNT_OFFSET);
	priv->base.sec = rtc_tm_to_time64(time);

	return 0;
}

/*
 * Disable the 32-bit free running counter.
 * This allows Clock Source and Divisors selection
 * to be performed without causing synchronization issues.
 */
static void s32g_rtc_disable(struct rtc_priv *priv)
{
	u32 rtcc = ioread32(priv->rtc_base + RTCC_OFFSET);

	rtcc &= ~RTCC_CNTEN;
	iowrite32(rtcc, priv->rtc_base + RTCC_OFFSET);
}

static void s32g_rtc_enable(struct rtc_priv *priv)
{
	u32 rtcc = ioread32(priv->rtc_base + RTCC_OFFSET);

	rtcc |= RTCC_CNTEN;
	iowrite32(rtcc, priv->rtc_base + RTCC_OFFSET);
}

static int rtc_clk_src_setup(struct rtc_priv *priv)
{
	u32 rtcc = 0;

	switch (priv->clk_src_idx) {
	case RTC_CLK_SRC0:
		rtcc |= RTCC_CLKSEL(RTC_CLK_SRC0);
		break;
	case RTC_CLK_SRC1:
		if (is_src1_reserved(priv))
			return -EOPNOTSUPP;
		rtcc |= RTCC_CLKSEL(RTC_CLK_SRC1);
		break;
	case RTC_CLK_SRC2:
		rtcc |= RTCC_CLKSEL(RTC_CLK_SRC2);
		break;
	case RTC_CLK_SRC3:
		rtcc |= RTCC_CLKSEL(RTC_CLK_SRC3);
		break;
	default:
		return -EINVAL;
	}

	switch (priv->rtc_data->clk_div) {
	case DIV512_32:
		rtcc |= RTCC_DIV512EN;
		rtcc |= RTCC_DIV32EN;
		break;
	case DIV512:
		rtcc |= RTCC_DIV512EN;
		break;
	case DIV32:
		rtcc |= RTCC_DIV32EN;
		break;
	case DIV1:
		break;
	default:
		return -EINVAL;
	}

	rtcc |= RTCC_RTCIE;
	/*
	 * Make sure the CNTEN is 0 before we configure
	 * the clock source and dividers.
	 */
	s32g_rtc_disable(priv);
	iowrite32(rtcc, priv->rtc_base + RTCC_OFFSET);
	s32g_rtc_enable(priv);

	return 0;
}

static const struct rtc_class_ops rtc_ops = {
	.read_time = s32g_rtc_read_time,
	.set_time = s32g_rtc_set_time,
	.read_alarm = s32g_rtc_read_alarm,
	.set_alarm = s32g_rtc_set_alarm,
	.alarm_irq_enable = s32g_rtc_alarm_irq_enable,
};

static int rtc_clk_dts_setup(struct rtc_priv *priv,
			     struct device *dev)
{
	int i;

	priv->ipg = devm_clk_get_enabled(dev, "ipg");
	if (IS_ERR(priv->ipg))
		return dev_err_probe(dev, PTR_ERR(priv->ipg),
				"Failed to get 'ipg' clock\n");

	for (i = 0; i < RTC_CLK_MUX_SIZE; i++) {
		priv->clk_src = devm_clk_get_enabled(dev, rtc_clk_src[i]);
		if (!IS_ERR(priv->clk_src)) {
			priv->clk_src_idx = i;
			break;
		}
	}

	if (IS_ERR(priv->clk_src))
		return dev_err_probe(dev, PTR_ERR(priv->clk_src),
				"Failed to get rtc module clock source\n");

	return 0;
}

static int s32g_rtc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtc_priv *priv;
	int ret = 0;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->rtc_data = of_device_get_match_data(dev);
	if (!priv->rtc_data)
		return -ENODEV;

	priv->rtc_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->rtc_base))
		return PTR_ERR(priv->rtc_base);

	device_init_wakeup(dev, true);

	ret = rtc_clk_dts_setup(priv, dev);
	if (ret)
		return ret;

	priv->rdev = devm_rtc_allocate_device(dev);
	if (IS_ERR(priv->rdev))
		return PTR_ERR(priv->rdev);

	ret = rtc_clk_src_setup(priv);
	if (ret)
		return ret;

	priv->rtc_hz = clk_get_rate(priv->clk_src);
	if (!priv->rtc_hz)
		return dev_err_probe(dev, -EINVAL, "Failed to get RTC frequency\n");

	priv->rtc_hz /= priv->rtc_data->clk_div;

	platform_set_drvdata(pdev, priv);
	priv->rdev->ops = &rtc_ops;

	priv->dt_irq_id = platform_get_irq(pdev, 0);
	if (priv->dt_irq_id < 0)
		return priv->dt_irq_id;

	ret = devm_request_irq(dev, priv->dt_irq_id,
			       s32g_rtc_handler, 0, dev_name(dev), pdev);
	if (ret) {
		dev_err(dev, "Request interrupt %d failed, error: %d\n",
			priv->dt_irq_id, ret);
		goto disable_rtc;
	}

	ret = devm_rtc_register_device(priv->rdev);
	if (ret)
		goto disable_rtc;

	return 0;

disable_rtc:
	s32g_rtc_disable(priv);
	return ret;
}

static void enable_api_irq(struct device *dev, unsigned int enabled)
{
	struct rtc_priv *priv = dev_get_drvdata(dev);
	u32 api_irq = RTCC_APIEN | RTCC_APIIE;
	u32 rtcc;

	rtcc = ioread32(priv->rtc_base + RTCC_OFFSET);
	if (enabled)
		rtcc |= api_irq;
	else
		rtcc &= ~api_irq;
	iowrite32(rtcc, priv->rtc_base + RTCC_OFFSET);
}

static int s32g_rtc_suspend(struct device *dev)
{
	struct rtc_priv *init_priv = dev_get_drvdata(dev);
	struct rtc_priv priv;
	long long base_sec;
	u32 rtcval, rtccnt;
	int ret = 0;
	u32 sec;

	if (!device_may_wakeup(dev))
		return 0;

	/* Save last known timestamp */
	ret = s32g_rtc_read_time(dev, &init_priv->base.tm);
	if (ret)
		return ret;

	/*
	 * Use a local copy of the RTC control block to
	 * avoid restoring it on resume path.
	 */
	memcpy(&priv, init_priv, sizeof(priv));

	rtccnt = ioread32(init_priv->rtc_base + RTCCNT_OFFSET);
	rtcval = ioread32(init_priv->rtc_base + RTCVAL_OFFSET);
	sec = cycles_to_sec(init_priv->rtc_hz, rtcval - rtccnt);

	/* Adjust for the number of seconds we'll be asleep */
	base_sec = rtc_tm_to_time64(&init_priv->base.tm);
	base_sec += sec;
	rtc_time64_to_tm(base_sec, &init_priv->base.tm);

	/* Reset RTC to prevent overflow.
	 * RTCCNT (RTC Counter) cannot be individually reset
	 * since it is RO (read-only).
	 */
	s32g_rtc_disable(&priv);
	s32g_rtc_enable(&priv);

	ret = sec_to_rtcval(&priv, sec, &rtcval);
	if (ret) {
		dev_warn(dev, "Alarm is too far in the future\n");
		return -ERANGE;
	}

	enable_api_irq(dev, 1);
	iowrite32(rtcval, priv.rtc_base + APIVAL_OFFSET);
	iowrite32(0, priv.rtc_base + RTCVAL_OFFSET);

	return ret;
}

static int s32g_rtc_resume(struct device *dev)
{
	struct rtc_priv *priv = dev_get_drvdata(dev);
	int ret;

	if (!device_may_wakeup(dev))
		return 0;

	/* Disable wake-up interrupts */
	enable_api_irq(dev, 0);

	ret = rtc_clk_src_setup(priv);
	if (ret)
		return ret;

	/*
	 * Now RTCCNT has just been reset, and is out of sync with priv->base;
	 * reapply the saved time settings.
	 */
	return s32g_rtc_set_time(dev, &priv->base.tm);
}

static const struct of_device_id rtc_dt_ids[] = {
	{ .compatible = "nxp,s32g2-rtc", .data = &rtc_s32g2_data},
	{ /* sentinel */ },
};

static DEFINE_SIMPLE_DEV_PM_OPS(s32g_rtc_pm_ops,
			 s32g_rtc_suspend, s32g_rtc_resume);

static struct platform_driver s32g_rtc_driver = {
	.driver		= {
		.name			= "s32g-rtc",
		.pm				= pm_sleep_ptr(&s32g_rtc_pm_ops),
		.of_match_table = rtc_dt_ids,
	},
	.probe		= s32g_rtc_probe,
};
module_platform_driver(s32g_rtc_driver);

MODULE_AUTHOR("NXP");
MODULE_DESCRIPTION("NXP RTC driver for S32G2/S32G3");
MODULE_LICENSE("GPL");
