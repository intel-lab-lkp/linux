/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2024, Intel Corporation. */

#ifndef _DEVLINK_HEALTH_H_
#define _DEVLINK_HEALTH_H_

#include <linux/types.h>

/**
 * DOC: devlink_health.h
 *
 * This header file stores everything that is needed for broadly understood
 * devlink health mechanism for ice driver.
 */

struct ice_pf;
struct ice_tx_ring;

enum ice_mdd_src {
	ICE_MDD_SRC_TX_PQM,
	ICE_MDD_SRC_TX_TCLAN,
	ICE_MDD_SRC_TX_TDPU,
	ICE_MDD_SRC_RX,
};

/**
 * struct ice_health - stores ice devlink health reporters and accompanied data
 * @tx_hang: devlink health reporter for tx_hang event
 * @mdd: devlink health reporter for MDD detection event
 */
struct ice_health {
	struct devlink_health_reporter *tx_hang;
	struct devlink_health_reporter *mdd;
};

void ice_health_init(struct ice_pf *pf);
void ice_health_deinit(struct ice_pf *pf);
void ice_health_clear(struct ice_pf *pf);

void ice_devlink_report_mdd_event(struct ice_pf *pf, enum ice_mdd_src src,
				  u8 pf_num, u16 vf_num, u8 event, u16 queue);
void ice_report_tx_hang(struct ice_pf *pf, struct ice_tx_ring *tx_ring,
			u16 vsi_num, u32 head, u32 intr);

#endif /* _DEVLINK_HEALTH_H_ */
