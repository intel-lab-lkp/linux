/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NET_DST_CACHE_H
#define _NET_DST_CACHE_H

#include <linux/jiffies.h>
#include <net/dst.h>
#if IS_ENABLED(CONFIG_IPV6)
#include <net/ip6_fib.h>
#endif
#include <net/ip.h>

#define DST_CACHE_INPUT_SHIFT (9)
#define DST_CACHE_INPUT_SIZE (1 << DST_CACHE_INPUT_SHIFT)
#define DST_CACHE_INPUT_BUCKET_SIZE (4)
#define DST_CACHE_INPUT_HASH_MASK (~(DST_CACHE_INPUT_BUCKET_SIZE - 1))
#define INVALID_DST_CACHE_INPUT_KEY (~(u64)(0))

struct dst_cache {
	struct dst_cache_pcpu __percpu *cache;
	unsigned long reset_ts;
};

extern unsigned int dst_cache_net_id __read_mostly;

/**
 * idst_for_each_in_bucket - iterate over a dst cache bucket
 * @pos:	the type * to use as a loop cursor
 * @head:	the head of the cpu dst cache.
 * @hash:	the hash of the bucket
 */
#define idst_for_each_in_bucket(pos, head, hash)		\
	for (pos = &head[hash];					\
	     pos < &head[hash + DST_CACHE_INPUT_BUCKET_SIZE];	\
	     pos++)

/**
 * idst_for_each_in_cache - iterate over the dst cache
 * @pos:	the type * to use as a loop cursor
 * @head:	the head of the cpu dst cache.
 */
#define idst_for_each_in_cache(pos, head)				\
	for (pos = head; pos < head + DST_CACHE_INPUT_SIZE; pos++)

/**
 *	dst_cache_get - perform cache lookup
 *	@dst_cache: the cache
 *
 *	The caller should use dst_cache_get_ip4() if it need to retrieve the
 *	source address to be used when xmitting to the cached dst.
 *	local BH must be disabled.
 */
struct dst_entry *dst_cache_get(struct dst_cache *dst_cache);

/**
 *	dst_cache_get_ip4 - perform cache lookup and fetch ipv4 source address
 *	@dst_cache: the cache
 *	@saddr: return value for the retrieved source address
 *
 *	local BH must be disabled.
 */
struct rtable *dst_cache_get_ip4(struct dst_cache *dst_cache, __be32 *saddr);

/**
 *	dst_cache_set_ip4 - store the ipv4 dst into the cache
 *	@dst_cache: the cache
 *	@dst: the entry to be cached
 *	@saddr: the source address to be stored inside the cache
 *
 *	local BH must be disabled.
 */
void dst_cache_set_ip4(struct dst_cache *dst_cache, struct dst_entry *dst,
		       __be32 saddr);

#if IS_ENABLED(CONFIG_IPV6)

/**
 *	dst_cache_set_ip6 - store the ipv6 dst into the cache
 *	@dst_cache: the cache
 *	@dst: the entry to be cached
 *	@saddr: the source address to be stored inside the cache
 *
 *	local BH must be disabled.
 */
void dst_cache_set_ip6(struct dst_cache *dst_cache, struct dst_entry *dst,
		       const struct in6_addr *saddr);

/**
 *	dst_cache_get_ip6 - perform cache lookup and fetch ipv6 source address
 *	@dst_cache: the cache
 *	@saddr: return value for the retrieved source address
 *
 *	local BH must be disabled.
 */
struct dst_entry *dst_cache_get_ip6(struct dst_cache *dst_cache,
				    struct in6_addr *saddr);
#endif

/**
 *	dst_cache_reset - invalidate the cache contents
 *	@dst_cache: the cache
 *
 *	This does not free the cached dst to avoid races and contentions.
 *	the dst will be freed on later cache lookup.
 */
static inline void dst_cache_reset(struct dst_cache *dst_cache)
{
	WRITE_ONCE(dst_cache->reset_ts, jiffies);
}

/**
 *	dst_cache_reset_now - invalidate the cache contents immediately
 *	@dst_cache: the cache
 *
 *	The caller must be sure there are no concurrent users, as this frees
 *	all dst_cache users immediately, rather than waiting for the next
 *	per-cpu usage like dst_cache_reset does. Most callers should use the
 *	higher speed lazily-freed dst_cache_reset function instead.
 */
void dst_cache_reset_now(struct dst_cache *dst_cache);

/**
 *	dst_cache_init - initialize the cache, allocating the required storage
 *	@dst_cache: the cache
 *	@gfp: allocation flags
 */
int dst_cache_init(struct dst_cache *dst_cache, gfp_t gfp);

/**
 *	dst_cache_destroy - empty the cache and free the allocated storage
 *	@dst_cache: the cache
 *
 *	No synchronization is enforced: it must be called only when the cache
 *	is unsed.
 */
void dst_cache_destroy(struct dst_cache *dst_cache);

/**
 *	dst_cache_input_get_noref - perform lookup in the input cache,
 *	return a noref dst
 *	@dst_cache: the input cache
 *	@skb: the packet according to which the dst entry will be searched
 *	local BH must be disabled.
 */
struct dst_entry *dst_cache_input_get_noref(struct dst_cache *dst_cache,
					    struct sk_buff *skb);

/**
 *	dst_cache_input_add - add the dst of the given skb to the input cache.
 *
 *	in case the cache bucket is full, the oldest entry will be deleted
 *	and replaced with the new one.
 *	@dst_cache: the input cache
 *	@skb: The packet according to which the dst entry will be searched
 *
 *	local BH must be disabled.
 */
void dst_cache_input_add(struct dst_cache *dst_cache,
			 const struct sk_buff *skb);

/**
 *	dst_cache_input_init - initialize the input cache,
 *	allocating the required storage
 */
int __init dst_cache_input_init(void);

static inline u64 create_dst_cache_key_ip4(const struct sk_buff *skb)
{
	struct iphdr *iphdr = ip_hdr(skb);

	return (((u64)ntohl(iphdr->daddr)) << 8) | iphdr->tos;
}

static inline u32 hash_dst_cache_key(u64 key)
{
	return hash_64(key, DST_CACHE_INPUT_SHIFT) & DST_CACHE_INPUT_HASH_MASK;
}
#endif
