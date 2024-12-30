// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 MediaTek Inc.
 */

#include <asm/io.h>
#include <linux/bits.h>
#include <linux/bitops.h>
#include <linux/container_of.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/mailbox_controller.h>
#include <linux/mailbox/mtk-apu-mailbox.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define MTK_APU_MBOX_INBOX		(0x0)
#define MTK_APU_MBOX_OUTBOX		(0x20)
#define MTK_APU_MBOX_INBOX_IRQ		(0xc0)
#define MTK_APU_MBOX_OUTBOX_IRQ		(0xc4)
#define MTK_APU_MBOX_INBOX_IRQ_MASK	(0xd0)

#define MTK_APU_MBOX_SPARE_OFF_START	(0x40)
#define MTK_APU_MBOX_SPARE_OFF_END	(0xB0)

struct mtk_apu_mailbox_platdata {
	u8 msg_mbox_slots;
};

struct mtk_apu_mailbox {
	u8 msg_mbox_slots;
	struct device *dev;
	void __iomem *regs;
	struct mbox_controller mbox;
	struct mtk_apu_mailbox_msg msgs;
};

static inline struct mtk_apu_mailbox *get_mtk_apu_mailbox(struct mbox_controller *mbox)
{
	return container_of(mbox, struct mtk_apu_mailbox, mbox);
}

static irqreturn_t mtk_apu_mailbox_irq(int irq, void *data)
{
	struct mbox_chan *chan = data;
	struct mtk_apu_mailbox *apu_mbox = get_mtk_apu_mailbox(chan->mbox);
	struct mbox_chan *link = &apu_mbox->mbox.chans[0];
	u8 data_cnt = fls(readl(apu_mbox->regs + MTK_APU_MBOX_OUTBOX_IRQ));

	memcpy_fromio(apu_mbox->msgs.data, apu_mbox->regs + MTK_APU_MBOX_OUTBOX,
		      sizeof(*apu_mbox->msgs.data) * data_cnt);

	mbox_chan_received_data(link, apu_mbox->msgs.data);

	return IRQ_WAKE_THREAD;
}

static irqreturn_t mtk_apu_mailbox_irq_thread(int irq, void *data)
{
	struct mbox_chan *chan = data;
	struct mtk_apu_mailbox *apu_mbox = get_mtk_apu_mailbox(chan->mbox);
	struct mbox_chan *link = &apu_mbox->mbox.chans[0];

	mbox_chan_received_data_bh(link, apu_mbox->msgs.data);
	writel(readl(apu_mbox->regs + MTK_APU_MBOX_OUTBOX_IRQ),
	       apu_mbox->regs + MTK_APU_MBOX_OUTBOX_IRQ);

	return IRQ_HANDLED;
}

static int mtk_apu_mailbox_send_data(struct mbox_chan *chan, void *data)
{
	struct mtk_apu_mailbox *apu_mbox = get_mtk_apu_mailbox(chan->mbox);
	struct mtk_apu_mailbox_msg *msg = data;

	if (msg->data_cnt <= 0 || msg->data_cnt > apu_mbox->msg_mbox_slots) {
		dev_err(apu_mbox->dev, "%s: invalid data_cnt %d\n", __func__, msg->data_cnt);
		return -EINVAL;
	}

	/*
	 *	Mask lowest "data_cnt-1" interrupts bits, so the interrupt on the other side
	 *	triggers only after the last data slot is written (sent).
	 */
	writel(GENMASK(msg->data_cnt - 2, 0), apu_mbox->regs + MTK_APU_MBOX_INBOX_IRQ_MASK);
	memcpy_toio(apu_mbox->regs + MTK_APU_MBOX_INBOX,
		    msg->data, sizeof(*msg->data) * msg->data_cnt);

	return 0;
}

static bool mtk_apu_mailbox_last_tx_done(struct mbox_chan *chan)
{
	struct mtk_apu_mailbox *apu_mbox = get_mtk_apu_mailbox(chan->mbox);

	return readl(apu_mbox->regs + MTK_APU_MBOX_INBOX_IRQ) == 0;
}

static const struct mbox_chan_ops mtk_apu_mailbox_ops = {
	.send_data = mtk_apu_mailbox_send_data,
	.last_tx_done = mtk_apu_mailbox_last_tx_done,
};

static bool mtk_apu_mailbox_regmap_accessible_reg(struct device *dev,
						  unsigned int reg)
{
	if (reg >= MTK_APU_MBOX_SPARE_OFF_START && reg < MTK_APU_MBOX_SPARE_OFF_END)
		return true;
	return false;
}

static const struct regmap_config mtk_apu_mailbox_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = 0x100,
	.readable_reg	= mtk_apu_mailbox_regmap_accessible_reg,
	.writeable_reg	= mtk_apu_mailbox_regmap_accessible_reg,
};

static int mtk_apu_mailbox_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct mtk_apu_mailbox_platdata *data;
	struct mtk_apu_mailbox *apu_mbox;
	struct regmap *regmap;
	int irq = -1, ret = 0;

	data = of_device_get_match_data(dev);

	apu_mbox = devm_kzalloc(dev, sizeof(*apu_mbox), GFP_KERNEL);
	if (!apu_mbox)
		return -ENOMEM;

	apu_mbox->dev = dev;
	platform_set_drvdata(pdev, apu_mbox);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	apu_mbox->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(apu_mbox->regs))
		return PTR_ERR(apu_mbox->regs);

	apu_mbox->msg_mbox_slots = data->msg_mbox_slots;
	apu_mbox->msgs.data = devm_kcalloc(dev, apu_mbox->msg_mbox_slots,
					   sizeof(*apu_mbox->msgs.data), GFP_KERNEL);
	if (!apu_mbox->msgs.data)
		return -ENOMEM;

	apu_mbox->mbox.txdone_irq = false;
	apu_mbox->mbox.txdone_poll = true;
	apu_mbox->mbox.txpoll_period = 1;
	apu_mbox->mbox.ops = &mtk_apu_mailbox_ops;
	apu_mbox->mbox.dev = dev;
	apu_mbox->mbox.num_chans = 1;
	apu_mbox->mbox.chans = devm_kcalloc(dev, apu_mbox->mbox.num_chans,
					    sizeof(*apu_mbox->mbox.chans),
					    GFP_KERNEL);
	if (!apu_mbox->mbox.chans)
		return -ENOMEM;

	ret = devm_mbox_controller_register(dev, &apu_mbox->mbox);
	if (ret)
		return ret;

	ret = devm_request_threaded_irq(dev, irq, mtk_apu_mailbox_irq,
					mtk_apu_mailbox_irq_thread, IRQF_ONESHOT,
					dev_name(dev), apu_mbox->mbox.chans);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to request IRQ\n");

	regmap = devm_regmap_init_mmio(dev, apu_mbox->regs, &mtk_apu_mailbox_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	return 0;
}

static const struct mtk_apu_mailbox_platdata mt8196_platdata = {
	.msg_mbox_slots = 8,
};

static const struct of_device_id mtk_apu_mailbox_of_match[] = {
	{ .compatible = "mediatek,mt8196-apu-mailbox", .data = &mt8196_platdata },
	{}
};
MODULE_DEVICE_TABLE(of, mtk_apu_mailbox_of_match);

static struct platform_driver mtk_apu_mailbox_driver = {
	.probe = mtk_apu_mailbox_probe,
	.driver = {
		.name = "mtk-apu-mailbox",
		.of_match_table = mtk_apu_mailbox_of_match,
	},
};
module_platform_driver(mtk_apu_mailbox_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek APU Mailbox Driver");
