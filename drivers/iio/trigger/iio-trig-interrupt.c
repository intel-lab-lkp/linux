// SPDX-License-Identifier: GPL-2.0-only
/*
 * Industrial I/O - generic interrupt based trigger support
 *
 * Copyright (c) 2008-2013 Jonathan Cameron
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>

#include <linux/iio/iio.h>
#include <linux/iio/trigger.h>


static irqreturn_t iio_interrupt_trigger_poll(int irq, void *private)
{
	iio_trigger_poll(private);
	return IRQ_HANDLED;
}

static int iio_interrupt_trigger_probe(struct platform_device *pdev)
{
	struct iio_trigger *trig;
	unsigned long irqflags;
	struct resource *irq_res;
	int irq, ret;

	irq_res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);

	if (irq_res == NULL)
		return -ENODEV;

	irqflags = (irq_res->flags & IRQF_TRIGGER_MASK) | IRQF_SHARED;

	irq = irq_res->start;

	trig = devm_iio_trigger_alloc(&pdev->dev, "irqtrig%d", irq);
	if (!trig)
		return -ENOMEM;

	ret = devm_request_irq(&pdev->dev, irq, iio_interrupt_trigger_poll,
			       irqflags, trig->name, trig);
	if (ret) {
		dev_err(&pdev->dev, "request IRQ-%d failed", irq);
		return ret;
	}

	return devm_iio_trigger_register(&pdev->dev, trig);
}

static struct platform_driver iio_interrupt_trigger_driver = {
	.probe = iio_interrupt_trigger_probe,
	.driver = {
		.name = "iio_interrupt_trigger",
	},
};

module_platform_driver(iio_interrupt_trigger_driver);

MODULE_AUTHOR("Jonathan Cameron <jic23@kernel.org>");
MODULE_DESCRIPTION("Interrupt trigger for the iio subsystem");
MODULE_LICENSE("GPL v2");
