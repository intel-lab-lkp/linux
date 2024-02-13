// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2023, Intel Corporation. */

#include "ice.h"
#include "ice_lib.h"
#include "ice_txrx.h"
#include "ice_fltr.h"
#include "ice_sf_eth.h"
#include "ice_devlink.h"
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
 * @sf_dev: subfunction associated with configured netdev
 *
 * Returns 0 on success, negative value on failure
 */
static int ice_sf_cfg_netdev(struct ice_sf_dev *sf_dev)
{
	struct ice_dynamic_port *dyn_port = sf_dev->dyn_port;
	struct ice_vsi *vsi = dyn_port->vsi;
	struct ice_netdev_priv *np;
	struct net_device *netdev;
	int err;

	netdev = alloc_etherdev_mqs(sizeof(*np), vsi->alloc_txq,
				    vsi->alloc_rxq);
	if (!netdev)
		return -ENOMEM;

	SET_NETDEV_DEV(netdev, &sf_dev->adev.dev);
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
	SET_NETDEV_DEVLINK_PORT(netdev, &sf_dev->priv->devlink_port);

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

static const struct devlink_ops ice_sf_devlink_ops;

/**
 * ice_sf_dev_probe - subfunction driver probe function
 * @adev: pointer to the auxiliary device
 * @id: pointer to the auxiliary_device id
 *
 * Configure VSI and netdev resources for the subfunction device.
 *
 * Return: zero on success or an error code on failure.
 */
static int ice_sf_dev_probe(struct auxiliary_device *adev,
			    const struct auxiliary_device_id *id)
{
	struct ice_sf_dev *sf_dev = ice_adev_to_sf_dev(adev);
	struct ice_dynamic_port *dyn_port = sf_dev->dyn_port;
	struct ice_vsi_cfg_params params = {};
	struct ice_vsi *vsi = dyn_port->vsi;
	struct ice_pf *pf = dyn_port->pf;
	struct device *dev = &adev->dev;
	struct ice_sf_priv *priv;
	int err;

	params.type = ICE_VSI_SF;
	params.pi = pf->hw.port_info;
	params.flags = ICE_VSI_FLAG_INIT;

	priv = ice_devlink_alloc(&adev->dev, sizeof(struct ice_sf_priv),
				 &ice_sf_devlink_ops);
	if (!priv) {
		dev_err(dev, "Subfunction devlink alloc failed");
		return -ENOMEM;
	}

	priv->dev = sf_dev;
	sf_dev->priv = priv;

	devlink_register(priv_to_devlink(priv));

	err = ice_vsi_cfg(vsi, &params);
	if (err) {
		dev_err(dev, "Subfunction vsi config failed");
		return err;
	}
	vsi->sf = dyn_port;

	err = ice_devlink_create_sf_dev_port(sf_dev);
	if (err)
		dev_dbg(dev, "Cannot add ice virtual devlink port for subfunction");

	err = ice_sf_cfg_netdev(sf_dev);
	if (err) {
		dev_err(dev, "subfunction netdev config failed");
		goto err_vsi_decfg;
	}

	err = ice_fltr_add_mac_and_broadcast(vsi, vsi->netdev->dev_addr,
					     ICE_FWD_TO_VSI);

	if (err)
		dev_dbg(dev, "can't add MAC filters %pM for VSI %d\n",
			vsi->netdev->dev_addr, vsi->idx);

	dev_dbg(dev, "MAC %pM filter added for vsi %d\n", vsi->netdev->dev_addr,
		vsi->idx);
	ice_napi_add(vsi);

	return err;

err_vsi_decfg:
	ice_vsi_decfg(vsi);
	return err;
}

/**
 * ice_sf_dev_remove - subfunction driver remove function
 * @adev: pointer to the auxiliary device
 *
 * Deinitalize VSI and netdev resources for the subfunction device.
 */
static void ice_sf_dev_remove(struct auxiliary_device *adev)
{
	struct ice_sf_dev *sf_dev = ice_adev_to_sf_dev(adev);
	struct devlink *devlink = priv_to_devlink(sf_dev->priv);
	struct ice_dynamic_port *dyn_port = sf_dev->dyn_port;
	struct ice_vsi *vsi = dyn_port->vsi;

	ice_vsi_close(vsi);

	unregister_netdev(vsi->netdev);
	clear_bit(ICE_VSI_NETDEV_REGISTERED, vsi->state);
	devlink_port_unregister(&sf_dev->priv->devlink_port);
	free_netdev(vsi->netdev);
	vsi->netdev = NULL;
	clear_bit(ICE_VSI_NETDEV_ALLOCD, vsi->state);
	devlink_unregister(devlink);
	devlink_free(devlink);
	ice_vsi_decfg(vsi);
}

static const struct auxiliary_device_id ice_sf_dev_id_table[] = {
	{ .name = "ice.sf", },
	{ },
};

MODULE_DEVICE_TABLE(auxiliary, ice_sf_dev_id_table);

static struct auxiliary_driver ice_sf_driver = {
	.name = "sf",
	.probe = ice_sf_dev_probe,
	.remove = ice_sf_dev_remove,
	.id_table = ice_sf_dev_id_table
};

static DEFINE_XARRAY_ALLOC1(ice_sf_aux_id);

/**
 * ice_sf_driver_register - Register new auxiliary subfunction driver
 *
 * Return: zero on success or an error code on failure.
 */
int ice_sf_driver_register(void)
{
	return auxiliary_driver_register(&ice_sf_driver);
}

/**
 * ice_sf_driver_unregister - Unregister new auxiliary subfunction driver
 *
 * Return: zero on success or an error code on failure.
 */
void ice_sf_driver_unregister(void)
{
	auxiliary_driver_unregister(&ice_sf_driver);
}

/**
 * ice_sf_dev_release - Release device associated with auxiliary device
 * @device: pointer to the device
 *
 * Since most of the code for subfunction deactivation is handled in
 * the remove handler, here just free tracking resources.
 */
static void ice_sf_dev_release(struct device *device)
{
	struct auxiliary_device *adev = to_auxiliary_dev(device);
	struct ice_sf_dev *sf_dev = ice_adev_to_sf_dev(adev);

	xa_erase(&ice_sf_aux_id, adev->id);
	kfree(sf_dev);
}

static ssize_t
sfnum_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct devlink_port_attrs *attrs;
	struct auxiliary_device *adev;
	struct ice_sf_dev *sf_dev;

	adev = to_auxiliary_dev(dev);
	sf_dev = ice_adev_to_sf_dev(adev);
	attrs = &sf_dev->dyn_port->devlink_port.attrs;

	return sysfs_emit(buf, "%u\n", attrs->pci_sf.sf);
}

static DEVICE_ATTR_RO(sfnum);

static struct attribute *ice_sf_device_attrs[] = {
	&dev_attr_sfnum.attr,
	NULL,
};

static const struct attribute_group ice_sf_attr_group = {
	.attrs = ice_sf_device_attrs,
};

static const struct attribute_group *ice_sf_attr_groups[2] = {
	&ice_sf_attr_group,
	NULL
};

/**
 * ice_sf_eth_activate - Activate Ethernet subfunction port
 * @dyn_port: the dynamic port instance for this subfunction
 * @extack: extack for reporting error messages
 *
 * Activate the dynamic port as an Ethernet subfunction. Setup the netdev
 * resources associated and initialize the auxiliary device.
 *
 * Return: zero on success or an error code on failure.
 */
int
ice_sf_eth_activate(struct ice_dynamic_port *dyn_port,
		    struct netlink_ext_ack *extack)
{
	struct ice_pf *pf = dyn_port->pf;
	struct ice_sf_dev *sf_dev;
	struct pci_dev *pdev;
	int err;
	u32 id;

	err  = xa_alloc(&ice_sf_aux_id, &id, NULL, xa_limit_32b,
			GFP_KERNEL);
	if (err) {
		NL_SET_ERR_MSG_MOD(extack, "Could not allocate subfunction ID");
		return err;
	}

	sf_dev = kzalloc(sizeof(*sf_dev), GFP_KERNEL);
	if (!sf_dev) {
		err = -ENOMEM;
		NL_SET_ERR_MSG_MOD(extack, "Could not allocate sf_dev memory");
		goto xa_erase;
	}
	pdev = pf->pdev;

	sf_dev->dyn_port = dyn_port;
	sf_dev->adev.id = id;
	sf_dev->adev.name = "sf";
	sf_dev->adev.dev.groups = ice_sf_attr_groups;
	sf_dev->adev.dev.release = ice_sf_dev_release;
	sf_dev->adev.dev.parent = &pdev->dev;

	err = auxiliary_device_init(&sf_dev->adev);
	if (err) {
		NL_SET_ERR_MSG_MOD(extack, "Failed to initialize auxiliary device");
		goto sf_dev_free;
	}

	err = auxiliary_device_add(&sf_dev->adev);
	if (err) {
		NL_SET_ERR_MSG_MOD(extack, "Auxiliary device failed to probe");
		goto aux_dev_uninit;
	}

	dyn_port->sf_dev = sf_dev;

	return 0;

aux_dev_uninit:
	auxiliary_device_uninit(&sf_dev->adev);
sf_dev_free:
	kfree(sf_dev);
xa_erase:
	xa_erase(&ice_sf_aux_id, id);

	return err;
}

/**
 * ice_sf_eth_deactivate - Deactivate Ethernet subfunction port
 * @dyn_port: the dynamic port instance for this subfunction
 *
 * Deactivate the Ethernet subfunction, removing its auxiliary device and the
 * associated resources.
 */
void ice_sf_eth_deactivate(struct ice_dynamic_port *dyn_port)
{
	struct ice_sf_dev *sf_dev = dyn_port->sf_dev;

	if (sf_dev) {
		auxiliary_device_delete(&sf_dev->adev);
		auxiliary_device_uninit(&sf_dev->adev);
	}

	dyn_port->sf_dev = NULL;
}
