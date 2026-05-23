// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 Jihong Min <hurryman2212@gmail.com>
 */
#include <crypto/eip93-ipsec.h>
#include <linux/err.h>
#include <linux/kmod.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>
#include <linux/udp.h>
#include <net/dst_metadata.h>
#include <net/esp.h>
#include <net/ip.h>
#include <net/ip6_checksum.h>
#include <net/ipv6.h>
#include <net/net_namespace.h>
#include <net/xfrm.h>

#include "airoha_eth.h"

#if IS_ENABLED(CONFIG_NET_AIROHA_XFRM)
DEFINE_STATIC_KEY_FALSE(airoha_xfrm_in_state_key);
DEFINE_STATIC_KEY_FALSE(airoha_xfrm_out_state_key);
#endif

#if IS_ENABLED(CONFIG_NET_AIROHA_XFRM) &&            \
	IS_REACHABLE(CONFIG_CRYPTO_DEV_EIP93) &&     \
	IS_ENABLED(CONFIG_CRYPTO_DEV_EIP93_IPSEC) && \
	IS_REACHABLE(CONFIG_INET_ESP) &&             \
	IS_REACHABLE(CONFIG_INET_ESP_OFFLOAD) &&     \
	IS_ENABLED(CONFIG_XFRM_OFFLOAD)
#define AIROHA_XFRM_FEATURES \
	(NETIF_F_HW_ESP | NETIF_F_HW_ESP_TX_CSUM | NETIF_F_GSO_ESP)

struct airoha_xfrm_state {
	struct airoha_gdm_port *port;
	struct eip93_ipsec_sa *sa;
};

static netdev_features_t airoha_xfrm_ipsec_features(struct eip93_ipsec *ipsec)
{
	netdev_features_t features = 0;
	u32 ipsec_features;

	ipsec_features = eip93_ipsec_features(ipsec);
	if (ipsec_features & EIP93_IPSEC_FEATURE_ESP)
		features |= NETIF_F_HW_ESP;
	if (ipsec_features & EIP93_IPSEC_FEATURE_HW_ESP_TX_CSUM)
		features |= NETIF_F_HW_ESP_TX_CSUM;
	if (ipsec_features & EIP93_IPSEC_FEATURE_GSO_ESP)
		features |= NETIF_F_GSO_ESP;

	return features;
}

static int airoha_xfrm_request_module(struct net_device *dev,
				      const char *module_name)
{
	int err;

	err = request_module("%s", module_name);
	if (err) {
		netdev_err(dev, "failed requesting module %s: %d\n",
			   module_name, err);
		return err < 0 ? err : -ENOENT;
	}

	return 0;
}

static int airoha_xfrm_request_modules(struct net_device *dev)
{
	int err;

	if (IS_MODULE(CONFIG_INET_ESP)) {
		err = airoha_xfrm_request_module(dev, "esp4");
		if (err)
			return err;
	}

	if (IS_MODULE(CONFIG_INET_ESP_OFFLOAD)) {
		err = airoha_xfrm_request_module(dev, "esp4_offload");
		if (err)
			return err;
	}

#if IS_REACHABLE(CONFIG_INET6_ESP)
	if (IS_MODULE(CONFIG_INET6_ESP)) {
		err = airoha_xfrm_request_module(dev, "esp6");
		if (err)
			return err;
	}
#endif

#if IS_REACHABLE(CONFIG_INET6_ESP_OFFLOAD)
	if (IS_MODULE(CONFIG_INET6_ESP_OFFLOAD)) {
		err = airoha_xfrm_request_module(dev, "esp6_offload");
		if (err)
			return err;
	}
#endif

	if (IS_MODULE(CONFIG_CRYPTO_DEV_EIP93)) {
		err = airoha_xfrm_request_module(dev, "crypto-hw-eip93");
		if (err)
			return err;
	}

	return 0;
}

static int airoha_xfrm_prepare_ipsec(struct net_device *dev)
{
	struct airoha_gdm_port *port = netdev_priv(dev);
	struct eip93_ipsec *ipsec;
	int err;

	if (port->xfrm_ipsec)
		return eip93_ipsec_available(port->xfrm_ipsec) ? 0 : -ENODEV;

	err = airoha_xfrm_request_modules(dev);
	if (err)
		return err;

	ipsec = eip93_ipsec_get(port->eth->dev);
	if (IS_ERR(ipsec)) {
		netdev_dbg(dev,
			   "EIP93 ESP packet backend is unavailable: %ld\n",
			   PTR_ERR(ipsec));
		return PTR_ERR(ipsec);
	}

	port->xfrm_ipsec = ipsec;
	netdev_info(dev, "ESP HW offload available via EIP93 packet backend\n");

	return 0;
}

static bool airoha_xfrm_state_supported(struct xfrm_state *x,
					struct netlink_ext_ack *extack)
{
	if (x->xso.type != XFRM_DEV_OFFLOAD_CRYPTO) {
		NL_SET_ERR_MSG_MOD(extack,
				   "only XFRM crypto offload is supported");
		return false;
	}

	switch (x->xso.dir) {
	case XFRM_DEV_OFFLOAD_OUT:
	case XFRM_DEV_OFFLOAD_IN:
		break;
	default:
		NL_SET_ERR_MSG_MOD(extack, "only in/out SAs are supported");
		return false;
	}

	switch (x->props.family) {
	case AF_INET:
		break;
#if IS_REACHABLE(CONFIG_INET6_ESP) && IS_REACHABLE(CONFIG_INET6_ESP_OFFLOAD)
	case AF_INET6:
		break;
#endif
	default:
		NL_SET_ERR_MSG_MOD(extack,
				   "only IPv4/IPv6 ESP offload is supported");
		return false;
	}

	if (x->outer_mode.family != x->props.family) {
		NL_SET_ERR_MSG_MOD(extack,
				   "only same-family ESP offload is supported");
		return false;
	}

	if (x->id.proto != IPPROTO_ESP) {
		NL_SET_ERR_MSG_MOD(extack, "only ESP offload is supported");
		return false;
	}

	switch (x->props.mode) {
	case XFRM_MODE_TUNNEL:
	case XFRM_MODE_TRANSPORT:
		break;
	default:
		NL_SET_ERR_MSG_MOD(extack,
				   "only tunnel/transport modes are supported");
		return false;
	}

	if (x->outer_mode.encap != x->props.mode) {
		NL_SET_ERR_MSG_MOD(extack,
				   "outer ESP mode does not match state mode");
		return false;
	}

	if (x->encap) {
		NL_SET_ERR_MSG_MOD(extack,
				   "NAT-T is unsupported by EIP93 packet ESP");
		return false;
	}

	if (x->tfcpad) {
		NL_SET_ERR_MSG_MOD(extack, "TFC padding is not supported");
		return false;
	}

	if (x->aead) {
		NL_SET_ERR_MSG_MOD(extack, "AEAD SAs are unsupported");
		return false;
	}

	if (!x->ealg || !x->aalg) {
		NL_SET_ERR_MSG_MOD(extack,
				   "encryption/authentication required");
		return false;
	}

	return true;
}

static const struct xfrmdev_ops airoha_xfrmdev_ops;

#if IS_ENABLED(CONFIG_NET_DSA)
static struct airoha_gdm_port *airoha_xfrm_dsa_dev_port(struct net_device *dev)
{
	struct net_device *conduit;
	struct dsa_port *dp;

	if (!dsa_user_dev_check(dev))
		return NULL;

	dp = dsa_port_from_netdev(dev);
	if (IS_ERR(dp))
		return NULL;

	conduit = dsa_port_to_conduit(dp);
	if (!conduit || conduit->xfrmdev_ops != &airoha_xfrmdev_ops)
		return NULL;

	return netdev_priv(conduit);
}

static struct net_device *airoha_xfrm_dsa_rx_dev(struct airoha_gdm_port *port,
						 struct sk_buff *skb)
{
	struct metadata_dst *md_dst = skb_metadata_dst(skb);
	struct dsa_port *cpu_dp = port->dev->dsa_ptr;
	struct dsa_port *dp;
	u32 source_port;

	if (!md_dst || md_dst->type != METADATA_HW_PORT_MUX)
		return port->dev;

	if (!cpu_dp || !cpu_dp->dst)
		return NULL;

	source_port = md_dst->u.port_info.port_id;
	list_for_each_entry(dp, &cpu_dp->dst->ports, list) {
		if (dp->type != DSA_PORT_TYPE_USER ||
		    dp->index != source_port || dp->cpu_dp != cpu_dp ||
		    dsa_port_to_conduit(dp) != port->dev || !dp->user)
			continue;

		return dp->user;
	}

	return NULL;
}

static bool airoha_xfrm_dsa_user_matches_port(struct net_device *user,
					      struct net_device *conduit)
{
	struct dsa_port *dp;

	if (!dsa_user_dev_check(user))
		return false;

	dp = dsa_port_from_netdev(user);
	if (IS_ERR(dp))
		return false;

	return dsa_port_to_conduit(dp) == conduit;
}
#else
static struct airoha_gdm_port *airoha_xfrm_dsa_dev_port(struct net_device *dev)
{
	return NULL;
}

static struct net_device *airoha_xfrm_dsa_rx_dev(struct airoha_gdm_port *port,
						 struct sk_buff *skb)
{
	return port->dev;
}
#endif

static struct airoha_gdm_port *airoha_xfrm_dev_port(struct net_device *dev)
{
	struct airoha_gdm_port *port;

	if (dev->xfrmdev_ops != &airoha_xfrmdev_ops)
		return NULL;

	port = airoha_xfrm_dsa_dev_port(dev);
	if (port)
		return port;

	return netdev_priv(dev);
}

static netdev_features_t airoha_xfrm_dev_features(struct net_device *dev)
{
	struct airoha_gdm_port *port = airoha_xfrm_dev_port(dev);

	if (!port || !port->xfrm_ipsec)
		return 0;

	return airoha_xfrm_ipsec_features(port->xfrm_ipsec);
}

static struct net_device *airoha_xfrm_rx_dev(struct airoha_gdm_port *port,
					     struct sk_buff *skb)
{
	if (!netdev_uses_dsa(port->dev))
		return port->dev;

	return airoha_xfrm_dsa_rx_dev(port, skb);
}

static void airoha_xfrm_state_advance_esn(struct xfrm_state *x)
{
	struct airoha_xfrm_state *state;

	state = (struct airoha_xfrm_state *)x->xso.offload_handle;
	if (state)
		eip93_ipsec_state_advance_esn(state->sa, x);
}

static int airoha_xfrm_state_add(struct net_device *dev, struct xfrm_state *x,
				 struct netlink_ext_ack *extack)
{
	struct airoha_gdm_port *port = airoha_xfrm_dev_port(dev);
	struct airoha_xfrm_state *state;
	int err;

	if (!port) {
		NL_SET_ERR_MSG_MOD(extack, "device lacks Airoha ESP offload");
		return -EOPNOTSUPP;
	}

	if (!(dev->features & NETIF_F_HW_ESP)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "ESP HW offload is disabled on device");
		return -EOPNOTSUPP;
	}

	if (!port->xfrm_ipsec || !eip93_ipsec_available(port->xfrm_ipsec)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "EIP93 packet backend is unavailable");
		return -EOPNOTSUPP;
	}

	if (!airoha_xfrm_state_supported(x, extack))
		return -EOPNOTSUPP;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	state->port = port;
	err = eip93_ipsec_state_add(port->xfrm_ipsec, x, extack, &state->sa);
	if (err) {
		kfree(state);
		return err;
	}

	x->xso.offload_handle = (unsigned long)state;
	atomic_inc(&port->xfrm_state_count);
	if (x->xso.dir == XFRM_DEV_OFFLOAD_OUT) {
		atomic_inc(&port->xfrm_out_state_count);
		static_branch_inc(&airoha_xfrm_out_state_key);
	} else {
		atomic_inc(&port->xfrm_in_state_count);
		static_branch_inc(&airoha_xfrm_in_state_key);
	}

	return 0;
}

static void airoha_xfrm_state_delete(struct net_device *dev,
				     struct xfrm_state *x)
{
	struct airoha_xfrm_state *state;
	struct airoha_gdm_port *port;

	state = (struct airoha_xfrm_state *)x->xso.offload_handle;
	if (!state)
		return;

	port = state->port;
	x->xso.offload_handle = 0;
	atomic_dec(&port->xfrm_state_count);
	if (x->xso.dir == XFRM_DEV_OFFLOAD_OUT) {
		atomic_dec(&port->xfrm_out_state_count);
		static_branch_dec(&airoha_xfrm_out_state_key);
	} else if (x->xso.dir == XFRM_DEV_OFFLOAD_IN) {
		atomic_dec(&port->xfrm_in_state_count);
		static_branch_dec(&airoha_xfrm_in_state_key);
	}

	eip93_ipsec_state_delete(state->sa);
	kfree(state);
}

static bool airoha_xfrm_offload_ok(struct sk_buff *skb, struct xfrm_state *x)
{
	struct net_device *dev = skb->dev;
	struct airoha_xfrm_state *state;
	struct airoha_gdm_port *port;

	if (!dev)
		return false;

	port = airoha_xfrm_dev_port(dev);
	if (!port)
		return false;

	if (unlikely(x->xso.dir != XFRM_DEV_OFFLOAD_OUT ||
		     x->xso.type != XFRM_DEV_OFFLOAD_CRYPTO ||
		     !(dev->features & NETIF_F_HW_ESP) || x->xso.dev != dev))
		return false;

	state = (struct airoha_xfrm_state *)x->xso.offload_handle;
	if (!state || state->port != port)
		return false;

	if (unlikely(skb_is_gso(skb)))
		return false;

	return true;
}

/*
 * EIP93 packet-out mode creates ESP padding, trailer and ICV. The generic ESP
 * xmit path should reserve tailroom only for plain, non-GSO ESP packets.
 */
static bool airoha_xfrm_esp_tx_hw_trailer(struct sk_buff *skb,
					  struct xfrm_state *x)
{
	return x->xso.dir == XFRM_DEV_OFFLOAD_OUT &&
	       x->xso.type == XFRM_DEV_OFFLOAD_CRYPTO && !x->encap &&
	       !skb_is_gso(skb);
}

static const struct xfrmdev_ops airoha_xfrmdev_ops = {
	.xdo_dev_state_add = airoha_xfrm_state_add,
	.xdo_dev_state_delete = airoha_xfrm_state_delete,
	.xdo_dev_state_free = airoha_xfrm_state_delete,
	.xdo_dev_offload_ok = airoha_xfrm_offload_ok,
	.xdo_dev_esp_tx_hw_trailer = airoha_xfrm_esp_tx_hw_trailer,
	.xdo_dev_state_advance_esn = airoha_xfrm_state_advance_esn,
};

void airoha_xfrm_build_netdev(struct net_device *dev)
{
	struct airoha_gdm_port *port = netdev_priv(dev);
	netdev_features_t features;

	atomic_set(&port->xfrm_state_count, 0);
	atomic_set(&port->xfrm_out_state_count, 0);
	atomic_set(&port->xfrm_in_state_count, 0);
	if (airoha_xfrm_prepare_ipsec(dev))
		return;

	features = airoha_xfrm_ipsec_features(port->xfrm_ipsec);
	if (!(features & NETIF_F_HW_ESP)) {
		eip93_ipsec_put(port->xfrm_ipsec);
		port->xfrm_ipsec = NULL;
		return;
	}

	dev->xfrmdev_ops = &airoha_xfrmdev_ops;
	dev->hw_features |= features;
	dev->hw_enc_features |= features;
	dev->gso_partial_features |= features & NETIF_F_GSO_ESP;
}

void airoha_xfrm_teardown_netdev(struct net_device *dev)
{
	struct airoha_gdm_port *port = netdev_priv(dev);

	if (port->xfrm_ipsec) {
		eip93_ipsec_put(port->xfrm_ipsec);
		port->xfrm_ipsec = NULL;
	}
}

/* Airoha TX checksum/GSO offloads run after EIP93 has encrypted the skb, so
 * they cannot operate on plaintext ESP payloads or build per-segment ESP data.
 */
netdev_features_t airoha_xfrm_fix_features(struct net_device *dev,
					   netdev_features_t features)
{
	netdev_features_t supported = airoha_xfrm_dev_features(dev);
	netdev_features_t unsupported = AIROHA_XFRM_FEATURES & ~supported;

	if (features & unsupported)
		features &= ~unsupported;

	if (!(features & NETIF_F_HW_ESP))
		features &= ~(NETIF_F_HW_ESP_TX_CSUM | NETIF_F_GSO_ESP);

	return features;
}

int airoha_xfrm_set_features(struct net_device *dev, netdev_features_t features)
{
	netdev_features_t changed = (dev->features ^ features) &
				    AIROHA_XFRM_FEATURES;
	netdev_features_t requested = features & AIROHA_XFRM_FEATURES;
	struct airoha_gdm_port *port = netdev_priv(dev);
	netdev_features_t supported;
	int err;

	if (!changed)
		return 0;

	if (requested & NETIF_F_HW_ESP) {
		err = airoha_xfrm_prepare_ipsec(dev);
		if (err)
			return err;
	}

	supported = airoha_xfrm_dev_features(dev);
	if (requested & ~supported)
		return -EOPNOTSUPP;

	if (atomic_read(&port->xfrm_state_count)) {
		netdev_err(dev, "cannot change ESP features with active SAs\n");
		return -EBUSY;
	}

	if (!(features & NETIF_F_HW_ESP))
		netdev_info(dev, "ESP HW offload disabled\n");

	return 0;
}

struct airoha_xfrm_rx_info {
	unsigned short family;
	int encap_type;
	int esp_offset;
	int packet_len;
	__be32 spi;
	__be32 seq;
};

struct airoha_xfrm_rx_ctx {
	struct sk_buff *skb;
	struct net_device *dev;
};

static bool airoha_xfrm_parse_rx_ipv4(struct sk_buff *skb,
				      struct airoha_xfrm_rx_info *info)
{
	struct ip_esp_hdr *esph;
	struct iphdr *iph;
	int packet_len;
	int iphlen;

	if (!pskb_may_pull(skb, sizeof(*iph)))
		return false;

	iph = ip_hdr(skb);
	if (iph->version != 4)
		return false;

	iphlen = iph->ihl * 4;
	if (iphlen < sizeof(*iph) || !pskb_may_pull(skb, iphlen))
		return false;

	if (ip_is_fragment(iph))
		return false;

	packet_len = ntohs(iph->tot_len);
	if (packet_len < iphlen || packet_len > skb->len)
		return false;

	switch (iph->protocol) {
	case IPPROTO_ESP:
		info->encap_type = 0;
		info->esp_offset = iphlen;
		info->packet_len = packet_len;
		break;
	case IPPROTO_UDP: {
		struct udphdr *uh;
		int udp_len;
		__be32 marker;

		if (!pskb_may_pull(skb, iphlen + sizeof(*uh) + sizeof(*esph)))
			return false;

		uh = (struct udphdr *)(skb->data + iphlen);
		udp_len = ntohs(uh->len);
		if (udp_len <= sizeof(*uh) + sizeof(*esph) ||
		    iphlen + udp_len > packet_len)
			return false;

		memcpy(&marker, skb->data + iphlen + sizeof(*uh),
		       sizeof(marker));
		if (!marker)
			return false;

		info->encap_type = UDP_ENCAP_ESPINUDP;
		info->esp_offset = iphlen + sizeof(*uh);
		info->packet_len = iphlen + udp_len;
		break;
	}
	default:
		return false;
	}

	if (info->esp_offset + sizeof(*esph) > info->packet_len ||
	    !pskb_may_pull(skb, info->esp_offset + sizeof(*esph)))
		return false;

	esph = (struct ip_esp_hdr *)(skb->data + info->esp_offset);
	info->family = AF_INET;
	info->spi = esph->spi;
	info->seq = esph->seq_no;

	return !!info->spi;
}

#if IS_ENABLED(CONFIG_IPV6)
static bool airoha_xfrm_parse_rx_ipv6(struct sk_buff *skb,
				      struct airoha_xfrm_rx_info *info)
{
	struct ip_esp_hdr *esph;
	struct ipv6hdr *ip6h;
	__be16 frag_off;
	int packet_len;
	int offset;
	u8 nexthdr;

	if (!pskb_may_pull(skb, sizeof(*ip6h)))
		return false;

	ip6h = ipv6_hdr(skb);
	if (ip6h->version != 6)
		return false;

	if (!ip6h->payload_len)
		return false;

	packet_len = sizeof(*ip6h) + ntohs(ip6h->payload_len);
	if (packet_len < sizeof(*ip6h) || packet_len > skb->len)
		return false;

	nexthdr = ip6h->nexthdr;
	offset = ipv6_skip_exthdr(skb, sizeof(*ip6h), &nexthdr, &frag_off);
	if (offset < 0 || frag_off)
		return false;

	switch (nexthdr) {
	case NEXTHDR_ESP:
		info->encap_type = 0;
		info->esp_offset = offset;
		info->packet_len = packet_len;
		break;
	case NEXTHDR_UDP: {
		struct udphdr *uh;
		int udp_len;
		__be32 marker;

		if (!pskb_may_pull(skb, offset + sizeof(*uh) + sizeof(*esph)))
			return false;

		uh = (struct udphdr *)(skb->data + offset);
		udp_len = ntohs(uh->len);
		if (udp_len <= sizeof(*uh) + sizeof(*esph) ||
		    offset + udp_len > packet_len)
			return false;

		memcpy(&marker, skb->data + offset + sizeof(*uh),
		       sizeof(marker));
		if (!marker)
			return false;

		info->encap_type = UDP_ENCAP_ESPINUDP;
		info->esp_offset = offset + sizeof(*uh);
		info->packet_len = offset + udp_len;
		break;
	}
	default:
		return false;
	}

	if (info->esp_offset + sizeof(*esph) > info->packet_len ||
	    !pskb_may_pull(skb, info->esp_offset + sizeof(*esph)))
		return false;

	esph = (struct ip_esp_hdr *)(skb->data + info->esp_offset);
	info->family = AF_INET6;
	info->spi = esph->spi;
	info->seq = esph->seq_no;

	return !!info->spi;
}
#else
static bool airoha_xfrm_parse_rx_ipv6(struct sk_buff *skb,
				      struct airoha_xfrm_rx_info *info)
{
	return false;
}
#endif

static bool airoha_xfrm_parse_rx_skb(struct sk_buff *skb,
				     struct airoha_xfrm_rx_info *info)
{
	switch (skb->protocol) {
	case htons(ETH_P_IP):
		return airoha_xfrm_parse_rx_ipv4(skb, info);
	case htons(ETH_P_IPV6):
		return airoha_xfrm_parse_rx_ipv6(skb, info);
	default:
		return false;
	}
}

static struct xfrm_state *
airoha_xfrm_rx_state_lookup(struct airoha_gdm_port *port, struct sk_buff *skb,
			    const struct airoha_xfrm_rx_info *info)
{
	struct airoha_xfrm_state *state;
	xfrm_address_t daddr = {};
	struct net_device *dev;
	struct xfrm_state *x;

	dev = airoha_xfrm_rx_dev(port, skb);
	if (!dev)
		return NULL;

	switch (info->family) {
	case AF_INET:
		daddr.a4 = ip_hdr(skb)->daddr;
		break;
	case AF_INET6:
		daddr.in6 = ipv6_hdr(skb)->daddr;
		break;
	default:
		return NULL;
	}

	x = xfrm_input_state_lookup(dev_net(dev), skb->mark, &daddr, info->spi,
				    IPPROTO_ESP, info->family);
	if (!x)
		return NULL;

	if (x->dir && x->dir != XFRM_SA_DIR_IN)
		goto err_put;

	if (x->xso.dir != XFRM_DEV_OFFLOAD_IN ||
	    x->xso.type != XFRM_DEV_OFFLOAD_CRYPTO || x->xso.dev != dev ||
	    !(dev->features & NETIF_F_HW_ESP) || !x->type_offload ||
	    !x->type_offload->input_tail)
		goto err_put;

	state = (struct airoha_xfrm_state *)x->xso.offload_handle;
	if (!state || state->port != port)
		goto err_put;

	if ((x->encap ? x->encap->encap_type : 0) != info->encap_type)
		goto err_put;

	return x;

err_put:
	xfrm_state_put(x);
	return NULL;
}

static u32 airoha_xfrm_rx_status(int err, struct xfrm_state *x)
{
	if (!err)
		return CRYPTO_SUCCESS;

	if (err == -EBADMSG) {
		if (x->props.mode == XFRM_MODE_TUNNEL)
			return CRYPTO_TUNNEL_ESP_AUTH_FAILED;

		return CRYPTO_TRANSPORT_ESP_AUTH_FAILED;
	}

	if (err == -EINVAL)
		return CRYPTO_INVALID_PACKET_SYNTAX;

	return CRYPTO_GENERIC_ERROR;
}

static int airoha_xfrm_rx_apply_result(struct sk_buff *skb,
				       struct xfrm_state *x,
				       struct eip93_ipsec_result result)
{
	struct xfrm_offload *xo = xfrm_offload(skb);

	if (!x || !result.packet_len || result.packet_len > skb->len || !xo)
		return -EINVAL;

	/*
	 * EIP93 inbound ESP mode removes the ESP pad/trailer/ICV and reports
	 * the decapsulated outer packet length plus the recovered next-header.
	 */
	xo->proto = result.nexthdr;
	xo->flags |= XFRM_ESP_NO_TRAILER;
	if (pskb_trim(skb, result.packet_len))
		return -EINVAL;

	if (x->props.family == AF_INET) {
		ip_hdr(skb)->tot_len = htons(skb->len);
		ip_send_check(ip_hdr(skb));
	} else if (x->props.family == AF_INET6) {
		int len = skb->len - skb_network_offset(skb) -
			  sizeof(struct ipv6hdr);

		if (len < 0)
			return -EINVAL;

		ipv6_hdr(skb)->payload_len = len > IPV6_MAXPLEN ? 0 :
								      htons(len);
	}

	return 0;
}

static void airoha_xfrm_rx_free_ctx(struct airoha_xfrm_rx_ctx *ctx)
{
	kfree(ctx);
}

static void airoha_xfrm_rx_finish(void *data, int err,
				  struct eip93_ipsec_result result)
{
	struct airoha_xfrm_rx_ctx *ctx = data;
	struct net_device *dev = ctx->dev;
	struct sk_buff *skb = ctx->skb;
	struct xfrm_offload *xo;
	struct xfrm_state *x;

	x = xfrm_input_state(skb);
	xo = xfrm_offload(skb);
	if (!err)
		err = airoha_xfrm_rx_apply_result(skb, x, result);
	if (xo) {
		xo->flags |= CRYPTO_DONE;
		xo->status = airoha_xfrm_rx_status(err, x);
	}

	airoha_xfrm_rx_free_ctx(ctx);
	netif_receive_skb(skb);
	dev_put(dev);
}

static bool airoha_xfrm_tx_esp_offset(struct sk_buff *skb, struct xfrm_state *x,
				      unsigned int *esp_offset)
{
	u8 *esph = (u8 *)ip_esp_hdr(skb);

	if (x->encap)
		esph += sizeof(struct udphdr);

	if (esph < skb->data ||
	    esph + sizeof(struct ip_esp_hdr) > skb_tail_pointer(skb))
		return false;

	*esp_offset = esph - skb->data;

	return true;
}

static void airoha_xfrm_tx_update_outer_len(struct sk_buff *skb)
{
	struct iphdr *iph = ip_hdr(skb);

	if (iph->version == 4) {
		iph->tot_len = htons(skb->len - skb_network_offset(skb));
		ip_send_check(iph);
	} else if (iph->version == 6) {
		int len = skb->len - skb_network_offset(skb) -
			  sizeof(struct ipv6hdr);

		if (len < 0)
			return;

		ipv6_hdr(skb)->payload_len = len > IPV6_MAXPLEN ? 0 :
								  htons(len);
	}
}

static void airoha_xfrm_tx_udp6_csum(struct sk_buff *skb,
				     struct xfrm_state *x)
{
#if IS_ENABLED(CONFIG_IPV6)
	struct udphdr *uh;
	struct ipv6hdr *ip6h;
	unsigned int offset;
	__wsum csum;
	int len;

	if (x->props.family != AF_INET6 || !x->encap ||
	    x->encap->encap_type != UDP_ENCAP_ESPINUDP)
		return;

	offset = skb_transport_offset(skb);
	if (offset + sizeof(*uh) > skb->len)
		return;

	uh = udp_hdr(skb);
	ip6h = ipv6_hdr(skb);
	len = ntohs(uh->len);
	if (len < sizeof(*uh) || len > skb->len - offset)
		return;

	uh->check = 0;
	csum = skb_checksum(skb, offset, len, 0);
	uh->check = csum_ipv6_magic(&ip6h->saddr, &ip6h->daddr, len,
				    IPPROTO_UDP, csum);
	if (!uh->check)
		uh->check = CSUM_MANGLED_0;
	#endif
}

static int airoha_xfrm_tx_apply_result(struct sk_buff *skb,
				       struct xfrm_state *x,
				       struct eip93_ipsec_result result)
{
	unsigned int current_esp_len;
	unsigned int esp_offset;
	unsigned int new_len;

	if (!result.packet_len ||
	    !airoha_xfrm_tx_esp_offset(skb, x, &esp_offset))
		return -EINVAL;

	current_esp_len = skb->len - esp_offset;
	if (result.packet_len == current_esp_len)
		return 0;

	new_len = esp_offset + result.packet_len;
	if (new_len < esp_offset)
		return -EINVAL;

	/*
	 * EIP93 outbound ESP mode reports the generated ESP packet length.
	 * Reflect it in skb->len before the packet resumes into the Ethernet
	 * TX path, because generic ESP left hardware-generated trailer bytes
	 * outside skb->len.
	 */
	if (new_len > skb->len) {
		unsigned int delta = new_len - skb->len;

		if (delta > skb_tailroom(skb))
			return -ENOMEM;
		skb_put(skb, delta);

		return 0;
	}

	return pskb_trim(skb, new_len);
}

bool airoha_xfrm_rx_skb(struct airoha_gdm_port *port, struct sk_buff *skb)
{
	struct airoha_xfrm_rx_info info;
	struct airoha_xfrm_state *state;
	struct airoha_xfrm_rx_ctx *ctx;
	struct sk_buff *trailer;
	struct xfrm_offload *xo;
	struct xfrm_state *x;
	struct sec_path *sp;
	int err;
	u32 mark = skb->mark;

	if (!airoha_xfrm_parse_rx_skb(skb, &info))
		return false;

	x = airoha_xfrm_rx_state_lookup(port, skb, &info);
	if (!x)
		return false;

	sp = secpath_set(skb);
	if (!sp)
		goto err_put_state;

	if (sp->len == XFRM_MAX_DEPTH) {
		secpath_reset(skb);
		goto err_put_state;
	}

	skb->mark = xfrm_smark_get(mark, x);
	sp->xvec[sp->len++] = x;
	sp->olen++;
	XFRM_SKB_CB(skb)->seq.input.low = info.seq;
	XFRM_SKB_CB(skb)->seq.input.hi = htonl(xfrm_replay_seqhi(x, info.seq));
	XFRM_SPI_SKB_CB(skb)->family = info.family;
	XFRM_SPI_SKB_CB(skb)->seq = info.seq;
	if (info.family == AF_INET) {
		XFRM_SPI_SKB_CB(skb)->daddroff = offsetof(struct iphdr, daddr);
		XFRM_TUNNEL_SKB_CB(skb)->tunnel.ip4 = NULL;
	} else {
		XFRM_SPI_SKB_CB(skb)->daddroff =
			offsetof(struct ipv6hdr, daddr);
		XFRM_TUNNEL_SKB_CB(skb)->tunnel.ip6 = NULL;
	}

	xo = xfrm_offload(skb);
	if (!xo)
		goto err_reset;

	state = (struct airoha_xfrm_state *)x->xso.offload_handle;
	if (!state || state->port != port)
		goto err_reset;

	if (skb_cloned(skb) || skb_is_nonlinear(skb)) {
		err = skb_cow_data(skb, 0, &trailer);
		if (err < 0)
			goto err_reset;

		if (skb_is_nonlinear(skb)) {
			err = skb_linearize(skb);
			if (err)
				goto err_reset;
		}
	}

	ctx = kmalloc(sizeof(*ctx), GFP_ATOMIC);
	if (!ctx)
		goto err_reset;

	if (!skb->dev)
		goto err_free_ctx;

	ctx->skb = skb;
	ctx->dev = skb->dev;
	skb->ip_summed = CHECKSUM_NONE;

	dev_hold(ctx->dev);
	err = eip93_ipsec_receive(state->sa, skb, info.packet_len,
				  airoha_xfrm_rx_finish, ctx);
	if (err == -EINPROGRESS)
		return true;

	dev_put(ctx->dev);
	airoha_xfrm_rx_free_ctx(ctx);
	skb->mark = mark;
	secpath_reset(skb);

	return false;

err_free_ctx:
	airoha_xfrm_rx_free_ctx(ctx);
err_reset:
	skb->mark = mark;
	secpath_reset(skb);
	return false;

err_put_state:
	xfrm_state_put(x);
	return false;
}

static void airoha_xfrm_tx_done(void *data, int err,
				struct eip93_ipsec_result result)
{
	struct sk_buff *skb = data;
	struct xfrm_offload *xo = xfrm_offload(skb);
	struct sec_path *sp = skb_sec_path(skb);
	struct xfrm_state *x;

	if (!xo || !sp || !sp->len) {
		kfree_skb(skb);
		return;
	}

	x = sp->xvec[sp->len - 1];
	if (!err)
		err = airoha_xfrm_tx_apply_result(skb, x, result);
	if (err) {
		XFRM_INC_STATS(xs_net(x), LINUX_MIB_XFRMOUTSTATEPROTOERROR);
		kfree_skb(skb);
		return;
	}

	airoha_xfrm_tx_update_outer_len(skb);
	airoha_xfrm_tx_udp6_csum(skb, x);
	xo->flags |= CRYPTO_DONE;
	xo->status = CRYPTO_SUCCESS;
	skb_push(skb, skb->data - skb_mac_header(skb));
	secpath_reset(skb);
	xfrm_dev_resume(skb);
}

int airoha_xfrm_encrypt_skb(struct airoha_gdm_port *port, struct sk_buff *skb)
{
	struct xfrm_offload *xo = xfrm_offload(skb);
	struct airoha_xfrm_state *state;
	struct net_device *dev;
	struct xfrm_state *x;
	struct sec_path *sp;
	struct ip_esp_hdr *esph;
	struct sk_buff *trailer;
	unsigned int esp_offset;
	unsigned int tailen;
	int err;

	if (!xo || !(xo->flags & XFRM_XMIT) || (xo->flags & CRYPTO_DONE))
		return 0;

	sp = skb_sec_path(skb);
	if (!sp || !sp->len)
		return -EINVAL;

	x = sp->xvec[sp->len - 1];
	dev = x->xso.dev;
	if (unlikely(x->xso.dir != XFRM_DEV_OFFLOAD_OUT ||
		     x->xso.type != XFRM_DEV_OFFLOAD_CRYPTO || !dev ||
		     !(dev->features & NETIF_F_HW_ESP)))
		return -EOPNOTSUPP;

	state = (struct airoha_xfrm_state *)x->xso.offload_handle;
	if (!state || state->port != port)
		return -EOPNOTSUPP;

	if (unlikely(skb_is_gso(skb)))
		return -EOPNOTSUPP;

	if (unlikely(skb->ip_summed == CHECKSUM_PARTIAL)) {
		err = skb_checksum_help(skb);
		if (err)
			return err;
	}

	tailen = xo->esp_tx_tailen;
	if (skb_cloned(skb) || skb_is_nonlinear(skb)) {
		err = skb_cow_data(skb, tailen, &trailer);
		if (err < 0)
			return err;

		if (skb_is_nonlinear(skb)) {
			err = skb_linearize(skb);
			if (err)
				return err;
		}
	}
	/*
	 * Generic ESP reserves this tailroom before the skb reaches us. Keep a
	 * small guard here because COW/linearization can replace the skb head.
	 */
	if (tailen && skb_tailroom(skb) < tailen) {
		err = pskb_expand_head(skb, 0, tailen - skb_tailroom(skb),
				       GFP_ATOMIC);
		if (err)
			return err;
	}

	if (!airoha_xfrm_tx_esp_offset(skb, x, &esp_offset))
		return -EINVAL;

	esph = (struct ip_esp_hdr *)(skb->data + esp_offset);
	esph->seq_no = htonl(xo->seq.low);

	return eip93_ipsec_xmit(state->sa, skb, esp_offset, airoha_xfrm_tx_done,
				skb);
}

static void airoha_xfrm_flush_dev(struct net_device *dev)
{
	xfrm_dev_state_flush(dev_net(dev), dev, true);
	xfrm_dev_policy_flush(dev_net(dev), dev, true);
}

static void airoha_xfrm_link_change(struct net_device *dev)
{
	struct airoha_gdm_port *port = airoha_xfrm_dev_port(dev);

	if (!port || !(dev->hw_features & NETIF_F_HW_ESP) ||
	    !atomic_read(&port->xfrm_state_count))
		return;

	netdev_dbg(dev, "carrier %s, preserving ESP HW offload SAs\n",
		   netif_carrier_ok(dev) ? "up" : "down");
}

#if IS_ENABLED(CONFIG_NET_DSA)
static void airoha_xfrm_dsa_attach_user(struct net_device *conduit,
					struct net_device *user)
{
	netdev_features_t features = airoha_xfrm_dev_features(conduit);

	if (conduit->xfrmdev_ops != &airoha_xfrmdev_ops ||
	    !airoha_xfrm_dsa_user_matches_port(user, conduit))
		return;

	if (!(features & NETIF_F_HW_ESP))
		return;

	if (user->xfrmdev_ops && user->xfrmdev_ops != &airoha_xfrmdev_ops) {
		netdev_dbg(conduit,
			   "DSA user %s already has XFRM offload ops\n",
			   user->name);
		return;
	}

	user->xfrmdev_ops = &airoha_xfrmdev_ops;
	user->hw_features |= features;
	user->hw_enc_features |= features;
	user->gso_partial_features |= features & NETIF_F_GSO_ESP;
	netdev_dbg(user, "ESP HW offload available via %s\n", conduit->name);
}

static void airoha_xfrm_dsa_detach_user(struct net_device *user)
{
	struct airoha_gdm_port *port;
	bool active = false;
	bool enabled;

	if (user->xfrmdev_ops != &airoha_xfrmdev_ops ||
	    !dsa_user_dev_check(user))
		return;

	enabled = user->features & NETIF_F_HW_ESP;
	port = airoha_xfrm_dsa_dev_port(user);
	if (port)
		active = atomic_read(&port->xfrm_state_count);

	if (active) {
		netdev_warn(user, "DSA detach with active ESP SAs, flushing\n");
		airoha_xfrm_flush_dev(user);
	}

	user->wanted_features &= ~AIROHA_XFRM_FEATURES;
	user->features &= ~AIROHA_XFRM_FEATURES;
	user->hw_features &= ~AIROHA_XFRM_FEATURES;
	user->hw_enc_features &= ~AIROHA_XFRM_FEATURES;
	user->gso_partial_features &= ~NETIF_F_GSO_ESP;
	user->xfrmdev_ops = NULL;

	if (active || enabled)
		netdev_features_change(user);
}

static void airoha_xfrm_dsa_feature_change(struct net_device *dev)
{
	struct airoha_gdm_port *port;

	if (dev->xfrmdev_ops != &airoha_xfrmdev_ops ||
	    !dsa_user_dev_check(dev) || (dev->features & NETIF_F_HW_ESP))
		return;

	port = airoha_xfrm_dsa_dev_port(dev);
	if (port && atomic_read(&port->xfrm_state_count)) {
		netdev_warn(dev, "DSA feature lost ESP SAs, flushing\n");
		airoha_xfrm_flush_dev(dev);
	}
}
#endif

static int airoha_xfrm_netdevice_event(struct notifier_block *nb,
				       unsigned long event, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);

	switch (event) {
	case NETDEV_CHANGE:
		airoha_xfrm_link_change(dev);
		break;
#if IS_ENABLED(CONFIG_NET_DSA)
	case NETDEV_CHANGEUPPER: {
		struct netdev_notifier_changeupper_info *info = ptr;

		if (info->linking)
			airoha_xfrm_dsa_attach_user(dev, info->upper_dev);
		else
			airoha_xfrm_dsa_detach_user(info->upper_dev);
		break;
	}
	case NETDEV_FEAT_CHANGE:
		airoha_xfrm_dsa_feature_change(dev);
		break;
	case NETDEV_UNREGISTER:
		airoha_xfrm_dsa_detach_user(dev);
		break;
#endif
	default:
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block airoha_xfrm_netdev_notifier = {
	.notifier_call = airoha_xfrm_netdevice_event,
};

static int airoha_xfrm_register_netdev_notifier(void)
{
	return register_netdevice_notifier(&airoha_xfrm_netdev_notifier);
}

static void airoha_xfrm_unregister_netdev_notifier(void)
{
	unregister_netdevice_notifier(&airoha_xfrm_netdev_notifier);
}

static void airoha_xfrm_drop_dev(struct net_device *dev, const char *reason)
{
	struct airoha_gdm_port *port = airoha_xfrm_dev_port(dev);
	bool advertised = dev->hw_features & AIROHA_XFRM_FEATURES;
	bool enabled = dev->features & NETIF_F_HW_ESP;
	bool active = false;

	if (port)
		active = atomic_read(&port->xfrm_state_count);

	if (active) {
		netdev_warn(dev, "%s, flushing ESP HW offload SAs\n", reason);
		airoha_xfrm_flush_dev(dev);
	}

	dev->wanted_features &= ~AIROHA_XFRM_FEATURES;
	dev->features &= ~AIROHA_XFRM_FEATURES;
	dev->hw_features &= ~AIROHA_XFRM_FEATURES;
	dev->hw_enc_features &= ~AIROHA_XFRM_FEATURES;
	dev->gso_partial_features &= ~NETIF_F_GSO_ESP;

	if (active || enabled || advertised)
		netdev_features_change(dev);
}

static void airoha_xfrm_drop_ipsec(struct eip93_ipsec *ipsec,
				   const char *reason)
{
	struct net_device *dev;
	struct net *net;

	rtnl_lock();
	for_each_net(net) {
		for_each_netdev(net, dev) {
			struct airoha_gdm_port *port;

			port = airoha_xfrm_dev_port(dev);
			if (!port || port->xfrm_ipsec != ipsec)
				continue;

			airoha_xfrm_drop_dev(dev, reason);
		}
	}

	for_each_net(net) {
		for_each_netdev(net, dev) {
			struct airoha_gdm_port *port;

			if (dev->xfrmdev_ops != &airoha_xfrmdev_ops)
				continue;

			if (airoha_xfrm_dsa_dev_port(dev))
				continue;

			port = netdev_priv(dev);
			if (dev == port->dev && port->xfrm_ipsec == ipsec) {
				eip93_ipsec_put(port->xfrm_ipsec);
				port->xfrm_ipsec = NULL;
			}
		}
	}

	for_each_net(net) {
		for_each_netdev(net, dev) {
			if (dev->xfrmdev_ops == &airoha_xfrmdev_ops &&
			    !(dev->hw_features & NETIF_F_HW_ESP))
				dev->xfrmdev_ops = NULL;
		}
	}
	rtnl_unlock();
}

static int airoha_xfrm_ipsec_event(struct notifier_block *nb,
				   unsigned long event, void *ptr)
{
	switch (event) {
	case EIP93_IPSEC_EVENT_REMOVE:
		airoha_xfrm_drop_ipsec(ptr, "EIP93 provider removed");
		break;
	case EIP93_IPSEC_EVENT_RESET:
		airoha_xfrm_drop_ipsec(ptr, "EIP93 provider reset");
		break;
	case EIP93_IPSEC_EVENT_DMA_ERROR:
		airoha_xfrm_drop_ipsec(ptr, "EIP93 DMA error");
		break;
	case EIP93_IPSEC_EVENT_CAPABILITY_LOSS:
		airoha_xfrm_drop_ipsec(ptr, "EIP93 capability loss");
		break;
	default:
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block airoha_xfrm_ipsec_notifier = {
	.notifier_call = airoha_xfrm_ipsec_event,
};

int airoha_xfrm_register_notifier(void)
{
	int err;

	err = airoha_xfrm_register_netdev_notifier();
	if (err)
		return err;

	err = eip93_ipsec_register_notifier(&airoha_xfrm_ipsec_notifier);
	if (err)
		airoha_xfrm_unregister_netdev_notifier();

	return err;
}

void airoha_xfrm_unregister_notifier(void)
{
	eip93_ipsec_unregister_notifier(&airoha_xfrm_ipsec_notifier);
	airoha_xfrm_unregister_netdev_notifier();
}
#else
void airoha_xfrm_build_netdev(struct net_device *dev)
{
}

void airoha_xfrm_teardown_netdev(struct net_device *dev)
{
}

netdev_features_t airoha_xfrm_fix_features(struct net_device *dev,
					   netdev_features_t features)
{
	return features & ~(NETIF_F_HW_ESP_TX_CSUM | NETIF_F_GSO_ESP);
}

int airoha_xfrm_set_features(struct net_device *dev, netdev_features_t features)
{
	return 0;
}

bool airoha_xfrm_rx_skb(struct airoha_gdm_port *port, struct sk_buff *skb)
{
	return false;
}

int airoha_xfrm_encrypt_skb(struct airoha_gdm_port *port, struct sk_buff *skb)
{
	return 0;
}

int airoha_xfrm_register_notifier(void)
{
	return 0;
}

void airoha_xfrm_unregister_notifier(void)
{
}

#endif
