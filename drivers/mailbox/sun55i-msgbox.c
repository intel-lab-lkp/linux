// SPDX-License-Identifier: GPL-2.0
/*
 * Allwinner sun55i/sun60i 4-Port Hardware Message Box Driver
 *
 * Copyright (C) 2026 Tim Michals <tcmichals@gmail.com>
 * Based on vendor sunxi-msgbox driver by Allwinner Technology Co., Ltd.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mailbox_controller.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/spinlock.h>

#define SUN55I_MAX_PROCESSORS		4
#define SUN55I_CHANS_PER_PROC		4
#define SUN55I_NUM_CHANS		((SUN55I_MAX_PROCESSORS - 1) * SUN55I_CHANS_PER_PROC)
#define SUN55I_FIFO_MAX			8

#define SUNXI_MSGBOX_OFFSET(n)			(0x100 * (n))
#define SUNXI_MSGBOX_READ_IRQ_ENABLE(n)		(0x020 + SUNXI_MSGBOX_OFFSET(n))
#define SUNXI_MSGBOX_READ_IRQ_STATUS(n)		(0x024 + SUNXI_MSGBOX_OFFSET(n))
#define SUNXI_MSGBOX_WRITE_IRQ_ENABLE(n)	(0x030 + SUNXI_MSGBOX_OFFSET(n))
#define SUNXI_MSGBOX_WRITE_IRQ_STATUS(n)	(0x034 + SUNXI_MSGBOX_OFFSET(n))
#define SUNXI_MSGBOX_FIFO_STATUS(n, p)		(0x050 + SUNXI_MSGBOX_OFFSET(n) + 0x4 * (p))
#define SUNXI_MSGBOX_MSG_STATUS(n, p)		(0x060 + SUNXI_MSGBOX_OFFSET(n) + 0x4 * (p))
#define SUNXI_MSGBOX_MSG_FIFO(n, p)		(0x070 + SUNXI_MSGBOX_OFFSET(n) + 0x4 * (p))

#define RD_IRQ_EN_BIT(p)			BIT((p) * 2)
#define RD_IRQ_PEND_BIT(p)			BIT((p) * 2)
#define MSG_NUM_MASK				GENMASK(3, 0)

struct sun55i_route {
	u8 remote_id;
	u8 remote_n;
};

/*
 * Hardware routing table for Cortex-A55 host (local_id = 0):
 *   local_n = 0 -> CPUS (remote_id = 2, remote_n = 0) -> Channels 0..3
 *   local_n = 1 -> DSP  (remote_id = 1, remote_n = 0) -> Channels 4..7
 *   local_n = 2 -> RV   (remote_id = 3, remote_n = 2) -> Channels 8..11
 */
static const struct sun55i_route arm_routes[3] = {
	[0] = { .remote_id = 2, .remote_n = 0 },
	[1] = { .remote_id = 1, .remote_n = 0 },
	[2] = { .remote_id = 3, .remote_n = 2 },
};

struct sun55i_msgbox {
	struct mbox_controller controller;
	void __iomem *regs[SUN55I_MAX_PROCESSORS];
	struct clk *clk;
	struct reset_control *reset;
	/* Protects concurrent MMIO register access */
	spinlock_t lock;
};

static inline struct sun55i_msgbox *to_sun55i_msgbox(struct mbox_chan *chan)
{
	return chan->con_priv;
}

static inline void sun55i_chan_to_route(int chan_idx, int *local_n, int *p,
					int *remote_id, int *remote_n)
{
	*local_n = chan_idx / SUN55I_CHANS_PER_PROC;
	*p = chan_idx % SUN55I_CHANS_PER_PROC;
	*remote_id = arm_routes[*local_n].remote_id;
	*remote_n = arm_routes[*local_n].remote_n;
}

static irqreturn_t sun55i_msgbox_irq(int irq, void *dev_id)
{
	struct sun55i_msgbox *mbox = dev_id;
	irqreturn_t ret = IRQ_NONE;
	int local_n, p, chan_idx;

	for (local_n = 0; local_n < 3; local_n++) {
		void __iomem *local_base = mbox->regs[0];
		u32 en, stat, pending;

		en = readl(local_base + SUNXI_MSGBOX_READ_IRQ_ENABLE(local_n));
		stat = readl(local_base + SUNXI_MSGBOX_READ_IRQ_STATUS(local_n));
		pending = en & stat;

		if (!pending)
			continue;

		for (p = 0; p < SUN55I_CHANS_PER_PROC; p++) {
			if (!(pending & RD_IRQ_PEND_BIT(p)))
				continue;

			chan_idx = local_n * SUN55I_CHANS_PER_PROC + p;
			while (readl(local_base +
				     SUNXI_MSGBOX_MSG_STATUS(local_n, p)) & MSG_NUM_MASK) {
				u32 msg = readl(local_base + SUNXI_MSGBOX_MSG_FIFO(local_n, p));

				mbox_chan_received_data(&mbox->controller.chans[chan_idx], &msg);
			}

			writel(RD_IRQ_PEND_BIT(p),
			       local_base + SUNXI_MSGBOX_READ_IRQ_STATUS(local_n));
			ret = IRQ_HANDLED;
		}
	}

	return ret;
}

static int sun55i_msgbox_send_data(struct mbox_chan *chan, void *data)
{
	struct sun55i_msgbox *mbox = to_sun55i_msgbox(chan);
	int n = chan - mbox->controller.chans;
	int local_n, p, remote_id, remote_n;
	u32 msg = data ? *(u32 *)data : 0;

	sun55i_chan_to_route(n, &local_n, &p, &remote_id, &remote_n);

	writel(msg, mbox->regs[remote_id] + SUNXI_MSGBOX_MSG_FIFO(remote_n, p));
	return 0;
}

static int sun55i_msgbox_startup(struct mbox_chan *chan)
{
	struct sun55i_msgbox *mbox = to_sun55i_msgbox(chan);
	int n = chan - mbox->controller.chans;
	int local_n, p, remote_id, remote_n;
	unsigned long flags;
	u32 val;

	sun55i_chan_to_route(n, &local_n, &p, &remote_id, &remote_n);

	/* Flush any stale receive data */
	while (readl(mbox->regs[0] + SUNXI_MSGBOX_MSG_STATUS(local_n, p)) & MSG_NUM_MASK)
		readl(mbox->regs[0] + SUNXI_MSGBOX_MSG_FIFO(local_n, p));

	/* Clear pending status */
	writel(RD_IRQ_PEND_BIT(p),
	       mbox->regs[0] + SUNXI_MSGBOX_READ_IRQ_STATUS(local_n));

	/* Enable receive IRQ */
	spin_lock_irqsave(&mbox->lock, flags);
	val = readl(mbox->regs[0] + SUNXI_MSGBOX_READ_IRQ_ENABLE(local_n));
	val |= RD_IRQ_EN_BIT(p);
	writel(val, mbox->regs[0] + SUNXI_MSGBOX_READ_IRQ_ENABLE(local_n));
	spin_unlock_irqrestore(&mbox->lock, flags);

	return 0;
}

static void sun55i_msgbox_shutdown(struct mbox_chan *chan)
{
	struct sun55i_msgbox *mbox = to_sun55i_msgbox(chan);
	int n = chan - mbox->controller.chans;
	int local_n, p, remote_id, remote_n;
	unsigned long flags;
	u32 val;

	sun55i_chan_to_route(n, &local_n, &p, &remote_id, &remote_n);

	/* Disable receive IRQ */
	spin_lock_irqsave(&mbox->lock, flags);
	val = readl(mbox->regs[0] + SUNXI_MSGBOX_READ_IRQ_ENABLE(local_n));
	val &= ~RD_IRQ_EN_BIT(p);
	writel(val, mbox->regs[0] + SUNXI_MSGBOX_READ_IRQ_ENABLE(local_n));
	spin_unlock_irqrestore(&mbox->lock, flags);

	/* Clear pending status and flush */
	writel(RD_IRQ_PEND_BIT(p),
	       mbox->regs[0] + SUNXI_MSGBOX_READ_IRQ_STATUS(local_n));
	while (readl(mbox->regs[0] + SUNXI_MSGBOX_MSG_STATUS(local_n, p)) & MSG_NUM_MASK)
		readl(mbox->regs[0] + SUNXI_MSGBOX_MSG_FIFO(local_n, p));
}

static bool sun55i_msgbox_last_tx_done(struct mbox_chan *chan)
{
	struct sun55i_msgbox *mbox = to_sun55i_msgbox(chan);
	int n = chan - mbox->controller.chans;
	int local_n, p, remote_id, remote_n;
	u32 count;

	sun55i_chan_to_route(n, &local_n, &p, &remote_id, &remote_n);

	count = readl(mbox->regs[remote_id] + SUNXI_MSGBOX_MSG_STATUS(remote_n, p)) & MSG_NUM_MASK;
	return count < SUN55I_FIFO_MAX;
}

static bool sun55i_msgbox_peek_data(struct mbox_chan *chan)
{
	struct sun55i_msgbox *mbox = to_sun55i_msgbox(chan);
	int n = chan - mbox->controller.chans;
	int local_n, p, remote_id, remote_n;
	u32 count;

	sun55i_chan_to_route(n, &local_n, &p, &remote_id, &remote_n);

	count = readl(mbox->regs[0] + SUNXI_MSGBOX_MSG_STATUS(local_n, p)) & MSG_NUM_MASK;
	return count > 0;
}

static const struct mbox_chan_ops sun55i_msgbox_chan_ops = {
	.send_data    = sun55i_msgbox_send_data,
	.startup      = sun55i_msgbox_startup,
	.shutdown     = sun55i_msgbox_shutdown,
	.last_tx_done = sun55i_msgbox_last_tx_done,
	.peek_data    = sun55i_msgbox_peek_data,
};

static int sun55i_msgbox_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mbox_chan *chans;
	struct sun55i_msgbox *mbox;
	int i, ret, irq_cnt, local_n;

	mbox = devm_kzalloc(dev, sizeof(*mbox), GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;

	spin_lock_init(&mbox->lock);

	for (i = 0; i < SUN55I_MAX_PROCESSORS; i++) {
		mbox->regs[i] = devm_platform_ioremap_resource(pdev, i);
		if (IS_ERR(mbox->regs[i]))
			return dev_err_probe(dev, PTR_ERR(mbox->regs[i]),
					     "failed to map resource %d\n", i);
	}

	mbox->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(mbox->clk))
		return dev_err_probe(dev, PTR_ERR(mbox->clk), "failed to get clock\n");

	ret = clk_prepare_enable(mbox->clk);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable clock\n");

	mbox->reset = devm_reset_control_get_optional_shared(dev, NULL);
	if (IS_ERR(mbox->reset)) {
		ret = PTR_ERR(mbox->reset);
		goto err_disable_clk;
	}

	ret = reset_control_deassert(mbox->reset);
	if (ret)
		goto err_disable_clk;

	/* Disable all read IRQs and clear status */
	for (local_n = 0; local_n < 3; local_n++) {
		writel(0, mbox->regs[0] + SUNXI_MSGBOX_READ_IRQ_ENABLE(local_n));
		writel(0xffffffff, mbox->regs[0] + SUNXI_MSGBOX_READ_IRQ_STATUS(local_n));
	}

	irq_cnt = platform_irq_count(pdev);
	if (irq_cnt < 0) {
		ret = irq_cnt;
		goto err_disable_clk;
	}

	for (i = 0; i < irq_cnt; i++) {
		int irq = platform_get_irq(pdev, i);

		if (irq > 0) {
			ret = devm_request_irq(dev, irq, sun55i_msgbox_irq,
					       IRQF_SHARED, dev_name(dev), mbox);
			if (ret)
				dev_warn(dev, "failed to request irq %d: %d\n", irq, ret);
		}
	}

	chans = devm_kcalloc(dev, SUN55I_NUM_CHANS, sizeof(*chans), GFP_KERNEL);
	if (!chans) {
		ret = -ENOMEM;
		goto err_disable_clk;
	}

	for (i = 0; i < SUN55I_NUM_CHANS; i++)
		chans[i].con_priv = mbox;

	mbox->controller.dev           = dev;
	mbox->controller.ops           = &sun55i_msgbox_chan_ops;
	mbox->controller.chans         = chans;
	mbox->controller.num_chans     = SUN55I_NUM_CHANS;
	mbox->controller.txdone_irq    = false;
	mbox->controller.txdone_poll   = true;
	mbox->controller.txpoll_period = 1;

	platform_set_drvdata(pdev, mbox);

	ret = mbox_controller_register(&mbox->controller);
	if (ret) {
		dev_err_probe(dev, ret, "failed to register controller\n");
		goto err_disable_clk;
	}

	return 0;

err_disable_clk:
	clk_disable_unprepare(mbox->clk);
	return ret;
}

static void sun55i_msgbox_remove(struct platform_device *pdev)
{
	struct sun55i_msgbox *mbox = platform_get_drvdata(pdev);

	mbox_controller_unregister(&mbox->controller);
	clk_disable_unprepare(mbox->clk);
}

static const struct of_device_id sun55i_msgbox_of_match[] = {
	{ .compatible = "allwinner,sun55i-a523-msgbox" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sun55i_msgbox_of_match);

static struct platform_driver sun55i_msgbox_driver = {
	.driver = {
		.name = "sun55i-msgbox",
		.of_match_table = sun55i_msgbox_of_match,
	},
	.probe = sun55i_msgbox_probe,
	.remove = sun55i_msgbox_remove,
};
module_platform_driver(sun55i_msgbox_driver);

MODULE_AUTHOR("Tim Michals <tcmichals@gmail.com>");
MODULE_DESCRIPTION("Allwinner sun55i/sun60i 4-Port Message Box Driver");
MODULE_LICENSE("GPL");
