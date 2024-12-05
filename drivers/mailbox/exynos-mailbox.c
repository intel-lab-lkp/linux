// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2020 Samsung Electronics Co., Ltd.
 * Copyright 2020 Google LLC.
 * Copyright 2024 Linaro Ltd.
 */

#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/mailbox_controller.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define EXYNOS_MBOX_MCUCTRL		0x0	/* Mailbox Control Register */
#define EXYNOS_MBOX_INTCR0		0x24	/* Interrupt Clear Register 0 */
#define EXYNOS_MBOX_INTMR0		0x28	/* Interrupt Mask Register 0 */
#define EXYNOS_MBOX_INTSR0		0x2c	/* Interrupt Status Register 0 */
#define EXYNOS_MBOX_INTMSR0		0x30	/* Interrupt Mask Status Register 0 */
#define EXYNOS_MBOX_INTGR1		0x40	/* Interrupt Generation Register 1 */
#define EXYNOS_MBOX_INTMR1		0x48	/* Interrupt Mask Register 1 */
#define EXYNOS_MBOX_INTSR1		0x4c	/* Interrupt Status Register 1 */
#define EXYNOS_MBOX_INTMSR1		0x50	/* Interrupt Mask Status Register 1 */

#define EXYNOS_MBOX_INTMR0_MASK		GENMASK(15, 0)
#define EXYNOS_MBOX_INTGR1_MASK		GENMASK(15, 0)

#define EXYNOS_MBOX_CHAN_COUNT		HWEIGHT32(EXYNOS_MBOX_INTGR1_MASK)

/**
 * struct exynos_mbox - driver's private data.
 * @regs:	mailbox registers base address.
 * @mbox:	pointer to the mailbox controller.
 * @dev:	pointer to the mailbox device.
 * @pclk:	pointer to the mailbox peripheral clock.
 */
struct exynos_mbox {
	void __iomem *regs;
	struct mbox_controller *mbox;
	struct device *dev;
	struct clk *pclk;
};

static int exynos_mbox_chan_index(struct mbox_chan *chan)
{
	struct mbox_controller *mbox = chan->mbox;
	int i;

	for (i = 0; i < mbox->num_chans; i++)
		if (chan == &mbox->chans[i])
			return i;
	return -EINVAL;
}

static int exynos_mbox_send_data(struct mbox_chan *chan, void *data)
{
	struct exynos_mbox *exynos_mbox = dev_get_drvdata(chan->mbox->dev);
	int index;

	index = exynos_mbox_chan_index(chan);
	if (index < 0)
		return index;

	writel_relaxed(BIT(index), exynos_mbox->regs + EXYNOS_MBOX_INTGR1);

	return 0;
}

static const struct mbox_chan_ops exynos_mbox_chan_ops = {
	.send_data = exynos_mbox_send_data,
};

static const struct of_device_id exynos_mbox_match[] = {
	{ .compatible = "google,gs101-acpm-mbox" },
	{},
};
MODULE_DEVICE_TABLE(of, exynos_mbox_match);

static int exynos_mbox_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct exynos_mbox *exynos_mbox;
	struct mbox_controller *mbox;
	struct mbox_chan *chans;
	int i;

	exynos_mbox = devm_kzalloc(dev, sizeof(*exynos_mbox), GFP_KERNEL);
	if (!exynos_mbox)
		return -ENOMEM;

	mbox = devm_kzalloc(dev, sizeof(*mbox), GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;

	chans = devm_kcalloc(dev, EXYNOS_MBOX_CHAN_COUNT, sizeof(*chans),
			     GFP_KERNEL);
	if (!chans)
		return -ENOMEM;

	exynos_mbox->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(exynos_mbox->regs))
		return PTR_ERR(exynos_mbox->regs);

	exynos_mbox->pclk = devm_clk_get_enabled(dev, "pclk");
	if (IS_ERR(exynos_mbox->pclk))
		return dev_err_probe(dev, PTR_ERR(exynos_mbox->pclk),
				     "Failed to enable clock.\n");

	mbox->num_chans = EXYNOS_MBOX_CHAN_COUNT;
	mbox->chans = chans;
	mbox->dev = dev;
	mbox->ops = &exynos_mbox_chan_ops;

	for (i = 0; i < EXYNOS_MBOX_CHAN_COUNT; i++)
		chans[i].mbox = mbox;

	exynos_mbox->dev = dev;
	exynos_mbox->mbox = mbox;

	platform_set_drvdata(pdev, exynos_mbox);

	/* Mask out all interrupts. We support just polling channels for now. */
	writel_relaxed(EXYNOS_MBOX_INTMR0_MASK,
		       exynos_mbox->regs + EXYNOS_MBOX_INTMR0);

	return devm_mbox_controller_register(dev, mbox);
}

static struct platform_driver exynos_mbox_driver = {
	.probe	= exynos_mbox_probe,
	.driver	= {
		.name = "exynos-acpm-mbox",
		.of_match_table	= of_match_ptr(exynos_mbox_match),
	},
};
module_platform_driver(exynos_mbox_driver);

MODULE_AUTHOR("Tudor Ambarus <tudor.ambarus@linaro.org>");
MODULE_DESCRIPTION("Exynos mailbox driver");
MODULE_LICENSE("GPL");
