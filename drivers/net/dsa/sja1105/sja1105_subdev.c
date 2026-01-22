// SPDX-License-Identifier: GPL-2.0
/* Copyright 2025 NXP
 */
#include <linux/device/devres.h>
#include <linux/ioport.h>
#include <linux/platform_device.h>

#include "sja1105.h"
#include "sja1105_subdev.h"

static const struct resource sja1110_mdio_cbt1_res =
	DEFINE_RES_REG_NAMED(0x704000, 0x4000, "mdio_cbt1");

static const struct resource sja1110_mdio_cbtx_res =
	DEFINE_RES_REG_NAMED(0x709000, 0x1000, "mdio_cbtx");

static bool fwnode_is_hierarchical_child(struct fwnode_handle *child,
					 struct fwnode_handle *parent)
{
	struct fwnode_handle *next = child;

	do {
		if (next == parent)
			return true;
		next = fwnode_get_parent(next);
	} while (next);

	return false;
}

static void of_subdev_del(void *data)
{
	struct platform_device *pdev = data;

	platform_device_unregister(pdev);
}

/**
 * devm_of_subdev_add() - Register an OF sub-device as a managed platform device
 * @pdevinfo: Platform device information structure containing parent, fwnode,
 *	name, resources, etc.
 *
 * This function registers a platform device as a sub-device of
 * @pdevinfo.parent using the information provided in @pdevinfo. The sub-device
 * will be automatically unregistered when @pdevinfo.parent is removed, thanks
 * to devres management.
 *
 * If the fwnode specified in @pdevinfo is not available (disabled in device
 * tree), this function returns success without creating the device.
 *
 * For the sub-device drivers to access their registers, a form of
 * devm_regmap_init(parent) should have been called prior to this, which
 * makes the parent regmap visible via dev_get_regmap(&pdev->dev.parent)
 * in the sub-device driver. The entire address space is made available through
 * this regmap to all sub-devices, although they are expected to segment it
 * according to the given resources.
 *
 * Return: 0 on success, negative error code on failure
 */
static int devm_of_subdev_add(const struct platform_device_info *pdevinfo)
{
	struct device *parent = pdevinfo->parent;
	struct platform_device *pdev;

	if (!fwnode_device_is_available(pdevinfo->fwnode))
		return 0;

	/* To avoid API abuse, ensure that the sub-device fwnode is,
	 * in fact, related to the parent.
	 */
	if (!fwnode_is_hierarchical_child(pdevinfo->fwnode, dev_fwnode(parent)))
		return -EINVAL;

	pdev = platform_device_register_full(pdevinfo);
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);

	return devm_add_action_or_reset(parent, of_subdev_del, pdev);
}

static int devm_sja1105_add_mdio_subdev(struct device *parent,
					struct device_node *np,
					const struct resource *res,
					size_t num_res)
{
	struct platform_device_info subdev;
	char name[64];
	u32 reg;
	int err;

	err = of_property_read_u32(np, "reg", &reg);
	if (err)
		return err;

	snprintf(name, sizeof(name), "%s.%pOFn", dev_name(parent), np);
	subdev = (struct platform_device_info) {
		.parent = parent,
		.fwnode = of_fwnode_handle(np),
		.name = name,
		.id = reg,
		.res = res,
		.num_res = num_res,
	};

	return devm_of_subdev_add(&subdev);
}

/* Legacy nodes which lack a proper resource description in the device tree,
 * so we need to specify it manually.
 */
static int devm_sja1105_add_mdio_subdevs(struct dsa_switch *ds,
					 struct device_node *mdio_node)
{
	struct device *parent = ds->dev;
	struct device_node *np;
	int err;

	np = of_get_compatible_child(mdio_node, "nxp,sja1110-base-tx-mdio");
	if (np) {
		err = devm_sja1105_add_mdio_subdev(parent, np,
						   &sja1110_mdio_cbtx_res, 1);
		of_node_put(np);
		if (err)
			return err;
	}

	np = of_get_compatible_child(mdio_node, "nxp,sja1110-base-t1-mdio");
	if (np) {
		err = devm_sja1105_add_mdio_subdev(parent, np,
						   &sja1110_mdio_cbt1_res, 1);
		of_node_put(np);
		if (err)
			return err;
	}

	return 0;
}

int devm_sja1105_add_subdevs(struct dsa_switch *ds)
{
	struct device_node *switch_node = dev_of_node(ds->dev);
	struct device_node *mdio_node;
	int rc = 0;

	mdio_node = of_get_available_child_by_name(switch_node, "mdios");
	if (mdio_node) {
		rc = devm_sja1105_add_mdio_subdevs(ds, mdio_node);
		of_node_put(mdio_node);
		if (rc)
			return rc;
	}

	return 0;
}

static bool of_child_node_exists(struct device_node *np, const char *name)
{
	for_each_child_of_node_scoped(np, child)
		if (!strcmp(of_node_full_name(child), name))
			return true;

	return false;
}

static int sja1105_create_pcs_nodes(struct sja1105_private *priv,
				    struct device_node *regs_node)
{
	struct dsa_switch *ds = priv->ds;
	struct device *dev = ds->dev;
	struct device_node *pcs_node;
	char node_name[32];
	u32 reg_props[2];
	int rc;

	for (int i = 0; i < priv->info->num_pcs_resources; i++) {
		const struct sja1105_pcs_resource *pcs_res;

		pcs_res = &priv->info->pcs_resources[i];

		/* phys_addr_t has variable size depending on the value of
		 * CONFIG_PHYS_ADDR_T_64BIT, cast to the larger unsigned long
		 * long type for printf.
		 */
		snprintf(node_name, sizeof(node_name), "ethernet-pcs@%llx",
			 (unsigned long long)pcs_res->res.start);

		if (of_child_node_exists(regs_node, node_name))
			continue;

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

		rc = of_changeset_add_prop_u32(&priv->of_cs, pcs_node,
					       "reg-io-width", 4);
		if (rc) {
			dev_err(dev, "Failed to add reg-io-width property to %s: %pe\n",
				node_name, ERR_PTR(rc));
			return rc;
		}

		/* The SJA1105 XPCS is integrated with a TX-inverting custom
		 * PMA. We need to invert the polarity in the PCS to obtain a
		 * non-inverted signal at the pins.
		 */
		rc = of_changeset_add_prop_u32(&priv->of_cs, pcs_node, "tx-polarity",
					       pcs_res->tx_polarity);
		if (rc) {
			dev_err(dev, "Failed to add tx-polarity property to %s: %pe\n",
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
	int rc;

	regs_node = of_changeset_create_node(&priv->of_cs, switch_node, "regs");
	if (!regs_node) {
		dev_err(dev, "Failed to create 'regs' device tree node\n");
		return ERR_PTR(-ENOMEM);
	}

	rc = of_changeset_add_prop_u32(&priv->of_cs, regs_node, "#address-cells", 1);
	if (rc) {
		dev_err(dev, "Failed to add #address-cells property: %pe\n",
			ERR_PTR(rc));
		return ERR_PTR(rc);
	}

	rc = of_changeset_add_prop_u32(&priv->of_cs, regs_node, "#size-cells", 1);
	if (rc) {
		dev_err(dev, "Failed to add #size-cells property: %pe\n",
			ERR_PTR(rc));
		return ERR_PTR(rc);
	}

	return regs_node;
}

static void sja1105_restore_device_tree(void *data)
{
	struct sja1105_private *priv = data;
	struct device *dev = priv->ds->dev;
	int rc;

	rc = of_changeset_revert(&priv->of_cs);
	if (rc) {
		dev_err(dev, "Failed to revert device tree changeset: %pe\n",
			ERR_PTR(rc));
	}

	of_changeset_destroy(&priv->of_cs);
}

int devm_sja1105_fill_device_tree(struct dsa_switch *ds)
{
	struct device_node *switch_node, *regs_node;
	struct sja1105_private *priv = ds->priv;
	bool regs_node_created = false;
	struct device *dev = ds->dev;
	int rc;

	if (!priv->info->num_pcs_resources)
		return 0;

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

	rc = devm_add_action_or_reset(dev, sja1105_restore_device_tree, priv);
	goto out_put_regs_node;

out_destroy_changeset:
	of_changeset_destroy(&priv->of_cs);
out_put_regs_node:
	if (!regs_node_created)
		of_node_put(regs_node);

	return rc;
}
