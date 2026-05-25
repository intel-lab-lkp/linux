/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2023 Texas Instruments Incorporated - http://www.ti.com/
 */

#ifndef __NET_TI_ICSSG_QOS_H
#define __NET_TI_ICSSG_QOS_H

#include <linux/atomic.h>
#include <linux/netdevice.h>
#include <net/pkt_sched.h>

#define ICSSG_MAX_TC_QUEUES			8
#define ICSSG_EXPRESS_Q_MASK_ALL		0xFF
#define ICSSG_IET_MAX_VERIFY_TIME		128
#define ICSSG_IET_MIN_VERIFY_TIME		1
#define ICSSG_IET_MAX_TX_MIN_FRAG_SIZE		252

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
};

struct prueth_qos_iet {
	bool fpe_enabled;
	bool mac_verify_configure;
	u32 tx_min_frag_size;
	u32 verify_time_ms;
	bool fpe_active;
	enum icssg_ietfpe_verify_states verify_status;
	struct mutex fpe_lock;
	u8 preemptible_tcs;
};

struct prueth_qos {
	struct prueth_qos_iet iet;
	struct prueth_qos_mqprio mqprio;
};

void icssg_qos_init(struct net_device *ndev);
void icssg_qos_link_state_update(struct net_device *ndev);
int icssg_qos_ndo_setup_tc(struct net_device *ndev, enum tc_setup_type type,
			   void *type_data);
int icssg_config_ietfpe(struct net_device *ndev, bool enable);
static inline int icssg_qos_validate_tx_min_frag_size(u32 min_frag_size,
						      struct netlink_ext_ack *extack)
{
	/* Firmware takes min_frag_size including FCS length.
	 * The firmware requires the fragment size (including FCS) to be
	 * a multiple of 64 bytes. Since 64 bytes = ETH_ZLEN + ETH_FCS_LEN,
	 * valid user-facing values are: 60, 124, 188, 252.
	 */

	if (min_frag_size < ETH_ZLEN) {
		NL_SET_ERR_MSG_MOD(extack,
				   "tx_min_frag_size must be at least 60 bytes");
		return -EINVAL;
	}

	if (min_frag_size > ICSSG_IET_MAX_TX_MIN_FRAG_SIZE) {
		NL_SET_ERR_MSG_MOD(extack,
				   "tx_min_frag_size must not exceed 252 bytes");
		return -EINVAL;
	}

	if ((min_frag_size + ETH_FCS_LEN) % (ETH_ZLEN + ETH_FCS_LEN)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "tx_min_frag_size must be a multiple of 64 bytes minus 4");
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
	if (verify_time_ms < ICSSG_IET_MIN_VERIFY_TIME ||
	    verify_time_ms > ICSSG_IET_MAX_VERIFY_TIME) {
		NL_SET_ERR_MSG_MOD(extack,
				   "verify_time must be between 1 and 128 ms");
		return -EINVAL;
	}

	return 0;
}
#endif /* __NET_TI_ICSSG_QOS_H */
