/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */
/* Copyright 2026 NXP */

#ifndef __LINUX_TC_FRER_H
#define __LINUX_TC_FRER_H

#include <linux/pkt_cls.h>

/* Base parameters passed in TCA_FRER_PARMS */
struct tc_frer {
	tc_gen;
};

/**
 * enum TCA_FRER_* - netlink attributes for the FRER tc action
 *
 * @TCA_FRER_FUNC:             Functional sub-command (tc_frer_func).
 *                             Mandatory.
 * @TCA_FRER_TAG_TYPE:         Redundancy tag type (tc_frer_tag_type).
 *                             Mandatory.
 *
 * Push-specific attributes (TCA_FRER_FUNC_PUSH):
 * Recover-specific attributes (TCA_FRER_FUNC_RECOVER):
 * @TCA_FRER_RCVY_INDIVIDUAL:  Flag. Force Individual Recovery.
 * @TCA_FRER_RCVY_ALG:         u8. Recovery algorithm (tc_frer_rcvy_alg).
 * @TCA_FRER_RCVY_HISTORY_LEN: u8. SequenceHistory window size (1-32).
 *                             Maps to frerSeqRcvyHistoryLength.
 * @TCA_FRER_RCVY_RESET_MSEC:  u32. Reset timer in milliseconds.
 *                             0 disables the timer.
 *                             Maps to frerSeqRcvyResetMSec.
 * @TCA_FRER_RCVY_TAKE_NO_SEQ: Flag. Accept frames without a redundancy
 *                             tag and pass them unconditionally.
 *                             Maps to frerSeqRcvyTakeNoSeq.
 * @TCA_FRER_RCVY_TAG_POP:     Flag. Remove the redundancy tag from
 *                             frames that pass the recovery function.
 *
 * Read-only statistics (filled on dump, IEEE 802.1CB Table 10-1):
 * @TCA_FRER_STATS_TAGLESS_PKTS:       frerCpsSeqRcvyTaglessPackets
 * @TCA_FRER_STATS_OUT_OF_ORDER_PKTS:  frerCpsSeqRcvyOutOfOrderPackets
 * @TCA_FRER_STATS_ROGUE_PKTS:         frerCpsSeqRcvyRoguePackets
 * @TCA_FRER_STATS_LOST_PKTS:          frerCpsSeqRcvyLostPackets
 * @TCA_FRER_STATS_RESETS:             frerCpsSeqRcvyResets
 * @TCA_FRER_STATS_PASSED_PKTS:        frerCpsSeqRcvyPassedPackets
 * @TCA_FRER_STATS_DISCARDED_PKTS:     frerCpsSeqRcvyDiscardedPackets
 * @TCA_FRER_STATS_SEQGEN_PKTS:        frerCpsSeqGenPackets
 */
enum {
	TCA_FRER_UNSPEC,
	TCA_FRER_TM,                       /* struct tcf_t */
	TCA_FRER_PARMS,                    /* struct tc_frer */
	TCA_FRER_PAD,
	TCA_FRER_FUNC,                     /* u8: tc_frer_func */
	TCA_FRER_TAG_TYPE,                 /* u8: tc_frer_tag_type */
	TCA_FRER_RCVY_INDIVIDUAL,          /* NLA_FLAG */
	TCA_FRER_RCVY_ALG,                 /* u8: tc_frer_rcvy_alg */
	TCA_FRER_RCVY_HISTORY_LEN,         /* u8: 1-32 */
	TCA_FRER_RCVY_RESET_MSEC,          /* u32 */
	TCA_FRER_RCVY_TAKE_NO_SEQ,         /* NLA_FLAG */
	TCA_FRER_RCVY_TAG_POP,             /* NLA_FLAG */
	TCA_FRER_STATS_TAGLESS_PKTS,       /* u64 */
	TCA_FRER_STATS_OUT_OF_ORDER_PKTS,  /* u64 */
	TCA_FRER_STATS_ROGUE_PKTS,         /* u64 */
	TCA_FRER_STATS_LOST_PKTS,          /* u64 */
	TCA_FRER_STATS_RESETS,             /* u64 */
	TCA_FRER_STATS_PASSED_PKTS,        /* u64 */
	TCA_FRER_STATS_DISCARDED_PKTS,     /* u64 */
	TCA_FRER_STATS_SEQGEN_PKTS,        /* u64 */
	__TCA_FRER_MAX,
};

#define TCA_FRER_MAX (__TCA_FRER_MAX - 1)

enum tc_frer_func {
	TCA_FRER_FUNC_PUSH    = 1,
	TCA_FRER_FUNC_RECOVER = 2,
};

enum tc_frer_tag_type {
	TCA_FRER_TAG_RTAG = 1,
	TCA_FRER_TAG_HSR,
	TCA_FRER_TAG_PRP,
};

enum tc_frer_rcvy_alg {
	TCA_FRER_RCVY_VECTOR_ALG = 0,  /* IEEE 802.1CB 7.4.3.4 */
	TCA_FRER_RCVY_MATCH_ALG  = 1,  /* IEEE 802.1CB 7.4.3.5 */
};

#endif /* __LINUX_TC_FRER_H */
