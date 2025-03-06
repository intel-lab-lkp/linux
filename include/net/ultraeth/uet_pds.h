/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */

#ifndef _UECON_PDS_H
#define _UECON_PDS_H

#include <linux/types.h>
#include <linux/rhashtable.h>
#include <uapi/linux/ultraeth.h>
#include <linux/skbuff.h>

/**
 * struct uet_pds - Packet Delivery Sublayer state structure
 *
 * @pdcep_hash: a hash table mapping <dst ip, job id, pid on fep> to struct PDC
 * @pdcid_hash: a hash table mapping PDC id to struct PDC
 *
 * @pdcep_hash is used in fast path to find the assigned PDC, @pdcid_hash
 * is used when allocating a new PDC
 */
struct uet_pds {
	struct rhashtable pdcep_hash;
	struct rhashtable pdcid_hash;

	spinlock_t gc_lock;
	struct hlist_head pdc_gc_list;
	struct work_struct pdc_gc_work;
};

struct uet_pdc *uet_pds_pdcep_insert(struct uet_pdc *pdc);
void uet_pds_pdcep_remove(struct uet_pdc *pdc);

int uet_pds_pdcid_insert(struct uet_pdc *pdc);
void uet_pds_pdcid_remove(struct uet_pdc *pdc);

int uet_pds_init(struct uet_pds *pds);
void uet_pds_uninit(struct uet_pds *pds);

void uet_pds_pdc_gc_queue(struct uet_pdc *pdc);
void uet_pds_clean_job(struct uet_pds *pds, u32 job_id);

int uet_pds_rx(struct uet_pds *pds, struct sk_buff *skb, __be32 local_fep_addr,
	       __be32 remote_fep_addr, __be16 dport, __u8 tos);
int uet_pds_tx(struct uet_pds *pds, struct sk_buff *skb, __be32 local_fep_addr,
	       __be32 remote_fep_addr, __be16 dport, u32 job_id);

static inline struct uet_prologue_hdr *pds_prologue_hdr(const struct sk_buff *skb)
{
	return (struct uet_prologue_hdr *)skb_network_header(skb);
}

static inline struct uet_pds_req_hdr *pds_req_hdr(const struct sk_buff *skb)
{
	return (struct uet_pds_req_hdr *)skb_network_header(skb);
}

static inline struct uet_pds_ack_hdr *pds_ack_hdr(const struct sk_buff *skb)
{
	return (struct uet_pds_ack_hdr *)skb_network_header(skb);
}

static inline struct uet_pds_nack_hdr *pds_nack_hdr(const struct sk_buff *skb)
{
	return (struct uet_pds_nack_hdr *)skb_network_header(skb);
}

static inline struct uet_pds_ack_ext_hdr *pds_ack_ext_hdr(const struct sk_buff *skb)
{
	return (struct uet_pds_ack_ext_hdr *)(pds_ack_hdr(skb) + 1);
}

static inline struct uet_ses_rsp_hdr *pds_ack_ses_rsp_hdr(const struct sk_buff *skb)
{
	/* TODO: ack_ext_hdr, CC_STATE, etc. */
	return (struct uet_ses_rsp_hdr *)(pds_ack_hdr(skb) + 1);
}

static inline struct uet_ses_req_hdr *pds_req_ses_req_hdr(const struct sk_buff *skb)
{
	/* TODO: ack_ext_hdr, CC_STATE, etc. */
	return (struct uet_ses_req_hdr *)(pds_req_hdr(skb) + 1);
}

static inline __be16 pds_ses_rsp_hdr_pack(__u8 opcode, __u8 version, __u8 list,
					  __u8 ses_rc)
{
	return cpu_to_be16((opcode & UET_SES_RSP_OPCODE_MASK) <<
			   UET_SES_RSP_OPCODE_SHIFT |
			   (version & UET_SES_RSP_VERSION_MASK) <<
			   UET_SES_RSP_VERSION_SHIFT |
			   (list & UET_SES_RSP_LIST_MASK) <<
			   UET_SES_RSP_LIST_SHIFT |
			   (ses_rc & UET_SES_RSP_RC_MASK) <<
			   UET_SES_RSP_RC_SHIFT);
}
#endif /* _UECON_PDS_H */
