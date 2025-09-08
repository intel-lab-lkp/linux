// SPDX-License-Identifier: GPL-2.0-only

#include <linux/virtio_admin.h>
#include <linux/virtio.h>
#include <net/ipv6.h>
#include <net/ip.h>
#include "virtio_net_ff.h"

#define VIRTNET_FF_ETHTOOL_GROUP_PRIORITY 1
#define VIRTNET_FF_MAX_GROUPS 1

struct virtnet_ethtool_rule {
	struct ethtool_rx_flow_spec flow_spec;
	u32 classifier_id;
};

/* New fields must be added before the classifier struct */
struct virtnet_classifier {
	size_t size;
	refcount_t refcount;
	u32 id;
	struct virtio_net_resource_obj_ff_classifier classifier;
};

static bool check_mask_vs_cap(const void *m, const void *c,
			      u16 len, bool partial)
{
	const u8 *mask = m;
	const u8 *cap = c;
	int i;

	for (i = 0; i < len; i++) {
		if (partial && ((mask[i] & cap[i]) != mask[i]))
			return false;
		if (!partial && mask[i] != cap[i])
			return false;
	}

	return true;
}

static
struct virtio_net_ff_selector *get_selector_cap(const struct virtnet_ff *ff,
						u8 selector_type)
{
	struct virtio_net_ff_selector *sel;
	u8 *buf;
	int i;

	buf = (u8 *)&ff->ff_mask->selectors;
	sel = (struct virtio_net_ff_selector *)buf;

	for (i = 0; i < ff->ff_mask->count; i++) {
		if (sel->type == selector_type)
			return sel;

		buf += sizeof(struct virtio_net_ff_selector) + sel->length;
		sel = (struct virtio_net_ff_selector *)buf;
	}

	return NULL;
}

static bool validate_eth_mask(const struct virtnet_ff *ff,
			      const struct virtio_net_ff_selector *sel,
			      const struct virtio_net_ff_selector *sel_cap)
{
	bool partial_mask = !!(sel_cap->flags & VIRTIO_NET_FF_MASK_F_PARTIAL_MASK);
	struct ethhdr *cap, *mask;
	struct ethhdr zeros = {0};

	cap = (struct ethhdr *)&sel_cap->mask;
	mask = (struct ethhdr *)&sel->mask;

	if (memcmp(&zeros.h_dest, mask->h_dest, sizeof(zeros.h_dest)) &&
	    !check_mask_vs_cap(mask->h_dest, cap->h_dest,
			       sizeof(mask->h_dest), partial_mask))
		return false;

	if (memcmp(&zeros.h_source, mask->h_source, sizeof(zeros.h_source)) &&
	    !check_mask_vs_cap(mask->h_source, cap->h_source,
			       sizeof(mask->h_source), partial_mask))
		return false;

	if (mask->h_proto &&
	    !check_mask_vs_cap(&mask->h_proto, &cap->h_proto,
			       sizeof(__be16), partial_mask))
		return false;

	return true;
}

static bool validate_ip4_mask(const struct virtnet_ff *ff,
			      const struct virtio_net_ff_selector *sel,
			      const struct virtio_net_ff_selector *sel_cap)
{
	bool partial_mask = !!(sel_cap->flags & VIRTIO_NET_FF_MASK_F_PARTIAL_MASK);
	struct iphdr *cap, *mask;

	cap = (struct iphdr *)&sel_cap->mask;
	mask = (struct iphdr *)&sel->mask;

	if (mask->saddr &&
	    !check_mask_vs_cap(&mask->saddr, &cap->saddr,
	    sizeof(__be32), partial_mask))
		return false;

	if (mask->daddr &&
	    !check_mask_vs_cap(&mask->daddr, &cap->daddr,
	    sizeof(__be32), partial_mask))
		return false;

	if (mask->protocol &&
	    !check_mask_vs_cap(&mask->protocol, &cap->protocol,
	    sizeof(u8), partial_mask))
		return false;

	return true;
}

static bool validate_ip6_mask(const struct virtnet_ff *ff,
			      const struct virtio_net_ff_selector *sel,
			      const struct virtio_net_ff_selector *sel_cap)
{
	bool partial_mask = !!(sel_cap->flags & VIRTIO_NET_FF_MASK_F_PARTIAL_MASK);
	struct ipv6hdr *cap, *mask;

	cap = (struct ipv6hdr *)&sel_cap->mask;
	mask = (struct ipv6hdr *)&sel->mask;

	if (!ipv6_addr_any(&mask->saddr) &&
	    !check_mask_vs_cap(&mask->saddr, &cap->saddr,
			       sizeof(cap->saddr), partial_mask))
		return false;

	if (!ipv6_addr_any(&mask->daddr) &&
	    !check_mask_vs_cap(&mask->daddr, &cap->daddr,
			       sizeof(cap->daddr), partial_mask))
		return false;

	if (mask->nexthdr &&
	    !check_mask_vs_cap(&mask->nexthdr, &cap->nexthdr,
	    sizeof(cap->nexthdr), partial_mask))
		return false;

	return true;
}

static bool validate_tcp_mask(const struct virtnet_ff *ff,
			      const struct virtio_net_ff_selector *sel,
			      const struct virtio_net_ff_selector *sel_cap)
{
	bool partial_mask = !!(sel_cap->flags & VIRTIO_NET_FF_MASK_F_PARTIAL_MASK);
	struct tcphdr *cap, *mask;

	cap = (struct tcphdr *)&sel_cap->mask;
	mask = (struct tcphdr *)&sel->mask;

	if (mask->source &&
	    !check_mask_vs_cap(&mask->source, &cap->source,
	    sizeof(cap->source), partial_mask))
		return false;

	if (mask->dest &&
	    !check_mask_vs_cap(&mask->dest, &cap->dest,
	    sizeof(cap->dest), partial_mask))
		return false;

	return true;
}

static bool validate_udp_mask(const struct virtnet_ff *ff,
			      const struct virtio_net_ff_selector *sel,
			      const struct virtio_net_ff_selector *sel_cap)
{
	bool partial_mask = !!(sel_cap->flags & VIRTIO_NET_FF_MASK_F_PARTIAL_MASK);
	struct udphdr *cap, *mask;

	cap = (struct udphdr *)&sel_cap->mask;
	mask = (struct udphdr *)&sel->mask;

	if (mask->source &&
	    !check_mask_vs_cap(&mask->source, &cap->source,
	    sizeof(cap->source), partial_mask))
		return false;

	if (mask->dest &&
	    !check_mask_vs_cap(&mask->dest, &cap->dest,
	    sizeof(cap->dest), partial_mask))
		return false;

	return true;
}

static bool validate_mask(const struct virtnet_ff *ff,
			  const struct virtio_net_ff_selector *sel)
{
	struct virtio_net_ff_selector *sel_cap = get_selector_cap(ff, sel->type);

	if (!sel_cap)
		return false;

	switch (sel->type) {
	case VIRTIO_NET_FF_MASK_TYPE_ETH:
		return validate_eth_mask(ff, sel, sel_cap);

	case VIRTIO_NET_FF_MASK_TYPE_IPV4:
		return validate_ip4_mask(ff, sel, sel_cap);

	case VIRTIO_NET_FF_MASK_TYPE_IPV6:
		return validate_ip6_mask(ff, sel, sel_cap);

	case VIRTIO_NET_FF_MASK_TYPE_TCP:
		return validate_tcp_mask(ff, sel, sel_cap);

	case VIRTIO_NET_FF_MASK_TYPE_UDP:
		return validate_udp_mask(ff, sel, sel_cap);
	}

	return false;
}

static void set_tcp(struct tcphdr *mask, struct tcphdr *key,
		    __be16 psrc_m, __be16 psrc_k,
		    __be16 pdst_m, __be16 pdst_k)
{
	if (psrc_m) {
		mask->source = psrc_m;
		key->source = psrc_k;
	}
	if (pdst_m) {
		mask->dest = pdst_m;
		key->dest = pdst_k;
	}
}

static void set_udp(struct udphdr *mask, struct udphdr *key,
		    __be16 psrc_m, __be16 psrc_k,
		    __be16 pdst_m, __be16 pdst_k)
{
	if (psrc_m) {
		mask->source = psrc_m;
		key->source = psrc_k;
	}
	if (pdst_m) {
		mask->dest = pdst_m;
		key->dest = pdst_k;
	}
}

static void parse_ip4(struct iphdr *mask, struct iphdr *key,
		      const struct ethtool_rx_flow_spec *fs)
{
	const struct ethtool_usrip4_spec *l3_mask = &fs->m_u.usr_ip4_spec;
	const struct ethtool_usrip4_spec *l3_val  = &fs->h_u.usr_ip4_spec;

	mask->saddr = l3_mask->ip4src;
	mask->daddr = l3_mask->ip4dst;
	key->saddr = l3_val->ip4src;
	key->daddr = l3_val->ip4dst;

	if (mask->protocol) {
		mask->protocol = l3_mask->proto;
		key->protocol = l3_val->proto;
	}
}

static void parse_ip6(struct ipv6hdr *mask, struct ipv6hdr *key,
		      const struct ethtool_rx_flow_spec *fs)
{
	const struct ethtool_usrip6_spec *l3_mask = &fs->m_u.usr_ip6_spec;
	const struct ethtool_usrip6_spec *l3_val  = &fs->h_u.usr_ip6_spec;

	if (!ipv6_addr_any((struct in6_addr *)l3_mask->ip6src)) {
		memcpy(&mask->saddr, l3_mask->ip6src, sizeof(mask->saddr));
		memcpy(&key->saddr, l3_val->ip6src, sizeof(key->saddr));
	}

	if (!ipv6_addr_any((struct in6_addr *)l3_mask->ip6dst)) {
		memcpy(&mask->daddr, l3_mask->ip6dst, sizeof(mask->daddr));
		memcpy(&key->daddr, l3_val->ip6dst, sizeof(key->daddr));
	}

	if (l3_mask->l4_proto) {
		mask->nexthdr = l3_mask->l4_proto;
		key->nexthdr = l3_val->l4_proto;
	}
}

static bool has_ipv4(u32 flow_type)
{
	return flow_type == TCP_V4_FLOW ||
	       flow_type == UDP_V4_FLOW ||
	       flow_type == IP_USER_FLOW;
}

static bool has_ipv6(u32 flow_type)
{
	return flow_type == TCP_V6_FLOW ||
	       flow_type == UDP_V6_FLOW ||
	       flow_type == IPV6_USER_FLOW;
}

static bool has_tcp(u32 flow_type)
{
	return flow_type == TCP_V4_FLOW || flow_type == TCP_V6_FLOW;
}

static bool has_udp(u32 flow_type)
{
	return flow_type == UDP_V4_FLOW || flow_type == UDP_V6_FLOW;
}

static int setup_classifier(struct virtnet_ff *ff,
			    struct virtnet_classifier **c)
{
	struct virtnet_classifier *tmp;
	unsigned long i;
	int err;

	xa_for_each(&ff->classifiers, i, tmp) {
		if ((*c)->size == tmp->size &&
		    !memcmp(&tmp->classifier, &(*c)->classifier, tmp->size)) {
			refcount_inc(&tmp->refcount);
			kfree(*c);
			*c = tmp;
			goto out;
		}
	}

	err = xa_alloc(&ff->classifiers, &(*c)->id, *c,
		       XA_LIMIT(0, le32_to_cpu(ff->ff_caps->classifiers_limit) - 1),
		       GFP_KERNEL);
	if (err)
		return err;

	err = virtio_device_object_create(ff->vdev,
					  VIRTIO_NET_RESOURCE_OBJ_FF_CLASSIFIER,
					  (*c)->id,
					  &(*c)->classifier,
					  (*c)->size);
	if (err)
		goto err_xarray;

	refcount_set(&(*c)->refcount, 1);
out:
	return 0;

err_xarray:
	xa_erase(&ff->classifiers, (*c)->id);

	return err;
}

static void try_destroy_classifier(struct virtnet_ff *ff, u32 classifier_id)
{
	struct virtnet_classifier *c;

	c = xa_load(&ff->classifiers, classifier_id);
	if (c && refcount_dec_and_test(&c->refcount)) {
		virtio_device_object_destroy(ff->vdev,
					     VIRTIO_NET_RESOURCE_OBJ_FF_CLASSIFIER,
					     c->id);

		xa_erase(&ff->classifiers, c->id);
		kfree(c);
	}
}

static void destroy_ethtool_rule(struct virtnet_ff *ff,
				 struct virtnet_ethtool_rule *eth_rule)
{
	ff->ethtool.num_rules--;

	virtio_device_object_destroy(ff->vdev,
				     VIRTIO_NET_RESOURCE_OBJ_FF_RULE,
				     eth_rule->flow_spec.location);

	xa_erase(&ff->ethtool.rules, eth_rule->flow_spec.location);
	try_destroy_classifier(ff, eth_rule->classifier_id);
	kfree(eth_rule);
}

static int insert_rule(struct virtnet_ff *ff,
		       struct virtnet_ethtool_rule *eth_rule,
		       u32 classifier_id,
		       const u8 *key,
		       size_t key_size)
{
	struct ethtool_rx_flow_spec *fs = &eth_rule->flow_spec;
	struct virtio_net_resource_obj_ff_rule *ff_rule;
	int err;

	ff_rule = kzalloc(sizeof(*ff_rule) + key_size, GFP_KERNEL);
	if (!ff_rule) {
		err = -ENOMEM;
		goto err_eth_rule;
	}
	/*
	 * Intentionally leave the priority as 0. All rules have the same
	 * priority.
	 */
	ff_rule->group_id = cpu_to_le32(VIRTNET_FF_ETHTOOL_GROUP_PRIORITY);
	ff_rule->classifier_id = cpu_to_le32(classifier_id);
	ff_rule->key_length = (u8)key_size;
	ff_rule->action = fs->ring_cookie == RX_CLS_FLOW_DISC ?
					     VIRTIO_NET_FF_ACTION_DROP :
					     VIRTIO_NET_FF_ACTION_RX_VQ;
	ff_rule->vq_index = fs->ring_cookie != RX_CLS_FLOW_DISC ?
					       cpu_to_le16(fs->ring_cookie) : 0;
	memcpy(&ff_rule->keys, key, key_size);

	err = virtio_device_object_create(ff->vdev,
					  VIRTIO_NET_RESOURCE_OBJ_FF_RULE,
					  fs->location,
					  ff_rule,
					  sizeof(*ff_rule) + key_size);
	if (err)
		goto err_ff_rule;

	eth_rule->classifier_id = classifier_id;
	ff->ethtool.num_rules++;
	kfree(ff_rule);

	return 0;

err_ff_rule:
	kfree(ff_rule);
err_eth_rule:
	xa_erase(&ff->ethtool.rules, eth_rule->flow_spec.location);
	kfree(eth_rule);

	return err;
}

static u32 flow_type_mask(u32 flow_type)
{
	return flow_type & ~(FLOW_EXT | FLOW_MAC_EXT | FLOW_RSS);
}

static bool supported_flow_type(const struct ethtool_rx_flow_spec *fs)
{
	switch (fs->flow_type) {
	case ETHER_FLOW:
	case IP_USER_FLOW:
	case IPV6_USER_FLOW:
	case TCP_V4_FLOW:
	case TCP_V6_FLOW:
	case UDP_V4_FLOW:
	case UDP_V6_FLOW:
		return true;
	}

	return false;
}

static int validate_flow_input(struct virtnet_ff *ff,
			       const struct ethtool_rx_flow_spec *fs,
			       u16 curr_queue_pairs)
{
	/* Force users to use RX_CLS_LOC_ANY - don't allow specific locations */
	if (fs->location != RX_CLS_LOC_ANY)
		return -EOPNOTSUPP;

	if (fs->ring_cookie != RX_CLS_FLOW_DISC &&
	    fs->ring_cookie >= curr_queue_pairs)
		return -EINVAL;

	if (fs->flow_type != flow_type_mask(fs->flow_type))
		return -EOPNOTSUPP;

	if (!supported_flow_type(fs))
		return -EOPNOTSUPP;
	return 0;
}

static void calculate_flow_sizes(struct ethtool_rx_flow_spec *fs,
				size_t *key_size, size_t *classifier_size,
				int *num_hdrs)
{
	size_t size = sizeof(struct ethhdr);

	*num_hdrs = 1;
	*key_size = sizeof(struct ethhdr);

	if (fs->flow_type == ETHER_FLOW)
		goto done;

	(*num_hdrs)++;
	if (has_ipv4(fs->flow_type))
		size += sizeof(struct iphdr);
	else if (has_ipv6(fs->flow_type))
		size += sizeof(struct ipv6hdr);

	if (has_tcp(fs->flow_type) || has_udp(fs->flow_type)) {
		(*num_hdrs)++;
		size += has_tcp(fs->flow_type) ? sizeof(struct tcphdr) :
						 sizeof(struct udphdr);
	}
done:
	*key_size = size;
	/*
	 * The classifier size is the size of the classifier header, a selector
	 * header for each type of header in the match criteria, and each header
	 * providing the mask for matching against.
	 */
	*classifier_size = *key_size +
			   sizeof(struct virtio_net_resource_obj_ff_classifier) +
			   sizeof(struct virtio_net_ff_selector) * (*num_hdrs);
}

static void setup_eth_hdr_key_mask(struct virtio_net_ff_selector *selector,
				  u8 *key,
				  const struct ethtool_rx_flow_spec *fs,
				  int num_hdrs)
{
	struct ethhdr *eth_m = (struct ethhdr *)&selector->mask;
	struct ethhdr *eth_k = (struct ethhdr *)key;

	selector->type = VIRTIO_NET_FF_MASK_TYPE_ETH;
	selector->length = sizeof(struct ethhdr);

	if (num_hdrs > 1) {
		eth_m->h_proto = cpu_to_be16(0xffff);
		eth_k->h_proto = cpu_to_be16(ETH_P_IP);
	} else {
		memcpy(eth_m, &fs->m_u.ether_spec, sizeof(*eth_m));
		memcpy(eth_k, &fs->h_u.ether_spec, sizeof(*eth_k));
	}
}

static int setup_ip_key_mask(struct virtio_net_ff_selector *selector,
			     u8 *key,
			     const struct ethtool_rx_flow_spec *fs,
			     int num_hdrs)
{
	struct ipv6hdr *v6_m = (struct ipv6hdr *)&selector->mask;
	struct iphdr *v4_m = (struct iphdr *)&selector->mask;
	struct ipv6hdr *v6_k = (struct ipv6hdr *)key;
	struct iphdr *v4_k = (struct iphdr *)key;

	if (has_ipv6(fs->flow_type)) {
		selector->type = VIRTIO_NET_FF_MASK_TYPE_IPV6;
		selector->length = sizeof(struct ipv6hdr);

		if (num_hdrs == 2 && (fs->h_u.usr_ip6_spec.l4_4_bytes ||
				      fs->h_u.usr_ip6_spec.tclass))
			return -EOPNOTSUPP;

		parse_ip6(v6_m, v6_k, fs);

		if (num_hdrs > 2) {
			v6_m->nexthdr = 0xff;
			if (has_tcp(fs->flow_type))
				v6_k->nexthdr = IPPROTO_TCP;
			else
				v6_k->nexthdr = IPPROTO_UDP;
		}
	} else {
		selector->type = VIRTIO_NET_FF_MASK_TYPE_IPV4;
		selector->length = sizeof(struct iphdr);

		if (num_hdrs == 2 &&
		    (fs->h_u.usr_ip4_spec.l4_4_bytes ||
		     fs->h_u.usr_ip4_spec.tos ||
		     fs->h_u.usr_ip4_spec.ip_ver != ETH_RX_NFC_IP4))
			return -EOPNOTSUPP;

		parse_ip4(v4_m, v4_k, fs);

		if (num_hdrs > 2) {
			v4_m->protocol = 0xff;
			if (has_tcp(fs->flow_type))
				v4_k->protocol = IPPROTO_TCP;
			else
				v4_k->protocol = IPPROTO_UDP;
		}
	}

	return 0;
}

static int setup_transport_key_mask(struct virtio_net_ff_selector *selector,
				    u8 *key,
				    struct ethtool_rx_flow_spec *fs)
{
	struct tcphdr *tcp_m = (struct tcphdr *)&selector->mask;
	struct udphdr *udp_m = (struct udphdr *)&selector->mask;
	const struct ethtool_tcpip6_spec *v6_l4_mask;
	const struct ethtool_tcpip4_spec *v4_l4_mask;
	const struct ethtool_tcpip6_spec *v6_l4_key;
	const struct ethtool_tcpip4_spec *v4_l4_key;
	struct tcphdr *tcp_k = (struct tcphdr *)key;
	struct udphdr *udp_k = (struct udphdr *)key;

	if (has_tcp(fs->flow_type)) {
		selector->type = VIRTIO_NET_FF_MASK_TYPE_TCP;
		selector->length = sizeof(struct tcphdr);

		if (has_ipv6(fs->flow_type)) {
			v6_l4_mask = &fs->m_u.tcp_ip6_spec;
			v6_l4_key = &fs->h_u.tcp_ip6_spec;

			set_tcp(tcp_m, tcp_k, v6_l4_mask->psrc, v6_l4_key->psrc,
				v6_l4_mask->pdst, v6_l4_key->pdst);
		} else {
			v4_l4_mask = &fs->m_u.tcp_ip4_spec;
			v4_l4_key = &fs->h_u.tcp_ip4_spec;

			set_tcp(tcp_m, tcp_k, v4_l4_mask->psrc, v4_l4_key->psrc,
				v4_l4_mask->pdst, v4_l4_key->pdst);
		}

	} else if (has_udp(fs->flow_type)) {
		selector->type = VIRTIO_NET_FF_MASK_TYPE_UDP;
		selector->length = sizeof(struct udphdr);

		if (has_ipv6(fs->flow_type)) {
			v6_l4_mask = &fs->m_u.udp_ip6_spec;
			v6_l4_key = &fs->h_u.udp_ip6_spec;

			set_udp(udp_m, udp_k, v6_l4_mask->psrc, v6_l4_key->psrc,
				v6_l4_mask->pdst, v6_l4_key->pdst);
		} else {
			v4_l4_mask = &fs->m_u.udp_ip4_spec;
			v4_l4_key = &fs->h_u.udp_ip4_spec;

			set_udp(udp_m, udp_k, v4_l4_mask->psrc, v4_l4_key->psrc,
				v4_l4_mask->pdst, v4_l4_key->pdst);
		}
	} else {
		return -EOPNOTSUPP;
	}

	return 0;
}

static int
validate_classifier_selectors(struct virtnet_ff *ff,
			      struct virtio_net_resource_obj_ff_classifier *classifier,
			      int num_hdrs)
{
	struct virtio_net_ff_selector *selector = classifier->selectors;

	for (int i = 0; i < num_hdrs; i++) {
		if (!validate_mask(ff, selector))
			return -EINVAL;

		selector = (struct virtio_net_ff_selector *)(((u8 *)selector) +
			    sizeof(*selector) + selector->length);
	}

	return 0;
}

static
struct virtio_net_ff_selector *next_selector(struct virtio_net_ff_selector *sel)
{
	void *nextsel;

	nextsel = (u8 *)sel + sizeof(struct virtio_net_ff_selector) +
		  sel->length;

	return nextsel;
}

static int build_and_insert(struct virtnet_ff *ff,
			    struct virtnet_ethtool_rule *eth_rule)
{
	struct virtio_net_resource_obj_ff_classifier *classifier;
	struct ethtool_rx_flow_spec *fs = &eth_rule->flow_spec;
	struct virtio_net_ff_selector *selector;
	struct virtnet_classifier *c;
	size_t classifier_size;
	size_t key_offset;
	size_t key_size;
	int num_hdrs;
	u8 *key;
	int err;

	calculate_flow_sizes(fs, &key_size, &classifier_size, &num_hdrs);

	key = kzalloc(key_size, GFP_KERNEL);
	if (!key)
		return -ENOMEM;

	/*
	 * virtio_net_ff_obj_ff_classifier is already included in the
	 * classifier_size.
	 */
	c = kzalloc(classifier_size +
		    sizeof(struct virtnet_classifier) -
		    sizeof(struct virtio_net_resource_obj_ff_classifier),
		    GFP_KERNEL);
	if (!c) {
		kfree(key);
		return -ENOMEM;
	}

	c->size = classifier_size;
	classifier = &c->classifier;
	classifier->count = num_hdrs;
	selector = &classifier->selectors[0];

	setup_eth_hdr_key_mask(selector, key, fs, num_hdrs);
	if (num_hdrs == 1)
		goto validate;

	key_offset = selector->length;
	selector = next_selector(selector);

	err = setup_ip_key_mask(selector, key + key_offset, fs, num_hdrs);
	if (err)
		goto err_classifier;

	if (num_hdrs == 2)
		goto validate;

	key_offset += selector->length;
	selector = next_selector(selector);

	err = setup_transport_key_mask(selector, key + key_offset, fs);
	if (err)
		goto err_classifier;

validate:
	err = validate_classifier_selectors(ff, classifier, num_hdrs);
	if (err)
		goto err_key;

	err = setup_classifier(ff, &c);
	if (err)
		goto err_classifier;

	err = insert_rule(ff, eth_rule, c->id, key, key_size);
	if (err) {
		try_destroy_classifier(ff, c->id);
		goto err_key;
	}

	return 0;

err_classifier:
	kfree(c);
err_key:
	kfree(key);

	return err;
}

int virtnet_ethtool_flow_insert(struct virtnet_ff *ff,
				struct ethtool_rx_flow_spec *fs,
				u16 curr_queue_pairs)
{
	struct virtnet_ethtool_rule *eth_rule;
	int err;

	if (!ff->ff_supported)
		return -EOPNOTSUPP;

	err = validate_flow_input(ff, fs, curr_queue_pairs);
	if (err)
		return err;

	eth_rule = kzalloc(sizeof(*eth_rule), GFP_KERNEL);
	if (!eth_rule)
		return -ENOMEM;

	err = xa_alloc(&ff->ethtool.rules, &fs->location, eth_rule,
		       XA_LIMIT(0, le32_to_cpu(ff->ff_caps->rules_limit) - 1),
		       GFP_KERNEL);
	if (err)
		goto err_rule;

	eth_rule->flow_spec = *fs;

	err = build_and_insert(ff, eth_rule);
	if (err)
		goto err_xa;

	return err;

err_xa:
	xa_erase(&ff->ethtool.rules, eth_rule->flow_spec.location);

err_rule:
	fs->location = RX_CLS_LOC_ANY;
	kfree(eth_rule);

	return err;
}

int virtnet_ethtool_flow_remove(struct virtnet_ff *ff, int location)
{
	struct virtnet_ethtool_rule *eth_rule;
	int err = 0;

	if (!ff->ff_supported)
		return -EOPNOTSUPP;

	eth_rule = xa_load(&ff->ethtool.rules, location);
	if (!eth_rule) {
		err = -ENOENT;
		goto out;
	}

	destroy_ethtool_rule(ff, eth_rule);
out:
	return err;
}

static size_t get_mask_size(u16 type)
{
	switch (type) {
	case VIRTIO_NET_FF_MASK_TYPE_ETH:
		return sizeof(struct ethhdr);
	case VIRTIO_NET_FF_MASK_TYPE_IPV4:
		return sizeof(struct iphdr);
	case VIRTIO_NET_FF_MASK_TYPE_IPV6:
		return sizeof(struct ipv6hdr);
	case VIRTIO_NET_FF_MASK_TYPE_TCP:
		return sizeof(struct tcphdr);
	case VIRTIO_NET_FF_MASK_TYPE_UDP:
		return sizeof(struct udphdr);
	}

	return 0;
}

void virtnet_ff_init(struct virtnet_ff *ff, struct virtio_device *vdev)
{
	struct virtio_admin_cmd_query_cap_id_result *cap_id_list __free(kfree) = NULL;
	size_t ff_mask_size = sizeof(struct virtio_net_ff_cap_mask_data) +
			      sizeof(struct virtio_net_ff_selector) *
			      VIRTIO_NET_FF_MASK_TYPE_MAX;
	struct virtio_net_resource_obj_ff_group ethtool_group = {};
	struct virtio_net_ff_selector *sel;
	int err;
	int i;

	cap_id_list = kzalloc(sizeof(*cap_id_list), GFP_KERNEL);
	if (!cap_id_list)
		return;

	err = virtio_device_cap_id_list_query(vdev, cap_id_list);
	if (err)
		return;

	if (!(VIRTIO_CAP_IN_LIST(cap_id_list,
				 VIRTIO_NET_FF_RESOURCE_CAP) &&
	      VIRTIO_CAP_IN_LIST(cap_id_list,
				 VIRTIO_NET_FF_SELECTOR_CAP) &&
	      VIRTIO_CAP_IN_LIST(cap_id_list,
				 VIRTIO_NET_FF_ACTION_CAP)))
		return;

	ff->ff_caps = kzalloc(sizeof(*ff->ff_caps), GFP_KERNEL);
	if (!ff->ff_caps)
		return;

	err = virtio_device_cap_get(vdev,
				    VIRTIO_NET_FF_RESOURCE_CAP,
				    ff->ff_caps,
				    sizeof(*ff->ff_caps));

	if (err)
		goto err_ff;

	/* VIRTIO_NET_FF_MASK_TYPE start at 1 */
	for (i = 1; i <= VIRTIO_NET_FF_MASK_TYPE_MAX; i++)
		ff_mask_size += get_mask_size(i);

	ff->ff_mask = kzalloc(ff_mask_size, GFP_KERNEL);
	if (!ff->ff_mask)
		goto err_ff;

	err = virtio_device_cap_get(vdev,
				    VIRTIO_NET_FF_SELECTOR_CAP,
				    ff->ff_mask,
				    ff_mask_size);

	if (err)
		goto err_ff_mask;

	ff->ff_actions = kzalloc(sizeof(*ff->ff_actions) +
					VIRTIO_NET_FF_ACTION_MAX,
					GFP_KERNEL);
	if (!ff->ff_actions)
		goto err_ff_mask;

	err = virtio_device_cap_get(vdev,
				    VIRTIO_NET_FF_ACTION_CAP,
				    ff->ff_actions,
				    sizeof(*ff->ff_actions) + VIRTIO_NET_FF_ACTION_MAX);

	if (err)
		goto err_ff_action;

	if (le32_to_cpu(ff->ff_caps->groups_limit) < VIRTNET_FF_MAX_GROUPS) {
		err = -ENOSPC;
		goto err_ff_action;
	}
	ff->ff_caps->groups_limit = cpu_to_le32(VIRTNET_FF_MAX_GROUPS);

	err = virtio_device_cap_set(vdev,
				    VIRTIO_NET_FF_RESOURCE_CAP,
				    ff->ff_caps,
				    sizeof(*ff->ff_caps));
	if (err)
		goto err_ff_action;

	ff_mask_size = sizeof(struct virtio_net_ff_cap_mask_data);
	sel = &ff->ff_mask->selectors[0];

	for (int i = 0; i < ff->ff_mask->count; i++) {
		ff_mask_size += sizeof(struct virtio_net_ff_selector) + sel->length;
		sel = (struct virtio_net_ff_selector *)((u8 *)sel + sizeof(*sel) + sel->length);
	}

	err = virtio_device_cap_set(vdev,
				    VIRTIO_NET_FF_SELECTOR_CAP,
				    ff->ff_mask,
				    ff_mask_size);
	if (err)
		goto err_ff_action;

	err = virtio_device_cap_set(vdev,
				    VIRTIO_NET_FF_ACTION_CAP,
				    ff->ff_actions,
				    sizeof(*ff->ff_actions) + VIRTIO_NET_FF_ACTION_MAX);
	if (err)
		goto err_ff_action;

	ethtool_group.group_priority = cpu_to_le16(VIRTNET_FF_ETHTOOL_GROUP_PRIORITY);

	/* Use priority for the object ID. */
	err = virtio_device_object_create(vdev,
					  VIRTIO_NET_RESOURCE_OBJ_FF_GROUP,
					  VIRTNET_FF_ETHTOOL_GROUP_PRIORITY,
					  &ethtool_group,
					  sizeof(ethtool_group));
	if (err)
		goto err_ff_action;

	xa_init_flags(&ff->classifiers, XA_FLAGS_ALLOC);
	xa_init_flags(&ff->ethtool.rules, XA_FLAGS_ALLOC);
	ff->vdev = vdev;
	ff->ff_supported = true;

	return;

err_ff_action:
	kfree(ff->ff_actions);
err_ff_mask:
	kfree(ff->ff_mask);
err_ff:
	kfree(ff->ff_caps);
}

void virtnet_ff_cleanup(struct virtnet_ff *ff)
{
	struct virtnet_ethtool_rule *eth_rule;
	unsigned long i;

	if (!ff->ff_supported)
		return;

	xa_for_each(&ff->ethtool.rules, i, eth_rule)
		destroy_ethtool_rule(ff, eth_rule);

	xa_destroy(&ff->ethtool.rules);
	xa_destroy(&ff->classifiers);

	virtio_device_object_destroy(ff->vdev,
				     VIRTIO_NET_RESOURCE_OBJ_FF_GROUP,
				     VIRTNET_FF_ETHTOOL_GROUP_PRIORITY);

	kfree(ff->ff_actions);
	kfree(ff->ff_mask);
	kfree(ff->ff_caps);
}
