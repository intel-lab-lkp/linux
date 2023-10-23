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

/* MAC Network Control Register */
#define LAN865X_MAC_NCR         0x00010000
#define LAN865X_TXEN		BIT(3) /* Transmit Enable */
#define LAN865X_RXEN		BIT(2) /* Receive Enable */
#define LAN865X_MAC_NCFGR	0x00010001 /* MAC Network Configuration Register */
#define LAN865X_MAC_HRB		0x00010020 /* MAC Hash Register Bottom */
#define LAN865X_MAC_HRT		0x00010021 /* MAC Hash Register Top */
#define LAN865X_MAC_SAB1	0x00010022 /* MAC Specific Address 1 Bottom Register */
#define LAN865X_MAC_SAT1	0x00010023 /* MAC Specific Address 1 Top Register */
/* Queue Transmit Configuration */
#define LAN865X_QTXCFG		0x000A0081
/* Queue Receive Configuration */
#define LAN865X_QRXCFG		0x000A0082
#define LAN865X_BUFSZ		GENMASK(22, 20) /* Buffer Size */

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
	u32 msg_enable;
	bool protected;
	bool txcte;
	bool rxcte;
	u32 cps;
};

static int lan865x_set_hw_macaddr(struct net_device *netdev)
{
	u32 regval;
	bool ret;
	struct lan865x_priv *priv = netdev_priv(netdev);
	const u8 *mac = netdev->dev_addr;

	ret = oa_tc6_read_register(priv->tc6, LAN865X_MAC_NCR, &regval);
	if (ret)
		goto error_mac;
	if ((regval & LAN865X_TXEN) | (regval & LAN865X_RXEN)) {
		if (netif_msg_drv(priv))
			netdev_warn(netdev, "Hardware must be disabled for MAC setting\n");
		return -EBUSY;
	}
	/* MAC address setting */
	regval = (mac[3] << 24) | (mac[2] << 16) | (mac[1] << 8) | mac[0];
	ret = oa_tc6_write_register(priv->tc6, LAN865X_MAC_SAB1, regval);
	if (ret)
		goto error_mac;

	regval = (mac[5] << 8) | mac[4];
	ret = oa_tc6_write_register(priv->tc6, LAN865X_MAC_SAT1, regval);
	if (ret)
		goto error_mac;

	return 0;

error_mac:
	return -ENODEV;
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
	strscpy(info->bus_info, dev_name(netdev->dev.parent),
		sizeof(info->bus_info));
}

static const struct ethtool_ops lan865x_ethtool_ops = {
	.get_drvinfo	= lan865x_get_drvinfo,
	.get_msglevel	= lan865x_get_msglevel,
	.set_msglevel	= lan865x_set_msglevel,
	.get_link_ksettings = phy_ethtool_get_link_ksettings,
	.set_link_ksettings = phy_ethtool_set_link_ksettings,
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
		if (oa_tc6_write_register(priv->tc6, LAN865X_MAC_HRT, hash_hi)) {
			if (netif_msg_timer(priv))
				netdev_err(netdev, "Failed to write reg_hashh");
			return;
		}
		if (oa_tc6_write_register(priv->tc6, LAN865X_MAC_HRB, hash_lo)) {
			if (netif_msg_timer(priv))
				netdev_err(netdev, "Failed to write reg_hashl");
			return;
		}
		regval &= (~MAC_PROMISCUOUS_MODE);
		regval &= (~MAC_MULTICAST_MODE);
		regval |= MAC_UNICAST_MODE;
	} else {
		/* enabling local mac address only */
		if (oa_tc6_write_register(priv->tc6, LAN865X_MAC_HRT, regval)) {
			if (netif_msg_timer(priv))
				netdev_err(netdev, "Failed to write reg_hashh");
			return;
		}
		if (oa_tc6_write_register(priv->tc6, LAN865X_MAC_HRB, regval)) {
			if (netif_msg_timer(priv))
				netdev_err(netdev, "Failed to write reg_hashl");
			return;
		}
	}
	if (oa_tc6_write_register(priv->tc6, LAN865X_MAC_NCFGR, regval)) {
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
	u32 regval;

	if (oa_tc6_read_register(priv->tc6, LAN865X_MAC_NCR, &regval))
		return -ENODEV;

	regval &= ~(LAN865X_TXEN | LAN865X_RXEN);

	if (oa_tc6_write_register(priv->tc6, LAN865X_MAC_NCR, regval))
		return -ENODEV;

	return 0;
}

static int lan865x_net_close(struct net_device *netdev)
{
	struct lan865x_priv *priv = netdev_priv(netdev);
	int ret;

	netif_stop_queue(netdev);
	if (netdev->phydev)
		phy_stop(netdev->phydev);
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
	u32 regval;

	if (oa_tc6_read_register(priv->tc6, LAN865X_MAC_NCR, &regval))
		return -ENODEV;

	regval |= LAN865X_TXEN | LAN865X_RXEN;

	if (oa_tc6_write_register(priv->tc6, LAN865X_MAC_NCR, regval))
		return -ENODEV;

	return 0;
}

static int lan865x_net_open(struct net_device *netdev)
{
	struct lan865x_priv *priv = netdev_priv(netdev);

	if (lan865x_hw_enable(priv) != 0) {
		if (netif_msg_ifup(priv))
			netdev_err(netdev, "Failed to enable hardware\n");
		return -ENODEV;
	}
	phy_start(netdev->phydev);
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

/* Configures the number of bytes allocated to each buffer in the
 * transmit/receive queue. LAN865x supports only 64 and 32 bytes cps and also 64
 * is the default value. So it is enough to configure the queue buffer size only
 * for 32 bytes. Generally cps can't be changed during run time and also it is
 * configured in the device tree. The values for the Tx/Rx queue buffer size are
 * taken from the LAN865x datasheet.
 */
static int lan865x_config_cps_buf(void *tc6, u32 cps)
{
	u32 regval;
	int ret;

	if (cps == 32) {
		ret = oa_tc6_read_register(tc6, LAN865X_QTXCFG, &regval);
		if (ret)
			return ret;

		regval &= ~LAN865X_BUFSZ;
		regval |= FIELD_PREP(LAN865X_BUFSZ, 0x0);

		ret = oa_tc6_write_register(tc6, LAN865X_QTXCFG, regval);
		if (ret)
			return ret;

		ret = oa_tc6_read_register(tc6, LAN865X_QRXCFG, &regval);
		if (ret)
			return ret;

		regval &= ~LAN865X_BUFSZ;
		regval |= FIELD_PREP(LAN865X_BUFSZ, 0x0);

		ret = oa_tc6_write_register(tc6, LAN865X_QRXCFG, regval);
		if (ret)
			return ret;
	}

	return 0;
}

static int lan865x_probe(struct spi_device *spi)
{
	struct net_device *netdev;
	struct lan865x_priv *priv;
	int ret;

	netdev = alloc_etherdev(sizeof(struct lan865x_priv));
	if (!netdev)
		return -ENOMEM;

	priv = netdev_priv(netdev);
	priv->netdev = netdev;
	priv->spi = spi;
	priv->msg_enable = 0;
	spi_set_drvdata(spi, priv);

	priv->tc6 = oa_tc6_init(spi, netdev, lan865x_config_cps_buf);
	if (!priv->tc6) {
		ret = -ENODEV;
		goto err_oa_tc6_init;
	}
	if (device_get_ethdev_address(&spi->dev, netdev))
		eth_hw_addr_random(netdev);

	ret = lan865x_set_hw_macaddr(netdev);
	if (ret) {
		if (netif_msg_probe(priv))
			dev_err(&spi->dev, "Failed to configure MAC");
		goto err_config;
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
		goto err_config;
	}

	return 0;

err_config:
	oa_tc6_exit(priv->tc6);
err_oa_tc6_init:
	free_netdev(priv->netdev);
	return ret;
}

static void lan865x_remove(struct spi_device *spi)
{
	struct lan865x_priv *priv = spi_get_drvdata(spi);

	oa_tc6_exit(priv->tc6);
	unregister_netdev(priv->netdev);
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
