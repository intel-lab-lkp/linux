// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Shanghai StarFive Technology Co., Ltd.
 *
 * Author: Jee Heng Sia <jeeheng.sia@starfivetech.com>
 * Author: Joshua Yeong <joshua.yeong@starfivetech.com>
 */

#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mailbox_controller.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#define CHAN_SET_OFFSET		0x0
#define CHAN_TX_STAT_OFFSET	0x4
#define CHAN_RX_STAT_OFFSET	0x8
#define CHAN_CLR_OFFSET		0xc
#define CHAN_RX_REG_OFFSET	0x40

#define CHAN_RX_DOORBELL	GENMASK(30, 0)

#define MEU0_OFFSET		0x4000
#define MEU1_OFFSET		0x0

#define MEU_CHANS_GROUP		2
#define MEU_NUM_DOORBELLS	31
#define MEU_TOTAL_CHANS		(MEU_CHANS_GROUP * MEU_NUM_DOORBELLS)

struct meu_db_link {
	unsigned int irq;
	void __iomem *tx_reg;
	void __iomem *rx_reg;
};

struct starfive_meu {
	void __iomem *base;
	struct meu_db_link mlink[MEU_CHANS_GROUP];
	struct mbox_controller mbox;
	struct device *dev;
};

/**
 * StarFive MEU Mailbox allocated channel information
 *
 * @meu: Pointer to parent mailbox device
 * @pchan: Physical channel within which this doorbell resides in
 * @doorbell: doorbell number pertaining to this channel
 */
struct meu_db_channel {
	struct starfive_meu *meu;
	unsigned int pchan;
	unsigned int doorbell;
};

static inline struct mbox_chan *
meu_db_mbox_to_channel(struct mbox_controller *mbox, unsigned int pchan,
		       unsigned int doorbell)
{
	struct meu_db_channel *chan_info;
	int i;

	for (i = 0; i < mbox->num_chans; i++) {
		chan_info = mbox->chans[i].con_priv;
		if (chan_info && chan_info->pchan == pchan &&
		    chan_info->doorbell == doorbell)
			return &mbox->chans[i];
	}

	return NULL;
}

static void meu_db_mbox_clear_irq(struct mbox_chan *chan)
{
	struct meu_db_channel *chan_info = chan->con_priv;
	void __iomem *base = chan_info->meu->mlink[chan_info->pchan].rx_reg;

	writel_relaxed(BIT(chan_info->doorbell), base + CHAN_CLR_OFFSET);
}

static unsigned int meu_db_mbox_irq_to_pchan_num(struct starfive_meu *meu, int irq)
{
	unsigned int pchan;

	for (pchan = 0; pchan < MEU_CHANS_GROUP; pchan++)
		if (meu->mlink[pchan].irq == irq)
			break;
	return pchan;
}

static struct mbox_chan *
meu_db_mbox_irq_to_channel(struct starfive_meu *meu, unsigned int pchan)
{
	void __iomem *base = meu->mlink[pchan].rx_reg;
	struct mbox_controller *mbox = &meu->mbox;
	struct mbox_chan *chan = NULL;
	unsigned int doorbell;
	unsigned long bits;

	bits = FIELD_GET(CHAN_RX_DOORBELL, readl_relaxed(base + CHAN_RX_STAT_OFFSET));
	if (!bits)
		/* No IRQs fired in specified physical channel */
		return NULL;

	/* An IRQ has fired, find the associated channel */
	for (doorbell = 0; bits; doorbell++) {
		if (!test_and_clear_bit(doorbell, &bits))
			continue;

		chan = meu_db_mbox_to_channel(mbox, pchan, doorbell);
		if (chan)
			break;

		/* Clear IRQ for unregistered doorbell */
		writel_relaxed(BIT(doorbell), base + CHAN_CLR_OFFSET);
		dev_err(mbox->dev,
			"Channel not registered: pchan: %d doorbell: %d\n",
			pchan, doorbell);
	}

	return chan;
}

static irqreturn_t meu_db_mbox_rx_handler(int irq, void *data)
{
	struct starfive_meu *meu = data;
	unsigned int pchan = meu_db_mbox_irq_to_pchan_num(meu, irq);
	struct mbox_chan *chan;

	while (NULL != (chan = meu_db_mbox_irq_to_channel(meu, pchan))) {
		mbox_chan_received_data(chan, NULL);
		meu_db_mbox_clear_irq(chan);
	}

	return IRQ_HANDLED;
}

static bool meu_db_last_tx_done(struct mbox_chan *chan)
{
	struct meu_db_channel *chan_info = chan->con_priv;
	void __iomem *base = chan_info->meu->mlink[chan_info->pchan].tx_reg;

	if (readl_relaxed(base + CHAN_TX_STAT_OFFSET) & BIT(chan_info->doorbell))
		return false;

	return true;
}

static int meu_db_send_data(struct mbox_chan *chan, void *data)
{
	struct meu_db_channel *chan_info = chan->con_priv;
	void __iomem *base = chan_info->meu->mlink[chan_info->pchan].tx_reg;

	/* Send event to co-processor */
	writel_relaxed(BIT(chan_info->doorbell), base + CHAN_SET_OFFSET);

	return 0;
}

static int meu_db_startup(struct mbox_chan *chan)
{
	meu_db_mbox_clear_irq(chan);
	return 0;
}

static void meu_db_shutdown(struct mbox_chan *chan)
{
	struct meu_db_channel *chan_info = chan->con_priv;
	struct mbox_controller *mbox = &chan_info->meu->mbox;
	int i;

	for (i = 0; i < mbox->num_chans; i++)
		if (chan == &mbox->chans[i])
			break;

	if (mbox->num_chans == i) {
		dev_warn(mbox->dev, "Request to free non-existent channel\n");
		return;
	}

	/* Reset channel */
	meu_db_mbox_clear_irq(chan);
	devm_kfree(mbox->dev, chan->con_priv);
	chan->con_priv = NULL;
}

static struct mbox_chan *meu_db_mbox_xlate(struct mbox_controller *mbox,
					   const struct of_phandle_args *spec)
{
	struct starfive_meu *meu = dev_get_drvdata(mbox->dev);
	unsigned int doorbell = spec->args[0] % MEU_NUM_DOORBELLS;
	unsigned int pchan = spec->args[0] / MEU_NUM_DOORBELLS;
	struct meu_db_channel *chan_info;
	struct mbox_chan *chan;
	int i;

	/* Bounds checking */
	if (pchan >= MEU_CHANS_GROUP || doorbell >= MEU_NUM_DOORBELLS) {
		dev_err(mbox->dev,
			"Invalid channel requested pchan: %d doorbell: %d\n",
			pchan, doorbell);
		return ERR_PTR(-EINVAL);
	}

	/* Is requested channel free? */
	chan = meu_db_mbox_to_channel(mbox, pchan, doorbell);
	if (chan) {
		dev_err(mbox->dev, "Channel in use: pchan: %d doorbell: %d\n",
			pchan, doorbell);
		return ERR_PTR(-EBUSY);
	}

	/* Find the first free slot */
	for (i = 0; i < mbox->num_chans; i++)
		if (!mbox->chans[i].con_priv)
			break;

	if (mbox->num_chans == i) {
		dev_err(mbox->dev, "No free channels left\n");
		return ERR_PTR(-EBUSY);
	}

	chan = &mbox->chans[i];

	chan_info = devm_kzalloc(mbox->dev, sizeof(*chan_info), GFP_KERNEL);
	if (!chan_info)
		return ERR_PTR(-ENOMEM);

	chan_info->meu = meu;
	chan_info->pchan = pchan;
	chan_info->doorbell = doorbell;

	chan->con_priv = chan_info;

	dev_dbg(mbox->dev, "mbox: created channel phys: %d doorbell: %d\n",
		pchan, doorbell);

	return chan;
}

static const struct mbox_chan_ops meu_db_ops = {
	.send_data = meu_db_send_data,
	.startup = meu_db_startup,
	.shutdown = meu_db_shutdown,
	.last_tx_done = meu_db_last_tx_done,
};

static int starfive_mbox_probe(struct platform_device *pdev)
{
	int meu_reg[MEU_CHANS_GROUP] = {MEU0_OFFSET, MEU1_OFFSET};
	struct starfive_meu *meu;
	struct mbox_chan *chans;
	int i, err;

	meu = devm_kzalloc(&pdev->dev, sizeof(*meu), GFP_KERNEL);
	if (!meu)
		return -ENOMEM;

	meu->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(meu->base))
		return PTR_ERR(meu->base);

	chans = devm_kcalloc(&pdev->dev, MEU_TOTAL_CHANS, sizeof(*chans), GFP_KERNEL);
	if (!chans)
		return -ENOMEM;

	meu->dev = &pdev->dev;
	meu->mbox.dev = &pdev->dev;
	meu->mbox.chans = chans;
	meu->mbox.num_chans = MEU_TOTAL_CHANS;
	meu->mbox.txdone_irq = false;
	meu->mbox.txdone_poll = true;
	meu->mbox.txpoll_period = 1;
	meu->mbox.of_xlate = meu_db_mbox_xlate;
	meu->mbox.ops = &meu_db_ops;

	platform_set_drvdata(pdev, meu);

	for (i = 0; i < MEU_CHANS_GROUP; i++) {
		int irq = meu->mlink[i].irq = platform_get_irq(pdev, i);

		if (irq <= 0) {
			dev_dbg(&pdev->dev, "No IRQ found for Channel %d\n", i);
			return irq;
		}

		meu->mlink[i].tx_reg = meu->base + meu_reg[i];
		meu->mlink[i].rx_reg = meu->mlink[i].tx_reg + CHAN_RX_REG_OFFSET;

		err = devm_request_threaded_irq(&pdev->dev, irq, NULL,
						meu_db_mbox_rx_handler,
						IRQF_ONESHOT, "meu_db_link", meu);
		if (err) {
			dev_err(&pdev->dev, "Can't claim IRQ %d\n", irq);
			return err;
		}
	}

	err = devm_mbox_controller_register(&pdev->dev, &meu->mbox);
	if (err) {
		dev_err(&pdev->dev, "Failed to register mailboxes %d\n", err);
		return err;
	}

	dev_info(&pdev->dev, "StarFive MEU Doorbell mailbox registered\n");
	return 0;
}

static const struct of_device_id starfive_mbox_of_match[] = {
	{ .compatible = "starfive,jh8100-meu",},
	{ },
};
MODULE_DEVICE_TABLE(of, starfive_mbox_of_match);

static struct platform_driver starfive_mbox_driver = {
	.probe	= starfive_mbox_probe,
	.driver = {
		.name = "starfive-meu-mailbox",
		.of_match_table = starfive_mbox_of_match,
	},
};

module_platform_driver(starfive_mbox_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("StarFive MEU: communicate between CPU cores and SCP");
MODULE_AUTHOR("Jee Heng Sia <jeeheng.sia@starfivetech.com>");
MODULE_AUTHOR("Joshua Yeong <joshua.yeong@starfivetech.com>");
