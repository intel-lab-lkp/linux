// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 * Copyright (c) 2025 Collabora Ltd
 *                    AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>
 */

#include <linux/arm-smccc.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mailbox_controller.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/soc/mediatek/mtk_sip_svc.h>

#define MTK_SIP_TINYSYS_SSPM_CONTROL	MTK_SIP_SMC_CMD(0x53c)
#define MTK_TINYSYS_SSPM_OP_MBOX_CLR	0
#define MTK_TINYSYS_SSPM_OP_MD2SPM_CLR	1

#define INTR_SET_OFS	0x0
#define INTR_CLR_OFS	0x4

struct mtk_tinysys_mhu_mbox_pdata {
	bool is_secure_mbox;
	bool notify_spm;
};

struct mtk_tinysys_mhu_mbox {
	void __iomem *base;
	int irq;
	const struct mtk_tinysys_mhu_mbox_pdata *pdata;
	struct mbox_controller mbox;
};

static inline struct mtk_tinysys_mhu_mbox *to_mtk_tinysys_mhu_mbox(struct mbox_controller *mbox)
{
	return container_of(mbox, struct mtk_tinysys_mhu_mbox, mbox);
}

static irqreturn_t mtk_tinysys_mhu_mbox_irq(int irq, void *data)
{
	u32 val;
	struct arm_smccc_res res;
	struct mbox_chan *chan = data;
	struct mtk_tinysys_mhu_mbox *priv = to_mtk_tinysys_mhu_mbox(chan->mbox);

	val = readl_relaxed(priv->base + INTR_CLR_OFS);
	if (!val)
		return IRQ_NONE;

	if (priv->pdata->is_secure_mbox) {
		/* Can't fail: ignore res.a0 checks */
		arm_smccc_smc(MTK_SIP_TINYSYS_SSPM_CONTROL,
			      MTK_TINYSYS_SSPM_OP_MBOX_CLR,
			      priv->irq, 0, 0, 0, 0, 0, &res);
	} else {
		writel(1, priv->base + INTR_CLR_OFS);
	}

	mbox_chan_received_data(chan, (void *)&val);

	if (priv->pdata->notify_spm)
		arm_smccc_smc(MTK_SIP_TINYSYS_SSPM_CONTROL,
			      MTK_TINYSYS_SSPM_OP_MD2SPM_CLR,
			      priv->irq, 0, 0, 0, 0, 0, &res);
	return IRQ_HANDLED;
}

static bool mtk_tinysys_mhu_mbox_last_tx_done(struct mbox_chan *chan)
{
	struct mtk_tinysys_mhu_mbox *priv = to_mtk_tinysys_mhu_mbox(chan->mbox);
	u32 val = readl_relaxed(priv->base + INTR_SET_OFS);

	return val == 0;
}

static int mtk_tinysys_mhu_mbox_send_data(struct mbox_chan *chan, void *data)
{
	struct mtk_tinysys_mhu_mbox *priv = to_mtk_tinysys_mhu_mbox(chan->mbox);
	u32 *arg = data;

	writel_relaxed(*arg, priv->base + INTR_SET_OFS);

	return 0;
}

static int mtk_tinysys_mhu_mbox_startup(struct mbox_chan *chan)
{
	struct mtk_tinysys_mhu_mbox *priv = to_mtk_tinysys_mhu_mbox(chan->mbox);

	irq_clear_status_flags(priv->irq, IRQ_NOAUTOEN);
	enable_irq(priv->irq);

	return 0;
}

static void mtk_tinysys_mhu_mbox_shutdown(struct mbox_chan *chan)
{
	struct mtk_tinysys_mhu_mbox *priv = to_mtk_tinysys_mhu_mbox(chan->mbox);

	disable_irq(priv->irq);
}

static const struct mbox_chan_ops tinysys_mbox_chan_ops = {
	.send_data = mtk_tinysys_mhu_mbox_send_data,
	.startup = mtk_tinysys_mhu_mbox_startup,
	.shutdown = mtk_tinysys_mhu_mbox_shutdown,
	.last_tx_done = mtk_tinysys_mhu_mbox_last_tx_done,
};

static int mtk_tinysys_mhu_mbox_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_tinysys_mhu_mbox *priv;
	struct mbox_controller *mbox;
	int ret;

	/* Allocate memory for device */
	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	priv->irq = platform_get_irq(pdev, 0);
	if (priv->irq < 0)
		return priv->irq;

	priv->pdata = device_get_match_data(dev);
	if (!priv->pdata)
		return -EINVAL;

	mbox = &priv->mbox;
	mbox->dev = dev;
	mbox->ops = &tinysys_mbox_chan_ops;
	mbox->txdone_irq = false;
	mbox->txdone_poll = true;
	mbox->txpoll_period = 1;
	mbox->num_chans = 1;
	mbox->chans = devm_kzalloc(dev, sizeof(*mbox->chans), GFP_KERNEL);
	if (!mbox->chans)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);

	ret = devm_request_irq(dev, priv->irq, mtk_tinysys_mhu_mbox_irq,
			       IRQF_NO_AUTOEN, dev_name(dev), mbox->chans);
	if (ret < 0)
		return ret;

	ret = devm_mbox_controller_register(dev, &priv->mbox);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to register mailbox\n");

	return 0;
}

static const struct mtk_tinysys_mhu_mbox_pdata mt6985_tsmhu_mbox_cfg = {
	/* Unsecured mailbox, no SPM notification */
};

static const struct mtk_tinysys_mhu_mbox_pdata mt6989_tsmhu_mbox_cfg = {
	.is_secure_mbox = true,
};

static const struct mtk_tinysys_mhu_mbox_pdata mt8196_tsmhu_mbox_cfg = {
	.notify_spm = true,
};

static const struct of_device_id mtk_tinysys_mhu_mbox_of_match[] = {
	{ .compatible = "mediatek,mt6985-tinysys-mhu-mbox", .data = &mt6985_tsmhu_mbox_cfg },
	{ .compatible = "mediatek,mt6989-tinysys-mhu-mbox", .data = &mt6989_tsmhu_mbox_cfg },
	{ .compatible = "mediatek,mt8196-tinysys-mhu-mbox", .data = &mt8196_tsmhu_mbox_cfg },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_tinysys_mhu_mbox_of_match);

static struct platform_driver mtk_tinysys_mhu_mbox_drv = {
	.probe = mtk_tinysys_mhu_mbox_probe,
	.driver = {
		.name = "mtk-tinysys-mhu-mbox",
		.of_match_table = mtk_tinysys_mhu_mbox_of_match,
	}
};
module_platform_driver(mtk_tinysys_mhu_mbox_drv);

MODULE_AUTHOR("AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>");
MODULE_DESCRIPTION("MediaTek TinySYS Mailbox Controller");
MODULE_LICENSE("GPL v2");
