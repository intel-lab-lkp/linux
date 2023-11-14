/* SPDX-License-Identifier: GPL-2.0 OR ISC */

#ifndef IPQESS_NOTIFIERS_H
#define IPQESS_NOTIFIERS_H

#include <linux/if_ether.h>

struct ipqess_switchdev_event_work {
	struct net_device *netdev;
	struct net_device *orig_netdev;
	struct work_struct work;
	unsigned long event;
	/* Specific for SWITCHDEV_FDB_ADD_TO_DEVICE and
	 * SWITCHDEV_FDB_DEL_TO_DEVICE
	 */
	unsigned char addr[ETH_ALEN];
	u16 vid;
	bool host_addr;
};

extern struct notifier_block ipqess_switchdev_notifier;
extern struct notifier_block ipqess_switchdev_blocking_notifier;

int ipqess_notifiers_register(void);
void ipqess_notifiers_unregister(void);

void ipqess_flush_workqueue(void);

#endif
