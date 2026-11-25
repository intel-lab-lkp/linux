// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026 André Eikmeyer <dev@deq.rocks>
 *
 * AWDL interface of the Apple firmware of the BCM4364 and BCM4377.
 *
 * The firmware runs Apple Wireless Direct Link itself once the host creates
 * an interface of type AWDL with interface_create and configures it through
 * the awdl_* iovars. The AWDL interface has a network device but no cfg80211
 * vif. Its interface events are handled here and not by cfg80211.
 */

#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>

#include "core.h"
#include "debug.h"
#include "feature.h"
#include "fwil.h"
#include "fwil_types.h"
#include "proto.h"
#include "awdl.h"

/* The firmware interface index of the AWDL interface */
#define BRCMF_AWDL_IFIDX		2
#define BRCMF_AWDL_IF_TIMEOUT		msecs_to_jiffies(2000)

/* The BSSID of all AWDL frames */
static const u8 brcmf_awdl_bssid[ETH_ALEN] = {
	0x00, 0x25, 0x00, 0xff, 0x94, 0x73,
};

void brcmf_awdl_attach(struct brcmf_pub *drvr)
{
	spin_lock_init(&drvr->awdl.lock);
	init_completion(&drvr->awdl.if_event);
}

/*
 * Called from the firmware event worker after an interface was added. Takes
 * the interface if it is the AWDL interface being created.
 */
static void brcmf_awdl_drop_if(struct brcmf_pub *drvr, struct brcmf_if *ifp);

bool brcmf_awdl_if_added(struct brcmf_pub *drvr, struct brcmf_if *ifp)
{
	struct brcmf_awdl *awdl = &drvr->awdl;
	bool own, late = false;

	spin_lock(&awdl->lock);
	own = (awdl->adding || awdl->abandoned) && ifp->ndev &&
	      ether_addr_equal(ifp->mac_addr, awdl->mac);
	if (own && awdl->adding) {
		ifp->awdl = true;
		strscpy(ifp->ndev->name, "awdl%d", IFNAMSIZ);
		/* Keeps udev from giving it a name from the bus path */
		ifp->ndev->name_assign_type = NET_NAME_PREDICTABLE;
		awdl->ifp = ifp;
		awdl->adding = false;
		complete(&awdl->if_event);
	} else if (own) {
		awdl->abandoned = false;
		late = true;
	}
	spin_unlock(&awdl->lock);

	/* brcmf_awdl_add() gave up on this interface */
	if (late)
		brcmf_awdl_drop_if(drvr, ifp);
	return own;
}

/*
 * Called from the firmware event worker before an interface is removed.
 * Returns true if the removal of the AWDL interface was requested by
 * brcmf_awdl_del(), which then removes it. When the firmware removes the
 * AWDL interface on its own, it is forgotten here and removed by the caller.
 */
bool brcmf_awdl_if_removed(struct brcmf_pub *drvr, struct brcmf_if *ifp)
{
	struct brcmf_awdl *awdl = &drvr->awdl;
	bool requested = false;

	if (!ifp->awdl)
		return false;

	spin_lock(&awdl->lock);
	if (awdl->ifp == ifp) {
		requested = awdl->removing;
		if (requested)
			complete(&awdl->if_event);
		else
			awdl->ifp = NULL;
	}
	spin_unlock(&awdl->lock);
	if (!requested)
		brcmf_proto_configure_addr_mode(drvr, ifp->ifidx,
						ADDR_INDIRECT);
	return requested;
}

/* The firmware interface index of the AWDL interface must be free */
static bool brcmf_awdl_ifidx_used(struct brcmf_pub *drvr)
{
	int i;

	for (i = 1; i < BRCMF_MAX_IFS; i++)
		if (drvr->iflist[i] && drvr->iflist[i]->ifidx == BRCMF_AWDL_IFIDX)
			return true;
	return false;
}

/* Undoes interface_create for an interface that was not attached */
static void brcmf_awdl_drop_if(struct brcmf_pub *drvr, struct brcmf_if *ifp)
{
	struct net_device *ndev = ifp->ndev;

	brcmf_fil_iovar_data_set(ifp, "interface_remove", NULL, 0);
	brcmf_proto_configure_addr_mode(drvr, ifp->ifidx, ADDR_INDIRECT);
	brcmf_proto_del_if(drvr, ifp);
	if (drvr->iflist[ifp->bsscfgidx] == ifp)
		drvr->iflist[ifp->bsscfgidx] = NULL;
	if (drvr->if2bss[ifp->ifidx] == ifp->bsscfgidx)
		drvr->if2bss[ifp->ifidx] = BRCMF_BSSIDX_INVALID;
	free_netdev(ndev);
}

/**
 * brcmf_awdl_add() - create the AWDL interface.
 *
 * @drvr: device.
 * @mac: MAC address of the AWDL interface.
 * @ndev: set to the network device of the AWDL interface.
 *
 * Called with RTNL and the wiphy mutex held. The interface uses the firmware
 * interface index 2 and the AWDL BSSID 00:25:00:ff:94:73, which the host
 * passes to the firmware.
 *
 * Return: 0 or a negative error code.
 */
int brcmf_awdl_add(struct brcmf_pub *drvr, const u8 *mac,
		   struct net_device **ndev)
{
	struct brcmf_awdl *awdl = &drvr->awdl;
	struct brcmf_if *pri = brcmf_get_ifp(drvr, 0);
	struct wl_interface_create_v3 req = {};
	struct brcmf_if *ifp;
	long left = 0;
	int err;

	ASSERT_RTNL();
	lockdep_assert_wiphy(drvr->wiphy);

	if (!pri || !brcmf_feat_is_enabled(pri, BRCMF_FEAT_AWDL))
		return -EOPNOTSUPP;
	if (!is_valid_ether_addr(mac))
		return -EINVAL;
	if (awdl->detached)
		return -ENODEV;
	if (awdl->ifp)
		return -EBUSY;
	if (brcmf_awdl_ifidx_used(drvr)) {
		bphy_err(drvr, "interface index %d is in use, no AWDL\n",
			 BRCMF_AWDL_IFIDX);
		return -EBUSY;
	}

	spin_lock(&awdl->lock);
	ether_addr_copy(awdl->mac, mac);
	awdl->adding = true;
	awdl->abandoned = false;
	reinit_completion(&awdl->if_event);
	spin_unlock(&awdl->lock);

	req.ver = WL_INTERFACE_CREATE_VER_3;
	req.len = sizeof(req);
	req.fixed_len = sizeof(req);
	req.iftype = WL_INTERFACE_CREATE_AWDL;
	req.flags = WL_INTERFACE_MAC_USE | WL_INTERFACE_IF_INDEX_USE |
		    WL_INTERFACE_BSSID_USE;
	ether_addr_copy(req.mac_addr, mac);
	ether_addr_copy(req.bssid, brcmf_awdl_bssid);
	req.if_index = BRCMF_AWDL_IFIDX;
	err = brcmf_fil_iovar_data_get(pri, "interface_create", &req,
				       sizeof(req));
	if (!err)
		left = wait_for_completion_timeout(&awdl->if_event,
						   BRCMF_AWDL_IF_TIMEOUT);

	spin_lock(&awdl->lock);
	awdl->adding = false;
	ifp = awdl->ifp;
	/* The firmware created the interface, its event may still come */
	if (!err && !ifp)
		awdl->abandoned = true;
	spin_unlock(&awdl->lock);

	if (err) {
		bphy_err(drvr, "AWDL interface_create failed: %d\n", err);
		return err;
	}
	if (!left || !ifp) {
		bphy_err(drvr, "no interface event for the AWDL interface\n");
		return -ETIMEDOUT;
	}

	/* Like an AP, AWDL sends to each peer through its own flowrings */
	brcmf_proto_configure_addr_mode(drvr, ifp->ifidx, ADDR_DIRECT);
	SET_NETDEV_DEV(ifp->ndev, wiphy_dev(drvr->wiphy));
	err = brcmf_net_attach(ifp, true);
	if (err) {
		spin_lock(&awdl->lock);
		awdl->ifp = NULL;
		spin_unlock(&awdl->lock);
		brcmf_awdl_drop_if(drvr, ifp);
		return err;
	}
	brcmf_dbg(INFO, "%s: AWDL interface, bsscfgidx %d ifidx %d\n",
		  ifp->ndev->name, ifp->bsscfgidx, ifp->ifidx);
	*ndev = ifp->ndev;
	return 0;
}

/**
 * brcmf_awdl_del() - remove the AWDL interface.
 *
 * @drvr: device.
 *
 * Called with RTNL and the wiphy mutex held. Deletes the flowrings of the
 * AWDL interface, stops AWDL and removes the interface in the firmware and on
 * the host.
 *
 * Return: 0 or a negative error code.
 */
int brcmf_awdl_del(struct brcmf_pub *drvr)
{
	struct brcmf_awdl *awdl = &drvr->awdl;
	struct brcmf_if *ifp;
	int err;

	ASSERT_RTNL();
	lockdep_assert_wiphy(drvr->wiphy);

	ifp = awdl->ifp;
	if (!ifp)
		return -ENOENT;

	netif_tx_disable(ifp->ndev);
	/* Deletes the flowrings of all peers */
	brcmf_proto_configure_addr_mode(drvr, ifp->ifidx, ADDR_INDIRECT);
	brcmf_fil_iovar_int_set(ifp, "awdl", 0);

	spin_lock(&awdl->lock);
	awdl->removing = true;
	reinit_completion(&awdl->if_event);
	spin_unlock(&awdl->lock);

	err = brcmf_fil_iovar_data_set(ifp, "interface_remove", NULL, 0);
	if (!err && !wait_for_completion_timeout(&awdl->if_event,
						 BRCMF_AWDL_IF_TIMEOUT)) {
		bphy_err(drvr, "no interface event for the AWDL removal\n");
		err = -ETIMEDOUT;
	}

	spin_lock(&awdl->lock);
	awdl->removing = false;
	awdl->ifp = NULL;
	spin_unlock(&awdl->lock);

	brcmf_remove_interface(ifp, true);
	return err;
}

/* Called before the interfaces of the device are removed */
void brcmf_awdl_detach(struct brcmf_pub *drvr)
{
	struct brcmf_awdl *awdl = &drvr->awdl;

	rtnl_lock();
	wiphy_lock(drvr->wiphy);
	spin_lock(&awdl->lock);
	awdl->detached = true;
	awdl->ifp = NULL;
	spin_unlock(&awdl->lock);
	wiphy_unlock(drvr->wiphy);
	rtnl_unlock();
}
