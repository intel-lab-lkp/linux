/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 André Eikmeyer <dev@deq.rocks>
 *
 * AWDL interface of the Apple firmware, see awdl.c
 */
#ifndef BRCMFMAC_AWDL_H
#define BRCMFMAC_AWDL_H

#include <linux/completion.h>
#include <linux/if_ether.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <net/cfg80211.h>
#include <net/netlink.h>

struct brcmf_pub;
struct brcmf_if;
struct net_device;
struct sk_buff;

enum brcmf_awdl_op {
	BRCMF_AWDL_OP_NONE,
	BRCMF_AWDL_OP_START,
	BRCMF_AWDL_OP_STOP,
};

/**
 * struct brcmf_awdl - AWDL interface of a device.
 *
 * @lock: protects @mac, @adding, @abandoned, @removing and @ifp against the
 *	firmware event worker. Changes of @ifp also hold RTNL and the wiphy
 *	mutex.
 * @if_event: completed by the interface event of the AWDL interface.
 * @mac: MAC address requested for the AWDL interface.
 * @adding: interface_create was sent, its interface event is awaited.
 * @abandoned: the interface event did not come in time, an AWDL interface
 *	that shows up later is removed again.
 * @removing: interface_remove was sent, its interface event is awaited.
 * @detached: the device goes away, no new AWDL interface is created.
 * @ifp: the AWDL interface, NULL when there is none.
 * @tx_seq: sequence number of the next data frame sent.
 * @ctrl_work: creates or removes the AWDL interface for the vendor commands,
 *	which run without RTNL.
 * @op: operation of @ctrl_work, under @lock.
 * @start_mac: MAC address for %BRCMF_AWDL_OP_START.
 * @payload: awdl_payload with the TLVs of user space.
 * @payload_len: length of @payload.
 * @oob: awdl_oob_af_auto with the TLVs of user space.
 * @oob_len: length of @oob.
 * @has_master: another device is the master.
 * @master: MAC address of the master.
 * @master_metric: metric of the master.
 * @master_distance: hops to the master.
 * @aftx_work: changes the firmware parameters 500 ms after the start.
 * @settle_work: changes the firmware parameters 5 s after the start.
 *
 * The members from @start_mac on are protected by the wiphy mutex.
 */
struct brcmf_awdl {
	spinlock_t lock; /* AWDL state against the event worker */
	struct completion if_event;
	u8 mac[ETH_ALEN];
	bool adding;
	bool abandoned;
	bool removing;
	bool detached;
	struct brcmf_if *ifp;
	u16 tx_seq;
	struct work_struct ctrl_work;
	enum brcmf_awdl_op op;
	u8 start_mac[ETH_ALEN];
	u8 *payload;
	u32 payload_len;
	u8 *oob;
	u32 oob_len;
	bool has_master;
	u8 master[ETH_ALEN];
	u32 master_metric;
	u8 master_distance;
	struct wiphy_delayed_work aftx_work;
	struct wiphy_delayed_work settle_work;
};

/* Length of the LLC/SNAP and AWDL data header of AWDL data frames */
#define BRCMF_AWDL_HDR_LEN		16

void brcmf_awdl_attach(struct brcmf_pub *drvr);
void brcmf_awdl_detach(struct brcmf_pub *drvr);
int brcmf_awdl_add(struct brcmf_pub *drvr, const u8 *mac,
		   struct net_device **ndev);
int brcmf_awdl_del(struct brcmf_pub *drvr);
void brcmf_awdl_suspend(struct brcmf_pub *drvr);
bool brcmf_awdl_if_added(struct brcmf_pub *drvr, struct brcmf_if *ifp);
bool brcmf_awdl_if_removed(struct brcmf_pub *drvr, struct brcmf_if *ifp);
int brcmf_awdl_encap(struct brcmf_if *ifp, struct sk_buff *skb);
int brcmf_awdl_decap(struct sk_buff *skb);

extern const struct nla_policy brcmf_awdl_vndr_policy[];
int brcmf_awdl_vndr_start(struct wiphy *wiphy, struct wireless_dev *wdev,
			  const void *data, int len);
int brcmf_awdl_vndr_stop(struct wiphy *wiphy, struct wireless_dev *wdev,
			 const void *data, int len);
int brcmf_awdl_vndr_set_frames(struct wiphy *wiphy, struct wireless_dev *wdev,
			       const void *data, int len);
int brcmf_awdl_vndr_set_sync(struct wiphy *wiphy, struct wireless_dev *wdev,
			     const void *data, int len);
int brcmf_awdl_vndr_peer_add(struct wiphy *wiphy, struct wireless_dev *wdev,
			     const void *data, int len);
int brcmf_awdl_vndr_peer_del(struct wiphy *wiphy, struct wireless_dev *wdev,
			     const void *data, int len);

#endif /* BRCMFMAC_AWDL_H */
