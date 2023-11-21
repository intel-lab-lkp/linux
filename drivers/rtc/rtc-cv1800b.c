// SPDX-License-Identifier: GPL-2.0-only
/*
 * rtc-cv1800b.c: RTC driver for Sophgo CV1800B RTC
 *
 * Author: Jingbao Qiu <qiujingbao.dlmu@gmail.com>
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/rtc.h>
#include <linux/clk.h>
#include <linux/delay.h>

#define RTC_ANA_CALIB                 0x000
#define RTC_SEC_PULSE_GEN             0x004
#define RTC_ALARM_TIME                0x008
#define RTC_ALARM_ENABLE              0x00c
#define RTC_SET_SEC_CNTR_VALUE        0x010
#define RTC_SET_SEC_CNTR_TRIG         0x014
#define RTC_SEC_CNTR_VALUE            0x018
#define RTC_INFO0                     0x01c
#define RTC_INFO1                     0x020
#define RTC_INFO2                     0x024
#define RTC_INFO3                     0x028
#define RTC_NOPOR_INFO0               0x02c
#define RTC_NOPOR_INFO1               0x030
#define RTC_NOPOR_INFO2               0x034
#define RTC_NOPOR_INFO3               0x038
#define RTC_DB_PWR_VBAT_DET           0x040
#define RTC_DB_BUTTON1                0x048
#define RTC_DB_PWR_ON                 0x04c
#define RTC_7SEC_RESET                0x050
#define RTC_THM_SHDN_AUTO_REBOOT      0x064
#define RTC_POR_DB_MAGIC_KEY          0x068
#define RTC_DB_SEL_PWR                0x06c
#define RTC_UP_SEQ0                   0x070
#define RTC_UP_SEQ1                   0x074
#define RTC_UP_SEQ2                   0x078
#define RTC_UP_SEQ3                   0x07c
#define RTC_UP_IF_EN                  0x080
#define RTC_UP_RSTN                   0x084
#define RTC_UP_MAX                    0x088
#define RTC_DN_SEQ0                   0x090
#define RTC_DN_SEQ1                   0x094
#define RTC_DN_SEQ2                   0x098
#define RTC_DN_SEQ3                   0x09c
#define RTC_DN_IF_EN                  0x0a0
#define RTC_DN_RSTN                   0x0a4
#define RTC_DN_MAX                    0x0a8
#define RTC_PWR_CYC_MAX               0x0b0
#define RTC_WARM_RST_MAX              0x0b4
#define RTC_EN_7SEC_RST               0x0b8
#define RTC_EN_PWR_WAKEUP             0x0bc
#define RTC_EN_SHDN_REQ               0x0c0
#define RTC_EN_THM_SHDN               0x0c4
#define RTC_EN_PWR_CYC_REQ            0x0c8
#define RTC_EN_WARM_RST_REQ           0x0cc
#define RTC_EN_PWR_VBAT_DET           0x0d0
#define FSM_STATE                     0x0d4
#define RTC_EN_WDG_RST_REQ            0x0e0
#define RTC_EN_SUSPEND_REQ            0x0e4
#define RTC_DB_REQ_WDG_RST            0x0e8
#define RTC_DB_REQ_SUSPEND            0x0ec
#define RTC_PG_REG                    0x0f0
#define RTC_ST_ON_REASON              0x0f8
#define RTC_ST_OFF_REASON             0x0fc
#define RTC_EN_WAKEUP_REQ             0x120
#define RTC_PWR_WAKEUP_POLARITY       0x128
#define RTC_DB_SEL_REQ                0x130
#define RTC_PWR_DET_SEL               0x140

#define REG_DISABLE_FUN               0x00UL
#define REG_ENABLE_FUN                BIT(0)
#define ACTIVATE_RTC_POR_DB_MAGIC_KEY 0x5af0
#define INIT_LOAD_TIME                0xff

struct cv1800b_rtc_priv {
	struct rtc_device *rtc_dev;
	void __iomem *core_map;
	struct clk *clk;
	int irq;
};

static int cv1800b_rtc_alarm_irq_enable(struct device *dev,
					unsigned int enabled)
{
	struct cv1800b_rtc_priv *data = dev_get_drvdata(dev);

	if (enabled)
		writel_relaxed(REG_ENABLE_FUN, data->core_map + RTC_ALARM_ENABLE);
	else
		writel_relaxed(REG_DISABLE_FUN,
			       data->core_map + RTC_ALARM_ENABLE);

	return 0;
}

static int cv1800b_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct cv1800b_rtc_priv *data = dev_get_drvdata(dev);
	unsigned long time = rtc_tm_to_time64(&alrm->time);

	writel_relaxed(time, data->core_map + RTC_ALARM_TIME);

	cv1800b_rtc_alarm_irq_enable(dev, 1);

	return 0;
}

static int cv1800b_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	u32 alrm_time, now_time;
	struct cv1800b_rtc_priv *data = dev_get_drvdata(dev);

	alrm_time = readl(data->core_map + RTC_ALARM_TIME);
	now_time = readl(data->core_map + RTC_SEC_CNTR_VALUE);
	rtc_time64_to_tm(alrm_time, &alrm->time);
	alrm->pending = now_time > alrm_time ? 1 : 0;
	alrm->enabled = readl(data->core_map + RTC_ALARM_ENABLE);

	return 0;
}

static int cv1800b_rtc_softinit(struct cv1800b_rtc_priv *dev)
{
	u32 timeout = 20;

	writel(ACTIVATE_RTC_POR_DB_MAGIC_KEY,
	       dev->core_map + RTC_POR_DB_MAGIC_KEY);
	writel(INIT_LOAD_TIME, dev->core_map + RTC_SET_SEC_CNTR_VALUE);
	writel(REG_DISABLE_FUN, dev->core_map + RTC_SET_SEC_CNTR_TRIG);

	while (readl(dev->core_map + RTC_SEC_CNTR_VALUE) == INIT_LOAD_TIME
	       && timeout--)
		udelay(5);

	if (!timeout)
		return -1;
	return 0;
}

static int cv1800b_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct cv1800b_rtc_priv *data = dev_get_drvdata(dev);
	u32 time = 0;

	if (!data)
		return -1;

	time = readl_relaxed(data->core_map + RTC_SEC_CNTR_VALUE);
	rtc_time64_to_tm(time, tm);

	return 0;
}

static int cv1800b_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	unsigned long time = rtc_tm_to_time64(tm);
	struct cv1800b_rtc_priv *data = dev_get_drvdata(dev);

	if (!data)
		return -1;

	writel(time, data->core_map + RTC_SET_SEC_CNTR_VALUE);
	writel(REG_ENABLE_FUN, data->core_map + RTC_SET_SEC_CNTR_TRIG);

	return 0;
}

static irqreturn_t cv1800b_rtc_irq_handler(int irq, void *dev_id)
{
	struct device *dev = dev_id;
	struct cv1800b_rtc_priv *data = dev_get_drvdata(dev);

	if (!data)
		return -1;
	writel(REG_DISABLE_FUN, data->core_map + RTC_ALARM_ENABLE);

	return IRQ_HANDLED;
}

static const struct rtc_class_ops cv800b_rtc_ops = {
	.read_time = cv1800b_rtc_read_time,
	.set_time = cv1800b_rtc_set_time,
	.read_alarm = cv1800b_rtc_read_alarm,
	.set_alarm = cv1800b_rtc_set_alarm,
	.alarm_irq_enable = cv1800b_rtc_alarm_irq_enable,
};

static int cv1800b_rtc_probe(struct platform_device *pdev)
{
	struct cv1800b_rtc_priv *rtc;
	struct resource *res;
	int ret;

	rtc = devm_kzalloc(&pdev->dev, sizeof(*rtc), GFP_KERNEL);
	if (!rtc)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		ret = -ENODEV;
		goto err;
	}

	rtc->core_map = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(rtc->core_map)) {
		ret = PTR_ERR(rtc->core_map);
		goto err;
	}

	rtc->irq = platform_get_irq(pdev, 0);
	platform_set_drvdata(pdev, rtc);
	if (rtc->irq < 0) {
		ret = -EINVAL;
		goto err;
	}

	ret =
	    devm_request_irq(&pdev->dev, rtc->irq, cv1800b_rtc_irq_handler,
			     IRQF_SHARED, "rtc alarm", &pdev->dev);
	if (ret)
		goto err;

	rtc->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(rtc->clk)) {
		dev_err(&pdev->dev, "no clock");
		ret = PTR_ERR(rtc->clk);
		goto err;
	}
	ret = clk_prepare_enable(rtc->clk);
	if (ret)
		goto err;
	ret = cv1800b_rtc_softinit(rtc);
	if (ret)
		goto err;
	cv1800b_rtc_alarm_irq_enable(&pdev->dev, 1);
	rtc->rtc_dev = devm_rtc_allocate_device(&pdev->dev);
	if (IS_ERR(rtc->rtc_dev)) {
		ret = PTR_ERR(rtc->rtc_dev);
		goto err;
	}
	rtc->rtc_dev->range_max = U32_MAX;
	rtc->rtc_dev->ops = &cv800b_rtc_ops;

	return rtc_register_device(rtc->rtc_dev);
err:
	return dev_err_probe(&pdev->dev, ret, "Failed to init cv1800b rtc\n");
}

static int __maybe_unused cv1800b_rtc_suspend_noirq(struct device *dev)
{
	struct cv1800b_rtc_priv *data = dev_get_drvdata(dev);

	clk_disable_unprepare(data->clk);

	return 0;
}

static int __maybe_unused cv1800b_rtc_resume_noirq(struct device *dev)
{
	struct cv1800b_rtc_priv *data = dev_get_drvdata(dev);

	clk_prepare_enable(data->clk);

	return 0;
}

static const struct dev_pm_ops cv1800b_rtc_pm_ops = {
	SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(cv1800b_rtc_suspend_noirq,
				      cv1800b_rtc_resume_noirq)
};

static const struct of_device_id cv1800b_dt_ids[] = {
	{ .compatible = "sophgo,cv800b-rtc" },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, cv1800b_dt_ids);

static struct platform_driver cv1800b_rtc_driver = {
	.driver = {
		   .name = "sophgo-cv800b-rtc",
		   .pm = &cv1800b_rtc_pm_ops,
		   .of_match_table = cv1800b_dt_ids,
		    },
	.probe = cv1800b_rtc_probe,
};

module_platform_driver(cv1800b_rtc_driver);
MODULE_AUTHOR("Jingbao Qiu");
MODULE_DESCRIPTION("Sophgo CV1800B RTC Driver");
MODULE_LICENSE("GPL");
