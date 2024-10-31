// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2024 NXP
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/of_irq.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/rtc.h>
#include <linux/iopoll.h>
#include <linux/math64.h>

#define RTCC_OFFSET	0x4ul
#define RTCS_OFFSET	0x8ul
#define RTCCNT_OFFSET	0xCul
#define APIVAL_OFFSET	0x10ul
#define RTCVAL_OFFSET	0x14ul

/* RTCC fields */
#define RTCC_CNTEN				BIT(31)
#define RTCC_RTCIE_SHIFT		30
#define RTCC_RTCIE				BIT(RTCC_RTCIE_SHIFT)
#define RTCC_ROVREN				BIT(28)
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
#define RTCS_ROVRF		BIT(10)

#define ROLLOVER_VAL		GENMASK(31, 0)
#define RTC_SYNCH_TIMEOUT	(100 * USEC_PER_MSEC)

#define RTC_CLK_MUX_SIZE	4

/*
 * S32G2 and S32G3 SoCs have RTC clock source 1 reserved and
 * should not be used.
 */
#define RTC_QUIRK_SRC1_RESERVED			BIT(2)

#define to_rtcpriv(_hw)	container_of(_hw, struct rtc_priv, clk)

enum {
	RTC_CLK_SRC0 = 0,
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

struct rtc_time_base {
	s64 sec;
	u64 cycles;
	u64 rollovers;
	struct rtc_time tm;
};

struct rtc_priv {
	struct rtc_device *rdev;
	u8 __iomem *rtc_base;
	struct clk_hw clk;
	struct clk *ipg;
	const struct rtc_soc_data *rtc_data;
	struct rtc_time_base base;
	u64 rtc_hz;
	u64 rollovers;
	int dt_irq_id;
	int runtime_src_idx;
	int suspend_src_idx;
	u32 runtime_div;
	u32 suspend_div;
};

struct rtc_soc_data {
	int default_runtime_src_idx;
	int default_suspend_src_idx;
	u32 default_runtime_div;
	u32 default_suspend_div;
	u32 quirks;
};

static const struct rtc_soc_data rtc_s32g2_data = {
	.default_runtime_src_idx = RTC_CLK_SRC2,
	.default_suspend_src_idx = RTC_CLK_MUX_SIZE + RTC_CLK_SRC0,
	.default_runtime_div = DIV512,
	.default_suspend_div = DIV512,
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
	if (!seconds || seconds > cycles_to_sec(priv->rtc_hz, ROLLOVER_VAL))
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

static s64 s32g_rtc_get_time_or_alrm(struct rtc_priv *priv,
				     u32 offset)
{
	u64 cycles, base_cycles;
	u32 counter;
	s64 sec;

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
	s64 sec;

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
	s64 sec;

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
	if (t_alrm <= t_crt) {
		dev_warn(dev, "Alarm is set in the past\n");
		return -EINVAL;
	}

	ret = sec_to_rtcval(priv, t_alrm - t_crt, &rtcval);
	if (ret) {
		/*
		 * Rollover support enables RTC alarm
		 * for a maximum timespan of ~3 months.
		 */
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

static int get_div_config(unsigned long req_rate,
			  unsigned long prate)
{
	if (req_rate == prate)
		return DIV1;
	else if (req_rate == prate / (DIV512 * DIV32))
		return DIV512_32;
	else if (req_rate == prate / DIV512)
		return DIV512;
	else if (req_rate == prate / DIV32)
		return DIV32;

	return 0;
}

static void adjust_dividers(struct rtc_priv *priv,
			    u32 div_val, u32 *reg)
{
	switch (div_val) {
	case DIV512_32:
		*reg |= RTCC_DIV512EN;
		*reg |= RTCC_DIV32EN;
		break;
	case DIV512:
		*reg |= RTCC_DIV512EN;
		break;
	case DIV32:
		*reg |= RTCC_DIV32EN;
		break;
	default:
		return;
	}

	priv->rtc_hz /= div_val;
}

static unsigned long get_prate_by_index(struct clk_hw *hw,
					u8 index)
{
	struct clk_hw *parent;

	parent = clk_hw_get_parent_by_index(hw, index);
	if (!parent)
		return -EINVAL;

	return clk_hw_get_rate(parent);
}

static int rtc_clk_determine_rate(struct clk_hw *hw,
				  struct clk_rate_request *req)
{
	struct rtc_priv *priv = to_rtcpriv(hw);
	struct device *dev = priv->rdev->dev.parent;
	int i, num_parents = clk_hw_get_num_parents(hw);
	u32 config;

	for (i = 0; i < num_parents; i++) {
		config = get_div_config(req->rate, get_prate_by_index(hw, i));
		if (config) {
			if (i < RTC_CLK_MUX_SIZE)
				/* Runtime clk source divisors */
				priv->runtime_div = config;
			else
				/* Suspend clk source divisors */
				priv->suspend_div = config;

			return 0;
		}
	}

	dev_err(dev, "Failed to determine RTC clock rate\n");
	return -EINVAL;
}

static u8 rtc_clk_get_parent(struct clk_hw *hw)
{
	struct rtc_priv *priv = to_rtcpriv(hw);

	return (ioread32(priv->rtc_base + RTCC_OFFSET) &
		RTCC_CLKSEL_MASK) >> RTCC_CLKSEL_OFFSET;
}

static int rtc_clk_src_switch(struct clk_hw *hw, u8 src)
{
	struct rtc_priv *priv = to_rtcpriv(hw);
	struct device *dev = priv->rdev->dev.parent;
	u32 rtcc = 0;

	switch (src % RTC_CLK_MUX_SIZE) {
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
		dev_err(dev, "Invalid clock mux parent: %d\n", src);
		return -EINVAL;
	}

	priv->rtc_hz = get_prate_by_index(hw, src);
	if (!priv->rtc_hz) {
		dev_err(dev, "Failed to get RTC frequency\n");
		return -EINVAL;
	}

	if (src < RTC_CLK_MUX_SIZE)
		adjust_dividers(priv, priv->runtime_div, &rtcc);
	else
		adjust_dividers(priv, priv->suspend_div, &rtcc);

	rtcc |= RTCC_RTCIE | RTCC_ROVREN;
	/*
	 * Make sure the CNTEN is 0 before we configure
	 * the clock source and dividers.
	 */
	s32g_rtc_disable(priv);
	iowrite32(rtcc, priv->rtc_base + RTCC_OFFSET);
	s32g_rtc_enable(priv);

	return 0;
}

static int rtc_clk_set_parent(struct clk_hw *hw, u8 index)
{
	struct rtc_priv *priv = to_rtcpriv(hw);

	/*
	 * 0-3 IDs are Runtime clk sources
	 * 4-7 IDs are Suspend clk sources
	 */
	if (index < RTC_CLK_MUX_SIZE) {
		/* Runtime clk source */
		priv->runtime_src_idx = index;
		return 0;
	} else if (index < RTC_CLK_MUX_SIZE * 2) {
		/* Suspend clk source */
		priv->suspend_src_idx = index;
		return 0;
	}

	return -EINVAL;
}

static const struct rtc_class_ops rtc_ops = {
	.read_time = s32g_rtc_read_time,
	.set_time = s32g_rtc_set_time,
	.read_alarm = s32g_rtc_read_alarm,
	.set_alarm = s32g_rtc_set_alarm,
	.alarm_irq_enable = s32g_rtc_alarm_irq_enable,
};

static const struct clk_ops rtc_clk_ops = {
	.determine_rate = rtc_clk_determine_rate,
	.get_parent = rtc_clk_get_parent,
	.set_parent = rtc_clk_set_parent,
};

static int priv_dts_init(struct rtc_priv *priv, struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);

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

	return 0;
}

static int rtc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtc_priv *priv;
	static const char *parents[RTC_CLK_MUX_SIZE * 2] = {
		"rtc_runtime_s0",
		"rtc_runtime_s1",
		"rtc_runtime_s2",
		"rtc_runtime_s3",
		"rtc_standby_s0",
		"rtc_standby_s1",
		"rtc_standby_s2",
		"rtc_standby_s3"
	};
	struct clk_init_data clk_init = {
		.name = "rtc_clk",
		.ops = &rtc_clk_ops,
		.flags = 0,
		.parent_names = parents,
		.num_parents = ARRAY_SIZE(parents),
	};
	int ret = 0;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->rtc_data = of_device_get_match_data(dev);
	if (!priv->rtc_data)
		return -ENODEV;

	priv->rtc_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->rtc_base))
		return dev_err_probe(dev, PTR_ERR(priv->rtc_base),
				"Failed to map registers\n");

	device_init_wakeup(dev, true);

	ret = priv_dts_init(priv, dev);
	if (ret)
		return ret;

	ret = clk_prepare_enable(priv->ipg);
	if (ret) {
		dev_err(dev, "Cannot enable 'ipg' clock, error: %d\n", ret);
		return ret;
	}

	priv->rdev = devm_rtc_allocate_device(dev);
	if (IS_ERR(priv->rdev)) {
		dev_err(dev, "Failed to allocate RTC device\n");
		ret = PTR_ERR(priv->rdev);
		goto disable_rtc;
	}

	if (!of_property_present(dev->of_node,
				 "assigned-clock-parents")) {
		/*
		 * If parent clocks are not specified via DT
		 * use SoC specific defaults for clock source mux
		 * and divisors.
		 */
		priv->runtime_src_idx = priv->rtc_data->default_runtime_src_idx;
		priv->suspend_src_idx = priv->rtc_data->default_suspend_src_idx;
		priv->runtime_div = priv->rtc_data->default_runtime_div;
		priv->suspend_div = priv->rtc_data->default_suspend_div;
	} else {
		priv->runtime_src_idx = -EINVAL;
		priv->suspend_src_idx = -EINVAL;
	}

	priv->clk.init = &clk_init;
	ret = devm_clk_hw_register(dev, &priv->clk);
	if (ret) {
		dev_err(dev, "Failed to register rtc_clk clk\n");
		goto disable_ipg_clk;
	}

	ret = of_clk_add_hw_provider(dev->of_node,
				     of_clk_hw_simple_get, priv->clk.clk);
	if (ret) {
		dev_err(dev, "Failed to add rtc_clk clk provider\n");
		goto disable_ipg_clk;
	}

	if (priv->runtime_src_idx < 0) {
		ret = priv->runtime_src_idx;
		dev_err(dev, "RTC runtime clock source is not specified\n");
		goto disable_ipg_clk;
	}

	ret = rtc_clk_src_switch(&priv->clk, priv->runtime_src_idx);
	if (ret) {
		dev_err(dev, "Failed clk source switch, err: %d\n", ret);
		goto disable_ipg_clk;
	}

	platform_set_drvdata(pdev, priv);
	priv->rdev->ops = &rtc_ops;

	ret = devm_rtc_register_device(priv->rdev);
	if (ret) {
		dev_err(dev, "Failed to register RTC device\n");
		goto disable_rtc;
	}

	ret = devm_request_irq(dev, priv->dt_irq_id,
			       rtc_handler, 0, dev_name(dev), pdev);
	if (ret) {
		dev_err(dev, "Request interrupt %d failed, error: %d\n",
			priv->dt_irq_id, ret);
		goto disable_rtc;
	}

	return 0;

disable_ipg_clk:
	clk_disable_unprepare(priv->ipg);
disable_rtc:
	s32g_rtc_disable(priv);
	return ret;
}

static void rtc_remove(struct platform_device *pdev)
{
	struct rtc_priv *priv = platform_get_drvdata(pdev);

	s32g_rtc_disable(priv);
}

static void  __maybe_unused enable_api_irq(struct device *dev, unsigned int enabled)
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

static int __maybe_unused rtc_suspend(struct device *dev)
{
	struct rtc_priv *init_priv = dev_get_drvdata(dev);
	struct rtc_priv priv;
	long long base_sec;
	int ret = 0;
	u32 rtcval;
	u32 sec;

	if (!device_may_wakeup(dev))
		return 0;

	if (init_priv->suspend_src_idx < 0)
		return 0;

	if (rtc_clk_get_parent(&init_priv->clk) == init_priv->suspend_src_idx)
		return 0;

	/* Save last known timestamp before we switch clocks and reinit RTC */
	ret = s32g_rtc_read_time(dev, &init_priv->base.tm);
	if (ret)
		return ret;

	/*
	 * Use a local copy of the RTC control block to
	 * avoid restoring it on resume path.
	 */
	memcpy(&priv, init_priv, sizeof(priv));

	ret = get_time_left(dev, init_priv, &sec);
	if (ret)
		return ret;

	/* Adjust for the number of seconds we'll be asleep */
	base_sec = rtc_tm_to_time64(&init_priv->base.tm);
	base_sec += sec;
	rtc_time64_to_tm(base_sec, &init_priv->base.tm);

	ret = rtc_clk_src_switch(&priv.clk, priv.suspend_src_idx);
	if (ret) {
		dev_err(dev, "Failed clk source switch, err: %d\n", ret);
		return ret;
	}

	ret = sec_to_rtcval(&priv, sec, &rtcval);
	if (ret) {
		dev_warn(dev, "Alarm is too far in the future\n");
		return ret;
	}

	s32g_rtc_alarm_irq_enable(dev, 0);
	enable_api_irq(dev, 1);
	iowrite32(rtcval, priv.rtc_base + APIVAL_OFFSET);
	iowrite32(0, priv.rtc_base + RTCVAL_OFFSET);

	return ret;
}

static int __maybe_unused rtc_resume(struct device *dev)
{
	struct rtc_priv *priv = dev_get_drvdata(dev);
	int ret;

	if (!device_may_wakeup(dev))
		return 0;

	if (rtc_clk_get_parent(&priv->clk) == priv->runtime_src_idx)
		return 0;

	/* Disable wake-up interrupts */
	enable_api_irq(dev, 0);

	ret = rtc_clk_src_switch(&priv->clk, priv->runtime_src_idx);
	if (ret) {
		dev_err(dev, "Failed clk source switch, err: %d\n", ret);
		return ret;
	}

	/*
	 * Now RTCCNT has just been reset, and is out of sync with priv->base;
	 * reapply the saved time settings
	 */
	return s32g_rtc_set_time(dev, &priv->base.tm);
}

static const struct of_device_id rtc_dt_ids[] = {
	{ .compatible = "nxp,s32g2-rtc", .data = &rtc_s32g2_data},
	{ /* sentinel */ },
};

static SIMPLE_DEV_PM_OPS(rtc_pm_ops,
			 rtc_suspend, rtc_resume);

static struct platform_driver rtc_driver = {
	.driver		= {
		.name			= "s32g-rtc",
		.pm				= &rtc_pm_ops,
		.of_match_table = rtc_dt_ids,
	},
	.probe		= rtc_probe,
	.remove	= rtc_remove,
};
module_platform_driver(rtc_driver);

MODULE_AUTHOR("NXP");
MODULE_DESCRIPTION("NXP RTC driver for S32G2/S32G3");
MODULE_LICENSE("GPL");
