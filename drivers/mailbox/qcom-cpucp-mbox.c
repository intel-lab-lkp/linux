// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/mailbox_controller.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#define APSS_CPUCP_IPC_CHAN_SUPPORTED		3

/* Rx Registers */
#define APSS_CPUCP_V2_RX_MBOX_CMD_MASK		GENMASK_ULL(63, 0)
#define APSS_CPUCP_V1_SEND_IRQ_VAL		BIT(28)
#define APSS_CPUCP_V1_CLEAR_IRQ_VAL		BIT(3)
#define APSS_CPUCP_V1_STATUS_IRQ_VAL		BIT(3)

struct qcom_cpucp_mbox_desc {
	u32 enable_reg;
	u32 map_reg;
	u32 rx_reg;
	u32 tx_reg;
	u32 status_reg;
	u32 clear_reg;
	u32 chan_stride;
	bool v2_mbox;
	u32 num_chans;
};

/**
 * struct qcom_cpucp_mbox - Holder for the mailbox driver
 * @chans:			The mailbox channel
 * @mbox:			The mailbox controller
 * @tx_base:			Base address of the CPUCP tx registers
 * @rx_base:			Base address of the CPUCP rx registers
 */
struct qcom_cpucp_mbox {
	struct mbox_chan chans[APSS_CPUCP_IPC_CHAN_SUPPORTED];
	const struct qcom_cpucp_mbox_desc *desc;
	struct mbox_controller mbox;
	void __iomem *tx_base;
	void __iomem *rx_base;
};

static inline int channel_number(struct mbox_chan *chan)
{
	return chan - chan->mbox->chans;
}

static irqreturn_t qcom_cpucp_mbox_irq_fn(int irq, void *data)
{
	struct qcom_cpucp_mbox *cpucp = data;
	const struct qcom_cpucp_mbox_desc *desc = cpucp->desc;
	int i;

	for (i = 0; i < desc->num_chans; i++) {
		u32 val = readl(cpucp->rx_base + desc->status_reg + (i * desc->chan_stride));
		struct mbox_chan *chan = &cpucp->chans[i];
		unsigned long flags;

		if (val & APSS_CPUCP_V1_STATUS_IRQ_VAL) {
			writel(APSS_CPUCP_V1_CLEAR_IRQ_VAL,
			       cpucp->rx_base + desc->clear_reg + (i * desc->chan_stride));
			/* Make sure reg write is complete before proceeding */
			mb();
			spin_lock_irqsave(&chan->lock, flags);
			if (chan->cl)
				mbox_chan_received_data(chan, NULL);
			spin_unlock_irqrestore(&chan->lock, flags);
		}
	}

	return IRQ_HANDLED;
}

static irqreturn_t qcom_cpucp_v2_mbox_irq_fn(int irq, void *data)
{
	struct qcom_cpucp_mbox *cpucp = data;
	const struct qcom_cpucp_mbox_desc *desc = cpucp->desc;
	u64 status;
	int i;

	status = readq(cpucp->rx_base + desc->status_reg);

	for_each_set_bit(i, (unsigned long *)&status, desc->num_chans) {
		u32 val = readl(cpucp->rx_base + desc->rx_reg + (i * desc->chan_stride));
		struct mbox_chan *chan = &cpucp->chans[i];
		unsigned long flags;

		/* Provide mutual exclusion with changes to chan->cl */
		spin_lock_irqsave(&chan->lock, flags);
		if (chan->cl)
			mbox_chan_received_data(chan, &val);
		writeq(BIT(i), cpucp->rx_base + desc->clear_reg);
		spin_unlock_irqrestore(&chan->lock, flags);
	}

	return IRQ_HANDLED;
}

static int qcom_cpucp_mbox_startup(struct mbox_chan *chan)
{
	struct qcom_cpucp_mbox *cpucp = container_of(chan->mbox, struct qcom_cpucp_mbox, mbox);
	const struct qcom_cpucp_mbox_desc *desc = cpucp->desc;
	unsigned long chan_id = channel_number(chan);
	u64 val;

	if (desc->v2_mbox) {
		val = readq(cpucp->rx_base + desc->enable_reg);
		val |= BIT(chan_id);
		writeq(val, cpucp->rx_base + desc->enable_reg);
	}

	return 0;
}

static void qcom_cpucp_mbox_shutdown(struct mbox_chan *chan)
{
	struct qcom_cpucp_mbox *cpucp = container_of(chan->mbox, struct qcom_cpucp_mbox, mbox);
	const struct qcom_cpucp_mbox_desc *desc = cpucp->desc;
	unsigned long chan_id = channel_number(chan);
	u64 val;

	if (desc->v2_mbox) {
		val = readq(cpucp->rx_base + desc->enable_reg);
		val &= ~BIT(chan_id);
		writeq(val, cpucp->rx_base + desc->enable_reg);
	}
}

static int qcom_cpucp_mbox_send_data(struct mbox_chan *chan, void *data)
{
	struct qcom_cpucp_mbox *cpucp = container_of(chan->mbox, struct qcom_cpucp_mbox, mbox);
	const struct qcom_cpucp_mbox_desc *desc = cpucp->desc;
	u32 val = desc->v2_mbox ? *(u32 *)data : APSS_CPUCP_V1_SEND_IRQ_VAL;
	unsigned long chan_id = channel_number(chan);
	u32 offset = desc->v2_mbox ? (chan_id * desc->chan_stride) : 0;

	writel(val, cpucp->tx_base + desc->tx_reg + offset);
	return 0;
}

static const struct mbox_chan_ops qcom_cpucp_mbox_chan_ops = {
	.startup = qcom_cpucp_mbox_startup,
	.send_data = qcom_cpucp_mbox_send_data,
	.shutdown = qcom_cpucp_mbox_shutdown
};

static int qcom_cpucp_mbox_probe(struct platform_device *pdev)
{
	const struct qcom_cpucp_mbox_desc *desc;
	struct device *dev = &pdev->dev;
	struct qcom_cpucp_mbox *cpucp;
	struct mbox_controller *mbox;
	struct resource *res;
	int irq, ret;

	desc = device_get_match_data(&pdev->dev);
	if (!desc)
		return -EINVAL;

	cpucp = devm_kzalloc(dev, sizeof(*cpucp), GFP_KERNEL);
	if (!cpucp)
		return -ENOMEM;

	cpucp->desc = desc;

	if (desc->v2_mbox) {
		cpucp->rx_base = devm_of_iomap(dev, dev->of_node, 0, NULL);
		if (IS_ERR(cpucp->rx_base))
			return PTR_ERR(cpucp->rx_base);
	/* Legacy mailbox quirks due to shared region with EPSS register space */
	} else {
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		if (!res) {
			dev_err(&pdev->dev, "Failed to get the device base address\n");
			return -ENODEV;
		}
		cpucp->rx_base = devm_ioremap(dev, res->start, resource_size(res));
		if (!cpucp->rx_base) {
			dev_err(dev, "Failed to ioremap the cpucp rx irq addr\n");
			return -ENOMEM;
		}
	}

	cpucp->tx_base = devm_of_iomap(dev, dev->of_node, 1, NULL);
	if (IS_ERR(cpucp->tx_base))
		return PTR_ERR(cpucp->tx_base);

	if (desc->v2_mbox) {
		writeq(0, cpucp->rx_base + desc->enable_reg);
		writeq(0, cpucp->rx_base + desc->clear_reg);
		writeq(0, cpucp->rx_base + desc->map_reg);
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_irq(dev, irq, desc->v2_mbox ? qcom_cpucp_v2_mbox_irq_fn :
		qcom_cpucp_mbox_irq_fn, IRQF_TRIGGER_HIGH, "apss_cpucp_mbox", cpucp);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to register irq: %d\n", irq);

	if (desc->v2_mbox)
		writeq(APSS_CPUCP_V2_RX_MBOX_CMD_MASK, cpucp->rx_base + desc->map_reg);

	mbox = &cpucp->mbox;
	mbox->dev = dev;
	mbox->num_chans = desc->num_chans;
	mbox->chans = cpucp->chans;
	mbox->ops = &qcom_cpucp_mbox_chan_ops;

	ret = devm_mbox_controller_register(dev, mbox);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to create mailbox\n");

	return 0;
}

static const struct qcom_cpucp_mbox_desc sc7280_cpucp_mbox = {
	.tx_reg = 0xC,
	.chan_stride = 0x1000,
	.status_reg = 0x30C,
	.clear_reg = 0x308,
	.v2_mbox = false,
	.num_chans = 2,
};

static const struct qcom_cpucp_mbox_desc x1e80100_cpucp_mbox = {
	.rx_reg = 0x104,
	.tx_reg = 0x104,
	.chan_stride = 0x8,
	.map_reg = 0x4000,
	.status_reg = 0x4400,
	.clear_reg = 0x4800,
	.enable_reg = 0x4C00,
	.v2_mbox = true,
	.num_chans = 3,
};

static const struct of_device_id qcom_cpucp_mbox_of_match[] = {
	{ .compatible = "qcom,x1e80100-cpucp-mbox", .data = &x1e80100_cpucp_mbox},
	{ .compatible = "qcom,sc7280-cpucp-mbox", .data = &sc7280_cpucp_mbox},
	{}
};
MODULE_DEVICE_TABLE(of, qcom_cpucp_mbox_of_match);

static struct platform_driver qcom_cpucp_mbox_driver = {
	.probe = qcom_cpucp_mbox_probe,
	.driver = {
		.name = "qcom_cpucp_mbox",
		.of_match_table = qcom_cpucp_mbox_of_match,
	},
};

static int __init qcom_cpucp_mbox_init(void)
{
	return platform_driver_register(&qcom_cpucp_mbox_driver);
}
core_initcall(qcom_cpucp_mbox_init);

static void __exit qcom_cpucp_mbox_exit(void)
{
	platform_driver_unregister(&qcom_cpucp_mbox_driver);
}
module_exit(qcom_cpucp_mbox_exit);

MODULE_DESCRIPTION("QTI CPUCP MBOX Driver");
MODULE_LICENSE("GPL");
