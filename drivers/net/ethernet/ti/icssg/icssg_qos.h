/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2023 Texas Instruments Incorporated - http://www.ti.com/
 */

#ifndef __NET_TI_ICSSG_QOS_H
#define __NET_TI_ICSSG_QOS_H

#include <linux/atomic.h>
#include <linux/netdevice.h>
#include <net/pkt_sched.h>

struct prueth_qos_mqprio {
	struct tc_mqprio_qopt_offload mqprio;
	u8 preemptible_tcs;
};

struct prueth_qos_iet {
	struct work_struct fpe_config_task;
	struct completion fpe_config_compl;
	struct prueth_emac *emac;
	atomic_t cancel_fpe_config;
	/* Set through priv flags to enable IET frame preemption */
	bool fpe_configured;
	/* Set through priv flags to enable IET MAC Verify state machine
	 * in firmware
	 */
	bool mac_verify_configured;
	/* Min TX fragment size, set via ethtool */
	u32 tx_min_frag_size;
	/* wait time between verification attempts in ms (according to clause
	 * 30.14.1.6 aMACMergeVerifyTime), set via ethtool
	 */
	u32 verify_time_ms;
	/* Set if IET FPE is active */
	bool fpe_enabled;
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
