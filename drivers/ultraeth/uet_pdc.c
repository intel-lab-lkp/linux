// SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause)

#include <linux/kernel.h>
#include <linux/slab.h>

#include <net/ultraeth/uet_context.h>
#include <net/ultraeth/uet_pdc.h>

struct metadata_dst *uet_pdc_dst(const struct uet_pdc_key *key, __be16 dport,
				 u8 tos)
{
	IP_TUNNEL_DECLARE_FLAGS(md_flags) = { };
	struct metadata_dst *mdst;

	mdst = __ip_tun_set_dst(key->src_ip, key->dst_ip, tos, 0, dport,
				md_flags, 0, 0);
	if (!mdst)
		return NULL;
	mdst->u.tun_info.mode |= IP_TUNNEL_INFO_TX;

	return mdst;
}

void uet_pdc_rx_refresh(struct uet_pdc *pdc)
{
	unsigned long rx_jiffies = jiffies;

	if (rx_jiffies != READ_ONCE(pdc->rx_last_jiffies))
		WRITE_ONCE(pdc->rx_last_jiffies, rx_jiffies);
}

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

static void __uet_pdc_mpr_advance_tx(struct uet_pdc *pdc, u32 bits)
{
	if (WARN_ON_ONCE(bits >= UET_PDC_MPR))
		return;

	bitmap_shift_right(pdc->tx_bitmap, pdc->tx_bitmap, bits, UET_PDC_MPR);
	bitmap_shift_right(pdc->ack_bitmap, pdc->ack_bitmap, bits, UET_PDC_MPR);
	pdc->tx_base_psn += bits;
	netdev_dbg(pds_netdev(pdc->pds), "%s: advancing tx to %u\n", __func__,
		   pdc->tx_base_psn);
}

static void uet_pdc_mpr_advance_tx(struct uet_pdc *pdc, u32 cack_psn)
{
	/* cumulative ack, clear all prior and including cack_psn */
	if (cack_psn > pdc->tx_base_psn)
		__uet_pdc_mpr_advance_tx(pdc, cack_psn - pdc->tx_base_psn);
	else if (test_bit(0, pdc->tx_bitmap) && test_bit(0, pdc->ack_bitmap))
		__uet_pdc_mpr_advance_tx(pdc, 1);
}

static void uet_pdc_rtx_skb(struct uet_pdc *pdc, struct sk_buff *skb, ktime_t ts)
{
	struct sk_buff *nskb = skb_clone(skb, GFP_ATOMIC);
	struct uet_prologue_hdr *prologue;

	if (!nskb)
		return;

	prologue = (struct uet_prologue_hdr *)nskb->data;
	if (!(uet_prologue_flags(prologue) & UET_PDS_REQ_FLAG_RETX))
		uet_pdc_build_prologue(prologue,
				       uet_prologue_ctl_type(prologue),
				       uet_prologue_next_hdr(prologue),
				       uet_prologue_flags(prologue) |
				       UET_PDS_REQ_FLAG_RETX);

	uet_pdc_xmit(pdc, nskb);
	skb->tstamp = ts;
	UET_SKB_CB(skb)->rtx_attempts++;
}

static void uet_pdc_rtx_timer_expired(struct timer_list *t)
{
	u64 smallest_diff = UET_PDC_RTX_DEFAULT_TIMEOUT_NSEC;
	struct uet_pdc *pdc = from_timer(pdc, t, rtx_timer);
	ktime_t now = ktime_get_real_ns();
	struct sk_buff *skb, *skb_tmp;

	spin_lock(&pdc->lock);
	skb = skb_rb_first(&pdc->rtx_queue);
	skb_rbtree_walk_from_safe(skb, skb_tmp) {
		ktime_t expire = ktime_add(skb->tstamp,
					   UET_PDC_RTX_DEFAULT_TIMEOUT_NSEC);

		if (ktime_before(now, expire)) {
			u64 diff = ktime_to_ns(ktime_sub(expire, now));

			if (diff < smallest_diff)
				smallest_diff = diff;
			continue;
		}
		if (UET_SKB_CB(skb)->rtx_attempts == UET_PDC_RTX_DEFAULT_MAX) {
			struct uet_prologue_hdr *prologue;

			/* XXX: close connection, count drops etc */
			prologue = (struct uet_prologue_hdr *)skb->data;
			netdev_dbg(pds_netdev(pdc->pds), "%s: psn: %u type: %u too many rtx attempts: %u\n",
				   __func__, UET_SKB_CB(skb)->psn,
				   uet_prologue_type(prologue),
				   UET_SKB_CB(skb)->rtx_attempts);
			if (uet_prologue_type(prologue) == UET_PDS_TYPE_CTRL_MSG &&
			    uet_prologue_ctl_type(prologue) == UET_CTL_TYPE_CLOSE) {
				uet_pdc_destroy(pdc);
				goto out_unlock;
			}
			/* if dropping the oldest packet move window */
			if (UET_SKB_CB(skb)->psn == pdc->tx_base_psn)
				uet_pdc_mpr_advance_tx(pdc, 1);
			rb_erase(&skb->rbnode, &pdc->rtx_queue);
			consume_skb(skb);
			continue;
		}

		uet_pdc_rtx_skb(pdc, skb, now);
	}

	mod_timer(&pdc->rtx_timer, jiffies +
				   nsecs_to_jiffies(smallest_diff));
out_unlock:
	spin_unlock(&pdc->lock);
}

static void uet_pdc_rbtree_insert(struct rb_root *root, struct sk_buff *skb)
{
	struct rb_node **p = &root->rb_node;
	struct rb_node *parent = NULL;
	struct sk_buff *skb1;

	while (*p) {
		parent = *p;
		skb1 = rb_to_skb(parent);
		if (before(UET_SKB_CB(skb)->psn, UET_SKB_CB(skb1)->psn))
			p = &parent->rb_left;
		else
			p = &parent->rb_right;
	}

	rb_link_node(&skb->rbnode, parent, p);
	rb_insert_color(&skb->rbnode, root);
}

static struct sk_buff *uet_pdc_rtx_find(struct uet_pdc *pdc, u32 psn)
{
	struct rb_node *parent, **p = &pdc->rtx_queue.rb_node;

	while (*p) {
		struct sk_buff *skb;

		parent = *p;
		skb = rb_to_skb(parent);
		if (psn == UET_SKB_CB(skb)->psn)
			return skb;

		if (before(psn, UET_SKB_CB(skb)->psn))
			p = &parent->rb_left;
		else
			p = &parent->rb_right;
	}

	return NULL;
}

static void uet_pdc_rtx_remove_skb(struct uet_pdc *pdc, struct sk_buff *skb)
{
	rb_erase(&skb->rbnode, &pdc->rtx_queue);
	consume_skb(skb);
}

static void uet_pdc_ack_psn(struct uet_pdc *pdc, struct sk_buff *ack_skb,
			    u32 psn, bool ecn_marked)
{
	struct sk_buff *skb = skb_rb_first(&pdc->rtx_queue);
	u32 first_psn = skb ? UET_SKB_CB(skb)->psn : 0;

	/* if the oldest PSN got ACKed and it hasn't been retransmitted
	 * we can move the timer to the next one
	 */
	if (skb && psn == first_psn) {
		struct sk_buff *next = skb_rb_next(skb);

		/* move timer only if first PSN wasn't retransmitted */
		if (next && !UET_SKB_CB(skb)->rtx_attempts) {
			ktime_t expire = ktime_add(next->tstamp,
						   UET_PDC_RTX_DEFAULT_TIMEOUT_NSEC);
			ktime_t now = ktime_get_ns();

			if (ktime_before(expire, now)) {
				u64 diff = ktime_to_ns(ktime_sub(expire, now));
				unsigned long diffj = nsecs_to_jiffies(diff);

				mod_timer(&pdc->rtx_timer, jiffies + diffj);
			}
		}
	} else {
		skb = uet_pdc_rtx_find(pdc, psn);
	}

	if (!skb)
		return;

	uet_pdc_rtx_remove_skb(pdc, skb);
}

static void uet_pdc_rtx_purge(struct uet_pdc *pdc)
{
	struct rb_node *p = rb_first(&pdc->rtx_queue);

	while (p) {
		struct sk_buff *skb = rb_to_skb(p);

		p = rb_next(p);
		uet_pdc_rtx_remove_skb(pdc, skb);
	}
}

static int uet_pdc_rtx_queue(struct uet_pdc *pdc, struct sk_buff *skb, u32 psn)
{
	struct sk_buff *rtx_skb = skb_clone(skb, GFP_ATOMIC);

	if (unlikely(!rtx_skb))
		return -ENOMEM;

	UET_SKB_CB(rtx_skb)->psn = psn;
	UET_SKB_CB(rtx_skb)->rtx_attempts = 0;
	uet_pdc_rbtree_insert(&pdc->rtx_queue, rtx_skb);

	if (!timer_pending(&pdc->rtx_timer))
		mod_timer(&pdc->rtx_timer, jiffies +
					   UET_PDC_RTX_DEFAULT_TIMEOUT_JIFFIES);

	return 0;
}

static s64 uet_pdc_get_psn(struct uet_pdc *pdc)
{
	unsigned long fzb = find_first_zero_bit(pdc->tx_bitmap, UET_PDC_MPR);

	if (unlikely(fzb == UET_PDC_MPR))
		return -1;

	set_bit(fzb, pdc->tx_bitmap);

	return pdc->tx_base_psn + fzb;
}

static void uet_pdc_put_psn(struct uet_pdc *pdc, u32 psn)
{
	unsigned long psn_bit = psn - pdc->tx_base_psn;

	clear_bit(psn_bit, pdc->tx_bitmap);
}

static int uet_pdc_tx_ctl(struct uet_pdc *pdc, u8 ctl_type, u8 flags,
			  __be32 psn, __be32 payload)
{
	struct uet_pds_ctl_hdr *ctl;
	struct sk_buff *skb;
	int ret;

	/* both CLOSE types need to be retransmitted and need a new PSN */
	switch (ctl_type) {
	case UET_CTL_TYPE_CLOSE:
	case UET_CTL_TYPE_REQ_CLOSE:
		/* payload & psn must be 0 */
		if (payload || psn)
			return -EINVAL;
		/* AR must be set */
		flags |= UET_PDS_CTL_FLAG_AR;
		break;
	default:
		WARN_ON(1);
		return -EINVAL;
	}

	skb = alloc_skb(sizeof(struct uet_pds_ctl_hdr), GFP_ATOMIC);
	if (!skb)
		return -ENOBUFS;
	ctl = skb_put(skb, sizeof(*ctl));
	uet_pdc_build_prologue(&ctl->prologue, UET_PDS_TYPE_CTRL_MSG,
			       ctl_type, flags);
	if (!psn) {
		s64 psn_new = uet_pdc_get_psn(pdc);

		if (psn_new == -1) {
			kfree_skb(skb);
			return -ENOSPC;
		}
		psn = cpu_to_be32(psn_new);
	}
	ctl->psn = psn;
	ctl->spdcid = cpu_to_be16(pdc->spdcid);
	ctl->dpdcid_pdc_info_offset = cpu_to_be16(pdc->dpdcid);
	ctl->payload = payload;

	ret = uet_pdc_rtx_queue(pdc, skb, be32_to_cpu(psn));
	if (ret) {
		uet_pdc_put_psn(pdc, be32_to_cpu(psn));
		kfree_skb(skb);
		return ret;
	}
	uet_pdc_xmit(pdc, skb);

	return 0;
}

static void uet_pdc_close(struct uet_pdc *pdc)
{
	u8 state;
	int ret;

	/* we have already transmitted the close control packet */
	if (pdc->state > UET_PDC_EP_STATE_ACK_WAIT)
		return;

	if (!RB_EMPTY_ROOT(&pdc->rtx_queue)) {
		if (pdc->state == UET_PDC_EP_STATE_ACK_WAIT)
			return;
		state = UET_PDC_EP_STATE_ACK_WAIT;
	} else {
		u8 ctl_type, ctl_flags = 0;

		if (pdc->is_initiator) {
			ctl_type = UET_CTL_TYPE_CLOSE;
			state = UET_PDC_EP_STATE_CLOSE_ACK_WAIT;
			ctl_flags = UET_PDS_CTL_FLAG_AR;
		} else {
			ctl_type = UET_CTL_TYPE_REQ_CLOSE;
			state = UET_PDC_EP_STATE_CLOSE_WAIT;
		}
		ret = uet_pdc_tx_ctl(pdc, ctl_type, ctl_flags, 0, 0);
		if (ret)
			return;
	}

	pdc->state = state;
}

static void uet_pdc_timeout_timer_expired(struct timer_list *t)
{
	struct uet_pdc *pdc = from_timer(pdc, t, timeout_timer);
	unsigned long now = jiffies, last_rx;
	bool rearm_timer = true;

	last_rx = READ_ONCE(pdc->rx_last_jiffies);
	if (time_after_eq(last_rx, now) ||
	    time_after_eq(last_rx + UET_PDC_IDLE_TIMEOUT_JIFFIES, now))
		goto rearm_timeout;
	spin_lock(&pdc->lock);
	switch (pdc->state) {
	case UET_PDC_EP_STATE_ACK_WAIT:
		uet_pdc_close(pdc);
		fallthrough;
	case UET_PDC_EP_STATE_CLOSE_WAIT:
	case UET_PDC_EP_STATE_CLOSE_ACK_WAIT:
		/* we waited too long for the last acks */
		if (time_before_eq(last_rx + (UET_PDC_IDLE_TIMEOUT_JIFFIES * 2),
				   now)) {
			if (!pdc->is_initiator)
				uet_pds_send_nack(pdc->pds, &pdc->key,
						  pdc->metadata->u.tun_info.key.tp_dst,
						  0,
						  cpu_to_be16(pdc->spdcid),
						  cpu_to_be16(pdc->dpdcid),
						  UET_PDS_NACK_CLOSING_IN_ERR,
						  cpu_to_be32(pdc->rx_base_psn + 1),
						  0);
			uet_pdc_destroy(pdc);
			rearm_timer = false;
		}
		break;
	default:
		uet_pdc_close(pdc);
		break;
	}
	spin_unlock(&pdc->lock);
rearm_timeout:
	if (rearm_timer)
		mod_timer(&pdc->timeout_timer,
			  now + UET_PDC_IDLE_TIMEOUT_JIFFIES);
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
			       u8 tos, __be16 dport, u32 ack_gen_trigger,
			       u32 ack_gen_min_pkt_add,
			       const struct uet_pdc_key *key, bool is_inbound)
{
	struct uet_pdc *pdc, *pdc_ins = ERR_PTR(-ENOMEM);
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
	pdc->ack_gen_trigger = ack_gen_trigger;
	pdc->ack_gen_min_pkt_add = ack_gen_min_pkt_add;
	pdc->rtx_queue = RB_ROOT;
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
	pdc->tx_bitmap = bitmap_zalloc(UET_PDC_MPR, GFP_ATOMIC);
	if (!pdc->tx_bitmap)
		goto err_tx_bitmap;
	pdc->ack_bitmap = bitmap_zalloc(UET_PDC_MPR, GFP_ATOMIC);
	if (!pdc->ack_bitmap)
		goto err_ack_bitmap;
	timer_setup(&pdc->rtx_timer, uet_pdc_rtx_timer_expired, 0);
	timer_setup(&pdc->timeout_timer, uet_pdc_timeout_timer_expired, 0);
	pdc->metadata = uet_pdc_dst(key, dport, tos);
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
	mod_timer(&pdc->timeout_timer,
		  jiffies + UET_PDC_IDLE_TIMEOUT_JIFFIES);

	return pdc_ins;

err_ep_insert:
	dst_release(&pdc->metadata->dst);
err_tun_dst:
	bitmap_free(pdc->ack_bitmap);
err_ack_bitmap:
	bitmap_free(pdc->tx_bitmap);
err_tx_bitmap:
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
	timer_delete_sync(&pdc->timeout_timer);
	timer_delete_sync(&pdc->rtx_timer);
	uet_pdc_rtx_purge(pdc);
	dst_release(&pdc->metadata->dst);
	bitmap_free(pdc->ack_bitmap);
	bitmap_free(pdc->tx_bitmap);
	bitmap_free(pdc->rx_bitmap);
	kfree(pdc);
}

void uet_pdc_destroy(struct uet_pdc *pdc)
{
	uet_pds_pdcep_remove(pdc);
	uet_pds_pdcid_remove(pdc);
	uet_pds_pdc_gc_queue(pdc);
}

static int uet_pdc_build_req(struct uet_pdc *pdc,
			     struct sk_buff *skb, u8 type, u8 flags)
{
	struct uet_pds_req_hdr *req;
	s64 psn;

	req = skb_push(skb, sizeof(*req));
	uet_pdc_build_prologue(&req->prologue, type,
			       UET_PDS_NEXT_HDR_REQ_STD, flags);
	switch (pdc->state) {
	case UET_PDC_EP_STATE_CLOSED:
		pdc->psn_start = get_random_u32();
		pdc->tx_base_psn = pdc->psn_start;
		pdc->rx_base_psn = pdc->psn_start;
		break;
	}

	psn = uet_pdc_get_psn(pdc);
	if (unlikely(psn == -1))
		return -ENOSPC;
	UET_SKB_CB(skb)->psn = psn;
	req->psn = cpu_to_be32(psn);
	req->spdcid = cpu_to_be16(pdc->spdcid);
	req->dpdcid = cpu_to_be16(pdc->dpdcid);

	return 0;
}

static void pdc_build_sack(struct uet_pdc *pdc,
			   struct uet_pds_ack_ext_hdr *ack_ext)
{
	u32 sack_base = pdc->lowest_unack_psn, shift;
	unsigned long bit, start_bit;
	s16 sack_psn_offset;
	u64 sack_bitmap;

	if (sack_base + UET_PDC_SACK_BITS > pdc->max_rcv_psn)
		sack_base = max(pdc->max_rcv_psn - UET_PDC_SACK_BITS,
				pdc->rx_base_psn);
	sack_base &= UET_PDC_SACK_MASK;
	sack_psn_offset = (s16)(sack_base -
				(pdc->rx_base_psn & UET_PDC_SACK_MASK));
	if (sack_base == pdc->rx_base_psn) {
		shift = 1;
		sack_bitmap = 1;
		bit = 0;
	} else if (sack_base < pdc->rx_base_psn) {
		shift = pdc->rx_base_psn - sack_base;
		sack_bitmap = U64_MAX >> (64 - shift);
		bit = 0;
	} else {
		shift = 0;
		sack_bitmap = 0;
		bit = sack_base - pdc->rx_base_psn;
	}

	start_bit = bit;
	for_each_set_bit_from(bit, pdc->rx_bitmap, UET_PDC_MPR) {
		shift += (bit - start_bit);
		if (shift >= UET_PDC_SACK_BITS)
			break;
		sack_bitmap |= BIT(shift);
	}

	pdc->lowest_unack_psn += UET_PDC_SACK_BITS;
	ack_ext->sack_psn_offset = cpu_to_be16(sack_psn_offset);
	ack_ext->sack_bitmap = cpu_to_be64(sack_bitmap);
}

static void pdc_build_ack(struct uet_pdc *pdc, struct sk_buff *skb, u32 psn,
			  u8 ack_flags, bool exact_psn)
{
	u8 type = pdc_should_sack(pdc) ? UET_PDS_TYPE_ACK_CC : UET_PDS_TYPE_ACK;
	struct uet_pds_ack_hdr *ack = skb_put(skb, sizeof(*ack));

	uet_pdc_build_prologue(&ack->prologue, type, UET_PDS_NEXT_HDR_RSP,
			       ack_flags);
	if (exact_psn) {
		ack->ack_psn_offset = 0;
		ack->cack_psn = cpu_to_be32(psn);
	} else {
		ack->ack_psn_offset = cpu_to_be16(psn - pdc->rx_base_psn);
		ack->cack_psn = cpu_to_be32(pdc->rx_base_psn);
	}
	ack->spdcid = cpu_to_be16(pdc->spdcid);
	ack->dpdcid = cpu_to_be16(pdc->dpdcid);

	if (pdc_should_sack(pdc)) {
		struct uet_pds_ack_ext_hdr *ack_ext = skb_put(skb,
							      sizeof(*ack_ext));

		pdc_build_sack(pdc, ack_ext);
	}
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
	unsigned int skb_size = sizeof(struct uet_ses_rsp_hdr) +
				sizeof(struct uet_pds_ack_hdr);
	struct sk_buff *skb;

	skb_size += pdc_should_sack(pdc) ? sizeof(struct uet_pds_ack_ext_hdr) : 0;
	skb = alloc_skb(skb_size, GFP_ATOMIC);
	if (!skb)
		return -ENOBUFS;

	uet_pdc_build_ses_ack(pdc, skb, ses_rc, msg_id, psn, ack_flags,
			      exact_psn);
	uet_pdc_xmit(pdc, skb);

	return 0;
}

int uet_pdc_tx_req(struct uet_pdc *pdc, struct sk_buff *skb, u8 type)
{
	struct uet_pds_req_hdr *req;
	int ret = 0;

	spin_lock_bh(&pdc->lock);
	if (pdc->tx_busy) {
		ret = -EBUSY;
		goto out_unlock;
	}

	switch (pdc->state) {
	case UET_PDC_EP_STATE_CLOSED:
		ret = uet_pdc_build_req(pdc, skb, type, UET_PDS_REQ_FLAG_SYN);
		if (ret)
			goto out_unlock;
		req = (struct uet_pds_req_hdr *)skb->data;
		ret = uet_pdc_rtx_queue(pdc, skb, be32_to_cpu(req->psn));
		if (ret) {
			uet_pdc_put_psn(pdc, be32_to_cpu(req->psn));
			goto out_unlock;
		}
		pdc->state = UET_PDC_EP_STATE_SYN_SENT;
		pdc->tx_busy = true;
		break;
	case UET_PDC_EP_STATE_SYN_SENT:
		break;
	case UET_PDC_EP_STATE_ESTABLISHED:
		ret = uet_pdc_build_req(pdc, skb, type, 0);
		if (ret)
			goto out_unlock;
		req = (struct uet_pds_req_hdr *)skb->data;
		ret = uet_pdc_rtx_queue(pdc, skb, be32_to_cpu(req->psn));
		if (ret) {
			uet_pdc_put_psn(pdc, be32_to_cpu(req->psn));
			goto out_unlock;
		}
		break;
	case UET_PDC_EP_STATE_QUIESCE:
		break;
	case UET_PDC_EP_STATE_ACK_WAIT:
		break;
	case UET_PDC_EP_STATE_CLOSE_ACK_WAIT:
		break;
	default:
		WARN_ON(1);
	}

out_unlock:
	netdev_dbg(pds_netdev(pdc->pds), "%s: tx_busy: %u pdc: [ tx_base_psn: %u"
				  " state: %u dpdcid: %u spdcid: %u ] proto 0x%x\n",
		   __func__, pdc->tx_busy, pdc->tx_base_psn, pdc->state,
		   pdc->dpdcid, pdc->spdcid, ntohs(skb->protocol));
	spin_unlock_bh(&pdc->lock);

	if (!ret)
		uet_pdc_xmit(pdc, skb);

	return ret;
}

static void uet_pdc_rx_sack(struct uet_pdc *pdc, struct sk_buff *skb,
			    u32 cack_psn, struct uet_pds_ack_ext_hdr *ext_ack,
			    bool ecn_marked)
{
	unsigned long bit, *sack_bitmap = (unsigned long *)&ext_ack->sack_bitmap;
	u32 sack_base_psn = cack_psn +
			    (s16)be16_to_cpu(ext_ack->sack_psn_offset);

	while ((bit = find_next_bit(sack_bitmap, 64, 0)) != 64) {
		/* skip bits that were already acked */
		if (sack_base_psn + bit <= pdc->tx_base_psn) {
			if (sack_base_psn + bit == pdc->tx_base_psn)
				__uet_pdc_mpr_advance_tx(pdc, 1);
			continue;
		}
		if (!psn_bit_valid((sack_base_psn + bit) - pdc->tx_base_psn))
			break;
		if (test_and_set_bit((sack_base_psn + bit) - pdc->tx_base_psn,
				     pdc->ack_bitmap))
			continue;
		uet_pdc_ack_psn(pdc, skb, sack_base_psn + bit, ecn_marked);
	}
}

int uet_pdc_rx_ack(struct uet_pdc *pdc, struct sk_buff *skb,
		   __be32 remote_fep_addr)
{
	struct uet_ses_rsp_hdr *ses_rsp = pds_ack_ses_rsp_hdr(skb);
	struct uet_pds_ack_hdr *ack = pds_ack_hdr(skb);
	s16 ack_psn_offset = be16_to_cpu(ack->ack_psn_offset);
	const char *drop_reason = "ack_psn not in MPR window";
	struct uet_pds_ack_ext_hdr *ext_ack = NULL;
	u32 cack_psn = be32_to_cpu(ack->cack_psn);
	u32 ack_psn = cack_psn + ack_psn_offset;
	bool is_sack = false, ecn_marked;
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
	if (!psn_bit_valid(psn_bit) || !test_bit(psn_bit, pdc->tx_bitmap)) {
		drop_reason = "ack_psn bit is invalid";
		goto err_dbg;
	}
	if (uet_prologue_type(&ack->prologue) == UET_PDS_TYPE_ACK_CC) {
		ext_ack = pds_ack_ext_hdr(skb);
		is_sack = !!ext_ack->sack_bitmap;
	}
	if (test_and_set_bit(psn_bit, pdc->ack_bitmap)) {
		/* SACK packets can include already acked packets */
		if (!is_sack) {
			drop_reason = "ack_psn bit already set in ack_bitmap";
			goto err_dbg;
		}
	}

	/* either using ROD mode or in SYN_SENT state */
	if (pdc->tx_busy)
		pdc->tx_busy = false;
	ecn_marked = !!(uet_prologue_flags(&ack->prologue) & UET_PDS_ACK_FLAG_M);
	/* we can advance only if the oldest pkt got acked or we got
	 * a cumulative ack clearing >= 1 older packets
	 */
	if (!psn_bit || cack_psn > pdc->tx_base_psn) {
		if (cack_psn >= pdc->tx_base_psn) {
			u32 i;

			for (i = 0; i <= cack_psn - pdc->tx_base_psn; i++)
				uet_pdc_ack_psn(pdc, skb, cack_psn - i,
						ecn_marked);
		}

		uet_pdc_mpr_advance_tx(pdc, cack_psn);
	}

	/* minor optimization, this can happen only if they are != */
	if (cack_psn != ack_psn)
		uet_pdc_ack_psn(pdc, skb, ack_psn, ecn_marked);

	if (is_sack)
		uet_pdc_rx_sack(pdc, skb, cack_psn, ext_ack, ecn_marked);

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
		ret = uet_job_fep_queue_skb(pds_context(pdc->pds),
					    uet_ses_rsp_job_id(ses_rsp), skb,
					    remote_fep_addr);
		if (!RB_EMPTY_ROOT(&pdc->rtx_queue) || ret < 0)
			break;
		uet_pdc_close(pdc);
		ret = 1;
		break;
	case UET_PDC_EP_STATE_CLOSE_ACK_WAIT:
		uet_pdc_destroy(pdc);
		ret = 0;
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
	unsigned long fzb = find_first_zero_bit(pdc->rx_bitmap, UET_PDC_MPR);
	u32 old_psn = pdc->rx_base_psn;

	if (fzb == 0)
		return;

	bitmap_shift_right(pdc->rx_bitmap, pdc->rx_bitmap, fzb, UET_PDC_MPR);
	pdc->rx_base_psn += fzb;
	netdev_dbg(pds_netdev(pdc->pds), "%s: advancing rx from %u to %u (%lu)\n",
		   __func__, old_psn, pdc->rx_base_psn, fzb);
}

static void uet_pdc_rx_req_handle_ack(struct uet_pdc *pdc, unsigned int len,
				      __be16 msg_id, u8 req_flags, u32 req_psn,
				      u8 ack_flags, bool first_ack)
{
	pdc->ack_gen_count += max(pdc->ack_gen_min_pkt_add, len);
	if (first_ack ||
	    (req_flags & (UET_PDS_REQ_FLAG_AR | UET_PDS_REQ_FLAG_RETX)) ||
	    pdc->ack_gen_count >= pdc->ack_gen_trigger) {
		/* first advance so if the current psn == rx_base_psn
		 * we will clear it with the cumulative ack
		 */
		uet_pdc_mpr_advance_rx(pdc);
		pdc->ack_gen_count = 0;
		/* req_psn is inside the cumulative ack range, so
		 * it is covered by it
		 */
		if (unlikely(req_psn < pdc->rx_base_psn))
			req_psn = pdc->rx_base_psn;
		uet_pdc_send_ses_ack(pdc, UET_SES_RSP_RC_NULL, msg_id, req_psn,
				     ack_flags, false);
	}
}

static bool uet_pdc_req_validate_mode(const struct uet_pdc *pdc,
				      const struct uet_pds_req_hdr *req)
{
	switch (uet_prologue_type(&req->prologue)) {
	case UET_PDS_TYPE_RUD_REQ:
		return pdc->mode == UET_PDC_MODE_RUD;
	case UET_PDS_TYPE_ROD_REQ:
		return pdc->mode == UET_PDC_MODE_ROD;
	}

	return false;
}

int uet_pdc_rx_req(struct uet_pdc *pdc, struct sk_buff *skb,
		   __be32 remote_fep_addr, __u8 tos)
{
	struct uet_ses_req_hdr *ses_req = pds_req_ses_req_hdr(skb);
	struct uet_pds_req_hdr *req = pds_req_hdr(skb);
	u8 req_flags = uet_prologue_flags(&req->prologue), ack_flags = 0;
	u32 req_psn = be32_to_cpu(req->psn);
	const char *drop_reason = "tx_busy";
	__be16 msg_id = ses_req->msg_id;
	unsigned int len = skb->len;
	bool first_ack = false;
	enum mpr_pos psn_pos;
	__u8 nack_code = 0;
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
	if (!uet_pdc_req_validate_mode(pdc, req)) {
		drop_reason = "pdc mode doesn't match request";
		nack_code = UET_PDS_NACK_PDC_MODE_MISMATCH;
		goto err_dbg;
	}

	if (req_flags & UET_PDS_REQ_FLAG_RETX)
		ack_flags |= UET_PDS_ACK_FLAG_RETX;
	if (INET_ECN_is_ce(tos))
		ack_flags |= UET_PDS_ACK_FLAG_M;
	psn_pos = psn_mpr_pos(pdc->rx_base_psn, req_psn);
	switch (psn_pos) {
	case UET_PDC_MPR_FUTURE:
		drop_reason = "req psn is in a future MPR window";
		if (req_flags & UET_PDS_REQ_FLAG_SYN)
			nack_code = UET_PDS_NACK_INVALID_SYN;
		else
			nack_code = UET_PDS_NACK_PSN_OOR_WINDOW;
		goto err_dbg;
	case UET_PDC_MPR_PREV:
		if ((int)(req_psn - pdc->rx_base_psn) < S16_MIN) {
			drop_reason = "req psn is too far in the past";
			nack_code = UET_PDS_NACK_PSN_OOR_WINDOW;
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

	switch (pdc->state) {
	case UET_PDC_EP_STATE_SYN_SENT:
		/* error */
		break;
	case UET_PDC_EP_STATE_NEW_ESTABLISHED:
		/* special state when a connection is new, we need to
		 * send first ack immediately
		 */
		pdc->state = UET_PDC_EP_STATE_ESTABLISHED;
		first_ack = true;
		fallthrough;
	case UET_PDC_EP_STATE_ESTABLISHED:
		if (!first_ack) {
			unsigned long psn_bit = req_psn - pdc->rx_base_psn - 1;

			if (!psn_bit_valid(psn_bit)) {
				drop_reason = "req psn bit is invalid";
				nack_code = UET_PDS_NACK_PSN_OOR_WINDOW;
				goto err_dbg;
			}
			if (test_and_set_bit(psn_bit, pdc->rx_bitmap)) {
				drop_reason = "req psn bit is already set in rx_bitmap";
				goto err_dbg;
			}
		}

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
		uet_pdc_rx_req_handle_ack(pdc, len, msg_id, req_flags,
					  req_psn, ack_flags, first_ack);

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

	if (nack_code)
		uet_pds_send_nack(pdc->pds, &pdc->key,
				  pdc->metadata->u.tun_info.key.tp_dst, 0,
				  cpu_to_be16(pdc->spdcid),
				  cpu_to_be16(pdc->dpdcid),
				  nack_code, req->psn,
				  pds_req_to_nack_flags(req_flags));
	goto out;
}

void uet_pdc_rx_nack(struct uet_pdc *pdc, struct sk_buff *skb)
{
	struct uet_pds_nack_hdr *nack = pds_nack_hdr(skb);
	u32 nack_psn = be32_to_cpu(nack->nack_psn_pkt_id);

	spin_lock(&pdc->lock);
	netdev_dbg(pds_netdev(pdc->pds), "%s: NACK pdc: [ spdcid: %u dpdcid: %u rx_base_psn %u ] "
					 "nack header: [ nack_code: %u vendor_code: %u nack_psn: %u ]\n",
		   __func__, pdc->spdcid, pdc->dpdcid, pdc->rx_base_psn,
		   nack->nack_code, nack->vendor_code, nack_psn);
	if (psn_mpr_pos(pdc->rx_base_psn, nack_psn) != UET_PDC_MPR_CUR)
		goto out;
	switch (nack->nack_code) {
	/* PDC_FATAL codes */
	case UET_PDS_NACK_CLOSING_IN_ERR:
	case UET_PDS_NACK_INV_DPDCID:
	case UET_PDS_NACK_NO_RESOURCE:
	case UET_PDS_NACK_PDC_HDR_MISMATCH:
	case UET_PDS_NACK_INVALID_SYN:
	case UET_PDS_NACK_PDC_MODE_MISMATCH:
		uet_pdc_destroy(pdc);
		break;
	}
out:
	spin_unlock(&pdc->lock);
}

int uet_pdc_rx_ctl(struct uet_pdc *pdc, struct sk_buff *skb,
		   __be32 remote_fep_addr)
{
	struct uet_pds_ctl_hdr *ctl = pds_ctl_hdr(skb);
	u32 ctl_psn = be32_to_cpu(ctl->psn);
	int ret = -EINVAL;

	spin_lock(&pdc->lock);
	netdev_dbg(pds_netdev(pdc->pds), "%s: CTRL pdc: [ spdcid: %u dpdcid: %u rx_base_psn %u ] "
					 "ctrl header: [ ctl_type: %u psn: %u ]\n",
		   __func__, pdc->spdcid, pdc->dpdcid, pdc->rx_base_psn,
		   uet_prologue_ctl_type(&ctl->prologue), ctl_psn);
	if (psn_mpr_pos(pdc->rx_base_psn, ctl_psn) != UET_PDC_MPR_CUR)
		goto out;
	switch (uet_prologue_ctl_type(&ctl->prologue)) {
	case UET_CTL_TYPE_CLOSE:
		/* only the initiator can send CLOSE */
		if (pdc->is_initiator)
			break;
		ret = 0;
		uet_pdc_send_ses_ack(pdc, UET_SES_RSP_RC_NULL, 0,
				     be32_to_cpu(ctl->psn),
				     0, true);
		uet_pdc_destroy(pdc);
		break;
	case UET_CTL_TYPE_REQ_CLOSE:
		/* only the target can send REQ_CLOSE */
		if (!pdc->is_initiator)
			break;
		uet_pdc_close(pdc);
		break;
	default:
		break;
	}
out:
	spin_unlock(&pdc->lock);

	return ret;
}
