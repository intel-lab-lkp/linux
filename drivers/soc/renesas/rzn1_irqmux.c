// SPDX-License-Identifier: GPL-2.0-only
/*
 * RZ/N1 GPIO Interrupt Multiplexer
 *
 * Copyright 2025 Schneider Electric
 * Author: Herve Codina <herve.codina@bootlin.com>
 */

#include <linux/bitops.h>
#include <linux/build_bug.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <dt-bindings/interrupt-controller/arm-gic.h>

/*
 * The array index is the output line index, the value at the index is the
 * GIC SPI interrupt number the output line is connected to.
 */
static const u32 rzn1_irqmux_output_lines[] = {
	103, 104, 105, 106, 107, 108, 109, 110
};

static int rzn1_irqmux_parent_args_to_line_index(struct device *dev,
						 const struct of_phandle_args *parent_args,
						 const u32 output_lines[],
						 unsigned int output_lines_count)
{
	unsigned int i;

	/*
	 * The parent interrupt should be one of the GIC controller.
	 * Three arguments must be provided.
	 *  - args[0]: GIC_SPI
	 *  - args[1]: The GIC interrupt number
	 *  - args[2]: The interrupt flags
	 *
	 * We retrieve the line index based on the GIC interrupt number
	 * provided and rzn1_irqmux_output_line[] mapping array.
	 */

	if (parent_args->args_count != 3 ||
	    parent_args->args[0] != GIC_SPI) {
		dev_err(dev, "Invalid interrupt-map item\n");
		return -EINVAL;
	}

	for (i = 0; i < output_lines_count; i++) {
		if (parent_args->args[1] == output_lines[i])
			return i;
	}

	dev_err(dev, "Invalid GIC interrupt %u\n", parent_args->args[1]);
	return -EINVAL;
}

static int rzn1_irqmux_setup(struct device *dev, struct device_node *np, u32 __iomem *regs)
{
	struct of_imap_parser imap_parser;
	struct of_imap_item imap_item;
	unsigned long index_done = 0;
	int index;
	int ret;
	u32 tmp;

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

	/* 8 output lines are available */
	BUILD_BUG_ON(ARRAY_SIZE(rzn1_irqmux_output_lines) != 8);

	/*
	 * index_done is an unsigned long integer. Be sure that no buffer
	 * overflow can occur.
	 */
	BUILD_BUG_ON(ARRAY_SIZE(rzn1_irqmux_output_lines) > BITS_PER_LONG);

	for_each_of_imap_item(&imap_parser, &imap_item) {
		index = rzn1_irqmux_parent_args_to_line_index(dev,
							      &imap_item.parent_args,
							      rzn1_irqmux_output_lines,
							      ARRAY_SIZE(rzn1_irqmux_output_lines));
		if (index < 0) {
			of_node_put(imap_item.parent_args.np);
			return index;
		}

		if (test_and_set_bit(index, &index_done)) {
			of_node_put(imap_item.parent_args.np);
			dev_err(dev, "Mux output line already defined\n");
			return -EINVAL;
		}

		/*
		 * The child #address-cells is 0 (already checked). The first
		 * value in imap item is the src hwirq.
		 */
		writel(imap_item.child_imap[0], regs + index);
	}

	return 0;
}

static int rzn1_irqmux_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	u32 __iomem *regs;

	regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	return rzn1_irqmux_setup(dev, np, regs);
}

static const struct of_device_id rzn1_irqmux_of_match[] = {
	{ .compatible = "renesas,rzn1-gpioirqmux", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rzn1_irqmux_of_match);

static struct platform_driver rzn1_irqmux_driver = {
	.probe = rzn1_irqmux_probe,
	.driver = {
		.name = "rzn1_irqmux",
		.of_match_table = rzn1_irqmux_of_match,
	},
};
module_platform_driver(rzn1_irqmux_driver);

MODULE_AUTHOR("Herve Codina <herve.codina@bootlin.com>");
MODULE_DESCRIPTION("Renesas RZ/N1 GPIO IRQ Multiplexer Driver");
MODULE_LICENSE("GPL");
