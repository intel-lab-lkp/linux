// SPDX-License-Identifier: GPL-2.0
/* Copyright 2025 NXP
 */
#include <linux/ioport.h>
#include <linux/mfd/core.h>

#include "sja1105.h"
#include "sja1105_mfd.h"

#define SJA1105_MAX_NUM_MDIOS	2
#define SJA1105_MAX_NUM_PCS	4
#define SJA1105_MAX_NUM_CELLS	(SJA1105_MAX_NUM_MDIOS + \
				 SJA1105_MAX_NUM_PCS + \
				 1) /* sentinel */

static const struct resource sja1110_mdio_cbtx_res =
	DEFINE_RES_REG_NAMED(0x709000, 0x1000, "mdio_cbtx");

static const struct resource sja1110_mdio_cbt1_res =
	DEFINE_RES_REG_NAMED(0x704000, 0x4000, "mdio_cbt1");

static void sja1105_mfd_add_pcs_cells(struct sja1105_private *priv,
				      struct device_node *regs_node,
				      struct mfd_cell *cells,
				      int *num_cells)
{
	for (int i = 0; i < priv->info->num_pcs_resources; i++) {
		const struct sja1105_pcs_resource *pcs_res;

		pcs_res = &priv->info->pcs_resources[i];

		cells[(*num_cells)++] = (struct mfd_cell) {
			.name = pcs_res->cell_name,
			.of_compatible = pcs_res->compatible,
			.of_reg = pcs_res->res.start,
			.use_of_reg = true,
			.resources = &pcs_res->res,
			.num_resources = 1,
			.parent_of_node = regs_node,
		};
	}
}

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
	struct mfd_cell cells[SJA1105_MAX_NUM_CELLS] = {};
	struct device_node *regs_node, *mdio_node;
	struct sja1105_private *priv = ds->priv;
	int num_cells = 0;
	int rc = 0;

	regs_node = of_get_available_child_by_name(switch_node, "regs");
	if (regs_node)
		sja1105_mfd_add_pcs_cells(priv, regs_node, cells, &num_cells);

	mdio_node = of_get_available_child_by_name(switch_node, "mdios");
	if (mdio_node)
		sja1105_mfd_add_mdio_cells(priv, mdio_node, cells, &num_cells);

	if (num_cells > 0)
		rc = devm_mfd_add_devices(ds->dev, PLATFORM_DEVID_AUTO, cells,
					  num_cells, NULL, 0, NULL);

	of_node_put(regs_node);
	of_node_put(mdio_node);
	return rc;
}

static bool sja1105_child_node_exists(struct device_node *node,
				      const char *name,
				      const struct resource *res)
{
	struct device_node *child = of_get_child_by_name(node, name);
	u32 reg[2];

	for_each_child_of_node(node, child) {
		if (!of_node_name_eq(child, name))
			continue;

		if (of_property_read_u32_array(child, "reg", reg, ARRAY_SIZE(reg)))
			continue;

		if (reg[0] == res->start && reg[1] == resource_size(res))
			return true;
	}

	return false;
}

static int sja1105_create_pcs_nodes(struct sja1105_private *priv,
				    struct device_node *regs_node)
{
	struct dsa_switch *ds = priv->ds;
	struct device *dev = ds->dev;
	struct device_node *pcs_node;
	const u32 reg_io_width = 4;
	char node_name[32];
	u32 reg_props[2];
	int rc;

	for (int i = 0; i < priv->info->num_pcs_resources; i++) {
		const struct sja1105_pcs_resource *pcs_res;

		pcs_res = &priv->info->pcs_resources[i];

		if (sja1105_child_node_exists(regs_node, "ethernet-pcs",
					      &pcs_res->res))
			continue;

		snprintf(node_name, sizeof(node_name), "ethernet-pcs@%llx",
			 pcs_res->res.start);

		pcs_node = of_changeset_create_node(&priv->of_cs, regs_node,
						    node_name);
		if (!pcs_node) {
			dev_err(dev, "Failed to create PCS node %s\n", node_name);
			return -ENOMEM;
		}

		rc = of_changeset_add_prop_string(&priv->of_cs, pcs_node,
						  "compatible",
						  pcs_res->compatible);
		if (rc) {
			dev_err(dev, "Failed to add compatible property to %s: %pe\n",
				node_name, ERR_PTR(rc));
			return rc;
		}

		reg_props[0] = pcs_res->res.start;
		reg_props[1] = resource_size(&pcs_res->res);
		rc = of_changeset_add_prop_u32_array(&priv->of_cs, pcs_node,
						     "reg", reg_props, 2);
		if (rc) {
			dev_err(dev, "Failed to add reg property to %s: %pe\n",
				node_name, ERR_PTR(rc));
			return rc;
		}

		rc = of_changeset_add_prop_string(&priv->of_cs, pcs_node,
						  "reg-names",
						  pcs_res->res.name);
		if (rc) {
			dev_err(dev, "Failed to add reg-names property to %s: %pe\n",
				node_name, ERR_PTR(rc));
			return rc;
		}

		rc = of_changeset_add_prop_u32_array(&priv->of_cs, pcs_node,
						     "reg-io-width",
						     &reg_io_width, 1);
		if (rc) {
			dev_err(dev, "Failed to add reg-io-width property to %s: %pe\n",
				node_name, ERR_PTR(rc));
			return rc;
		}

		dev_dbg(dev, "Created OF node %pOF\n", pcs_node);
		priv->pcs_fwnode[pcs_res->port] = of_fwnode_handle(pcs_node);
	}

	return 0;
}

static struct device_node *sja1105_create_regs_node(struct sja1105_private *priv,
						    struct device_node *switch_node)
{
	struct device *dev = priv->ds->dev;
	struct device_node *regs_node;
	const u32 addr_size_cells = 1;
	int rc;

	regs_node = of_changeset_create_node(&priv->of_cs, switch_node, "regs");
	if (!regs_node) {
		dev_err(dev, "Failed to create 'regs' device tree node\n");
		return ERR_PTR(-ENOMEM);
	}

	rc = of_changeset_add_prop_u32_array(&priv->of_cs, regs_node,
					     "#address-cells",
					     &addr_size_cells, 1);
	if (rc) {
		dev_err(dev, "Failed to add #address-cells property: %pe\n",
			ERR_PTR(rc));
		return ERR_PTR(rc);
	}

	rc = of_changeset_add_prop_u32_array(&priv->of_cs, regs_node,
					     "#size-cells",
					     &addr_size_cells, 1);
	if (rc) {
		dev_err(dev, "Failed to add #size-cells property: %pe\n",
			ERR_PTR(rc));
		return ERR_PTR(rc);
	}

	return regs_node;
}

int sja1105_fill_device_tree(struct dsa_switch *ds)
{
	struct device_node *switch_node, *regs_node;
	struct sja1105_private *priv = ds->priv;
	bool regs_node_created = false;
	struct device *dev = ds->dev;
	int rc;

	switch_node = dev_of_node(dev);
	of_changeset_init(&priv->of_cs);

	regs_node = of_get_child_by_name(switch_node, "regs");
	if (!regs_node) {
		regs_node = sja1105_create_regs_node(priv, switch_node);
		if (IS_ERR(regs_node)) {
			rc = PTR_ERR(regs_node);
			goto out_destroy_changeset;
		}

		regs_node_created = true;
		dev_dbg(dev, "Created OF node %pOF\n", regs_node);
	}

	rc = sja1105_create_pcs_nodes(priv, regs_node);
	if (rc)
		goto out_destroy_changeset;

	rc = of_changeset_apply(&priv->of_cs);
	if (rc) {
		dev_err(dev, "Failed to apply device tree changeset: %pe\n",
			ERR_PTR(rc));
		goto out_destroy_changeset;
	}

	/* Don't destroy the changeset - we need it for reverting later */
	goto out_put_regs_node;

out_destroy_changeset:
	of_changeset_destroy(&priv->of_cs);
out_put_regs_node:
	if (!regs_node_created)
		of_node_put(regs_node);

	return rc;
}

void sja1105_restore_device_tree(struct dsa_switch *ds)
{
	struct sja1105_private *priv = ds->priv;
	struct device *dev = ds->dev;
	int rc;

	rc = of_changeset_revert(&priv->of_cs);
	if (rc) {
		dev_err(dev, "Failed to revert device tree changeset: %pe\n",
			ERR_PTR(rc));
	}

	of_changeset_destroy(&priv->of_cs);
}
