/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TUN_VNET_H
#define TUN_VNET_H

/* High bits in flags field are unused. */
#define TUN_VNET_LE     0x80000000
#define TUN_VNET_BE     0x40000000

typedef struct virtio_net_hash *(*tun_vnet_hash_add)(struct sk_buff *);
typedef const struct virtio_net_hash *(*tun_vnet_hash_find)(const struct sk_buff *);

struct tun_vnet_hash {
	bool report;
	bool rss;
	struct tun_vnet_rss common;
	u32 rss_key[VIRTIO_NET_RSS_MAX_KEY_SIZE];
	u16 rss_indirection_table[];
};

static inline bool tun_vnet_legacy_is_little_endian(unsigned int flags)
{
	bool be = IS_ENABLED(CONFIG_TUN_VNET_CROSS_LE) &&
		  (flags & TUN_VNET_BE);

	return !be && virtio_legacy_is_little_endian();
}

static inline long tun_get_vnet_be(unsigned int flags, int __user *argp)
{
	int be = !!(flags & TUN_VNET_BE);

	if (!IS_ENABLED(CONFIG_TUN_VNET_CROSS_LE))
		return -EINVAL;

	if (put_user(be, argp))
		return -EFAULT;

	return 0;
}

static inline long tun_set_vnet_be(unsigned int *flags, int __user *argp)
{
	int be;

	if (!IS_ENABLED(CONFIG_TUN_VNET_CROSS_LE))
		return -EINVAL;

	if (get_user(be, argp))
		return -EFAULT;

	if (be)
		*flags |= TUN_VNET_BE;
	else
		*flags &= ~TUN_VNET_BE;

	return 0;
}

static inline bool tun_vnet_is_little_endian(unsigned int flags)
{
	return flags & TUN_VNET_LE || tun_vnet_legacy_is_little_endian(flags);
}

static inline u16 tun_vnet16_to_cpu(unsigned int flags, __virtio16 val)
{
	return __virtio16_to_cpu(tun_vnet_is_little_endian(flags), val);
}

static inline __virtio16 cpu_to_tun_vnet16(unsigned int flags, u16 val)
{
	return __cpu_to_virtio16(tun_vnet_is_little_endian(flags), val);
}

static inline long tun_vnet_ioctl(int *vnet_hdr_sz, unsigned int *flags,
				  unsigned int cmd, int __user *sp)
{
	int s;

	switch (cmd) {
	case TUNGETVNETHDRSZ:
		s = *vnet_hdr_sz;
		if (put_user(s, sp))
			return -EFAULT;
		return 0;

	case TUNSETVNETHDRSZ:
		if (get_user(s, sp))
			return -EFAULT;
		if (s < (int)sizeof(struct virtio_net_hdr))
			return -EINVAL;

		*vnet_hdr_sz = s;
		return 0;

	case TUNGETVNETLE:
		s = !!(*flags & TUN_VNET_LE);
		if (put_user(s, sp))
			return -EFAULT;
		return 0;

	case TUNSETVNETLE:
		if (get_user(s, sp))
			return -EFAULT;
		if (s)
			*flags |= TUN_VNET_LE;
		else
			*flags &= ~TUN_VNET_LE;
		return 0;

	case TUNGETVNETBE:
		return tun_get_vnet_be(*flags, sp);

	case TUNSETVNETBE:
		return tun_set_vnet_be(flags, sp);

	default:
		return -EINVAL;
	}
}

static inline long tun_vnet_ioctl_gethashtypes(u32 __user *argp)
{
	return put_user(VIRTIO_NET_SUPPORTED_HASH_TYPES, argp) ? -EFAULT : 0;
}

static inline long tun_vnet_ioctl_sethash(struct tun_vnet_hash __rcu **hashp,
					  unsigned int cmd,
					  void __user *argp)
{
	struct tun_vnet_rss common;
	struct tun_vnet_hash *hash;
	size_t indirection_table_size;
	size_t key_size;
	size_t size;

	switch (cmd) {
	case TUNSETVNETREPORTINGAUTOMQ:
		if (get_user(common.hash_types, (u32 __user *)argp))
			return -EFAULT;

		if (common.hash_types) {
			hash = kzalloc(sizeof(*hash), GFP_KERNEL);
			if (!hash)
				return -ENOMEM;

			hash->report = true;
			hash->common.hash_types = common.hash_types;
		} else {
			hash = NULL;
		}
		break;

	case TUNSETVNETREPORTINGRSS:
	case TUNSETVNETRSS:
		if (copy_from_user(&common, argp, sizeof(common)))
			return -EFAULT;
		argp = (struct tun_vnet_rss __user *)argp + 1;

		indirection_table_size = ((size_t)common.indirection_table_mask + 1) * 2;
		key_size = virtio_net_hash_key_length(common.hash_types);
		size = struct_size(hash, rss_indirection_table,
				   (size_t)common.indirection_table_mask + 1);

		hash = kmalloc(size, GFP_KERNEL);
		if (!hash)
			return -ENOMEM;

		if (copy_from_user(hash->rss_indirection_table,
				   argp, indirection_table_size)) {
			kfree(hash);
			return -EFAULT;
		}
		argp = (u16 __user *)argp + common.indirection_table_mask + 1;

		if (copy_from_user(hash->rss_key, argp, key_size)) {
			kfree(hash);
			return -EFAULT;
		}

		virtio_net_toeplitz_convert_key(hash->rss_key, key_size);
		hash->report = cmd == TUNSETVNETREPORTINGRSS;
		hash->rss = true;
		hash->common = common;
		break;

	default:
		return -EINVAL;
	}

	kfree_rcu_mightsleep(rcu_replace_pointer_rtnl(*hashp, hash));
	return 0;
}

static inline void tun_vnet_hash_report(const struct tun_vnet_hash *hash,
					struct sk_buff *skb,
					const struct flow_keys_basic *keys,
					u32 value,
					tun_vnet_hash_add vnet_hash_add)
{
	struct virtio_net_hash *report;

	if (!hash || !hash->report)
		return;

	report = vnet_hash_add(skb);
	if (!report)
		return;

	*report = (struct virtio_net_hash) {
		.report = virtio_net_hash_report(hash->common.hash_types, keys),
		.value = value
	};
}

static inline u16 tun_vnet_rss_select_queue(u32 numqueues,
					    const struct tun_vnet_hash *hash,
					    struct sk_buff *skb,
					    tun_vnet_hash_add vnet_hash_add)
{
	struct virtio_net_hash *report;
	struct virtio_net_hash ret;
	u16 index;

	if (!numqueues)
		return 0;

	virtio_net_hash_rss(skb, hash->common.hash_types, hash->rss_key, &ret);

	if (!ret.report)
		return hash->common.unclassified_queue % numqueues;

	if (hash->report) {
		report = vnet_hash_add(skb);
		if (report)
			*report = ret;
	}

	index = ret.value & hash->common.indirection_table_mask;

	return hash->rss_indirection_table[index] % numqueues;
}

static inline int tun_vnet_hdr_get(int sz, unsigned int flags,
				   struct iov_iter *from,
				   struct virtio_net_hdr *hdr)
{
	u16 hdr_len;

	if (iov_iter_count(from) < sz)
		return -EINVAL;

	if (!copy_from_iter_full(hdr, sizeof(*hdr), from))
		return -EFAULT;

	hdr_len = tun_vnet16_to_cpu(flags, hdr->hdr_len);

	if (hdr->flags & VIRTIO_NET_HDR_F_NEEDS_CSUM) {
		hdr_len = max(tun_vnet16_to_cpu(flags, hdr->csum_start) + tun_vnet16_to_cpu(flags, hdr->csum_offset) + 2, hdr_len);
		hdr->hdr_len = cpu_to_tun_vnet16(flags, hdr_len);
	}

	if (hdr_len > iov_iter_count(from))
		return -EINVAL;

	iov_iter_advance(from, sz - sizeof(*hdr));

	return hdr_len;
}

static inline int tun_vnet_hdr_put(int sz, struct iov_iter *iter,
				   const struct virtio_net_hdr_v1_hash *hdr)
{
	int content_sz = MIN(sizeof(*hdr), sz);

	if (unlikely(iov_iter_count(iter) < sz))
		return -EINVAL;

	if (unlikely(copy_to_iter(hdr, content_sz, iter) != content_sz))
		return -EFAULT;

	if (iov_iter_zero(sz - content_sz, iter) != sz - content_sz)
		return -EFAULT;

	return 0;
}

static inline int tun_vnet_hdr_to_skb(unsigned int flags, struct sk_buff *skb,
				      const struct virtio_net_hdr *hdr)
{
	return virtio_net_hdr_to_skb(skb, hdr, tun_vnet_is_little_endian(flags));
}

static inline int tun_vnet_hdr_from_skb(int sz, unsigned int flags,
					const struct net_device *dev,
					const struct sk_buff *skb,
					tun_vnet_hash_find vnet_hash_find,
					struct virtio_net_hdr_v1_hash *hdr)
{
	int vlan_hlen = skb_vlan_tag_present(skb) ? VLAN_HLEN : 0;
	const struct virtio_net_hash *report = sz < sizeof(struct virtio_net_hdr_v1_hash) ?
					       NULL : vnet_hash_find(skb);

	*hdr = (struct virtio_net_hdr_v1_hash) {
		.hash_report = VIRTIO_NET_HASH_REPORT_NONE
	};

	if (report) {
		hdr->hash_value = cpu_to_le32(report->value);
		hdr->hash_report = cpu_to_le16(report->report);
	}

	if (virtio_net_hdr_from_skb(skb, (struct virtio_net_hdr *)hdr,
				    tun_vnet_is_little_endian(flags), true,
				    vlan_hlen)) {
		struct skb_shared_info *sinfo = skb_shinfo(skb);

		if (net_ratelimit()) {
			netdev_err(dev, "unexpected GSO type: 0x%x, gso_size %d, hdr_len %d\n",
				   sinfo->gso_type, tun_vnet16_to_cpu(flags, hdr->hdr.gso_size),
				   tun_vnet16_to_cpu(flags, hdr->hdr.hdr_len));
			print_hex_dump(KERN_ERR, "tun: ",
				       DUMP_PREFIX_NONE,
				       16, 1, skb->head,
				       min(tun_vnet16_to_cpu(flags, hdr->hdr.hdr_len), 64), true);
		}
		WARN_ON_ONCE(1);
		return -EINVAL;
	}

	return 0;
}

#endif /* TUN_VNET_H */
