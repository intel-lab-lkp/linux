// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2026 Bootlin
 *
 * Authors: Mathieu Dubois-Briand <mathieu.dubois-briand@bootlin.com>
 */

#include <linux/etherdevice.h>
#include <linux/hex.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>

#include "../internals.h"

/* Return the base address plus the offset supplied by the consumer. */
static int fixed_layout_read_mac_base(void *priv, const char *id, int index,
				      unsigned int offset, void *buf, size_t bytes)
{
	u8 mac[ETH_ALEN];
	char ascii[3 * ETH_ALEN];

	if (bytes == ETH_ALEN) {
		ether_addr_copy(mac, buf);
	} else if (bytes == sizeof(ascii) - 1) {
		memcpy(ascii, buf, bytes);
		ascii[bytes] = '\0';
		if (!mac_pton(ascii, mac))
			return -EINVAL;
	} else {
		return -EINVAL;
	}

	eth_addr_add(mac, index);
	ether_addr_copy(buf, mac);

	return 0;
}

static int fixed_layout_fixup_cell(struct nvmem_cell_info *info)
{
	if (!of_device_is_compatible(info->np, "mac-base"))
		return 0;

	/* The binding permits binary or colon-separated ASCII MAC addresses. */
	if ((info->bytes != ETH_ALEN && info->bytes != 3 * ETH_ALEN - 1) ||
	    info->nbits)
		return -EINVAL;

	info->raw_len = info->bytes;
	info->bytes = ETH_ALEN;
	info->read_post_process = fixed_layout_read_mac_base;

	return 0;
}

static int fixed_layout_add_cells(struct nvmem_layout *layout)
{
	struct device_node *np;
	int ret;

	np = of_nvmem_layout_get_container(layout->nvmem);
	if (!np)
		return -ENOENT;

	ret = nvmem_add_cells_from_dt(layout->nvmem, np, fixed_layout_fixup_cell);
	of_node_put(np);

	return ret;
}

static int fixed_layout_probe(struct nvmem_layout *layout)
{
	layout->add_cells = fixed_layout_add_cells;

	return nvmem_layout_register(layout);
}

static void fixed_layout_remove(struct nvmem_layout *layout)
{
	nvmem_layout_unregister(layout);
}

static const struct of_device_id fixed_layout_of_match_table[] = {
	{ .compatible = "fixed-layout", },
	{},
};

static struct nvmem_layout_driver fixed_layout_layout = {
	.driver = {
		.name = "fixed-layout",
		.of_match_table = fixed_layout_of_match_table,
	},
	.probe = fixed_layout_probe,
	.remove = fixed_layout_remove,
};
module_nvmem_layout_driver(fixed_layout_layout);

MODULE_AUTHOR("Mathieu Dubois-Briand");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(of, fixed_layout_of_match_table);
MODULE_DESCRIPTION("NVMEM fixed-layout driver");
