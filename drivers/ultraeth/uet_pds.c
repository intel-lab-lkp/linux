// SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause)

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/bug.h>

#include <net/ultraeth/uet_context.h>
#include <net/ultraeth/uet_pdc.h>

static const struct rhashtable_params uet_pds_pdcid_rht_params = {
	.head_offset = offsetof(struct uet_pdc, pdcid_node),
	.key_offset = offsetof(struct uet_pdc, spdcid),
	.key_len = sizeof(u16),
	.nelem_hint = 2048,
	.max_size = UET_PDC_MAX_ID,
	.automatic_shrinking = true,
};

static const struct rhashtable_params uet_pds_pdcep_rht_params = {
	.head_offset = offsetof(struct uet_pdc, pdcep_node),
	.key_offset = offsetof(struct uet_pdc, key),
	.key_len = sizeof(struct uet_pdc_key),
	.nelem_hint = 2048,
	.automatic_shrinking = true,
};

static void uet_pds_pdc_gc_flush(struct uet_pds *pds)
{
	HLIST_HEAD(deleted_head);
	struct hlist_node *tmp;
	struct uet_pdc *pdc;

	spin_lock_bh(&pds->gc_lock);
	hlist_move_list(&pds->pdc_gc_list, &deleted_head);
	spin_unlock_bh(&pds->gc_lock);

	synchronize_rcu();

	hlist_for_each_entry_safe(pdc, tmp, &deleted_head, gc_node)
		uet_pdc_free(pdc);
}

static void uet_pds_pdc_gc_work(struct work_struct *work)
{
	struct uet_pds *pds = container_of(work, struct uet_pds, pdc_gc_work);

	uet_pds_pdc_gc_flush(pds);
}

void uet_pds_pdc_gc_queue(struct uet_pdc *pdc)
{
	struct uet_pds *pds = pdc->pds;

	spin_lock_bh(&pds->gc_lock);
	if (hlist_unhashed(&pdc->gc_node))
		hlist_add_head(&pdc->gc_node, &pds->pdc_gc_list);
	spin_unlock_bh(&pds->gc_lock);

	queue_work(system_long_wq, &pds->pdc_gc_work);
}

int uet_pds_init(struct uet_pds *pds)
{
	int ret;

	spin_lock_init(&pds->gc_lock);
	INIT_HLIST_HEAD(&pds->pdc_gc_list);
	INIT_WORK(&pds->pdc_gc_work, uet_pds_pdc_gc_work);

	ret = rhashtable_init(&pds->pdcid_hash, &uet_pds_pdcid_rht_params);
	if (ret)
		goto err_pdcid_hash;

	ret = rhashtable_init(&pds->pdcep_hash, &uet_pds_pdcep_rht_params);
	if (ret)
		goto err_pdcep_hash;

	return 0;

err_pdcep_hash:
	rhashtable_destroy(&pds->pdcid_hash);
err_pdcid_hash:
	return ret;
}

struct uet_pdc *uet_pds_pdcep_insert(struct uet_pdc *pdc)
{
	struct uet_pds *pds = pdc->pds;

	return rhashtable_lookup_get_insert_fast(&pds->pdcep_hash,
						 &pdc->pdcep_node,
						 uet_pds_pdcep_rht_params);
}

void uet_pds_pdcep_remove(struct uet_pdc *pdc)
{
	struct uet_pds *pds = pdc->pds;

	rhashtable_remove_fast(&pds->pdcep_hash, &pdc->pdcep_node,
			       uet_pds_pdcep_rht_params);
}

int uet_pds_pdcid_insert(struct uet_pdc *pdc)
{
	struct uet_pds *pds = pdc->pds;

	return rhashtable_insert_fast(&pds->pdcid_hash, &pdc->pdcid_node,
				      uet_pds_pdcid_rht_params);
}

void uet_pds_pdcid_remove(struct uet_pdc *pdc)
{
	struct uet_pds *pds = pdc->pds;

	rhashtable_remove_fast(&pds->pdcid_hash, &pdc->pdcid_node,
			       uet_pds_pdcid_rht_params);
}

static void uet_pds_pdcep_hash_free(void *ptr, void *arg)
{
	struct uet_pdc *pdc = ptr;

	uet_pdc_destroy(pdc);
}

void uet_pds_uninit(struct uet_pds *pds)
{
	rhashtable_free_and_destroy(&pds->pdcep_hash, uet_pds_pdcep_hash_free, NULL);
	/* the above call should also release all PDC ids */
	WARN_ON(atomic_read(&pds->pdcid_hash.nelems));
	rhashtable_destroy(&pds->pdcid_hash);
	uet_pds_pdc_gc_flush(pds);
	cancel_work_sync(&pds->pdc_gc_work);
	rcu_barrier();
}

void uet_pds_clean_job(struct uet_pds *pds, u32 job_id)
{
	struct rhashtable_iter iter;
	struct uet_pdc *pdc;

	rhashtable_walk_enter(&pds->pdcid_hash, &iter);
	rhashtable_walk_start(&iter);
	while ((pdc = rhashtable_walk_next(&iter))) {
		if (pdc->key.job_id == job_id)
			uet_pdc_destroy(pdc);
	}
	rhashtable_walk_stop(&iter);
	rhashtable_walk_exit(&iter);
}

static int uet_pds_rx_ack(struct uet_pds *pds, struct sk_buff *skb,
			  __be32 local_fep_addr, __be32 remote_fep_addr)
{
	struct uet_pds_req_hdr *pds_req = pds_req_hdr(skb);
	u16 pdcid = be16_to_cpu(pds_req->dpdcid);
	struct uet_pdc *pdc;

	pdc = rhashtable_lookup_fast(&pds->pdcid_hash, &pdcid,
				     uet_pds_pdcid_rht_params);
	if (!pdc)
		return -ENOENT;

	return uet_pdc_rx_ack(pdc, skb, remote_fep_addr);
}

static struct uet_pdc *uet_pds_new_pdc_rx(struct uet_pds *pds,
					  struct sk_buff *skb,
					  __be16 dport,
					  struct uet_pdc_key *key,
					  u8 mode, u8 state)
{
	struct uet_ses_req_hdr *ses_req = pds_req_ses_req_hdr(skb);
	struct uet_pds_req_hdr *req = pds_req_hdr(skb);

	return uet_pdc_create(pds, be32_to_cpu(req->psn), state,
			      be16_to_cpu(req->spdcid),
			      uet_ses_req_pid_on_fep(ses_req),
			      mode, 0, dport, key, true);
}

static int uet_pds_rx_req(struct uet_pds *pds, struct sk_buff *skb,
			  __be32 local_fep_addr, __be32 remote_fep_addr,
			  __be16 dport, __u8 tos)
{
	struct uet_ses_req_hdr *ses_req = pds_req_ses_req_hdr(skb);
	struct uet_pds_req_hdr *pds_req = pds_req_hdr(skb);
	u16 pdcid = be16_to_cpu(pds_req->dpdcid);
	struct uet_pdc_key key = {};
	struct uet_fep *fep;
	struct uet_pdc *pdc;

	key.src_ip = local_fep_addr;
	key.dst_ip = remote_fep_addr;
	key.job_id = uet_ses_req_job_id(ses_req);

	pdc = rhashtable_lookup_fast(&pds->pdcid_hash, &pdcid,
				     uet_pds_pdcid_rht_params);
	/* new flow */
	if (unlikely(!pdc)) {
		struct uet_prologue_hdr *prologue = pds_prologue_hdr(skb);
		struct uet_context *ctx;
		struct uet_job *job;

		if (!(uet_prologue_flags(prologue) & UET_PDS_REQ_FLAG_SYN))
			return -EINVAL;

		ctx = container_of(pds, struct uet_context, pds);
		job = uet_job_find(&ctx->job_reg, key.job_id);
		if (!job)
			return -ENOENT;
		fep = rcu_dereference(job->fep);
		if (!fep)
			return -ECONNREFUSED;
		if (fep->addr.in_address.ip != local_fep_addr)
			return -ENOENT;

		pdc = uet_pds_new_pdc_rx(pds, skb, dport, &key,
					 UET_PDC_MODE_RUD,
					 UET_PDC_EP_STATE_NEW_ESTABLISHED);
		if (IS_ERR(pdc))
			return PTR_ERR(pdc);
	}

	return uet_pdc_rx_req(pdc, skb, remote_fep_addr, tos);
}

static bool uet_pds_rx_valid_req_next_hdr(const struct uet_prologue_hdr *prologue)
{
	switch (uet_prologue_next_hdr(prologue)) {
	case UET_PDS_NEXT_HDR_REQ_STD:
		break;
	default:
		return false;
	}

	return true;
}

static bool uet_pds_rx_valid_ack_next_hdr(const struct uet_prologue_hdr *prologue)
{
	switch (uet_prologue_next_hdr(prologue)) {
	case UET_PDS_NEXT_HDR_RSP:
	case UET_PDS_NEXT_HDR_RSP_DATA:
	case UET_PDS_NEXT_HDR_RSP_DATA_SMALL:
		break;
	default:
		return false;
	}

	return true;
}

int uet_pds_rx(struct uet_pds *pds, struct sk_buff *skb, __be32 local_fep_addr,
	       __be32 remote_fep_addr, __be16 dport, __u8 tos)
{
	struct uet_prologue_hdr *prologue;
	unsigned int offset = 0;
	int ret = -EINVAL;

	if (!pskb_may_pull(skb, sizeof(struct uet_prologue_hdr)))
		return ret;

	prologue = pds_prologue_hdr(skb);
	switch (uet_prologue_type(prologue)) {
	case UET_PDS_TYPE_ACK:
		if (!uet_pds_rx_valid_ack_next_hdr(prologue))
			break;
		offset += sizeof(struct uet_pds_ack_hdr) +
			  sizeof(struct uet_ses_rsp_hdr);
		if (!pskb_may_pull(skb, offset))
			break;

		__net_timestamp(skb);
		ret = uet_pds_rx_ack(pds, skb, local_fep_addr, remote_fep_addr);
		break;
	case UET_PDS_TYPE_RUD_REQ:
		if (!uet_pds_rx_valid_req_next_hdr(prologue))
			break;
		offset = sizeof(struct uet_pds_ack_hdr) +
			 sizeof(struct uet_ses_req_hdr);
		if (!pskb_may_pull(skb, offset))
			break;
		ret = uet_pds_rx_req(pds, skb, local_fep_addr, remote_fep_addr,
				     dport, tos);
		break;
	default:
		break;
	}

	return ret;
}
