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
 *
 * The firmware passes AWDL data frames as 802.3 frames with an LLC/SNAP
 * header with the Apple OUI and an AWDL data header:
 *
 *   aa aa 03 00 17 f2 08 00   LLC/SNAP, OUI 00:17:f2, protocol 0x0800
 *   03 04                     AWDL data header
 *   seq (le16) 00 00          sequence number, reserved
 *   ethertype (be16)
 *
 * The network device of the AWDL interface sends and receives plain
 * Ethernet frames. The header is added before frames are sent and removed
 * from received frames.
 */

#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/skbuff.h>
#include <linux/unaligned.h>
#include <net/netlink.h>

#include "core.h"
#include "debug.h"
#include "feature.h"
#include "fwil.h"
#include "fwil_types.h"
#include "bus.h"
#include "proto.h"
#include "cfg80211.h"
#include "vendor.h"
#include "awdl.h"

/* The firmware interface index of the AWDL interface */
#define BRCMF_AWDL_IFIDX		2
#define BRCMF_AWDL_IF_TIMEOUT		msecs_to_jiffies(2000)

/* LLC/SNAP with the Apple OUI and the start of the AWDL data header */
static const u8 brcmf_awdl_llc[10] = {
	0xaa, 0xaa, 0x03, 0x00, 0x17, 0xf2, 0x08, 0x00, 0x03, 0x04,
};

#define BRCMF_AWDL_SEQ_OFF		10
#define BRCMF_AWDL_ETHERTYPE_OFF	14

/* The BSSID of all AWDL frames */
static const u8 brcmf_awdl_bssid[ETH_ALEN] = {
	0x00, 0x25, 0x00, 0xff, 0x94, 0x73,
};

/*
 * Firmware parameters the host sets for AWDL. The tables below and the
 * offsets patched into them, in awdl_election_tree, awdl_opmode, slot_bss
 * and awdl_peer_op, are copied from an iovar trace of macOS 15.4.1 on a
 * MacBookAir9,1. They are not documented otherwise, their fields are not
 * known beyond what the comments say. They are used with the firmware
 * 9.30.514.0.32.5.94 of the BCM4364 and 16.20.380.0.3.6.130 of the BCM4377.
 */
#define BRCMF_AWDL_CONFIG_START		0x83
#define BRCMF_AWDL_CONFIG_SETTLED	0x73
#define BRCMF_AWDL_PRESENCEMODE		4
#define BRCMF_AWDL_OSOC_CHAN		44
#define BRCMF_AWDL_AFTX_DELAY		msecs_to_jiffies(500)
#define BRCMF_AWDL_SETTLE_DELAY		msecs_to_jiffies(5000)
#define BRCMF_AWDL_TLV_MAX		2048

static const u8 brcmf_awdl_sync_params[] = {
	0x00, 0x24, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00,
	0x10, 0x00, 0x6e, 0x00, 0x00, 0x00, 0x10, 0x00,
	0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
};

static const u8 brcmf_awdl_psf_dwell[] = {
	0x00, 0x00, 0x00, 0x00, 0x6e, 0x00, 0x6e, 0x00,
};

/* Header of awdl_oob_af_auto, the AWDL BSSID at 8, followed by the TLV length */
static const u8 brcmf_awdl_oob_hdr[] = {
	0xff, 0xff, 0xff, 0xff, 0xd8, 0x40, 0x00, 0x00,
	0x00, 0x25, 0x00, 0xff, 0x94, 0x73, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
};

/* awdl_opmode: byte 1 the role, 2 master and 0 slave, 4 to 9 the master */
#define BRCMF_AWDL_OPMODE_ROLE		1
#define BRCMF_AWDL_OPMODE_ROLE_MASTER	2
#define BRCMF_AWDL_OPMODE_ROLE_SLAVE	0
#define BRCMF_AWDL_OPMODE_ADDR		4
static const u8 brcmf_awdl_opmode[] = {
	0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
};

/* awdl_election_tree: the master at 0x0f, its metric at 0x15, distance 0x19 */
#define BRCMF_AWDL_ELECTION_ADDR	0x0f
#define BRCMF_AWDL_ELECTION_METRIC	0x15
#define BRCMF_AWDL_ELECTION_DISTANCE	0x19
static const u8 brcmf_awdl_election_tree[] = {
	0x00, 0x00, 0x00, 0x41, 0x00, 0x00, 0x00, 0x00,
	0x05, 0xba, 0x00, 0x00, 0x01, 0x03, 0x04, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00,
};

/*
 * slot_bss with the chanspecs of slice 0 at 0x4c and of slice 1 at 0x78, 16
 * each. Chips without SDB get 9 at 0x0c and all chanspecs in slice 0.
 */
#define BRCMF_AWDL_SLOT_SDB		0x0c
#define BRCMF_AWDL_SLOT_SDB_OFF		9
#define BRCMF_AWDL_SLOT_SLICE0		0x4c
#define BRCMF_AWDL_SLOT_SLICE1		0x78
#define BRCMF_AWDL_SLOTS		16
static const u8 brcmf_awdl_slot_bss[] = {
	0x00, 0x80, 0x01, 0x01, 0x01, 0x00, 0x90, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x04, 0x03, 0x00, 0x00,
	0x01, 0x00, 0x2a, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x0f, 0x02, 0x00, 0x03, 0xff, 0xff, 0xe2, 0x2a,
	0xe2, 0x2a, 0xe2, 0x2a, 0xe2, 0x2a, 0xe2, 0x2a,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x06,
	0xe2, 0x2a, 0xe2, 0x2a, 0xe2, 0x2a, 0xe2, 0x2a,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x10, 0x02, 0x00, 0xe2, 0x2a, 0xe2, 0x2a,
	0xe2, 0x2a, 0xe2, 0x2a, 0xe2, 0x2a, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe2, 0x2a,
	0xe2, 0x2a, 0xe2, 0x2a, 0xe2, 0x2a, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x28, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x10, 0x02, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x10, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/*
 * awdl_peer_op: byte 1 the operation, 2 to 7 the peer, 8 the flags, from 9
 * the HT and VHT capabilities elements of the peer. An add without them is
 * 0x25 bytes long.
 */
#define BRCMF_AWDL_PEER_OP_ADD		0
#define BRCMF_AWDL_PEER_OP_DEL		1
#define BRCMF_AWDL_PEER_OP_HDR_LEN	9
#define BRCMF_AWDL_PEER_OP_ADD_LEN	0x25
#define BRCMF_AWDL_PEER_IES_MAX		128

static void brcmf_awdl_ctrl_work(struct work_struct *work);
static void brcmf_awdl_aftx_work(struct wiphy *wiphy, struct wiphy_work *work);
static void brcmf_awdl_settle_work(struct wiphy *wiphy,
				   struct wiphy_work *work);

void brcmf_awdl_attach(struct brcmf_pub *drvr)
{
	struct brcmf_awdl *awdl = &drvr->awdl;

	spin_lock_init(&awdl->lock);
	init_completion(&awdl->if_event);
	INIT_WORK(&awdl->ctrl_work, brcmf_awdl_ctrl_work);
	wiphy_delayed_work_init(&awdl->aftx_work, brcmf_awdl_aftx_work);
	wiphy_delayed_work_init(&awdl->settle_work, brcmf_awdl_settle_work);
}

/*
 * Called from the firmware event worker after an interface was added. Takes
 * the interface if it is the AWDL interface being created.
 */
static void brcmf_awdl_drop_if(struct brcmf_pub *drvr, struct brcmf_if *ifp);
static void brcmf_awdl_send_result(struct brcmf_pub *drvr, int event,
				   int status, struct net_device *ndev);

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
	if (!requested) {
		brcmf_proto_configure_addr_mode(drvr, ifp->ifidx,
						ADDR_INDIRECT);
		brcmf_awdl_send_result(drvr, BRCMF_VNDR_EVTS_AWDL_STOPPED,
				       -ENOLINK, NULL);
	}
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

	wiphy_delayed_work_cancel(drvr->wiphy, &awdl->aftx_work);
	wiphy_delayed_work_cancel(drvr->wiphy, &awdl->settle_work);
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

/**
 * brcmf_awdl_encap() - add the AWDL data header to a frame to be sent.
 *
 * @ifp: the AWDL interface.
 * @skb: Ethernet frame, an 802.3 frame on return.
 *
 * Called in the transmit path of the AWDL network device, which serializes
 * it.
 *
 * Return: 0 or a negative error code, the caller frees @skb on error.
 */
int brcmf_awdl_encap(struct brcmf_if *ifp, struct sk_buff *skb)
{
	struct brcmf_awdl *awdl = &ifp->drvr->awdl;
	__be16 proto;
	u8 *hdr;

	if (skb->len < ETH_HLEN || skb_cow_head(skb, BRCMF_AWDL_HDR_LEN))
		return -EINVAL;

	proto = eth_hdr(skb)->h_proto;
	hdr = skb_push(skb, BRCMF_AWDL_HDR_LEN);
	memmove(hdr, hdr + BRCMF_AWDL_HDR_LEN, 2 * ETH_ALEN);
	put_unaligned_be16(skb->len - ETH_HLEN, hdr + 2 * ETH_ALEN);
	hdr += ETH_HLEN;
	memcpy(hdr, brcmf_awdl_llc, sizeof(brcmf_awdl_llc));
	put_unaligned_le16(awdl->tx_seq++, hdr + BRCMF_AWDL_SEQ_OFF);
	put_unaligned_le16(0, hdr + BRCMF_AWDL_SEQ_OFF + 2);
	put_unaligned(proto, (__be16 *)(hdr + BRCMF_AWDL_ETHERTYPE_OFF));
	skb_reset_mac_header(skb);
	return 0;
}

/**
 * brcmf_awdl_decap() - remove the AWDL data header from a received frame.
 *
 * @skb: frame after eth_type_trans(), an Ethernet frame on return.
 *
 * The addresses are moved behind the AWDL data header, where they form an
 * Ethernet header with the ethertype of the AWDL data header.
 *
 * Return: 0 or a negative error code for frames that are not AWDL data
 * frames.
 */
int brcmf_awdl_decap(struct sk_buff *skb)
{
	__be16 proto;
	u8 *eth;

	if (skb->protocol != htons(ETH_P_802_2) ||
	    !pskb_may_pull(skb, BRCMF_AWDL_HDR_LEN) ||
	    memcmp(skb->data, brcmf_awdl_llc, sizeof(brcmf_awdl_llc)) ||
	    skb_cow_head(skb, 0))
		return -EINVAL;

	proto = get_unaligned((__be16 *)(skb->data + BRCMF_AWDL_ETHERTYPE_OFF));
	eth = skb->data + BRCMF_AWDL_HDR_LEN - ETH_HLEN;
	memmove(eth, skb_mac_header(skb), 2 * ETH_ALEN);
	skb_pull_rcsum(skb, BRCMF_AWDL_HDR_LEN);
	skb_set_mac_header(skb, -ETH_HLEN);
	skb_reset_network_header(skb);
	skb->protocol = proto;
	return 0;
}

/* Sets a firmware parameter of the AWDL interface */
static int brcmf_awdl_set(struct brcmf_if *ifp, const char *name,
			  const void *data, u32 len)
{
	int err;

	err = brcmf_fil_iovar_data_set(ifp, name, data, len);
	if (err)
		bphy_err(ifp->drvr, "AWDL %s failed: %d\n", name, err);
	return err;
}

static int brcmf_awdl_set_u32(struct brcmf_if *ifp, const char *name,
			      u32 val)
{
	__le32 data = cpu_to_le32(val);

	return brcmf_awdl_set(ifp, name, &data, sizeof(data));
}

static void brcmf_awdl_free_frames(struct brcmf_awdl *awdl)
{
	kfree(awdl->payload);
	kfree(awdl->oob);
	awdl->payload = NULL;
	awdl->oob = NULL;
	awdl->payload_len = 0;
	awdl->oob_len = 0;
}

/* Builds awdl_payload and awdl_oob_af_auto around the TLVs of user space */
static int brcmf_awdl_store_frames(struct brcmf_awdl *awdl,
				   const struct nlattr *payload,
				   const struct nlattr *oob)
{
	u32 plen = nla_len(payload), olen = nla_len(oob);
	u8 *p, *o;

	p = kmalloc(2 + plen, GFP_KERNEL);
	o = kmalloc(sizeof(brcmf_awdl_oob_hdr) + 2 + olen, GFP_KERNEL);
	if (!p || !o) {
		kfree(p);
		kfree(o);
		return -ENOMEM;
	}
	put_unaligned_le16(plen, p);
	memcpy(p + 2, nla_data(payload), plen);
	memcpy(o, brcmf_awdl_oob_hdr, sizeof(brcmf_awdl_oob_hdr));
	put_unaligned_le16(olen, o + sizeof(brcmf_awdl_oob_hdr));
	memcpy(o + sizeof(brcmf_awdl_oob_hdr) + 2, nla_data(oob), olen);

	brcmf_awdl_free_frames(awdl);
	awdl->payload = p;
	awdl->payload_len = 2 + plen;
	awdl->oob = o;
	awdl->oob_len = sizeof(brcmf_awdl_oob_hdr) + 2 + olen;
	return 0;
}

static int brcmf_awdl_set_payload(struct brcmf_awdl *awdl)
{
	return brcmf_awdl_set(awdl->ifp, "awdl_payload", awdl->payload,
			      awdl->payload_len);
}

static int brcmf_awdl_set_opmode(struct brcmf_awdl *awdl)
{
	u8 buf[sizeof(brcmf_awdl_opmode)];

	memcpy(buf, brcmf_awdl_opmode, sizeof(buf));
	if (awdl->has_master) {
		buf[BRCMF_AWDL_OPMODE_ROLE] = BRCMF_AWDL_OPMODE_ROLE_SLAVE;
		ether_addr_copy(buf + BRCMF_AWDL_OPMODE_ADDR, awdl->master);
	} else {
		buf[BRCMF_AWDL_OPMODE_ROLE] = BRCMF_AWDL_OPMODE_ROLE_MASTER;
		ether_addr_copy(buf + BRCMF_AWDL_OPMODE_ADDR, awdl->mac);
	}
	return brcmf_awdl_set(awdl->ifp, "awdl_opmode", buf, sizeof(buf));
}

static int brcmf_awdl_set_election(struct brcmf_awdl *awdl)
{
	u8 buf[sizeof(brcmf_awdl_election_tree)];

	memcpy(buf, brcmf_awdl_election_tree, sizeof(buf));
	if (awdl->has_master) {
		ether_addr_copy(buf + BRCMF_AWDL_ELECTION_ADDR, awdl->master);
		put_unaligned_le32(awdl->master_metric,
				   buf + BRCMF_AWDL_ELECTION_METRIC);
		buf[BRCMF_AWDL_ELECTION_DISTANCE] = awdl->master_distance;
	} else {
		ether_addr_copy(buf + BRCMF_AWDL_ELECTION_ADDR, awdl->mac);
	}
	return brcmf_awdl_set(awdl->ifp, "awdl_election_tree", buf,
			      sizeof(buf));
}

static int brcmf_awdl_set_slots(struct brcmf_pub *drvr)
{
	u8 buf[sizeof(brcmf_awdl_slot_bss)];
	u8 *s0, *s1;
	int i;

	memcpy(buf, brcmf_awdl_slot_bss, sizeof(buf));
	if (brcmf_feat_is_quirk_enabled(drvr->awdl.ifp,
					BRCMF_FEAT_QUIRK_AWDL_NO_SDB)) {
		buf[BRCMF_AWDL_SLOT_SDB] = BRCMF_AWDL_SLOT_SDB_OFF;
		for (i = 0; i < BRCMF_AWDL_SLOTS; i++) {
			s0 = buf + BRCMF_AWDL_SLOT_SLICE0 + 2 * i;
			s1 = buf + BRCMF_AWDL_SLOT_SLICE1 + 2 * i;
			if (!get_unaligned_le16(s0))
				memcpy(s0, s1, 2);
			memset(s1, 0, 2);
		}
	}
	return brcmf_awdl_set(drvr->awdl.ifp, "slot_bss", buf, sizeof(buf));
}

/*
 * The firmware takes the frames and then the synchronization state, each
 * refresh sends both. A vendor command returns the errors of the part it
 * changes, the errors of the other part are only logged.
 *
 * brcmf_awdl_refresh_frames() sends the frames and returns the first error.
 */
static int brcmf_awdl_refresh_frames(struct brcmf_awdl *awdl)
{
	int err, ret;

	ret = brcmf_awdl_set(awdl->ifp, "awdl_oob_af_auto", awdl->oob,
			     awdl->oob_len);
	err = brcmf_awdl_set_payload(awdl);
	return ret ?: err;
}

/*
 * Sends the synchronization state, returns the first error. awdl_opmode
 * fails while the firmware does not know the new master yet, the next
 * refresh sends it again.
 */
static int brcmf_awdl_refresh_sync(struct brcmf_awdl *awdl)
{
	int err, ret;

	ret = brcmf_awdl_set(awdl->ifp, "awdl_sync_params",
			     brcmf_awdl_sync_params,
			     sizeof(brcmf_awdl_sync_params));
	err = brcmf_awdl_set_opmode(awdl);
	ret = ret ?: err;
	err = brcmf_awdl_set_election(awdl);
	ret = ret ?: err;
	err = brcmf_awdl_set(awdl->ifp, "awdl_sync_params",
			     brcmf_awdl_sync_params,
			     sizeof(brcmf_awdl_sync_params));
	return ret ?: err;
}

/*
 * Starts AWDL on the AWDL interface, the firmware expects this order. AWDL
 * does not run without awdl_config, slot_bss and awdl, a failure of one of
 * them fails the start. The other parameters are retried by the next
 * refresh.
 */
static int brcmf_awdl_enable(struct brcmf_pub *drvr)
{
	struct brcmf_awdl *awdl = &drvr->awdl;
	struct brcmf_if *ifp = awdl->ifp;
	int err;

	brcmf_awdl_set_payload(awdl);
	brcmf_awdl_set_u32(ifp, "awdl_aftxmode", 0);
	brcmf_awdl_set(ifp, "awdl_sync_params", brcmf_awdl_sync_params,
		       sizeof(brcmf_awdl_sync_params));
	brcmf_awdl_set_u32(ifp, "awdl_osoc_chan", 0);
	err = brcmf_awdl_set_u32(ifp, "awdl_config", BRCMF_AWDL_CONFIG_START);
	if (err)
		return err;
	brcmf_awdl_set(ifp, "awdl_psf_dwell", brcmf_awdl_psf_dwell,
		       sizeof(brcmf_awdl_psf_dwell));
	err = brcmf_awdl_set_slots(drvr);
	if (err)
		return err;
	brcmf_awdl_set_payload(awdl);
	brcmf_awdl_set_election(awdl);
	err = brcmf_awdl_set_u32(ifp, "awdl", 1);
	if (err)
		return err;
	brcmf_awdl_set_u32(ifp, "awdl_presencemode", BRCMF_AWDL_PRESENCEMODE);
	err = brcmf_awdl_set_slots(drvr);
	if (err)
		return err;
	brcmf_awdl_refresh_frames(awdl);
	brcmf_awdl_refresh_sync(awdl);
	wiphy_delayed_work_queue(drvr->wiphy, &awdl->aftx_work,
				 BRCMF_AWDL_AFTX_DELAY);
	wiphy_delayed_work_queue(drvr->wiphy, &awdl->settle_work,
				 BRCMF_AWDL_SETTLE_DELAY);
	return 0;
}

static void brcmf_awdl_aftx_work(struct wiphy *wiphy, struct wiphy_work *work)
{
	struct brcmf_awdl *awdl = container_of(work, struct brcmf_awdl,
					       aftx_work.work);

	if (!awdl->ifp)
		return;
	brcmf_awdl_set_u32(awdl->ifp, "awdl_aftxmode", 0);
	brcmf_awdl_set_u32(awdl->ifp, "awdl_osoc_chan", BRCMF_AWDL_OSOC_CHAN);
}

static void brcmf_awdl_settle_work(struct wiphy *wiphy,
				   struct wiphy_work *work)
{
	struct brcmf_awdl *awdl = container_of(work, struct brcmf_awdl,
					       settle_work.work);
	static const u8 dwell_off[sizeof(brcmf_awdl_psf_dwell)];

	if (!awdl->ifp)
		return;
	brcmf_awdl_set_u32(awdl->ifp, "awdl_config", BRCMF_AWDL_CONFIG_SETTLED);
	brcmf_awdl_set(awdl->ifp, "awdl_psf_dwell", dwell_off,
		       sizeof(dwell_off));
}

static void brcmf_awdl_send_result(struct brcmf_pub *drvr, int event,
				   int status, struct net_device *ndev)
{
	struct brcmf_if *pri = brcmf_get_ifp(drvr, 0);
	struct sk_buff *skb;

	skb = cfg80211_vendor_event_alloc(drvr->wiphy,
					  pri && pri->vif ? &pri->vif->wdev :
							    NULL,
					  NLMSG_DEFAULT_SIZE, event,
					  GFP_KERNEL);
	if (!skb)
		return;
	if (nla_put_s32(skb, BRCMF_AWDL_ATTR_STATUS, status) ||
	    (ndev && nla_put_u32(skb, BRCMF_AWDL_ATTR_IFINDEX,
				 ndev->ifindex))) {
		kfree_skb(skb);
		return;
	}
	cfg80211_vendor_event(skb, GFP_KERNEL);
}

/* Runs the AWDL_START and AWDL_STOP vendor commands with RTNL */
static void brcmf_awdl_ctrl_work(struct work_struct *work)
{
	struct brcmf_awdl *awdl = container_of(work, struct brcmf_awdl,
					       ctrl_work);
	struct brcmf_pub *drvr = container_of(awdl, struct brcmf_pub, awdl);
	struct net_device *ndev = NULL;
	enum brcmf_awdl_op op;
	int err;

	rtnl_lock();
	wiphy_lock(drvr->wiphy);
	spin_lock(&awdl->lock);
	op = awdl->op;
	awdl->op = BRCMF_AWDL_OP_NONE;
	spin_unlock(&awdl->lock);

	switch (op) {
	case BRCMF_AWDL_OP_START:
		err = brcmf_awdl_add(drvr, awdl->start_mac, &ndev);
		if (!err) {
			awdl->has_master = false;
			err = brcmf_awdl_enable(drvr);
			if (err) {
				brcmf_awdl_del(drvr);
				ndev = NULL;
			}
		}
		brcmf_awdl_send_result(drvr, BRCMF_VNDR_EVTS_AWDL_STARTED, err,
				       ndev);
		break;
	case BRCMF_AWDL_OP_STOP:
		err = brcmf_awdl_del(drvr);
		brcmf_awdl_send_result(drvr, BRCMF_VNDR_EVTS_AWDL_STOPPED, err,
				       NULL);
		break;
	default:
		break;
	}
	wiphy_unlock(drvr->wiphy);
	rtnl_unlock();
}

/*
 * Removes the AWDL interface before system sleep, with RTNL and the wiphy
 * mutex held, while the firmware still takes commands. A queued operation is
 * dropped.
 */
void brcmf_awdl_suspend(struct brcmf_pub *drvr)
{
	struct brcmf_awdl *awdl = &drvr->awdl;
	int err;

	spin_lock(&awdl->lock);
	awdl->op = BRCMF_AWDL_OP_NONE;
	spin_unlock(&awdl->lock);
	if (!awdl->ifp)
		return;
	err = brcmf_awdl_del(drvr);
	brcmf_awdl_send_result(drvr, BRCMF_VNDR_EVTS_AWDL_STOPPED, err, NULL);
}

const struct nla_policy brcmf_awdl_vndr_policy[BRCMF_AWDL_ATTR_MAX + 1] = {
	[BRCMF_AWDL_ATTR_ADDR] = NLA_POLICY_ETH_ADDR,
	[BRCMF_AWDL_ATTR_PAYLOAD] = { .type = NLA_BINARY,
				      .len = BRCMF_AWDL_TLV_MAX },
	[BRCMF_AWDL_ATTR_OOB] = { .type = NLA_BINARY,
				  .len = BRCMF_AWDL_TLV_MAX },
	[BRCMF_AWDL_ATTR_MASTER] = NLA_POLICY_ETH_ADDR,
	[BRCMF_AWDL_ATTR_MASTER_METRIC] = { .type = NLA_U32 },
	[BRCMF_AWDL_ATTR_MASTER_DISTANCE] = { .type = NLA_U8 },
	[BRCMF_AWDL_ATTR_STATUS] = { .type = NLA_REJECT },
	[BRCMF_AWDL_ATTR_IFINDEX] = { .type = NLA_REJECT },
	[BRCMF_AWDL_ATTR_PEER_IES] = { .type = NLA_BINARY,
				       .len = BRCMF_AWDL_PEER_IES_MAX },
};

/* The device of a vendor command and its parsed attributes */
static struct brcmf_pub *brcmf_awdl_vndr_parse(struct wiphy *wiphy,
					       const void *data, int len,
					       struct nlattr **tb)
{
	struct brcmf_cfg80211_info *cfg = wiphy_to_cfg(wiphy);
	struct brcmf_pub *drvr = cfg->pub;
	struct brcmf_if *pri = brcmf_get_ifp(drvr, 0);
	int err;

	if (!pri || !brcmf_feat_is_enabled(pri, BRCMF_FEAT_AWDL))
		return ERR_PTR(-EOPNOTSUPP);
	err = nla_parse(tb, BRCMF_AWDL_ATTR_MAX, data, len,
			brcmf_awdl_vndr_policy, NULL);
	if (err)
		return ERR_PTR(err);
	return drvr;
}

int brcmf_awdl_vndr_start(struct wiphy *wiphy, struct wireless_dev *wdev,
			  const void *data, int len)
{
	struct nlattr *tb[BRCMF_AWDL_ATTR_MAX + 1];
	struct brcmf_pub *drvr;
	struct brcmf_awdl *awdl;
	int err;

	drvr = brcmf_awdl_vndr_parse(wiphy, data, len, tb);
	if (IS_ERR(drvr))
		return PTR_ERR(drvr);
	awdl = &drvr->awdl;
	if (!tb[BRCMF_AWDL_ATTR_ADDR] || !tb[BRCMF_AWDL_ATTR_PAYLOAD] ||
	    !tb[BRCMF_AWDL_ATTR_OOB])
		return -EINVAL;
	if (!is_valid_ether_addr(nla_data(tb[BRCMF_AWDL_ATTR_ADDR])))
		return -EINVAL;
	if (awdl->detached)
		return -ENODEV;
	if (awdl->ifp || awdl->op != BRCMF_AWDL_OP_NONE)
		return -EBUSY;

	err = brcmf_awdl_store_frames(awdl, tb[BRCMF_AWDL_ATTR_PAYLOAD],
				      tb[BRCMF_AWDL_ATTR_OOB]);
	if (err)
		return err;
	ether_addr_copy(awdl->start_mac, nla_data(tb[BRCMF_AWDL_ATTR_ADDR]));
	spin_lock(&awdl->lock);
	awdl->op = BRCMF_AWDL_OP_START;
	spin_unlock(&awdl->lock);
	schedule_work(&awdl->ctrl_work);
	return 0;
}

int brcmf_awdl_vndr_stop(struct wiphy *wiphy, struct wireless_dev *wdev,
			 const void *data, int len)
{
	struct nlattr *tb[BRCMF_AWDL_ATTR_MAX + 1];
	struct brcmf_pub *drvr;
	struct brcmf_awdl *awdl;

	drvr = brcmf_awdl_vndr_parse(wiphy, data, len, tb);
	if (IS_ERR(drvr))
		return PTR_ERR(drvr);
	awdl = &drvr->awdl;
	if (awdl->detached)
		return -ENODEV;
	/* A queued START is answered with AWDL_STARTED first */
	if (awdl->op != BRCMF_AWDL_OP_NONE)
		return -EBUSY;
	if (!awdl->ifp)
		return -ENOENT;

	spin_lock(&awdl->lock);
	awdl->op = BRCMF_AWDL_OP_STOP;
	spin_unlock(&awdl->lock);
	schedule_work(&awdl->ctrl_work);
	return 0;
}

int brcmf_awdl_vndr_set_frames(struct wiphy *wiphy, struct wireless_dev *wdev,
			       const void *data, int len)
{
	struct nlattr *tb[BRCMF_AWDL_ATTR_MAX + 1];
	struct brcmf_pub *drvr;
	int err;

	drvr = brcmf_awdl_vndr_parse(wiphy, data, len, tb);
	if (IS_ERR(drvr))
		return PTR_ERR(drvr);
	if (!tb[BRCMF_AWDL_ATTR_PAYLOAD] || !tb[BRCMF_AWDL_ATTR_OOB])
		return -EINVAL;
	if (!drvr->awdl.ifp)
		return -ENOENT;

	err = brcmf_awdl_store_frames(&drvr->awdl, tb[BRCMF_AWDL_ATTR_PAYLOAD],
				      tb[BRCMF_AWDL_ATTR_OOB]);
	if (err)
		return err;
	err = brcmf_awdl_refresh_frames(&drvr->awdl);
	brcmf_awdl_refresh_sync(&drvr->awdl);
	return err;
}

int brcmf_awdl_vndr_set_sync(struct wiphy *wiphy, struct wireless_dev *wdev,
			     const void *data, int len)
{
	struct nlattr *tb[BRCMF_AWDL_ATTR_MAX + 1];
	struct brcmf_pub *drvr;
	struct brcmf_awdl *awdl;

	drvr = brcmf_awdl_vndr_parse(wiphy, data, len, tb);
	if (IS_ERR(drvr))
		return PTR_ERR(drvr);
	awdl = &drvr->awdl;
	if (!awdl->ifp)
		return -ENOENT;

	if (tb[BRCMF_AWDL_ATTR_MASTER]) {
		if (!tb[BRCMF_AWDL_ATTR_MASTER_METRIC] ||
		    !tb[BRCMF_AWDL_ATTR_MASTER_DISTANCE])
			return -EINVAL;
		awdl->has_master = true;
		ether_addr_copy(awdl->master,
				nla_data(tb[BRCMF_AWDL_ATTR_MASTER]));
		awdl->master_metric =
			nla_get_u32(tb[BRCMF_AWDL_ATTR_MASTER_METRIC]);
		awdl->master_distance =
			nla_get_u8(tb[BRCMF_AWDL_ATTR_MASTER_DISTANCE]);
	} else {
		awdl->has_master = false;
	}
	brcmf_awdl_refresh_frames(awdl);
	return brcmf_awdl_refresh_sync(awdl);
}

static int brcmf_awdl_peer_op(struct wiphy *wiphy, const void *data, int len,
			      bool add)
{
	u8 op[BRCMF_AWDL_PEER_OP_HDR_LEN + BRCMF_AWDL_PEER_IES_MAX] = {};
	struct nlattr *tb[BRCMF_AWDL_ATTR_MAX + 1];
	u32 op_len = BRCMF_AWDL_PEER_OP_HDR_LEN;
	struct brcmf_pub *drvr;
	struct nlattr *ies;

	drvr = brcmf_awdl_vndr_parse(wiphy, data, len, tb);
	if (IS_ERR(drvr))
		return PTR_ERR(drvr);
	if (!tb[BRCMF_AWDL_ATTR_ADDR])
		return -EINVAL;
	if (!drvr->awdl.ifp)
		return -ENOENT;

	op[1] = add ? BRCMF_AWDL_PEER_OP_ADD : BRCMF_AWDL_PEER_OP_DEL;
	ether_addr_copy(op + 2, nla_data(tb[BRCMF_AWDL_ATTR_ADDR]));
	op[8] = add;
	if (add) {
		ies = tb[BRCMF_AWDL_ATTR_PEER_IES];
		if (ies) {
			memcpy(op + BRCMF_AWDL_PEER_OP_HDR_LEN, nla_data(ies),
			       nla_len(ies));
			op_len += nla_len(ies);
		}
		op_len = max_t(u32, op_len, BRCMF_AWDL_PEER_OP_ADD_LEN);
	}
	return brcmf_fil_iovar_data_set(drvr->awdl.ifp, "awdl_peer_op", op,
					op_len);
}

int brcmf_awdl_vndr_peer_add(struct wiphy *wiphy, struct wireless_dev *wdev,
			     const void *data, int len)
{
	return brcmf_awdl_peer_op(wiphy, data, len, true);
}

int brcmf_awdl_vndr_peer_del(struct wiphy *wiphy, struct wireless_dev *wdev,
			     const void *data, int len)
{
	return brcmf_awdl_peer_op(wiphy, data, len, false);
}

/* Called before the interfaces of the device are removed */
void brcmf_awdl_detach(struct brcmf_pub *drvr)
{
	struct brcmf_awdl *awdl = &drvr->awdl;

	rtnl_lock();
	wiphy_lock(drvr->wiphy);
	wiphy_delayed_work_cancel(drvr->wiphy, &awdl->aftx_work);
	wiphy_delayed_work_cancel(drvr->wiphy, &awdl->settle_work);
	spin_lock(&awdl->lock);
	awdl->detached = true;
	awdl->op = BRCMF_AWDL_OP_NONE;
	awdl->ifp = NULL;
	spin_unlock(&awdl->lock);
	brcmf_awdl_free_frames(awdl);
	wiphy_unlock(drvr->wiphy);
	rtnl_unlock();

	/* A queued operation finds the device detached */
	cancel_work_sync(&awdl->ctrl_work);
}
