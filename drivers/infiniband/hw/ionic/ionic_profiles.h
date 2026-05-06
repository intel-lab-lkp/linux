/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2018-2026, Advanced Micro Devices, Inc. */

#ifndef _IONIC_PROFILES_H_
#define _IONIC_PROFILES_H_

#include "ionic_ibdev.h"

enum ionic_dcqcn_var {
	/* notification point */
	NP_ICNP_802P_PRIO,		/* 0..7 (prio) */
	NP_CNP_DSCP,			/* 0..63 (dscp) */

	RP_TOKEN_BUCKET_SIZE,		/* 100..200000000 (100kb - 200gb) */
	/* reaction point alpha update */
	RP_INITIAL_ALPHA_VALUE,		/* 0..1023 */
	RP_DCE_TCP_G,			/* 0..1023 */
	RP_DCE_TCP_RTT,			/* 1..131071 (us) */

	/* reaction point rate decrease */
	RP_RATE_REDUCE_MONITOR_PERIOD,	/* 1.. (us) */
	RP_RATE_TO_SET_ON_FIRST_CNP,	/* 0 disable, 1.. (Mbps) */
	RP_MIN_RATE,			/* 1.. (Mbps) */
	RP_GD,				/* 1..11 */
	RP_MIN_DEC_FAC,			/* 0..100 (%) */

	/* reaction point rate increase */
	RP_CLAMP_TGT_RATE,		/* 0..1 (bool) */
	RP_CLAMP_TGT_RATE_ATI,		/* 0..1 (bool) */
	RP_THRESHOLD,			/* 1..31 */
	RP_TIME_RESET,			/* 1..32767 (x RP_DCE_TCP_RTT) */
	RP_QP_RATE,			/* 1.. (Mbps) */
	RP_BYTE_RESET,			/* 1..4294967296 (B) */
	RP_AI_RATE,			/* 1.. (Mbps) */
	RP_HAI_RATE,			/* 1.. (Mbps) */

	DCQCN_VAR_COUNT
};

struct ionic_match_rule {
	bool			(*match)(struct rdma_ah_attr *attr, int cond);
	const char		*name;
	int			cond;
	int			prof;
};

struct ionic_profile_vals {
	int			v[DCQCN_VAR_COUNT];
};

struct ionic_dcqcn_param_attr {
	char			*name;
	int			min;
	int			max;
};

struct ionic_dcqcn_param_entry {
	struct ionic_profile	*profile;
	enum ionic_dcqcn_var	var;
};

struct ionic_profile {
	struct ionic_ibdev		*dev;
	struct ionic_profile_vals	vals;
	struct ionic_dcqcn_param_entry	*entries;
	int idx;

	struct dentry			*debug;
	struct dentry			*roce_np_debug;
	struct dentry			*roce_rp_debug;
};

struct ionic_profile_root {
	struct ionic_ibdev	*dev;
	int			profiles_default;
	int			profiles_count;
	struct ionic_profile	*profiles;
	spinlock_t		rules_lock;	/* lock to atomic update the rules */
	int			rules_count;
	struct ionic_match_rule	*rules;

	struct dentry		*debug;
	struct dentry		*profiles_debug;
};

#endif /* _IONIC_PROFILES_H_ */
