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
static inline int icssg_qos_validate_tx_min_frag_size(u32 min_frag_size,
						      struct netlink_ext_ack *extack)
{
	/* Firmware takes min_frag_size including FCS length */
	min_frag_size += ETH_FCS_LEN;

	/* The minimum size of the non-final mPacket supported
	 * by the firmware is 64B and multiples of 64B.
	 */
	if (min_frag_size < 64) {
		NL_SET_ERR_MSG_MOD(extack,
				   "tx_min_frag_size must be at least 64 bytes");
		return -EINVAL;
	}

	if (min_frag_size % (ETH_ZLEN + ETH_FCS_LEN)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "tx_min_frag_size must be a multiple of 64 bytes");
		return -EINVAL;
	}

	return 0;
}

static inline int icssg_qos_validate_verify_time(u32 verify_time_ms,
						 struct netlink_ext_ack *extack)
{
	/* 802.3-2018 clause 30.14.1.6: aMACMergeVerifyTime must be
	 * between 1 and 128 ms inclusive
	 */
	if (verify_time_ms < 1 || verify_time_ms > 128) {
		NL_SET_ERR_MSG_MOD(extack,
				   "verify_time must be between 1 and 128 ms");
		return -EINVAL;
	}

	return 0;
}
#endif /* __NET_TI_ICSSG_QOS_H */
