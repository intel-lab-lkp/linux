// SPDX-License-Identifier: GPL-2.0+
/*
 * Microchip's LAN865x 10BASE-T1S MAC-PHY driver
 *
 * Author: Parthiban Veerasooran <parthiban.veerasooran@microchip.com>
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/etherdevice.h>
#include <linux/mdio.h>
#include <linux/phy.h>
#include <linux/of.h>
#include <linux/oa_tc6.h>

#define DRV_NAME		"lan865x"
#define DRV_VERSION		"0.1"

#define REG_STDR_RESET		0x00000003
#define REG_MAC_ADDR_BO		0x00010022
#define REG_MAC_ADDR_L		0x00010024
#define REG_MAC_ADDR_H		0x00010025
#define REG_MAC_NW_CTRL         0x00010000
#define REG_MAC_NW_CONFIG	0x00010001
#define REG_MAC_HASHL		0x00010020
#define REG_MAC_HASHH		0x00010021
#define REG_MAC_ADDR_BO		0x00010022
#define REG_MAC_ADDR_L		0x00010024
#define REG_MAC_ADDR_H		0x00010025

#define CCS_Q0_TX_CFG		0x000A0081
#define CCS_Q0_RX_CFG		0x000A0082

/* Buffer configuration for 32-bytes chunk payload */
#define CCS_Q0_TX_CFG_32	0x70000000
#define CCS_Q0_RX_CFG_32	0x30000C00

#define NW_RX_STATUS		BIT(2)
#define NW_TX_STATUS		BIT(3)
#define NW_DISABLE		0x0

#define MAC_PROMISCUOUS_MODE	BIT(4)
#define MAC_MULTICAST_MODE	BIT(6)
#define MAC_UNICAST_MODE	BIT(7)

#define TX_TIMEOUT		(4 * HZ)
#define LAN865X_MSG_DEFAULT	\
	(NETIF_MSG_PROBE | NETIF_MSG_IFUP | NETIF_MSG_IFDOWN | NETIF_MSG_LINK)

struct lan865x_priv {
	struct net_device *netdev;
	struct spi_device *spi;
	struct oa_tc6 *tc6;
	struct mii_bus *mdiobus;
	struct phy_device *phydev;
	struct device *dev;
	u32 msg_enable;
	bool txcte;
	bool rxcte;
	u32 cps;
	bool protected;
};

static void lan865x_handle_link_change(struct net_device *netdev)
{
	struct lan865x_priv *priv = netdev_priv(netdev);

	phy_print_status(priv->phydev);
}

static int lan865x_mdiobus_read(struct mii_bus *bus, int phy_id, int idx)
{
	struct lan865x_priv *priv = bus->priv;
	u32 regval;
	bool ret;

	ret = oa_tc6_read_register(priv->tc6, 0xFF00 | (idx & 0xFF), &regval, 1);
	if (ret)
		return -ENODEV;

	return regval;
}

static int lan865x_mdiobus_write(struct mii_bus *bus, int phy_id, int idx,
				 u16 regval)
{
	struct lan865x_priv *priv = bus->priv;
	u32 value = regval;
	bool ret;

	ret = oa_tc6_write_register(priv->tc6, 0xFF00 | (idx & 0xFF), &value, 1);
	if (ret)
		return -ENODEV;

	return 0;
}

static int lan865x_phy_init(struct lan865x_priv *priv)
{
	int ret;

	priv->mdiobus = mdiobus_alloc();
	if (!priv->mdiobus) {
		netdev_err(priv->netdev, "MDIO bus alloc failed\n");
		return -ENODEV;
	}

	priv->mdiobus->phy_mask = ~(u32)BIT(1);
	priv->mdiobus->priv = priv;
	priv->mdiobus->read = lan865x_mdiobus_read;
	priv->mdiobus->write = lan865x_mdiobus_write;
	priv->mdiobus->name = "lan865x-mdiobus";
	priv->mdiobus->parent = priv->dev;

	snprintf(priv->mdiobus->id, ARRAY_SIZE(priv->mdiobus->id),
		 "%s", dev_name(&priv->spi->dev));

	ret = mdiobus_register(priv->mdiobus);
	if (ret) {
		netdev_err(priv->netdev, "Could not register MDIO bus\n");
		mdiobus_free(priv->mdiobus);
		return ret;
	}
	priv->phydev = phy_find_first(priv->mdiobus);
	if (!priv->phydev) {
		netdev_err(priv->netdev, "No PHY found\n");
		mdiobus_unregister(priv->mdiobus);
		mdiobus_free(priv->mdiobus);
		return -ENODEV;
	}
	priv->phydev->is_internal = true;
	ret = phy_connect_direct(priv->netdev, priv->phydev,
				 &lan865x_handle_link_change,
				 PHY_INTERFACE_MODE_INTERNAL);
	if (ret) {
		netdev_err(priv->netdev, "Can't attach PHY to %s\n", priv->mdiobus->id);
		return ret;
	}
	phy_attached_info(priv->phydev);
	return ret;
}

static int lan865x_set_hw_macaddr(struct net_device *netdev)
{
	u32 regval;
	bool ret;
	struct lan865x_priv *priv = netdev_priv(netdev);
	const u8 *mac = netdev->dev_addr;

	ret = oa_tc6_read_register(priv->tc6, REG_MAC_NW_CTRL, &regval, 1);
	if (ret)
		goto error_mac;
	if ((regval & NW_TX_STATUS) | (regval & NW_RX_STATUS)) {
		if (netif_msg_drv(priv))
			netdev_warn(netdev, "Hardware must be disabled for MAC setting\n");
		return -EBUSY;
	}
	/* MAC address setting */
	regval = (mac[3] << 24) | (mac[2] << 16) | (mac[1] << 8) |
		mac[0];
	ret = oa_tc6_write_register(priv->tc6, REG_MAC_ADDR_L, &regval, 1);
	if (ret)
		goto error_mac;

	regval = (mac[5] << 8) | mac[4];
	ret = oa_tc6_write_register(priv->tc6, REG_MAC_ADDR_H, &regval, 1);
	if (ret)
		goto error_mac;

	regval = (mac[5] << 24) | (mac[4] << 16) |
		(mac[3] << 8) | mac[2];
	ret = oa_tc6_write_register(priv->tc6, REG_MAC_ADDR_BO, &regval, 1);
	if (ret)
		goto error_mac;

	return 0;

error_mac:
	return -ENODEV;
}

static int
lan865x_set_link_ksettings(struct net_device *netdev,
			   const struct ethtool_link_ksettings *cmd)
{
	struct lan865x_priv *priv = netdev_priv(netdev);
	int ret = 0;

	if (cmd->base.autoneg != AUTONEG_DISABLE ||
	    cmd->base.speed != SPEED_10 || cmd->base.duplex != DUPLEX_HALF) {
		if (netif_msg_link(priv))
			netdev_warn(netdev, "Unsupported link setting");
		ret = -EOPNOTSUPP;
	} else {
		if (netif_msg_link(priv))
			netdev_warn(netdev, "Hardware must be disabled to set link mode");
		ret = -EBUSY;
	}
	return ret;
}

static int
lan865x_get_link_ksettings(struct net_device *netdev,
			   struct ethtool_link_ksettings *cmd)
{
	ethtool_link_ksettings_zero_link_mode(cmd, supported);
	ethtool_link_ksettings_add_link_mode(cmd, supported, 10baseT_Half);
	ethtool_link_ksettings_add_link_mode(cmd, supported, TP);

	cmd->base.speed = SPEED_10;
	cmd->base.duplex = DUPLEX_HALF;
	cmd->base.port	= PORT_TP;
	cmd->base.autoneg = AUTONEG_DISABLE;

	return 0;
}

static void lan865x_set_msglevel(struct net_device *netdev, u32 val)
{
	struct lan865x_priv *priv = netdev_priv(netdev);

	priv->msg_enable = val;
}

static u32 lan865x_get_msglevel(struct net_device *netdev)
{
	struct lan865x_priv *priv = netdev_priv(netdev);

	return priv->msg_enable;
}

static void
lan865x_get_drvinfo(struct net_device *netdev, struct ethtool_drvinfo *info)
{
	strscpy(info->driver, DRV_NAME, sizeof(info->driver));
	strscpy(info->version, DRV_VERSION, sizeof(info->version));
	strscpy(info->bus_info,
		dev_name(netdev->dev.parent), sizeof(info->bus_info));
}

static const struct ethtool_ops lan865x_ethtool_ops = {
	.get_drvinfo	= lan865x_get_drvinfo,
	.get_msglevel	= lan865x_get_msglevel,
	.set_msglevel	= lan865x_set_msglevel,
	.get_link_ksettings = lan865x_get_link_ksettings,
	.set_link_ksettings = lan865x_set_link_ksettings,
};

static void lan865x_tx_timeout(struct net_device *netdev, unsigned int txqueue)
{
	netdev->stats.tx_errors++;
}

static int lan865x_set_mac_address(struct net_device *netdev, void *addr)
{
	struct sockaddr *address = addr;

	if (netif_running(netdev))
		return -EBUSY;
	if (!is_valid_ether_addr(address->sa_data))
		return -EADDRNOTAVAIL;

	eth_hw_addr_set(netdev, address->sa_data);
	return lan865x_set_hw_macaddr(netdev);
}

static u32 lan865x_hash(u8 addr[ETH_ALEN])
{
	return (ether_crc(ETH_ALEN, addr) >> 26) & 0x3f;
}

static void lan865x_set_multicast_list(struct net_device *netdev)
{
	struct lan865x_priv *priv = netdev_priv(netdev);
	u32 regval = 0;

	if (netdev->flags & IFF_PROMISC) {
		/* Enabling promiscuous mode */
		regval |= MAC_PROMISCUOUS_MODE;
		regval &= (~MAC_MULTICAST_MODE);
		regval &= (~MAC_UNICAST_MODE);
	} else if (netdev->flags & IFF_ALLMULTI) {
		/* Enabling all multicast mode */
		regval &= (~MAC_PROMISCUOUS_MODE);
		regval |= MAC_MULTICAST_MODE;
		regval &= (~MAC_UNICAST_MODE);
	} else if (!netdev_mc_empty(netdev)) {
		/* Enabling specific multicast addresses */
		struct netdev_hw_addr *ha;
		u32 hash_lo = 0;
		u32 hash_hi = 0;

		netdev_for_each_mc_addr(ha, netdev) {
			u32 bit_num = lan865x_hash(ha->addr);
			u32 mask = 1 << (bit_num & 0x1f);

			if (bit_num & 0x20)
				hash_hi |= mask;
			else
				hash_lo |= mask;
		}
		if (oa_tc6_write_register(priv->tc6, REG_MAC_HASHH, &hash_hi, 1)) {
			if (netif_msg_timer(priv))
				netdev_err(netdev, "Failed to write reg_hashh");
			return;
		}
		if (oa_tc6_write_register(priv->tc6, REG_MAC_HASHL, &hash_lo, 1)) {
			if (netif_msg_timer(priv))
				netdev_err(netdev, "Failed to write reg_hashl");
			return;
		}
		regval &= (~MAC_PROMISCUOUS_MODE);
		regval &= (~MAC_MULTICAST_MODE);
		regval |= MAC_UNICAST_MODE;
	} else {
		/* enabling local mac address only */
		if (oa_tc6_write_register(priv->tc6, REG_MAC_HASHH, &regval, 1)) {
			if (netif_msg_timer(priv))
				netdev_err(netdev, "Failed to write reg_hashh");
			return;
		}
		if (oa_tc6_write_register(priv->tc6, REG_MAC_HASHL, &regval, 1)) {
			if (netif_msg_timer(priv))
				netdev_err(netdev, "Failed to write reg_hashl");
			return;
		}
	}
	if (oa_tc6_write_register(priv->tc6, REG_MAC_NW_CONFIG, &regval, 1)) {
		if (netif_msg_timer(priv))
			netdev_err(netdev, "Failed to enable promiscuous mode");
	}
}

static netdev_tx_t lan865x_send_packet(struct sk_buff *skb,
				       struct net_device *netdev)
{
	struct lan865x_priv *priv = netdev_priv(netdev);

	return oa_tc6_send_eth_pkt(priv->tc6, skb);
}

static int lan865x_hw_disable(struct lan865x_priv *priv)
{
	u32 regval = NW_DISABLE;

	if (oa_tc6_write_register(priv->tc6, REG_MAC_NW_CTRL, &regval, 1))
		return -ENODEV;

	return 0;
}

static int lan865x_net_close(struct net_device *netdev)
{
	struct lan865x_priv *priv = netdev_priv(netdev);
	int ret;

	netif_stop_queue(netdev);
	ret = lan865x_hw_disable(priv);
	if (ret) {
		if (netif_msg_ifup(priv))
			netdev_err(netdev, "Failed to disable the hardware\n");
		return ret;
	}

	return 0;
}

static int lan865x_hw_enable(struct lan865x_priv *priv)
{
	u32 regval = NW_TX_STATUS | NW_RX_STATUS;

	if (oa_tc6_write_register(priv->tc6, REG_MAC_NW_CTRL, &regval, 1))
		return -ENODEV;

	return 0;
}

static int lan865x_net_open(struct net_device *netdev)
{
	struct lan865x_priv *priv = netdev_priv(netdev);
	int ret;

	if (!is_valid_ether_addr(netdev->dev_addr)) {
		if (netif_msg_ifup(priv))
			netdev_err(netdev, "Invalid MAC address %pm", netdev->dev_addr);
		return -EADDRNOTAVAIL;
	}
	if (lan865x_hw_disable(priv)) {
		if (netif_msg_ifup(priv))
			netdev_err(netdev, "Failed to disable the hardware\n");
		return -ENODEV;
	}
	ret = lan865x_set_hw_macaddr(netdev);
	if (ret != 0)
		return ret;

	if (lan865x_hw_enable(priv) != 0) {
		if (netif_msg_ifup(priv))
			netdev_err(netdev, "Failed to enable hardware\n");
		return -ENODEV;
	}
	netif_start_queue(netdev);

	return 0;
}

static const struct net_device_ops lan865x_netdev_ops = {
	.ndo_open		= lan865x_net_open,
	.ndo_stop		= lan865x_net_close,
	.ndo_start_xmit		= lan865x_send_packet,
	.ndo_set_rx_mode	= lan865x_set_multicast_list,
	.ndo_set_mac_address	= lan865x_set_mac_address,
	.ndo_tx_timeout		= lan865x_tx_timeout,
	.ndo_validate_addr	= eth_validate_addr,
};

static int lan865x_get_dt_data(struct lan865x_priv *priv)
{
	struct spi_device *spi = priv->spi;
	int ret;

	if (of_property_present(spi->dev.of_node, "oa-chunk-size")) {
		ret = of_property_read_u32(spi->dev.of_node, "oa-chunk-size",
					   &priv->cps);
		if (ret < 0)
			return ret;
	} else {
		priv->cps = 64;
		dev_info(&spi->dev, "Property oa-chunk-size is not found in dt and proceeding with the size 64\n");
	}

	if (of_property_present(spi->dev.of_node, "oa-tx-cut-through"))
		priv->txcte = true;
	else
		dev_info(&spi->dev, "Property oa-tx-cut-through is not found in dt and proceeding with tx store and forward mode\n");

	if (of_property_present(spi->dev.of_node, "oa-rx-cut-through"))
		priv->rxcte = true;
	else
		dev_info(&spi->dev, "Property oa-rx-cut-through is not found in dt and proceeding with rx store and forward mode\n");

	if (of_property_present(spi->dev.of_node, "oa-protected"))
		priv->protected = true;
	else
		dev_info(&spi->dev, "Property oa-protected is not found in dt and proceeding with protection enabled\n");

	return 0;
}

static int lan865x_probe(struct spi_device *spi)
{
	struct net_device *netdev;
	struct lan865x_priv *priv;
	u32 regval;
	int ret;

	netdev = alloc_etherdev(sizeof(struct lan865x_priv));
	if (!netdev)
		return -ENOMEM;

	priv = netdev_priv(netdev);
	priv->netdev = netdev;
	priv->spi = spi;
	priv->msg_enable = 0;
	spi_set_drvdata(spi, priv);
	SET_NETDEV_DEV(netdev, &spi->dev);

	ret = lan865x_get_dt_data(priv);
	if (ret)
		return ret;

	spi->rt = true;
	spi_setup(spi);

	priv->tc6 = oa_tc6_init(spi, netdev);
	if (!priv->tc6) {
		ret = -ENOMEM;
		goto error_oa_tc6_init;
	}

	if (priv->cps == 32) {
		regval = CCS_Q0_TX_CFG_32;
		ret = oa_tc6_write_register(priv->tc6, CCS_Q0_TX_CFG, &regval, 1);
		if (ret)
			return ret;

		regval = CCS_Q0_RX_CFG_32;
		ret = oa_tc6_write_register(priv->tc6, CCS_Q0_RX_CFG, &regval, 1);
		if (ret)
			return ret;
	}

	if (oa_tc6_configure(priv->tc6, priv->cps, priv->protected, priv->txcte,
			     priv->rxcte))
		goto err_macphy_config;

	ret = lan865x_phy_init(priv);
	if (ret)
		goto error_phy;

	if (device_get_ethdev_address(&spi->dev, netdev))
		eth_hw_addr_random(netdev);

	ret = lan865x_set_hw_macaddr(netdev);
	if (ret) {
		if (netif_msg_probe(priv))
			dev_err(&spi->dev, "Failed to configure MAC");
		goto error_set_mac;
	}

	netdev->if_port = IF_PORT_10BASET;
	netdev->irq = spi->irq;
	netdev->netdev_ops = &lan865x_netdev_ops;
	netdev->watchdog_timeo = TX_TIMEOUT;
	netdev->ethtool_ops = &lan865x_ethtool_ops;
	ret = register_netdev(netdev);
	if (ret) {
		if (netif_msg_probe(priv))
			dev_err(&spi->dev, "Register netdev failed (ret = %d)",
				ret);
		goto error_netdev_register;
	}

	phy_start(priv->phydev);
	return 0;

error_netdev_register:
error_set_mac:
	phy_disconnect(priv->phydev);
	mdiobus_unregister(priv->mdiobus);
	mdiobus_free(priv->mdiobus);
error_phy:
err_macphy_config:
	oa_tc6_deinit(priv->tc6);
error_oa_tc6_init:
	free_netdev(priv->netdev);
	return ret;
}

static void lan865x_remove(struct spi_device *spi)
{
	struct lan865x_priv *priv = spi_get_drvdata(spi);

	phy_stop(priv->phydev);
	phy_disconnect(priv->phydev);
	mdiobus_unregister(priv->mdiobus);
	mdiobus_free(priv->mdiobus);
	unregister_netdev(priv->netdev);
	if (oa_tc6_deinit(priv->tc6))
		dev_err(&spi->dev, "Failed to deinitialize oa tc6\n");
	free_netdev(priv->netdev);
}

#ifdef CONFIG_OF
static const struct of_device_id lan865x_dt_ids[] = {
	{ .compatible = "microchip,lan865x" },
	{ /* Sentinel */ }
};
MODULE_DEVICE_TABLE(of, lan865x_dt_ids);
#endif

#ifdef CONFIG_ACPI
static const struct acpi_device_id lan865x_acpi_ids[] = {
	{ .id = "LAN865X",
	},
	{},
};
MODULE_DEVICE_TABLE(acpi, lan865x_acpi_ids);
#endif

static struct spi_driver lan865x_driver = {
	.driver = {
		.name = DRV_NAME,
#ifdef CONFIG_OF
		.of_match_table = lan865x_dt_ids,
#endif
#ifdef CONFIG_ACPI
		   .acpi_match_table = ACPI_PTR(lan865x_acpi_ids),
#endif
	 },
	.probe = lan865x_probe,
	.remove = lan865x_remove,
};
module_spi_driver(lan865x_driver);

MODULE_DESCRIPTION(DRV_NAME " 10Base-T1S MACPHY Ethernet Driver");
MODULE_AUTHOR("Parthiban Veerasooran <parthiban.veerasooran@microchip.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("spi:" DRV_NAME);
