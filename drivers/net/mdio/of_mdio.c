// SPDX-License-Identifier: GPL-2.0-only
/*
 * OF helpers for the MDIO (Ethernet PHY) API
 *
 * Copyright (c) 2009 Secret Lab Technologies, Ltd.
 *
 * This file provides helper functions for extracting PHY device information
 * out of the OpenFirmware device tree and using it to populate an mii_bus.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/fwnode_mdio.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/phy.h>
#include <linux/phy_fixed.h>
#include <linux/sched.h>
#include <linux/string.h>

#define DEFAULT_GPIO_RESET_DELAY	10	/* in microseconds */

MODULE_AUTHOR("Grant Likely <grant.likely@secretlab.ca>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("OpenFirmware MDIO bus (Ethernet PHY) accessors");

#if IS_ENABLED(CONFIG_OF_DYNAMIC)
/*
 * OF changes can nest when probing one MDIO device enables another node on
 * the same bus. Serialize independent changes while allowing that nesting.
 */
static DEFINE_MUTEX(of_mdio_reconfig_mutex);
static struct task_struct *of_mdio_reconfig_owner;
static unsigned int of_mdio_reconfig_depth;

static void of_mdio_reconfig_lock(void)
{
	if (!mutex_trylock(&of_mdio_reconfig_mutex)) {
		if (READ_ONCE(of_mdio_reconfig_owner) == current) {
			of_mdio_reconfig_depth++;
			return;
		}
		mutex_lock(&of_mdio_reconfig_mutex);
	}

	WARN_ON_ONCE(READ_ONCE(of_mdio_reconfig_owner));
	WARN_ON_ONCE(of_mdio_reconfig_depth);
	WRITE_ONCE(of_mdio_reconfig_owner, current);
	of_mdio_reconfig_depth = 1;
}

static void of_mdio_reconfig_unlock(void)
{
	WARN_ON_ONCE(READ_ONCE(of_mdio_reconfig_owner) != current);
	WARN_ON_ONCE(!of_mdio_reconfig_depth);

	if (--of_mdio_reconfig_depth)
		return;

	WRITE_ONCE(of_mdio_reconfig_owner, NULL);
	mutex_unlock(&of_mdio_reconfig_mutex);
}
#else
static inline void of_mdio_reconfig_lock(void)
{
}

static inline void of_mdio_reconfig_unlock(void)
{
}
#endif

/* Extract the clause 22 phy ID from the compatible string of the form
 * ethernet-phy-idAAAA.BBBB */
static int of_get_phy_id(struct device_node *device, u32 *phy_id)
{
	return fwnode_get_phy_id(of_fwnode_handle(device), phy_id);
}

int of_mdiobus_phy_device_register(struct mii_bus *mdio, struct phy_device *phy,
				   struct device_node *child, u32 addr)
{
	return fwnode_mdiobus_phy_device_register(mdio, phy,
						  of_fwnode_handle(child),
						  addr);
}
EXPORT_SYMBOL(of_mdiobus_phy_device_register);

static int of_mdiobus_register_phy(struct mii_bus *mdio,
				    struct device_node *child, u32 addr)
{
	return fwnode_mdiobus_register_phy(mdio, of_fwnode_handle(child), addr);
}

static int of_mdiobus_register_device(struct mii_bus *mdio,
				      struct device_node *child, u32 addr)
{
	struct fwnode_handle *fwnode = of_fwnode_handle(child);
	struct mdio_device *mdiodev;
	int rc;

	mdiodev = mdio_device_create(mdio, addr);
	if (IS_ERR(mdiodev))
		return PTR_ERR(mdiodev);

	/* Associate the OF node with the device structure so it
	 * can be looked up later.
	 */
	device_set_node(&mdiodev->dev, fwnode_handle_get(fwnode));

	/* All data is now stored in the mdiodev struct; register it. */
	rc = mdio_device_register(mdiodev);
	if (rc) {
		mdio_device_free(mdiodev);
		return rc;
	}

	dev_dbg(&mdio->dev, "registered mdio device %pOFn at address %i\n",
		child, addr);
	return 0;
}

static int of_mdiobus_register_child(struct mii_bus *mdio,
				     struct device_node *child, u32 addr)
{
	int rc;

	if (of_node_test_and_set_flag(child, OF_POPULATED))
		return 0;

	if (of_mdiobus_child_is_phy(child))
		rc = of_mdiobus_register_phy(mdio, child, addr);
	else
		rc = of_mdiobus_register_device(mdio, child, addr);

	if (rc)
		of_node_clear_flag(child, OF_POPULATED);

	return rc;
}

/* The following is a list of PHY compatible strings which appear in
 * some DTBs. The compatible string is never matched against a PHY
 * driver, so is pointless. We only expect devices which are not PHYs
 * to have a compatible string, so they can be matched to an MDIO
 * driver.  Encourage users to upgrade their DT blobs to remove these.
 */
static const struct of_device_id whitelist_phys[] = {
	{ .compatible = "brcm,40nm-ephy" },
	{ .compatible = "broadcom,bcm5241" },
	{ .compatible = "marvell,88E1111", },
	{ .compatible = "marvell,88e1116", },
	{ .compatible = "marvell,88e1118", },
	{ .compatible = "marvell,88e1145", },
	{ .compatible = "marvell,88e1149r", },
	{ .compatible = "marvell,88e1310", },
	{ .compatible = "marvell,88E1510", },
	{ .compatible = "marvell,88E1514", },
	{ .compatible = "moxa,moxart-rtl8201cp", },
	{}
};

/*
 * Return true if the child node is for a phy. It must either:
 * o Compatible string of "ethernet-phy-idX.X"
 * o Compatible string of "ethernet-phy-ieee802.3-c45"
 * o Compatible string of "ethernet-phy-ieee802.3-c22"
 * o In the white list above (and issue a warning)
 * o No compatibility string
 *
 * A device which is not a phy is expected to have a compatible string
 * indicating what sort of device it is.
 */
bool of_mdiobus_child_is_phy(struct device_node *child)
{
	u32 phy_id;

	if (of_get_phy_id(child, &phy_id) != -EINVAL)
		return true;

	if (of_device_is_compatible(child, "ethernet-phy-ieee802.3-c45"))
		return true;

	if (of_device_is_compatible(child, "ethernet-phy-ieee802.3-c22"))
		return true;

	if (of_match_node(whitelist_phys, child)) {
		pr_warn(FW_WARN
			"%pOF: Whitelisted compatible string. Please remove\n",
			child);
		return true;
	}

	if (!of_property_present(child, "compatible"))
		return true;

	return false;
}
EXPORT_SYMBOL(of_mdiobus_child_is_phy);

static int of_mdiobus_scan_phy(struct mii_bus *mdio,
			       struct device_node *child)
{
	int addr, rc;

	if (!of_mdiobus_child_is_phy(child))
		return -ENODEV;

	for (addr = 0; addr < PHY_MAX_ADDR; addr++) {
		if (mdiobus_is_registered_device(mdio, addr))
			continue;

		dev_info(&mdio->dev, "scan phy %pOFn at address %i\n",
			 child, addr);

		/* -ENODEV means that scanning should continue. */
		rc = of_mdiobus_register_child(mdio, child, addr);
		if (!rc)
			return 0;
		if (rc != -ENODEV)
			return rc;
	}

	return -ENODEV;
}

static int __of_mdiobus_parse_phys(struct mii_bus *mdio, struct device_node *np,
				   bool *scanphys)
{
	struct device_node *child;
	int addr, rc = 0;

	/* Loop over the child nodes and register a phy_device for each phy */
	for_each_available_child_of_node(np, child) {
		if (of_node_name_eq(child, "ethernet-phy-package")) {
			/* Ignore invalid ethernet-phy-package node */
			if (!of_property_present(child, "reg"))
				continue;

			rc = __of_mdiobus_parse_phys(mdio, child, NULL);
			if (rc && rc != -ENODEV)
				goto exit;

			continue;
		}

		addr = of_mdio_parse_addr(&mdio->dev, child);
		if (addr < 0) {
			/* Skip scanning for invalid ethernet-phy-package node */
			if (scanphys)
				*scanphys = true;
			continue;
		}

		rc = of_mdiobus_register_child(mdio, child, addr);

		if (rc == -ENODEV)
			dev_err(&mdio->dev,
				"MDIO device at address %d is missing.\n",
				addr);
		else if (rc)
			goto exit;
	}

	return 0;
exit:
	of_node_put(child);
	return rc;
}

/**
 * __of_mdiobus_register - Register mii_bus and create PHYs from the device tree
 * @mdio: pointer to mii_bus structure
 * @np: pointer to device_node of MDIO bus.
 * @owner: module owning the @mdio object.
 *
 * This function registers the mii_bus structure and registers a phy_device
 * for each child node of @np.
 */
int __of_mdiobus_register(struct mii_bus *mdio, struct device_node *np,
			  struct module *owner)
{
	struct device_node *child;
	bool scanphys = false;
	int rc;

	if (!np)
		return __mdiobus_register(mdio, owner);

	/* Do not continue if the node is disabled */
	if (!of_device_is_available(np))
		return -ENODEV;

	/* Mask out all PHYs from auto probing.  Instead the PHYs listed in
	 * the device tree are populated after the bus has been registered */
	mdio->phy_mask = ~0;

	device_set_node(&mdio->dev, of_fwnode_handle(np));

	/* Get bus level PHY reset GPIO details */
	mdio->reset_delay_us = DEFAULT_GPIO_RESET_DELAY;
	of_property_read_u32(np, "reset-delay-us", &mdio->reset_delay_us);
	mdio->reset_post_delay_us = 0;
	of_property_read_u32(np, "reset-post-delay-us", &mdio->reset_post_delay_us);

	/* Register the MDIO bus */
	rc = __mdiobus_register(mdio, owner);
	if (rc)
		return rc;

	of_mdio_reconfig_lock();
	rc = mdiobus_device_change_begin(mdio, false);
	if (rc) {
		of_mdio_reconfig_unlock();
		mdiobus_unregister(mdio);
		return rc;
	}

	/* Loop over the child nodes and register a phy_device for each phy */
	rc = __of_mdiobus_parse_phys(mdio, np, &scanphys);
	if (rc)
		goto out_change;

	if (!scanphys)
		goto out_change;

	/* auto scan for PHYs with empty reg property */
	for_each_available_child_of_node(np, child) {
		/* Skip PHYs with reg property set or ethernet-phy-package node */
		if (of_property_present(child, "reg") ||
		    of_node_name_eq(child, "ethernet-phy-package"))
			continue;

		rc = of_mdiobus_scan_phy(mdio, child);
		if (rc && rc != -ENODEV)
			goto put_child;
		rc = 0;
	}

out_change:
	mdiobus_device_change_end(mdio, false);
	of_mdio_reconfig_unlock();
	if (!rc)
		return 0;

	mdiobus_unregister(mdio);
	return rc;

put_child:
	of_node_put(child);
	goto out_change;
}
EXPORT_SYMBOL(__of_mdiobus_register);

#if IS_ENABLED(CONFIG_OF_DYNAMIC)
static bool of_mdiobus_node_is_available(struct device_node *node)
{
	return !of_node_check_flag(node, OF_DETACHED) &&
	       of_device_is_available(node);
}

static int of_mdiobus_add_node(struct mii_bus *mdio,
			       struct device_node *node)
{
	struct device_node *child;
	int addr, rc, ret = 0;

	if (!of_mdiobus_node_is_available(node))
		return 0;

	if (of_node_name_eq(node, "ethernet-phy-package")) {
		if (!of_property_present(node, "reg"))
			return 0;

		for_each_available_child_of_node(node, child) {
			rc = of_mdiobus_add_node(mdio, child);
			if (rc && rc != -ENODEV && !ret)
				ret = rc;
		}

		return ret;
	}

	addr = of_mdio_parse_addr(&mdio->dev, node);
	if (addr < 0) {
		if (of_property_present(node, "reg"))
			return addr;

		rc = of_mdiobus_scan_phy(mdio, node);
	} else {
		rc = of_mdiobus_register_child(mdio, node, addr);
	}

	if (rc == -ENODEV && addr >= 0)
		dev_err(&mdio->dev,
			"MDIO device at address %d is missing.\n", addr);

	return rc;
}

static bool of_mdiobus_node_is_busy(struct mii_bus *mdio,
				    struct device_node *node)
{
	struct mdio_device *mdiodev;
	struct phy_device *phydev;
	bool busy = false;

	if (of_node_name_eq(node, "ethernet-phy-package")) {
		for_each_child_of_node_scoped(node, child) {
			if (of_mdiobus_node_is_busy(mdio, child))
				return true;
		}

		return false;
	}

	if (!of_node_check_flag(node, OF_POPULATED))
		return false;

	mdiodev = of_mdio_find_device(node);
	if (!mdiodev)
		return true;
	if (mdiodev->bus != mdio) {
		put_device(&mdiodev->dev);
		return true;
	}

	mutex_lock(&mdio->mdio_map_lock);
	busy = rcu_access_pointer(mdio->mdio_map[mdiodev->addr]) != mdiodev ||
	       (mdio->mdio_map_pending & BIT(mdiodev->addr));
	if (mdiodev->flags & MDIO_DEVICE_FLAG_PHY) {
		phydev = to_phy_device(&mdiodev->dev);
		if (phydev->attached) {
			busy = true;
			dev_warn(&mdiodev->dev,
				 "cannot remove an attached PHY; remove its consumer first\n");
		}
	}
	mutex_unlock(&mdio->mdio_map_lock);
	put_device(&mdiodev->dev);

	return busy;
}

static struct device_node *
of_mdiobus_get_removal_scope(struct device_node *node)
{
	struct device_node *parent;

	if (of_node_name_eq(node, "ethernet-phy-package"))
		return of_node_get(node);

	parent = of_get_parent(node);
	if (of_node_name_eq(parent, "ethernet-phy-package"))
		return parent;

	of_node_put(parent);
	return of_node_get(node);
}

static int of_mdiobus_remove_node(struct mii_bus *mdio,
				  struct device_node *node)
{
	struct mdio_device *mdiodev;
	struct device_node *child;
	int ret;

	if (of_node_name_eq(node, "ethernet-phy-package")) {
		for_each_child_of_node(node, child) {
			ret = of_mdiobus_remove_node(mdio, child);
			if (ret) {
				of_node_put(child);
				return ret;
			}
		}
		return 0;
	}

	if (!of_node_check_flag(node, OF_POPULATED))
		return 0;

	/* The OF node lookup also covers PHYs found by address scanning. */
	mdiodev = of_mdio_find_device(node);
	if (!mdiodev)
		return -EBUSY;
	if (mdiodev->bus != mdio) {
		put_device(&mdiodev->dev);
		return -ENODEV;
	}

	ret = mdiodev->device_remove(mdiodev, true);
	if (!ret)
		mdiodev->device_free(mdiodev);
	put_device(&mdiodev->dev);

	return ret == -ENODEV ? 0 : ret;
}

static struct mii_bus *of_mdiobus_find_parent(struct device_node *node)
{
	struct device_node *parent, *bus_node;
	struct mii_bus *mdio;

	parent = of_get_parent(node);
	if (!parent)
		return NULL;

	if (of_node_name_eq(parent, "ethernet-phy-package")) {
		if (!of_device_is_available(parent) ||
		    !of_property_present(parent, "reg")) {
			of_node_put(parent);
			return NULL;
		}

		bus_node = of_get_parent(parent);
		of_node_put(parent);
	} else {
		bus_node = parent;
	}

	mdio = of_mdio_find_bus(bus_node);
	of_node_put(bus_node);

	return mdio;
}

#if IS_ENABLED(CONFIG_OF_OVERLAY)
/* Overlay entry notifier errors cannot stop removal after the tree changed. */
static bool of_mdiobus_live_node_is_busy(struct device_node *node)
{
	struct device_node *scope;
	struct mii_bus *mdio;
	bool busy;

	scope = of_mdiobus_get_removal_scope(node);
	mdio = of_mdiobus_find_parent(scope);
	/* Non-MDIO nodes and buses already removed cannot block an overlay. */
	if (!mdio) {
		busy = false;
		goto out_put_scope;
	}

	busy = true;
	if (!mdiobus_device_change_begin(mdio, true)) {
		busy = of_mdiobus_node_is_busy(mdio, scope);
		mdiobus_device_change_end(mdio, true);
	}
	put_device(&mdio->dev);

out_put_scope:
	of_node_put(scope);
	return busy;
}

static struct device_node *
of_mdiobus_overlay_target_child(struct device_node *target,
				struct device_node *overlay_child)
{
	const char *name = kbasename(overlay_child->full_name);
	struct device_node *child;

	for_each_child_of_node(target, child) {
		if (!of_node_cmp(kbasename(child->full_name), name))
			return child;
	}

	return NULL;
}

static bool of_mdiobus_overlay_node_is_busy(struct device_node *overlay,
					    struct device_node *target,
					    bool added)
{
	struct device_node *overlay_child, *target_child;
	bool busy, child_added;

	if ((added || of_property_present(overlay, "status")) &&
	    of_mdiobus_live_node_is_busy(target))
		return true;

	for_each_child_of_node(overlay, overlay_child) {
		target_child = of_mdiobus_overlay_target_child(target,
							       overlay_child);
		if (!target_child)
			continue;

		child_added = of_node_check_flag(target_child, OF_OVERLAY);
		busy = of_mdiobus_overlay_node_is_busy(overlay_child,
						       target_child, child_added);
		of_node_put(target_child);
		if (busy) {
			of_node_put(overlay_child);
			return true;
		}
	}

	return false;
}

static int of_mdiobus_overlay_notify(struct notifier_block *nb,
				     unsigned long action, void *arg)
{
	struct of_overlay_notify_data *nd = arg;
	bool busy;

	if (action != OF_OVERLAY_PRE_REMOVE)
		return NOTIFY_OK;

	busy = of_mdiobus_overlay_node_is_busy(nd->overlay, nd->target,
					       false);

	return busy ? notifier_from_errno(-EBUSY) : NOTIFY_OK;
}

static struct notifier_block of_mdio_overlay_notifier = {
	.notifier_call = of_mdiobus_overlay_notify,
};
#endif

static int of_mdiobus_notify(struct notifier_block *nb, unsigned long action,
			     void *arg)
{
	struct of_reconfig_data *rd = arg;
	struct device_node *scope;
	struct mii_bus *mdio;
	enum of_reconfig_change change;
	bool removing;
	int rc, ret = NOTIFY_OK;

	of_mdio_reconfig_lock();
	change = of_reconfig_get_state_change(action, rd);
	switch (change) {
	case OF_RECONFIG_CHANGE_ADD:
		/* A newer change may have made this notification stale. */
		if (!of_mdiobus_node_is_available(rd->dn))
			goto out_unlock;
		removing = false;
		break;
	case OF_RECONFIG_CHANGE_REMOVE:
		/* A newer change may have made this notification stale. */
		if (of_mdiobus_node_is_available(rd->dn))
			goto out_unlock;
		removing = true;
		break;
	default:
		goto out_unlock;
	}

	mdio = of_mdiobus_find_parent(rd->dn);
	if (!mdio)
		goto out_unlock;

	rc = mdiobus_device_change_begin(mdio, removing);
	if (rc) {
		if (!removing)
			ret = notifier_from_errno(-EPROBE_DEFER);
		else if (rc != -ENODEV)
			ret = notifier_from_errno(rc);
		goto out_put_mdio;
	}

	if (!removing) {
		rc = of_mdiobus_add_node(mdio, rd->dn);
	} else {
		/* The node may already be detached from its parent hierarchy. */
		scope = of_mdiobus_get_removal_scope(rd->dn);
		if (of_mdiobus_node_is_busy(mdio, scope))
			rc = -EBUSY;
		else
			rc = of_mdiobus_remove_node(mdio, rd->dn);
		of_node_put(scope);
	}

	mdiobus_device_change_end(mdio, removing);
	if (rc && (removing || rc != -ENODEV))
		ret = notifier_from_errno(rc);

out_put_mdio:
	put_device(&mdio->dev);
out_unlock:
	of_mdio_reconfig_unlock();

	return ret;
}

static struct notifier_block of_mdio_notifier = {
	.notifier_call = of_mdiobus_notify,
};

static int __init of_mdio_init(void)
{
	int ret;

	ret = of_reconfig_notifier_register(&of_mdio_notifier);
	if (ret)
		return ret;

#if IS_ENABLED(CONFIG_OF_OVERLAY)
	ret = of_overlay_notifier_register(&of_mdio_overlay_notifier);
	if (ret)
		of_reconfig_notifier_unregister(&of_mdio_notifier);
#endif

	return ret;
}
module_init(of_mdio_init);

static void __exit of_mdio_exit(void)
{
#if IS_ENABLED(CONFIG_OF_OVERLAY)
	of_overlay_notifier_unregister(&of_mdio_overlay_notifier);
#endif
	of_reconfig_notifier_unregister(&of_mdio_notifier);
}
module_exit(of_mdio_exit);
#endif /* CONFIG_OF_DYNAMIC */

/**
 * of_mdio_find_device - Given a device tree node, find the mdio_device
 * @np: pointer to the mdio_device's device tree node
 *
 * If successful, returns a pointer to the mdio_device with the embedded
 * struct device refcount incremented by one, or NULL on failure.
 * The caller should call put_device() on the mdio_device after its use
 */
struct mdio_device *of_mdio_find_device(struct device_node *np)
{
	return fwnode_mdio_find_device(of_fwnode_handle(np));
}
EXPORT_SYMBOL(of_mdio_find_device);

/**
 * of_phy_find_device - Give a PHY node, find the phy_device
 * @phy_np: Pointer to the phy's device tree node
 *
 * If successful, returns a pointer to the phy_device with the embedded
 * struct device refcount incremented by one, or NULL on failure.
 */
struct phy_device *of_phy_find_device(struct device_node *phy_np)
{
	return fwnode_phy_find_device(of_fwnode_handle(phy_np));
}
EXPORT_SYMBOL(of_phy_find_device);

/**
 * of_phy_connect - Connect to the phy described in the device tree
 * @dev: pointer to net_device claiming the phy
 * @phy_np: Pointer to device tree node for the PHY
 * @hndlr: Link state callback for the network device
 * @flags: flags to pass to the PHY
 * @iface: PHY data interface type
 *
 * If successful, returns a pointer to the phy_device with the embedded
 * struct device refcount incremented by one, or NULL on failure. The
 * refcount must be dropped by calling phy_disconnect() or phy_detach().
 */
struct phy_device *of_phy_connect(struct net_device *dev,
				  struct device_node *phy_np,
				  void (*hndlr)(struct net_device *), u32 flags,
				  phy_interface_t iface)
{
	struct phy_device *phy = of_phy_find_device(phy_np);
	int ret;

	if (!phy)
		return NULL;

	phy->dev_flags |= flags;

	ret = phy_connect_direct(dev, phy, hndlr, iface);

	/* refcount is held by phy_connect_direct() on success */
	put_device(&phy->mdio.dev);

	return ret ? NULL : phy;
}
EXPORT_SYMBOL(of_phy_connect);

/**
 * of_phy_get_and_connect
 * - Get phy node and connect to the phy described in the device tree
 * @dev: pointer to net_device claiming the phy
 * @np: Pointer to device tree node for the net_device claiming the phy
 * @hndlr: Link state callback for the network device
 *
 * If successful, returns a pointer to the phy_device with the embedded
 * struct device refcount incremented by one, or NULL on failure. The
 * refcount must be dropped by calling phy_disconnect() or phy_detach().
 */
struct phy_device *of_phy_get_and_connect(struct net_device *dev,
					  struct device_node *np,
					  void (*hndlr)(struct net_device *))
{
	phy_interface_t iface;
	struct device_node *phy_np;
	struct phy_device *phy;
	int ret;

	ret = of_get_phy_mode(np, &iface);
	if (ret)
		return NULL;
	if (of_phy_is_fixed_link(np)) {
		ret = of_phy_register_fixed_link(np);
		if (ret < 0) {
			netdev_err(dev, "broken fixed-link specification\n");
			return NULL;
		}
		phy_np = of_node_get(np);
	} else {
		phy_np = of_parse_phandle(np, "phy-handle", 0);
		if (!phy_np)
			return NULL;
	}

	phy = of_phy_connect(dev, phy_np, hndlr, 0, iface);

	of_node_put(phy_np);

	return phy;
}
EXPORT_SYMBOL(of_phy_get_and_connect);

/*
 * of_phy_is_fixed_link() and of_phy_register_fixed_link() must
 * support two DT bindings:
 * - the old DT binding, where 'fixed-link' was a property with 5
 *   cells encoding various information about the fixed PHY
 * - the new DT binding, where 'fixed-link' is a sub-node of the
 *   Ethernet device.
 */
bool of_phy_is_fixed_link(struct device_node *np)
{
	struct device_node *dn;
	int err;
	const char *managed;

	/* New binding */
	dn = of_get_child_by_name(np, "fixed-link");
	if (dn) {
		of_node_put(dn);
		return true;
	}

	err = of_property_read_string(np, "managed", &managed);
	if (err == 0 && strcmp(managed, "auto") != 0)
		return true;

	/* Old binding */
	if (of_property_count_u32_elems(np, "fixed-link") == 5)
		return true;

	return false;
}
EXPORT_SYMBOL(of_phy_is_fixed_link);

int of_phy_register_fixed_link(struct device_node *np)
{
	struct fixed_phy_status status = {};
	struct device_node *fixed_link_node;
	u32 fixed_link_prop[5];
	const char *managed;

	if (of_property_read_string(np, "managed", &managed) == 0 &&
	    strcmp(managed, "in-band-status") == 0) {
		/* status is zeroed, namely its .link member */
		goto register_phy;
	}

	/* New binding */
	fixed_link_node = of_get_child_by_name(np, "fixed-link");
	if (fixed_link_node) {
		status.link = 1;
		status.duplex = of_property_read_bool(fixed_link_node,
						      "full-duplex");
		if (of_property_read_u32(fixed_link_node, "speed",
					 &status.speed)) {
			of_node_put(fixed_link_node);
			return -EINVAL;
		}
		status.pause = of_property_read_bool(fixed_link_node, "pause");
		status.asym_pause = of_property_read_bool(fixed_link_node,
							  "asym-pause");
		of_node_put(fixed_link_node);

		goto register_phy;
	}

	/* Old binding */
	if (of_property_read_u32_array(np, "fixed-link", fixed_link_prop,
				       ARRAY_SIZE(fixed_link_prop)) == 0) {
		pr_warn_once("%pOF uses deprecated array-style fixed-link binding!\n",
			     np);
		status.link = 1;
		status.duplex = fixed_link_prop[1];
		status.speed  = fixed_link_prop[2];
		status.pause  = fixed_link_prop[3];
		status.asym_pause = fixed_link_prop[4];
		goto register_phy;
	}

	return -ENODEV;

register_phy:
	return PTR_ERR_OR_ZERO(fixed_phy_register(&status, np));
}
EXPORT_SYMBOL(of_phy_register_fixed_link);

void of_phy_deregister_fixed_link(struct device_node *np)
{
	struct phy_device *phydev;

	phydev = of_phy_find_device(np);
	if (!phydev)
		return;

	fixed_phy_unregister(phydev);

	put_device(&phydev->mdio.dev);	/* of_phy_find_device() */
}
EXPORT_SYMBOL(of_phy_deregister_fixed_link);
