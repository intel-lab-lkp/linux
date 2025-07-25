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

static int irqmux_is_phandle_args_equal(const struct of_phandle_args *a,
					const struct of_phandle_args *b)
{
	int i;

	if (a->np != b->np)
		return false;

	if (a->args_count != b->args_count)
		return false;

	for (i = 0; i < a->args_count; i++) {
		if (a->args[i] != b->args[i])
			return false;
	}

	return true;
}

static int irqmux_find_interrupt_index(struct device *dev, struct device_node *np,
				       const struct of_phandle_args *expected_irq)
{
	struct of_phandle_args out_irq;
	bool is_equal;
	int ret;
	int i;

	for (i = 0; i < IRQMUX_MAX_IRQS; i++) {
		ret = of_irq_parse_one(np, i, &out_irq);
		if (ret)
			return ret;

		is_equal = irqmux_is_phandle_args_equal(expected_irq, &out_irq);
		of_node_put(out_irq.np);
		if (is_equal)
			return i;
	}

	return -ENOENT;
}

struct irqmux_cb_data {
	struct device_node *np;
	struct device *dev;
	u32 __iomem *regs;
};

static int irqmux_imap_cb(void *data, const __be32 *imap,
			  const struct of_phandle_args *parent_args)
{
	struct irqmux_cb_data *priv = data;
	u32 src_hwirq;
	int index;

	/*
	 * The child #address-cells is 0. Already checked in irqmux_setup().
	 * The first value in imap is the src_hwirq
	 */
	src_hwirq = be32_to_cpu(*imap);

	/*
	 * Get the index in our interrupt array that matches the parent in the
	 * interrupt-map
	 */
	index = irqmux_find_interrupt_index(priv->dev, priv->np, parent_args);
	if (index < 0)
		return dev_err_probe(priv->dev, index, "output interrupt not found\n");

	dev_info(priv->dev, "interrupt %u mapped to output interrupt[%u]\n",
		 src_hwirq, index);

	/*
	 * Our interrupt array items matches 1:1 the interrupt lines that could
	 * be configured by registers (same order, same number).
	 * Configure the related register with the src hwirq retrieved from the
	 * interrupt-map.
	 */
	writel(src_hwirq, priv->regs + index);

	return 0;
}

static int irqmux_setup(struct device *dev, struct device_node *np, u32 __iomem *regs)
{
	struct irqmux_cb_data cb_data;
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

	cb_data.dev = dev;
	cb_data.regs = regs;
	cb_data.np = np;
	return of_irq_foreach_imap(np, irqmux_imap_cb, &cb_data);
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
