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

int uet_pds_rx(struct uet_pds *pds, struct sk_buff *skb, __be32 local_fep_addr,
	       __be32 remote_fep_addr, __be16 dport, __u8 tos)
{
	if (!pskb_may_pull(skb, sizeof(struct uet_prologue_hdr)))
		return -EINVAL;

	return 0;
}
