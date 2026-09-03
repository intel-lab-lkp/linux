// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * Copyright(c) 2020 Intel Corporation.
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 *
 */

#include "netdev.h"
#include "ipoib.h"

#define HFI2_IPOIB_SKB_PAD ((NET_SKB_PAD) + (NET_IP_ALIGN))

static void copy_ipoib_buf(struct sk_buff *skb, void *data, int size)
{
	skb_checksum_none_assert(skb);
	skb->protocol = *((__be16 *)data);

	skb_put_data(skb, data, size);
	skb->mac_header = HFI2_IPOIB_PSEUDO_LEN;
	skb_pull(skb, HFI2_IPOIB_ENCAP_LEN);
}

static struct sk_buff *prepare_frag_skb(struct napi_struct *napi, int size)
{
	struct sk_buff *skb;
	int skb_size = SKB_DATA_ALIGN(size + HFI2_IPOIB_SKB_PAD);
	void *frag;

	skb_size += SKB_DATA_ALIGN(sizeof(struct skb_shared_info));
	skb_size = SKB_DATA_ALIGN(skb_size);
	frag = napi_alloc_frag(skb_size);

	if (unlikely(!frag))
		return napi_alloc_skb(napi, size);

	skb = build_skb(frag, skb_size);

	if (unlikely(!skb)) {
		skb_free_frag(frag);
		return NULL;
	}

	skb_reserve(skb, HFI2_IPOIB_SKB_PAD);
	return skb;
}

struct sk_buff *hfi2_ipoib_prepare_skb(struct hfi2_netdev_rxq *rxq,
				       int size, void *data)
{
	struct napi_struct *napi = &rxq->napi;
	int skb_size = size + HFI2_IPOIB_ENCAP_LEN;
	struct sk_buff *skb;

	/*
	 * For smaller(4k + skb overhead) allocations we will go using
	 * napi cache. Otherwise we will try to use napi frag cache.
	 */
	if (size <= SKB_WITH_OVERHEAD(PAGE_SIZE))
		skb = napi_alloc_skb(napi, skb_size);
	else
		skb = prepare_frag_skb(napi, skb_size);

	if (unlikely(!skb))
		return NULL;

	copy_ipoib_buf(skb, data, size);

	return skb;
}

int hfi2_ipoib_rxq_init(struct net_device *netdev)
{
	struct hfi2_ipoib_dev_priv *ipoib_priv = hfi2_ipoib_priv(netdev);
	struct hfi2_pportdata *ppd = ipoib_priv->ppd;
	int ret;

	ret = hfi2_netdev_rx_init(ppd);
	if (ret)
		return ret;

	hfi2_init_aip_rsm(ppd);

	return ret;
}

void hfi2_ipoib_rxq_deinit(struct net_device *netdev)
{
	struct hfi2_ipoib_dev_priv *ipoib_priv = hfi2_ipoib_priv(netdev);
	struct hfi2_pportdata *ppd = ipoib_priv->ppd;

	hfi2_deinit_aip_rsm(ppd);
	hfi2_netdev_rx_destroy(ppd);
}
