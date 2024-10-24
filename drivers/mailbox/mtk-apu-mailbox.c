// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 MediaTek Inc.
 */

#include <asm/io.h>
#include <linux/bits.h>
#include <linux/interrupt.h>
#include <linux/mailbox_controller.h>
#include <linux/mailbox/mtk-apu-mailbox.h>
#include <linux/platform_device.h>

#define INBOX		(0x0)
#define OUTBOX		(0x20)
#define INBOX_IRQ	(0xc0)
#define OUTBOX_IRQ	(0xc4)
#define INBOX_IRQ_MASK	(0xd0)

#define SPARE_OFF_START	(0x40)
#define SPARE_OFF_END	(0xB0)

struct mtk_apu_mailbox {
	struct device *dev;
	void __iomem *regs;
	struct mbox_controller controller;
	u32 msgs[MSG_MBOX_SLOTS];
};

struct mtk_apu_mailbox *g_mbox;

static irqreturn_t mtk_apu_mailbox_irq_top_half(int irq, void *dev_id)
{
	struct mtk_apu_mailbox *mbox = dev_id;
	struct mbox_chan *link = &mbox->controller.chans[0];
	int i;

	for (i = 0; i < MSG_MBOX_SLOTS; i++)
		mbox->msgs[i] = readl(mbox->regs + OUTBOX + i * sizeof(u32));

	mbox_chan_received_data(link, &mbox->msgs);

	return IRQ_WAKE_THREAD;
}

static irqreturn_t mtk_apu_mailbox_irq_btm_half(int irq, void *dev_id)
{
	struct mtk_apu_mailbox *mbox = dev_id;
	struct mbox_chan *link = &mbox->controller.chans[0];

	mbox_chan_received_data_bh(link, &mbox->msgs);
	writel(readl(mbox->regs + OUTBOX_IRQ), mbox->regs + OUTBOX_IRQ);

	return IRQ_HANDLED;
}

static int mtk_apu_mailbox_send_data(struct mbox_chan *chan, void *data)
{
	struct mtk_apu_mailbox *mbox = container_of(chan->mbox,
						    struct mtk_apu_mailbox,
						    controller);
	struct mtk_apu_mailbox_msg *msg = data;
	int i;

	if (msg->send_cnt <= 0 || msg->send_cnt > MSG_MBOX_SLOTS) {
		dev_err(mbox->dev, "%s: invalid send_cnt %d\n", __func__, msg->send_cnt);
		return -EINVAL;
	}

	/*
	 *	Mask lowest "send_cnt-1" interrupts bits, so the interrupt on the other side
	 *	triggers only after the last data slot is written (sent).
	 */
	writel(GENMASK(msg->send_cnt - 2, 0), mbox->regs + INBOX_IRQ_MASK);
	for (i = 0; i < msg->send_cnt; i++)
		writel(msg->data[i], mbox->regs + INBOX + i * sizeof(u32));

	return 0;
}

static bool mtk_apu_mailbox_last_tx_done(struct mbox_chan *chan)
{
	struct mtk_apu_mailbox *mbox = container_of(chan->mbox,
						    struct mtk_apu_mailbox,
						    controller);

	return readl(mbox->regs + INBOX_IRQ) == 0;
}

static const struct mbox_chan_ops mtk_apu_mailbox_ops = {
	.send_data = mtk_apu_mailbox_send_data,
	.last_tx_done = mtk_apu_mailbox_last_tx_done,
};

/**
 * mtk_apu_mbox_write - Write value to specifice mtk_apu_mbox spare register.
 * @val: Value to be written.
 * @offset: Offset of the spare register.
 *
 * Return: 0 if successful
 *	   negative value if error happened
 */
int mtk_apu_mbox_write(u32 val, u32 offset)
{
	if (!g_mbox) {
		pr_err("mtk apu mbox was not initialized, stop writing register\n");
		return -ENODEV;
	}

	if (offset < SPARE_OFF_START || offset >= SPARE_OFF_END) {
		dev_err(g_mbox->dev, "Invalid offset %d for mtk apu mbox spare register\n", offset);
		return -EINVAL;
	}

	writel(val, g_mbox->regs + offset);
	return 0;
}
EXPORT_SYMBOL_NS(mtk_apu_mbox_write, MTK_APU_MAILBOX);

/**
 * mtk_apu_mbox_read - Read value to specifice mtk_apu_mbox spare register.
 * @offset: Offset of the spare register.
 * @val: Pointer to store read value.
 *
 * Return: 0 if successful
 *	   negative value if error happened
 */
int mtk_apu_mbox_read(u32 offset, u32 *val)
{
	if (!g_mbox) {
		pr_err("mtk apu mbox was not initialized, stop reading register\n");
		return -ENODEV;
	}

	if (offset < SPARE_OFF_START || offset >= SPARE_OFF_END) {
		dev_err(g_mbox->dev, "Invalid offset %d for mtk apu mbox spare register\n", offset);
		return -EINVAL;
	}

	*val = readl(g_mbox->regs + offset);

	return 0;
}
EXPORT_SYMBOL_NS(mtk_apu_mbox_read, MTK_APU_MAILBOX);

static int mtk_apu_mailbox_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_apu_mailbox *mbox;
	int irq = -1, ret = 0;

	mbox = devm_kzalloc(dev, sizeof(*mbox), GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;

	mbox->dev = dev;
	platform_set_drvdata(pdev, mbox);

	mbox->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mbox->regs))
		return PTR_ERR(mbox->regs);

	mbox->controller.txdone_irq = false;
	mbox->controller.txdone_poll = true;
	mbox->controller.txpoll_period = 1;
	mbox->controller.ops = &mtk_apu_mailbox_ops;
	mbox->controller.dev = dev;
	/*
	 * Here we only register 1 mbox channel.
	 * The remaining channels are used by other modules.
	 */
	mbox->controller.num_chans = 1;
	mbox->controller.chans = devm_kcalloc(dev, mbox->controller.num_chans,
					      sizeof(*mbox->controller.chans),
					      GFP_KERNEL);
	if (!mbox->controller.chans)
		return -ENOMEM;

	ret = devm_mbox_controller_register(dev, &mbox->controller);
	if (ret)
		return ret;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_threaded_irq(dev, irq, mtk_apu_mailbox_irq_top_half,
					mtk_apu_mailbox_irq_btm_half, IRQF_ONESHOT,
					dev_name(dev), mbox);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to request IRQ\n");

	g_mbox = mbox;

	dev_dbg(dev, "registered mtk apu mailbox\n");

	return 0;
}

static void mtk_apu_mailbox_remove(struct platform_device *pdev)
{
	g_mbox = NULL;
}

static const struct of_device_id mtk_apu_mailbox_of_match[] = {
	{ .compatible = "mediatek,mt8188-apu-mailbox" },
	{ .compatible = "mediatek,mt8196-apu-mailbox" },
	{}
};
MODULE_DEVICE_TABLE(of, mtk_apu_mailbox_of_match);

static struct platform_driver mtk_apu_mailbox_driver = {
	.probe = mtk_apu_mailbox_probe,
	.remove = mtk_apu_mailbox_remove,
	.driver = {
		.name = "mtk-apu-mailbox",
		.of_match_table = mtk_apu_mailbox_of_match,
	},
};

module_platform_driver(mtk_apu_mailbox_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek APU Mailbox Driver");
