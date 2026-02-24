/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2023 Texas Instruments Incorporated - http://www.ti.com/
 */

#ifndef __NET_TI_ICSSG_QOS_H
#define __NET_TI_ICSSG_QOS_H

#include <linux/atomic.h>
#include <linux/netdevice.h>
#include <net/pkt_sched.h>

enum icssg_ietfpe_verify_states {
	ICSSG_IETFPE_STATE_UNKNOWN = 0,
	ICSSG_IETFPE_STATE_INITIAL,
	ICSSG_IETFPE_STATE_VERIFYING,
	ICSSG_IETFPE_STATE_SUCCEEDED,
	ICSSG_IETFPE_STATE_FAILED,
	ICSSG_IETFPE_STATE_DISABLED
};

struct prueth_qos_mqprio {
	struct tc_mqprio_qopt_offload mqprio;
	u8 preemptible_tcs;
};

struct prueth_qos_iet {
	struct work_struct fpe_config_task;
	struct prueth_emac *emac;
	atomic_t enable_fpe_config;
	/* Set when IET frame preemption is enabled via ethtool */
	bool fpe_enabled;
	/* Set when the IET MAC Verify state machine is enabled
	 * via ethtool
	 */
	bool mac_verify_configure;
	/* Min TX fragment size, set via ethtool */
	u32 tx_min_frag_size;
	/* wait time between verification attempts in ms (according to clause
	 * 30.14.1.6 aMACMergeVerifyTime), set via ethtool
	 */
	u32 verify_time_ms;
	/* Set if IET FPE is active */
	bool fpe_active;
	/* State of verification state machine */
	enum icssg_ietfpe_verify_states verify_status;
	/* Mutex to serialize FPE configuration access */
	struct mutex fpe_lock;
};

struct prueth_qos {
	struct prueth_qos_iet iet;
	struct prueth_qos_mqprio mqprio;
};

void icssg_qos_init(struct net_device *ndev);
void icssg_qos_link_up(struct net_device *ndev);
void icssg_qos_link_down(struct net_device *ndev);
int icssg_qos_ndo_setup_tc(struct net_device *ndev, enum tc_setup_type type,
			   void *type_data);
#endif /* __NET_TI_ICSSG_QOS_H */
