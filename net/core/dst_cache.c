// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * net/core/dst_cache.c - dst entry cache
 *
 * Copyright (c) 2016 Paolo Abeni <pabeni@redhat.com>
 */

#include <linux/kernel.h>
#include <linux/percpu.h>
#include <linux/xarray.h>
#include <linux/rcupdate_wait.h>
#include <net/dst_cache.h>
#include <net/route.h>
#if IS_ENABLED(CONFIG_IPV6)
#include <net/ip6_fib.h>
#endif
#include <uapi/linux/in.h>

static DEFINE_XARRAY_FLAGS(dst_caches, XA_FLAGS_ALLOC);

struct dst_cache_entry {
	struct dst_cache_pcpu __percpu *cache;
	struct rcu_head rcu;
};

struct dst_cache_pcpu {
	unsigned long refresh_ts;
	struct dst_entry *dst;
	u32 cookie;
	union {
		struct in_addr in_saddr;
		struct in6_addr in6_saddr;
	};
};

static void dst_cache_per_cpu_dst_set(struct dst_cache_pcpu *dst_cache,
				      struct dst_entry *dst, u32 cookie)
{
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
	struct dst_cache_entry *entry;
	int last_id, ret = -ENOMEM;

	dst_cache->cache = alloc_percpu_gfp(struct dst_cache_pcpu,
					    gfp | __GFP_ZERO);
	if (!dst_cache->cache)
		return -ENOMEM;

	entry = kmalloc(sizeof(*entry), gfp | __GFP_ZERO);
	if (!entry)
		goto free_cache;

	ret = xa_alloc_cyclic_bh(&dst_caches, &dst_cache->id, entry,
				 xa_limit_32b, &last_id, gfp);
	if (ret < 0)
		goto free_entry;

	entry->cache = dst_cache->cache;
	dst_cache_reset(dst_cache);
	return 0;

free_entry:
	kfree(entry);

free_cache:
	free_percpu(dst_cache->cache);
	dst_cache->cache = NULL;
	return ret;
}
EXPORT_SYMBOL_GPL(dst_cache_init);

static void dst_cache_entry_free(struct rcu_head *rcu)
{
	struct dst_cache_entry *entry = container_of(rcu, struct dst_cache_entry, rcu);

	free_percpu(entry->cache);
	kfree(entry);
}

void dst_cache_destroy(struct dst_cache *dst_cache)
{
	struct dst_cache_entry *entry;
	int i;

	if (!dst_cache->cache)
		return;

	entry = xa_erase_bh(&dst_caches, dst_cache->id);

	for_each_possible_cpu(i)
		dst_release(per_cpu_ptr(dst_cache->cache, i)->dst);

	if (!WARN_ON_ONCE(!entry))
		call_rcu(&entry->rcu, dst_cache_entry_free);
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

static void dst_cache_flush_dev(struct dst_cache_entry *entry,
				struct net_device *dev)
{
	int i;

	for_each_possible_cpu(i) {
		struct dst_cache_pcpu *idst = per_cpu_ptr(entry->cache, i);
		struct dst_entry *dst = READ_ONCE(idst->dst);

		if (!dst || !dst_hold_safe(dst))
			continue;

		if (!list_empty(&dst->rt_uncached) || dst->dev != dev)
			goto release;

		dst->dev = blackhole_netdev;
		netdev_ref_replace(dev, blackhole_netdev, &dst->dev_tracker,
				   GFP_ATOMIC);

release:
		dst_release(dst);
	}
}

static int dst_cache_netdev_event(struct notifier_block *this, unsigned long event, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	struct dst_cache_entry *entry;
	XA_STATE(xas, &dst_caches, 0);

	if (event == NETDEV_UNREGISTER) {
		rcu_read_lock();
		xas_for_each(&xas, entry, UINT_MAX) {
			dst_cache_flush_dev(entry, dev);
			if (need_resched()) {
				xas_pause(&xas);
				cond_resched_rcu();
			}
		}
		rcu_read_unlock();
	}

	return NOTIFY_DONE;
}

static struct notifier_block dst_cache_notifier = {
	.notifier_call = dst_cache_netdev_event,
};

static int __init dst_cache_notifier_init(void)
{
	return register_netdevice_notifier(&dst_cache_notifier);
}

subsys_initcall(dst_cache_notifier_init);
