// SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause)

#include <linux/kernel.h>
#include <linux/slab.h>

#include <net/ultraeth/uet_context.h>
#include <net/ultraeth/uet_pdc.h>

static void uet_pdc_xmit(struct uet_pdc *pdc, struct sk_buff *skb)
{
	skb->dev = pds_netdev(pdc->pds);

	if (!dst_hold_safe(&pdc->metadata->dst)) {
		kfree_skb(skb);
		return;
	}

	skb_dst_set(skb, &pdc->metadata->dst);
	dev_queue_xmit(skb);
}

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
	pdc->rx_bitmap = bitmap_zalloc(UET_PDC_MPR, GFP_ATOMIC);
	if (!pdc->rx_bitmap)
		goto err_rx_bitmap;
	pdc->ack_bitmap = bitmap_zalloc(UET_PDC_MPR, GFP_ATOMIC);
	if (!pdc->ack_bitmap)
		goto err_ack_bitmap;
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
	bitmap_free(pdc->ack_bitmap);
err_ack_bitmap:
	bitmap_free(pdc->rx_bitmap);
err_rx_bitmap:
	uet_pds_pdcid_remove(pdc);
err_id_get:
	kfree(pdc);
err_alloc:
	goto out;
}

void uet_pdc_free(struct uet_pdc *pdc)
{
	dst_release(&pdc->metadata->dst);
	bitmap_free(pdc->ack_bitmap);
	bitmap_free(pdc->rx_bitmap);
	kfree(pdc);
}

void uet_pdc_destroy(struct uet_pdc *pdc)
{
	uet_pds_pdcep_remove(pdc);
	uet_pds_pdcid_remove(pdc);
	uet_pds_pdc_gc_queue(pdc);
}

static void pdc_build_ack(struct uet_pdc *pdc, struct sk_buff *skb, u32 psn,
			  u8 ack_flags, bool exact_psn)
{
	struct uet_pds_ack_hdr *ack = skb_put(skb, sizeof(*ack));

	uet_pdc_build_prologue(&ack->prologue, UET_PDS_TYPE_ACK,
			       UET_PDS_NEXT_HDR_RSP, ack_flags);
	if (exact_psn) {
		ack->ack_psn_offset = 0;
		ack->cack_psn = cpu_to_be32(psn);
	} else {
		ack->ack_psn_offset = cpu_to_be16(psn - pdc->rx_base_psn);
		ack->cack_psn = cpu_to_be32(pdc->rx_base_psn);
	}
	ack->spdcid = cpu_to_be16(pdc->spdcid);
	ack->dpdcid = cpu_to_be16(pdc->dpdcid);
}

static void uet_pdc_build_ses_ack(struct uet_pdc *pdc, struct sk_buff *skb,
				  __u8 ses_rc, __be16 msg_id, u32 psn,
				  u8 ack_flags, bool exact_psn)
{
	struct uet_ses_rsp_hdr *ses_rsp;
	__be16 packed;

	pdc_build_ack(pdc, skb, psn, ack_flags, exact_psn);
	ses_rsp = skb_put(skb, sizeof(*ses_rsp));
	memset(ses_rsp, 0, sizeof(*ses_rsp));
	packed = pds_ses_rsp_hdr_pack(UET_SES_RSP_OP_RESPONSE, 0,
				      UET_SES_RSP_LIST_EXPECTED, ses_rc);
	ses_rsp->lst_opcode_ver_rc = packed;
	ses_rsp->idx_gen_job_id = cpu_to_be32(pdc->key.job_id);
	ses_rsp->msg_id = msg_id;
}

static int uet_pdc_send_ses_ack(struct uet_pdc *pdc, __u8 ses_rc, __be16 msg_id,
				u32 psn, u8 ack_flags, bool exact_psn)
{
	struct sk_buff *skb;

	skb = alloc_skb(sizeof(struct uet_ses_rsp_hdr) +
			sizeof(struct uet_pds_ack_hdr), GFP_ATOMIC);
	if (!skb)
		return -ENOBUFS;

	uet_pdc_build_ses_ack(pdc, skb, ses_rc, msg_id, psn, ack_flags,
			      exact_psn);
	uet_pdc_xmit(pdc, skb);

	return 0;
}

static void uet_pdc_mpr_advance_tx(struct uet_pdc *pdc, u32 bits)
{
	if (!test_bit(0, pdc->ack_bitmap))
		return;

	bitmap_shift_right(pdc->ack_bitmap, pdc->ack_bitmap, bits, UET_PDC_MPR);
	pdc->tx_base_psn += bits;
	netdev_dbg(pds_netdev(pdc->pds), "%s: advancing tx to %u\n", __func__,
		   pdc->tx_base_psn);
}

int uet_pdc_rx_ack(struct uet_pdc *pdc, struct sk_buff *skb,
		   __be32 remote_fep_addr)
{
	struct uet_ses_rsp_hdr *ses_rsp = pds_ack_ses_rsp_hdr(skb);
	struct uet_pds_ack_hdr *ack = pds_ack_hdr(skb);
	s16 ack_psn_offset = be16_to_cpu(ack->ack_psn_offset);
	const char *drop_reason = "ack_psn not in MPR window";
	u32 cack_psn = be32_to_cpu(ack->cack_psn);
	u32 ack_psn = cack_psn + ack_psn_offset;
	int ret = -EINVAL;
	u32 psn_bit;

	spin_lock(&pdc->lock);
	netdev_dbg(pds_netdev(pdc->pds), "%s: tx_busy: %u pdc: [ tx_base_psn: %u"
				  " state: %u dpdcid: %u spdcid: %u ]\n"
				  "ses: [ msg id: %u cack_psn: %u spdcid: %u"
				  " dpdcid: %u ack_psn: %u ]\n",
		   __func__, pdc->tx_busy, pdc->tx_base_psn,
		   pdc->state, pdc->dpdcid, pdc->spdcid,
		   be16_to_cpu(ses_rsp->msg_id), be32_to_cpu(ack->cack_psn),
		   be16_to_cpu(ack->spdcid), be16_to_cpu(ack->dpdcid), ack_psn);

	if (psn_mpr_pos(pdc->tx_base_psn, ack_psn) != UET_PDC_MPR_CUR)
		goto err_dbg;

	psn_bit = ack_psn - pdc->tx_base_psn;
	if (!psn_bit_valid(psn_bit)) {
		drop_reason = "ack_psn bit is invalid";
		goto err_dbg;
	}
	if (test_and_set_bit(psn_bit, pdc->ack_bitmap)) {
		drop_reason = "ack_psn bit already set in ack_bitmap";
		goto err_dbg;
	}

	/* either using ROD mode or in SYN_SENT state */
	if (pdc->tx_busy)
		pdc->tx_busy = false;
	/* we can advance only if the oldest pkt got acked */
	if (!psn_bit)
		uet_pdc_mpr_advance_tx(pdc, 1);

	ret = 0;
	switch (pdc->state) {
	case UET_PDC_EP_STATE_SYN_SENT:
	case UET_PDC_EP_STATE_NEW_ESTABLISHED:
		pdc->dpdcid = be16_to_cpu(ack->spdcid);
		pdc->state = UET_PDC_EP_STATE_ESTABLISHED;
		fallthrough;
	case UET_PDC_EP_STATE_ESTABLISHED:
		ret = uet_job_fep_queue_skb(pds_context(pdc->pds),
					    uet_ses_rsp_job_id(ses_rsp), skb,
					    remote_fep_addr);
		break;
	case UET_PDC_EP_STATE_ACK_WAIT:
		break;
	case UET_PDC_EP_STATE_CLOSE_ACK_WAIT:
		break;
	}

out:
	spin_unlock(&pdc->lock);

	return ret;
err_dbg:
	netdev_dbg(pds_netdev(pdc->pds), "%s: drop reason: [ %s ]\n"
				  "pdc: [ tx_base_psn: %u state: %u"
				  " dpdcid: %u spdcid: %u ]\n"
				  "ses: [ msg id: %u cack_psn: %u spdcid: %u"
				  " dpdcid: %u ack_psn: %u ]\n",
		  __func__, drop_reason, pdc->tx_base_psn,
		  pdc->state, pdc->dpdcid, pdc->spdcid,
		  be16_to_cpu(ses_rsp->msg_id), be32_to_cpu(ack->cack_psn),
		  be16_to_cpu(ack->spdcid), be16_to_cpu(ack->dpdcid), ack_psn);
	goto out;
}

static void uet_pdc_mpr_advance_rx(struct uet_pdc *pdc)
{
	if (!test_bit(0, pdc->rx_bitmap))
		return;

	bitmap_shift_right(pdc->rx_bitmap, pdc->rx_bitmap, 1, UET_PDC_MPR);
	pdc->rx_base_psn++;
	netdev_dbg(pds_netdev(pdc->pds), "%s: advancing rx to %u\n",
		   __func__, pdc->rx_base_psn);
}

int uet_pdc_rx_req(struct uet_pdc *pdc, struct sk_buff *skb,
		   __be32 remote_fep_addr, __u8 tos)
{
	struct uet_ses_req_hdr *ses_req = pds_req_ses_req_hdr(skb);
	struct uet_pds_req_hdr *req = pds_req_hdr(skb);
	u8 req_flags = uet_prologue_flags(&req->prologue), ack_flags = 0;
	u32 req_psn = be32_to_cpu(req->psn);
	const char *drop_reason = "tx_busy";
	unsigned long psn_bit;
	enum mpr_pos psn_pos;
	int ret = -EINVAL;

	spin_lock(&pdc->lock);
	netdev_dbg(pds_netdev(pdc->pds), "%s: tx_busy: %u pdc: [ tx_base_psn: %u"
				  " state: %u dpdcid: %u spdcid: %u ]\n"
				  "req: [ psn: %u spdcid: %u dpdcid: %u prologue flags: 0x%x ]\n"
				  "ses_req: [ opcode: %u msg id: %u job id: %u "
				  "pid_on_fep: %u flags: 0x%x ]\n",
		   __func__, pdc->tx_busy, pdc->tx_base_psn,
		   pdc->state, pdc->dpdcid, pdc->spdcid,
		   req_psn, be16_to_cpu(req->spdcid), be16_to_cpu(req->dpdcid),
		   uet_prologue_flags(&req->prologue),
		   uet_ses_req_opcode(ses_req), be16_to_cpu(ses_req->msg_id),
		   uet_ses_req_job_id(ses_req), uet_ses_req_pid_on_fep(ses_req),
		   uet_ses_req_flags(ses_req));

	if (unlikely(pdc->tx_busy))
		goto err_dbg;

	if (req_flags & UET_PDS_REQ_FLAG_RETX)
		ack_flags |= UET_PDS_ACK_FLAG_RETX;
	if (INET_ECN_is_ce(tos))
		ack_flags |= UET_PDS_ACK_FLAG_M;
	psn_pos = psn_mpr_pos(pdc->rx_base_psn, req_psn);
	switch (psn_pos) {
	case UET_PDC_MPR_FUTURE:
		drop_reason = "req psn is in a future MPR window";
		goto err_dbg;
	case UET_PDC_MPR_PREV:
		if ((int)(req_psn - pdc->rx_base_psn) < S16_MIN) {
			drop_reason = "req psn is too far in the past";
			goto err_dbg;
		}
		uet_pdc_send_ses_ack(pdc, UET_SES_RSP_RC_NULL, ses_req->msg_id,
				     req_psn, ack_flags, true);
		netdev_dbg(pds_netdev(pdc->pds), "%s: received a request in previous MPR window (psn %u)\n"
					  "pdc: [ rx_base_psn: %u state: %u"
					  " dpdcid: %u spdcid: %u ]\n",
			   __func__, req_psn, pdc->rx_base_psn,
			   pdc->state, pdc->dpdcid, pdc->spdcid);
		goto out;
	case UET_PDC_MPR_CUR:
		break;
	}

	psn_bit = req_psn - pdc->rx_base_psn;
	if (!psn_bit_valid(psn_bit)) {
		drop_reason = "req psn bit is invalid";
		goto err_dbg;
	}
	if (test_and_set_bit(psn_bit, pdc->rx_bitmap)) {
		drop_reason = "req psn bit is already set in rx_bitmap";
		goto err_dbg;
	}

	ret = 0;
	switch (pdc->state) {
	case UET_PDC_EP_STATE_SYN_SENT:
		/* error */
		break;
	case UET_PDC_EP_STATE_ESTABLISHED:
		/* Rx request and do an upcall, potentially return an ack */
		ret = uet_job_fep_queue_skb(pds_context(pdc->pds),
					    uet_ses_req_job_id(ses_req), skb,
					    remote_fep_addr);
		/* TODO: handle errors in sending the error */
		/* TODO: more specific RC codes */
		break;
	case UET_PDC_EP_STATE_ACK_WAIT:
		break;
	case UET_PDC_EP_STATE_CLOSE_ACK_WAIT:
		break;
	}

	if (ret >= 0)
		uet_pdc_send_ses_ack(pdc, UET_SES_RSP_RC_NULL, ses_req->msg_id,
				     req_psn, ack_flags, false);
	/* TODO: NAK */

	if (!psn_bit)
		uet_pdc_mpr_advance_rx(pdc);

out:
	spin_unlock(&pdc->lock);

	return ret;
err_dbg:
	netdev_dbg(pds_netdev(pdc->pds), "%s: drop reason: [ %s ]\n"
				  "pdc: [ rx_base_psn: %u state: %u"
				  " dpdcid: %u spdcid: %u ]\n"
				  "ses_req: [ msg id: %u ack_psn: %u spdcid: %u"
				  " dpdcid: %u ]\n",
		  __func__, drop_reason, pdc->rx_base_psn,
		  pdc->state, pdc->dpdcid, pdc->spdcid,
		  be16_to_cpu(ses_req->msg_id), be32_to_cpu(req->psn),
		  be16_to_cpu(req->spdcid), be16_to_cpu(req->dpdcid));
	goto out;
}
