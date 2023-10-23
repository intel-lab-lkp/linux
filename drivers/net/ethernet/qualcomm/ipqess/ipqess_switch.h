/* SPDX-License-Identifier: GPL-2.0 OR ISC */

#ifndef IPQESS_SWITCH_H
#define IPQESS_SWITCH_H

#include <linux/dsa/qca8k.h>

#define IPQESS_SWITCH_MAX_PORTS       5
#define IPQESS_SWITCH_AGEING_TIME_MIN 7000
#define IPQESS_SWITCH_AGEING_TIME_MAX 458745000
#define IPQESS_SWITCH_CPU_PORT        0
#define IPQESS_SWITCH_NUM_PORTS       5
#define IPQESS_SWITCH_ID              0x14

struct ipqess_switch {
	struct net_device *napi_leader;
	struct qca8k_priv *priv;
	struct ipqess_edma *edma;
	struct ipqess_port *port_list[IPQESS_SWITCH_MAX_PORTS];
	struct devlink *devlink;
	struct reset_control *psgmii_rst;
	bool port0_enabled;

	/* List of MAC addresses that must be forwarded on the cpu port */
	struct mutex		addr_lists_lock;
	struct list_head	fdbs;
	struct list_head	mdbs;
};

struct ipqess_devlink_priv {
	struct ipqess_switch *sw;
};

unsigned int ipqess_switch_fastest_ageing_time(struct ipqess_switch *sw,
					       unsigned int ageing_time);
int ipqess_set_ageing_time(struct ipqess_switch *sw, unsigned int msecs);

int ipqess_switch_setup(struct ipqess_switch *sw);

#endif
