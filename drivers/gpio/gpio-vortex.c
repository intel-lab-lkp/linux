// SPDX-License-Identifier: GPL-2.0-only
/*
 *  GPIO driver for Vortex86 SoCs
 *
 *  Author: Marcos Del Sol Vives <marcos@orca.pet>
 */

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/ioport.h>
#include <linux/spinlock.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/regmap.h>
#include <linux/regmap.h>
#include <linux/ioport.h>
#include <linux/types.h>
#include <linux/platform_device.h>

#define DAT_RANGE 0
#define DIR_RANGE 1

struct vortex_gpio {
	struct regmap_range ranges[2];
	struct regmap_access_table access_table;
};

static int vortex_gpio_probe(struct platform_device *pdev)
{
	struct gpio_regmap_config gpiocfg = {};
	struct resource *dat_res, *dir_res;
	struct device *dev = &pdev->dev;
	struct regmap_config rmcfg = {};
	unsigned long io_start, io_end;
	struct vortex_gpio *priv;
	struct regmap *map;
	void __iomem *regs;

	dat_res = platform_get_resource_byname(pdev, IORESOURCE_IO, "dat");
	if (unlikely(!dat_res)) {
		dev_err(dev, "failed to get data register\n");
		return -ENODEV;
	}

	dir_res = platform_get_resource_byname(pdev, IORESOURCE_IO, "dir");
	if (unlikely(!dir_res)) {
		dev_err(dev, "failed to get direction register\n");
		return -ENODEV;
	}

	if (unlikely(resource_size(dat_res) != resource_size(dir_res))) {
		dev_err(dev, "data and direction size mismatch\n");
		return -EINVAL;
	}

	priv = devm_kzalloc(&pdev->dev, sizeof(struct vortex_gpio),
			    GFP_KERNEL);
	if (unlikely(!priv))
		return -ENOMEM;
	pdev->dev.driver_data = priv;

	/* Map an I/O window that covers both data and direction */
	io_start = min(dat_res->start, dir_res->start);
	io_end = max(dat_res->end, dir_res->end);
	regs = devm_ioport_map(dev, io_start, io_end - io_start + 1);
	if (unlikely(!regs))
		return -ENOMEM;

	/* Dynamically build access table from gpiocfg */
	priv->ranges[DAT_RANGE].range_min = dat_res->start - io_start;
	priv->ranges[DAT_RANGE].range_max = dat_res->end - io_start;
	priv->ranges[DIR_RANGE].range_min = dir_res->start - io_start;
	priv->ranges[DIR_RANGE].range_max = dir_res->end - io_start;
	priv->access_table.n_yes_ranges = ARRAY_SIZE(priv->ranges);
	priv->access_table.yes_ranges = priv->ranges;

	rmcfg.reg_bits = 8;
	rmcfg.val_bits = 8;
	rmcfg.io_port = true;
	rmcfg.wr_table = &priv->access_table;
	rmcfg.rd_table = &priv->access_table;

	map = devm_regmap_init_mmio(dev, regs, &rmcfg);
	if (unlikely(IS_ERR(map)))
		return dev_err_probe(dev, PTR_ERR(map),
				     "Unable to initialize register map\n");

	gpiocfg.parent = dev;
	gpiocfg.regmap = map;
	gpiocfg.ngpio = 8 * resource_size(dat_res);
	gpiocfg.ngpio_per_reg = 8;
	gpiocfg.reg_dat_base = GPIO_REGMAP_ADDR(priv->ranges[DAT_RANGE].range_min);
	gpiocfg.reg_set_base = GPIO_REGMAP_ADDR(priv->ranges[DAT_RANGE].range_min);
	gpiocfg.reg_dir_out_base = GPIO_REGMAP_ADDR(priv->ranges[DIR_RANGE].range_min);
	gpiocfg.flags = GPIO_REGMAP_DIR_BEFORE_SET;

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
