/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 NXP */

#ifndef __NET_TC_FRER_H
#define __NET_TC_FRER_H

#include <net/act_api.h>
#include <linux/tc_act/tc_frer.h>

/**
 * struct frer_seqgen - sequence number generator state (embedded in tcf_frer)
 */
struct frer_seqgen {
	u32		gen_seq_num;
	u64		seq_space;	/* 1 << 16 */
	spinlock_t	lock;		/* protects frer_seqgen state */
	u64		stats_pkts;	/* frerCpsSeqGenPackets */
};

/**
 * struct frer_rcvy - sequence recovery state (embedded in tcf_frer)
 *
 */
struct frer_rcvy {
	u8		alg;
	u8		history_len;	/* 1-32 */
	u32		reset_msec;
	u64		seq_space;
	u32		rcvy_seq_num;
	u32		seq_history;
	bool		take_any;
	bool		take_no_seq;
	struct hrtimer	hrtimer;
	spinlock_t	lock;		/* protects frer_rcvy state */
	/* statistics */
	u64		stats_tagless_pkts;
	u64		stats_out_of_order_pkts;
	u64		stats_rogue_pkts;
	u64		stats_lost_pkts;
	u64		stats_resets;
	u64		stats_passed_pkts;
	u64		stats_discarded_pkts;
};

/**
 * struct tcf_frer - per tc_action FRER private data
 */
struct tcf_frer {
	struct tc_action	common;
	u8			func;
	u8			tag_type;
	bool			tag_pop;
	bool			individual;	/* Individual Recovery flag */
	/* push path */
	struct frer_seqgen	seqgen;
	/* recover path */
	struct frer_rcvy	rcvy;
};

#define to_frer(a) ((struct tcf_frer *)(a))

static inline bool is_tcf_frer(const struct tc_action *a)
{
#ifdef CONFIG_NET_CLS_ACT
	if (a->ops && a->ops->id == TCA_ID_FRER)
		return true;
#endif
	return false;
}

#endif /* __NET_TC_FRER_H */
