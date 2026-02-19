// SPDX-License-Identifier: GPL-2.0
#include "xilinx_tsn.h"

#define DRIVER_NAME             "xilinx_tsn_emac"
#define DRIVER_DESCRIPTION      "Xilinx TSN driver"
#define DRIVER_VERSION          "1.0"

/**
 * tsn_adjust_link_tsn - Adjust link parameters
 * @ndev: Pointer to the net_device structure
 *
 * This function is called when the PHY link state changes. It configures
 * the EMAC link speed register based on the current PHY settings and
 * updates link status information.
 */
static void tsn_adjust_link_tsn(struct net_device *ndev)
{
	struct tsn_emac *emac = netdev_priv(ndev);
	struct phy_device *phy = ndev->phydev;
	u32 emmc_reg;

	if (!phy || emac->last_link == phy->link)
		return;

	if (phy->link) {
		emmc_reg = emac_ior(emac, TSN_EMMC_OFFSET);
		emmc_reg &= ~TSN_EMMC_LINKSPEED_MASK;

		switch (phy->speed) {
		case SPEED_1000:
			emmc_reg |= TSN_EMMC_LINKSPEED_1000;
			break;
		case SPEED_100:
			emmc_reg |= TSN_EMMC_LINKSPEED_100;
			break;
		default:
			dev_warn(&ndev->dev, "Unsupported speed: %d\n", phy->speed);
			break;
		}

		emac_iow(emac, TSN_EMMC_OFFSET, emmc_reg);
		dev_info(&ndev->dev, "Link up: %d Mbps, %s duplex\n",
			 phy->speed, phy->duplex ? "full" : "half");
	} else {
		dev_info(&ndev->dev, "Link down\n");
	}

	emac->last_link = phy->link;
}

/**
 * emac_open - Open the network interface
 * @ndev: Pointer to the net_device structure
 *
 * This function is called when the network interface is brought up.
 * It connects to the PHY device and starts the PHY if available.
 *
 * Return: 0 on success, negative error code on failure
 */
static int emac_open(struct net_device *ndev)
{
	struct tsn_emac *emac = netdev_priv(ndev);
	struct phy_device *phydev = NULL;

	if (emac->phy_node) {
		phydev = of_phy_connect(emac->ndev, emac->phy_node,
					tsn_adjust_link_tsn,
					emac->phy_flags,
					emac->phy_mode);
		if (!phydev)
			dev_err(emac->common->dev, "of_phy_connect() failed\n");
		else
			phy_start(phydev);
	}

	return 0;
}

/**
 * emac_stop - Stop the network interface
 * @ndev: Pointer to the net_device structure
 *
 * This function is called when the network interface is brought down.
 * It disconnects the PHY device to stop link monitoring.
 *
 * Return: 0 on success
 */
static int emac_stop(struct net_device *ndev)
{
	if (ndev->phydev)
		phy_disconnect(ndev->phydev);

	return 0;
}

/**
 * emac_validate_addr - Validate the MAC address
 * @ndev: Pointer to the net_device structure
 *
 * This function validates the current MAC address of the device.
 *
 * Return: 0 if address is valid, negative error code otherwise
 */
static int emac_validate_addr(struct net_device *ndev)
{
	return eth_validate_addr(ndev);
}

/**
 * emac_start_xmit - Transmit packet handler
 * @skb: Socket buffer containing the packet
 * @ndev: Pointer to the net_device structure
 *
 * This function handles packet transmission for EMAC interfaces.
 * Currently drops packets and updates statistics as EMAC is not
 * configured for actual transmission.
 *
 * Return: NETDEV_TX_OK always
 */
static netdev_tx_t emac_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct tsn_emac *emac = netdev_priv(ndev);

	return tsn_start_xmit_dmaengine(emac->common, skb, ndev);
}

static const struct net_device_ops emac_netdev_ops = {
	.ndo_open		= emac_open,
	.ndo_stop		= emac_stop,
	.ndo_start_xmit		= emac_start_xmit,
	.ndo_set_mac_address	= tsn_ndo_set_mac_address,
	.ndo_validate_addr	= emac_validate_addr,
};

/**
 * emac_get_drvinfo - Get various TSN Ethernet driver information.
 * @ndev:       Pointer to net_device structure
 * @ed:         Pointer to ethtool_drvinfo structure
 *
 * This implements ethtool command for getting the driver information.
 * Issue "ethtool -i ethX" under linux prompt to execute this function.
 */
static void emac_get_drvinfo(struct net_device *ndev,
			     struct ethtool_drvinfo *ed)
{
	strscpy(ed->driver, DRIVER_NAME, sizeof(ed->driver));
	strscpy(ed->version, DRIVER_VERSION, sizeof(ed->version));
}

static const struct ethtool_ops emac_ethtool_ops = {
	.get_drvinfo	= emac_get_drvinfo,
	.get_link	= ethtool_op_get_link,
	.get_link_ksettings	= phy_ethtool_get_link_ksettings,
	.set_link_ksettings	= phy_ethtool_set_link_ksettings,
};

/**
 * tsn_emac_init - Initialize TSN EMAC interfaces
 * @pdev: Platform device pointer
 *
 * This function initializes all EMAC interfaces found in the device tree.
 * For each EMAC, it allocates a network device, maps register regions,
 * sets up PHY connections, configures MDIO bus, and registers the
 * network interface with the kernel.
 *
 * Return: 0 on success, negative error code on failure
 */
int tsn_emac_init(struct platform_device *pdev)
{
	struct tsn_priv *common = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;
	struct device_node *emac_np;
	int ret, array_idx = 0;

	for_each_child_of_node(dev->of_node, emac_np) {
		struct net_device *ndev;
		struct tsn_emac *emac;
		u8 mac_addr[ETH_ALEN];
		struct resource res;
		u32 mac_id = 0;

		if (!of_node_name_eq(emac_np, "ethernet-mac"))
			continue;

		ret = of_property_read_u32(emac_np, "xlnx,mac-id", &mac_id);
		if (ret) {
			dev_err(dev, "Missing mandatory property 'xlnx,mac-id' for EMAC %d\n",
				array_idx + 1);
			of_node_put(emac_np);
			goto err_cleanup_all;
		}

		ndev = alloc_etherdev(sizeof(*emac));
		if (!ndev) {
			ret = -ENOMEM;
			of_node_put(emac_np);
			goto err_cleanup_all;
		}

		ret = of_address_to_resource(emac_np, 0, &res);
		if (ret) {
			dev_err_probe(dev, ret, "failed to get emac resource\n");
			goto err_free_ndev_put_node;
		}

		emac = netdev_priv(ndev);
		memset(emac, 0, sizeof(*emac));
		emac->ndev = ndev;
		emac->common = common;
		emac->regs_start = common->regs_start + res.start;
		emac->regs = common->regs + res.start;
		emac->emac_num = mac_id;
		/* basic netdev config */
		ndev->netdev_ops = &emac_netdev_ops;
		ndev->ethtool_ops = &emac_ethtool_ops;
		ndev->min_mtu = ETH_ZLEN - ETH_HLEN;
		ndev->max_mtu = ETH_DATA_LEN;
		SET_NETDEV_DEV(ndev, dev);

		/* Retrieve the MAC address */
		ret = of_get_mac_address(emac_np, mac_addr);
		if (ret == 0 && is_valid_ether_addr(mac_addr))
			eth_hw_addr_set(ndev, mac_addr);

		emac->phy_node = of_parse_phandle(emac_np, "phy-handle", 0);
		if (!emac->phy_node) {
			dev_err(&pdev->dev, "Failed to get 'phy-handle' from device tree\n");

		} else {
			ret = tsn_mdio_setup(emac, emac_np);
			if (ret) {
				dev_warn(&pdev->dev, "error registering MDIO bus for EMAC %d: %d\n",
					 mac_id, ret);
				goto err_put_phy_node;
			}
		}

		ret = register_netdev(ndev);
		if (ret) {
			dev_err(dev, "Failed to register net device for MAC %d\n", mac_id);
			goto err_teardown_mdio;
		}

		common->emacs[array_idx] = emac;
		array_idx++;
		common->num_emacs = array_idx;
		continue;

err_teardown_mdio:
		if (emac->phy_node)
			tsn_mdio_teardown(emac);
err_put_phy_node:
		if (emac->phy_node)
			of_node_put(emac->phy_node);
err_free_ndev_put_node:
		free_netdev(ndev);
		of_node_put(emac_np);
		dev_warn(dev, "EMAC %d initialization failed, rolling back\n", mac_id);
		goto err_cleanup_all;
	}

	if (array_idx == 0)
		return -ENODEV;

	return 0;

err_cleanup_all:
	/* Cleanup all initialized EMACs in reverse order */
	while (array_idx > 0) {
		struct tsn_emac *old = common->emacs[--array_idx];

		if (!old)
			continue;

		dev_info(dev, "Cleaning up MAC %u (array[%d])\n", old->emac_num, array_idx);

		unregister_netdev(old->ndev);

		if (old->phy_node) {
			tsn_mdio_teardown(old);
			of_node_put(old->phy_node);
		}

		free_netdev(old->ndev);
		common->emacs[array_idx] = NULL;
	}

	common->num_emacs = 0;

	return ret;
}

/**
 * tsn_emac_exit - Cleanup TSN EMAC interfaces
 * @pdev: Platform device pointer
 *
 * This function performs cleanup for all initialized EMAC interfaces.
 * It unregisters network devices, tears down MDIO buses, releases
 * PHY connections, and frees allocated memory for each EMAC instance.
 */
void tsn_emac_exit(struct platform_device *pdev)
{
	struct tsn_priv *common = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;
	int i;

	/* Cleanup only the EMACs that were actually initialized */
	for (i = 0; i < common->num_emacs; i++) {
		struct tsn_emac *emac = common->emacs[i];

		if (!emac)
			continue;

		dev_info(dev, "Cleaning up MAC %u (array[%d])\n", emac->emac_num, i);

		unregister_netdev(emac->ndev);
		if (emac->phy_node) {
			tsn_mdio_teardown(emac);
			of_node_put(emac->phy_node);
		}
		free_netdev(emac->ndev);
		common->emacs[i] = NULL;
	}

	common->num_emacs = 0;
}
