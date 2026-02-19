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
	int ret;

	/* Register PTP interrupts */
	ret = tsn_ptp_init_and_register_irqs(emac);
	if (ret) {
		dev_err(emac->common->dev,
			"EMAC %d: Failed to register PTP interrupts: %d\n",
			emac->emac_num, ret);
		return ret;
	}

	if (emac->phy_node) {
		phydev = of_phy_connect(emac->ndev, emac->phy_node,
					tsn_adjust_link_tsn,
					emac->phy_flags,
					emac->phy_mode);
		if (!phydev) {
			dev_err(emac->common->dev, "of_phy_connect() failed\n");
			tsn_ptp_unregister_irqs(emac);
			return -ENODEV;
		}
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
	struct tsn_emac *emac = netdev_priv(ndev);

	if (ndev->phydev)
		phy_disconnect(ndev->phydev);

	tsn_ptp_unregister_irqs(emac);

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
	u16 queue = skb_get_queue_mapping(skb);

	if (queue == emac->common->num_priorities)
		return tsn_ptp_xmit(skb, emac);

	return tsn_start_xmit_dmaengine(emac->common, skb, ndev);
}

/**
 * emac_select_queue - select queue for packet transmission
 * @ndev:	Pointer to net_device structure
 * @skb:	socket buffer containing the packet
 * @sb_dev:	fallback device (not used)
 *
 * Return:	Queue index for PTP packets or default queue
 *
 * This function selects the appropriate queue for packet transmission.
 * PTP packets (ETH_P_1588) are directed to a dedicated PTP queue.
 */
static u16 emac_select_queue(struct net_device *ndev,
			     struct sk_buff *skb,
			     struct net_device *sb_dev)
{
	struct tsn_emac *emac = netdev_priv(ndev);
	struct tsn_priv *common = emac->common;
	struct ethhdr *hdr = (struct ethhdr *)skb->data;

	/* PTP over Ethernet (Layer 2) */
	if (hdr->h_proto == htons(ETH_P_1588))
		return common->num_priorities;
	return netdev_pick_tx(ndev, skb, sb_dev);
}

/**
 *  emac_set_timestamp_mode - sets up the hardware for the requested mode
 *  @emac:	Pointer to TSN EMAC structure
 *  @config:	the hwtstamp configuration requested
 *
 * Return:	0 on success, Negative value on errors
 */
static int emac_set_timestamp_mode(struct tsn_emac *emac,
				   struct hwtstamp_config *config)
{
	/* reserved for future extensions */
	if (config->flags)
		return -EINVAL;

	if (config->tx_type < HWTSTAMP_TX_OFF ||
	    config->tx_type > HWTSTAMP_TX_ON)
		return -ERANGE;

	emac->ptp_ts_type = config->tx_type;

	/* On RX always timestamp everything */
	switch (config->rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		emac->current_rx_filter = HWTSTAMP_FILTER_NONE;
		break;
	case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
		emac->current_rx_filter = HWTSTAMP_FILTER_PTP_V2_L2_EVENT;
		config->rx_filter = HWTSTAMP_FILTER_PTP_V2_L2_EVENT;
		break;
	default:
		return -ERANGE;
	}
	return 0;
}

/**
 * emac_set_ts_config - user entry point for timestamp mode
 * @emac:	Pointer to TSN EMAC structure
 * @ifr:	ioctl data
 *
 * Set hardware to the requested more. If unsupported return an error
 * with no changes. Otherwise, store the mode for future reference
 *
 * Return:	0 on success, Negative value on errors
 */
static int emac_set_ts_config(struct tsn_emac *emac, struct ifreq *ifr)
{
	struct hwtstamp_config config;
	int err;

	if (copy_from_user(&config, ifr->ifr_data, sizeof(config)))
		return -EFAULT;

	err = emac_set_timestamp_mode(emac, &config);
	if (err)
		return err;

	/* save these settings for future reference */
	memcpy(&emac->tstamp_config, &config, sizeof(emac->tstamp_config));

	return copy_to_user(ifr->ifr_data, &config,
			sizeof(config)) ? -EFAULT : 0;
}

/**
 * emac_get_ts_config - return the current timestamp configuration
 * to the user
 * @emac:	pointer to TSN EMAC structure
 * @ifr:	ioctl data
 *
 * Return:	0 on success, Negative value on errors
 */
static int emac_get_ts_config(struct tsn_emac *emac, struct ifreq *ifr)
{
	struct hwtstamp_config *config = &emac->tstamp_config;

	return copy_to_user(ifr->ifr_data, config,
			    sizeof(*config)) ? -EFAULT : 0;
}

/**
 * emac_ioctl - Ioctl MII Interface
 * @dev:	Pointer to net_device structure
 * @rq:	ioctl request structure
 * @cmd:	ioctl command
 *
 * Return:	0 on success, Negative value on errors
 */
static int emac_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	struct tsn_emac *emac = netdev_priv(dev);

	if (!netif_running(dev))
		return -EINVAL;

	switch (cmd) {
	case SIOCGMIIPHY:
	case SIOCGMIIREG:
	case SIOCSMIIREG:
		if (!dev->phydev)
			return -EOPNOTSUPP;
		return phy_mii_ioctl(dev->phydev, rq, cmd);
	case SIOCSHWTSTAMP:
		return emac_set_ts_config(emac, rq);
	case SIOCGHWTSTAMP:
		return emac_get_ts_config(emac, rq);
	default:
		return -EOPNOTSUPP;
	}
}

static const struct net_device_ops emac_netdev_ops = {
	.ndo_open		= emac_open,
	.ndo_stop		= emac_stop,
	.ndo_start_xmit		= emac_start_xmit,
	.ndo_set_mac_address	= tsn_ndo_set_mac_address,
	.ndo_validate_addr	= emac_validate_addr,
	.ndo_select_queue	= emac_select_queue,
	.ndo_eth_ioctl		= emac_ioctl,
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

/**
 * emac_get_ts_info - Get timestamping and PTP information
 * @ndev: Pointer to net_device structure
 * @info: Pointer to ethtool_ts_info structure
 *
 * This function provides hardware timestamping capabilities and
 * PTP hardware clock index for ethtool -T command.
 *
 * Return: 0 on success
 */
static int emac_get_ts_info(struct net_device *ndev,
			    struct kernel_ethtool_ts_info *info)
{
	struct tsn_emac *emac = netdev_priv(ndev);
	struct tsn_priv *common = emac->common;

	info->so_timestamping = SOF_TIMESTAMPING_TX_HARDWARE |
				SOF_TIMESTAMPING_RX_HARDWARE |
				SOF_TIMESTAMPING_RAW_HARDWARE;

	info->tx_types = BIT(HWTSTAMP_TX_OFF) |
			 BIT(HWTSTAMP_TX_ON);

	info->rx_filters = BIT(HWTSTAMP_FILTER_NONE) |
			   BIT(HWTSTAMP_FILTER_PTP_V2_L2_EVENT);

	if (common->phc_index >= 0)
		info->phc_index = common->phc_index;
	else
		info->phc_index = -1;

	return 0;
}

static const struct ethtool_ops emac_ethtool_ops = {
	.get_drvinfo	= emac_get_drvinfo,
	.get_link	= ethtool_op_get_link,
	.get_link_ksettings	= phy_ethtool_get_link_ksettings,
	.set_link_ksettings	= phy_ethtool_set_link_ksettings,
	.get_ts_info		= emac_get_ts_info,
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

		ndev = alloc_etherdev_mqs(sizeof(*emac),
					  common->num_tx_queues + 1,
					  common->num_rx_queues);
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

		/* PTP timer initialization - ONLY for MAC 1 */
		if (emac->emac_num == TSN_TEMAC1) {
			ret = tsn_ptp_timer_init(emac, emac_np);
			if (ret) {
				dev_err(dev, "Failed to initialize PTP timer for EMAC %d: %d\n",
					emac->emac_num, ret);
				goto err_teardown_mdio;
			}
		}

		ret = tsn_ptp_get_irq_info(emac, emac_np);
		if (ret) {
			dev_err(dev, "Failed to get PTP IRQ info for EMAC %d: %d\n",
				emac->emac_num, ret);
			goto err_remove_ptp;
		}

		ret = register_netdev(ndev);
		if (ret) {
			dev_err(dev, "Failed to register net device for MAC %d\n", mac_id);
			goto err_remove_ptp;
		}

		common->emacs[array_idx] = emac;
		array_idx++;
		common->num_emacs = array_idx;
		continue;

err_remove_ptp:
		if (emac->emac_num == TSN_TEMAC1)
			tsn_ptp_timer_exit(emac);
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
		if (old->emac_num == TSN_TEMAC1)
			tsn_ptp_timer_exit(old);

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

		if (emac->emac_num == TSN_TEMAC1)
			tsn_ptp_timer_exit(emac);

		if (emac->phy_node) {
			tsn_mdio_teardown(emac);
			of_node_put(emac->phy_node);
		}
		free_netdev(emac->ndev);
		common->emacs[i] = NULL;
	}

	common->num_emacs = 0;
}
