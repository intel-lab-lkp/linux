// SPDX-License-Identifier: GPL-2.0
/* Copyright 2025 NXP
 */
#include <linux/ioport.h>
#include <linux/mfd/core.h>

#include "sja1105.h"
#include "sja1105_mfd.h"

static const struct resource sja1110_mdio_cbtx_res =
	DEFINE_RES_REG_NAMED(0x709000, 0x1000, "mdio_cbtx");

static const struct resource sja1110_mdio_cbt1_res =
	DEFINE_RES_REG_NAMED(0x704000, 0x4000, "mdio_cbt1");

static void sja1105_mfd_add_mdio_cells(struct sja1105_private *priv,
				       struct device_node *mdio_node,
				       struct mfd_cell *cells,
				       int *num_cells)
{
	struct device_node *np;

	np = of_get_compatible_child(mdio_node, "nxp,sja1110-base-tx-mdio");
	if (np && of_device_is_available(np)) {
		cells[(*num_cells)++] = (struct mfd_cell) {
			.name = "sja1110-base-tx-mdio",
			.of_compatible = "nxp,sja1110-base-tx-mdio",
			.resources = &sja1110_mdio_cbtx_res,
			.num_resources = 1,
			.parent_of_node = mdio_node,
		};
	}
	if (np)
		of_node_put(np);

	np = of_get_compatible_child(mdio_node, "nxp,sja1110-base-t1-mdio");
	if (np && of_device_is_available(np)) {
		cells[(*num_cells)++] = (struct mfd_cell) {
			.name = "sja1110-base-t1-mdio",
			.of_compatible = "nxp,sja1110-base-t1-mdio",
			.resources = &sja1110_mdio_cbt1_res,
			.num_resources = 1,
			.parent_of_node = mdio_node,
		};
	}
	if (np)
		of_node_put(np);
}

int sja1105_mfd_add_devices(struct dsa_switch *ds)
{
	struct device_node *switch_node = dev_of_node(ds->dev);
	struct sja1105_private *priv = ds->priv;
	struct device_node *mdio_node;
	struct mfd_cell cells[2] = {};
	int num_cells = 0;
	int rc = 0;

	mdio_node = of_get_available_child_by_name(switch_node, "mdios");
	if (mdio_node)
		sja1105_mfd_add_mdio_cells(priv, mdio_node, cells, &num_cells);

	if (num_cells > 0)
		rc = devm_mfd_add_devices(ds->dev, PLATFORM_DEVID_AUTO, cells,
					  num_cells, NULL, 0, NULL);

	of_node_put(mdio_node);
	return rc;
}
