// SPDX-License-Identifier: GPL-2.0-only
/*
 * GPIO driver for Vortex86 SoCs
 *
 * Vortex SoCs may have either a single set (DX/MX/SX) of data/direction ports,
 * or two non-contiguous sets (DX2/DX3).
 *
 * Because gpio-regmap is not designed to handle ranges with holes of arbitrary
 * sizes in them, this driver reports a virtual layout where ports 0..n-1 are
 * data ports, and n..n*2-1 are direction ports. The xlate function then maps
 * these virtual ports back to the real hardware registers relative to the
 * requested I/O window.
 *
 * Author: Marcos Del Sol Vives <marcos@orca.pet>
 */

#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/gpio/regmap.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/types.h>

struct vortex_gpio {
	struct regmap_range ranges[4];
	struct regmap_access_table access_table;
	struct device *dev;
};

static int vortex_gpio_xlate(struct gpio_regmap *gpio, unsigned int base,
			     unsigned int offset, unsigned int *reg,
			     unsigned int *mask)
{
	struct vortex_gpio *priv = gpio_regmap_get_drvdata(gpio);
	unsigned int virtual_port, r_min, r_size;
	int i;

	virtual_port = base + offset / 8;

	for (i = 0; i < priv->access_table.n_yes_ranges; i++) {
		r_min = priv->ranges[i].range_min;
		r_size = priv->ranges[i].range_max - r_min + 1;

		if (virtual_port < r_size) {
			*reg = virtual_port + r_min;
			*mask = BIT(offset % 8);
			return 0;
		}
		virtual_port -= r_size;
	}

	/* should never happen */
	dev_err(priv->dev, "tried to translate an out-of-bounds virtual port: %u\n",
		base + offset / 8);
	return -EINVAL;
}

static int vortex_gpio_add_range(struct vortex_gpio *priv,
				 struct platform_device *pdev,
				 const char *res_name)
{
	struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_IO, res_name);
	if (!res)
		return 0;

	priv->ranges[priv->access_table.n_yes_ranges].range_min = res->start;
	priv->ranges[priv->access_table.n_yes_ranges].range_max = res->end;
	priv->access_table.n_yes_ranges++;

	return resource_size(res);
}

static int vortex_gpio_probe(struct platform_device *pdev)
{
	struct gpio_regmap_config gpiocfg = {};
	struct device *dev = &pdev->dev;
	struct regmap_config rmcfg = {};
	unsigned long io_min, io_max;
	struct vortex_gpio *priv;
	int i, dat_len, dir_len;
	struct regmap *map;
	void __iomem *regs;

	/* Initialize private data */
	priv = devm_kzalloc(dev, sizeof(struct vortex_gpio),
			    GFP_KERNEL);
	if (unlikely(!priv))
		return -ENOMEM;
	priv->dev = dev;
	priv->access_table.yes_ranges = priv->ranges;

	/* Add I/O ports from platform data to ranges */
	dat_len = vortex_gpio_add_range(priv, pdev, "dat1");
	if (unlikely(!dat_len)) {
		dev_err(dev, "failed to get data register\n");
		return -ENODEV;
	}
	dat_len += vortex_gpio_add_range(priv, pdev, "dat2");

	dir_len = vortex_gpio_add_range(priv, pdev, "dir1");
	if (unlikely(!dir_len)) {
		dev_err(dev, "failed to get direction register\n");
		return -ENODEV;
	}
	dir_len += vortex_gpio_add_range(priv, pdev, "dir2");

	if (unlikely(dat_len != dir_len)) {
		dev_err(dev, "data and direction size mismatch (%d vs %d)\n",
			dat_len, dir_len);
		return -EINVAL;
	}

	/* Request smallest I/O window that covers all registers */
	io_min = priv->ranges[0].range_min;
	io_max = priv->ranges[0].range_max;
	for (i = 1; i < priv->access_table.n_yes_ranges; i++) {
		io_min = min(io_min, priv->ranges[i].range_min);
		io_max = max(io_max, priv->ranges[i].range_max);
	}

	regs = devm_ioport_map(dev, io_min, io_max - io_min + 1);
	if (unlikely(!regs))
		return -ENOMEM;

	/* Subtract io_min to make them relative to the window */
	for (i = 0; i < priv->access_table.n_yes_ranges; i++) {
		priv->ranges[i].range_min -= io_min;
		priv->ranges[i].range_max -= io_min;
	}

	rmcfg.reg_bits = 8;
	rmcfg.val_bits = 8;
	rmcfg.io_port = true;
	rmcfg.wr_table = &priv->access_table;
	rmcfg.rd_table = &priv->access_table;

	map = devm_regmap_init_mmio(dev, regs, &rmcfg);
	if (IS_ERR(map))
		return dev_err_probe(dev, PTR_ERR(map),
				     "Unable to initialize register map\n");

	gpiocfg.parent = dev;
	gpiocfg.regmap = map;
	gpiocfg.drvdata = priv;
	gpiocfg.ngpio = 8 * dat_len;
	gpiocfg.ngpio_per_reg = 8;
	gpiocfg.reg_dat_base = GPIO_REGMAP_ADDR(0);
	gpiocfg.reg_set_base = GPIO_REGMAP_ADDR(0);
	gpiocfg.reg_dir_out_base = GPIO_REGMAP_ADDR(dat_len);
	gpiocfg.flags = GPIO_REGMAP_DIR_BEFORE_SET;
	gpiocfg.reg_mask_xlate = vortex_gpio_xlate;

	return PTR_ERR_OR_ZERO(devm_gpio_regmap_register(dev, &gpiocfg));
}

static struct platform_driver vortex_gpio_driver = {
	.driver.name = "vortex-gpio",
	.probe = vortex_gpio_probe,
};

module_platform_driver(vortex_gpio_driver);

MODULE_AUTHOR("Marcos Del Sol Vives <marcos@orca.pet>");
MODULE_DESCRIPTION("GPIO driver for Vortex86 SoCs");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:vortex-gpio");
