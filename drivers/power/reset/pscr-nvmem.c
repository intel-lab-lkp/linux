// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Vaisala Oyj. All rights reserved.
// Copyright (c) 2024 Pengutronix, Oleksij Rempel <kernel@pengutronix.de>
/*
 * Based on drivers/power/reset/nvmem-reboot-mode.c
 * Copyright (c) Vaisala Oyj. All rights reserved.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pscr.h>

struct pscr_nvmem {
	struct pscr_driver pscr_drv;
	struct nvmem_cell *cell;
	size_t max_magic_bytes;
};

static int pscr_nvmem_write(struct pscr_driver *pscr_drv, u32 magic)
{
	struct pscr_nvmem *priv = container_of(pscr_drv, struct pscr_nvmem,
					       pscr_drv);
	size_t size = min(priv->max_magic_bytes, sizeof(magic));
	int ret;

	ret = nvmem_cell_write(priv->cell, &magic, size);
	if (ret < 0)
		dev_err(pscr_drv->dev, "update reason bits failed: %pe\n",
			ERR_PTR(ret));

	return ret;
}

static int pscr_nvmem_probe(struct platform_device *pdev)
{
	const char *pscr = "pscr";
	struct pscr_nvmem *priv;
	size_t bytes, bits, magic_bits;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->pscr_drv.dev = &pdev->dev;
	priv->pscr_drv.write = pscr_nvmem_write;

	priv->cell = devm_nvmem_cell_get(&pdev->dev, pscr);
	if (IS_ERR(priv->cell))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->cell),
				     "failed to get the nvmem %s cell\n", pscr);

	ret = nvmem_cell_get_size(priv->cell, &bytes, &bits);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret, "failed to get the nvmem %s size\n",
				     pscr);

	if (!bytes || bytes > sizeof(u32) || bits > 32)
		return dev_err_probe(&pdev->dev, -EINVAL, "invalid nvmem %s size. bytes: %zu, bits: %zu\n",
				     pscr, bytes, bits);

	ret = devm_pscr_register(&pdev->dev, &priv->pscr_drv);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to register pscr driver\n");

	magic_bits = fls(priv->pscr_drv.max_magic);
	priv->max_magic_bytes = DIV_ROUND_UP(magic_bits, 8);

	if (!bits)
		bits = bytes * 8;

	if (magic_bits > bits)
		return dev_err_probe(&pdev->dev, -EINVAL, "provided magic can't fit into nvmem %s. bytes: %zu, bits: %zu, magic_bits: %zu\n",
				     pscr, bytes, bits, magic_bits); 

	return ret;
}

static const struct of_device_id pscr_nvmem_of_match[] = {
	{ .compatible = "pscr-nvmem" },
	{}
};
MODULE_DEVICE_TABLE(of, pscr_nvmem_of_match);

static struct platform_driver pscr_nvmem_driver = {
	.probe = pscr_nvmem_probe,
	.driver = {
		.name = "pscr-nvmem",
		.of_match_table = pscr_nvmem_of_match,
	},
};
module_platform_driver(pscr_nvmem_driver);

MODULE_AUTHOR("Oleksij Rempel <o.rempel@pengutronix.de>");
MODULE_DESCRIPTION("NVMEM Driver for Power State Change Reason Tracking");
MODULE_LICENSE("GPL v2");
