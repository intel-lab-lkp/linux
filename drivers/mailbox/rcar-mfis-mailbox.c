// SPDX-License-Identifier: GPL-2.0
/*
 * Renesas MFIS (Multifunctional Interface) Mailbox Driver
 *
 * Copyright (c) 2025, Renesas Electronics Corporation. All rights reserved.
 */

#include <linux/device.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/interrupt.h>
#include <linux/mailbox_controller.h>
#include <linux/module.h>
#include <linux/platform_device.h>

static int mfis_send_data(struct mbox_chan *link, void *data)
{
	void __iomem *reg = link->con_priv;

	/*Trigger interrupt request to firmware(SCP)*/
	iowrite32(0x1, reg);

	return 0;
}

static irqreturn_t mfis_rx_interrupt(int irq, void *data)
{
	struct mbox_chan *link = data;
	void __iomem *reg = link->con_priv;

	mbox_chan_received_data(link, 0);

	/* Clear interrupt register */
	iowrite32(0x0, reg);

	return IRQ_HANDLED;
}

static int mfis_startup(struct mbox_chan *link)
{
	struct mbox_controller *mbox = link->mbox;
	struct device *dev = mbox->dev;
	int irq;
	int ret;

	irq = of_irq_get(dev->of_node, 0);

	ret = request_irq(irq, mfis_rx_interrupt,
			  IRQF_SHARED, "mfis-mbox", link);
	if (ret) {
		dev_err(dev,
			"Unable to acquire IRQ %d\n", irq);
		return ret;
	}
	return 0;
}

static void mfis_shutdown(struct mbox_chan *link)
{
	struct mbox_controller *mbox = link->mbox;
	struct device *dev = mbox->dev;
	int irq;

	irq = of_irq_get(dev->of_node, 0);

	free_irq(irq, link);
}

static bool mfis_last_tx_done(struct mbox_chan *link)
{
	return true;
}

static const struct mbox_chan_ops mfis_chan_ops = {
	.send_data	= mfis_send_data,
	.startup	= mfis_startup,
	.shutdown	= mfis_shutdown,
	.last_tx_done	= mfis_last_tx_done
};

static int mfis_mbox_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mbox_controller *mbox;
	void __iomem *reg;
	int ret, count = 2, i;

	mbox = devm_kzalloc(dev, sizeof(*mbox), GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;

	mbox->chans = devm_kcalloc(dev, count, sizeof(*mbox->chans), GFP_KERNEL);
	if (!mbox->chans)
		return -ENOMEM;

	reg = devm_platform_ioremap_resource(pdev, i);
	if (IS_ERR(reg))
		return PTR_ERR(reg);

	for (i = 0; i < count; i++) {
		mbox->chans[i].mbox	= mbox;
		mbox->chans[i].con_priv	= reg + ((1 - i) * 4);
	}

	mbox->txdone_poll	= true;
	mbox->txdone_irq	= false;
	mbox->txpoll_period	= 1;
	mbox->num_chans		= count;
	mbox->ops		= &mfis_chan_ops;
	mbox->dev		= dev;

	ret = mbox_controller_register(mbox);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, mbox);
	dev_info(dev, "MFIS mailbox is probed\n");

	return 0;
}

static const struct of_device_id mfis_mbox_of_match[] = {
	{ .compatible = "renesas,mfis-mbox", },
	{},
};
MODULE_DEVICE_TABLE(of, mfis_mbox_of_match);

static struct platform_driver mfis_mbox_driver = {
	.driver = {
		.name = "renesas-mfis-mbox",
		.of_match_table = mfis_mbox_of_match,
	},
	.probe	= mfis_mbox_probe,
};
module_platform_driver(mfis_mbox_driver);
MODULE_DESCRIPTION("Renesas MFIS mailbox driver");
MODULE_LICENSE("GPL v2");
