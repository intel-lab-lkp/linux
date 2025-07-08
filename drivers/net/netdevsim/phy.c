// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2025 Maxime Chevallier <maxime.chevallier@bootlin.com>

#include <linux/debugfs.h>
#include <linux/list.h>
#include <linux/netdevice.h>
#include <linux/slab.h>
#include <linux/phy.h>
#include <linux/phy_fixed.h>
#include <linux/phy_link_topology.h>
#include <linux/platform_device.h>

#include "netdevsim.h"

static atomic_t bus_num = ATOMIC_INIT(0);

/* Dumb MDIO bus for the virtual PHY to sit on */
struct nsim_mdiobus {
	struct platform_device *pdev;
	struct mii_bus *mii;
};

static int nsim_mdio_read(struct mii_bus *bus, int phy_addr, int reg_num)
{
	return 0;
}

static int nsim_mdio_write(struct mii_bus *bus, int phy_addr, int reg_num,
			   u16 val)
{
	return 0;
}

struct nsim_phy_device {
	struct phy_device *phy;
	struct dentry *phy_dir;

	struct list_head node;

	bool link;
};

/* Virtual PHY driver for netdevsim */
static int nsim_match_phy_device(struct phy_device *phydev,
				 const struct phy_driver *drv)
{
	struct mii_bus *mii = phydev->mdio.bus;

	return (mii->read == nsim_mdio_read) &&
	       (mii->write == nsim_mdio_write);
}

static int nsim_get_features(struct phy_device *phydev)
{
	/* Act like a 1G PHY */
	linkmode_set_bit(ETHTOOL_LINK_MODE_TP_BIT, phydev->supported);

	linkmode_set_bit(ETHTOOL_LINK_MODE_1000baseT_Half_BIT, phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_1000baseT_Full_BIT, phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_100baseT_Half_BIT, phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_100baseT_Full_BIT, phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_10baseT_Half_BIT, phydev->supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_10baseT_Full_BIT, phydev->supported);

	return 0;
}

static int nsim_config_aneg(struct phy_device *phydev)
{
	return 0;
}

static int nsim_read_status(struct phy_device *phydev)
{
	struct nsim_phy_device *ns_phy = phydev->priv;

	if (!ns_phy)
		return 0;

	if (ns_phy->link) {
		phydev->speed = SPEED_1000;
		phydev->duplex = DUPLEX_FULL;
	} else {
		phydev->speed = SPEED_UNKNOWN;
		phydev->duplex = DUPLEX_UNKNOWN;
	}

	phydev->link = ns_phy->link;

	return 0;
}

static struct phy_driver nsim_virtual_phy_drv[] = {
	{
		.name			= "Netdevsim virtual PHY driver",
		.get_features		= nsim_get_features,
		.match_phy_device	= nsim_match_phy_device,
		.config_aneg		= nsim_config_aneg,
		.read_status		= nsim_read_status,
	},
};

static struct nsim_mdiobus *nsim_mdiobus_create(void)
{
	struct nsim_mdiobus *mb;

	mb = kzalloc(sizeof(*mb), GFP_KERNEL);
	if (!mb)
		return NULL;

	mb->pdev = platform_device_register_simple("nsim MDIO bus",
						   atomic_read(&bus_num),
						   NULL, 0);
	if (IS_ERR(mb->pdev))
		goto free_mb;

	mb->mii = mdiobus_alloc();
	if (!mb->mii)
		goto free_pdev;

	snprintf(mb->mii->id, MII_BUS_ID_SIZE, "nsim-%d", atomic_read(&bus_num));
	atomic_inc(&bus_num);
	mb->mii->name = "nsim MDIO Bus";
	mb->mii->priv = mb;
	mb->mii->parent = &mb->pdev->dev;
	mb->mii->read = &nsim_mdio_read;
	mb->mii->write = &nsim_mdio_write;
	mb->mii->phy_mask = ~0;

	if (mdiobus_register(mb->mii))
		goto free_mdiobus;

	return mb;

free_mdiobus:
	atomic_dec(&bus_num);
	mdiobus_free(mb->mii);
free_pdev:
	platform_device_unregister(mb->pdev);
free_mb:
	kfree(mb);

	return NULL;
}

static void nsim_mdiobus_destroy(struct nsim_mdiobus *mb)
{
	mdiobus_unregister(mb->mii);
	mdiobus_free(mb->mii);
	atomic_dec(&bus_num);
	platform_device_unregister(mb->pdev);
	kfree(mb);
}

static struct nsim_phy_device *nsim_phy_register(void)
{
	struct nsim_phy_device *ns_phy;
	struct nsim_mdiobus *mb;
	int err;

	mb = nsim_mdiobus_create();
	if (IS_ERR(mb))
		return ERR_CAST(mb);

	ns_phy = kzalloc(sizeof(*ns_phy), GFP_KERNEL);
	if (!ns_phy) {
		err = -ENOMEM;
		goto out_mdio;
	}

	INIT_LIST_HEAD(&ns_phy->node);

	ns_phy->phy = get_phy_device(mb->mii, 0, false);
	if (IS_ERR(ns_phy->phy)) {
		err = PTR_ERR(ns_phy->phy);
		goto out_phy_free;
	}

	err = phy_device_register(ns_phy->phy);
	if (err)
		goto out_phy;

	ns_phy->phy->priv = ns_phy;

	return ns_phy;

out_phy:
	phy_device_free(ns_phy->phy);
out_phy_free:
	kfree(ns_phy);
out_mdio:
	nsim_mdiobus_destroy(mb);
	return ERR_PTR(err);
}

static void nsim_phy_destroy(struct nsim_phy_device *ns_phy)
{
	struct phy_device *phydev = ns_phy->phy;
	struct mii_bus *mii = phydev->mdio.bus;
	struct nsim_mdiobus *mb = mii->priv;

	debugfs_remove_recursive(ns_phy->phy_dir);

	phy_device_remove(phydev);
	list_del(&ns_phy->node);
	kfree(ns_phy);

	nsim_mdiobus_destroy(mb);
}

static int nsim_phy_state_link_set(void *data, u64 val)
{
	struct nsim_phy_device *ns_phy = (struct nsim_phy_device *)data;

	ns_phy->link = !!val;

	phy_trigger_machine(ns_phy->phy);

	return 0;
}

static int nsim_phy_state_link_get(void *data, u64 *val)
{
	struct nsim_phy_device *ns_phy = (struct nsim_phy_device *)data;

	*val = ns_phy->link;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(nsim_phy_state_link_fops, nsim_phy_state_link_get,
			 nsim_phy_state_link_set, "%llu\n");

static void nsim_phy_debugfs_create(struct nsim_dev_port *port,
				    struct nsim_phy_device *ns_phy)
{
	char phy_dir_name[sizeof("phy") + 10];

	sprintf(phy_dir_name, "phy%u", ns_phy->phy->phyindex);

	/* create debugfs stuff */
	ns_phy->phy_dir = debugfs_create_dir(phy_dir_name, port->ddir);

	debugfs_create_file("link", 0600, ns_phy->phy_dir, ns_phy, &nsim_phy_state_link_fops);
}

static void nsim_adjust_link(struct net_device *dev)
{
	phy_print_status(dev->phydev);
}

static ssize_t
nsim_phy_add_write(struct file *file, const char __user *data,
		   size_t count, loff_t *ppos)
{
	struct net_device *dev = file->private_data;
	struct netdevsim *ns = netdev_priv(dev);
	struct nsim_phy_device *ns_phy;
	struct phy_device *pphy;
	u32 parent_id;
	char buf[10];
	ssize_t ret;
	int err;

	if (*ppos != 0)
		return 0;

	if (count >= sizeof(buf))
		return -ENOSPC;

	ret = copy_from_user(buf, data, count);
	if (ret)
		return -EFAULT;
	buf[count] = '\0';

	ret = kstrtouint(buf, 10, &parent_id);
	if (ret)
		return -EINVAL;

	ns_phy = nsim_phy_register();
	if (IS_ERR(ns_phy))
		return PTR_ERR(ns_phy);

	if (!parent_id) {
		if (!dev->phydev) {
			err = phy_connect_direct(dev, ns_phy->phy, nsim_adjust_link,
						 PHY_INTERFACE_MODE_NA);
			if (err)
				return err;

			phy_attached_info(ns_phy->phy);

			phy_start(ns_phy->phy);
		} else {
			phy_link_topo_add_phy(dev, ns_phy->phy, PHY_UPSTREAM_MAC, dev);
		}
	} else {
		pphy = phy_link_topo_get_phy(dev, parent_id);
		if (!pphy)
			return -EINVAL;

		phy_link_topo_add_phy(dev, ns_phy->phy, PHY_UPSTREAM_PHY, pphy);
	}

	nsim_phy_debugfs_create(ns->nsim_dev_port, ns_phy);

	list_add(&ns_phy->node, &ns->nsim_dev->phy_list);

	return count;
}

static const struct file_operations nsim_phy_add_fops = {
	.open = simple_open,
	.write = nsim_phy_add_write,
	.llseek = generic_file_llseek,
	.owner = THIS_MODULE,
};

static ssize_t
nsim_phy_del_write(struct file *file, const char __user *data,
		   size_t count, loff_t *ppos)
{
	struct net_device *dev = file->private_data;
	struct nsim_phy_device *ns_phy;
	struct phy_device *phydev;
	u32 phy_index;
	char buf[10];
	ssize_t ret;

	if (*ppos != 0)
		return 0;

	if (count >= sizeof(buf))
		return -ENOSPC;

	ret = copy_from_user(buf, data, count);
	if (ret)
		return -EFAULT;
	buf[count] = '\0';

	ret = kstrtouint(buf, 10, &phy_index);
	if (ret)
		return -EINVAL;

	phydev = phy_link_topo_get_phy(dev, phy_index);
	if (!phydev)
		return -ENODEV;

	ns_phy = phydev->priv;

	if (dev->phydev && dev->phydev == phydev) {
		phy_stop(phydev);
		phy_detach(phydev);
	} else {
		phy_link_topo_del_phy(dev, phydev);
	}

	nsim_phy_destroy(ns_phy);

	return count;
}

static const struct file_operations nsim_phy_del_fops = {
	.open = simple_open,
	.write = nsim_phy_del_write,
	.llseek = generic_file_llseek,
	.owner = THIS_MODULE,
};

void nsim_phy_init(struct netdevsim *ns)
{
	debugfs_create_file("phy_add", 0200, ns->nsim_dev_port->ddir,
			    ns->netdev, &nsim_phy_add_fops);

	debugfs_create_file("phy_del", 0200, ns->nsim_dev_port->ddir,
			    ns->netdev, &nsim_phy_del_fops);
}

void nsim_phy_teardown(struct netdevsim *ns)
{
	struct nsim_phy_device *ns_phy, *pos;

	list_for_each_entry_safe(ns_phy, pos, &ns->nsim_dev->phy_list, node)
		nsim_phy_destroy(ns_phy);
}

int __init nsim_phy_drv_register(void)
{
	return phy_drivers_register(nsim_virtual_phy_drv,
				    ARRAY_SIZE(nsim_virtual_phy_drv),
				    THIS_MODULE);
}

void __exit nsim_phy_drv_unregister(void)
{
	phy_drivers_unregister(nsim_virtual_phy_drv,
			       ARRAY_SIZE(nsim_virtual_phy_drv));
}
