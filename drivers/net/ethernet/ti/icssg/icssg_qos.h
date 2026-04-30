/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2023 Texas Instruments Incorporated - http://www.ti.com/
 */

#ifndef __NET_TI_ICSSG_QOS_H
#define __NET_TI_ICSSG_QOS_H

#include <linux/atomic.h>
#include <linux/netdevice.h>
#include <net/pkt_sched.h>

/**
 * enum icssg_ietfpe_verify_states - status of MAC Merge Verify returned by firmware
 * @ICSSG_IETFPE_STATE_UNKNOWN:
 *	verification status is unknown
 * @ICSSG_IETFPE_STATE_INITIAL:
 *	Firmware returns this if verify state diagram is idle
 * @ICSSG_IETFPE_STATE_VERIFYING:
 *	Firmware returns this if verification is ongoing
 * @ICSSG_IETFPE_STATE_SUCCEEDED:
 *	Firmware returns this if verify state diagram completes verification
 * @ICSSG_IETFPE_STATE_FAILED:
 *	Firmware returns this if verify state diagram fails during verification
 * @ICSSG_IETFPE_STATE_DISABLED:
 *	verification is disabled by the driver
 */
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
	struct prueth_emac *emac;

	/* Configuration state - protected by fpe_lock */
	bool fpe_enabled;
	bool mac_verify_configure;
	u32 tx_min_frag_size;
	u32 verify_time_ms;

	/* Runtime state - protected by fpe_lock */
	bool fpe_active;
	enum icssg_ietfpe_verify_states verify_status;

	/* Synchronization: single mutex protects all FPE operations */
	struct mutex fpe_lock;
};

struct prueth_qos {
	struct prueth_qos_iet iet;
	struct prueth_qos_mqprio mqprio;
};

void icssg_qos_init(struct net_device *ndev);
void icssg_qos_link_state_update(struct net_device *ndev);
int icssg_qos_ndo_setup_tc(struct net_device *ndev, enum tc_setup_type type,
			   void *type_data);
void icssg_config_ietfpe(struct prueth_emac *emac, bool enable);
#endif /* __NET_TI_ICSSG_QOS_H */
