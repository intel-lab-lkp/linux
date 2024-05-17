// SPDX-License-Identifier: GPL-2.0-only
/*
 * I2C interface driver for AD24xx A2B transceivers
 *
 * Copyright (c) 2023-2024 Alvin Šipraga <alsi@bang-olufsen.dk>
 */

#include <linux/a2b/a2b.h>
#include <linux/a2b/ad24xx.h>
#include <linux/clk.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>

struct ad24xx_i2c {
	struct device *dev;
	struct i2c_client *base_client;
	struct i2c_client *bus_client;
	struct regmap *base_regmap;
	struct regmap *bus_regmap;
	struct a2b_bus a2b_bus;
	struct mutex mutex;
	unsigned int irqs_enabled;
	struct irq_domain *irqdomain;
	int irq;
	struct clk *sync_clk;
};

#define to_ad24xx_i2c(iface) container_of(iface, struct ad24xx_i2c, a2b_bus)

static bool ad24xx_i2c_private_reg(unsigned int reg)
{
	/*
	 * "Private" registers which are owned by this interface driver should
	 * not be accessed by the constituent A2B drivers.
	 */
	switch (reg) {
	case A2B_CHIP:
	case A2B_NODEADR:
	case A2B_INTSRC:
	case A2B_INTTYPE:
		return true;
	default:
		return false;
	}
}

static int __ad24xx_i2c_read(struct a2b_bus *a2b_bus,
			     const struct a2b_node *node, unsigned int reg,
			     unsigned int *val)
{
	struct ad24xx_i2c *ad = to_ad24xx_i2c(a2b_bus);
	unsigned int nodeadr;
	int ret;

	if (ad24xx_i2c_private_reg(reg))
		return -EACCES;

	/* Main node access */
	if (is_a2b_main(node))
		return regmap_read(ad->base_regmap, reg, val);

	/* Sub node access */
	nodeadr = FIELD_PREP(A2B_NODEADR_NODE_MASK, node->addr - 1);

	ret = regmap_write(ad->base_regmap, A2B_NODEADR, nodeadr);
	if (ret)
		return ret;

	ret = regmap_read(ad->bus_regmap, reg, val);
	if (ret)
		return ret;

	return 0;
}

static int ad24xx_i2c_read(struct a2b_bus *a2b_bus, const struct a2b_node *node,
			   unsigned int reg, unsigned int *val)
{
	struct ad24xx_i2c *ad = to_ad24xx_i2c(a2b_bus);
	int ret;

	mutex_lock(&ad->mutex);
	ret = __ad24xx_i2c_read(a2b_bus, node, reg, val);
	mutex_unlock(&ad->mutex);
	return ret;
}

static int __ad24xx_i2c_write(struct a2b_bus *a2b_bus,
			      const struct a2b_node *node, unsigned int reg,
			      unsigned int val)
{
	struct ad24xx_i2c *ad = to_ad24xx_i2c(a2b_bus);
	unsigned int nodeadr;
	int ret;

	if (ad24xx_i2c_private_reg(reg))
		return -EACCES;

	/* Main node access */
	if (is_a2b_main(node))
		return regmap_write(ad->base_regmap, reg, val);

	/* Sub node access */
	nodeadr = FIELD_PREP(A2B_NODEADR_NODE_MASK, node->addr - 1);

	ret = regmap_write(ad->base_regmap, A2B_NODEADR, nodeadr);
	if (ret)
		return ret;

	ret = regmap_write(ad->bus_regmap, reg, val);
	if (ret)
		return ret;

	return 0;
}

static int ad24xx_i2c_write(struct a2b_bus *a2b_bus,
			    const struct a2b_node *node, unsigned int reg,
			    unsigned int val)
{
	struct ad24xx_i2c *ad = to_ad24xx_i2c(a2b_bus);
	int ret;

	mutex_lock(&ad->mutex);
	ret = __ad24xx_i2c_write(a2b_bus, node, reg, val);
	mutex_unlock(&ad->mutex);
	return ret;
}

static int ad24xx_i2c_xfer(struct a2b_bus *a2b_bus, const struct a2b_node *node,
			   struct i2c_msg *msgs, int num)
{
	struct ad24xx_i2c *ad = to_ad24xx_i2c(a2b_bus);
	struct i2c_msg msgs2[2];
	unsigned int nodeadr;
	int ret;
	int i;

	/* Mains only have one I2C interface and it operates in slave mode */
	if (is_a2b_main(node))
		return -EINVAL;

	/*
	 * Enforce some basic assumptions this function makes about the
	 * transfer. If this proves insufficient, some more complex logic will
	 * be needed.
	 */
	if (num > 2 || (num == 2 && msgs[0].addr != msgs[1].addr))
		return -EOPNOTSUPP;

	/* Modify the messages to use the I2C address of the BUS client */
	for (i = 0; i < num; i++) {
		msgs2[i] = msgs[i];
		msgs2[i].addr = ad->bus_client->addr;
	}

	mutex_lock(&ad->mutex);

	/* Set I2C peripheral address in subordinate node */
	nodeadr = FIELD_PREP(A2B_NODEADR_NODE_MASK, node->addr - 1);

	ret = regmap_write(ad->base_regmap, A2B_NODEADR, nodeadr);
	if (ret)
		goto out;

	ret = regmap_write(ad->bus_regmap, A2B_CHIP, msgs[0].addr);
	if (ret)
		goto out;

	/* Set peripheral bit */
	nodeadr |= FIELD_PREP(A2B_NODEADR_PERI_MASK, 1);

	ret = regmap_write(ad->base_regmap, A2B_NODEADR, nodeadr);
	if (ret)
		goto out;

	ret = i2c_transfer(ad->bus_client->adapter, msgs2, num);
	if (ret < 0)
		goto out;

out:
	mutex_unlock(&ad->mutex);

	if (ret < 0)
		return ret;

	return num;
}

static int ad24xx_i2c_get_inttype(struct a2b_bus *a2b_bus,
				  unsigned int *val)
{
	struct ad24xx_i2c *ad = to_ad24xx_i2c(a2b_bus);
	int ret;

	mutex_lock(&ad->mutex);
	ret = regmap_read(ad->base_regmap, A2B_INTTYPE, val);
	mutex_unlock(&ad->mutex);

	return ret;
}

static struct clk *ad24xx_i2c_get_sync_clk(struct a2b_bus *a2b_bus)
{
	struct ad24xx_i2c *ad = to_ad24xx_i2c(a2b_bus);

	return ad->sync_clk;
}

struct a2b_bus_ops ad24xx_i2c_a2b_bus_ops = {
	.read = ad24xx_i2c_read,
	.write = ad24xx_i2c_write,
	.i2c_xfer = ad24xx_i2c_xfer,
	.get_inttype = ad24xx_i2c_get_inttype,
	.get_sync_clk = ad24xx_i2c_get_sync_clk,
};

static irqreturn_t ad24xx_i2c_irq_handler(int irq, void *data)
{
	struct ad24xx_i2c *ad = data;
	bool handled = false;
	unsigned long hwirq;
	unsigned int val;
	unsigned int virq;
	int ret;

	/*
	 * The transceiver asserts the IRQ line as long as there are pending
	 * interrupts. Process them all here so that the interrupt can be
	 * configured with an edge trigger.
	 */
	while (true) {
		mutex_lock(&ad->mutex);
		ret = regmap_read(ad->base_regmap, A2B_INTSRC, &val);
		mutex_unlock(&ad->mutex);
		if (ret) {
			dev_err_ratelimited(
				ad->dev,
				"failed to read interrupt source: %d\n", ret);
			break;
		}

		if (val & A2B_INTSRC_MSTINT_MASK)
			hwirq = 0;
		else if (val & A2B_INTSRC_SLVINT_MASK)
			hwirq = (val & A2B_INTSRC_INODE_MASK) + 1;
		else
			break;

		/*
		 * Pending interrupts are only cleared when reading the
		 * interrupt type. Normally this is done in the corresponding
		 * node's interrupt handler, but in case the interrupt is
		 * disabled, it has to be read here.
		 */
		if (!(BIT(hwirq) & ad->irqs_enabled)) {
			ret = ad24xx_i2c_get_inttype(&ad->a2b_bus, &val);
			if (ret)
				dev_err_ratelimited(
					ad->dev,
					"failed to read interrupt type: %d\n",
					ret);
			handled = true;
			continue;
		}

		virq = irq_find_mapping(ad->irqdomain, hwirq);
		if (!virq)
			break;

		handle_nested_irq(virq);
		handled = true;
	}

	return handled ? IRQ_HANDLED : IRQ_NONE;
}

static void ad24xx_i2c_irq_enable(struct irq_data *irq_data)
{
	struct ad24xx_i2c *ad = irq_data_get_irq_chip_data(irq_data);
	irq_hw_number_t hwirq = irq_data->hwirq;

	ad->irqs_enabled |= BIT(hwirq);
}

static void ad24xx_i2c_irq_disable(struct irq_data *irq_data)
{
	struct ad24xx_i2c *ad = irq_data_get_irq_chip_data(irq_data);
	irq_hw_number_t hwirq = irq_data->hwirq;

	ad->irqs_enabled &= ~BIT(hwirq);
}

static const struct irq_chip ad24xx_i2c_irq_chip = {
	.name = "ad24xx-i2c",
	.irq_enable = ad24xx_i2c_irq_enable,
	.irq_disable = ad24xx_i2c_irq_disable,
};

static int ad24xx_i2c_irqdomain_map(struct irq_domain *irqdomain,
				    unsigned int irq, irq_hw_number_t hwirq)
{
	irq_set_chip_data(irq, irqdomain->host_data);
	irq_set_chip_and_handler(irq, &ad24xx_i2c_irq_chip, handle_simple_irq);
	irq_set_nested_thread(irq, 1);
	irq_set_noprobe(irq);

	return 0;
}

static void ad24xx_i2c_irqdomain_unmap(struct irq_domain *irqdomain,
				       unsigned int irq)
{
	irq_set_nested_thread(irq, 0);
	irq_set_chip_and_handler(irq, NULL, NULL);
	irq_set_chip_data(irq, NULL);
}

static const struct irq_domain_ops ad24xx_i2c_irqdomain_ops = {
	.map = ad24xx_i2c_irqdomain_map,
	.unmap = ad24xx_i2c_irqdomain_unmap,
	.xlate = irq_domain_xlate_onecell,
};

static void devm_ad24xx_i2c_release_irqdomain(void *data)
{
	struct irq_domain *irqdomain = data;
	int virq;
	int i;

	for (i = 0; i < A2B_MAX_NODES; i++) {
		virq = irq_find_mapping(irqdomain, i);
		if (virq)
			irq_dispose_mapping(virq);
	}

	irq_domain_remove(irqdomain);
}

static int ad24xx_i2c_irq_setup(struct ad24xx_i2c *ad)
{
	u32 intsize;
	int ret;

	if (!of_property_read_bool(ad->dev->of_node, "interrupt-controller") ||
	    of_property_read_u32(ad->dev->of_node, "#interrupt-cells",
				 &intsize) ||
	    intsize != 1)
		return -EINVAL;

	ad->irqdomain = irq_domain_add_linear(ad->dev->of_node, A2B_MAX_NODES,
					      &ad24xx_i2c_irqdomain_ops, ad);
	if (!ad->irqdomain)
		return -ENOMEM;

	ret = devm_add_action_or_reset(
		ad->dev, devm_ad24xx_i2c_release_irqdomain, ad->irqdomain);
	if (ret)
		return ret;

	ret = devm_request_threaded_irq(ad->dev, ad->irq, NULL,
					ad24xx_i2c_irq_handler, IRQF_ONESHOT,
					"ad24xx-i2c", ad);
	if (ret)
		return ret;

	return 0;
}

static int ad24xx_i2c_bus_setup(struct ad24xx_i2c *ad)
{
	struct device *dev = ad->dev;
	unsigned long sff_rate;
	int ret;

	ad->a2b_bus.ops = &ad24xx_i2c_a2b_bus_ops;
	ad->a2b_bus.parent = dev;
	ad->a2b_bus.priv = ad;

	sff_rate = clk_get_rate(ad->sync_clk);
	if (sff_rate == 48000)
		ad->a2b_bus.sff = A2B_SFF_48000;
	else if (sff_rate == 44100)
		ad->a2b_bus.sff = A2B_SFF_44100;
	else
		return -EINVAL;

	ret = a2b_register_bus(&ad->a2b_bus);
	if (ret)
		return ret;

	return 0;
}

static const struct regmap_config ad24xx_i2c_base_regmap_config = {
	.disable_locking = true,
	.reg_bits = 8,
	.val_bits = 8,
	.reg_stride = 1,
	.max_register = A2B_REG_MAX,
};

static const struct regmap_config ad24xx_i2c_bus_regmap_config = {
	.disable_locking = true,
	.reg_bits = 8,
	.val_bits = 8,
	.reg_stride = 1,
	.max_register = A2B_REG_MAX,
};

static void ad24xx_i2c_remove(struct i2c_client *client)
{
	struct ad24xx_i2c *ad = i2c_get_clientdata(client);

	a2b_unregister_bus(&ad->a2b_bus);
}

static int ad24xx_i2c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct device_node *np;
	struct ad24xx_i2c *ad;
	struct regmap_config *base_regmap_config;
	struct regmap_config *bus_regmap_config;
	u32 bus_addr;
	int ret;
	int i;

	ad = devm_kzalloc(dev, sizeof(*ad), GFP_KERNEL);
	if (!ad)
		return -ENOMEM;

	base_regmap_config = devm_kmemdup(dev, &ad24xx_i2c_base_regmap_config,
					  sizeof(*base_regmap_config),
					  GFP_KERNEL);
	if (!base_regmap_config)
		return -ENOMEM;

	bus_regmap_config = devm_kmemdup(dev, &ad24xx_i2c_bus_regmap_config,
					  sizeof(*bus_regmap_config),
					  GFP_KERNEL);
	if (!bus_regmap_config)
		return -ENOMEM;

	i2c_set_clientdata(client, ad);
	ad->dev = dev;
	ad->irq = client->irq;
	ad->base_client = client;
	mutex_init(&ad->mutex);

	/* Optionally enable regulators for VIN or for out-of-band bus power */
	ret = devm_regulator_get_enable_optional(dev, "vin");
	if (ret && ret != -ENODEV)
		return ret;

	ret = devm_regulator_get_enable_optional(dev, "bus");
	if (ret && ret != -ENODEV)
		return ret;

	ad->base_regmap =
		devm_regmap_init_i2c(ad->base_client, base_regmap_config);
	if (IS_ERR(ad->base_regmap))
		return PTR_ERR(ad->base_regmap);

	np = client->dev.of_node;
	if (!np)
		return -EINVAL;

	i = of_property_match_string(np, "reg-names", "bus");
	if (i < 0)
		return -EINVAL;

	ret = of_property_read_u32_index(np, "reg", i, &bus_addr);
	if (ret)
		return ret;

	ad->bus_client =
		devm_i2c_new_dummy_device(dev, client->adapter, bus_addr);
	if (IS_ERR(ad->bus_client))
		return PTR_ERR(ad->bus_client);

	ad->bus_regmap =
		devm_regmap_init_i2c(ad->bus_client, bus_regmap_config);
	if (IS_ERR(ad->bus_regmap))
		return PTR_ERR(ad->bus_regmap);

	ad->sync_clk = devm_clk_get_enabled(dev, "sync");
	if (IS_ERR(ad->sync_clk))
		return PTR_ERR(ad->sync_clk);

	ret = ad24xx_i2c_irq_setup(ad);
	if (ret)
		return ret;

	ret = ad24xx_i2c_bus_setup(ad);
	if (ret)
		return ret;

	return 0;
}

static const struct of_device_id ad24xx_i2c_of_match_table[] = {
	{ .compatible = "adi,ad2403" },
	{ .compatible = "adi,ad2410" },
	{ .compatible = "adi,ad2425" },
	{ .compatible = "adi,ad2428" },
	{ .compatible = "adi,ad2429" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ad24xx_i2c_of_match_table);

static const struct i2c_device_id ad24xx_i2c_id_table[] = {
	{ .name = "ad24xx", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, ad24xx_i2c_id_table);

static struct i2c_driver ad24xx_i2c_driver = {
	.driver = {
		.name = "ad24xx-i2c",
		.of_match_table = ad24xx_i2c_of_match_table,
	},
	.probe = ad24xx_i2c_probe,
	.remove = ad24xx_i2c_remove,
	.id_table = ad24xx_i2c_id_table,
};
module_i2c_driver(ad24xx_i2c_driver);

MODULE_AUTHOR("Alvin Šipraga <alsi@bang-olufsen.dk>");
MODULE_DESCRIPTION("AD24xx I2C driver");
MODULE_LICENSE("GPL");
