// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2021 Rafał Miłecki <rafal@milecki.pl>
 */

#include <linux/bcm47xx_nvram.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define NVRAM_MAGIC			"FLSH"

struct brcm_nvram {
	struct device *dev;
	uint8_t *data;
	size_t size;
	struct nvmem_cell_info *cells;
	int ncells;
};

struct brcm_nvram_header {
	char magic[4];
	__le32 len;
	__le32 crc_ver_init;	/* 0:7 crc, 8:15 ver, 16:31 sdram_init */
	__le32 config_refresh;	/* 0:15 sdram_config, 16:31 sdram_refresh */
	__le32 config_ncdl;	/* ncdl values for memc */
};

static int brcm_nvram_read(void *context, unsigned int offset, void *val,
			   size_t bytes)
{
	struct brcm_nvram *priv = context;

	memcpy(val, priv->data + offset, bytes);

	return 0;
}

static int brcm_nvram_read_post_process_macaddr(void *context, const char *id, int index,
						unsigned int offset, void *buf, size_t bytes)
{
	u8 mac[ETH_ALEN];

	if (bytes != 3 * ETH_ALEN - 1)
		return -EINVAL;

	if (!mac_pton(buf, mac))
		return -EINVAL;

	if (index)
		eth_addr_add(mac, index);

	ether_addr_copy(buf, mac);

	return 0;
}

static int brcm_nvram_add_cells(struct brcm_nvram *priv, uint8_t *data,
				size_t len)
{
	struct device *dev = priv->dev;
	char *var, *value, *eq;
	uint8_t tmp;
	int idx;
	int err = 0;

	tmp = priv->data[len - 1];
	priv->data[len - 1] = '\0';

	priv->ncells = 0;
	for (var = data + sizeof(struct brcm_nvram_header);
	     var < (char *)data + len && *var;
	     var += strlen(var) + 1) {
		priv->ncells++;
	}

	priv->cells = devm_kcalloc(dev, priv->ncells, sizeof(*priv->cells), GFP_KERNEL);
	if (!priv->cells) {
		err = -ENOMEM;
		goto out;
	}

	for (var = data + sizeof(struct brcm_nvram_header), idx = 0;
	     var < (char *)data + len && *var;
	     var = value + strlen(value) + 1, idx++) {
		eq = strchr(var, '=');
		if (!eq)
			break;
		*eq = '\0';
		value = eq + 1;

		priv->cells[idx].name = devm_kstrdup(dev, var, GFP_KERNEL);
		if (!priv->cells[idx].name) {
			err = -ENOMEM;
			goto out;
		}
		priv->cells[idx].offset = value - (char *)data;
		priv->cells[idx].bytes = strlen(value);
		priv->cells[idx].np = of_get_child_by_name(dev->of_node, priv->cells[idx].name);
		if (!strcmp(var, "et0macaddr") ||
		    !strcmp(var, "et1macaddr") ||
		    !strcmp(var, "et2macaddr")) {
			priv->cells[idx].raw_len = strlen(value);
			priv->cells[idx].bytes = ETH_ALEN;
			priv->cells[idx].read_post_process = brcm_nvram_read_post_process_macaddr;
		}
	}

out:
	priv->data[len - 1] = tmp;
	return err;
}

static int brcm_nvram_parse(struct brcm_nvram *priv)
{
	struct brcm_nvram_header *header = (struct brcm_nvram_header *)priv->data;
	struct device *dev = priv->dev;
	size_t len;
	int err;

	if (memcmp(header->magic, NVRAM_MAGIC, 4)) {
		dev_err(dev, "Invalid NVRAM magic\n");
		return -EINVAL;
	}

	len = le32_to_cpu(header->len);
	if (len > priv->size) {
		dev_err(dev, "NVRAM length (%zd) exceeds mapped size (%zd)\n", len, priv->size);
		return -EINVAL;
	}

	err = brcm_nvram_add_cells(priv, priv->data, len);
	if (err)
		dev_err(dev, "Failed to add cells: %d\n", err);

	return 0;
}

static int brcm_nvram_probe(struct platform_device *pdev)
{
	struct nvmem_config config = {
		.name = "brcm-nvram",
		.reg_read = brcm_nvram_read,
	};
	struct device *dev = &pdev->dev;
	struct brcm_nvram *priv;
	struct resource *res;
	void __iomem *base;
	int err;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dev = dev;

	base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	priv->size = resource_size(res);

	priv->data = devm_kzalloc(dev, priv->size, GFP_KERNEL);
	if (!priv->data)
		return -ENOMEM;

	memcpy_fromio(priv->data, base, priv->size);

	err = brcm_nvram_parse(priv);
	if (err)
		return err;

	bcm47xx_nvram_init_from_iomem(base, priv->size);

	config.dev = dev;
	config.cells = priv->cells;
	config.ncells = priv->ncells;
	config.priv = priv;
	config.size = priv->size;

	return PTR_ERR_OR_ZERO(devm_nvmem_register(dev, &config));
}

static const struct of_device_id brcm_nvram_of_match_table[] = {
	{ .compatible = "brcm,nvram", },
	{},
};

static struct platform_driver brcm_nvram_driver = {
	.probe = brcm_nvram_probe,
	.driver = {
		.name = "brcm_nvram",
		.of_match_table = brcm_nvram_of_match_table,
	},
};

static int __init brcm_nvram_init(void)
{
	return platform_driver_register(&brcm_nvram_driver);
}

subsys_initcall_sync(brcm_nvram_init);

MODULE_AUTHOR("Rafał Miłecki");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(of, brcm_nvram_of_match_table);
