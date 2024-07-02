// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * net/core/dst_cache.c - dst entry cache
 *
 * Copyright (c) 2016 Paolo Abeni <pabeni@redhat.com>
 */

#include <linux/kernel.h>
#include <linux/percpu.h>
#include <net/dst_cache.h>
#include <net/route.h>
#if IS_ENABLED(CONFIG_IPV6)
#include <net/ip6_fib.h>
#endif
#include <uapi/linux/in.h>
#include <net/netns/generic.h>

struct dst_cache_pcpu {
	unsigned long refresh_ts;
	struct dst_entry *dst;
	u32 cookie;
	union {
		struct in_addr in_saddr;
		struct in6_addr in6_saddr;
		u64 key;
	};
};

unsigned int dst_cache_net_id __read_mostly;

static void dst_cache_per_cpu_dst_set(struct dst_cache_pcpu *dst_cache,
				      struct dst_entry *dst, u32 cookie)
{
	DEBUG_NET_WARN_ON_ONCE(!in_softirq());
	dst_release(dst_cache->dst);
	if (dst)
		dst_hold(dst);

	dst_cache->cookie = cookie;
	dst_cache->dst = dst;
}

static struct dst_entry *dst_cache_per_cpu_get(struct dst_cache *dst_cache,
					       struct dst_cache_pcpu *idst)
{
	struct dst_entry *dst;

	DEBUG_NET_WARN_ON_ONCE(!in_softirq());
	dst = idst->dst;
	if (!dst)
		goto fail;

	/* the cache already hold a dst reference; it can't go away */
	dst_hold(dst);

	if (unlikely(!time_after(idst->refresh_ts,
				 READ_ONCE(dst_cache->reset_ts)) ||
		     (dst->obsolete && !dst->ops->check(dst, idst->cookie)))) {
		dst_cache_per_cpu_dst_set(idst, NULL, 0);
		dst_release(dst);
		goto fail;
	}
	return dst;

fail:
	idst->refresh_ts = jiffies;
	return NULL;
}

struct dst_entry *dst_cache_get(struct dst_cache *dst_cache)
{
	if (!dst_cache->cache)
		return NULL;

	return dst_cache_per_cpu_get(dst_cache, this_cpu_ptr(dst_cache->cache));
}
EXPORT_SYMBOL_GPL(dst_cache_get);

struct rtable *dst_cache_get_ip4(struct dst_cache *dst_cache, __be32 *saddr)
{
	struct dst_cache_pcpu *idst;
	struct dst_entry *dst;

	if (!dst_cache->cache)
		return NULL;

	idst = this_cpu_ptr(dst_cache->cache);
	dst = dst_cache_per_cpu_get(dst_cache, idst);
	if (!dst)
		return NULL;

	*saddr = idst->in_saddr.s_addr;
	return dst_rtable(dst);
}
EXPORT_SYMBOL_GPL(dst_cache_get_ip4);

void dst_cache_set_ip4(struct dst_cache *dst_cache, struct dst_entry *dst,
		       __be32 saddr)
{
	struct dst_cache_pcpu *idst;

	if (!dst_cache->cache)
		return;

	idst = this_cpu_ptr(dst_cache->cache);
	dst_cache_per_cpu_dst_set(idst, dst, 0);
	idst->in_saddr.s_addr = saddr;
}
EXPORT_SYMBOL_GPL(dst_cache_set_ip4);

#if IS_ENABLED(CONFIG_IPV6)
void dst_cache_set_ip6(struct dst_cache *dst_cache, struct dst_entry *dst,
		       const struct in6_addr *saddr)
{
	struct dst_cache_pcpu *idst;

	if (!dst_cache->cache)
		return;

	idst = this_cpu_ptr(dst_cache->cache);
	dst_cache_per_cpu_dst_set(idst, dst,
				  rt6_get_cookie(dst_rt6_info(dst)));
	idst->in6_saddr = *saddr;
}
EXPORT_SYMBOL_GPL(dst_cache_set_ip6);

struct dst_entry *dst_cache_get_ip6(struct dst_cache *dst_cache,
				    struct in6_addr *saddr)
{
	struct dst_cache_pcpu *idst;
	struct dst_entry *dst;

	if (!dst_cache->cache)
		return NULL;

	idst = this_cpu_ptr(dst_cache->cache);
	dst = dst_cache_per_cpu_get(dst_cache, idst);
	if (!dst)
		return NULL;

	*saddr = idst->in6_saddr;
	return dst;
}
EXPORT_SYMBOL_GPL(dst_cache_get_ip6);
#endif

int dst_cache_init(struct dst_cache *dst_cache, gfp_t gfp)
{
	dst_cache->cache = alloc_percpu_gfp(struct dst_cache_pcpu,
					    gfp | __GFP_ZERO);
	if (!dst_cache->cache)
		return -ENOMEM;

	dst_cache_reset(dst_cache);
	return 0;
}
EXPORT_SYMBOL_GPL(dst_cache_init);

void dst_cache_destroy(struct dst_cache *dst_cache)
{
	int i;

	if (!dst_cache->cache)
		return;

	for_each_possible_cpu(i)
		dst_release(per_cpu_ptr(dst_cache->cache, i)->dst);

	free_percpu(dst_cache->cache);
}
EXPORT_SYMBOL_GPL(dst_cache_destroy);

void dst_cache_reset_now(struct dst_cache *dst_cache)
{
	int i;

	if (!dst_cache->cache)
		return;

	dst_cache_reset(dst_cache);
	for_each_possible_cpu(i) {
		struct dst_cache_pcpu *idst = per_cpu_ptr(dst_cache->cache, i);
		struct dst_entry *dst = idst->dst;

		idst->cookie = 0;
		idst->dst = NULL;
		dst_release(dst);
	}
}
EXPORT_SYMBOL_GPL(dst_cache_reset_now);

static void dst_cache_input_set(struct dst_cache_pcpu *idst,
				struct dst_entry *dst, u64 key)
{
	dst_cache_per_cpu_dst_set(idst, dst, 0);
	idst->key = key;
	idst->refresh_ts = jiffies;
}

static struct dst_entry *__dst_cache_input_get_noref(struct dst_cache_pcpu *idst)
{
	struct dst_entry *dst = idst->dst;

	if (unlikely(dst->obsolete && !dst->ops->check(dst, idst->cookie))) {
		dst_cache_input_set(idst, NULL, INVALID_DST_CACHE_INPUT_KEY);
		goto fail;
	}

	idst->refresh_ts = jiffies;
	return dst;

fail:
	return NULL;
}

struct dst_entry *dst_cache_input_get_noref(struct dst_cache *dst_cache,
					    struct sk_buff *skb)
{
	struct dst_entry *out_dst = NULL;
	struct dst_cache_pcpu *pcpu_cache;
	struct dst_cache_pcpu *idst;
	u32 hash;
	u64 key;

	pcpu_cache = this_cpu_ptr(dst_cache->cache);
	key = create_dst_cache_key_ip4(skb);
	hash = hash_dst_cache_key(key);
	idst_for_each_in_bucket(idst, pcpu_cache, hash) {
		if (key == idst->key) {
			out_dst = __dst_cache_input_get_noref(idst);
			goto out;
		}
	}
out:
	return out_dst;
}

static void dst_cache_input_reset_now(struct dst_cache *dst_cache)
{
	struct dst_cache_pcpu *caches;
	struct dst_cache_pcpu *idst;
	struct dst_entry *dst;
	int i;

	for_each_possible_cpu(i) {
		caches = per_cpu_ptr(dst_cache->cache, i);
		idst_for_each_in_cache(idst, caches) {
			idst->key = INVALID_DST_CACHE_INPUT_KEY;
			dst = idst->dst;
			if (dst)
				dst_release(dst);
		}
	}
}

static int __net_init dst_cache_input_net_init(struct net *net)
{
	struct dst_cache *dst_cache = net_generic(net, dst_cache_net_id);

	dst_cache->cache = (struct dst_cache_pcpu __percpu *)alloc_percpu_gfp(struct dst_cache_pcpu[DST_CACHE_INPUT_SIZE],
									      GFP_KERNEL | __GFP_ZERO);
	if (!dst_cache->cache)
		return -ENOMEM;

	dst_cache_input_reset_now(dst_cache);
	return 0;
}

static void __net_exit dst_cache_input_net_exit(struct net *net)
{
	struct dst_cache *dst_cache = net_generic(net, dst_cache_net_id);

	dst_cache_input_reset_now(dst_cache);
	free_percpu(dst_cache->cache);
	dst_cache->cache = NULL;
}

static bool idst_empty(struct dst_cache_pcpu *idst)
{
	return idst->key == INVALID_DST_CACHE_INPUT_KEY;
}

void dst_cache_input_add(struct dst_cache *dst_cache, const struct sk_buff *skb)
{
	struct dst_cache_pcpu *entry = NULL;
	struct dst_cache_pcpu *pcpu_cache;
	struct dst_cache_pcpu *idst;
	u32 hash;
	u64 key;

	pcpu_cache = this_cpu_ptr(dst_cache->cache);
	key = create_dst_cache_key_ip4(skb);
	hash = hash_dst_cache_key(key);
	idst_for_each_in_bucket(idst, pcpu_cache, hash) {
		if (idst_empty(idst)) {
			entry = idst;
			goto add_to_cache;
		}
		if (!entry || time_before(idst->refresh_ts, entry->refresh_ts))
			entry = idst;
	}

add_to_cache:
	dst_cache_input_set(entry, skb_dst(skb), key);
}

static struct pernet_operations dst_cache_input_ops __net_initdata = {
	.init = dst_cache_input_net_init,
	.exit = dst_cache_input_net_exit,
	.id   = &dst_cache_net_id,
	.size = sizeof(struct dst_cache),
};

int __init dst_cache_input_init(void)
{
	return register_pernet_subsys(&dst_cache_input_ops);
}
subsys_initcall(dst_cache_input_init);
