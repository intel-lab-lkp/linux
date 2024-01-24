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
#include <linux/pscrr.h>

struct pscrr_nvmem {
	struct pscrr_device pscrr_dev;
	struct nvmem_cell *cell;
	size_t max_magic_bytes;
};

static int pscrr_nvmem_write(struct pscrr_device *pscrr_dev, u32 magic)
{
	struct pscrr_nvmem *priv = container_of(pscrr_dev, struct pscrr_nvmem,
						pscrr_dev);
	size_t size = min(priv->max_magic_bytes, sizeof(magic));
	int ret;

	ret = nvmem_cell_write(priv->cell, &magic, size);
	if (ret < 0) {
		dev_err(pscrr_dev->dev, "update reason bits failed: %pe\n",
			ERR_PTR(ret));
		return ret;
	}

	return 0;
}

static int pscrr_nvmem_read(struct pscrr_device *pscrr_dev, u32 *magic)
{
	struct pscrr_nvmem *priv = container_of(pscrr_dev, struct pscrr_nvmem,
						pscrr_dev);
	size_t len;
	void *buf;

	buf = nvmem_cell_read(priv->cell, &len);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	*magic = 0;
	memcpy(magic, buf, min(len, sizeof(*magic)));
	kfree(buf);

	return 0;
}

static int pscrr_nvmem_probe(struct platform_device *pdev)
{
	size_t bytes, bits, magic_bits;
	struct pscrr_nvmem *priv;
	const char *pscr = "pscr";
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->pscrr_dev.dev = &pdev->dev;
	priv->pscrr_dev.write = pscrr_nvmem_write;
	priv->pscrr_dev.read = pscrr_nvmem_read;

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

	ret = devm_pscrr_register(&pdev->dev, &priv->pscrr_dev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to register pscr driver\n");

	magic_bits = fls(priv->pscrr_dev.max_magic_val);
	priv->max_magic_bytes = DIV_ROUND_UP(magic_bits, 8);

	if (!bits)
		bits = bytes * 8;

	if (magic_bits > bits)
		return dev_err_probe(&pdev->dev, -EINVAL, "provided magic can't fit into nvmem %s. bytes: %zu, bits: %zu, magic_bits: %zu\n",
				     pscr, bytes, bits, magic_bits);

	return handle_last_pscr(&priv->pscrr_dev);
}

static const struct of_device_id pscrr_nvmem_of_match[] = {
	{ .compatible = "pscrr-nvmem" },
	{}
};
MODULE_DEVICE_TABLE(of, pscrr_nvmem_of_match);

static struct platform_driver pscrr_nvmem_driver = {
	.probe = pscrr_nvmem_probe,
	.driver = {
		.name = "pscrr-nvmem",
		.of_match_table = pscrr_nvmem_of_match,
	},
};
module_platform_driver(pscrr_nvmem_driver);

MODULE_AUTHOR("Oleksij Rempel <o.rempel@pengutronix.de>");
MODULE_DESCRIPTION("NVMEM Driver for Power State Change Reason Recording");
MODULE_LICENSE("GPL");
