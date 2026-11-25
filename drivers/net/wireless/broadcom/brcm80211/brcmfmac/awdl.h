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

struct brcmf_pub;
struct brcmf_if;

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
};

void brcmf_awdl_attach(struct brcmf_pub *drvr);
void brcmf_awdl_detach(struct brcmf_pub *drvr);
int brcmf_awdl_add(struct brcmf_pub *drvr, const u8 *mac,
		   struct net_device **ndev);
int brcmf_awdl_del(struct brcmf_pub *drvr);
bool brcmf_awdl_if_added(struct brcmf_pub *drvr, struct brcmf_if *ifp);
bool brcmf_awdl_if_removed(struct brcmf_pub *drvr, struct brcmf_if *ifp);

#endif /* BRCMFMAC_AWDL_H */
