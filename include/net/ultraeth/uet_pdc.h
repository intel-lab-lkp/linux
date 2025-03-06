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
#define UET_PDC_MPR 128

#define UET_SKB_CB(skb)       ((struct uet_skb_cb *)&((skb)->cb[0]))

struct uet_skb_cb {
	u32 psn;
	__be32 remote_fep_addr;
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

	u32 rx_base_psn;
	u32 tx_base_psn;

	struct hlist_node gc_node;
	struct rcu_head rcu;
};

struct uet_pdc *uet_pdc_create(struct uet_pds *pds, u32 rx_base_psn, u8 state,
			       u16 dpdcid, u16 pid_on_fep, u8 mode,
			       u8 tos, __be16 dport,
			       const struct uet_pdc_key *key, bool is_inbound);
void uet_pdc_destroy(struct uet_pdc *pdc);
void uet_pdc_free(struct uet_pdc *pdc);
#endif /* _UECON_PDC_H */
