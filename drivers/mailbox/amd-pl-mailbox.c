// SPDX-License-Identifier: GPL-2.0
/*
 * AMD Programmable Logic Mailbox soft IP controller driver
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mailbox/zynqmp-ipi-message.h>
#include <linux/mailbox_controller.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#define AMD_PL_MBOX_WRDATA		0x00
#define AMD_PL_MBOX_RDDATA		0x08
#define AMD_PL_MBOX_STATUS		0x10
#define AMD_PL_MBOX_ERROR		0x14
#define AMD_PL_MBOX_SEND_THRESHOLD	0x18
#define AMD_PL_MBOX_RECV_THRESHOLD	0x1c
#define AMD_PL_MBOX_IRQ_STATUS		0x20
#define AMD_PL_MBOX_IRQ_ENABLE		0x24
#define AMD_PL_MBOX_CTRL		0x2c

#define AMD_PL_MBOX_STS_RX_EMPTY	BIT(0)
#define AMD_PL_MBOX_STS_TX_FULL		BIT(1)
#define AMD_PL_MBOX_STS_TX_THRESHOLD	BIT(2)
#define AMD_PL_MBOX_IRQ_RX_THRESHOLD	BIT(1)
#define AMD_PL_MBOX_IRQ_ERROR		BIT(2)
#define AMD_PL_MBOX_CTRL_TX_RESET	BIT(0)
#define AMD_PL_MBOX_CTRL_RX_RESET	BIT(1)

/* FIFO depth range of the IP, in 32-bit words. */
#define AMD_PL_MBOX_FIFO_LEN_MIN	16
#define AMD_PL_MBOX_FIFO_LEN_MAX	8192

/*
 * The send threshold is active while send FIFO occupancy is less than or
 * equal to the programmed value. The receive threshold is active while
 * receive FIFO occupancy is greater than the programmed value. The related
 * interrupt status bits latch when a threshold condition becomes active,
 * and a new event takes priority over a simultaneous status clear.
 *
 * A send threshold of zero makes the status bit indicate an empty TX FIFO.
 * A receive threshold of zero raises an interrupt for the first received
 * word after the RX FIFO becomes empty.
 */

enum {
	AMD_PL_MBOX_CHAN_TX = 0,
	AMD_PL_MBOX_CHAN_RX = 1,
	AMD_PL_MBOX_NUM_CHANS = 2,
};

/**
 * struct amd_pl_mbox - AMD PL Mailbox controller state
 * @regs: mapped registers
 * @clk: interface clock, enabled while a channel is started
 * @irq: receive interrupt
 * @fifo_len: FIFO depth in 32-bit words
 * @tx_lock: serializes TX register access against TX shutdown
 * @tx_active: TX channel is started and its registers may be accessed
 * @controller: common mailbox controller
 * @chans: transmit and receive mailbox channels
 * @rx_msg: receive buffer passed to mailbox clients
 */
struct amd_pl_mbox {
	void __iomem *regs;
	struct clk *clk;
	int irq;
	u32 fifo_len;
	spinlock_t tx_lock; /* protects tx_active */
	bool tx_active;
	struct mbox_controller controller;
	struct mbox_chan chans[AMD_PL_MBOX_NUM_CHANS];
	struct zynqmp_ipi_message *rx_msg;
};

static int amd_pl_mbox_send_data(struct mbox_chan *chan, void *data)
{
	struct amd_pl_mbox *mb = chan->con_priv;
	struct zynqmp_ipi_message *msg = data;
	unsigned long flags;
	size_t offset;
	int ret = 0;

	if (chan != &mb->chans[AMD_PL_MBOX_CHAN_TX])
		return -EINVAL;

	/* The TX FIFO is empty here, since TX completion waits for it. */
	if (!msg || !msg->len || msg->len > mb->fifo_len * sizeof(u32))
		return -EINVAL;

	/*
	 * The mailbox core can submit a queued message after shutdown() has
	 * started, while the interface clock is being disabled. Do not access
	 * registers then.
	 */
	spin_lock_irqsave(&mb->tx_lock, flags);
	if (!mb->tx_active) {
		ret = -ENODEV;
		goto unlock;
	}

	for (offset = 0; offset < msg->len; offset += sizeof(u32)) {
		size_t count = min(sizeof(u32), msg->len - offset);
		u32 word = 0;

		if (readl(mb->regs + AMD_PL_MBOX_STATUS) &
		    AMD_PL_MBOX_STS_TX_FULL) {
			ret = -EBUSY;
			goto unlock;
		}

		memcpy(&word, &msg->data[offset], count);
		writel(word, mb->regs + AMD_PL_MBOX_WRDATA);
	}

unlock:
	spin_unlock_irqrestore(&mb->tx_lock, flags);
	return ret;
}

static bool amd_pl_mbox_last_tx_done(struct mbox_chan *chan)
{
	struct amd_pl_mbox *mb = chan->con_priv;
	unsigned long flags;
	bool done = true;

	/*
	 * The mailbox core can poll after shutdown() has started, while the
	 * interface clock is being disabled. Do not access registers then.
	 */
	spin_lock_irqsave(&mb->tx_lock, flags);
	if (mb->tx_active)
		/* SIT is zero, so the threshold status means TX FIFO empty. */
		done = readl(mb->regs + AMD_PL_MBOX_STATUS) &
		       AMD_PL_MBOX_STS_TX_THRESHOLD;
	spin_unlock_irqrestore(&mb->tx_lock, flags);

	return done;
}

static int amd_pl_mbox_startup(struct mbox_chan *chan)
{
	struct amd_pl_mbox *mb = chan->con_priv;
	unsigned long flags;
	u32 irq_enable;
	int ret;

	if (chan != &mb->chans[AMD_PL_MBOX_CHAN_TX] &&
	    chan != &mb->chans[AMD_PL_MBOX_CHAN_RX])
		return -EINVAL;

	ret = clk_prepare_enable(mb->clk);
	if (ret)
		return ret;

	if (chan == &mb->chans[AMD_PL_MBOX_CHAN_TX]) {
		writel(AMD_PL_MBOX_CTRL_TX_RESET, mb->regs + AMD_PL_MBOX_CTRL);
		writel(0, mb->regs + AMD_PL_MBOX_SEND_THRESHOLD);

		spin_lock_irqsave(&mb->tx_lock, flags);
		mb->tx_active = true;
		spin_unlock_irqrestore(&mb->tx_lock, flags);

		return 0;
	}

	writel(AMD_PL_MBOX_CTRL_RX_RESET, mb->regs + AMD_PL_MBOX_CTRL);
	/* Interrupt when the RX FIFO changes from empty to nonempty. */
	writel(0, mb->regs + AMD_PL_MBOX_RECV_THRESHOLD);
	writel(AMD_PL_MBOX_IRQ_RX_THRESHOLD | AMD_PL_MBOX_IRQ_ERROR,
	       mb->regs + AMD_PL_MBOX_IRQ_STATUS);
	irq_enable = readl(mb->regs + AMD_PL_MBOX_IRQ_ENABLE);
	irq_enable |= AMD_PL_MBOX_IRQ_RX_THRESHOLD | AMD_PL_MBOX_IRQ_ERROR;
	writel(irq_enable, mb->regs + AMD_PL_MBOX_IRQ_ENABLE);
	enable_irq(mb->irq);

	return 0;
}

static void amd_pl_mbox_shutdown(struct mbox_chan *chan)
{
	struct amd_pl_mbox *mb = chan->con_priv;
	unsigned long flags;
	u32 irq_enable;

	if (chan == &mb->chans[AMD_PL_MBOX_CHAN_TX]) {
		spin_lock_irqsave(&mb->tx_lock, flags);
		mb->tx_active = false;
		spin_unlock_irqrestore(&mb->tx_lock, flags);

		writel(AMD_PL_MBOX_CTRL_TX_RESET, mb->regs + AMD_PL_MBOX_CTRL);
		clk_disable_unprepare(mb->clk);
		return;
	}

	if (chan != &mb->chans[AMD_PL_MBOX_CHAN_RX])
		return;

	disable_irq(mb->irq);
	irq_enable = readl(mb->regs + AMD_PL_MBOX_IRQ_ENABLE);
	irq_enable &= ~(AMD_PL_MBOX_IRQ_RX_THRESHOLD | AMD_PL_MBOX_IRQ_ERROR);
	writel(irq_enable, mb->regs + AMD_PL_MBOX_IRQ_ENABLE);
	writel(AMD_PL_MBOX_IRQ_RX_THRESHOLD | AMD_PL_MBOX_IRQ_ERROR,
	       mb->regs + AMD_PL_MBOX_IRQ_STATUS);
	writel(AMD_PL_MBOX_CTRL_RX_RESET, mb->regs + AMD_PL_MBOX_CTRL);
	clk_disable_unprepare(mb->clk);
}

static const struct mbox_chan_ops amd_pl_mbox_ops = {
	.send_data = amd_pl_mbox_send_data,
	.last_tx_done = amd_pl_mbox_last_tx_done,
	.startup = amd_pl_mbox_startup,
	.shutdown = amd_pl_mbox_shutdown,
};

static void amd_pl_mbox_rx(struct amd_pl_mbox *mb)
{
	/* data[] follows a size_t in a kzalloc() buffer: u32 aligned. */
	u32 *rx_words = (u32 *)mb->rx_msg->data;
	u32 words;

	/*
	 * The hardware reports neither the peer's message length nor the FIFO
	 * occupancy. Read until the FIFO is empty and deliver the words as
	 * messages of at most one FIFO depth each.
	 */
	do {
		for (words = 0; words < mb->fifo_len; words++) {
			if (readl(mb->regs + AMD_PL_MBOX_STATUS) &
			    AMD_PL_MBOX_STS_RX_EMPTY)
				break;

			rx_words[words] = readl(mb->regs + AMD_PL_MBOX_RDDATA);
		}

		if (!words)
			break;

		mb->rx_msg->len = words * sizeof(u32);
		mbox_chan_received_data(&mb->chans[AMD_PL_MBOX_CHAN_RX],
					mb->rx_msg);
	} while (words == mb->fifo_len);
}

static irqreturn_t amd_pl_mbox_irq_thread(int irq, void *data)
{
	struct amd_pl_mbox *mb = data;
	u32 pending_intr;
	u32 irq_enable;
	u32 irq_status;

	irq_status = readl(mb->regs + AMD_PL_MBOX_IRQ_STATUS);
	irq_enable = readl(mb->regs + AMD_PL_MBOX_IRQ_ENABLE);
	pending_intr = irq_status & irq_enable;

	if (!pending_intr) {
		dev_warn_ratelimited(mb->controller.dev,
				     "unexpected interrupt: status %#x enable %#x\n",
				     irq_status, irq_enable);
		return IRQ_NONE;
	}

	/*
	 * Acknowledge before draining. The receive event latches when the RX
	 * FIFO goes from empty to nonempty, so a word that arrives after the
	 * drain has emptied the FIFO raises a new event.
	 */
	writel(pending_intr, mb->regs + AMD_PL_MBOX_IRQ_STATUS);

	if (pending_intr & AMD_PL_MBOX_IRQ_ERROR)
		dev_warn_ratelimited(mb->controller.dev,
				     "mailbox FIFO error: %#x\n",
				     readl(mb->regs + AMD_PL_MBOX_ERROR));

	if (pending_intr & AMD_PL_MBOX_IRQ_RX_THRESHOLD)
		amd_pl_mbox_rx(mb);

	return IRQ_HANDLED;
}

static struct mbox_chan *amd_pl_mbox_xlate(struct mbox_controller *ctlr,
					   const struct of_phandle_args *sp)
{
	if (sp->args_count != 1 || sp->args[0] >= AMD_PL_MBOX_NUM_CHANS)
		return ERR_PTR(-EINVAL);

	return &ctlr->chans[sp->args[0]];
}

static int amd_pl_mbox_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct amd_pl_mbox *mb;
	int ret;
	int i;

	mb = devm_kzalloc(dev, sizeof(*mb), GFP_KERNEL);
	if (!mb)
		return -ENOMEM;

	spin_lock_init(&mb->tx_lock);

	mb->fifo_len = AMD_PL_MBOX_FIFO_LEN_MIN;
	ret = of_property_read_u32(dev->of_node, "amd,pl-mailbox-depth",
				   &mb->fifo_len);
	if (ret && ret != -EINVAL)
		return dev_err_probe(dev, ret, "failed to read FIFO length\n");

	if (mb->fifo_len < AMD_PL_MBOX_FIFO_LEN_MIN ||
	    mb->fifo_len > AMD_PL_MBOX_FIFO_LEN_MAX)
		return dev_err_probe(dev, -EINVAL, "invalid FIFO length %u\n",
				     mb->fifo_len);

	mb->rx_msg = devm_kzalloc(dev, struct_size(mb->rx_msg, data,
						   mb->fifo_len * sizeof(u32)),
				  GFP_KERNEL);
	if (!mb->rx_msg)
		return -ENOMEM;

	mb->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mb->regs))
		return PTR_ERR(mb->regs);

	mb->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(mb->clk))
		return dev_err_probe(dev, PTR_ERR(mb->clk), "failed to get clock\n");

	mb->irq = platform_get_irq(pdev, 0);
	if (mb->irq < 0)
		return mb->irq;

	for (i = 0; i < AMD_PL_MBOX_NUM_CHANS; i++)
		mb->chans[i].con_priv = mb;

	mb->controller.dev = dev;
	mb->controller.ops = &amd_pl_mbox_ops;
	mb->controller.chans = mb->chans;
	mb->controller.num_chans = AMD_PL_MBOX_NUM_CHANS;
	mb->controller.txdone_poll = true;
	mb->controller.txpoll_period = 1;
	mb->controller.of_xlate = amd_pl_mbox_xlate;

	/* Draining up to one FIFO depth of words is done in process context. */
	ret = devm_request_threaded_irq(dev, mb->irq, NULL,
					amd_pl_mbox_irq_thread,
					IRQF_ONESHOT | IRQF_NO_AUTOEN,
					dev_name(dev), mb);
	if (ret)
		return ret;

	return devm_mbox_controller_register(dev, &mb->controller);
}

static const struct of_device_id amd_pl_mbox_of_match[] = {
	{ .compatible = "amd,pl-mailbox-2.1" },
	{ }
};
MODULE_DEVICE_TABLE(of, amd_pl_mbox_of_match);

static struct platform_driver amd_pl_mbox_driver = {
	.probe = amd_pl_mbox_probe,
	.driver = {
		.name = "amd-pl-mailbox",
		.of_match_table = amd_pl_mbox_of_match,
	},
};
module_platform_driver(amd_pl_mbox_driver);

MODULE_DESCRIPTION("AMD PL Mailbox soft IP controller");
MODULE_LICENSE("GPL");
