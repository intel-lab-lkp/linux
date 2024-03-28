// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024, The Linux Foundation. All rights reserved.
 */

#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/platform_device.h>
#include <linux/mailbox_controller.h>
#include <linux/module.h>

#define APSS_CPUCP_IPC_CHAN_SUPPORTED		3
#define APSS_CPUCP_MBOX_CMD_OFF			0x4

/* Tx Registers */
#define APSS_CPUCP_TX_MBOX_IDR			0
#define APSS_CPUCP_TX_MBOX_CMD			0x100

/* Rx Registers */
#define APSS_CPUCP_RX_MBOX_IDR			0
#define APSS_CPUCP_RX_MBOX_CMD			0x100
#define APSS_CPUCP_RX_MBOX_MAP			0x4000
#define APSS_CPUCP_RX_MBOX_STAT			0x4400
#define APSS_CPUCP_RX_MBOX_CLEAR		0x4800
#define APSS_CPUCP_RX_MBOX_EN			0x4C00
#define APSS_CPUCP_RX_MBOX_CMD_MASK		0xFFFFFFFFFFFFFFFF

/**
 * struct qcom_cpucp_mbox - Holder for the mailbox driver
 * @chans:			The mailbox channel
 * @mbox:			The mailbox controller
 * @tx_base:			Base address of the CPUCP tx registers
 * @rx_base:			Base address of the CPUCP rx registers
 * @dev:			Device associated with this instance
 * @irq:			CPUCP to AP irq
 */
struct qcom_cpucp_mbox {
	struct mbox_chan chans[APSS_CPUCP_IPC_CHAN_SUPPORTED];
	struct mbox_controller mbox;
	void __iomem *tx_base;
	void __iomem *rx_base;
	struct device *dev;
	int irq;
};

static inline int channel_number(struct mbox_chan *chan)
{
	return chan - chan->mbox->chans;
}

static irqreturn_t qcom_cpucp_mbox_irq_fn(int irq, void *data)
{
	struct qcom_cpucp_mbox *cpucp = data;
	struct mbox_chan *chan;
	unsigned long flags;
	u64 status;
	u32 val;
	int i;

	status = readq(cpucp->rx_base + APSS_CPUCP_RX_MBOX_STAT);

	for (i = 0; i < APSS_CPUCP_IPC_CHAN_SUPPORTED; i++) {
		val = 0;
		if (status & ((u64)1 << i)) {
			val = readl(cpucp->rx_base + APSS_CPUCP_RX_MBOX_CMD + (i * 8) + APSS_CPUCP_MBOX_CMD_OFF);
			chan = &cpucp->chans[i];
			spin_lock_irqsave(&chan->lock, flags);
			if (chan->cl)
				mbox_chan_received_data(chan, &val);
			spin_unlock_irqrestore(&chan->lock, flags);
			writeq(status, cpucp->rx_base + APSS_CPUCP_RX_MBOX_CLEAR);
		}
	}

	return IRQ_HANDLED;
}

static int qcom_cpucp_mbox_startup(struct mbox_chan *chan)
{
	struct qcom_cpucp_mbox *cpucp = container_of(chan->mbox, struct qcom_cpucp_mbox, mbox);
	unsigned long chan_id = channel_number(chan);
	u64 val;

	val = readq(cpucp->rx_base + APSS_CPUCP_RX_MBOX_EN);
	val |= BIT(chan_id);
	writeq(val, cpucp->rx_base + APSS_CPUCP_RX_MBOX_EN);

	return 0;
}

static void qcom_cpucp_mbox_shutdown(struct mbox_chan *chan)
{
	struct qcom_cpucp_mbox *cpucp = container_of(chan->mbox, struct qcom_cpucp_mbox, mbox);
	unsigned long chan_id = channel_number(chan);
	u64 val;

	val = readq(cpucp->rx_base + APSS_CPUCP_RX_MBOX_EN);
	val &= ~BIT(chan_id);
	writeq(val, cpucp->rx_base + APSS_CPUCP_RX_MBOX_EN);
}

static int qcom_cpucp_mbox_send_data(struct mbox_chan *chan, void *data)
{
	struct qcom_cpucp_mbox *cpucp = container_of(chan->mbox, struct qcom_cpucp_mbox, mbox);
	unsigned long chan_id = channel_number(chan);
	u32 *val = data;

	writel(*val, cpucp->tx_base + APSS_CPUCP_TX_MBOX_CMD + (chan_id * 8) + APSS_CPUCP_MBOX_CMD_OFF);

	return 0;
}

static const struct mbox_chan_ops qcom_cpucp_mbox_chan_ops = {
	.startup = qcom_cpucp_mbox_startup,
	.send_data = qcom_cpucp_mbox_send_data,
	.shutdown = qcom_cpucp_mbox_shutdown
};

static int qcom_cpucp_mbox_probe(struct platform_device *pdev)
{
	struct qcom_cpucp_mbox *cpucp;
	struct mbox_controller *mbox;
	int ret;

	cpucp = devm_kzalloc(&pdev->dev, sizeof(*cpucp), GFP_KERNEL);
	if (!cpucp)
		return -ENOMEM;

	cpucp->dev = &pdev->dev;

	cpucp->rx_base = devm_of_iomap(cpucp->dev, cpucp->dev->of_node, 0, NULL);
	if (IS_ERR(cpucp->rx_base))
		return PTR_ERR(cpucp->rx_base);

	cpucp->tx_base = devm_of_iomap(cpucp->dev, cpucp->dev->of_node, 1, NULL);
	if (IS_ERR(cpucp->tx_base))
		return PTR_ERR(cpucp->tx_base);

	writeq(0, cpucp->rx_base + APSS_CPUCP_RX_MBOX_EN);
	writeq(0, cpucp->rx_base + APSS_CPUCP_RX_MBOX_CLEAR);
	writeq(0, cpucp->rx_base + APSS_CPUCP_RX_MBOX_MAP);

	cpucp->irq = platform_get_irq(pdev, 0);
	if (cpucp->irq < 0) {
		dev_err(&pdev->dev, "Failed to get the IRQ\n");
		return cpucp->irq;
	}

	ret = devm_request_irq(&pdev->dev, cpucp->irq, qcom_cpucp_mbox_irq_fn,
			       IRQF_TRIGGER_HIGH, "apss_cpucp_mbox", cpucp);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to register the irq: %d\n", ret);
		return ret;
	}

	writeq(APSS_CPUCP_RX_MBOX_CMD_MASK, cpucp->rx_base + APSS_CPUCP_RX_MBOX_MAP);

	mbox = &cpucp->mbox;
	mbox->dev = cpucp->dev;
	mbox->num_chans = APSS_CPUCP_IPC_CHAN_SUPPORTED;
	mbox->chans = cpucp->chans;
	mbox->ops = &qcom_cpucp_mbox_chan_ops;
	mbox->txdone_irq = false;
	mbox->txdone_poll = false;

	ret = mbox_controller_register(mbox);
	if (ret) {
		dev_err(&pdev->dev, "Failed to create mailbox\n");
		return ret;
	}

	platform_set_drvdata(pdev, cpucp);

	return 0;
}

static int qcom_cpucp_mbox_remove(struct platform_device *pdev)
{
	struct qcom_cpucp_mbox *cpucp = platform_get_drvdata(pdev);

	mbox_controller_unregister(&cpucp->mbox);

	return 0;
}

static const struct of_device_id qcom_cpucp_mbox_of_match[] = {
	{ .compatible = "qcom,x1e80100-cpucp-mbox"},
	{}
};
MODULE_DEVICE_TABLE(of, qcom_cpucp_mbox_of_match);

static struct platform_driver qcom_cpucp_mbox_driver = {
	.probe = qcom_cpucp_mbox_probe,
	.remove = qcom_cpucp_mbox_remove,
	.driver = {
		.name = "qcom_cpucp_mbox",
		.of_match_table = qcom_cpucp_mbox_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(qcom_cpucp_mbox_driver);

MODULE_DESCRIPTION("QTI CPUCP MBOX Driver");
MODULE_LICENSE("GPL");
