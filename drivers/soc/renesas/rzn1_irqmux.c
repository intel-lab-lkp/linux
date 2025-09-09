// SPDX-License-Identifier: GPL-2.0-only
/*
 * RZ/N1 GPIO Interrupt Multiplexer
 *
 * Copyright 2025 Schneider Electric
 * Author: Herve Codina <herve.codina@bootlin.com>
 */

#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>

#define IRQMUX_MAX_IRQS 8

static int irqmux_setup(struct device *dev, struct device_node *np, u32 __iomem *regs)
{
	struct of_imap_parser imap_parser;
	struct of_imap_item imap_item;
	unsigned int index = 0;
	u32 tmp;
	int ret;

	/* We support only #interrupt-cells = <1> and #address-cells = <0> */
	ret = of_property_read_u32(np, "#interrupt-cells", &tmp);
	if (ret)
		return ret;
	if (tmp != 1)
		return -EINVAL;

	ret = of_property_read_u32(np, "#address-cells", &tmp);
	if (ret)
		return ret;
	if (tmp != 0)
		return -EINVAL;

	ret = of_imap_parser_init(&imap_parser, np, &imap_item);
	if (ret)
		return ret;

	for_each_of_imap_item(&imap_parser, &imap_item) {
		/*
		 * The child #address-cells is 0 (already checked). The first
		 * value in imap item is the src hwirq.
		 *
		 * imap items matches 1:1 the interrupt lines that could
		 * be configured by registers (same order, same number).
		 * Configure the related register with the src hwirq retrieved
		 * from the interrupt-map.
		 */
		if (index > IRQMUX_MAX_IRQS) {
			of_node_put(imap_item.parent_args.np);
			dev_err(dev, "too much items in interrupt-map\n");
			return -EINVAL;
		}

		writel(imap_item.child_imap[0], regs + index);
		index++;
	}

	return 0;
}

static int irqmux_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	u32 __iomem *regs;
	int nr_irqs;
	int ret;

	regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	nr_irqs = of_irq_count(np);
	if (nr_irqs < 0)
		return nr_irqs;

	if (nr_irqs > IRQMUX_MAX_IRQS) {
		dev_err(dev, "too many output interrupts\n");
		return -ENOENT;
	}

	ret = irqmux_setup(dev, np, regs);
	if (ret)
		return dev_err_probe(dev, ret, "failed to setup mux\n");

	return 0;
}

static const struct of_device_id irqmux_of_match[] = {
	{ .compatible = "renesas,rzn1-gpioirqmux", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, irq_mux_of_match);

static struct platform_driver irqmux_driver = {
	.probe = irqmux_probe,
	.driver = {
		.name = "rzn1_irqmux",
		.of_match_table = irqmux_of_match,
	},
};
module_platform_driver(irqmux_driver);

MODULE_AUTHOR("Herve Codina <herve.codina@bootlin.com>");
MODULE_DESCRIPTION("Renesas RZ/N1 GPIO IRQ Multiplexer Driver");
MODULE_LICENSE("GPL");
