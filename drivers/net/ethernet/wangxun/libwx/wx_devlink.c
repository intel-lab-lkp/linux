// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2015 - 2024 Beijing WangXun Technology Co., Ltd. */

#include <linux/vmalloc.h>
#include <linux/pci.h>

#include "wx_type.h"
#include "wx_sriov.h"
#include "wx_devlink.h"

static const struct devlink_ops wx_pf_devlink_ops = {
};

static void wx_devlink_free(void *devlink_ptr)
{
	devlink_unregister((struct devlink *)devlink_ptr);
	devlink_free((struct devlink *)devlink_ptr);
}

struct wx_dl_priv *wx_create_devlink(struct device *dev)
{
	struct devlink *devlink;

	devlink = devlink_alloc(&wx_pf_devlink_ops, sizeof(devlink), dev);
	if (!devlink)
		return NULL;

	/* Add an action to teardown the devlink when unwinding the driver */
	if (devm_add_action_or_reset(dev, wx_devlink_free, devlink))
		return NULL;

	devlink_register(devlink);

	return devlink_priv(devlink);
}
EXPORT_SYMBOL(wx_create_devlink);

/**
 * wx_devlink_set_switch_id - Set unique switch id based on pci dsn
 * @wx: the PF to create a devlink port for
 * @ppid: struct with switch id information
 */
static void wx_devlink_set_switch_id(struct wx *wx,
				     struct netdev_phys_item_id *ppid)
{
	struct pci_dev *pdev = wx->pdev;
	u64 id;

	id = pci_get_dsn(pdev);

	ppid->id_len = sizeof(id);
	put_unaligned_be64(id, &ppid->id);
}

/**
 * wx_devlink_create_pf_port - Create a devlink port for this PF
 * @wx: the PF to create a devlink port for
 *
 * Create and register a devlink_port for this PF.
 * This function has to be called under devl_lock.
 *
 * Return: zero on success or an error code on failure.
 */
int wx_devlink_create_pf_port(struct wx *wx)
{
	struct devlink *devlink = priv_to_devlink(wx->dl_priv);
	struct devlink_port_attrs attrs = {};
	int err;

	attrs.flavour = DEVLINK_PORT_FLAVOUR_PHYSICAL;
	attrs.phys.port_number = wx->bus.func;
	wx_devlink_set_switch_id(wx, &attrs.switch_id);
	devlink_port_attrs_set(&wx->devlink_port, &attrs);
	err = devlink_port_register(devlink, &wx->devlink_port, 0);
	if (err) {
		wx_err(wx, "Failed to create devlink port for PF%d, error %d\n",
		       wx->bus.func, err);
		return err;
	}

	return 0;
}
EXPORT_SYMBOL(wx_devlink_create_pf_port);

/**
 * wx_devlink_destroy_pf_port - Destroy the devlink_port for this PF
 * @wx: the PF to cleanup
 *
 * Unregisters the devlink_port structure associated with this PF.
 * This function has to be called under devl_lock.
 */
void wx_devlink_destroy_pf_port(struct wx *wx)
{
	devl_port_unregister(&wx->devlink_port);
}
EXPORT_SYMBOL(wx_devlink_destroy_pf_port);

/**
 * wx_devlink_port_get_vf_fn_mac - .port_fn_hw_addr_get devlink handler
 * @port: devlink port structure
 * @hw_addr: MAC address of the port
 * @hw_addr_len: length of MAC address
 * @extack: extended netdev ack structure
 *
 * Callback for the devlink .port_fn_hw_addr_get operation
 * Return: zero on success or an error code on failure.
 */
static int wx_devlink_port_get_vf_fn_mac(struct devlink_port *port,
					 u8 *hw_addr, int *hw_addr_len,
					 struct netlink_ext_ack *extack)
{
	struct vf_data_storage *vfinfo = container_of(port,
						      struct vf_data_storage,
						      devlink_port);
	struct devlink_port_attrs *attrs = &port->attrs;
	struct devlink_port_pci_vf_attrs *pci_vf;
	struct wx *wx = vfinfo->vf_priv_wx;
	u16 vf_id;

	pci_vf = &attrs->pci_vf;
	vf_id = pci_vf->vf;

	ether_addr_copy(hw_addr, wx->vfinfo[vf_id].vf_mac_addr);
	*hw_addr_len = ETH_ALEN;

	return 0;
}

/**
 * wx_devlink_port_set_vf_fn_mac - .port_fn_hw_addr_set devlink handler
 * @port: devlink port structure
 * @hw_addr: MAC address of the port
 * @hw_addr_len: length of MAC address
 * @extack: extended netdev ack structure
 *
 * Callback for the devlink .port_fn_hw_addr_set operation
 * Return: zero on success or an error code on failure.
 */
static int wx_devlink_port_set_vf_fn_mac(struct devlink_port *port,
					 const u8 *hw_addr,
					 int hw_addr_len,
					 struct netlink_ext_ack *extack)

{
	struct vf_data_storage *vfinfo = container_of(port,
						      struct vf_data_storage,
						      devlink_port);
	struct devlink_port_attrs *attrs = &port->attrs;
	struct devlink_port_pci_vf_attrs *pci_vf;
	struct wx *wx = vfinfo->vf_priv_wx;
	int ret = 0;
	u16 vf_id;

	pci_vf = &attrs->pci_vf;
	vf_id = pci_vf->vf;

	if (!is_valid_ether_addr(hw_addr) || vf_id >= wx->num_vfs)
		return -EINVAL;

	ret = wx_set_vf_mac(wx, vf_id, hw_addr);
	if (ret >= 0) {
		wx->vfinfo[vf_id].pf_set_mac = true;
		if (!netif_running(wx->netdev))
			wx_err(wx, "Bring the PF device up before use vfs\n");
	} else {
		wx_err(wx, "The VF MAC address was NOT set due to invalid\n");
	}

	return 0;
}

static const struct devlink_port_ops wx_devlink_vf_port_ops = {
	.port_fn_hw_addr_get = wx_devlink_port_get_vf_fn_mac,
	.port_fn_hw_addr_set = wx_devlink_port_set_vf_fn_mac,
};

int wx_devlink_create_vf_port(struct wx *wx, int vf_id)
{
	struct devlink *devlink = priv_to_devlink(wx->dl_priv);
	struct devlink_port_attrs attrs = {};
	struct devlink_port *devlink_port;
	int err;

	devlink_port = &wx->vfinfo[vf_id].devlink_port;
	attrs.flavour = DEVLINK_PORT_FLAVOUR_PCI_VF;
	attrs.pci_vf.pf = wx->bus.func;
	attrs.pci_vf.vf = vf_id;

	wx_devlink_set_switch_id(wx, &attrs.switch_id);
	devlink_port_attrs_set(devlink_port, &attrs);
	err = devl_port_register_with_ops(devlink, devlink_port, vf_id + 1,
					  &wx_devlink_vf_port_ops);
	if (err) {
		wx_err(wx, "Failed to create devlink port for VF %d, error %d\n",
		       vf_id, err);
		return err;
	}

	return 0;
}

void wx_devlink_destroy_vf_port(struct wx *wx)
{
	int i;

	for (i = 0; i < wx->num_vfs; i++)
		devl_port_unregister(&wx->vfinfo[i].devlink_port);
}
