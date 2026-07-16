// SPDX-License-Identifier: GPL-2.0-only
#include <linux/kernel.h>
#include <linux/skbuff.h>
#include <linux/export.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/if_vlan.h>
#include <linux/filter.h>
#include <net/dsa.h>
#include <net/dst_metadata.h>
#include <net/ip.h>
#include <net/ipv6.h>
#include <net/gre.h>
#include <net/pptp.h>
#include <net/tipc.h>
#include <linux/igmp.h>
#include <linux/icmp.h>
#include <linux/sctp.h>
#include <linux/dccp.h>
#include <linux/if_tunnel.h>
#include <linux/if_pppox.h>
#include <linux/ppp_defs.h>
#include <linux/stddef.h>
#include <linux/if_ether.h>
#include <linux/if_hsr.h>
#include <linux/mpls.h>
#include <linux/tcp.h>
#include <linux/ptp_classify.h>
#include <net/flow_dissector.h>
#include <net/pkt_cls.h>
#include <scsi/fc/fc_fcoe.h>
#include <uapi/linux/batadv_packet.h>
#include <linux/bpf.h>
#if IS_ENABLED(CONFIG_NF_CONNTRACK)
#include <net/netfilter/nf_conntrack_core.h>
#include <net/netfilter/nf_conntrack_labels.h>
#endif
#include <linux/bpf-netns.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/string_choices.h>
#include <linux/math64.h>

/* Per-shape fast-path gates, one static_branch per shape so operators
 * enable only what their deployment carries. All default off.
 */
DEFINE_STATIC_KEY_FALSE(flow_dissector_eth_ip_key);
EXPORT_SYMBOL(flow_dissector_eth_ip_key);
DEFINE_STATIC_KEY_FALSE(flow_dissector_vlan_key);
EXPORT_SYMBOL(flow_dissector_vlan_key);
DEFINE_STATIC_KEY_FALSE(flow_dissector_qinq_key);
EXPORT_SYMBOL(flow_dissector_qinq_key);
DEFINE_STATIC_KEY_FALSE(flow_dissector_pppoe_key);
EXPORT_SYMBOL(flow_dissector_pppoe_key);
DEFINE_STATIC_KEY_FALSE(flow_dissector_mpls_key);
EXPORT_SYMBOL(flow_dissector_mpls_key);
DEFINE_STATIC_KEY_FALSE(flow_dissector_ipip_key);
EXPORT_SYMBOL(flow_dissector_ipip_key);
DEFINE_STATIC_KEY_FALSE(flow_dissector_gre_key);
EXPORT_SYMBOL(flow_dissector_gre_key);

/* Per-cpu, summed on read via /proc/net/flow_dissector_stats.
 * occurrences[]: shape handled by the slow path (with the gate off,
 * the shape's eligible-fraction signal); fast_hits[]: fast body ran;
 * dissects: denominator.
 */
struct flow_dissector_stats {
	u64 occurrences[FLOW_DISSECTOR_SHAPE__MAX];
	u64 fast_hits[FLOW_DISSECTOR_SHAPE__MAX];
	u64 dissects;
};

static DEFINE_PER_CPU(struct flow_dissector_stats, flow_dissector_pcpu_stats);

static inline void flow_dissector_count_slow(enum flow_dissector_shape shape)
{
	this_cpu_inc(flow_dissector_pcpu_stats.occurrences[shape]);
}

static inline void flow_dissector_count_fast(enum flow_dissector_shape shape)
{
	this_cpu_inc(flow_dissector_pcpu_stats.fast_hits[shape]);
}

/* Counting map: eth_ip counts in the dispatcher (only when the leaf
 * did not set ENCAP), vlan/qinq/pppoe/mpls in their helpers, ipip/gre
 * in the inner-descent helpers. The slow path counts where it
 * recognises each shape, gate on or off.
 */

/* IPv4 version/IHL byte of an option-less header: version 4, IHL 5. */
#define FLOW_DIS_IPV4_VIHL_NOOPT	0x45

/* Fast-path helper forward declarations. */
static bool flow_dissect_fast_ipv4(const struct sk_buff *skb,
				   struct flow_dissector *flow_dissector,
				   void *target_container,
				   const void *data,
				   int nhoff, int hlen);
static bool flow_dissect_fast_ipv6(const struct sk_buff *skb,
				   struct flow_dissector *flow_dissector,
				   void *target_container,
				   const void *data,
				   int nhoff, int hlen);
static bool flow_dissect_fast_ipip_inner(const struct sk_buff *skb,
					 struct flow_dissector *flow_dissector,
					 void *target_container,
					 const void *data,
					 __be16 inner_eth_proto,
					 int inner_nhoff, int hlen);
static bool flow_dissect_fast_gre_inner(const struct sk_buff *skb,
					struct flow_dissector *flow_dissector,
					void *target_container,
					const void *data,
					int nhoff, int hlen);

/* One of the two dissectors the fast-path eligibility check admits;
 * defined here so flow_dissect_fast() below can reference it (its keys
 * and init live near the bottom of the file).
 */
static struct flow_dissector flow_keys_dissector_symmetric __read_mostly;

static void dissector_set_key(struct flow_dissector *flow_dissector,
			      enum flow_dissector_key_id key_id)
{
	flow_dissector->used_keys |= (1ULL << key_id);
}

void skb_flow_dissector_init(struct flow_dissector *flow_dissector,
			     const struct flow_dissector_key *key,
			     unsigned int key_count)
{
	unsigned int i;

	memset(flow_dissector, 0, sizeof(*flow_dissector));

	for (i = 0; i < key_count; i++, key++) {
		/* User should make sure that every key target offset is within
		 * boundaries of unsigned short.
		 */
		BUG_ON(key->offset > USHRT_MAX);
		BUG_ON(dissector_uses_key(flow_dissector,
					  key->key_id));

		dissector_set_key(flow_dissector, key->key_id);
		flow_dissector->offset[key->key_id] = key->offset;
	}

	/* Ensure that the dissector always includes control and basic key.
	 * That way we are able to avoid handling lack of these in fast path.
	 */
	BUG_ON(!dissector_uses_key(flow_dissector,
				   FLOW_DISSECTOR_KEY_CONTROL));
	BUG_ON(!dissector_uses_key(flow_dissector,
				   FLOW_DISSECTOR_KEY_BASIC));
}
EXPORT_SYMBOL(skb_flow_dissector_init);

/* Enabled while any netns has a BPF flow dissector program attached;
 * lets __skb_flow_dissect() skip the program lookup entirely in the
 * common no-program case. Maintained by the netns-BPF attach/detach
 * paths in kernel/bpf/net_namespace.c.
 */
DEFINE_STATIC_KEY_FALSE(netns_bpf_flow_dissector_enabled);
EXPORT_SYMBOL(netns_bpf_flow_dissector_enabled);

#ifdef CONFIG_BPF_SYSCALL
int flow_dissector_bpf_prog_attach_check(struct net *net,
					 struct bpf_prog *prog)
{
	enum netns_bpf_attach_type type = NETNS_BPF_FLOW_DISSECTOR;

	if (net == &init_net) {
		/* BPF flow dissector in the root namespace overrides
		 * any per-net-namespace one. When attaching to root,
		 * make sure we don't have any BPF program attached
		 * to the non-root namespaces.
		 */
		struct net *ns;

		for_each_net(ns) {
			if (ns == &init_net)
				continue;
			if (rcu_access_pointer(ns->bpf.run_array[type]))
				return -EEXIST;
		}
	} else {
		/* Make sure root flow dissector is not attached
		 * when attaching to the non-root namespace.
		 */
		if (rcu_access_pointer(init_net.bpf.run_array[type]))
			return -EEXIST;
	}

	return 0;
}
#endif /* CONFIG_BPF_SYSCALL */

/**
 * skb_flow_get_ports - extract the upper layer ports and return them
 * @skb: sk_buff to extract the ports from
 * @thoff: transport header offset
 * @ip_proto: protocol for which to get port offset
 * @data: raw buffer pointer to the packet, if NULL use skb->data
 * @hlen: packet header length, if @data is NULL use skb_headlen(skb)
 *
 * The function will try to retrieve the ports at offset thoff + poff where poff
 * is the protocol port offset returned from proto_ports_offset
 */
__be32 skb_flow_get_ports(const struct sk_buff *skb, int thoff, u8 ip_proto,
			  const void *data, int hlen)
{
	int poff = proto_ports_offset(ip_proto);

	if (!data) {
		data = skb->data;
		hlen = skb_headlen(skb);
	}

	if (poff >= 0) {
		__be32 *ports, _ports;

		ports = __skb_header_pointer(skb, thoff + poff,
					     sizeof(_ports), data, hlen, &_ports);
		if (ports)
			return *ports;
	}

	return 0;
}
EXPORT_SYMBOL(skb_flow_get_ports);

static bool icmp_has_id(u8 type)
{
	switch (type) {
	case ICMP_ECHO:
	case ICMP_ECHOREPLY:
	case ICMP_TIMESTAMP:
	case ICMP_TIMESTAMPREPLY:
	case ICMPV6_ECHO_REQUEST:
	case ICMPV6_ECHO_REPLY:
		return true;
	}

	return false;
}

/**
 * skb_flow_get_icmp_tci - extract ICMP(6) Type, Code and Identifier fields
 * @skb: sk_buff to extract from
 * @key_icmp: struct flow_dissector_key_icmp to fill
 * @data: raw buffer pointer to the packet
 * @thoff: offset to extract at
 * @hlen: packet header length
 */
void skb_flow_get_icmp_tci(const struct sk_buff *skb,
			   struct flow_dissector_key_icmp *key_icmp,
			   const void *data, int thoff, int hlen)
{
	struct icmphdr *ih, _ih;

	ih = __skb_header_pointer(skb, thoff, sizeof(_ih), data, hlen, &_ih);
	if (!ih)
		return;

	key_icmp->type = ih->type;
	key_icmp->code = ih->code;

	/* As we use 0 to signal that the Id field is not present,
	 * avoid confusion with packets without such field
	 */
	if (icmp_has_id(ih->type))
		key_icmp->id = ih->un.echo.id ? ntohs(ih->un.echo.id) : 1;
	else
		key_icmp->id = 0;
}
EXPORT_SYMBOL(skb_flow_get_icmp_tci);

/* If FLOW_DISSECTOR_KEY_ICMP is set, dissect an ICMP packet
 * using skb_flow_get_icmp_tci().
 */
static void __skb_flow_dissect_icmp(const struct sk_buff *skb,
				    struct flow_dissector *flow_dissector,
				    void *target_container, const void *data,
				    int thoff, int hlen)
{
	struct flow_dissector_key_icmp *key_icmp;

	if (!dissector_uses_key(flow_dissector, FLOW_DISSECTOR_KEY_ICMP))
		return;

	key_icmp = skb_flow_dissector_target(flow_dissector,
					     FLOW_DISSECTOR_KEY_ICMP,
					     target_container);

	skb_flow_get_icmp_tci(skb, key_icmp, data, thoff, hlen);
}

static void __skb_flow_dissect_ah(const struct sk_buff *skb,
				  struct flow_dissector *flow_dissector,
				  void *target_container, const void *data,
				  int nhoff, int hlen)
{
	struct flow_dissector_key_ipsec *key_ah;
	struct ip_auth_hdr _hdr, *hdr;

	if (!dissector_uses_key(flow_dissector, FLOW_DISSECTOR_KEY_IPSEC))
		return;

	hdr = __skb_header_pointer(skb, nhoff, sizeof(_hdr), data, hlen, &_hdr);
	if (!hdr)
		return;

	key_ah = skb_flow_dissector_target(flow_dissector,
					   FLOW_DISSECTOR_KEY_IPSEC,
					   target_container);

	key_ah->spi = hdr->spi;
}

static void __skb_flow_dissect_esp(const struct sk_buff *skb,
				   struct flow_dissector *flow_dissector,
				   void *target_container, const void *data,
				   int nhoff, int hlen)
{
	struct flow_dissector_key_ipsec *key_esp;
	struct ip_esp_hdr _hdr, *hdr;

	if (!dissector_uses_key(flow_dissector, FLOW_DISSECTOR_KEY_IPSEC))
		return;

	hdr = __skb_header_pointer(skb, nhoff, sizeof(_hdr), data, hlen, &_hdr);
	if (!hdr)
		return;

	key_esp = skb_flow_dissector_target(flow_dissector,
					    FLOW_DISSECTOR_KEY_IPSEC,
					    target_container);

	key_esp->spi = hdr->spi;
}

static void __skb_flow_dissect_l2tpv3(const struct sk_buff *skb,
				      struct flow_dissector *flow_dissector,
				      void *target_container, const void *data,
				      int nhoff, int hlen)
{
	struct flow_dissector_key_l2tpv3 *key_l2tpv3;
	struct {
		__be32 session_id;
	} *hdr, _hdr;

	if (!dissector_uses_key(flow_dissector, FLOW_DISSECTOR_KEY_L2TPV3))
		return;

	hdr = __skb_header_pointer(skb, nhoff, sizeof(_hdr), data, hlen, &_hdr);
	if (!hdr)
		return;

	key_l2tpv3 = skb_flow_dissector_target(flow_dissector,
					       FLOW_DISSECTOR_KEY_L2TPV3,
					       target_container);

	key_l2tpv3->session_id = hdr->session_id;
}

void skb_flow_dissect_meta(const struct sk_buff *skb,
			   struct flow_dissector *flow_dissector,
			   void *target_container)
{
	struct flow_dissector_key_meta *meta;

	if (!dissector_uses_key(flow_dissector, FLOW_DISSECTOR_KEY_META))
		return;

	meta = skb_flow_dissector_target(flow_dissector,
					 FLOW_DISSECTOR_KEY_META,
					 target_container);
	meta->ingress_ifindex = skb->skb_iif;
#if IS_ENABLED(CONFIG_NET_TC_SKB_EXT)
	if (tc_skb_ext_tc_enabled()) {
		struct tc_skb_ext *ext;

		ext = skb_ext_find(skb, TC_SKB_EXT);
		if (ext)
			meta->l2_miss = ext->l2_miss;
	}
#endif
}
EXPORT_SYMBOL(skb_flow_dissect_meta);

static void
skb_flow_dissect_set_enc_control(enum flow_dissector_key_id type,
				 u32 ctrl_flags,
				 struct flow_dissector *flow_dissector,
				 void *target_container)
{
	struct flow_dissector_key_control *ctrl;

	if (!dissector_uses_key(flow_dissector, FLOW_DISSECTOR_KEY_ENC_CONTROL))
		return;

	ctrl = skb_flow_dissector_target(flow_dissector,
					 FLOW_DISSECTOR_KEY_ENC_CONTROL,
					 target_container);
	ctrl->addr_type = type;
	ctrl->flags = ctrl_flags;
}

void
skb_flow_dissect_ct(const struct sk_buff *skb,
		    struct flow_dissector *flow_dissector,
		    void *target_container, u16 *ctinfo_map,
		    size_t mapsize, bool post_ct, u16 zone)
{
#if IS_ENABLED(CONFIG_NF_CONNTRACK)
	struct flow_dissector_key_ct *key;
	enum ip_conntrack_info ctinfo;
	struct nf_conn_labels *cl;
	struct nf_conn *ct;

	if (!dissector_uses_key(flow_dissector, FLOW_DISSECTOR_KEY_CT))
		return;

	ct = nf_ct_get(skb, &ctinfo);
	if (!ct && !post_ct)
		return;

	key = skb_flow_dissector_target(flow_dissector,
					FLOW_DISSECTOR_KEY_CT,
					target_container);

	if (!ct) {
		key->ct_state = TCA_FLOWER_KEY_CT_FLAGS_TRACKED |
				TCA_FLOWER_KEY_CT_FLAGS_INVALID;
		key->ct_zone = zone;
		return;
	}

	if (ctinfo < mapsize)
		key->ct_state = ctinfo_map[ctinfo];
#if IS_ENABLED(CONFIG_NF_CONNTRACK_ZONES)
	key->ct_zone = ct->zone.id;
#endif
#if IS_ENABLED(CONFIG_NF_CONNTRACK_MARK)
	key->ct_mark = READ_ONCE(ct->mark);
#endif

	cl = nf_ct_labels_find(ct);
	if (cl)
		memcpy(key->ct_labels, cl->bits, sizeof(key->ct_labels));
#endif /* CONFIG_NF_CONNTRACK */
}
EXPORT_SYMBOL(skb_flow_dissect_ct);

void
skb_flow_dissect_tunnel_info(const struct sk_buff *skb,
			     struct flow_dissector *flow_dissector,
			     void *target_container)
{
	struct ip_tunnel_info *info;
	struct ip_tunnel_key *key;
	u32 ctrl_flags = 0;

	/* A quick check to see if there might be something to do. */
	if (!dissector_uses_key(flow_dissector,
				FLOW_DISSECTOR_KEY_ENC_KEYID) &&
	    !dissector_uses_key(flow_dissector,
				FLOW_DISSECTOR_KEY_ENC_IPV4_ADDRS) &&
	    !dissector_uses_key(flow_dissector,
				FLOW_DISSECTOR_KEY_ENC_IPV6_ADDRS) &&
	    !dissector_uses_key(flow_dissector,
				FLOW_DISSECTOR_KEY_ENC_CONTROL) &&
	    !dissector_uses_key(flow_dissector,
				FLOW_DISSECTOR_KEY_ENC_PORTS) &&
	    !dissector_uses_key(flow_dissector,
				FLOW_DISSECTOR_KEY_ENC_IP) &&
	    !dissector_uses_key(flow_dissector,
				FLOW_DISSECTOR_KEY_ENC_OPTS))
		return;

	info = skb_tunnel_info(skb);
	if (!info)
		return;

	key = &info->key;

	if (test_bit(IP_TUNNEL_CSUM_BIT, key->tun_flags))
		ctrl_flags |= FLOW_DIS_F_TUNNEL_CSUM;
	if (test_bit(IP_TUNNEL_DONT_FRAGMENT_BIT, key->tun_flags))
		ctrl_flags |= FLOW_DIS_F_TUNNEL_DONT_FRAGMENT;
	if (test_bit(IP_TUNNEL_OAM_BIT, key->tun_flags))
		ctrl_flags |= FLOW_DIS_F_TUNNEL_OAM;
	if (test_bit(IP_TUNNEL_CRIT_OPT_BIT, key->tun_flags))
		ctrl_flags |= FLOW_DIS_F_TUNNEL_CRIT_OPT;

	switch (ip_tunnel_info_af(info)) {
	case AF_INET:
		skb_flow_dissect_set_enc_control(FLOW_DISSECTOR_KEY_IPV4_ADDRS,
						 ctrl_flags, flow_dissector,
						 target_container);
		if (dissector_uses_key(flow_dissector,
				       FLOW_DISSECTOR_KEY_ENC_IPV4_ADDRS)) {
			struct flow_dissector_key_ipv4_addrs *ipv4;

			ipv4 = skb_flow_dissector_target(flow_dissector,
							 FLOW_DISSECTOR_KEY_ENC_IPV4_ADDRS,
							 target_container);
			ipv4->src = key->u.ipv4.src;
			ipv4->dst = key->u.ipv4.dst;
		}
		break;
	case AF_INET6:
		skb_flow_dissect_set_enc_control(FLOW_DISSECTOR_KEY_IPV6_ADDRS,
						 ctrl_flags, flow_dissector,
						 target_container);
		if (dissector_uses_key(flow_dissector,
				       FLOW_DISSECTOR_KEY_ENC_IPV6_ADDRS)) {
			struct flow_dissector_key_ipv6_addrs *ipv6;

			ipv6 = skb_flow_dissector_target(flow_dissector,
							 FLOW_DISSECTOR_KEY_ENC_IPV6_ADDRS,
							 target_container);
			ipv6->src = key->u.ipv6.src;
			ipv6->dst = key->u.ipv6.dst;
		}
		break;
	default:
		skb_flow_dissect_set_enc_control(0, ctrl_flags, flow_dissector,
						 target_container);
		break;
	}

	if (dissector_uses_key(flow_dissector, FLOW_DISSECTOR_KEY_ENC_KEYID)) {
		struct flow_dissector_key_keyid *keyid;

		keyid = skb_flow_dissector_target(flow_dissector,
						  FLOW_DISSECTOR_KEY_ENC_KEYID,
						  target_container);
		keyid->keyid = tunnel_id_to_key32(key->tun_id);
	}

	if (dissector_uses_key(flow_dissector, FLOW_DISSECTOR_KEY_ENC_PORTS)) {
		struct flow_dissector_key_ports *tp;

		tp = skb_flow_dissector_target(flow_dissector,
					       FLOW_DISSECTOR_KEY_ENC_PORTS,
					       target_container);
		tp->src = key->tp_src;
		tp->dst = key->tp_dst;
	}

	if (dissector_uses_key(flow_dissector, FLOW_DISSECTOR_KEY_ENC_IP)) {
		struct flow_dissector_key_ip *ip;

		ip = skb_flow_dissector_target(flow_dissector,
					       FLOW_DISSECTOR_KEY_ENC_IP,
					       target_container);
		ip->tos = key->tos;
		ip->ttl = key->ttl;
	}

	if (dissector_uses_key(flow_dissector, FLOW_DISSECTOR_KEY_ENC_OPTS)) {
		struct flow_dissector_key_enc_opts *enc_opt;
		IP_TUNNEL_DECLARE_FLAGS(flags) = { };
		u32 val;

		enc_opt = skb_flow_dissector_target(flow_dissector,
						    FLOW_DISSECTOR_KEY_ENC_OPTS,
						    target_container);

		if (!info->options_len)
			return;

		enc_opt->len = info->options_len;
		ip_tunnel_info_opts_get(enc_opt->data, info);

		ip_tunnel_set_options_present(flags);
		ip_tunnel_flags_and(flags, info->key.tun_flags, flags);

		val = find_next_bit(flags, __IP_TUNNEL_FLAG_NUM,
				    IP_TUNNEL_GENEVE_OPT_BIT);
		enc_opt->dst_opt_type = val < __IP_TUNNEL_FLAG_NUM ? val : 0;
	}
}
EXPORT_SYMBOL(skb_flow_dissect_tunnel_info);

void skb_flow_dissect_hash(const struct sk_buff *skb,
			   struct flow_dissector *flow_dissector,
			   void *target_container)
{
	struct flow_dissector_key_hash *key;

	if (!dissector_uses_key(flow_dissector, FLOW_DISSECTOR_KEY_HASH))
		return;

	key = skb_flow_dissector_target(flow_dissector,
					FLOW_DISSECTOR_KEY_HASH,
					target_container);

	key->hash = skb_get_hash_raw(skb);
}
EXPORT_SYMBOL(skb_flow_dissect_hash);

static enum flow_dissect_ret
__skb_flow_dissect_mpls(const struct sk_buff *skb,
			struct flow_dissector *flow_dissector,
			void *target_container, const void *data, int nhoff,
			int hlen, int lse_index, bool *entropy_label)
{
	struct mpls_label *hdr, _hdr;
	u32 entry, label, bos;

	if (!dissector_uses_key(flow_dissector,
				FLOW_DISSECTOR_KEY_MPLS_ENTROPY) &&
	    !dissector_uses_key(flow_dissector, FLOW_DISSECTOR_KEY_MPLS))
		return FLOW_DISSECT_RET_OUT_GOOD;

	if (lse_index >= FLOW_DIS_MPLS_MAX)
		return FLOW_DISSECT_RET_OUT_GOOD;

	hdr = __skb_header_pointer(skb, nhoff, sizeof(_hdr), data,
				   hlen, &_hdr);
	if (!hdr)
		return FLOW_DISSECT_RET_OUT_BAD;

	entry = ntohl(hdr->entry);
	label = (entry & MPLS_LS_LABEL_MASK) >> MPLS_LS_LABEL_SHIFT;
	bos = (entry & MPLS_LS_S_MASK) >> MPLS_LS_S_SHIFT;

	if (dissector_uses_key(flow_dissector, FLOW_DISSECTOR_KEY_MPLS)) {
		struct flow_dissector_key_mpls *key_mpls;
		struct flow_dissector_mpls_lse *lse;

		key_mpls = skb_flow_dissector_target(flow_dissector,
						     FLOW_DISSECTOR_KEY_MPLS,
						     target_container);
		lse = &key_mpls->ls[lse_index];

		lse->mpls_ttl = (entry & MPLS_LS_TTL_MASK) >> MPLS_LS_TTL_SHIFT;
		lse->mpls_bos = bos;
		lse->mpls_tc = (entry & MPLS_LS_TC_MASK) >> MPLS_LS_TC_SHIFT;
		lse->mpls_label = label;
		dissector_set_mpls_lse(key_mpls, lse_index);
	}

	if (*entropy_label &&
	    dissector_uses_key(flow_dissector,
			       FLOW_DISSECTOR_KEY_MPLS_ENTROPY)) {
		struct flow_dissector_key_keyid *key_keyid;

		key_keyid = skb_flow_dissector_target(flow_dissector,
						      FLOW_DISSECTOR_KEY_MPLS_ENTROPY,
						      target_container);
		key_keyid->keyid = cpu_to_be32(label);
	}

	*entropy_label = label == MPLS_LABEL_ENTROPY;

	return bos ? FLOW_DISSECT_RET_OUT_GOOD : FLOW_DISSECT_RET_PROTO_AGAIN;
}

static enum flow_dissect_ret
__skb_flow_dissect_arp(const struct sk_buff *skb,
		       struct flow_dissector *flow_dissector,
		       void *target_container, const void *data,
		       int nhoff, int hlen)
{
	struct flow_dissector_key_arp *key_arp;
	struct {
		unsigned char ar_sha[ETH_ALEN];
		unsigned char ar_sip[4];
		unsigned char ar_tha[ETH_ALEN];
		unsigned char ar_tip[4];
	} *arp_eth, _arp_eth;
	const struct arphdr *arp;
	struct arphdr _arp;

	if (!dissector_uses_key(flow_dissector, FLOW_DISSECTOR_KEY_ARP))
		return FLOW_DISSECT_RET_OUT_GOOD;

	arp = __skb_header_pointer(skb, nhoff, sizeof(_arp), data,
				   hlen, &_arp);
	if (!arp)
		return FLOW_DISSECT_RET_OUT_BAD;

	if (arp->ar_hrd != htons(ARPHRD_ETHER) ||
	    arp->ar_pro != htons(ETH_P_IP) ||
	    arp->ar_hln != ETH_ALEN ||
	    arp->ar_pln != 4 ||
	    (arp->ar_op != htons(ARPOP_REPLY) &&
	     arp->ar_op != htons(ARPOP_REQUEST)))
		return FLOW_DISSECT_RET_OUT_BAD;

	arp_eth = __skb_header_pointer(skb, nhoff + sizeof(_arp),
				       sizeof(_arp_eth), data,
				       hlen, &_arp_eth);
	if (!arp_eth)
		return FLOW_DISSECT_RET_OUT_BAD;

	key_arp = skb_flow_dissector_target(flow_dissector,
					    FLOW_DISSECTOR_KEY_ARP,
					    target_container);

	memcpy(&key_arp->sip, arp_eth->ar_sip, sizeof(key_arp->sip));
	memcpy(&key_arp->tip, arp_eth->ar_tip, sizeof(key_arp->tip));

	/* Only store the lower byte of the opcode;
	 * this covers ARPOP_REPLY and ARPOP_REQUEST.
	 */
	key_arp->op = ntohs(arp->ar_op) & 0xff;

	ether_addr_copy(key_arp->sha, arp_eth->ar_sha);
	ether_addr_copy(key_arp->tha, arp_eth->ar_tha);

	return FLOW_DISSECT_RET_OUT_GOOD;
}

static enum flow_dissect_ret
__skb_flow_dissect_cfm(const struct sk_buff *skb,
		       struct flow_dissector *flow_dissector,
		       void *target_container, const void *data,
		       int nhoff, int hlen)
{
	struct flow_dissector_key_cfm *key, *hdr, _hdr;

	if (!dissector_uses_key(flow_dissector, FLOW_DISSECTOR_KEY_CFM))
		return FLOW_DISSECT_RET_OUT_GOOD;

	hdr = __skb_header_pointer(skb, nhoff, sizeof(*key), data, hlen, &_hdr);
	if (!hdr)
		return FLOW_DISSECT_RET_OUT_BAD;

	key = skb_flow_dissector_target(flow_dissector, FLOW_DISSECTOR_KEY_CFM,
					target_container);

	key->mdl_ver = hdr->mdl_ver;
	key->opcode = hdr->opcode;

	return FLOW_DISSECT_RET_OUT_GOOD;
}

static enum flow_dissect_ret
__skb_flow_dissect_gre(const struct sk_buff *skb,
		       struct flow_dissector_key_control *key_control,
		       struct flow_dissector *flow_dissector,
		       void *target_container, const void *data,
		       __be16 *p_proto, int *p_nhoff, int *p_hlen,
		       unsigned int flags)
{
	struct flow_dissector_key_keyid *key_keyid;
	struct gre_base_hdr *hdr, _hdr;
	int offset = 0;
	u16 gre_ver;

	hdr = __skb_header_pointer(skb, *p_nhoff, sizeof(_hdr),
				   data, *p_hlen, &_hdr);
	if (!hdr)
		return FLOW_DISSECT_RET_OUT_BAD;

	/* Only look inside GRE without routing */
	if (hdr->flags & GRE_ROUTING)
		return FLOW_DISSECT_RET_OUT_GOOD;

	/* Only look inside GRE for version 0 and 1 */
	gre_ver = ntohs(hdr->flags & GRE_VERSION);
	if (gre_ver > 1)
		return FLOW_DISSECT_RET_OUT_GOOD;

	*p_proto = hdr->protocol;
	if (gre_ver) {
		/* Version1 must be PPTP, and check the flags */
		if (!(*p_proto == GRE_PROTO_PPP && (hdr->flags & GRE_KEY)))
			return FLOW_DISSECT_RET_OUT_GOOD;
	}

	offset += sizeof(struct gre_base_hdr);

	if (hdr->flags & GRE_CSUM)
		offset += sizeof_field(struct gre_full_hdr, csum) +
			  sizeof_field(struct gre_full_hdr, reserved1);

	if (hdr->flags & GRE_KEY) {
		const __be32 *keyid;
		__be32 _keyid;

		keyid = __skb_header_pointer(skb, *p_nhoff + offset,
					     sizeof(_keyid),
					     data, *p_hlen, &_keyid);
		if (!keyid)
			return FLOW_DISSECT_RET_OUT_BAD;

		if (dissector_uses_key(flow_dissector,
				       FLOW_DISSECTOR_KEY_GRE_KEYID)) {
			key_keyid = skb_flow_dissector_target(flow_dissector,
							      FLOW_DISSECTOR_KEY_GRE_KEYID,
							      target_container);
			if (gre_ver == 0)
				key_keyid->keyid = *keyid;
			else
				key_keyid->keyid = *keyid & GRE_PPTP_KEY_MASK;
		}
		offset += sizeof_field(struct gre_full_hdr, key);
	}

	if (hdr->flags & GRE_SEQ)
		offset += sizeof_field(struct pptp_gre_header, seq);

	if (gre_ver == 0) {
		if (*p_proto == htons(ETH_P_TEB)) {
			const struct ethhdr *eth;
			struct ethhdr _eth;

			eth = __skb_header_pointer(skb, *p_nhoff + offset,
						   sizeof(_eth),
						   data, *p_hlen, &_eth);
			if (!eth)
				return FLOW_DISSECT_RET_OUT_BAD;
			*p_proto = eth->h_proto;
			offset += sizeof(*eth);

			/* Cap headers that we access via pointers at the
			 * end of the Ethernet header as our maximum alignment
			 * at that point is only 2 bytes.
			 */
			if (NET_IP_ALIGN)
				*p_hlen = *p_nhoff + offset;
		}
	} else { /* version 1, must be PPTP */
		u8 _ppp_hdr[PPP_HDRLEN];
		u8 *ppp_hdr;

		if (hdr->flags & GRE_ACK)
			offset += sizeof_field(struct pptp_gre_header, ack);

		ppp_hdr = __skb_header_pointer(skb, *p_nhoff + offset,
					       sizeof(_ppp_hdr),
					       data, *p_hlen, _ppp_hdr);
		if (!ppp_hdr)
			return FLOW_DISSECT_RET_OUT_BAD;

		switch (PPP_PROTOCOL(ppp_hdr)) {
		case PPP_IP:
			*p_proto = htons(ETH_P_IP);
			break;
		case PPP_IPV6:
			*p_proto = htons(ETH_P_IPV6);
			break;
		default:
			/* Could probably catch some more like MPLS */
			break;
		}

		offset += PPP_HDRLEN;
	}

	*p_nhoff += offset;
	key_control->flags |= FLOW_DIS_ENCAPSULATION;
	if (flags & FLOW_DISSECTOR_F_STOP_AT_ENCAP)
		return FLOW_DISSECT_RET_OUT_GOOD;

	return FLOW_DISSECT_RET_PROTO_AGAIN;
}

/**
 * __skb_flow_dissect_batadv() - dissect batman-adv header
 * @skb: sk_buff to with the batman-adv header
 * @key_control: flow dissectors control key
 * @data: raw buffer pointer to the packet, if NULL use skb->data
 * @p_proto: pointer used to update the protocol to process next
 * @p_nhoff: pointer used to update inner network header offset
 * @hlen: packet header length
 * @flags: any combination of FLOW_DISSECTOR_F_*
 *
 * ETH_P_BATMAN packets are tried to be dissected. Only
 * &struct batadv_unicast packets are actually processed because they contain an
 * inner ethernet header and are usually followed by actual network header. This
 * allows the flow dissector to continue processing the packet.
 *
 * Return: FLOW_DISSECT_RET_PROTO_AGAIN when &struct batadv_unicast was found,
 *  FLOW_DISSECT_RET_OUT_GOOD when dissector should stop after encapsulation,
 *  otherwise FLOW_DISSECT_RET_OUT_BAD
 */
static enum flow_dissect_ret
__skb_flow_dissect_batadv(const struct sk_buff *skb,
			  struct flow_dissector_key_control *key_control,
			  const void *data, __be16 *p_proto, int *p_nhoff,
			  int hlen, unsigned int flags)
{
	struct {
		struct batadv_unicast_packet batadv_unicast;
		struct ethhdr eth;
	} *hdr, _hdr;

	hdr = __skb_header_pointer(skb, *p_nhoff, sizeof(_hdr), data, hlen,
				   &_hdr);
	if (!hdr)
		return FLOW_DISSECT_RET_OUT_BAD;

	if (hdr->batadv_unicast.version != BATADV_COMPAT_VERSION)
		return FLOW_DISSECT_RET_OUT_BAD;

	if (hdr->batadv_unicast.packet_type != BATADV_UNICAST)
		return FLOW_DISSECT_RET_OUT_BAD;

	*p_proto = hdr->eth.h_proto;
	*p_nhoff += sizeof(*hdr);

	key_control->flags |= FLOW_DIS_ENCAPSULATION;
	if (flags & FLOW_DISSECTOR_F_STOP_AT_ENCAP)
		return FLOW_DISSECT_RET_OUT_GOOD;

	return FLOW_DISSECT_RET_PROTO_AGAIN;
}

static void
__skb_flow_dissect_tcp(const struct sk_buff *skb,
		       struct flow_dissector *flow_dissector,
		       void *target_container, const void *data,
		       int thoff, int hlen)
{
	struct flow_dissector_key_tcp *key_tcp;
	struct tcphdr *th, _th;

	if (!dissector_uses_key(flow_dissector, FLOW_DISSECTOR_KEY_TCP))
		return;

	th = __skb_header_pointer(skb, thoff, sizeof(_th), data, hlen, &_th);
	if (!th)
		return;

	if (unlikely(__tcp_hdrlen(th) < sizeof(_th)))
		return;

	key_tcp = skb_flow_dissector_target(flow_dissector,
					    FLOW_DISSECTOR_KEY_TCP,
					    target_container);
	key_tcp->flags = (*(__be16 *) &tcp_flag_word(th) & htons(0x0FFF));
}

static void
__skb_flow_dissect_ports(const struct sk_buff *skb,
			 struct flow_dissector *flow_dissector,
			 void *target_container, const void *data,
			 int nhoff, u8 ip_proto, int hlen)
{
	struct flow_dissector_key_ports_range *key_ports_range = NULL;
	struct flow_dissector_key_ports *key_ports = NULL;
	__be32 ports;

	if (dissector_uses_key(flow_dissector, FLOW_DISSECTOR_KEY_PORTS))
		key_ports = skb_flow_dissector_target(flow_dissector,
						      FLOW_DISSECTOR_KEY_PORTS,
						      target_container);

	if (dissector_uses_key(flow_dissector, FLOW_DISSECTOR_KEY_PORTS_RANGE))
		key_ports_range = skb_flow_dissector_target(flow_dissector,
							    FLOW_DISSECTOR_KEY_PORTS_RANGE,
							    target_container);

	if (!key_ports && !key_ports_range)
		return;

	ports = skb_flow_get_ports(skb, nhoff, ip_proto, data, hlen);

	if (key_ports)
		key_ports->ports = ports;

	if (key_ports_range)
		key_ports_range->tp.ports = ports;
}

static void
__skb_flow_dissect_ipv4(const struct sk_buff *skb,
			struct flow_dissector *flow_dissector,
			void *target_container, const void *data,
			const struct iphdr *iph)
{
	struct flow_dissector_key_ip *key_ip;

	if (!dissector_uses_key(flow_dissector, FLOW_DISSECTOR_KEY_IP))
		return;

	key_ip = skb_flow_dissector_target(flow_dissector,
					   FLOW_DISSECTOR_KEY_IP,
					   target_container);
	key_ip->tos = iph->tos;
	key_ip->ttl = iph->ttl;
}

static void
__skb_flow_dissect_ipv6(const struct sk_buff *skb,
			struct flow_dissector *flow_dissector,
			void *target_container, const void *data,
			const struct ipv6hdr *iph)
{
	struct flow_dissector_key_ip *key_ip;

	if (!dissector_uses_key(flow_dissector, FLOW_DISSECTOR_KEY_IP))
		return;

	key_ip = skb_flow_dissector_target(flow_dissector,
					   FLOW_DISSECTOR_KEY_IP,
					   target_container);
	key_ip->tos = ipv6_get_dsfield(iph);
	key_ip->ttl = iph->hop_limit;
}

/* Maximum number of protocol headers that can be parsed in
 * __skb_flow_dissect
 */
#define MAX_FLOW_DISSECT_HDRS	15

static bool skb_flow_dissect_allowed(int *num_hdrs)
{
	++*num_hdrs;

	return (*num_hdrs <= MAX_FLOW_DISSECT_HDRS);
}

static void __skb_flow_bpf_to_target(const struct bpf_flow_keys *flow_keys,
				     struct flow_dissector *flow_dissector,
				     void *target_container)
{
	struct flow_dissector_key_ports_range *key_ports_range = NULL;
	struct flow_dissector_key_ports *key_ports = NULL;
	struct flow_dissector_key_control *key_control;
	struct flow_dissector_key_basic *key_basic;
	struct flow_dissector_key_addrs *key_addrs;
	struct flow_dissector_key_tags *key_tags;

	key_control = skb_flow_dissector_target(flow_dissector,
						FLOW_DISSECTOR_KEY_CONTROL,
						target_container);
	key_control->thoff = flow_keys->thoff;
	if (flow_keys->is_frag)
		key_control->flags |= FLOW_DIS_IS_FRAGMENT;
	if (flow_keys->is_first_frag)
		key_control->flags |= FLOW_DIS_FIRST_FRAG;
	if (flow_keys->is_encap)
		key_control->flags |= FLOW_DIS_ENCAPSULATION;

	key_basic = skb_flow_dissector_target(flow_dissector,
					      FLOW_DISSECTOR_KEY_BASIC,
					      target_container);
	key_basic->n_proto = flow_keys->n_proto;
	key_basic->ip_proto = flow_keys->ip_proto;

	if (flow_keys->addr_proto == ETH_P_IP &&
	    dissector_uses_key(flow_dissector, FLOW_DISSECTOR_KEY_IPV4_ADDRS)) {
		key_addrs = skb_flow_dissector_target(flow_dissector,
						      FLOW_DISSECTOR_KEY_IPV4_ADDRS,
						      target_container);
		key_addrs->v4addrs.src = flow_keys->ipv4_src;
		key_addrs->v4addrs.dst = flow_keys->ipv4_dst;
		key_control->addr_type = FLOW_DISSECTOR_KEY_IPV4_ADDRS;
	} else if (flow_keys->addr_proto == ETH_P_IPV6 &&
		   dissector_uses_key(flow_dissector,
				      FLOW_DISSECTOR_KEY_IPV6_ADDRS)) {
		key_addrs = skb_flow_dissector_target(flow_dissector,
						      FLOW_DISSECTOR_KEY_IPV6_ADDRS,
						      target_container);
		memcpy(&key_addrs->v6addrs.src, &flow_keys->ipv6_src,
		       sizeof(key_addrs->v6addrs.src));
		memcpy(&key_addrs->v6addrs.dst, &flow_keys->ipv6_dst,
		       sizeof(key_addrs->v6addrs.dst));
		key_control->addr_type = FLOW_DISSECTOR_KEY_IPV6_ADDRS;
	}

	if (dissector_uses_key(flow_dissector, FLOW_DISSECTOR_KEY_PORTS)) {
		key_ports = skb_flow_dissector_target(flow_dissector,
						      FLOW_DISSECTOR_KEY_PORTS,
						      target_container);
		key_ports->src = flow_keys->sport;
		key_ports->dst = flow_keys->dport;
	}
	if (dissector_uses_key(flow_dissector,
			       FLOW_DISSECTOR_KEY_PORTS_RANGE)) {
		key_ports_range = skb_flow_dissector_target(flow_dissector,
							    FLOW_DISSECTOR_KEY_PORTS_RANGE,
							    target_container);
		key_ports_range->tp.src = flow_keys->sport;
		key_ports_range->tp.dst = flow_keys->dport;
	}

	if (dissector_uses_key(flow_dissector,
			       FLOW_DISSECTOR_KEY_FLOW_LABEL)) {
		key_tags = skb_flow_dissector_target(flow_dissector,
						     FLOW_DISSECTOR_KEY_FLOW_LABEL,
						     target_container);
		key_tags->flow_label = ntohl(flow_keys->flow_label);
	}
}

u32 bpf_flow_dissect(struct bpf_prog *prog, struct bpf_flow_dissector *ctx,
		     __be16 proto, int nhoff, int hlen, unsigned int flags)
{
	struct bpf_flow_keys *flow_keys = ctx->flow_keys;
	u32 result;

	/* Pass parameters to the BPF program */
	memset(flow_keys, 0, sizeof(*flow_keys));
	flow_keys->n_proto = proto;
	flow_keys->nhoff = nhoff;
	flow_keys->thoff = flow_keys->nhoff;

	BUILD_BUG_ON((int)BPF_FLOW_DISSECTOR_F_PARSE_1ST_FRAG !=
		     (int)FLOW_DISSECTOR_F_PARSE_1ST_FRAG);
	BUILD_BUG_ON((int)BPF_FLOW_DISSECTOR_F_STOP_AT_FLOW_LABEL !=
		     (int)FLOW_DISSECTOR_F_STOP_AT_FLOW_LABEL);
	BUILD_BUG_ON((int)BPF_FLOW_DISSECTOR_F_STOP_AT_ENCAP !=
		     (int)FLOW_DISSECTOR_F_STOP_AT_ENCAP);
	flow_keys->flags = flags;

	result = bpf_prog_run_pin_on_cpu(prog, ctx);

	flow_keys->nhoff = clamp_t(u16, flow_keys->nhoff, nhoff, hlen);
	flow_keys->thoff = clamp_t(u16, flow_keys->thoff,
				   flow_keys->nhoff, hlen);

	return result;
}

static bool is_pppoe_ses_hdr_valid(const struct pppoe_hdr *hdr)
{
	return hdr->ver == 1 && hdr->type == 1 && hdr->code == 0;
}

/* The IPv4 fast-path assumes a 20-byte IPv4 header (IHL == 5). The
 * runtime IHL check below is the dynamic invariant; this static
 * assertion is the compile-time one, in the unlikely case struct
 * iphdr ever grows.
 */
static_assert(sizeof(struct iphdr) == 20);

/* Straight-line eth + IPv4 (IHL=5, not fragmented) + TCP|UDP. Returns
 * true when @target_container has been filled byte-identically to the
 * slow path; false on any miss (caller falls back to the slow path).
 */
static bool flow_dissect_fast_ipv4(const struct sk_buff *skb,
				   struct flow_dissector *flow_dissector,
				   void *target_container,
				   const void *data,
				   int nhoff, int hlen)
{
	struct flow_dissector_key_control *key_control;
	struct flow_dissector_key_addrs *key_addrs;
	struct flow_dissector_key_basic *key_basic;
	struct flow_dissector_key_ports *key_ports;
	const struct iphdr *iph;
	int thoff;

	if (unlikely(hlen - nhoff < (int)sizeof(*iph) + 4))
		return false;

	iph = (const struct iphdr *)((const u8 *)data + nhoff);

	if (unlikely(*(const u8 *)iph != FLOW_DIS_IPV4_VIHL_NOOPT))
		return false;

	if (unlikely(iph->frag_off & htons(IP_MF | IP_OFFSET)))
		return false;

	if (unlikely(iph->protocol != IPPROTO_TCP &&
		     iph->protocol != IPPROTO_UDP)) {
		/* IPIP / IPv6-in-IPv4 outer: defer to the inner fast-path and
		 * stamp ENCAP. Outer addrs are not written -- the inner pass
		 * overwrites them, as in the slow path.
		 */
		if (static_branch_unlikely(&flow_dissector_ipip_key)) {
			if (iph->protocol == IPPROTO_IPIP)
				return flow_dissect_fast_ipip_inner(skb,
						flow_dissector, target_container,
						data, htons(ETH_P_IP),
						nhoff + (int)sizeof(*iph), hlen);
			if (iph->protocol == IPPROTO_IPV6)
				return flow_dissect_fast_ipip_inner(skb,
						flow_dissector, target_container,
						data, htons(ETH_P_IPV6),
						nhoff + (int)sizeof(*iph), hlen);
		}
		if (static_branch_unlikely(&flow_dissector_gre_key) &&
		    iph->protocol == IPPROTO_GRE)
			return flow_dissect_fast_gre_inner(skb, flow_dissector,
					target_container, data,
					nhoff + (int)sizeof(*iph), hlen);
		return false;
	}

	thoff = nhoff + (int)sizeof(*iph);

	if (dissector_uses_key(flow_dissector,
			       FLOW_DISSECTOR_KEY_CONTROL)) {
		key_control = skb_flow_dissector_target(flow_dissector,
							FLOW_DISSECTOR_KEY_CONTROL,
							target_container);
		key_control->addr_type = FLOW_DISSECTOR_KEY_IPV4_ADDRS;
		key_control->thoff = min_t(u16, thoff,
					   skb ? skb->len : hlen);
		key_control->flags = 0;
	}
	if (dissector_uses_key(flow_dissector,
			       FLOW_DISSECTOR_KEY_BASIC)) {
		key_basic = skb_flow_dissector_target(flow_dissector,
						      FLOW_DISSECTOR_KEY_BASIC,
						      target_container);
		key_basic->n_proto = htons(ETH_P_IP);
		key_basic->ip_proto = iph->protocol;
	}
	if (dissector_uses_key(flow_dissector,
			       FLOW_DISSECTOR_KEY_IPV4_ADDRS)) {
		key_addrs = skb_flow_dissector_target(flow_dissector,
						      FLOW_DISSECTOR_KEY_IPV4_ADDRS,
						      target_container);
		memcpy(&key_addrs->v4addrs.src, &iph->saddr,
		       sizeof(key_addrs->v4addrs.src));
		memcpy(&key_addrs->v4addrs.dst, &iph->daddr,
		       sizeof(key_addrs->v4addrs.dst));
	}
	if (dissector_uses_key(flow_dissector,
			       FLOW_DISSECTOR_KEY_PORTS)) {
		const __be32 *ports = (const __be32 *)
			((const u8 *)data + thoff);

		key_ports = skb_flow_dissector_target(flow_dissector,
						      FLOW_DISSECTOR_KEY_PORTS,
						      target_container);
		key_ports->ports = *ports;
	}

	return true;
}

/* Same as flow_dissect_fast_ipv4 but for a fixed 40-byte IPv6 header
 * (no extension headers). The nexthdr check below holds the run-time
 * invariant; the static_assert holds the compile-time one.
 */
static_assert(sizeof(struct ipv6hdr) == 40);

static bool flow_dissect_fast_ipv6(const struct sk_buff *skb,
				   struct flow_dissector *flow_dissector,
				   void *target_container,
				   const void *data,
				   int nhoff, int hlen)
{
	struct flow_dissector_key_control *key_control;
	struct flow_dissector_key_addrs *key_addrs;
	struct flow_dissector_key_basic *key_basic;
	struct flow_dissector_key_ports *key_ports;
	const struct ipv6hdr *iph;
	int thoff;

	if (unlikely(hlen - nhoff < (int)sizeof(*iph) + 4))
		return false;

	iph = (const struct ipv6hdr *)((const u8 *)data + nhoff);

	if (unlikely((*(const u8 *)iph >> 4) != 6))
		return false;

	/* Any non-zero flow label defers, checked before everything else
	 * (including the tunnel descents below). Callers passing
	 * FLOW_DISSECTOR_F_STOP_AT_FLOW_LABEL (skb_get_hash() -- so
	 * RPS/RFS, fq, cake) make the slow path stop at a non-zero label
	 * even on a tunnel outer, and dissectors requesting the label key
	 * write it; both diverge from the plain parse. Deferring on the
	 * label itself covers every combination without threading flags
	 * into the helpers, and zero-label IPv6 (the overwhelmingly
	 * common case) stays on the fast path.
	 */
	if (unlikely((iph->flow_lbl[0] & 0x0f) |
		     iph->flow_lbl[1] | iph->flow_lbl[2]))
		return false;

	if (unlikely(iph->nexthdr != IPPROTO_TCP &&
		     iph->nexthdr != IPPROTO_UDP)) {
		/* IPIP / IPV6-in-IPv6 tunnel outer: same pattern as the
		 * IPv4 helper. The nexthdr_IPIP / nexthdr_IPV6 cases
		 * defer to the inner IP fast-path and stamp ENCAP.
		 */
		bool ipip = static_branch_unlikely(&flow_dissector_ipip_key) &&
			    (iph->nexthdr == IPPROTO_IPIP ||
			     iph->nexthdr == IPPROTO_IPV6);
		bool gre = static_branch_unlikely(&flow_dissector_gre_key) &&
			   iph->nexthdr == IPPROTO_GRE;

		if (!ipip && !gre)
			return false;

		/* Mirror the slow path's outer-IPv6 writes before the
		 * descent. The slow path fills v6addrs for the outer
		 * header and the inner pass then overwrites only what
		 * it uses — an inner IPv4 leaves the tail of the addrs
		 * union holding outer-v6 bytes. Byte-identical means
		 * reproducing exactly that, residue included. (The
		 * outer flow label is always zero here — a non-zero
		 * label deferred above — so there is no label residue.)
		 */
		if (dissector_uses_key(flow_dissector,
				       FLOW_DISSECTOR_KEY_IPV6_ADDRS)) {
			key_addrs = skb_flow_dissector_target(flow_dissector,
							      FLOW_DISSECTOR_KEY_IPV6_ADDRS,
							      target_container);
			memcpy(&key_addrs->v6addrs.src, &iph->saddr,
			       sizeof(key_addrs->v6addrs.src));
			memcpy(&key_addrs->v6addrs.dst, &iph->daddr,
			       sizeof(key_addrs->v6addrs.dst));
			key_control = skb_flow_dissector_target(flow_dissector,
								FLOW_DISSECTOR_KEY_CONTROL,
								target_container);
			key_control->addr_type = FLOW_DISSECTOR_KEY_IPV6_ADDRS;
		}

		if (ipip)
			return flow_dissect_fast_ipip_inner(skb,
					flow_dissector, target_container,
					data,
					iph->nexthdr == IPPROTO_IPIP ?
					htons(ETH_P_IP) : htons(ETH_P_IPV6),
					nhoff + (int)sizeof(*iph), hlen);
		return flow_dissect_fast_gre_inner(skb, flow_dissector,
				target_container, data,
				nhoff + (int)sizeof(*iph), hlen);
	}

	thoff = nhoff + (int)sizeof(*iph);

	if (dissector_uses_key(flow_dissector,
			       FLOW_DISSECTOR_KEY_CONTROL)) {
		key_control = skb_flow_dissector_target(flow_dissector,
							FLOW_DISSECTOR_KEY_CONTROL,
							target_container);
		key_control->addr_type = FLOW_DISSECTOR_KEY_IPV6_ADDRS;
		key_control->thoff = min_t(u16, thoff,
					   skb ? skb->len : hlen);
		key_control->flags = 0;
	}
	if (dissector_uses_key(flow_dissector,
			       FLOW_DISSECTOR_KEY_BASIC)) {
		key_basic = skb_flow_dissector_target(flow_dissector,
						      FLOW_DISSECTOR_KEY_BASIC,
						      target_container);
		key_basic->n_proto = htons(ETH_P_IPV6);
		key_basic->ip_proto = iph->nexthdr;
	}
	if (dissector_uses_key(flow_dissector,
			       FLOW_DISSECTOR_KEY_IPV6_ADDRS)) {
		key_addrs = skb_flow_dissector_target(flow_dissector,
						      FLOW_DISSECTOR_KEY_IPV6_ADDRS,
						      target_container);
		memcpy(&key_addrs->v6addrs.src, &iph->saddr,
		       sizeof(key_addrs->v6addrs.src));
		memcpy(&key_addrs->v6addrs.dst, &iph->daddr,
		       sizeof(key_addrs->v6addrs.dst));
	}
	if (dissector_uses_key(flow_dissector,
			       FLOW_DISSECTOR_KEY_PORTS)) {
		const __be32 *ports = (const __be32 *)
			((const u8 *)data + thoff);

		key_ports = skb_flow_dissector_target(flow_dissector,
						      FLOW_DISSECTOR_KEY_PORTS,
						      target_container);
		key_ports->ports = *ports;
	}

	return true;
}

/* Strip up to two 802.1Q/802.1AD tags: outer tag ->
 * FLOW_DISSECTOR_KEY_VLAN, inner -> _CVLAN, mirroring the slow path's
 * MAX -> VLAN -> CVLAN state machine; more than two tags defer.
 * Depth 0 is gated by the vlan key, depth >= 1 by the qinq key (the
 * qinq sysctl also flips vlan on -- see the proc_handlers).
 */
static bool flow_dissect_fast_vlan(const struct sk_buff *skb,
				   struct flow_dissector *flow_dissector,
				   void *target_container,
				   const void *data,
				   __be16 proto, int nhoff, int hlen,
				   int vlan_depth)
{
	struct flow_dissector_key_num_of_vlans *key_nvs;
	struct flow_dissector_key_vlan *key_vlan;
	enum flow_dissector_key_id vlan_key;
	__be16 inner_proto;
	__be16 saved_tpid;
	u16 tci_prio;
	u16 tci_id;
	bool ok;

	if (vlan_depth >= 1 &&
	    !static_branch_unlikely(&flow_dissector_qinq_key))
		return false;
	if (vlan_depth >= 2)
		return false;

	vlan_key = vlan_depth == 0 ?
		   FLOW_DISSECTOR_KEY_VLAN :
		   FLOW_DISSECTOR_KEY_CVLAN;
	saved_tpid = proto;

	/* HW-stripped tag is only ever the outermost. */
	if (vlan_depth == 0 && skb && skb_vlan_tag_present(skb)) {
		tci_id = skb_vlan_tag_get_id(skb);
		tci_prio = skb_vlan_tag_get_prio(skb);
		inner_proto = skb->protocol;
	} else {
		const struct vlan_hdr *vlan;

		if (unlikely(hlen - nhoff < (int)sizeof(*vlan)))
			return false;
		vlan = (const struct vlan_hdr *)((const u8 *)data + nhoff);
		tci_id = ntohs(vlan->h_vlan_TCI) & VLAN_VID_MASK;
		tci_prio = (ntohs(vlan->h_vlan_TCI) &
			    VLAN_PRIO_MASK) >> VLAN_PRIO_SHIFT;
		inner_proto = vlan->h_vlan_encapsulated_proto;
		nhoff += (int)sizeof(*vlan);
	}

	if (dissector_uses_key(flow_dissector, vlan_key)) {
		key_vlan = skb_flow_dissector_target(flow_dissector,
						     vlan_key,
						     target_container);
		key_vlan->vlan_id = tci_id;
		key_vlan->vlan_priority = tci_prio;
		key_vlan->vlan_tpid = saved_tpid;
		key_vlan->vlan_eth_type = inner_proto;
	}
	if (dissector_uses_key(flow_dissector,
			       FLOW_DISSECTOR_KEY_NUM_OF_VLANS)) {
		key_nvs = skb_flow_dissector_target(flow_dissector,
						    FLOW_DISSECTOR_KEY_NUM_OF_VLANS,
						    target_container);
		key_nvs->num_of_vlans++;
	}

	switch (inner_proto) {
	case htons(ETH_P_IP):
		ok = flow_dissect_fast_ipv4(skb, flow_dissector,
					    target_container, data,
					    nhoff, hlen);
		break;
	case htons(ETH_P_IPV6):
		ok = flow_dissect_fast_ipv6(skb, flow_dissector,
					    target_container, data,
					    nhoff, hlen);
		break;
	case htons(ETH_P_8021Q):
	case htons(ETH_P_8021AD):
		ok = flow_dissect_fast_vlan(skb, flow_dissector,
					    target_container, data,
					    inner_proto, nhoff, hlen,
					    vlan_depth + 1);
		break;
	default:
		return false;
	}

	/* Count only on full success -- a miss defers and the slow path
	 * counts the occurrence. Depth 0 counts vlan, depth >= 1 qinq; a
	 * double-tagged hit counts both, as the slow path does.
	 */
	if (ok)
		flow_dissector_count_fast(vlan_depth == 0 ?
					  FLOW_DISSECTOR_SHAPE_VLAN :
					  FLOW_DISSECTOR_SHAPE_QINQ);
	return ok;
}

/* PPPoE session (RFC 2516) + 2-byte PPP protocol: write
 * FLOW_DISSECTOR_KEY_PPPOE if requested, then tail-call the eth_ip
 * fast-path for PPP_IP / PPP_IPV6. Anything else (PFC-compressed
 * protocol, LCP, MPLS) defers.
 */
static bool flow_dissect_fast_pppoe(const struct sk_buff *skb,
				    struct flow_dissector *flow_dissector,
				    void *target_container,
				    const void *data,
				    int nhoff, int hlen)
{
	struct flow_dissector_key_pppoe *key_pppoe;
	const struct {
		struct pppoe_hdr hdr;
		__be16 proto;
	} *hdr;
	__be16 inner_eth_proto;
	u16 ppp_proto;
	bool ok;

	if (unlikely(hlen - nhoff < (int)sizeof(*hdr)))
		return false;
	hdr = (const void *)((const u8 *)data + nhoff);
	if (!is_pppoe_ses_hdr_valid(&hdr->hdr))
		return false;

	ppp_proto = ntohs(hdr->proto);
	switch (ppp_proto) {
	case PPP_IP:
		inner_eth_proto = htons(ETH_P_IP);
		break;
	case PPP_IPV6:
		inner_eth_proto = htons(ETH_P_IPV6);
		break;
	default:
		/* MPLS unicast/multicast + everything else (LCP, IPCP,
		 * compressed PFC, ...) defers to the slow path.
		 */
		return false;
	}

	if (dissector_uses_key(flow_dissector,
			       FLOW_DISSECTOR_KEY_PPPOE)) {
		key_pppoe = skb_flow_dissector_target(flow_dissector,
						      FLOW_DISSECTOR_KEY_PPPOE,
						      target_container);
		key_pppoe->session_id = hdr->hdr.sid;
		key_pppoe->ppp_proto = htons(ppp_proto);
		key_pppoe->type = htons(ETH_P_PPP_SES);
	}

	nhoff += PPPOE_SES_HLEN;

	if (inner_eth_proto == htons(ETH_P_IP))
		ok = flow_dissect_fast_ipv4(skb, flow_dissector,
					    target_container, data,
					    nhoff, hlen);
	else
		ok = flow_dissect_fast_ipv6(skb, flow_dissector,
					    target_container, data,
					    nhoff, hlen);

	if (ok)
		flow_dissector_count_fast(FLOW_DISSECTOR_SHAPE_PPPOE);
	return ok;
}

/* Single MPLS label (BoS=1): write FLOW_DISSECTOR_KEY_MPLS lse[0]
 * when requested and stop -- the slow path returns OUT_GOOD on BoS
 * without descending into the inner packet, so the fast path must
 * not tail-call either. Multi-label stacks defer.
 */
static bool flow_dissect_fast_mpls(const struct sk_buff *skb,
				   struct flow_dissector *flow_dissector,
				   void *target_container,
				   const void *data,
				   __be16 proto, int nhoff, int hlen)
{
	struct flow_dissector_key_control *key_control;
	struct flow_dissector_key_basic *key_basic;
	struct flow_dissector_key_mpls *key_mpls;
	struct flow_dissector_mpls_lse *lse;
	const struct mpls_label *hdr;
	u32 entry, label, bos;

	if (unlikely(hlen - nhoff < (int)sizeof(*hdr)))
		return false;
	hdr = (const struct mpls_label *)((const u8 *)data + nhoff);
	entry = ntohl(hdr->entry);
	bos = (entry & MPLS_LS_S_MASK) >> MPLS_LS_S_SHIFT;

	/* Multi-label stack: defer. */
	if (!bos)
		return false;

	/* Neither MPLS key requested: the slow path returns OUT_GOOD without
	 * writing the key; skipping the write block matches it.
	 */
	label = (entry & MPLS_LS_LABEL_MASK) >> MPLS_LS_LABEL_SHIFT;

	if (dissector_uses_key(flow_dissector, FLOW_DISSECTOR_KEY_MPLS)) {
		key_mpls = skb_flow_dissector_target(flow_dissector,
						     FLOW_DISSECTOR_KEY_MPLS,
						     target_container);
		lse = &key_mpls->ls[0];
		lse->mpls_ttl = (entry & MPLS_LS_TTL_MASK) >> MPLS_LS_TTL_SHIFT;
		lse->mpls_bos = bos;
		lse->mpls_tc = (entry & MPLS_LS_TC_MASK) >> MPLS_LS_TC_SHIFT;
		lse->mpls_label = label;
		dissector_set_mpls_lse(key_mpls, 0);
	}

	/* Mirror the slow path's out_good terminal: nhoff advances past the
	 * LSE, then thoff/basic are written from the final proto/ip_proto.
	 */
	nhoff += (int)sizeof(*hdr);
	if (dissector_uses_key(flow_dissector, FLOW_DISSECTOR_KEY_CONTROL)) {
		key_control = skb_flow_dissector_target(flow_dissector,
							FLOW_DISSECTOR_KEY_CONTROL,
							target_container);
		key_control->thoff = min_t(u16, nhoff,
					   skb ? skb->len : hlen);
	}
	if (dissector_uses_key(flow_dissector, FLOW_DISSECTOR_KEY_BASIC)) {
		key_basic = skb_flow_dissector_target(flow_dissector,
						      FLOW_DISSECTOR_KEY_BASIC,
						      target_container);
		key_basic->n_proto = proto;
		key_basic->ip_proto = 0;
	}

	flow_dissector_count_fast(FLOW_DISSECTOR_SHAPE_MPLS);
	return true;
}

/* IPIP / 4in6 / 6in4 inner descent: recurse into the inner IP
 * fast-path, then stamp FLOW_DIS_ENCAPSULATION -- the inner helper
 * zeros key_control->flags, so the ENCAP write must come after.
 */
static bool flow_dissect_fast_ipip_inner(const struct sk_buff *skb,
					 struct flow_dissector *flow_dissector,
					 void *target_container,
					 const void *data,
					 __be16 inner_eth_proto,
					 int inner_nhoff, int hlen)
{
	struct flow_dissector_key_control *key_control;
	bool ok;

	if (inner_eth_proto == htons(ETH_P_IP))
		ok = flow_dissect_fast_ipv4(skb, flow_dissector,
					    target_container, data,
					    inner_nhoff, hlen);
	else if (inner_eth_proto == htons(ETH_P_IPV6))
		ok = flow_dissect_fast_ipv6(skb, flow_dissector,
					    target_container, data,
					    inner_nhoff, hlen);
	else
		return false;

	if (!ok)
		return false;

	if (dissector_uses_key(flow_dissector,
			       FLOW_DISSECTOR_KEY_CONTROL)) {
		key_control = skb_flow_dissector_target(flow_dissector,
							FLOW_DISSECTOR_KEY_CONTROL,
							target_container);
		key_control->flags |= FLOW_DIS_ENCAPSULATION;
	}
	flow_dissector_count_fast(FLOW_DISSECTOR_SHAPE_IPIP);
	return true;
}

/* Plain-GRE inner descent (version 0, no flags, inner IP): a 4-byte
 * base header the slow path steps over with PROTO_AGAIN. Any flags,
 * version 1 (PPTP) or a non-IP inner defers. ENCAP is stamped after
 * the inner pass, which zeros key_control->flags.
 */
static bool flow_dissect_fast_gre_inner(const struct sk_buff *skb,
					struct flow_dissector *flow_dissector,
					void *target_container,
					const void *data,
					int nhoff, int hlen)
{
	struct flow_dissector_key_control *key_control;
	const struct gre_base_hdr *hdr;
	__be16 inner_proto;
	int inner_nhoff;
	bool ok;

	if (unlikely(hlen - nhoff < (int)sizeof(*hdr)))
		return false;
	hdr = (const struct gre_base_hdr *)((const u8 *)data + nhoff);

	/* flags field carries the GRE control flags *and* the 3-bit
	 * version in its low bits. A zero word means "no flags set
	 * AND version 0" — the byte-identical fast subset.
	 */
	if (hdr->flags != 0)
		return false;

	switch (hdr->protocol) {
	case htons(ETH_P_IP):
		inner_proto = htons(ETH_P_IP);
		break;
	case htons(ETH_P_IPV6):
		inner_proto = htons(ETH_P_IPV6);
		break;
	default:
		return false;
	}

	inner_nhoff = nhoff + (int)sizeof(*hdr);

	if (inner_proto == htons(ETH_P_IP))
		ok = flow_dissect_fast_ipv4(skb, flow_dissector,
					    target_container, data,
					    inner_nhoff, hlen);
	else
		ok = flow_dissect_fast_ipv6(skb, flow_dissector,
					    target_container, data,
					    inner_nhoff, hlen);

	if (!ok)
		return false;

	/* Re-establish ENCAP after the inner pass zeroed key_control->flags. */
	if (dissector_uses_key(flow_dissector,
			       FLOW_DISSECTOR_KEY_CONTROL)) {
		key_control = skb_flow_dissector_target(flow_dissector,
							FLOW_DISSECTOR_KEY_CONTROL,
							target_container);
		key_control->flags |= FLOW_DIS_ENCAPSULATION;
	}
	flow_dissector_count_fast(FLOW_DISSECTOR_SHAPE_GRE);
	return true;
}

/* Did a fast-path leaf stamp FLOW_DIS_ENCAPSULATION? The dispatcher
 * counts eth_ip only for plain top-level IP; descents count under
 * their own shape (UDP-tunnel descents not at all).
 */
static bool flow_dissect_fast_is_encap(struct flow_dissector *flow_dissector,
				       void *target_container)
{
	struct flow_dissector_key_control *key_control;

	if (!dissector_uses_key(flow_dissector, FLOW_DISSECTOR_KEY_CONTROL))
		return false;
	key_control = skb_flow_dissector_target(flow_dissector,
						FLOW_DISSECTOR_KEY_CONTROL,
						target_container);
	return key_control->flags & FLOW_DIS_ENCAPSULATION;
}

/* Top-level dispatcher: eligibility check (only the two standard
 * dissectors and flag subset) + per-proto switch with per-shape
 * static_branch gating. Each case's branch is a forward not-taken JMP
 * when its sysctl is 0 — matches the slow-path cost.
 */
static bool flow_dissect_fast(const struct sk_buff *skb,
			      struct flow_dissector *flow_dissector,
			      void *target_container,
			      const void *data,
			      __be16 proto, int nhoff, int hlen,
			      unsigned int flags)
{
	if (flow_dissector != &flow_keys_dissector &&
	    flow_dissector != &flow_keys_dissector_symmetric)
		return false;

	/* skb_get_hash() -- behind RPS/RFS, fq, fq_codel, cake -- passes
	 * FLOW_DISSECTOR_F_STOP_AT_FLOW_LABEL on every dissect, so the
	 * flag must be admitted or the fast path never runs for the
	 * kernel's main consumers. Its only semantic (stop early on a
	 * non-zero IPv6 flow label) is subsumed by the IPv6 fast path
	 * deferring on any non-zero label. Any other flag defers.
	 */
	if (flags & ~(unsigned int)(FLOW_DISSECTOR_F_PARSE_1ST_FRAG |
				    FLOW_DISSECTOR_F_STOP_AT_FLOW_LABEL))
		return false;

	switch (proto) {
	case htons(ETH_P_8021Q):
	case htons(ETH_P_8021AD):
		if (!static_branch_unlikely(&flow_dissector_vlan_key))
			return false;
		return flow_dissect_fast_vlan(skb, flow_dissector,
					      target_container, data,
					      proto, nhoff, hlen, 0);
	case htons(ETH_P_IP):
		if (!static_branch_unlikely(&flow_dissector_eth_ip_key))
			return false;
		if (!flow_dissect_fast_ipv4(skb, flow_dissector,
					    target_container, data,
					    nhoff, hlen))
			return false;
		/* Plain top-level eth+IP only; descents count under their own shape. */
		if (!flow_dissect_fast_is_encap(flow_dissector, target_container))
			flow_dissector_count_fast(FLOW_DISSECTOR_SHAPE_ETH_IP);
		return true;
	case htons(ETH_P_IPV6):
		if (!static_branch_unlikely(&flow_dissector_eth_ip_key))
			return false;
		if (!flow_dissect_fast_ipv6(skb, flow_dissector,
					    target_container, data,
					    nhoff, hlen))
			return false;
		if (!flow_dissect_fast_is_encap(flow_dissector, target_container))
			flow_dissector_count_fast(FLOW_DISSECTOR_SHAPE_ETH_IP);
		return true;
	case htons(ETH_P_PPP_SES):
		if (!static_branch_unlikely(&flow_dissector_pppoe_key))
			return false;
		return flow_dissect_fast_pppoe(skb, flow_dissector,
					       target_container, data,
					       nhoff, hlen);
	case htons(ETH_P_MPLS_UC):
	case htons(ETH_P_MPLS_MC):
		if (!static_branch_unlikely(&flow_dissector_mpls_key))
			return false;
		return flow_dissect_fast_mpls(skb, flow_dissector,
					      target_container, data,
					      proto, nhoff, hlen);
	default:
		return false;
	}
}

/**
 * __skb_flow_dissect - extract the flow_keys struct and return it
 * @net: associated network namespace, derived from @skb if NULL
 * @skb: sk_buff to extract the flow from, can be NULL if the rest are specified
 * @flow_dissector: list of keys to dissect
 * @target_container: target structure to put dissected values into
 * @data: raw buffer pointer to the packet, if NULL use skb->data
 * @proto: protocol for which to get the flow, if @data is NULL use skb->protocol
 * @nhoff: network header offset, if @data is NULL use skb_network_offset(skb)
 * @hlen: packet header length, if @data is NULL use skb_headlen(skb)
 * @flags: flags that control the dissection process, e.g.
 *         FLOW_DISSECTOR_F_STOP_AT_ENCAP.
 *
 * The function will try to retrieve individual keys into target specified
 * by flow_dissector from either the skbuff or a raw buffer specified by the
 * rest parameters.
 *
 * Caller must take care of zeroing target container memory.
 */
bool __skb_flow_dissect(const struct net *net,
			const struct sk_buff *skb,
			struct flow_dissector *flow_dissector,
			void *target_container, const void *data,
			__be16 proto, int nhoff, int hlen, unsigned int flags)
{
	struct flow_dissector_key_control *key_control;
	struct flow_dissector_key_basic *key_basic;
	struct flow_dissector_key_addrs *key_addrs;
	struct flow_dissector_key_tags *key_tags;
	struct flow_dissector_key_vlan *key_vlan;
	enum flow_dissect_ret fdret;
	enum flow_dissector_key_id dissector_vlan = FLOW_DISSECTOR_KEY_MAX;
	bool mpls_el = false;
	int mpls_lse = 0;
	int num_hdrs = 0;
	int nhoff_init = 0;
	bool eth_ip_top = false;
	u8 ip_proto = 0;
	bool ret;

	if (!data) {
		data = skb->data;
		proto = skb_vlan_tag_present(skb) ?
			 skb->vlan_proto : skb->protocol;
		nhoff = skb_network_offset(skb);
		hlen = skb_headlen(skb);
#if IS_ENABLED(CONFIG_NET_DSA)
		if (unlikely(skb->dev && netdev_uses_dsa(skb->dev) &&
			     proto == htons(ETH_P_XDSA))) {
			struct metadata_dst *md_dst = skb_metadata_dst(skb);
			const struct dsa_device_ops *ops;
			int offset = 0;

			ops = skb->dev->dsa_ptr->tag_ops;
			/* Only DSA header taggers break flow dissection */
			if (ops->needed_headroom &&
			    (!md_dst || md_dst->type != METADATA_HW_PORT_MUX)) {
				if (ops->flow_dissect)
					ops->flow_dissect(skb, &proto, &offset);
				else
					dsa_tag_generic_flow_dissect(skb,
								     &proto,
								     &offset);
				hlen -= offset;
				nhoff += offset;
			}
		}
#endif
	}

	/* It is ensured by skb_flow_dissector_init() that control key will
	 * be always present.
	 */
	key_control = skb_flow_dissector_target(flow_dissector,
						FLOW_DISSECTOR_KEY_CONTROL,
						target_container);

	/* It is ensured by skb_flow_dissector_init() that basic key will
	 * be always present.
	 */
	key_basic = skb_flow_dissector_target(flow_dissector,
					      FLOW_DISSECTOR_KEY_BASIC,
					      target_container);

	if (static_branch_unlikely(&netns_bpf_flow_dissector_enabled)) {
		rcu_read_lock();

		if (skb) {
			if (!net) {
				if (skb->dev)
					net = dev_net_rcu(skb->dev);
				else if (skb->sk)
					net = sock_net(skb->sk);
			}
		}

		DEBUG_NET_WARN_ON_ONCE(!net);
		if (net) {
			enum netns_bpf_attach_type type = NETNS_BPF_FLOW_DISSECTOR;
			struct bpf_prog_array *run_array;

			run_array = rcu_dereference(init_net.bpf.run_array[type]);
			if (!run_array)
				run_array = rcu_dereference(net->bpf.run_array[type]);

			if (run_array) {
				struct bpf_flow_keys flow_keys;
				struct bpf_flow_dissector ctx = {
					.flow_keys = &flow_keys,
					.data = data,
					.data_end = data + hlen,
				};
				__be16 n_proto = proto;
				struct bpf_prog *prog;
				u32 result;

				if (skb) {
					ctx.skb = skb;
					/* we can't use 'proto' in the skb case
					 * because it might be set to
					 * skb->vlan_proto which has been pulled
					 * from the data
					 */
					n_proto = skb->protocol;
				}

				prog = READ_ONCE(run_array->items[0].prog);
				result = bpf_flow_dissect(prog, &ctx, n_proto,
							  nhoff, hlen, flags);
				if (result != BPF_FLOW_DISSECTOR_CONTINUE) {
					__skb_flow_bpf_to_target(&flow_keys,
								 flow_dissector,
								 target_container);
					rcu_read_unlock();
					return result == BPF_OK;
				}
			}
		}

		rcu_read_unlock();
	}

	/* Opt-in fast-path; flow_dissect_fast() does the eligibility check
	 * and per-shape gating.
	 */
	this_cpu_inc(flow_dissector_pcpu_stats.dissects);

	if (flow_dissect_fast(skb, flow_dissector, target_container,
			      data, proto, nhoff, hlen, flags))
		return true;

	if (dissector_uses_key(flow_dissector,
			       FLOW_DISSECTOR_KEY_ETH_ADDRS)) {
		struct flow_dissector_key_eth_addrs *key_eth_addrs;

		key_eth_addrs = skb_flow_dissector_target(flow_dissector,
							  FLOW_DISSECTOR_KEY_ETH_ADDRS,
							  target_container);
		/* TC filter blocks can be shared across devices with
		 * different link types, so we cannot validate this
		 * when the filter is installed -- check at dissect time.
		 */
		if (skb && skb->dev &&
		    skb->dev->type == ARPHRD_ETHER &&
		    skb_mac_header_was_set(skb))
			memcpy(key_eth_addrs, eth_hdr(skb), sizeof(*key_eth_addrs));
		else
			memset(key_eth_addrs, 0, sizeof(*key_eth_addrs));
	}

	if (dissector_uses_key(flow_dissector,
			       FLOW_DISSECTOR_KEY_NUM_OF_VLANS)) {
		struct flow_dissector_key_num_of_vlans *key_num_of_vlans;

		key_num_of_vlans = skb_flow_dissector_target(flow_dissector,
							     FLOW_DISSECTOR_KEY_NUM_OF_VLANS,
							     target_container);
		key_num_of_vlans->num_of_vlans = 0;
	}

	nhoff_init = nhoff;

proto_again:
	fdret = FLOW_DISSECT_RET_CONTINUE;

	switch (proto) {
	case htons(ETH_P_IP): {
		const struct iphdr *iph;
		struct iphdr _iph;

		iph = __skb_header_pointer(skb, nhoff, sizeof(_iph), data, hlen, &_iph);
		if (!iph || iph->ihl < 5) {
			fdret = FLOW_DISSECT_RET_OUT_BAD;
			break;
		}

		/* Top-level eth+IPv4: eth_ip shape candidate (confirmed at out:). */
		if (nhoff == nhoff_init)
			eth_ip_top = true;

		nhoff += iph->ihl * 4;

		ip_proto = iph->protocol;

		if (dissector_uses_key(flow_dissector,
				       FLOW_DISSECTOR_KEY_IPV4_ADDRS)) {
			key_addrs = skb_flow_dissector_target(flow_dissector,
							      FLOW_DISSECTOR_KEY_IPV4_ADDRS,
							      target_container);

			memcpy(&key_addrs->v4addrs.src, &iph->saddr,
			       sizeof(key_addrs->v4addrs.src));
			memcpy(&key_addrs->v4addrs.dst, &iph->daddr,
			       sizeof(key_addrs->v4addrs.dst));
			key_control->addr_type = FLOW_DISSECTOR_KEY_IPV4_ADDRS;
		}

		__skb_flow_dissect_ipv4(skb, flow_dissector,
					target_container, data, iph);

		if (ip_is_fragment(iph)) {
			key_control->flags |= FLOW_DIS_IS_FRAGMENT;

			if (iph->frag_off & htons(IP_OFFSET)) {
				fdret = FLOW_DISSECT_RET_OUT_GOOD;
				break;
			} else {
				key_control->flags |= FLOW_DIS_FIRST_FRAG;
				if (!(flags &
				      FLOW_DISSECTOR_F_PARSE_1ST_FRAG)) {
					fdret = FLOW_DISSECT_RET_OUT_GOOD;
					break;
				}
			}
		}

		break;
	}
	case htons(ETH_P_IPV6): {
		const struct ipv6hdr *iph;
		struct ipv6hdr _iph;

		iph = __skb_header_pointer(skb, nhoff, sizeof(_iph), data, hlen, &_iph);
		if (!iph) {
			fdret = FLOW_DISSECT_RET_OUT_BAD;
			break;
		}

		if (nhoff == nhoff_init)
			eth_ip_top = true;

		ip_proto = iph->nexthdr;
		nhoff += sizeof(struct ipv6hdr);

		if (dissector_uses_key(flow_dissector,
				       FLOW_DISSECTOR_KEY_IPV6_ADDRS)) {
			key_addrs = skb_flow_dissector_target(flow_dissector,
							      FLOW_DISSECTOR_KEY_IPV6_ADDRS,
							      target_container);

			memcpy(&key_addrs->v6addrs.src, &iph->saddr,
			       sizeof(key_addrs->v6addrs.src));
			memcpy(&key_addrs->v6addrs.dst, &iph->daddr,
			       sizeof(key_addrs->v6addrs.dst));
			key_control->addr_type = FLOW_DISSECTOR_KEY_IPV6_ADDRS;
		}

		if ((dissector_uses_key(flow_dissector,
					FLOW_DISSECTOR_KEY_FLOW_LABEL) ||
		     (flags & FLOW_DISSECTOR_F_STOP_AT_FLOW_LABEL)) &&
		    ip6_flowlabel(iph)) {
			__be32 flow_label = ip6_flowlabel(iph);

			if (dissector_uses_key(flow_dissector,
					       FLOW_DISSECTOR_KEY_FLOW_LABEL)) {
				key_tags = skb_flow_dissector_target(flow_dissector,
								     FLOW_DISSECTOR_KEY_FLOW_LABEL,
								     target_container);
				key_tags->flow_label = ntohl(flow_label);
			}
			if (flags & FLOW_DISSECTOR_F_STOP_AT_FLOW_LABEL) {
				fdret = FLOW_DISSECT_RET_OUT_GOOD;
				break;
			}
		}

		__skb_flow_dissect_ipv6(skb, flow_dissector,
					target_container, data, iph);

		break;
	}
	case htons(ETH_P_8021AD):
	case htons(ETH_P_8021Q): {
		const struct vlan_hdr *vlan = NULL;
		struct vlan_hdr _vlan;
		__be16 saved_vlan_tpid = proto;

		if (dissector_vlan == FLOW_DISSECTOR_KEY_MAX &&
		    skb && skb_vlan_tag_present(skb)) {
			proto = skb->protocol;
		} else {
			vlan = __skb_header_pointer(skb, nhoff, sizeof(_vlan),
						    data, hlen, &_vlan);
			if (!vlan) {
				fdret = FLOW_DISSECT_RET_OUT_BAD;
				break;
			}

			proto = vlan->h_vlan_encapsulated_proto;
			nhoff += sizeof(*vlan);
		}

		if (dissector_uses_key(flow_dissector, FLOW_DISSECTOR_KEY_NUM_OF_VLANS) &&
		    !(key_control->flags & FLOW_DIS_ENCAPSULATION)) {
			struct flow_dissector_key_num_of_vlans *key_nvs;

			key_nvs = skb_flow_dissector_target(flow_dissector,
							    FLOW_DISSECTOR_KEY_NUM_OF_VLANS,
							    target_container);
			key_nvs->num_of_vlans++;
		}

		if (dissector_vlan == FLOW_DISSECTOR_KEY_MAX) {
			dissector_vlan = FLOW_DISSECTOR_KEY_VLAN;
			/* First (outer) tag: the vlan fast-path shape. */
			flow_dissector_count_slow(FLOW_DISSECTOR_SHAPE_VLAN);
		} else if (dissector_vlan == FLOW_DISSECTOR_KEY_VLAN) {
			dissector_vlan = FLOW_DISSECTOR_KEY_CVLAN;
			/* Second tag: the qinq (double-tag) fast-path shape. */
			flow_dissector_count_slow(FLOW_DISSECTOR_SHAPE_QINQ);
		} else {
			fdret = FLOW_DISSECT_RET_PROTO_AGAIN;
			break;
		}

		if (dissector_uses_key(flow_dissector, dissector_vlan)) {
			key_vlan = skb_flow_dissector_target(flow_dissector,
							     dissector_vlan,
							     target_container);

			if (!vlan) {
				key_vlan->vlan_id = skb_vlan_tag_get_id(skb);
				key_vlan->vlan_priority = skb_vlan_tag_get_prio(skb);
			} else {
				key_vlan->vlan_id = ntohs(vlan->h_vlan_TCI) &
					VLAN_VID_MASK;
				key_vlan->vlan_priority =
					(ntohs(vlan->h_vlan_TCI) &
					 VLAN_PRIO_MASK) >> VLAN_PRIO_SHIFT;
			}
			key_vlan->vlan_tpid = saved_vlan_tpid;
			key_vlan->vlan_eth_type = proto;
		}

		fdret = FLOW_DISSECT_RET_PROTO_AGAIN;
		break;
	}
	case htons(ETH_P_PPP_SES): {
		struct {
			struct pppoe_hdr hdr;
			__be16 proto;
		} *hdr, _hdr;
		u16 ppp_proto;

		hdr = __skb_header_pointer(skb, nhoff, sizeof(_hdr), data, hlen, &_hdr);
		if (!hdr) {
			fdret = FLOW_DISSECT_RET_OUT_BAD;
			break;
		}

		if (!is_pppoe_ses_hdr_valid(&hdr->hdr)) {
			fdret = FLOW_DISSECT_RET_OUT_BAD;
			break;
		}

		flow_dissector_count_slow(FLOW_DISSECTOR_SHAPE_PPPOE);

		/* PFC (compressed 1-byte protocol) frames are not processed.
		 * A compressed protocol field has the least significant bit of
		 * the most significant octet set, which will fail the following
		 * ppp_proto_is_valid(), returning FLOW_DISSECT_RET_OUT_BAD.
		 */
		ppp_proto = ntohs(hdr->proto);
		nhoff += PPPOE_SES_HLEN;

		if (ppp_proto == PPP_IP) {
			proto = htons(ETH_P_IP);
			fdret = FLOW_DISSECT_RET_PROTO_AGAIN;
		} else if (ppp_proto == PPP_IPV6) {
			proto = htons(ETH_P_IPV6);
			fdret = FLOW_DISSECT_RET_PROTO_AGAIN;
		} else if (ppp_proto == PPP_MPLS_UC) {
			proto = htons(ETH_P_MPLS_UC);
			fdret = FLOW_DISSECT_RET_PROTO_AGAIN;
		} else if (ppp_proto == PPP_MPLS_MC) {
			proto = htons(ETH_P_MPLS_MC);
			fdret = FLOW_DISSECT_RET_PROTO_AGAIN;
		} else if (ppp_proto_is_valid(ppp_proto)) {
			fdret = FLOW_DISSECT_RET_OUT_GOOD;
		} else {
			fdret = FLOW_DISSECT_RET_OUT_BAD;
			break;
		}

		if (dissector_uses_key(flow_dissector,
				       FLOW_DISSECTOR_KEY_PPPOE)) {
			struct flow_dissector_key_pppoe *key_pppoe;

			key_pppoe = skb_flow_dissector_target(flow_dissector,
							      FLOW_DISSECTOR_KEY_PPPOE,
							      target_container);
			key_pppoe->session_id = hdr->hdr.sid;
			key_pppoe->ppp_proto = htons(ppp_proto);
			key_pppoe->type = htons(ETH_P_PPP_SES);
		}
		break;
	}
	case htons(ETH_P_TIPC): {
		struct tipc_basic_hdr *hdr, _hdr;

		hdr = __skb_header_pointer(skb, nhoff, sizeof(_hdr),
					   data, hlen, &_hdr);
		if (!hdr) {
			fdret = FLOW_DISSECT_RET_OUT_BAD;
			break;
		}

		if (dissector_uses_key(flow_dissector,
				       FLOW_DISSECTOR_KEY_TIPC)) {
			key_addrs = skb_flow_dissector_target(flow_dissector,
							      FLOW_DISSECTOR_KEY_TIPC,
							      target_container);
			key_addrs->tipckey.key = tipc_hdr_rps_key(hdr);
			key_control->addr_type = FLOW_DISSECTOR_KEY_TIPC;
		}
		fdret = FLOW_DISSECT_RET_OUT_GOOD;
		break;
	}

	case htons(ETH_P_MPLS_UC):
	case htons(ETH_P_MPLS_MC):
		flow_dissector_count_slow(FLOW_DISSECTOR_SHAPE_MPLS);
		fdret = __skb_flow_dissect_mpls(skb, flow_dissector,
						target_container, data,
						nhoff, hlen, mpls_lse,
						&mpls_el);
		nhoff += sizeof(struct mpls_label);
		mpls_lse++;
		break;
	case htons(ETH_P_FCOE):
		if ((hlen - nhoff) < FCOE_HEADER_LEN) {
			fdret = FLOW_DISSECT_RET_OUT_BAD;
			break;
		}

		nhoff += FCOE_HEADER_LEN;
		fdret = FLOW_DISSECT_RET_OUT_GOOD;
		break;

	case htons(ETH_P_ARP):
	case htons(ETH_P_RARP):
		fdret = __skb_flow_dissect_arp(skb, flow_dissector,
					       target_container, data,
					       nhoff, hlen);
		break;

	case htons(ETH_P_BATMAN):
		fdret = __skb_flow_dissect_batadv(skb, key_control, data,
						  &proto, &nhoff, hlen, flags);
		break;

	case htons(ETH_P_1588): {
		struct ptp_header *hdr, _hdr;

		hdr = __skb_header_pointer(skb, nhoff, sizeof(_hdr), data,
					   hlen, &_hdr);
		if (!hdr) {
			fdret = FLOW_DISSECT_RET_OUT_BAD;
			break;
		}

		nhoff += sizeof(struct ptp_header);
		fdret = FLOW_DISSECT_RET_OUT_GOOD;
		break;
	}

	case htons(ETH_P_PRP):
	case htons(ETH_P_HSR): {
		struct hsr_tag *hdr, _hdr;

		hdr = __skb_header_pointer(skb, nhoff, sizeof(_hdr), data, hlen,
					   &_hdr);
		if (!hdr) {
			fdret = FLOW_DISSECT_RET_OUT_BAD;
			break;
		}

		proto = hdr->encap_proto;
		nhoff += HSR_HLEN;
		fdret = FLOW_DISSECT_RET_PROTO_AGAIN;
		break;
	}

	case htons(ETH_P_CFM):
		fdret = __skb_flow_dissect_cfm(skb, flow_dissector,
					       target_container, data,
					       nhoff, hlen);
		break;

	default:
		fdret = FLOW_DISSECT_RET_OUT_BAD;
		break;
	}

	/* Process result of proto processing */
	switch (fdret) {
	case FLOW_DISSECT_RET_OUT_GOOD:
		goto out_good;
	case FLOW_DISSECT_RET_PROTO_AGAIN:
		if (skb_flow_dissect_allowed(&num_hdrs))
			goto proto_again;
		goto out_good;
	case FLOW_DISSECT_RET_CONTINUE:
	case FLOW_DISSECT_RET_IPPROTO_AGAIN:
		break;
	case FLOW_DISSECT_RET_OUT_BAD:
	default:
		goto out_bad;
	}

ip_proto_again:
	fdret = FLOW_DISSECT_RET_CONTINUE;

	switch (ip_proto) {
	case IPPROTO_GRE:
		if (flags & FLOW_DISSECTOR_F_STOP_BEFORE_ENCAP) {
			fdret = FLOW_DISSECT_RET_OUT_GOOD;
			break;
		}
		flow_dissector_count_slow(FLOW_DISSECTOR_SHAPE_GRE);

		fdret = __skb_flow_dissect_gre(skb, key_control, flow_dissector,
					       target_container, data,
					       &proto, &nhoff, &hlen, flags);
		break;

	case NEXTHDR_HOP:
	case NEXTHDR_ROUTING:
	case NEXTHDR_DEST: {
		u8 _opthdr[2], *opthdr;

		if (proto != htons(ETH_P_IPV6))
			break;

		opthdr = __skb_header_pointer(skb, nhoff, sizeof(_opthdr),
					      data, hlen, &_opthdr);
		if (!opthdr) {
			fdret = FLOW_DISSECT_RET_OUT_BAD;
			break;
		}

		ip_proto = opthdr[0];
		nhoff += (opthdr[1] + 1) << 3;

		fdret = FLOW_DISSECT_RET_IPPROTO_AGAIN;
		break;
	}
	case NEXTHDR_FRAGMENT: {
		struct frag_hdr _fh, *fh;

		if (proto != htons(ETH_P_IPV6))
			break;

		fh = __skb_header_pointer(skb, nhoff, sizeof(_fh),
					  data, hlen, &_fh);

		if (!fh) {
			fdret = FLOW_DISSECT_RET_OUT_BAD;
			break;
		}

		key_control->flags |= FLOW_DIS_IS_FRAGMENT;

		nhoff += sizeof(_fh);
		ip_proto = fh->nexthdr;

		if (!(fh->frag_off & htons(IP6_OFFSET))) {
			key_control->flags |= FLOW_DIS_FIRST_FRAG;
			if (flags & FLOW_DISSECTOR_F_PARSE_1ST_FRAG) {
				fdret = FLOW_DISSECT_RET_IPPROTO_AGAIN;
				break;
			}
		}

		fdret = FLOW_DISSECT_RET_OUT_GOOD;
		break;
	}
	case IPPROTO_IPIP:
		if (flags & FLOW_DISSECTOR_F_STOP_BEFORE_ENCAP) {
			fdret = FLOW_DISSECT_RET_OUT_GOOD;
			break;
		}
		/* Counted below the STOP check: a stop-flagged dissect is
		 * one the fast path could never have handled.
		 */
		flow_dissector_count_slow(FLOW_DISSECTOR_SHAPE_IPIP);

		proto = htons(ETH_P_IP);

		key_control->flags |= FLOW_DIS_ENCAPSULATION;
		if (flags & FLOW_DISSECTOR_F_STOP_AT_ENCAP) {
			fdret = FLOW_DISSECT_RET_OUT_GOOD;
			break;
		}

		fdret = FLOW_DISSECT_RET_PROTO_AGAIN;
		break;

	case IPPROTO_IPV6:
		if (flags & FLOW_DISSECTOR_F_STOP_BEFORE_ENCAP) {
			fdret = FLOW_DISSECT_RET_OUT_GOOD;
			break;
		}

		proto = htons(ETH_P_IPV6);

		key_control->flags |= FLOW_DIS_ENCAPSULATION;
		if (flags & FLOW_DISSECTOR_F_STOP_AT_ENCAP) {
			fdret = FLOW_DISSECT_RET_OUT_GOOD;
			break;
		}

		fdret = FLOW_DISSECT_RET_PROTO_AGAIN;
		break;


	case IPPROTO_MPLS:
		proto = htons(ETH_P_MPLS_UC);
		fdret = FLOW_DISSECT_RET_PROTO_AGAIN;
		break;

	case IPPROTO_TCP:
		__skb_flow_dissect_tcp(skb, flow_dissector, target_container,
				       data, nhoff, hlen);
		break;

	case IPPROTO_ICMP:
	case IPPROTO_ICMPV6:
		__skb_flow_dissect_icmp(skb, flow_dissector, target_container,
					data, nhoff, hlen);
		break;
	case IPPROTO_L2TP:
		__skb_flow_dissect_l2tpv3(skb, flow_dissector, target_container,
					  data, nhoff, hlen);
		break;
	case IPPROTO_ESP:
		__skb_flow_dissect_esp(skb, flow_dissector, target_container,
				       data, nhoff, hlen);
		break;
	case IPPROTO_AH:
		__skb_flow_dissect_ah(skb, flow_dissector, target_container,
				      data, nhoff, hlen);
		break;
	default:
		break;
	}

	if (!(key_control->flags & FLOW_DIS_IS_FRAGMENT))
		__skb_flow_dissect_ports(skb, flow_dissector, target_container,
					 data, nhoff, ip_proto, hlen);

	/* Process result of IP proto processing */
	switch (fdret) {
	case FLOW_DISSECT_RET_PROTO_AGAIN:
		if (skb_flow_dissect_allowed(&num_hdrs))
			goto proto_again;
		break;
	case FLOW_DISSECT_RET_IPPROTO_AGAIN:
		if (skb_flow_dissect_allowed(&num_hdrs))
			goto ip_proto_again;
		break;
	case FLOW_DISSECT_RET_OUT_GOOD:
	case FLOW_DISSECT_RET_CONTINUE:
		break;
	case FLOW_DISSECT_RET_OUT_BAD:
	default:
		goto out_bad;
	}

out_good:
	ret = true;

out:
	key_control->thoff = min_t(u16, nhoff, skb ? skb->len : hlen);
	key_basic->n_proto = proto;
	key_basic->ip_proto = ip_proto;

	/* eth_ip shape: top-level eth+IP, TCP/UDP, no encap -- counted here
	 * because the fast path returns earlier; with the gate off this is
	 * the shape's eligible-fraction signal.
	 */
	if (ret && eth_ip_top &&
	    !(key_control->flags & FLOW_DIS_ENCAPSULATION) &&
	    (ip_proto == IPPROTO_TCP || ip_proto == IPPROTO_UDP))
		flow_dissector_count_slow(FLOW_DISSECTOR_SHAPE_ETH_IP);

	return ret;

out_bad:
	ret = false;
	goto out;
}
EXPORT_SYMBOL(__skb_flow_dissect);

static siphash_aligned_key_t hashrnd;
static __always_inline void __flow_hash_secret_init(void)
{
	net_get_random_once(&hashrnd, sizeof(hashrnd));
}

static const void *flow_keys_hash_start(const struct flow_keys *flow)
{
	BUILD_BUG_ON(FLOW_KEYS_HASH_OFFSET % SIPHASH_ALIGNMENT);
	return &flow->FLOW_KEYS_HASH_START_FIELD;
}

static inline size_t flow_keys_hash_length(const struct flow_keys *flow)
{
	size_t diff = FLOW_KEYS_HASH_OFFSET + sizeof(flow->addrs);

	BUILD_BUG_ON((sizeof(*flow) - FLOW_KEYS_HASH_OFFSET) % sizeof(u32));

	switch (flow->control.addr_type) {
	case FLOW_DISSECTOR_KEY_IPV4_ADDRS:
		diff -= sizeof(flow->addrs.v4addrs);
		break;
	case FLOW_DISSECTOR_KEY_IPV6_ADDRS:
		diff -= sizeof(flow->addrs.v6addrs);
		break;
	case FLOW_DISSECTOR_KEY_TIPC:
		diff -= sizeof(flow->addrs.tipckey);
		break;
	}
	return sizeof(*flow) - diff;
}

__be32 flow_get_u32_src(const struct flow_keys *flow)
{
	switch (flow->control.addr_type) {
	case FLOW_DISSECTOR_KEY_IPV4_ADDRS:
		return flow->addrs.v4addrs.src;
	case FLOW_DISSECTOR_KEY_IPV6_ADDRS:
		return (__force __be32)ipv6_addr_hash(
			&flow->addrs.v6addrs.src);
	case FLOW_DISSECTOR_KEY_TIPC:
		return flow->addrs.tipckey.key;
	default:
		return 0;
	}
}
EXPORT_SYMBOL(flow_get_u32_src);

__be32 flow_get_u32_dst(const struct flow_keys *flow)
{
	switch (flow->control.addr_type) {
	case FLOW_DISSECTOR_KEY_IPV4_ADDRS:
		return flow->addrs.v4addrs.dst;
	case FLOW_DISSECTOR_KEY_IPV6_ADDRS:
		return (__force __be32)ipv6_addr_hash(
			&flow->addrs.v6addrs.dst);
	default:
		return 0;
	}
}
EXPORT_SYMBOL(flow_get_u32_dst);

/* Sort the source and destination IP and the ports,
 * to have consistent hash within the two directions
 */
static inline void __flow_hash_consistentify(struct flow_keys *keys)
{
	int addr_diff, i;

	switch (keys->control.addr_type) {
	case FLOW_DISSECTOR_KEY_IPV4_ADDRS:
		if ((__force u32)keys->addrs.v4addrs.dst <
		    (__force u32)keys->addrs.v4addrs.src)
			swap(keys->addrs.v4addrs.src, keys->addrs.v4addrs.dst);

		if ((__force u16)keys->ports.dst <
		    (__force u16)keys->ports.src) {
			swap(keys->ports.src, keys->ports.dst);
		}
		break;
	case FLOW_DISSECTOR_KEY_IPV6_ADDRS:
		addr_diff = memcmp(&keys->addrs.v6addrs.dst,
				   &keys->addrs.v6addrs.src,
				   sizeof(keys->addrs.v6addrs.dst));
		if (addr_diff < 0) {
			for (i = 0; i < 4; i++)
				swap(keys->addrs.v6addrs.src.s6_addr32[i],
				     keys->addrs.v6addrs.dst.s6_addr32[i]);
		}
		if ((__force u16)keys->ports.dst <
		    (__force u16)keys->ports.src) {
			swap(keys->ports.src, keys->ports.dst);
		}
		break;
	}
}

static inline u32 __flow_hash_from_keys(struct flow_keys *keys,
					const siphash_key_t *keyval)
{
	u32 hash;

	__flow_hash_consistentify(keys);

	hash = siphash(flow_keys_hash_start(keys),
		       flow_keys_hash_length(keys), keyval);
	if (!hash)
		hash = 1;

	return hash;
}

u32 flow_hash_from_keys(struct flow_keys *keys)
{
	__flow_hash_secret_init();
	return __flow_hash_from_keys(keys, &hashrnd);
}
EXPORT_SYMBOL(flow_hash_from_keys);

u32 flow_hash_from_keys_seed(struct flow_keys *keys,
			     const siphash_key_t *keyval)
{
	return __flow_hash_from_keys(keys, keyval);
}
EXPORT_SYMBOL(flow_hash_from_keys_seed);

static inline u32 ___skb_get_hash(const struct sk_buff *skb,
				  struct flow_keys *keys,
				  const siphash_key_t *keyval)
{
	skb_flow_dissect_flow_keys(skb, keys,
				   FLOW_DISSECTOR_F_STOP_AT_FLOW_LABEL);

	return __flow_hash_from_keys(keys, keyval);
}

struct _flow_keys_digest_data {
	__be16	n_proto;
	u8	ip_proto;
	u8	padding;
	__be32	ports;
	__be32	src;
	__be32	dst;
};

void make_flow_keys_digest(struct flow_keys_digest *digest,
			   const struct flow_keys *flow)
{
	struct _flow_keys_digest_data *data =
	    (struct _flow_keys_digest_data *)digest;

	BUILD_BUG_ON(sizeof(*data) > sizeof(*digest));

	memset(digest, 0, sizeof(*digest));

	data->n_proto = flow->basic.n_proto;
	data->ip_proto = flow->basic.ip_proto;
	data->ports = flow->ports.ports;
	data->src = flow->addrs.v4addrs.src;
	data->dst = flow->addrs.v4addrs.dst;
}
EXPORT_SYMBOL(make_flow_keys_digest);

u32 __skb_get_hash_symmetric_net(const struct net *net, const struct sk_buff *skb)
{
	struct flow_keys keys;

	__flow_hash_secret_init();

	memset(&keys, 0, sizeof(keys));
	__skb_flow_dissect(net, skb, &flow_keys_dissector_symmetric,
			   &keys, NULL, 0, 0, 0, 0);

	return __flow_hash_from_keys(&keys, &hashrnd);
}
EXPORT_SYMBOL_GPL(__skb_get_hash_symmetric_net);

/**
 * __skb_get_hash_net: calculate a flow hash
 * @net: associated network namespace, derived from @skb if NULL
 * @skb: sk_buff to calculate flow hash from
 *
 * This function calculates a flow hash based on src/dst addresses
 * and src/dst port numbers.  Sets hash in skb to non-zero hash value
 * on success, zero indicates no valid hash.  Also, sets l4_hash in skb
 * if hash is a canonical 4-tuple hash over transport ports.
 */
void __skb_get_hash_net(const struct net *net, struct sk_buff *skb)
{
	struct flow_keys keys;
	u32 hash;

	memset(&keys, 0, sizeof(keys));

	__skb_flow_dissect(net, skb, &flow_keys_dissector,
			   &keys, NULL, 0, 0, 0,
			   FLOW_DISSECTOR_F_STOP_AT_FLOW_LABEL);

	__flow_hash_secret_init();

	hash = __flow_hash_from_keys(&keys, &hashrnd);

	__skb_set_sw_hash(skb, hash, flow_keys_have_l4(&keys));
}
EXPORT_SYMBOL(__skb_get_hash_net);

__u32 skb_get_hash_perturb(const struct sk_buff *skb,
			   const siphash_key_t *perturb)
{
	struct flow_keys keys;

	return ___skb_get_hash(skb, &keys, perturb);
}
EXPORT_SYMBOL(skb_get_hash_perturb);

u32 __skb_get_poff(const struct sk_buff *skb, const void *data,
		   const struct flow_keys_basic *keys, int hlen)
{
	u32 poff = keys->control.thoff;

	/* skip L4 headers for fragments after the first */
	if ((keys->control.flags & FLOW_DIS_IS_FRAGMENT) &&
	    !(keys->control.flags & FLOW_DIS_FIRST_FRAG))
		return poff;

	switch (keys->basic.ip_proto) {
	case IPPROTO_TCP: {
		/* access doff as u8 to avoid unaligned access */
		const u8 *doff;
		u8 _doff;

		doff = __skb_header_pointer(skb, poff + 12, sizeof(_doff),
					    data, hlen, &_doff);
		if (!doff)
			return poff;

		poff += max_t(u32, sizeof(struct tcphdr), (*doff & 0xF0) >> 2);
		break;
	}
	case IPPROTO_UDP:
	case IPPROTO_UDPLITE:
		poff += sizeof(struct udphdr);
		break;
	/* For the rest, we do not really care about header
	 * extensions at this point for now.
	 */
	case IPPROTO_ICMP:
		poff += sizeof(struct icmphdr);
		break;
	case IPPROTO_ICMPV6:
		poff += sizeof(struct icmp6hdr);
		break;
	case IPPROTO_IGMP:
		poff += sizeof(struct igmphdr);
		break;
	case IPPROTO_DCCP:
		poff += sizeof(struct dccp_hdr);
		break;
	case IPPROTO_SCTP:
		poff += sizeof(struct sctphdr);
		break;
	}

	return poff;
}

/**
 * skb_get_poff - get the offset to the payload
 * @skb: sk_buff to get the payload offset from
 *
 * The function will get the offset to the payload as far as it could
 * be dissected.  The main user is currently BPF, so that we can dynamically
 * truncate packets without needing to push actual payload to the user
 * space and can analyze headers only, instead.
 */
u32 skb_get_poff(const struct sk_buff *skb)
{
	struct flow_keys_basic keys;

	if (!skb_flow_dissect_flow_keys_basic(NULL, skb, &keys,
					      NULL, 0, 0, 0, 0))
		return 0;

	return __skb_get_poff(skb, skb->data, &keys, skb_headlen(skb));
}

__u32 __get_hash_from_flowi6(const struct flowi6 *fl6, struct flow_keys *keys)
{
	memset(keys, 0, sizeof(*keys));

	memcpy(&keys->addrs.v6addrs.src, &fl6->saddr,
	    sizeof(keys->addrs.v6addrs.src));
	memcpy(&keys->addrs.v6addrs.dst, &fl6->daddr,
	    sizeof(keys->addrs.v6addrs.dst));
	keys->control.addr_type = FLOW_DISSECTOR_KEY_IPV6_ADDRS;
	keys->ports.src = fl6->fl6_sport;
	keys->ports.dst = fl6->fl6_dport;
	keys->keyid.keyid = fl6->fl6_gre_key;
	keys->tags.flow_label = (__force u32)flowi6_get_flowlabel(fl6);
	keys->basic.ip_proto = fl6->flowi6_proto;

	return flow_hash_from_keys(keys);
}
EXPORT_SYMBOL(__get_hash_from_flowi6);

static const struct flow_dissector_key flow_keys_dissector_keys[] = {
	{
		.key_id = FLOW_DISSECTOR_KEY_CONTROL,
		.offset = offsetof(struct flow_keys, control),
	},
	{
		.key_id = FLOW_DISSECTOR_KEY_BASIC,
		.offset = offsetof(struct flow_keys, basic),
	},
	{
		.key_id = FLOW_DISSECTOR_KEY_IPV4_ADDRS,
		.offset = offsetof(struct flow_keys, addrs.v4addrs),
	},
	{
		.key_id = FLOW_DISSECTOR_KEY_IPV6_ADDRS,
		.offset = offsetof(struct flow_keys, addrs.v6addrs),
	},
	{
		.key_id = FLOW_DISSECTOR_KEY_TIPC,
		.offset = offsetof(struct flow_keys, addrs.tipckey),
	},
	{
		.key_id = FLOW_DISSECTOR_KEY_PORTS,
		.offset = offsetof(struct flow_keys, ports),
	},
	{
		.key_id = FLOW_DISSECTOR_KEY_VLAN,
		.offset = offsetof(struct flow_keys, vlan),
	},
	{
		.key_id = FLOW_DISSECTOR_KEY_FLOW_LABEL,
		.offset = offsetof(struct flow_keys, tags),
	},
	{
		.key_id = FLOW_DISSECTOR_KEY_GRE_KEYID,
		.offset = offsetof(struct flow_keys, keyid),
	},
};

static const struct flow_dissector_key flow_keys_dissector_symmetric_keys[] = {
	{
		.key_id = FLOW_DISSECTOR_KEY_CONTROL,
		.offset = offsetof(struct flow_keys, control),
	},
	{
		.key_id = FLOW_DISSECTOR_KEY_BASIC,
		.offset = offsetof(struct flow_keys, basic),
	},
	{
		.key_id = FLOW_DISSECTOR_KEY_IPV4_ADDRS,
		.offset = offsetof(struct flow_keys, addrs.v4addrs),
	},
	{
		.key_id = FLOW_DISSECTOR_KEY_IPV6_ADDRS,
		.offset = offsetof(struct flow_keys, addrs.v6addrs),
	},
	{
		.key_id = FLOW_DISSECTOR_KEY_PORTS,
		.offset = offsetof(struct flow_keys, ports),
	},
};

static const struct flow_dissector_key flow_keys_basic_dissector_keys[] = {
	{
		.key_id = FLOW_DISSECTOR_KEY_CONTROL,
		.offset = offsetof(struct flow_keys, control),
	},
	{
		.key_id = FLOW_DISSECTOR_KEY_BASIC,
		.offset = offsetof(struct flow_keys, basic),
	},
};

struct flow_dissector flow_keys_dissector __read_mostly;
EXPORT_SYMBOL(flow_keys_dissector);

struct flow_dissector flow_keys_basic_dissector __read_mostly;
EXPORT_SYMBOL(flow_keys_basic_dissector);

static int __init init_default_flow_dissectors(void)
{
	skb_flow_dissector_init(&flow_keys_dissector,
				flow_keys_dissector_keys,
				ARRAY_SIZE(flow_keys_dissector_keys));
	skb_flow_dissector_init(&flow_keys_dissector_symmetric,
				flow_keys_dissector_symmetric_keys,
				ARRAY_SIZE(flow_keys_dissector_symmetric_keys));
	skb_flow_dissector_init(&flow_keys_basic_dissector,
				flow_keys_basic_dissector_keys,
				ARRAY_SIZE(flow_keys_basic_dissector_keys));
	return 0;
}
core_initcall(init_default_flow_dissectors);

/* Per-shape fast-path sysctls under /proc/sys/net/flow_dissector/. */
/* proc_handler for net.flow_dissector.vlan. Writes of 0 also clear
 * flow_dissector_qinq_key — QinQ depth-2 is meaningless when the
 * outer single-VLAN gate is off, so the slow path would have to
 * handle every VLAN-tagged packet anyway.
 */
static int proc_set_vlan_key(const struct ctl_table *table, int write,
			     void *buffer, size_t *lenp, loff_t *ppos)
{
	int ret;

	ret = proc_do_static_key(table, write, buffer, lenp, ppos);
	if (ret == 0 && write &&
	    !static_branch_unlikely(&flow_dissector_vlan_key) &&
	    static_branch_unlikely(&flow_dissector_qinq_key))
		static_branch_disable(&flow_dissector_qinq_key);
	return ret;
}

/* proc_handler for net.flow_dissector.qinq. Writes of 1 also enable
 * flow_dissector_vlan_key — QinQ extends the VLAN fast-path to depth
 * 2; it cannot fire when the depth-0 entry is gated off.
 */
static int proc_set_qinq_key(const struct ctl_table *table, int write,
			     void *buffer, size_t *lenp, loff_t *ppos)
{
	int ret;

	ret = proc_do_static_key(table, write, buffer, lenp, ppos);
	if (ret == 0 && write &&
	    static_branch_unlikely(&flow_dissector_qinq_key) &&
	    !static_branch_unlikely(&flow_dissector_vlan_key))
		static_branch_enable(&flow_dissector_vlan_key);
	return ret;
}

static struct ctl_table flow_dissector_sysctl_table[] = {
	{
		.procname	= "eth_ip",
		.data		= &flow_dissector_eth_ip_key.key,
		.maxlen		= sizeof(flow_dissector_eth_ip_key),
		.mode		= 0644,
		.proc_handler	= proc_do_static_key,
	},
	{
		.procname	= "vlan",
		.data		= &flow_dissector_vlan_key.key,
		.maxlen		= sizeof(flow_dissector_vlan_key),
		.mode		= 0644,
		.proc_handler	= proc_set_vlan_key,
	},
	{
		.procname	= "qinq",
		.data		= &flow_dissector_qinq_key.key,
		.maxlen		= sizeof(flow_dissector_qinq_key),
		.mode		= 0644,
		.proc_handler	= proc_set_qinq_key,
	},
	{
		.procname	= "pppoe",
		.data		= &flow_dissector_pppoe_key.key,
		.maxlen		= sizeof(flow_dissector_pppoe_key),
		.mode		= 0644,
		.proc_handler	= proc_do_static_key,
	},
	{
		.procname	= "mpls",
		.data		= &flow_dissector_mpls_key.key,
		.maxlen		= sizeof(flow_dissector_mpls_key),
		.mode		= 0644,
		.proc_handler	= proc_do_static_key,
	},
	{
		.procname	= "ipip",
		.data		= &flow_dissector_ipip_key.key,
		.maxlen		= sizeof(flow_dissector_ipip_key),
		.mode		= 0644,
		.proc_handler	= proc_do_static_key,
	},
	{
		.procname	= "gre",
		.data		= &flow_dissector_gre_key.key,
		.maxlen		= sizeof(flow_dissector_gre_key),
		.mode		= 0644,
		.proc_handler	= proc_do_static_key,
	},
};

static const char * const flow_dissector_shape_names[FLOW_DISSECTOR_SHAPE__MAX] = {
	[FLOW_DISSECTOR_SHAPE_ETH_IP] = "eth_ip",
	[FLOW_DISSECTOR_SHAPE_VLAN]   = "vlan",
	[FLOW_DISSECTOR_SHAPE_QINQ]   = "qinq",
	[FLOW_DISSECTOR_SHAPE_PPPOE]  = "pppoe",
	[FLOW_DISSECTOR_SHAPE_MPLS]   = "mpls",
	[FLOW_DISSECTOR_SHAPE_IPIP]   = "ipip",
	[FLOW_DISSECTOR_SHAPE_GRE]    = "gre",
};

static bool flow_dissector_shape_gate(enum flow_dissector_shape shape)
{
	switch (shape) {
	case FLOW_DISSECTOR_SHAPE_ETH_IP:
		return static_key_enabled(&flow_dissector_eth_ip_key);
	case FLOW_DISSECTOR_SHAPE_VLAN:
		return static_key_enabled(&flow_dissector_vlan_key);
	case FLOW_DISSECTOR_SHAPE_QINQ:
		return static_key_enabled(&flow_dissector_qinq_key);
	case FLOW_DISSECTOR_SHAPE_PPPOE:
		return static_key_enabled(&flow_dissector_pppoe_key);
	case FLOW_DISSECTOR_SHAPE_MPLS:
		return static_key_enabled(&flow_dissector_mpls_key);
	case FLOW_DISSECTOR_SHAPE_IPIP:
		return static_key_enabled(&flow_dissector_ipip_key);
	case FLOW_DISSECTOR_SHAPE_GRE:
		return static_key_enabled(&flow_dissector_gre_key);
	default:
		return false;
	}
}

/* /proc/net/flow_dissector_stats: per-shape occurrences (slow path),
 * fast_hits, and eligible% = (occurrences + fast_hits) / dissects --
 * the signal a policy layer thresholds against a per-shape
 * break-even.
 */
static int flow_dissector_stats_show(struct seq_file *seq, void *v)
{
	u64 occ[FLOW_DISSECTOR_SHAPE__MAX] = {};
	u64 fast[FLOW_DISSECTOR_SHAPE__MAX] = {};
	u64 dissects = 0;
	int cpu, i;

	for_each_possible_cpu(cpu) {
		const struct flow_dissector_stats *s =
			per_cpu_ptr(&flow_dissector_pcpu_stats, cpu);

		for (i = 0; i < FLOW_DISSECTOR_SHAPE__MAX; i++) {
			occ[i]  += READ_ONCE(s->occurrences[i]);
			fast[i] += READ_ONCE(s->fast_hits[i]);
		}
		dissects += READ_ONCE(s->dissects);
	}

	seq_printf(seq, "%-8s %16s %16s %10s %5s\n",
		   "shape", "occurrences", "fast_hits", "eligible%", "gate");
	for (i = 0; i < FLOW_DISSECTOR_SHAPE__MAX; i++) {
		u64 total = occ[i] + fast[i];
		u64 pct_x100 = dissects ? div64_u64(total * 10000, dissects) : 0;

		seq_printf(seq, "%-8s %16llu %16llu %7llu.%02llu %5s\n",
			   flow_dissector_shape_names[i], occ[i], fast[i],
			   pct_x100 / 100, pct_x100 % 100,
			   str_on_off(flow_dissector_shape_gate(i)));
	}
	seq_printf(seq, "dissects: %llu\n", dissects);
	return 0;
}

static int __init flow_dissector_sysctl_init(void)
{
	if (!register_net_sysctl(&init_net, "net/flow_dissector",
				 flow_dissector_sysctl_table))
		return -ENOMEM;

	/* Global (init_net) counters — matches the global gates; per-netns
	 * is a noted follow-up. A missing proc file is not fatal.
	 */
	proc_create_single("flow_dissector_stats", 0444, init_net.proc_net,
			   flow_dissector_stats_show);
	return 0;
}
late_initcall(flow_dissector_sysctl_init);
