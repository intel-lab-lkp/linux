// SPDX-License-Identifier: (GPL-2.0+ OR BSD-3-Clause)
/*
 * NXP NETC Timer driver
 * Copyright 2025 NXP
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/fsl/netc_global.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/ptp_clock_kernel.h>

#define NETC_TMR_PCI_VENDOR		0x1131
#define NETC_TMR_PCI_DEVID		0xee02

#define NETC_TMR_CTRL			0x0080
#define  TMR_CTRL_CK_SEL		GENMASK(1, 0)
#define  TMR_CTRL_TE			BIT(2)
#define  TMR_COMP_MODE			BIT(15)
#define  TMR_CTRL_TCLK_PERIOD		GENMASK(25, 16)
#define  TMR_CTRL_FS			BIT(28)
#define  TMR_ALARM1P			BIT(31)

#define NETC_TMR_TEVENT			0x0084
#define  TMR_TEVENT_ALM1EN		BIT(16)
#define  TMR_TEVENT_ALM2EN		BIT(17)

#define NETC_TMR_TEMASK			0x0088
#define NETC_TMR_CNT_L			0x0098
#define NETC_TMR_CNT_H			0x009c
#define NETC_TMR_ADD			0x00a0
#define NETC_TMR_PRSC			0x00a8
#define NETC_TMR_OFF_L			0x00b0
#define NETC_TMR_OFF_H			0x00b4

/* i = 0, 1, i indicates the index of TMR_ALARM */
#define NETC_TMR_ALARM_L(i)		(0x00b8 + (i) * 8)
#define NETC_TMR_ALARM_H(i)		(0x00bc + (i) * 8)

#define NETC_TMR_FIPER_CTRL		0x00dc
#define  FIPER_CTRL_DIS(i)		(BIT(7) << (i) * 8)
#define  FIPER_CTRL_PG(i)		(BIT(6) << (i) * 8)

#define NETC_TMR_CUR_TIME_L		0x00f0
#define NETC_TMR_CUR_TIME_H		0x00f4

#define NETC_TMR_REGS_BAR		0

#define NETC_TMR_FIPER_NUM		3
#define NETC_TMR_DEFAULT_PRSC		2
#define NETC_TMR_DEFAULT_ALARM		GENMASK_ULL(63, 0)

/* 1588 timer reference clock source select */
#define NETC_TMR_CCM_TIMER1		0 /* enet_timer1_clk_root, from CCM */
#define NETC_TMR_SYSTEM_CLK		1 /* enet_clk_root/2, from CCM */
#define NETC_TMR_EXT_OSC		2 /* tmr_1588_clk, from IO pins */

#define NETC_TMR_SYSCLK_333M		333333333U

struct netc_timer {
	void __iomem *base;
	struct pci_dev *pdev;
	spinlock_t lock; /* Prevent concurrent access to registers */

	struct clk *src_clk;
	struct ptp_clock *clock;
	struct ptp_clock_info caps;
	int phc_index;
	u32 clk_select;
	u32 clk_freq;
	u32 oclk_prsc;
	/* High 32-bit is integer part, low 32-bit is fractional part */
	u64 period;

	int irq;
};

#define netc_timer_rd(p, o)		netc_read((p)->base + (o))
#define netc_timer_wr(p, o, v)		netc_write((p)->base + (o), v)
#define ptp_to_netc_timer(ptp)		container_of((ptp), struct netc_timer, caps)

static u64 netc_timer_cnt_read(struct netc_timer *priv)
{
	u32 tmr_cnt_l, tmr_cnt_h;
	u64 ns;

	/* The user must read the TMR_CNC_L register first to get
	 * correct 64-bit TMR_CNT_H/L counter values.
	 */
	tmr_cnt_l = netc_timer_rd(priv, NETC_TMR_CNT_L);
	tmr_cnt_h = netc_timer_rd(priv, NETC_TMR_CNT_H);
	ns = (((u64)tmr_cnt_h) << 32) | tmr_cnt_l;

	return ns;
}

static void netc_timer_cnt_write(struct netc_timer *priv, u64 ns)
{
	u32 tmr_cnt_h = upper_32_bits(ns);
	u32 tmr_cnt_l = lower_32_bits(ns);

	/* The user must write to TMR_CNT_L register first. */
	netc_timer_wr(priv, NETC_TMR_CNT_L, tmr_cnt_l);
	netc_timer_wr(priv, NETC_TMR_CNT_H, tmr_cnt_h);
}

static u64 netc_timer_offset_read(struct netc_timer *priv)
{
	u32 tmr_off_l, tmr_off_h;
	u64 offset;

	tmr_off_l = netc_timer_rd(priv, NETC_TMR_OFF_L);
	tmr_off_h = netc_timer_rd(priv, NETC_TMR_OFF_H);
	offset = (((u64)tmr_off_h) << 32) | tmr_off_l;

	return offset;
}

static void netc_timer_offset_write(struct netc_timer *priv, u64 offset)
{
	u32 tmr_off_h = upper_32_bits(offset);
	u32 tmr_off_l = lower_32_bits(offset);

	netc_timer_wr(priv, NETC_TMR_OFF_L, tmr_off_l);
	netc_timer_wr(priv, NETC_TMR_OFF_H, tmr_off_h);
}

static u64 netc_timer_cur_time_read(struct netc_timer *priv)
{
	u32 time_h, time_l;
	u64 ns;

	/* The user should read NETC_TMR_CUR_TIME_L first to
	 * get correct current time.
	 */
	time_l = netc_timer_rd(priv, NETC_TMR_CUR_TIME_L);
	time_h = netc_timer_rd(priv, NETC_TMR_CUR_TIME_H);
	ns = (u64)time_h << 32 | time_l;

	return ns;
}

static void netc_timer_alarm_write(struct netc_timer *priv,
				   u64 alarm, int index)
{
	u32 alarm_h = upper_32_bits(alarm);
	u32 alarm_l = lower_32_bits(alarm);

	netc_timer_wr(priv, NETC_TMR_ALARM_L(index), alarm_l);
	netc_timer_wr(priv, NETC_TMR_ALARM_H(index), alarm_h);
}

static void netc_timer_adjust_period(struct netc_timer *priv, u64 period)
{
	u32 fractional_period = lower_32_bits(period);
	u32 integral_period = upper_32_bits(period);
	u32 tmr_ctrl, old_tmr_ctrl;
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);

	old_tmr_ctrl = netc_timer_rd(priv, NETC_TMR_CTRL);
	tmr_ctrl = u32_replace_bits(old_tmr_ctrl, integral_period,
				    TMR_CTRL_TCLK_PERIOD);
	if (tmr_ctrl != old_tmr_ctrl)
		netc_timer_wr(priv, NETC_TMR_CTRL, tmr_ctrl);

	netc_timer_wr(priv, NETC_TMR_ADD, fractional_period);

	spin_unlock_irqrestore(&priv->lock, flags);
}

static int netc_timer_adjfine(struct ptp_clock_info *ptp, long scaled_ppm)
{
	struct netc_timer *priv = ptp_to_netc_timer(ptp);
	u64 new_period;

	new_period = adjust_by_scaled_ppm(priv->period, scaled_ppm);
	netc_timer_adjust_period(priv, new_period);

	return 0;
}

static int netc_timer_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	struct netc_timer *priv = ptp_to_netc_timer(ptp);
	u64 tmr_cnt, tmr_off;
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);

	tmr_off = netc_timer_offset_read(priv);
	if (delta < 0 && tmr_off < abs(delta)) {
		delta += tmr_off;
		if (!tmr_off)
			netc_timer_offset_write(priv, 0);

		tmr_cnt = netc_timer_cnt_read(priv);
		tmr_cnt += delta;
		netc_timer_cnt_write(priv, tmr_cnt);
	} else {
		tmr_off += delta;
		netc_timer_offset_write(priv, tmr_off);
	}

	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int netc_timer_gettimex64(struct ptp_clock_info *ptp,
				 struct timespec64 *ts,
				 struct ptp_system_timestamp *sts)
{
	struct netc_timer *priv = ptp_to_netc_timer(ptp);
	unsigned long flags;
	u64 ns;

	spin_lock_irqsave(&priv->lock, flags);

	ptp_read_system_prets(sts);
	ns = netc_timer_cur_time_read(priv);
	ptp_read_system_postts(sts);

	spin_unlock_irqrestore(&priv->lock, flags);

	*ts = ns_to_timespec64(ns);

	return 0;
}

static int netc_timer_settime64(struct ptp_clock_info *ptp,
				const struct timespec64 *ts)
{
	struct netc_timer *priv = ptp_to_netc_timer(ptp);
	u64 ns = timespec64_to_ns(ts);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	netc_timer_offset_write(priv, 0);
	netc_timer_cnt_write(priv, ns);
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

int netc_timer_get_phc_index(struct pci_dev *timer_pdev)
{
	struct netc_timer *priv;

	if (!timer_pdev)
		return -ENODEV;

	priv = pci_get_drvdata(timer_pdev);
	if (!priv)
		return -EINVAL;

	return priv->phc_index;
}
EXPORT_SYMBOL_GPL(netc_timer_get_phc_index);

static const struct ptp_clock_info netc_timer_ptp_caps = {
	.owner		= THIS_MODULE,
	.name		= "NETC Timer PTP clock",
	.max_adj	= 500000000,
	.n_alarm	= 2,
	.n_pins		= 0,
	.adjfine	= netc_timer_adjfine,
	.adjtime	= netc_timer_adjtime,
	.gettimex64	= netc_timer_gettimex64,
	.settime64	= netc_timer_settime64,
};

static void netc_timer_init(struct netc_timer *priv)
{
	u32 tmr_emask = TMR_TEVENT_ALM1EN | TMR_TEVENT_ALM2EN;
	u32 fractional_period = lower_32_bits(priv->period);
	u32 integral_period = upper_32_bits(priv->period);
	u32 tmr_ctrl, fiper_ctrl;
	struct timespec64 now;
	u64 ns;
	int i;

	/* Software must enable timer first and the clock selected must be
	 * active, otherwise, the registers which are in the timer clock
	 * domain are not accessible.
	 */
	tmr_ctrl = (priv->clk_select & TMR_CTRL_CK_SEL) | TMR_CTRL_TE;
	netc_timer_wr(priv, NETC_TMR_CTRL, tmr_ctrl);
	netc_timer_wr(priv, NETC_TMR_PRSC, priv->oclk_prsc);

	/* Disable FIPER by default */
	fiper_ctrl = netc_timer_rd(priv, NETC_TMR_FIPER_CTRL);
	for (i = 0; i < NETC_TMR_FIPER_NUM; i++) {
		fiper_ctrl |= FIPER_CTRL_DIS(i);
		fiper_ctrl &= ~FIPER_CTRL_PG(i);
	}
	netc_timer_wr(priv, NETC_TMR_FIPER_CTRL, fiper_ctrl);

	ktime_get_real_ts64(&now);
	ns = timespec64_to_ns(&now);
	netc_timer_cnt_write(priv, ns);

	/* Allow atomic writes to TCLK_PERIOD and TMR_ADD, An update to
	 * TCLK_PERIOD does not take effect until TMR_ADD is written.
	 */
	tmr_ctrl |= ((integral_period << 16) & TMR_CTRL_TCLK_PERIOD) |
		     TMR_COMP_MODE | TMR_CTRL_FS;
	netc_timer_wr(priv, NETC_TMR_CTRL, tmr_ctrl);
	netc_timer_wr(priv, NETC_TMR_ADD, fractional_period);
	netc_timer_wr(priv, NETC_TMR_TEMASK, tmr_emask);
}

static int netc_timer_pci_probe(struct pci_dev *pdev)
{
	struct device *dev = &pdev->dev;
	struct netc_timer *priv;
	int err, len;

	pcie_flr(pdev);
	err = pci_enable_device_mem(pdev);
	if (err)
		return dev_err_probe(dev, err, "Failed to enable device\n");

	err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (err) {
		dev_err(dev, "dma_set_mask_and_coherent() failed, err:%pe\n",
			ERR_PTR(err));
		goto disable_dev;
	}

	err = pci_request_mem_regions(pdev, KBUILD_MODNAME);
	if (err) {
		dev_err(dev, "pci_request_regions() failed, err:%pe\n",
			ERR_PTR(err));
		goto disable_dev;
	}

	pci_set_master(pdev);
	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		err = -ENOMEM;
		goto release_mem_regions;
	}

	priv->pdev = pdev;
	len = pci_resource_len(pdev, NETC_TMR_REGS_BAR);
	priv->base = ioremap(pci_resource_start(pdev, NETC_TMR_REGS_BAR), len);
	if (!priv->base) {
		err = -ENXIO;
		dev_err(dev, "ioremap() failed\n");
		goto free_priv;
	}

	pci_set_drvdata(pdev, priv);

	return 0;

free_priv:
	kfree(priv);
release_mem_regions:
	pci_release_mem_regions(pdev);
disable_dev:
	pci_disable_device(pdev);

	return err;
}

static void netc_timer_pci_remove(struct pci_dev *pdev)
{
	struct netc_timer *priv = pci_get_drvdata(pdev);

	iounmap(priv->base);
	kfree(priv);
	pci_release_mem_regions(pdev);
	pci_disable_device(pdev);
}

static int netc_timer_get_reference_clk_source(struct netc_timer *priv)
{
	struct device *dev = &priv->pdev->dev;
	struct device_node *np = dev->of_node;
	const char *clk_name = NULL;
	u64 ns = NSEC_PER_SEC;

	/* Select NETC system clock as the reference clock by default */
	priv->clk_select = NETC_TMR_SYSTEM_CLK;
	priv->clk_freq = NETC_TMR_SYSCLK_333M;
	priv->period = div_u64(ns << 32, priv->clk_freq);

	if (!np)
		return 0;

	of_property_read_string(np, "clock-names", &clk_name);
	if (!clk_name)
		return 0;

	/* Update the clock source of the reference clock if the clock
	 * name is specified in DTS node.
	 */
	if (!strcmp(clk_name, "system"))
		priv->clk_select = NETC_TMR_SYSTEM_CLK;
	else if (!strcmp(clk_name, "ccm_timer"))
		priv->clk_select = NETC_TMR_CCM_TIMER1;
	else if (!strcmp(clk_name, "ext_1588"))
		priv->clk_select = NETC_TMR_EXT_OSC;
	else
		return -EINVAL;

	priv->src_clk = devm_clk_get(dev, clk_name);
	if (IS_ERR(priv->src_clk)) {
		dev_err(dev, "Failed to get reference clock source\n");
		return PTR_ERR(priv->src_clk);
	}

	priv->clk_freq = clk_get_rate(priv->src_clk);
	priv->period = div_u64(ns << 32, priv->clk_freq);

	return 0;
}

static int netc_timer_parse_dt(struct netc_timer *priv)
{
	return netc_timer_get_reference_clk_source(priv);
}

static irqreturn_t netc_timer_isr(int irq, void *data)
{
	struct netc_timer *priv = data;
	u32 tmr_event, tmr_emask;
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);

	tmr_event = netc_timer_rd(priv, NETC_TMR_TEVENT);
	tmr_emask = netc_timer_rd(priv, NETC_TMR_TEMASK);

	tmr_event &= tmr_emask;
	if (tmr_event & TMR_TEVENT_ALM1EN)
		netc_timer_alarm_write(priv, NETC_TMR_DEFAULT_ALARM, 0);

	if (tmr_event & TMR_TEVENT_ALM2EN)
		netc_timer_alarm_write(priv, NETC_TMR_DEFAULT_ALARM, 1);

	/* Clear interrupts status */
	netc_timer_wr(priv, NETC_TMR_TEVENT, tmr_event);

	spin_unlock_irqrestore(&priv->lock, flags);

	return IRQ_HANDLED;
}

static int netc_timer_init_msix_irq(struct netc_timer *priv)
{
	struct pci_dev *pdev = priv->pdev;
	char irq_name[64];
	int err, n;

	n = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSIX);
	if (n != 1) {
		err = (n < 0) ? n : -EPERM;
		dev_err(&pdev->dev, "pci_alloc_irq_vectors() failed\n");
		return err;
	}

	priv->irq = pci_irq_vector(pdev, 0);
	snprintf(irq_name, sizeof(irq_name), "ptp-netc %s", pci_name(pdev));
	err = request_irq(priv->irq, netc_timer_isr, 0, irq_name, priv);
	if (err) {
		dev_err(&pdev->dev, "request_irq() failed\n");
		pci_free_irq_vectors(pdev);
		return err;
	}

	return 0;
}

static void netc_timer_free_msix_irq(struct netc_timer *priv)
{
	struct pci_dev *pdev = priv->pdev;

	disable_irq(priv->irq);
	free_irq(priv->irq, priv);
	pci_free_irq_vectors(pdev);
}

static int netc_timer_probe(struct pci_dev *pdev,
			    const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;
	struct netc_timer *priv;
	int err;

	err = netc_timer_pci_probe(pdev);
	if (err)
		return err;

	priv = pci_get_drvdata(pdev);
	err = netc_timer_parse_dt(priv);
	if (err) {
		dev_err(dev, "Failed to parse DT node\n");
		goto timer_pci_remove;
	}

	priv->caps = netc_timer_ptp_caps;
	priv->oclk_prsc = NETC_TMR_DEFAULT_PRSC;
	priv->phc_index = -1; /* initialize it as an invalid index */
	spin_lock_init(&priv->lock);

	err = clk_prepare_enable(priv->src_clk);
	if (err) {
		dev_err(dev, "Failed to enable timer source clock\n");
		goto timer_pci_remove;
	}

	err = netc_timer_init_msix_irq(priv);
	if (err)
		goto disable_clk;

	netc_timer_init(priv);
	priv->clock = ptp_clock_register(&priv->caps, dev);
	if (IS_ERR(priv->clock)) {
		err = PTR_ERR(priv->clock);
		goto free_msix_irq;
	}

	priv->phc_index = ptp_clock_index(priv->clock);

	return 0;

free_msix_irq:
	netc_timer_free_msix_irq(priv);
disable_clk:
	clk_disable_unprepare(priv->src_clk);
timer_pci_remove:
	netc_timer_pci_remove(pdev);

	return err;
}

static void netc_timer_remove(struct pci_dev *pdev)
{
	struct netc_timer *priv = pci_get_drvdata(pdev);

	ptp_clock_unregister(priv->clock);
	netc_timer_free_msix_irq(priv);
	clk_disable_unprepare(priv->src_clk);
	netc_timer_pci_remove(pdev);
}

static const struct pci_device_id netc_timer_id_table[] = {
	{ PCI_DEVICE(NETC_TMR_PCI_VENDOR, NETC_TMR_PCI_DEVID) },
	{ 0, } /* End of table. */
};
MODULE_DEVICE_TABLE(pci, netc_timer_id_table);

static struct pci_driver netc_timer_driver = {
	.name = KBUILD_MODNAME,
	.id_table = netc_timer_id_table,
	.probe = netc_timer_probe,
	.remove = netc_timer_remove,
};
module_pci_driver(netc_timer_driver);

MODULE_DESCRIPTION("NXP NETC Timer PTP Driver");
MODULE_LICENSE("Dual BSD/GPL");
