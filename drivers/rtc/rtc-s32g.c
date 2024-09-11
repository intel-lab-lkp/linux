// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2024 NXP
 */

#include <linux/clk.h>
#include <linux/of_irq.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/rtc.h>
#include <linux/iopoll.h>

#define RTCSUPV_OFFSET	0x0ul
#define RTCC_OFFSET	0x4ul
#define RTCS_OFFSET	0x8ul
#define RTCCNT_OFFSET	0xCul
#define APIVAL_OFFSET	0x10ul
#define RTCVAL_OFFSET	0x14ul

/* RTCSUPV fields */
#define RTCSUPV_SUPV		BIT(31)

/* RTCC fields */
#define RTCC_CNTEN		BIT(31)
#define RTCC_RTCIE_SHIFT	30
#define RTCC_RTCIE		BIT(RTCC_RTCIE_SHIFT)
#define RTCC_ROVREN		BIT(28)
#define RTCC_APIEN		BIT(15)
#define RTCC_APIIE		BIT(14)
#define RTCC_CLKSEL_MASK	GENMASK(13, 12)
#define RTCC_CLKSEL(n)		(((n) << 12) & RTCC_CLKSEL_MASK)
#define RTCC_DIV512EN		BIT(11)
#define RTCC_DIV32EN		BIT(10)

/* RTCS fields */
#define RTCS_RTCF		BIT(29)
#define RTCS_INV_RTC		BIT(18)
#define RTCS_APIF		BIT(13)
#define RTCS_ROVRF		BIT(10)

#define ROLLOVER_VAL		0xFFFFFFFFULL
#define RTC_SYNCH_TIMEOUT	(100 * USEC_PER_MSEC)

/* Clock sources - usable with RTCC_CLKSEL */
#define S32G_RTC_SOURCE_SIRC	0x0
#define S32G_RTC_SOURCE_FIRC	0x2

struct rtc_time_base {
	s64 sec;
	u64 cycles;
	u64 rollovers;
#ifdef CONFIG_PM_SLEEP
	struct rtc_time tm;
#endif
};

struct rtc_priv {
	struct rtc_device *rdev;
	struct device *dev;
	u8 __iomem *rtc_base;
	struct clk *firc;
	struct clk *sirc;
	struct clk *ipg;
	struct rtc_time_base base;
	u64 rtc_hz;
	u64 rollovers;
	int dt_irq_id;
	u8 clk_source;
	bool div512;
	bool div32;
};

static u64 cycles_to_sec(u64 hz, u64 cycles)
{
	return cycles / hz;
}

/*
 * Convert a number of seconds to a value suitable for RTCVAL in our clock's
 * current configuration.
 * @rtcval: The value to go into RTCVAL[RTCVAL]
 * Returns: 0 for success, -EINVAL if @seconds push the counter at least
 *          twice the rollover interval
 */
static int sec_to_rtcval(const struct rtc_priv *priv,
			 unsigned long seconds, u32 *rtcval)
{
	u32 rtccnt, delta_cnt;
	u32 target_cnt = 0;

	/* For now, support at most one rollover of the counter */
	if (!seconds || seconds > cycles_to_sec(priv->rtc_hz, ULONG_MAX))
		return -EINVAL;

	/*
	 * RTCCNT is read-only; we must return a value relative to the
	 * current value of the counter (and hope we don't linger around
	 * too much before we get to enable the interrupt)
	 */
	delta_cnt = seconds * priv->rtc_hz;
	rtccnt = ioread32(priv->rtc_base + RTCCNT_OFFSET);

	if (~rtccnt < delta_cnt)
		target_cnt = (delta_cnt - ~rtccnt);
	else
		target_cnt = rtccnt + delta_cnt;

	/*
	 * According to RTCVAL register description,
	 * its minimum value should be 4.
	 */
	if (unlikely(target_cnt < 4))
		target_cnt = 4;

	*rtcval = target_cnt;

	return 0;
}

static irqreturn_t rtc_handler(int irq, void *dev)
{
	struct rtc_priv *priv = platform_get_drvdata(dev);
	u32 status;

	status = ioread32(priv->rtc_base + RTCS_OFFSET);
	if (status & RTCS_ROVRF) {
		if (priv->rollovers == ULONG_MAX)
			priv->rollovers = 0;
		else
			priv->rollovers++;
	}

	if (status & RTCS_RTCF) {
		iowrite32(0x0, priv->rtc_base + RTCVAL_OFFSET);
		rtc_update_irq(priv->rdev, 1, RTC_AF);
	}

	if (status & RTCS_APIF)
		rtc_update_irq(priv->rdev, 1, RTC_PF);

	iowrite32(status, priv->rtc_base + RTCS_OFFSET);

	return IRQ_HANDLED;
}

static int get_time_left(struct device *dev, struct rtc_priv *priv,
			 u32 *sec)
{
	u32 rtccnt = ioread32(priv->rtc_base + RTCCNT_OFFSET);
	u32 rtcval = ioread32(priv->rtc_base + RTCVAL_OFFSET);

	if (rtcval < rtccnt) {
		dev_err(dev, "RTC timer expired before entering suspend\n");
		return -EIO;
	}

	*sec = cycles_to_sec(priv->rtc_hz, rtcval - rtccnt);

	return 0;
}

static int s32g_rtc_get_time_or_alrm(struct rtc_priv *priv,
				     u32 offset)
{
	u64 cycles, sec, base_cycles;
	u32 counter;

	counter = ioread32(priv->rtc_base + offset);
	cycles = priv->rollovers * ROLLOVER_VAL + counter;
	base_cycles = priv->base.cycles + priv->base.rollovers * ROLLOVER_VAL;

	if (cycles < base_cycles)
		return -EINVAL;

	cycles -= base_cycles;
	sec = priv->base.sec + cycles_to_sec(priv->rtc_hz, cycles);

	return sec;
}

static int s32g_rtc_read_time(struct device *dev,
			      struct rtc_time *tm)
{
	struct rtc_priv *priv = dev_get_drvdata(dev);
	u64 sec;

	if (!tm)
		return -EINVAL;

	sec = s32g_rtc_get_time_or_alrm(priv, RTCCNT_OFFSET);
	if (sec < 0)
		return -EINVAL;

	rtc_time64_to_tm(sec, tm);

	return 0;
}

static int s32g_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct rtc_priv *priv = dev_get_drvdata(dev);
	u32 rtcc, sec_left;
	u64 sec;

	if (!alrm)
		return -EINVAL;

	sec = s32g_rtc_get_time_or_alrm(priv, RTCVAL_OFFSET);
	if (sec < 0)
		return -EINVAL;

	rtc_time64_to_tm(sec, &alrm->time);

	rtcc = ioread32(priv->rtc_base + RTCC_OFFSET);
	alrm->enabled = sec && (rtcc & RTCC_RTCIE);

	alrm->pending = 0;
	if (alrm->enabled && !get_time_left(dev, priv, &sec_left))
		alrm->pending = !!sec_left;

	return 0;
}

static int s32g_rtc_alarm_irq_enable(struct device *dev, unsigned int enabled)
{
	struct rtc_priv *priv = dev_get_drvdata(dev);
	u32 rtcc;

	if (!priv->dt_irq_id)
		return -EIO;

	/*
	 * RTCIE cannot be deasserted because it will also disable the
	 * rollover interrupt.
	 */
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
	int ret = 0;
	u32 rtcval, rtcs;

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
	if (t_alrm <= t_crt) {
		dev_warn(dev, "Alarm is set in the past\n");
		return -EINVAL;
	}

	ret = sec_to_rtcval(priv, t_alrm - t_crt, &rtcval);
	if (ret) {
		dev_warn(dev, "Alarm is set too far in the future\n");
		return ret;
	}

	ret = read_poll_timeout(ioread32, rtcs, !(rtcs & RTCS_INV_RTC),
				0, RTC_SYNCH_TIMEOUT, false, priv->rtc_base + RTCS_OFFSET);
	if (ret) {
		dev_err(dev, "Synchronization failed\n");
		return ret;
	}

	iowrite32(rtcval, priv->rtc_base + RTCVAL_OFFSET);

	return 0;
}

static int s32g_rtc_set_time(struct device *dev,
			     struct rtc_time *time)
{
	struct rtc_priv *priv = dev_get_drvdata(dev);

	if (!time)
		return -EINVAL;

	priv->base.rollovers = priv->rollovers;
	priv->base.cycles = ioread32(priv->rtc_base + RTCCNT_OFFSET);
	priv->base.sec = rtc_tm_to_time64(time);

	return 0;
}

static const struct rtc_class_ops rtc_ops = {
	.read_time = s32g_rtc_read_time,
	.set_time = s32g_rtc_set_time,
	.read_alarm = s32g_rtc_read_alarm,
	.set_alarm = s32g_rtc_set_alarm,
	.alarm_irq_enable = s32g_rtc_alarm_irq_enable,
};

static void rtc_disable(struct rtc_priv *priv)
{
	u32 rtcc = ioread32(priv->rtc_base + RTCC_OFFSET);

	rtcc &= ~RTCC_CNTEN;
	iowrite32(rtcc, priv->rtc_base + RTCC_OFFSET);
}

static void rtc_enable(struct rtc_priv *priv)
{
	u32 rtcc = ioread32(priv->rtc_base + RTCC_OFFSET);

	rtcc |= RTCC_CNTEN;
	iowrite32(rtcc, priv->rtc_base + RTCC_OFFSET);
}

static int rtc_init(struct rtc_priv *priv)
{
	struct device *dev = priv->dev;
	struct clk *sclk;
	u32 rtcc = 0;
	u32 clksel;
	int ret;

	ret = clk_prepare_enable(priv->ipg);
	if (ret) {
		dev_err(dev, "Cannot enable 'ipg' clock, error: %d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(priv->sirc);
	if (ret) {
		dev_err(dev, "Cannot enable 'sirc' clock, error: %d\n", ret);
		goto disable_ipg_clk;
	}

	ret = clk_prepare_enable(priv->firc);
	if (ret) {
		dev_err(dev, "Cannot enable 'firc' clock, error: %d\n", ret);
		goto disable_sirc_clk;
	}

	/* Make sure the RTC ticking is disabled before we configure dividers */
	rtc_disable(priv);

	clksel = RTCC_CLKSEL(priv->clk_source);
	rtcc |= clksel;

	/* Precompute the base frequency of the clock */
	switch (clksel) {
	case RTCC_CLKSEL(S32G_RTC_SOURCE_SIRC):
		sclk = priv->sirc;
		break;
	case RTCC_CLKSEL(S32G_RTC_SOURCE_FIRC):
		sclk = priv->firc;
		break;
	default:
		dev_err(dev, "Invalid clksel value: %u\n", clksel);
		ret = -EINVAL;
		goto disable_firc_clk;
	}

	priv->rtc_hz = clk_get_rate(sclk);
	if (!priv->rtc_hz) {
		dev_err(dev, "Invalid RTC frequency\n");
		ret = -EINVAL;
		goto disable_firc_clk;
	}

	/* Adjust frequency if dividers are enabled */
	if (priv->div512) {
		rtcc |= RTCC_DIV512EN;
		priv->rtc_hz /= 512;
	}

	if (priv->div32) {
		rtcc |= RTCC_DIV32EN;
		priv->rtc_hz /= 32;
	}

	rtcc |= RTCC_RTCIE | RTCC_ROVREN;
	iowrite32(rtcc, priv->rtc_base + RTCC_OFFSET);

	return 0;

disable_firc_clk:
	clk_disable_unprepare(priv->firc);
disable_sirc_clk:
	clk_disable_unprepare(priv->sirc);
disable_ipg_clk:
	clk_disable_unprepare(priv->ipg);
	return ret;
}

static int priv_dts_init(struct rtc_priv *priv)
{
	struct device *dev = priv->dev;
	struct platform_device *pdev = to_platform_device(dev);
	u32 clksel = S32G_RTC_SOURCE_SIRC;
	/* div512 and div32 */
	u32 div[2] = { 0 };
	int ret;

	priv->sirc = devm_clk_get(dev, "sirc");
	if (IS_ERR(priv->sirc)) {
		dev_err(dev, "Failed to get 'sirc' clock\n");
		return PTR_ERR(priv->sirc);
	}

	priv->firc = devm_clk_get(dev, "firc");
	if (IS_ERR(priv->firc)) {
		dev_err(dev, "Failed to get 'firc' clock\n");
		return PTR_ERR(priv->firc);
	}

	priv->ipg = devm_clk_get(dev, "ipg");
	if (IS_ERR(priv->ipg)) {
		dev_err(dev, "Failed to get 'ipg' clock\n");
		return PTR_ERR(priv->ipg);
	}

	priv->dt_irq_id = platform_get_irq(pdev, 0);
	if (priv->dt_irq_id < 0) {
		dev_err(dev, "Error reading interrupt # from dts\n");
		return priv->dt_irq_id;
	}

	ret = device_property_read_u32_array(dev, "nxp,dividers", div,
					     ARRAY_SIZE(div));
	if (ret) {
		dev_err(dev, "Error reading dividers configuration, err: %d\n", ret);
		return ret;
	}

	ret = device_property_read_u32(dev, "nxp,clksel", &clksel);
	if (ret) {
		dev_err(dev, "Error reading clksel configuration, err: %d\n", ret);
		return ret;
	}

	priv->div512 = !!div[0];
	priv->div32 = !!div[1];

	switch (clksel) {
	case S32G_RTC_SOURCE_SIRC:
	case S32G_RTC_SOURCE_FIRC:
		priv->clk_source = clksel;
		break;
	default:
		dev_err(dev, "Unsupported clksel: %d\n", clksel);
		return -EINVAL;
	}

	return 0;
}

static int rtc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtc_priv *priv;
	int ret = 0;

	priv = devm_kzalloc(dev, sizeof(struct rtc_priv),
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->rtc_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->rtc_base)) {
		dev_err(dev, "Failed to map registers\n");
		return PTR_ERR(priv->rtc_base);
	}

	device_init_wakeup(dev, true);
	priv->dev = dev;

	ret = priv_dts_init(priv);
	if (ret)
		return ret;

	ret = rtc_init(priv);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, priv);
	rtc_enable(priv);

	priv->rdev = devm_rtc_device_register(dev, "s32g_rtc",
					      &rtc_ops, THIS_MODULE);
	if (IS_ERR_OR_NULL(priv->rdev)) {
		dev_err(dev, "Error registering RTC device, err: %ld\n",
			PTR_ERR(priv->rdev));
		ret = PTR_ERR(priv->rdev);
		goto disable_rtc;
	}

	ret = devm_request_irq(dev, priv->dt_irq_id,
			       rtc_handler, 0, dev_name(dev), pdev);
	if (ret) {
		dev_err(&pdev->dev, "Request interrupt %d failed, error: %d\n",
			priv->dt_irq_id, ret);
		goto disable_rtc;
	}

	return 0;

disable_rtc:
	rtc_disable(priv);
	return ret;
}

static void rtc_remove(struct platform_device *pdev)
{
	struct rtc_priv *priv = platform_get_drvdata(pdev);

	rtc_disable(priv);
}

#ifdef CONFIG_PM_SLEEP
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

static int adjust_dividers(u32 sec, struct rtc_priv *priv)
{
	u64 rtcval_max = U32_MAX;
	u64 rtcval;

	priv->div32 = 0;
	priv->div512 = 0;

	rtcval = sec * priv->rtc_hz;
	if (rtcval < rtcval_max)
		return 0;

	if (rtcval / 32 < rtcval_max) {
		priv->div32 = 1;
		return 0;
	}

	if (rtcval / 512 < rtcval_max) {
		priv->div512 = 1;
		return 0;
	}

	if (rtcval / (512 * 32) < rtcval_max) {
		priv->div32 = 1;
		priv->div512 = 1;
		return 0;
	}

	return -EINVAL;
}

static int rtc_suspend(struct device *dev)
{
	struct rtc_priv *init_priv = dev_get_drvdata(dev);
	struct rtc_priv priv;
	long long base_sec;
	int ret = 0;
	u32 rtcval;
	u32 sec;

	if (!device_may_wakeup(dev))
		return 0;

	/* Save last known timestamp before we switch clocks and reinit RTC */
	ret = s32g_rtc_read_time(dev, &priv.base.tm);
	if (ret)
		return ret;

	if (init_priv->clk_source == S32G_RTC_SOURCE_SIRC)
		return 0;

	/*
	 * Use a local copy of the RTC control block to
	 * avoid restoring it on resume path.
	 */
	memcpy(&priv, init_priv, sizeof(priv));

	/* Switch to SIRC */
	priv.clk_source = S32G_RTC_SOURCE_SIRC;

	ret = get_time_left(dev, init_priv, &sec);
	if (ret)
		return ret;

	/* Adjust for the number of seconds we'll be asleep */
	base_sec = rtc_tm_to_time64(&init_priv->base.tm);
	base_sec += sec;
	rtc_time64_to_tm(base_sec, &init_priv->base.tm);

	rtc_disable(&priv);

	ret = adjust_dividers(sec, &priv);
	if (ret) {
		dev_err(dev, "Failed to adjust RTC dividers to match a %u seconds delay\n", sec);
		return ret;
	}

	ret = rtc_init(&priv);
	if (ret)
		return ret;

	ret = sec_to_rtcval(&priv, sec, &rtcval);
	if (ret) {
		dev_warn(dev, "Alarm is too far in the future\n");
		return ret;
	}

	s32g_rtc_alarm_irq_enable(dev, 0);
	enable_api_irq(dev, 1);
	iowrite32(rtcval, priv.rtc_base + APIVAL_OFFSET);
	iowrite32(0, priv.rtc_base + RTCVAL_OFFSET);

	rtc_enable(&priv);

	return ret;
}

static int rtc_resume(struct device *dev)
{
	struct rtc_priv *priv = dev_get_drvdata(dev);
	int ret;

	if (!device_may_wakeup(dev))
		return 0;

	/* Disable wake-up interrupts */
	enable_api_irq(dev, 0);

	/* Reinitialize the driver using the initial settings */
	ret = rtc_init(priv);
	if (ret)
		return ret;

	rtc_enable(priv);

	/*
	 * Now RTCCNT has just been reset, and is out of sync with priv->base;
	 * reapply the saved time settings
	 */
	return s32g_rtc_set_time(dev, &priv->base.tm);
}
#endif /* CONFIG_PM_SLEEP */

static const struct of_device_id rtc_dt_ids[] = {
	{.compatible = "nxp,s32g-rtc" },
	{ /* sentinel */ },
};

static SIMPLE_DEV_PM_OPS(rtc_pm_ops,
			 rtc_suspend, rtc_resume);

static struct platform_driver rtc_driver = {
	.driver		= {
		.name			= "s32g-rtc",
		.pm				= &rtc_pm_ops,
		.of_match_table = of_match_ptr(rtc_dt_ids),
	},
	.probe		= rtc_probe,
	.remove_new	= rtc_remove,
};
module_platform_driver(rtc_driver);

MODULE_AUTHOR("NXP");
MODULE_DESCRIPTION("NXP RTC driver for S32G2/S32G3");
MODULE_LICENSE("GPL");
