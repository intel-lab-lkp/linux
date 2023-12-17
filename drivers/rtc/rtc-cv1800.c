// SPDX-License-Identifier: GPL-2.0-only
/*
 * rtc-cv1800.c: RTC driver for Sophgo cv1800 RTC
 *
 * Author: Jingbao Qiu <qiujingbao.dlmu@gmail.com>
 */
#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/rtc.h>
#include <linux/platform_device.h>
#include <linux/of.h>

#define ANA_CALIB                   0x0
#define SEC_PULSE_GEN               0x4
#define ALARM_TIME                  0x8
#define ALARM_ENABLE                0xC
#define SET_SEC_CNTR_VAL            0x10
#define SET_SEC_CNTR_TRIG           0x14
#define SEC_CNTR_VAL                0x18
#define APB_RDATA_SEL               0x3C
#define POR_DB_MAGIC_KEY            0x68
#define EN_PWR_WAKEUP               0xBC
#define MACRO_DA_CLEAR_ALL          0x480
#define MACRO_DA_SOC_READY          0x48C
#define MACRO_RO_T                  0x4A8
#define MACRO_RG_SET_T              0x498

#define CTRL                        0x08
#define FC_COARSE_EN                0x40
#define FC_COARSE_CAL               0x44
#define FC_FINE_EN                  0x48
#define FC_FINE_CAL                 0x50
#define CTRL_MODE_MASK              BIT(10)
#define CTRL_MODE_OSC32K            0x00UL
#define CTRL_MODE_XTAL32K           BIT(0)

#define FC_COARSE_CAL_VAL_SHIFT     0
#define FC_COARSE_CAL_VAL_MASK      GENMASK(15, 0)
#define FC_COARSE_CAL_TIME_SHIFT    16
#define FC_COARSE_CAL_TIME_MASK     GENMASK(31, 16)
#define FC_FINE_CAL_VAL_SHIFT       0
#define FC_FINE_CAL_VAL_MASK        GENMASK(23, 0)
#define FC_FINE_CAL_TIME_SHIFT      24
#define FC_FINE_CAL_TIME_MASK       GENMASK(31, 24)

#define SEC_PULSE_GEN_INT_SHIFT     0
#define SEC_PULSE_GEN_INT_MASK      GENMASK(7, 0)
#define SEC_PULSE_GEN_FRAC_SHIFT    8
#define SEC_PULSE_GEN_FRAC_MASK     GENMASK(24, 8)
#define SEC_PULSE_GEN_SEL_SHIFT     31
#define SEC_PULSE_GEN_SEL_MASK      GENMASK(30, 0)

#define CALIB_INIT_VAL              (BIT(8) || BIT(16))
#define CALIB_SEL_FTUNE_MASK        GENMASK(30, 0)
#define CALIB_OFFSET_INIT           128
#define CALIB_OFFSET_SHIFT          BIT(0)
#define CALIB_FREQ                  256000000000
#define CALIB_FRAC_EXT              10000
#define CALIB_FREQ_NS               40
#define CALIB_FREQ_MULT             256
#define CALIB_FC_COARSE_PLUS_OFFSET 770
#define CALIB_FC_COARSE_SUB_OFFSET  755

#define REG_ENABLE_FUN              BIT(0)
#define REG_DISABLE_FUN             0x00UL
#define REG_INIT_TIMEOUT            100
#define SEC_MAX_VAL                 0xFFFFFFFF
#define ALARM_ENABLE_MASK           BIT(0)
#define SET_SEC_CNTR_VAL_UPDATE     (BIT(28) || BIT(29))
#define DEALY_TIME_PREPARE          400
#define DEALY_TIME_LOOP             100

struct cv1800_priv {
	struct rtc_device *dev;
	void __iomem *base_data;
	void __iomem *base_ctrl;
	struct clk *clk;
	spinlock_t rtc_lock;
	int irq;
};

static int cv1800_rtc_alarm_irq_enable(struct device *dev, unsigned int enabled)
{
	struct cv1800_priv *info = dev_get_drvdata(dev);

	if (enabled)
		writel(REG_ENABLE_FUN, info->base_data + ALARM_ENABLE);
	else
		writel(REG_DISABLE_FUN, info->base_data + ALARM_ENABLE);

	return 0;
}

static int cv1800_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct cv1800_priv *info = dev_get_drvdata(dev);
	unsigned long alarm_time;

	alarm_time = rtc_tm_to_time64(&alrm->time);

	if (alarm_time > SEC_MAX_VAL)
		return -EINVAL;

	writel(REG_DISABLE_FUN, info->base_data + ALARM_ENABLE);

	udelay(DEALY_TIME_PREPARE);

	writel(alarm_time, info->base_data + ALARM_TIME);
	writel(REG_ENABLE_FUN, info->base_data + ALARM_ENABLE);

	readl(info->base_data + SEC_CNTR_VAL);

	return 0;
}

static int cv1800_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alarm)
{
	struct cv1800_priv *info = dev_get_drvdata(dev);

	alarm->enabled = readl(info->base_data + ALARM_ENABLE) &
			 ALARM_ENABLE_MASK;

	rtc_time64_to_tm(readl(info->base_data + ALARM_TIME), &alarm->time);

	return 0;
}

static int cv1800_rtc_32k_coarse_val_calib(struct cv1800_priv *info)
{
	uint32_t calib_val = 0;
	uint32_t coarse_val = 0;
	uint32_t coarse_time_now = 0;
	uint32_t coarse_time_next = 0;
	uint32_t offset = CALIB_OFFSET_INIT;
	uint32_t timeout = REG_INIT_TIMEOUT;
	uint32_t get_val_timeout;
	uint32_t sec_pulse_val;

	writel(CALIB_INIT_VAL, info->base_data + ANA_CALIB);
	udelay(DEALY_TIME_PREPARE);

	/* Select 32K OSC tuning val source from sys */
	sec_pulse_val = readl(info->base_data + SEC_PULSE_GEN) &
			SEC_PULSE_GEN_SEL_MASK;
	writel(sec_pulse_val, info->base_data + SEC_PULSE_GEN);

	calib_val = readl(info->base_data + ANA_CALIB);

	writel(REG_ENABLE_FUN, info->base_ctrl + FC_COARSE_EN);

	while (--timeout) {
		coarse_time_now = readl(info->base_ctrl + FC_COARSE_CAL) >>
				  FC_COARSE_CAL_TIME_SHIFT;

		get_val_timeout = REG_INIT_TIMEOUT;

		while (coarse_time_next <= coarse_time_now &&
		       --get_val_timeout) {
			coarse_time_next =
				readl(info->base_ctrl + FC_COARSE_CAL) >>
				FC_COARSE_CAL_TIME_SHIFT;
			udelay(DEALY_TIME_LOOP);
		}

		if (!get_val_timeout)
			return -1;

		udelay(DEALY_TIME_PREPARE);

		coarse_val = readl(info->base_ctrl + FC_COARSE_CAL) &
			     FC_COARSE_CAL_VAL_MASK;

		if (coarse_val > CALIB_FC_COARSE_PLUS_OFFSET) {
			calib_val += offset;
			offset >>= CALIB_OFFSET_SHIFT;
			writel(calib_val, info->base_data + ANA_CALIB);
		} else if (coarse_val < CALIB_FC_COARSE_SUB_OFFSET) {
			calib_val -= offset;
			offset >>= CALIB_OFFSET_SHIFT;
			writel(calib_val, info->base_data + ANA_CALIB);
		} else {
			writel(REG_DISABLE_FUN, info->base_ctrl + FC_COARSE_EN);
			break;
		}

		if (offset == 0)
			return -1;
	}

	return 0;
}

static int cv1800_rtc_32k_fine_val_calib(struct cv1800_priv *info)
{
	uint32_t fc_val;
	uint64_t freq = CALIB_FREQ;
	uint32_t sec_cnt;
	uint32_t timeout = REG_INIT_TIMEOUT;
	uint32_t fc_time_now = 0;
	uint32_t fc_time_next = 0;

	writel(REG_ENABLE_FUN, info->base_ctrl + FC_FINE_EN);

	fc_time_now = readl(info->base_ctrl + FC_FINE_CAL) >>
		      FC_FINE_CAL_TIME_SHIFT;

	while (fc_time_next <= fc_time_now && --timeout) {
		fc_time_next = readl(info->base_ctrl + FC_FINE_CAL) >>
			       FC_FINE_CAL_TIME_SHIFT;
		udelay(DEALY_TIME_LOOP);
	}

	if (!timeout)
		return -1;

	fc_val = readl(info->base_ctrl + FC_FINE_CAL) & FC_FINE_CAL_VAL_MASK;

	do_div(freq, CALIB_FREQ_NS);
	freq = freq * CALIB_FRAC_EXT;
	do_div(freq, fc_val);

	sec_cnt = ((do_div(freq, CALIB_FRAC_EXT) * CALIB_FREQ_MULT) /
			   CALIB_FRAC_EXT &
		   SEC_PULSE_GEN_INT_MASK) +
		  (freq << SEC_PULSE_GEN_FRAC_SHIFT);

	writel(sec_cnt, info->base_data + SEC_PULSE_GEN);
	writel(REG_DISABLE_FUN, info->base_ctrl + FC_FINE_EN);

	return 0;
}

static void rtc_enable_sec_counter(struct cv1800_priv *info)
{
	uint32_t val;

	/* select inner sec pulse and select reg set calibration val */
	val = readl(info->base_data + SEC_PULSE_GEN) & SEC_PULSE_GEN_SEL_MASK;
	writel(val, info->base_data + SEC_PULSE_GEN);

	val = readl(info->base_data + ANA_CALIB) & CALIB_SEL_FTUNE_MASK;
	writel(val, info->base_data + ANA_CALIB);

	readl(info->base_data + SEC_CNTR_VAL);
	writel(REG_DISABLE_FUN, info->base_data + ALARM_ENABLE);
}

static int cv1800_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct cv1800_priv *info = dev_get_drvdata(dev);
	unsigned long sec;
	unsigned long sec_ro_t;
	unsigned long flag;

	spin_lock_irqsave(&info->rtc_lock, flag);

	sec = readl(info->base_data + SEC_CNTR_VAL);
	sec_ro_t = readl(info->base_data + MACRO_RO_T);

	if (sec_ro_t > SET_SEC_CNTR_VAL_UPDATE) {
		sec = sec_ro_t;
		writel(sec, info->base_data + SET_SEC_CNTR_VAL);
		writel(REG_ENABLE_FUN, info->base_data + SET_SEC_CNTR_TRIG);
	}

	spin_unlock_irqrestore(&info->rtc_lock, flag);

	rtc_time64_to_tm(sec, tm);

	return 0;
}

static int cv1800_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct cv1800_priv *info = dev_get_drvdata(dev);
	unsigned long sec;
	int ret;
	unsigned long flag;

	ret = rtc_valid_tm(tm);
	if (ret)
		return ret;

	sec = rtc_tm_to_time64(tm);

	spin_lock_irqsave(&info->rtc_lock, flag);

	writel(sec, info->base_data + SET_SEC_CNTR_VAL);
	writel(REG_ENABLE_FUN, info->base_data + SET_SEC_CNTR_TRIG);

	writel(sec, info->base_data + MACRO_RG_SET_T);

	spin_unlock_irqrestore(&info->rtc_lock, flag);

	return 0;
}

static irqreturn_t cv1800_irq_handler(int irq, void *dev_id)
{
	struct device *dev = dev_id;
	struct cv1800_priv *info = dev_get_drvdata(dev);
	struct rtc_wkalrm alrm;

	writel(REG_DISABLE_FUN, info->base_data + ALARM_ENABLE);

	rtc_read_alarm(info->dev, &alrm);
	alrm.enabled = 0;
	rtc_set_alarm(info->dev, &alrm);

	return IRQ_HANDLED;
}

static const struct rtc_class_ops cv800b_ops = {
	.read_time = cv1800_rtc_read_time,
	.set_time = cv1800_rtc_set_time,
	.read_alarm = cv1800_rtc_read_alarm,
	.set_alarm = cv1800_rtc_set_alarm,
	.alarm_irq_enable = cv1800_rtc_alarm_irq_enable,
};

static int cv1800_rtc_probe(struct platform_device *pdev)
{
	struct cv1800_priv *rtc;
	int ret;

	rtc = devm_kzalloc(&pdev->dev, sizeof(struct cv1800_priv), GFP_KERNEL);
	if (!rtc)
		return -ENOMEM;

	rtc->base_ctrl = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rtc->base_ctrl))
		return PTR_ERR(rtc->base_ctrl);

	rtc->base_data = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(rtc->base_data))
		return PTR_ERR(rtc->base_data);

	rtc->irq = platform_get_irq(pdev, 0);
	if (rtc->irq < 0)
		return rtc->irq;

	ret = devm_request_irq(&pdev->dev, rtc->irq, cv1800_irq_handler,
			       IRQF_TRIGGER_HIGH, "alarm", &pdev->dev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "cannot register interrupt handler\n");

	rtc->clk = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(rtc->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(rtc->clk),
				     "clk not found\n");

	platform_set_drvdata(pdev, rtc);

	spin_lock_init(&rtc->rtc_lock);

	rtc->dev = devm_rtc_device_register(&pdev->dev, dev_name(&pdev->dev),
					    &cv800b_ops, THIS_MODULE);
	if (IS_ERR(rtc->dev))
		return dev_err_probe(&pdev->dev, PTR_ERR(rtc->dev),
				     "can't register rtc device\n");

	/* if use internal clk,so coarse calibrate rtc */
	if ((readl(rtc->base_ctrl + CTRL) & CTRL_MODE_MASK) ==
	    CTRL_MODE_OSC32K) {
		ret = cv1800_rtc_32k_coarse_val_calib(rtc);
		if (ret)
			dev_err(&pdev->dev, "failed to coarse RTC val !\n");

		ret = cv1800_rtc_32k_fine_val_calib(rtc);
		if (ret)
			dev_err(&pdev->dev, "failed to fine RTC val !\n");
	}

	rtc_enable_sec_counter(rtc);

	return 0;
}

static const struct of_device_id cv1800_dt_ids[] = {
	{ .compatible = "sophgo,cv1800-rtc" },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, cv1800_dt_ids);

static struct platform_driver cv1800_driver = {
	.driver = {
		.name = "cv1800-rtc",
		.of_match_table = cv1800_dt_ids,
	},
	.probe = cv1800_rtc_probe,
};

module_platform_driver(cv1800_driver);
MODULE_AUTHOR("Jingbao Qiu");
MODULE_DESCRIPTION("Sophgo CV1800 RTC Driver");
MODULE_LICENSE("GPL");
