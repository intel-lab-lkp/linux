// SPDX-License-Identifier: (GPL-2.0-or-later OR BSD-3-Clause)
/*
 * Copyright (c) 2014-2025, Advanced Micro Devices, Inc.
 * Copyright (c) 2014, Synopsys, Inc.
 * All rights reserved
 *
 * Author: Raju Rangoju <Raju.Rangoju@amd.com>
 */
#include <linux/crc32.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <net/tcp.h>
#include <net/udp.h>
#include "xgbe.h"
#include "xgbe-common.h"

#define XGBE_TEST_PKT_SIZE (sizeof(struct ethhdr) + \
			    sizeof(struct iphdr) +  \
			    sizeof(struct xgbe_hdr))

#define XGBE_TEST_PKT_MAGIC	0xdeadbeefdeadfeedULL
#define XGBE_LB_TIMEOUT	msecs_to_jiffies(200)

#define XGBE_LOOPBACK_NONE	0
#define XGBE_LOOPBACK_MAC	1

struct xgbe_hdr {
	__be32 version;
	__be64 magic;
	u8 id;
} __packed;

struct xgbe_pkt_attrs {
	unsigned char *src;
	const unsigned char *dst;
	u32 ip_src;
	u32 ip_dst;
	int tcp;
	int sport;
	int dport;
	int timeout;
	int size;
	int max_size;
	u8 id;
	u16 queue_mapping;
	u64 timestamp;
};

struct xgbe_test_data {
	struct xgbe_pkt_attrs *packet;
	struct packet_type pt;
	struct completion comp;
	int ok;
};

struct xgbe_test {
	char name[ETH_GSTRING_LEN];
	int lb;
	int (*fn)(struct xgbe_prv_data *pdata);
};

static u8 xgbe_test_id;

static int xgbe_config_mac_loopback(struct xgbe_prv_data *pdata, bool enable)
{
	XGMAC_IOWRITE_BITS(pdata, MAC_RCR, LM, enable ? 1 : 0);
	return 0;
}

static struct sk_buff *xgbe_test_get_skb(struct xgbe_prv_data *pdata,
					 struct xgbe_pkt_attrs *attr)
{
	struct sk_buff *skb = NULL;
	struct udphdr *uh = NULL;
	struct tcphdr *th = NULL;
	struct xgbe_hdr *hdr;
	struct ethhdr *eh;
	struct iphdr *ih;
	int iplen, size;

	size = attr->size + XGBE_TEST_PKT_SIZE;

	if (attr->tcp)
		size += sizeof(struct tcphdr);
	else
		size += sizeof(struct udphdr);

	if (attr->max_size && attr->max_size > size)
		size = attr->max_size;

	skb = netdev_alloc_skb(pdata->netdev, size);
	if (!skb)
		return NULL;

	prefetchw(skb->data);

	eh = skb_push(skb, ETH_HLEN);
	skb_reset_mac_header(skb);

	skb_set_network_header(skb, skb->len);
	ih = skb_put(skb, sizeof(*ih));

	skb_set_transport_header(skb, skb->len);
	if (attr->tcp)
		th = skb_put(skb, sizeof(*th));
	else
		uh = skb_put(skb, sizeof(*uh));

	eth_zero_addr(eh->h_source);
	eth_zero_addr(eh->h_dest);
	if (attr->src)
		ether_addr_copy(eh->h_source, attr->src);
	if (attr->dst)
		ether_addr_copy(eh->h_dest, attr->dst);

	eh->h_proto = htons(ETH_P_IP);

	if (attr->tcp) {
		th->source = htons(attr->sport);
		th->dest = htons(attr->dport);
		th->doff = sizeof(struct tcphdr) / 4;
		th->check = 0;
	} else {
		uh->source = htons(attr->sport);
		uh->dest = htons(attr->dport);
		uh->len = htons(sizeof(*hdr) + sizeof(*uh) + attr->size);
		if (attr->max_size)
			uh->len = htons(attr->max_size -
					  (sizeof(*ih) + sizeof(*eh)));
		uh->check = 0;
	}

	ih->ihl = 5;
	ih->ttl = 32;
	ih->version = IPVERSION;
	if (attr->tcp)
		ih->protocol = IPPROTO_TCP;
	else
		ih->protocol = IPPROTO_UDP;
	iplen = sizeof(*ih) + sizeof(*hdr) + attr->size;
	if (attr->tcp)
		iplen += sizeof(*th);
	else
		iplen += sizeof(*uh);

	if (attr->max_size)
		iplen = attr->max_size - sizeof(*eh);

	ih->tot_len = htons(iplen);
	ih->frag_off = 0;
	ih->saddr = htonl(attr->ip_src);
	ih->daddr = htonl(attr->ip_dst);
	ih->tos = 0;
	ih->id = 0;
	ip_send_check(ih);

	hdr = skb_put(skb, sizeof(*hdr));
	hdr->version = 0;
	hdr->magic = cpu_to_be64(XGBE_TEST_PKT_MAGIC);
	attr->id = xgbe_test_id;
	hdr->id = xgbe_test_id++;

	if (attr->size)
		skb_put(skb, attr->size);
	if (attr->max_size && attr->max_size > skb->len)
		skb_put(skb, attr->max_size - skb->len);

	skb->csum = 0;
	skb->ip_summed = CHECKSUM_PARTIAL;
	if (attr->tcp) {
		th->check = ~tcp_v4_check(skb->len, ih->saddr, ih->daddr, 0);
		skb->csum_start = skb_transport_header(skb) - skb->head;
		skb->csum_offset = offsetof(struct tcphdr, check);
	} else {
		udp4_hwcsum(skb, ih->saddr, ih->daddr);
	}

	skb->protocol = htons(ETH_P_IP);
	skb->pkt_type = PACKET_HOST;
	skb->dev = pdata->netdev;

	if (attr->timestamp)
		skb->tstamp = ns_to_ktime(attr->timestamp);

	return skb;
}

static int xgbe_test_loopback_validate(struct sk_buff *skb,
				       struct net_device *ndev,
				       struct packet_type *pt,
				       struct net_device *orig_ndev)
{
	struct xgbe_test_data *tdata = pt->af_packet_priv;
	const unsigned char *dst = tdata->packet->dst;
	unsigned char *src = tdata->packet->src;
	struct xgbe_hdr *hdr;
	struct ethhdr *eh;
	struct iphdr *ih;
	struct tcphdr *th;
	struct udphdr *uh;

	skb = skb_unshare(skb, GFP_ATOMIC);
	if (!skb)
		goto out;

	if (skb_linearize(skb))
		goto out;

	if (skb_headlen(skb) < (XGBE_TEST_PKT_SIZE - ETH_HLEN))
		goto out;

	eh = (struct ethhdr *)skb_mac_header(skb);
	if (dst) {
		if (!ether_addr_equal_unaligned(eh->h_dest, dst))
			goto out;
	}
	if (src) {
		if (!ether_addr_equal_unaligned(eh->h_source, src))
			goto out;
	}

	ih = ip_hdr(skb);

	if (tdata->packet->tcp) {
		if (ih->protocol != IPPROTO_TCP)
			goto out;

		th = (struct tcphdr *)((u8 *)ih + 4 * ih->ihl);
		if (th->dest != htons(tdata->packet->dport))
			goto out;

		hdr = (struct xgbe_hdr *)((u8 *)th + sizeof(*th));
	} else {
		if (ih->protocol != IPPROTO_UDP)
			goto out;

		uh = (struct udphdr *)((u8 *)ih + 4 * ih->ihl);
		if (uh->dest != htons(tdata->packet->dport))
			goto out;

		hdr = (struct xgbe_hdr *)((u8 *)uh + sizeof(*uh));
	}

	if (hdr->magic != cpu_to_be64(XGBE_TEST_PKT_MAGIC))
		goto out;
	if (tdata->packet->id != hdr->id)
		goto out;

	tdata->ok = true;
	complete(&tdata->comp);
out:
	kfree_skb(skb);
	return 0;
}

static int __xgbe_test_loopback(struct xgbe_prv_data *pdata,
				struct xgbe_pkt_attrs *attr)
{
	struct xgbe_test_data *tdata;
	struct sk_buff *skb = NULL;
	int ret = 0;

	tdata = kzalloc(sizeof(*tdata), GFP_KERNEL);
	if (!tdata)
		return -ENOMEM;

	tdata->ok = false;
	init_completion(&tdata->comp);

	tdata->pt.type = htons(ETH_P_IP);
	tdata->pt.func = xgbe_test_loopback_validate;
	tdata->pt.dev = pdata->netdev;
	tdata->pt.af_packet_priv = tdata;
	tdata->packet = attr;

	dev_add_pack(&tdata->pt);

	skb = xgbe_test_get_skb(pdata, attr);
	if (!skb) {
		ret = -ENOMEM;
		goto cleanup;
	}

	ret = dev_direct_xmit(skb, attr->queue_mapping);
	if (ret)
		goto cleanup;

	if (!attr->timeout)
		attr->timeout = XGBE_LB_TIMEOUT;

	wait_for_completion_timeout(&tdata->comp, attr->timeout);
	ret = tdata->ok ? 0 : -ETIMEDOUT;

	if (ret)
		netdev_err(pdata->netdev, "Response timedout: ret %d\n", ret);
cleanup:
	dev_remove_pack(&tdata->pt);
	kfree(tdata);
	return ret;
}

static int xgbe_test_mac_loopback(struct xgbe_prv_data *pdata)
{
	struct xgbe_pkt_attrs attr = {};

	attr.dst = pdata->netdev->dev_addr;
	return __xgbe_test_loopback(pdata, &attr);
}

static const struct xgbe_test xgbe_selftests[] = {
	{
		.name = "MAC Loopback               ",
		.lb = XGBE_LOOPBACK_MAC,
		.fn = xgbe_test_mac_loopback,
	},
};

void xgbe_selftest_run(struct net_device *dev,
		       struct ethtool_test *etest, u64 *buf)
{
	struct xgbe_prv_data *pdata = netdev_priv(dev);
	int count = xgbe_selftest_get_count(pdata);
	int i, ret;

	memset(buf, 0, sizeof(*buf) * count);
	xgbe_test_id = 0;

	if (etest->flags != ETH_TEST_FL_OFFLINE) {
		netdev_err(pdata->netdev, "Only offline tests are supported\n");
		etest->flags |= ETH_TEST_FL_FAILED;
		return;
	} else if (!netif_carrier_ok(dev)) {
		netdev_err(pdata->netdev,
			   "Invalid link, cannot execute tests\n");
		etest->flags |= ETH_TEST_FL_FAILED;
		return;
	}

	/* Wait for queues drain */
	msleep(200);

	for (i = 0; i < count; i++) {
		ret = 0;

		switch (xgbe_selftests[i].lb) {
		case XGBE_LOOPBACK_MAC:
			ret = xgbe_config_mac_loopback(pdata, true);
			break;
		case XGBE_LOOPBACK_NONE:
			break;
		default:
			ret = -EOPNOTSUPP;
			break;
		}

		/*
		 * First tests will always be MAC / PHY loopback.
		 * If any of them is not supported we abort earlier.
		 */
		if (ret) {
			netdev_err(pdata->netdev, "Loopback not supported\n");
			etest->flags |= ETH_TEST_FL_FAILED;
			break;
		}

		ret = xgbe_selftests[i].fn(pdata);
		if (ret && (ret != -EOPNOTSUPP))
			etest->flags |= ETH_TEST_FL_FAILED;
		buf[i] = ret;

		switch (xgbe_selftests[i].lb) {
		case XGBE_LOOPBACK_MAC:
			xgbe_config_mac_loopback(pdata, false);
			break;
		default:
			break;
		}
	}
}

void xgbe_selftest_get_strings(struct xgbe_prv_data *pdata, u8 *data)
{
	u8 *p = data;
	int i;

	for (i = 0; i < xgbe_selftest_get_count(pdata); i++) {
		snprintf(p, ETH_GSTRING_LEN, "%2d. %s", i + 1,
			 xgbe_selftests[i].name);
		p += ETH_GSTRING_LEN;
	}
}

int xgbe_selftest_get_count(struct xgbe_prv_data *pdata)
{
	return ARRAY_SIZE(xgbe_selftests);
}

