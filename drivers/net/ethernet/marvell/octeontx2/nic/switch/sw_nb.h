/* SPDX-License-Identifier: GPL-2.0 */
/* Marvell switch driver
 *
 * Copyright (C) 2026 Marvell.
 *
 */
#ifndef SW_NB_H_
#define SW_NB_H_

#include <linux/kconfig.h>

struct net_device;
struct otx2_nic;
struct af2pf_fdb_refresh_req;
struct msg_rsp;

int otx2_mbox_up_handler_af2pf_fdb_refresh(struct otx2_nic *pf,
					   struct af2pf_fdb_refresh_req *req,
					   struct msg_rsp *rsp);

#if IS_ENABLED(CONFIG_OCTEONTX_SWITCH)
enum {
	OTX2_DEV_UP = 1,
	OTX2_DEV_DOWN,
	OTX2_DEV_CHANGE,
	OTX2_NEIGH_UPDATE,
	OTX2_FIB_ENTRY_REPLACE,
	OTX2_FIB_ENTRY_ADD,
	OTX2_FIB_ENTRY_DEL,
	OTX2_FIB_ENTRY_APPEND,
	OTX2_CMD_MAX,
};

int sw_nb_register(struct net_device *netdev);
int sw_nb_unregister(struct net_device *netdev);
bool sw_nb_is_valid_dev(struct net_device *netdev);
struct net_device *sw_nb_resolve_pf_dev(struct net_device *dev);

bool sw_nb_is_cavium_dev(struct net_device *netdev);
int sw_nb_fib_event_to_otx2_event(int event, struct net_device *netdev);
int sw_nb_inetaddr_event_to_otx2_event(int event, struct net_device *netdev);

const char *sw_nb_get_cmd2str(int cmd);
#else
static inline int sw_nb_register(struct net_device *netdev) { return 0; }
static inline int sw_nb_unregister(struct net_device *netdev) { return 0; }
#endif

#endif /* SW_NB_H_ */
