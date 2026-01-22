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
