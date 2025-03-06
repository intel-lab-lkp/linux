/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */

#ifndef _UECON_PDC_H
#define _UECON_PDC_H

#include <linux/rhashtable.h>
#include <linux/rcupdate.h>
#include <linux/spinlock.h>
#include <linux/limits.h>
#include <linux/refcount.h>
#include <net/dst.h>
#include <net/dst_metadata.h>

#define UET_PDC_ID_MAX_ATTEMPTS 128
#define UET_PDC_MAX_ID U16_MAX
#define UET_PDC_RTX_DEFAULT_TIMEOUT_SEC 30
#define UET_PDC_RTX_DEFAULT_TIMEOUT_JIFFIES (UET_PDC_RTX_DEFAULT_TIMEOUT_SEC * \
					     HZ)
#define UET_PDC_RTX_DEFAULT_TIMEOUT_NSEC (UET_PDC_RTX_DEFAULT_TIMEOUT_SEC * \
					  NSEC_PER_SEC)
#define UET_PDC_RTX_DEFAULT_MAX 3
#define UET_PDC_MPR 128

#define UET_SKB_CB(skb)       ((struct uet_skb_cb *)&((skb)->cb[0]))

struct uet_skb_cb {
	u32 psn;
	__be32 remote_fep_addr;
	u8 rtx_attempts;
};

enum {
	UET_PDC_EP_STATE_CLOSED,
	UET_PDC_EP_STATE_SYN_SENT,
	UET_PDC_EP_STATE_NEW_ESTABLISHED,
	UET_PDC_EP_STATE_ESTABLISHED,
	UET_PDC_EP_STATE_QUIESCE,
	UET_PDC_EP_STATE_ACK_WAIT,
	UET_PDC_EP_STATE_CLOSE_ACK_WAIT
};

struct uet_pdc_key {
	__be32 src_ip;
	__be32 dst_ip;
	u32 job_id;
};

enum {
	UET_PDC_MODE_ROD,
	UET_PDC_MODE_RUD,
	UET_PDC_MODE_RUDI,
	UET_PDC_MODE_UUD
};

enum mpr_pos {
	UET_PDC_MPR_PREV,
	UET_PDC_MPR_CUR,
	UET_PDC_MPR_FUTURE
};

struct uet_pdc_pkt {
	struct sk_buff *skb;
	struct timer_list rtx_timer;
	u32 psn;
	int rtx;
};

struct uet_pdc {
	struct rhash_head pdcid_node;
	struct rhash_head pdcep_node;
	struct uet_pdc_key key;
	struct uet_pds *pds;

	struct metadata_dst *metadata;

	spinlock_t lock;
	u32 psn_start;
	u16 state;
	u16 spdcid;
	u16 dpdcid;
	u16 pid_on_fep;
	u8 tx_busy;
	u8 mode;
	bool is_initiator;

	int rtx_max;
	struct timer_list rtx_timer;
	unsigned long rtx_timeout;

	unsigned long *rx_bitmap;
	unsigned long *tx_bitmap;
	unsigned long *ack_bitmap;

	u32 rx_base_psn;
	u32 tx_base_psn;

	u32 ack_gen_trigger;
	u32 ack_gen_min_pkt_add;
	u32 ack_gen_count;

	struct rb_root rtx_queue;

	struct hlist_node gc_node;
	struct rcu_head rcu;
};

struct uet_pdc *uet_pdc_create(struct uet_pds *pds, u32 rx_base_psn, u8 state,
			       u16 dpdcid, u16 pid_on_fep, u8 mode,
			       u8 tos, __be16 dport, u32 ack_gen_trigger,
			       u32 ack_gen_min_pkt_add,
			       const struct uet_pdc_key *key, bool is_inbound);
void uet_pdc_destroy(struct uet_pdc *pdc);
void uet_pdc_free(struct uet_pdc *pdc);
int uet_pdc_rx_req(struct uet_pdc *pdc, struct sk_buff *skb,
		   __be32 remote_fep_addr, __u8 tos);
int uet_pdc_rx_ack(struct uet_pdc *pdc, struct sk_buff *skb,
		   __be32 remote_fep_addr);
int uet_pdc_tx_req(struct uet_pdc *pdc, struct sk_buff *skb, u8 type);

static inline void uet_pdc_build_prologue(struct uet_prologue_hdr *prologue,
					  u8 type, u8 next, u8 flags)
{
	prologue->type_next_flags = cpu_to_be16((type & UET_PROLOGUE_TYPE_MASK) <<
						UET_PROLOGUE_TYPE_SHIFT |
						(next & UET_PROLOGUE_NEXT_MASK) <<
						UET_PROLOGUE_NEXT_SHIFT |
						(flags & UET_PROLOGUE_FLAGS_MASK));
}

static inline enum mpr_pos psn_mpr_pos(u32 base_psn, u32 psn)
{
	if (base_psn > psn)
		return UET_PDC_MPR_PREV;
	else if (psn - base_psn < UET_PDC_MPR)
		return UET_PDC_MPR_CUR;
	else
		return UET_PDC_MPR_FUTURE;
}

static inline bool psn_bit_valid(u32 bit)
{
	return bit < UET_PDC_MPR;
}

static inline bool before(u32 seq1, u32 seq2)
{
	return (s32)(seq1-seq2) < 0;
}
#endif /* _UECON_PDC_H */
