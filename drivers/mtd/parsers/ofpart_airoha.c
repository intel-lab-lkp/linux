// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Christian Marangi <ansuelsmth@gmail.com>
 */

#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>

#include "ofpart_airoha.h"

int airoha_partitions_post_parse(struct mtd_info *mtd,
				 struct mtd_partition *parts,
				 int nr_parts)
{
	struct mtd_partition *part;
	int len, a_cells, s_cells;
	struct device_node *pp;
	struct property *prop;
	const __be32 *reg;
	__be32 *new_reg;

	part = &parts[nr_parts - 1];
	pp = part->of_node;

	/* Skip if ART partition have a valid offset instead of a dynamic one */
	if (!of_device_is_compatible(pp, "airoha,dynamic-art"))
		return 0;

	/* ART partition is set at the end of flash - size */
	part->offset = mtd->size - part->size;

	/* Update the offset with the new calculate value in DT */
	prop = kzalloc(sizeof(*prop), GFP_KERNEL);
	if (!prop)
		return -ENOMEM;

	/* Reg already validated by fixed-partition parser */
	reg = of_get_property(pp, "reg", &len);

	/* Fixed partition */
	a_cells = of_n_addr_cells(pp);
	s_cells = of_n_size_cells(pp);

	prop->name = "reg";
	prop->length = (a_cells + s_cells) * sizeof(__be32);
	prop->value = kmemdup(reg, (a_cells + s_cells) * sizeof(__be32),
			      GFP_KERNEL);
	new_reg = prop->value;
	memset(new_reg, 0, a_cells * sizeof(__be32));
	new_reg[a_cells - 1] = cpu_to_be32(part->offset);
	if (a_cells > 1)
		new_reg[0] = cpu_to_be32(part->offset >> 32);
	of_update_property(pp, prop);

	return 0;
}
