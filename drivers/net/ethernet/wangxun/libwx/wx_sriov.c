// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2015 - 2023 Beijing WangXun Technology Co., Ltd. */

#include <linux/etherdevice.h>
#include <linux/pci.h>

#include "wx_type.h"
#include "wx_mbx.h"
#include "wx_sriov.h"

static void wx_vf_configuration(struct pci_dev *pdev, int event_mask)
{
	unsigned int vfn = (event_mask & GENMASK(5, 0));
	struct wx *wx = pci_get_drvdata(pdev);

	bool enable = ((event_mask & BIT(31)) != 0);

	if (enable)
		eth_zero_addr(wx->vfinfo[vfn].vf_mac_addr);
}

static void wx_alloc_vf_macvlans(struct wx *wx, u8 num_vfs)
{
	struct vf_macvlans *mv_list;
	int num_vf_macvlans, i;

	/* Initialize list of VF macvlans */
	INIT_LIST_HEAD(&wx->vf_mvs.l);

	num_vf_macvlans = wx->mac.num_rar_entries -
			  (WX_MAX_PF_MACVLANS + 1 + num_vfs);
	if (!num_vf_macvlans)
		return;

	mv_list = kcalloc(num_vf_macvlans, sizeof(struct vf_macvlans),
			  GFP_KERNEL);
	if (mv_list) {
		for (i = 0; i < num_vf_macvlans; i++) {
			mv_list[i].vf = -1;
			mv_list[i].free = true;
			list_add(&mv_list[i].l, &wx->vf_mvs.l);
		}
		wx->mv_list = mv_list;
	}
}

static int __wx_enable_sriov(struct wx *wx, u8 num_vfs)
{
	u32 value = 0;
	int i;

	set_bit(WX_FLAG_SRIOV_ENABLED, wx->flags);
	wx_err(wx, "SR-IOV enabled with %d VFs\n", num_vfs);

	/* Enable VMDq flag so device will be set in VM mode */
	set_bit(WX_FLAG_VMDQ_ENABLED, wx->flags);
	if (!wx->ring_feature[RING_F_VMDQ].limit)
		wx->ring_feature[RING_F_VMDQ].limit = 1;
	wx->ring_feature[RING_F_VMDQ].offset = num_vfs;

	wx_alloc_vf_macvlans(wx, num_vfs);
	/* Initialize default switching mode VEB */
	wr32m(wx, WX_PSR_CTL, WX_PSR_CTL_SW_EN, WX_PSR_CTL_SW_EN);

	/* If call to enable VFs succeeded then allocate memory
	 * for per VF control structures.
	 */
	wx->vfinfo = kcalloc(num_vfs, sizeof(struct vf_data_storage), GFP_KERNEL);
	if (!wx->vfinfo)
		return -ENOMEM;

	/* enable spoof checking for all VFs */
	for (i = 0; i < num_vfs; i++) {
		/* enable spoof checking for all VFs */
		wx->vfinfo[i].spoofchk_enabled = true;
		wx->vfinfo[i].link_enable = true;
		/* Untrust all VFs */
		wx->vfinfo[i].trusted = false;
		/* set the default xcast mode */
		wx->vfinfo[i].xcast_mode = WXVF_XCAST_MODE_NONE;
	}

	if (wx->mac.type == wx_mac_sp) {
		if (num_vfs < 32)
			value = WX_CFG_PORT_CTL_NUM_VT_32;
		else
			value = WX_CFG_PORT_CTL_NUM_VT_64;
	} else {
		value = WX_CFG_PORT_CTL_NUM_VT_8;
	}
	wr32m(wx, WX_CFG_PORT_CTL,
	      WX_CFG_PORT_CTL_NUM_VT_MASK,
	      value);

	return 0;
}

static void wx_sriov_reinit(struct wx *wx)
{
	rtnl_lock();
	wx->setup_tc(wx->netdev, netdev_get_num_tc(wx->netdev));
	rtnl_unlock();
}

int wx_disable_sriov(struct wx *wx)
{
	/* If our VFs are assigned we cannot shut down SR-IOV
	 * without causing issues, so just leave the hardware
	 * available but disabled
	 */
	if (pci_vfs_assigned(wx->pdev)) {
		wx_err(wx, "Unloading driver while VFs are assigned.\n");
		return -EPERM;
	}
	/* disable iov and allow time for transactions to clear */
	pci_disable_sriov(wx->pdev);

	/* set num VFs to 0 to prevent access to vfinfo */
	wx->num_vfs = 0;

	/* free VF control structures */
	kfree(wx->vfinfo);
	wx->vfinfo = NULL;

	/* free macvlan list */
	kfree(wx->mv_list);
	wx->mv_list = NULL;

	/* set default pool back to 0 */
	wr32m(wx, WX_PSR_VM_CTL, WX_PSR_VM_CTL_POOL_MASK, 0);
	wx->ring_feature[RING_F_VMDQ].offset = 0;

	clear_bit(WX_FLAG_SRIOV_ENABLED, wx->flags);
	/* Disable VMDq flag so device will be set in VM mode */
	if (wx->ring_feature[RING_F_VMDQ].limit == 1)
		clear_bit(WX_FLAG_VMDQ_ENABLED, wx->flags);

	return 0;
}
EXPORT_SYMBOL(wx_disable_sriov);

static int wx_pci_sriov_enable(struct pci_dev *dev,
			       int num_vfs)
{
	struct wx *wx = pci_get_drvdata(dev);
	int err = 0, i;

	err = __wx_enable_sriov(wx, num_vfs);
	if (err)
		goto err_out;

	wx->num_vfs = num_vfs;
	for (i = 0; i < wx->num_vfs; i++)
		wx_vf_configuration(dev, (i | BIT(31)));

	/* reset before enabling SRIOV to avoid mailbox issues */
	wx_sriov_reinit(wx);

	err = pci_enable_sriov(dev, num_vfs);
	if (err) {
		wx_err(wx, "Failed to enable PCI sriov: %d\n", err);
		goto err_out;
	}

	return num_vfs;
err_out:
	return err;
}

static int wx_pci_sriov_disable(struct pci_dev *dev)
{
	struct wx *wx = pci_get_drvdata(dev);
	int err;

	err = wx_disable_sriov(wx);

	/* reset before enabling SRIOV to avoid mailbox issues */
	if (!err)
		wx_sriov_reinit(wx);

	return err;
}

static int wx_check_sriov_allowed(struct wx *wx, int num_vfs)
{
	u16 max_vfs;

	max_vfs = (wx->mac.type == wx_mac_sp) ? 63 : 7;

	if (num_vfs > max_vfs)
		return -EPERM;

	return 0;
}

int wx_pci_sriov_configure(struct pci_dev *pdev, int num_vfs)
{
	struct wx *wx = pci_get_drvdata(pdev);
	int err;

	err = wx_check_sriov_allowed(wx, num_vfs);
	if (err)
		return err;

	if (!num_vfs) {
		if (!pci_vfs_assigned(pdev)) {
			wx_pci_sriov_disable(pdev);
			return 0;
		}

		wx_err(wx, "can't free VFs because some are assigned to VMs.\n");
		return -EBUSY;
	}

	err = wx_pci_sriov_enable(pdev, num_vfs);
	if (err)
		return err;

	return num_vfs;
}
EXPORT_SYMBOL(wx_pci_sriov_configure);
