// SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause)

#include <linux/kernel.h>
#include <linux/slab.h>

#include <net/ultraeth/uet_context.h>
#include <net/ultraeth/uet_pdc.h>

/* use the approach as nf nat, try a few rounds starting at random offset */
static bool uet_pdc_id_get(struct uet_pdc *pdc)
{
	int attempts = UET_PDC_ID_MAX_ATTEMPTS, i;

	pdc->spdcid = get_random_u16();
try_again:
	for (i = 0; i < attempts; i++, pdc->spdcid++) {
		if (uet_pds_pdcid_insert(pdc) == 0)
			return true;
	}

	if (attempts > 16) {
		attempts /= 2;
		pdc->spdcid = get_random_u16();
		goto try_again;
	}

	return false;
}

struct uet_pdc *uet_pdc_create(struct uet_pds *pds, u32 rx_base_psn, u8 state,
			       u16 dpdcid, u16 pid_on_fep, u8 mode,
			       u8 tos, __be16 dport,
			       const struct uet_pdc_key *key, bool is_inbound)
{
	struct uet_pdc *pdc, *pdc_ins = ERR_PTR(-ENOMEM);
	IP_TUNNEL_DECLARE_FLAGS(md_flags) = { };
	int ret __maybe_unused;

	switch (mode) {
	case UET_PDC_MODE_RUD:
		break;
	case UET_PDC_MODE_ROD:
		fallthrough;
	case UET_PDC_MODE_RUDI:
		fallthrough;
	case UET_PDC_MODE_UUD:
		fallthrough;
	default:
		return ERR_PTR(-EOPNOTSUPP);
	}

	pdc = kzalloc(sizeof(*pdc), GFP_ATOMIC);
	if (!pdc)
		goto err_alloc;
	memcpy(&pdc->key, key, sizeof(*key));
	pdc->pds = pds;
	pdc->mode = mode;
	pdc->is_initiator = !is_inbound;

	if (!uet_pdc_id_get(pdc))
		goto err_id_get;

	spin_lock_init(&pdc->lock);

	pdc->rx_base_psn = rx_base_psn;
	pdc->tx_base_psn = rx_base_psn;
	pdc->state = state;
	pdc->dpdcid = dpdcid;
	pdc->pid_on_fep = pid_on_fep;
	pdc->metadata = __ip_tun_set_dst(key->src_ip, key->dst_ip, tos, 0, dport,
					 md_flags, 0, 0);
	if (!pdc->metadata)
		goto err_tun_dst;

#ifdef CONFIG_DST_CACHE
	ret = dst_cache_init(&pdc->metadata->u.tun_info.dst_cache, GFP_ATOMIC);
	if (ret) {
		pdc_ins = ERR_PTR(ret);
		goto err_ep_insert;
	}
#endif
	pdc->metadata->u.tun_info.mode |= IP_TUNNEL_INFO_TX;

	if (is_inbound) {
		/* this PDC is a result of packet Rx */
		pdc_ins = pdc;
		goto out;
	}

	pdc_ins = uet_pds_pdcep_insert(pdc);
	if (!pdc_ins) {
		pdc_ins = pdc;
	} else {
		/* someone beat us to it or there was an error, either way
		 * we free the newly created pdc and drop the ref
		 */
		goto err_ep_insert;
	}

out:
	return pdc_ins;

err_ep_insert:
	dst_release(&pdc->metadata->dst);
err_tun_dst:
	uet_pds_pdcid_remove(pdc);
err_id_get:
	kfree(pdc);
err_alloc:
	goto out;
}

void uet_pdc_free(struct uet_pdc *pdc)
{
	dst_release(&pdc->metadata->dst);
	kfree(pdc);
}

void uet_pdc_destroy(struct uet_pdc *pdc)
{
	uet_pds_pdcep_remove(pdc);
	uet_pds_pdcid_remove(pdc);
	uet_pds_pdc_gc_queue(pdc);
}
