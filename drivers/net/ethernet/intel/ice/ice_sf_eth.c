// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2023, Intel Corporation. */

#include "ice.h"
#include "ice_lib.h"
#include "ice_txrx.h"
#include "ice_fltr.h"
#include "ice_sf_eth.h"
#include "devlink/ice_devlink_port.h"

static const struct net_device_ops ice_sf_netdev_ops = {
	.ndo_open = ice_open,
	.ndo_stop = ice_stop,
	.ndo_start_xmit = ice_start_xmit,
	.ndo_vlan_rx_add_vid = ice_vlan_rx_add_vid,
	.ndo_vlan_rx_kill_vid = ice_vlan_rx_kill_vid,
	.ndo_change_mtu = ice_change_mtu,
	.ndo_get_stats64 = ice_get_stats64,
	.ndo_tx_timeout = ice_tx_timeout,
	.ndo_bpf = ice_xdp,
	.ndo_xdp_xmit = ice_xdp_xmit,
	.ndo_xsk_wakeup = ice_xsk_wakeup,
};

/**
 * ice_sf_cfg_netdev - Allocate, configure and register a netdev
 * @dyn_port: subfunction associated with configured netdev
 *
 * Returns 0 on success, negative value on failure
 */
static int ice_sf_cfg_netdev(struct ice_dynamic_port *dyn_port)
{
	struct net_device *netdev;
	struct ice_vsi *vsi = dyn_port->vsi;
	struct ice_netdev_priv *np;
	int err;

	netdev = alloc_etherdev_mqs(sizeof(*np), vsi->alloc_txq,
				    vsi->alloc_rxq);
	if (!netdev)
		return -ENOMEM;

	SET_NETDEV_DEV(netdev, &vsi->back->pdev->dev);
	set_bit(ICE_VSI_NETDEV_ALLOCD, vsi->state);
	vsi->netdev = netdev;
	np = netdev_priv(netdev);
	np->vsi = vsi;

	ice_set_netdev_features(netdev);

	netdev->xdp_features = NETDEV_XDP_ACT_BASIC | NETDEV_XDP_ACT_REDIRECT |
			       NETDEV_XDP_ACT_XSK_ZEROCOPY |
			       NETDEV_XDP_ACT_RX_SG;

	eth_hw_addr_set(netdev, dyn_port->hw_addr);
	ether_addr_copy(netdev->perm_addr, dyn_port->hw_addr);
	netdev->netdev_ops = &ice_sf_netdev_ops;
	ice_set_ethtool_sf_ops(netdev);
	SET_NETDEV_DEVLINK_PORT(netdev, &dyn_port->devlink_port);

	err = register_netdev(netdev);
	if (err) {
		free_netdev(netdev);
		vsi->netdev = NULL;
		return -ENOMEM;
	}
	set_bit(ICE_VSI_NETDEV_REGISTERED, vsi->state);
	netif_carrier_off(netdev);
	netif_tx_stop_all_queues(netdev);

	return 0;
}

/**
 * ice_sf_eth_activate - Activate Ethernet subfunction port
 * @dyn_port: the dynamic port instance for this subfunction
 * @extack: extack for reporting error messages
 *
 * Setups netdev resources and filters for a subfunction.
 *
 * Return: zero on success or an error code on failure.
 */
int
ice_sf_eth_activate(struct ice_dynamic_port *dyn_port,
		    struct netlink_ext_ack *extack)
{
	struct ice_vsi_cfg_params params = {};
	struct ice_vsi *vsi = dyn_port->vsi;
	struct ice_pf *pf = dyn_port->pf;
	int err;

	params.type = ICE_VSI_SF;
	params.pi = pf->hw.port_info;
	params.flags = ICE_VSI_FLAG_INIT;

	err = ice_vsi_cfg(vsi, &params);
	if (err) {
		NL_SET_ERR_MSG_MOD(extack, "Subfunction vsi config failed");
		return err;
	}

	err = ice_sf_cfg_netdev(dyn_port);
	if (err) {
		NL_SET_ERR_MSG_MOD(extack, "Subfunction netdev config failed");
		goto err_vsi_decfg;
	}

	err = ice_fltr_add_mac_and_broadcast(vsi, vsi->netdev->dev_addr,
					     ICE_FWD_TO_VSI);
	if (err)
		NL_SET_ERR_MSG_MOD(extack, "can't add MAC filters for subfunction VSI");

	ice_napi_add(vsi);

	return err;

err_vsi_decfg:
	ice_vsi_decfg(vsi);
	return err;
}

/**
 * ice_sf_eth_deactivate - Deactivate subfunction
 * @dyn_port: the dynamic port instance for this subfunction
 *
 * Free netdev resources and filters for a subfunction.
 */
void ice_sf_eth_deactivate(struct ice_dynamic_port *dyn_port)
{
	struct ice_vsi *vsi = dyn_port->vsi;

	ice_vsi_close(vsi);
	unregister_netdev(vsi->netdev);
	clear_bit(ICE_VSI_NETDEV_REGISTERED, vsi->state);
	free_netdev(vsi->netdev);
	clear_bit(ICE_VSI_NETDEV_ALLOCD, vsi->state);
	vsi->netdev = NULL;
	ice_vsi_decfg(vsi);
}
