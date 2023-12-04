// SPDX-License-Identifier: GPL-2.0
/* IPVS:	Stateless Maglev Hashing scheduling module
 *
 * Authors:	Lev Pantiukhin <kndrvt@yandex-team.ru>
 *
 */

/* The mh algorithm is to assign a preference list of all the lookup
 * table positions to each destination and populate the table with
 * the most-preferred position of destinations. Then it is to select
 * destination with the hash key of source IP address through looking
 * up a the lookup table.
 * The mhs algorithm is modificated stateless version of mh algorithm.
 * It uses 2 look up tables and chooses one of 2 destinations.
 *
 * The mh algorithm is detailed in:
 * [3.4 Consistent Hasing]
https://www.usenix.org/system/files/conference/nsdi16/nsdi16-paper-eisenbud.pdf
 *
 */

#define KMSG_COMPONENT "IPVS"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <linux/ip.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/skbuff.h>

#include <net/ip_vs.h>

#include <linux/siphash.h>
#include <linux/bitops.h>
#include <linux/gcd.h>

#include <linux/list_sort.h>

#define IP_VS_SVC_F_SCHED_MH_FALLBACK       IP_VS_SVC_F_SCHED1 /* MH fallback */
#define IP_VS_SVC_F_SCHED_MH_PORT           IP_VS_SVC_F_SCHED2 /* MH use port */

struct ip_vs_mhs_lookup {
	struct ip_vs_dest __rcu    *dest;    /* real server (cache) */
};

struct ip_vs_mhs_dest_setup {
	unsigned int offset; /* starting offset */
	unsigned int skip;    /* skip */
	unsigned int perm;    /* next_offset */
	int turns;    /* weight / gcd() and rshift */
};

/* Available prime numbers for MH table */
static int primes[] = {251, 509, 1021, 2039, 4093,
					   8191, 16381, 32749, 65521, 131071};

/* For IPVS MH entry hash table */
#ifndef CONFIG_IP_VS_MH_TAB_INDEX
#define CONFIG_IP_VS_MH_TAB_INDEX	12
#endif
#define IP_VS_MH_TAB_BITS		(CONFIG_IP_VS_MH_TAB_INDEX / 2)
#define IP_VS_MH_TAB_INDEX		(CONFIG_IP_VS_MH_TAB_INDEX - 8)
#define IP_VS_MH_TAB_SIZE               primes[IP_VS_MH_TAB_INDEX]

struct ip_vs_mhs_state {
	struct rcu_head rcu_head;
	struct ip_vs_mhs_lookup *lookup;
	struct ip_vs_mhs_dest_setup *dest_setup;
	hsiphash_key_t hash1, hash2;
	int gcd;
	int rshift;
};

struct ip_vs_mhs_two_states {
	struct ip_vs_mhs_state *first;
	struct ip_vs_mhs_state *second;
	ktime_t *timestamps;
	ktime_t unstable_timeout;
};

struct ip_vs_mhs_two_dests {
	struct ip_vs_dest *dest;
	struct ip_vs_dest *new_dest;
	bool unstable;
};

static inline bool
ip_vs_mhs_is_new_conn(const struct sk_buff *skb, struct ip_vs_iphdr *iph)
{
	switch (iph->protocol) {
	case IPPROTO_TCP: {
		struct tcphdr _tcph, *th;

		th = skb_header_pointer(skb, iph->len, sizeof(_tcph), &_tcph);
		if (!th)
			return false;
		return th->syn;
	}
	default:
		return false;
	}
}

static inline void
generate_hash_secret(hsiphash_key_t *hash1, hsiphash_key_t *hash2)
{
	hash1->key[0] = 2654435761UL;
	hash1->key[1] = 2654435761UL;

	hash2->key[0] = 2654446892UL;
	hash2->key[1] = 2654446892UL;
}

/* Returns hash value for IPVS MH entry */
static inline unsigned int
ip_vs_mhs_hashkey(int af, const union nf_inet_addr *addr, __be16 port,
		  hsiphash_key_t *key, unsigned int offset)
{
	unsigned int v;
	__be32 addr_fold = addr->ip;

#ifdef CONFIG_IP_VS_IPV6
	if (af == AF_INET6)
		addr_fold = addr->ip6[0] ^ addr->ip6[1] ^
				addr->ip6[2] ^ addr->ip6[3];
#endif
	v = (offset + ntohs(port) + ntohl(addr_fold));
	return hsiphash(&v, sizeof(v), key);
}

/* Reset all the hash buckets of the specified table. */
static void ip_vs_mhs_reset(struct ip_vs_mhs_state *s)
{
	int i;
	struct ip_vs_mhs_lookup *l;
	struct ip_vs_dest *dest;

	l = &s->lookup[0];
	for (i = 0; i < IP_VS_MH_TAB_SIZE; i++) {
		dest = rcu_dereference_protected(l->dest, 1);
		if (dest) {
			ip_vs_dest_put(dest);
			RCU_INIT_POINTER(l->dest, NULL);
		}
		l++;
	}
}

/* Update timestamps with new lookup table */
static void
ip_vs_mhs_update_timestamps(struct ip_vs_mhs_two_states *states)
{
	unsigned int offset = 0;

	while (offset < IP_VS_MH_TAB_SIZE) {
		if (states->first->lookup[offset].dest ==
			states->second->lookup[offset].dest) {
			if (states->timestamps[offset]) {
				/* stabilization */
				states->timestamps[offset] = (ktime_t)0;
			}
		} else {
			if (!states->timestamps[offset]) {
				/* destabilization */
				states->timestamps[offset] = ktime_get();
			}
		}
		++offset;
	}
}

static int
ip_vs_mhs_permutate(struct ip_vs_mhs_state *s, struct ip_vs_service *svc)
{
	struct list_head *p;
	struct ip_vs_mhs_dest_setup *ds;
	struct ip_vs_dest *dest;
	int lw;

	/* If gcd is smaller then 1, number of dests or
	 * all weight of dests are zero. So, skip
	 * permutation for the dests.
	 */
	if (s->gcd < 1)
		return 0;

	/* Set dest_setup for the dests permutation */
	p = &svc->destinations;
	ds = &s->dest_setup[0];
	while ((p = p->next) != &svc->destinations) {
		dest = list_entry(p, struct ip_vs_dest, n_list);

		ds->offset = ip_vs_mhs_hashkey(svc->af, &dest->addr, dest->port,
					       &s->hash1, 0) %
			     IP_VS_MH_TAB_SIZE;
		ds->skip = ip_vs_mhs_hashkey(svc->af, &dest->addr, dest->port,
					     &s->hash2, 0) %
			   (IP_VS_MH_TAB_SIZE - 1) + 1;
		ds->perm = ds->offset;

		lw = atomic_read(&dest->weight);
		ds->turns = ((lw / s->gcd) >> s->rshift) ?: (lw != 0);
		ds++;
	}
	return 0;
}

static int
ip_vs_mhs_populate(struct ip_vs_mhs_state *s, struct ip_vs_service *svc)
{
	int n, c, dt_count;
	unsigned long *table;
	struct list_head *p;
	struct ip_vs_mhs_dest_setup *ds;
	struct ip_vs_dest *dest, *new_dest;

	/* If gcd is smaller then 1, number of dests or
	 * all last_weight of dests are zero. So, skip
	 * the population for the dests and reset lookup table.
	 */
	if (s->gcd < 1) {
		ip_vs_mhs_reset(s);
		return 0;
	}

	table = kcalloc(BITS_TO_LONGS(IP_VS_MH_TAB_SIZE), sizeof(unsigned long),
			GFP_KERNEL);
	if (!table)
		return -ENOMEM;

	p = &svc->destinations;
	n = 0;
	dt_count = 0;
	while (n < IP_VS_MH_TAB_SIZE) {
		if (p == &svc->destinations)
			p = p->next;

		ds = &s->dest_setup[0];
		while (p != &svc->destinations) {
			/* Ignore added server with zero weight */
			if (ds->turns < 1) {
				p = p->next;
				ds++;
				continue;
			}

			c = ds->perm;
			while (test_bit(c, table)) {
				/* Add skip, mod s->tab_size */
				ds->perm += ds->skip;
				if (ds->perm >= IP_VS_MH_TAB_SIZE)
					ds->perm -= IP_VS_MH_TAB_SIZE;
				c = ds->perm;
			}

			__set_bit(c, table);

			dest = rcu_dereference_protected(s->lookup[c].dest, 1);
			new_dest = list_entry(p, struct ip_vs_dest, n_list);
			if (dest != new_dest) {
				if (dest)
					ip_vs_dest_put(dest);
				ip_vs_dest_hold(new_dest);
				RCU_INIT_POINTER(s->lookup[c].dest, new_dest);
			}

			if (++n == IP_VS_MH_TAB_SIZE)
				goto out;

			if (++dt_count >= ds->turns) {
				dt_count = 0;
				p = p->next;
				ds++;
			}
		}
	}

out:
	kfree(table);
	return 0;
}

/* Assign all the hash buckets of the specified table with the service. */
static int
ip_vs_mhs_reassign(struct ip_vs_mhs_state *s, struct ip_vs_service *svc)
{
	int ret;

	if (svc->num_dests > IP_VS_MH_TAB_SIZE)
		return -EINVAL;

	if (svc->num_dests >= 1) {
		s->dest_setup = kcalloc(svc->num_dests,
					sizeof(struct ip_vs_mhs_dest_setup),
					GFP_KERNEL);
		if (!s->dest_setup)
			return -ENOMEM;
	}

	ip_vs_mhs_permutate(s, svc);

	ret = ip_vs_mhs_populate(s, svc);
	if (ret < 0)
		goto out;

	IP_VS_DBG_BUF(6, "MHS: %s(): reassign lookup table of %s:%u\n",
		      __func__,
		      IP_VS_DBG_ADDR(svc->af, &svc->addr),
		      ntohs(svc->port));

out:
	if (svc->num_dests >= 1) {
		kfree(s->dest_setup);
		s->dest_setup = NULL;
	}
	return ret;
}

static int
ip_vs_mhs_gcd_weight(struct ip_vs_service *svc)
{
	struct ip_vs_dest *dest;
	int weight;
	int g = 0;

	list_for_each_entry(dest, &svc->destinations, n_list) {
		weight = atomic_read(&dest->weight);
		if (weight > 0) {
			if (g > 0)
				g = gcd(weight, g);
			else
				g = weight;
		}
	}
	return g;
}

/* To avoid assigning huge weight for the MH table,
 * calculate shift value with gcd.
 */
static int
ip_vs_mhs_shift_weight(struct ip_vs_service *svc, int gcd)
{
	struct ip_vs_dest *dest;
	int new_weight, weight = 0;
	int mw, shift;

	/* If gcd is smaller then 1, number of dests or
	 * all weight of dests are zero. So, return
	 * shift value as zero.
	 */
	if (gcd < 1)
		return 0;

	list_for_each_entry(dest, &svc->destinations, n_list) {
		new_weight = atomic_read(&dest->weight);
		if (new_weight > weight)
			weight = new_weight;
	}

	/* Because gcd is greater than zero,
	 * the maximum weight and gcd are always greater than zero
	 */
	mw = weight / gcd;

	/* shift = occupied bits of weight/gcd - MH highest bits */
	shift = fls(mw) - IP_VS_MH_TAB_BITS;
	return (shift >= 0) ? shift : 0;
}

static ktime_t
ip_vs_mhs_get_unstable_timeout(struct ip_vs_service *svc)
{
	struct ip_vs_proto_data *pd;
	u64 tcp_to, tcp_fin_to;

	pd = ip_vs_proto_data_get(svc->ipvs, IPPROTO_TCP);
	tcp_to = pd->timeout_table[IP_VS_TCP_S_ESTABLISHED];
	tcp_fin_to = pd->timeout_table[IP_VS_TCP_S_FIN_WAIT];
	return ns_to_ktime(jiffies64_to_nsecs(max(tcp_to, tcp_fin_to)));
}

static void
ip_vs_mhs_state_free(struct rcu_head *head)
{
	struct ip_vs_mhs_state *s;

	s = container_of(head, struct ip_vs_mhs_state, rcu_head);
	kfree(s->lookup);
	kfree(s);
}

static int
ip_vs_mhs_init_svc(struct ip_vs_service *svc)
{
	struct ip_vs_mhs_state *s0, *s1;
	struct ip_vs_mhs_two_states *states;
	ktime_t *tss;
	int ret;

	/* Allocate timestamps */
	tss = kcalloc(IP_VS_MH_TAB_SIZE, sizeof(ktime_t), GFP_KERNEL);
	if (!tss)
		return -ENOMEM;

	/* Allocate the first MH table for this service */
	s0 = kzalloc(sizeof(*s0), GFP_KERNEL);
	if (!s0) {
		kfree(tss);
		return -ENOMEM;
	}

	s0->lookup = kcalloc(IP_VS_MH_TAB_SIZE, sizeof(struct ip_vs_mhs_lookup),
			     GFP_KERNEL);
	if (!s0->lookup) {
		kfree(tss);
		kfree(s0);
		return -ENOMEM;
	}

	generate_hash_secret(&s0->hash1, &s0->hash2);
	s0->gcd = ip_vs_mhs_gcd_weight(svc);
	s0->rshift = ip_vs_mhs_shift_weight(svc, s0->gcd);

	IP_VS_DBG(6,
		  "MHS: %s(): The first lookup table (memory=%zdbytes) allocated\n",
		  __func__,
		  sizeof(struct ip_vs_mhs_lookup) * IP_VS_MH_TAB_SIZE);

	/* Assign the first lookup table with current dests */
	ret = ip_vs_mhs_reassign(s0, svc);
	if (ret < 0) {
		kfree(tss);
		ip_vs_mhs_reset(s0);
		ip_vs_mhs_state_free(&s0->rcu_head);
		return ret;
	}

	/* Allocate the second MH table for this service */
	s1 = kzalloc(sizeof(*s1), GFP_KERNEL);
	if (!s1) {
		kfree(tss);
		ip_vs_mhs_reset(s0);
		ip_vs_mhs_state_free(&s0->rcu_head);
		return -ENOMEM;
	}
	s1->lookup = kcalloc(IP_VS_MH_TAB_SIZE, sizeof(struct ip_vs_mhs_lookup),
			     GFP_KERNEL);
	if (!s1->lookup) {
		kfree(tss);
		ip_vs_mhs_reset(s0);
		ip_vs_mhs_state_free(&s0->rcu_head);
		kfree(s1);
		return -ENOMEM;
	}

	s1->hash1 = s0->hash1;
	s1->hash2 = s0->hash2;
	s1->gcd = s0->gcd;
	s1->rshift = s0->rshift;

	IP_VS_DBG(6,
		  "MHS: %s(): The second lookup table (memory=%zdbytes) allocated\n",
		  __func__,
		  sizeof(struct ip_vs_mhs_lookup) * IP_VS_MH_TAB_SIZE);

	/* Assign the second lookup table with current dests */
	ret = ip_vs_mhs_reassign(s1, svc);
	if (ret < 0) {
		kfree(tss);
		ip_vs_mhs_reset(s0);
		ip_vs_mhs_state_free(&s0->rcu_head);
		ip_vs_mhs_reset(s1);
		ip_vs_mhs_state_free(&s1->rcu_head);
		return ret;
	}

	/* Allocate, initialize and attach states */
	states = kcalloc(1, sizeof(struct ip_vs_mhs_two_states), GFP_KERNEL);
	if (!states) {
		kfree(tss);
		ip_vs_mhs_reset(s0);
		ip_vs_mhs_state_free(&s0->rcu_head);
		ip_vs_mhs_reset(s1);
		ip_vs_mhs_state_free(&s1->rcu_head);
		return -ENOMEM;
	}

	states->first = s0;
	states->second = s1;
	states->timestamps = tss;
	states->unstable_timeout = ip_vs_mhs_get_unstable_timeout(svc);
	svc->sched_data = states;
	return 0;
}

static void
ip_vs_mhs_done_svc(struct ip_vs_service *svc)
{
	struct ip_vs_mhs_two_states *states = svc->sched_data;

	kfree(states->timestamps);

	/* Got to clean up the first lookup entry here */
	ip_vs_mhs_reset(states->first);

	call_rcu(&states->first->rcu_head, ip_vs_mhs_state_free);
	IP_VS_DBG(6,
		  "MHS: The first MH lookup table (memory=%zdbytes) released\n",
		  sizeof(struct ip_vs_mhs_lookup) * IP_VS_MH_TAB_SIZE);

	/* Got to clean up the second lookup entry here */
	ip_vs_mhs_reset(states->second);

	call_rcu(&states->second->rcu_head, ip_vs_mhs_state_free);
	IP_VS_DBG(6,
		  "MHS: The second MH lookup table (memory=%zdbytes) released\n",
		  sizeof(struct ip_vs_mhs_lookup) * IP_VS_MH_TAB_SIZE);

	kfree(states);
}

static int
ip_vs_mhs_dest_changed(struct ip_vs_service *svc,
		       struct ip_vs_dest *dest)
{
	struct ip_vs_mhs_two_states *states = svc->sched_data;
	struct ip_vs_mhs_state *s1 = states->second;
	int ret;

	s1->gcd = ip_vs_mhs_gcd_weight(svc);
	s1->rshift = ip_vs_mhs_shift_weight(svc, s1->gcd);

	/* Assign the lookup table with the updated service */
	ret = ip_vs_mhs_reassign(s1, svc);

	ip_vs_mhs_update_timestamps(states);
	states->unstable_timeout = ip_vs_mhs_get_unstable_timeout(svc);
	IP_VS_DBG(6,
		  "MHS: %s: set unstable timeout: %llu",
		  __func__,
		  ktime_divns(states->unstable_timeout,
			      NSEC_PER_SEC));
	return ret;
}

/* Helper function to get port number */
static inline __be16
ip_vs_mhs_get_port(const struct sk_buff *skb, struct ip_vs_iphdr *iph)
{
	__be16 _ports[2], *ports;

	/* At this point we know that we have a valid packet of some kind.
	 * Because ICMP packets are only guaranteed to have the first 8
	 * bytes, let's just grab the ports.  Fortunately they're in the
	 * same position for all three of the protocols we care about.
	 */
	switch (iph->protocol) {
	case IPPROTO_TCP:
	case IPPROTO_UDP:
	case IPPROTO_SCTP:
		ports = skb_header_pointer(skb, iph->len, sizeof(_ports),
					   &_ports);
		if (unlikely(!ports))
			return 0;

		if (likely(!ip_vs_iph_inverse(iph)))
			return ports[0];
		else
			return ports[1];
	default:
		return 0;
	}
}

/* Get ip_vs_dest associated with supplied parameters. */
static inline void
ip_vs_mhs_get(struct ip_vs_service *svc,
	      struct ip_vs_mhs_two_states *states,
	      struct ip_vs_mhs_two_dests *dests,
	      const union nf_inet_addr *addr,
	      __be16 port)
{
	unsigned int hash;
	ktime_t timestamp;

	hash = ip_vs_mhs_hashkey(svc->af, addr, port, &states->first->hash1,
				 0) % IP_VS_MH_TAB_SIZE;
	dests->dest = rcu_dereference(states->first->lookup[hash].dest);
	dests->new_dest = rcu_dereference(states->second->lookup[hash].dest);
	timestamp = states->timestamps[hash];

	/* only unstable hashes have non-zero value */
	if (timestamp > 0) {
		/* unstable */
		if (timestamp + states->unstable_timeout > ktime_get()) {
			/* timer didn't expire */
			dests->unstable = true;
			return;
		}
		/* unstable -> stable */
		if (dests->dest)
			ip_vs_dest_put(dests->dest);
		if (dests->new_dest)
			ip_vs_dest_hold(dests->new_dest);
		dests->dest = dests->new_dest;
		RCU_INIT_POINTER(states->first->lookup[hash].dest,
				 dests->new_dest);
		states->timestamps[hash] = (ktime_t)0;
	}
	/* stable */
	dests->unstable = false;
}

/* Stateless Maglev Hashing scheduling */
static struct ip_vs_dest *
ip_vs_mhs_schedule(struct ip_vs_service *svc,
		   const struct sk_buff *skb,
		   struct ip_vs_iphdr *iph,
		   bool *need_state)
{
	struct ip_vs_mhs_two_dests dests;
	struct ip_vs_dest *final_dest = NULL;
	struct ip_vs_mhs_two_states *states = svc->sched_data;
	__be16 port = 0;
	const union nf_inet_addr *hash_addr;

	*need_state = false;
	hash_addr = ip_vs_iph_inverse(iph) ? &iph->daddr : &iph->saddr;

	if (svc->flags & IP_VS_SVC_F_SCHED_MH_PORT)
		port = ip_vs_mhs_get_port(skb, iph);

	ip_vs_mhs_get(svc, states, &dests, hash_addr, port);
	IP_VS_DBG_BUF(6,
		      "MHS: %s(): source IP address %s:%u --> server %s and %s\n",
		      __func__,
		      IP_VS_DBG_ADDR(svc->af, hash_addr),
		      ntohs(port),
		      dests.dest
		      ? IP_VS_DBG_ADDR(dests.dest->af, &dests.dest->addr)
		      : "NULL",
		      dests.new_dest
		      ? IP_VS_DBG_ADDR(dests.new_dest->af,
				       &dests.new_dest->addr)
		      : "NULL");

	if (!dests.dest && !dests.new_dest) {
		/* Both dests is NULL */
		return NULL;
	}

	if (!(dests.dest && dests.new_dest)) {
		/* dest is NULL or new_dest is NULL,
		 * so we send all packets to singular available dest
		 * and create state
		 */
		if (dests.new_dest) {
			/* dest is NULL */
			final_dest = dests.new_dest;
		} else {
			/* new_dest is NULL */
			final_dest = dests.dest;
		}
		*need_state = true;
		IP_VS_DBG(6,
			  "MHS: %s(): One dest, need_state=%s\n",
			  __func__,
			  *need_state ? "true" : "false");
	} else if (dests.unstable) {
		/* unstable */
		if (iph->protocol == IPPROTO_TCP) {
			/* TCP */
			*need_state = true;
			if (ip_vs_mhs_is_new_conn(skb, iph)) {
				/* SYN packet */
				final_dest = dests.new_dest;
				IP_VS_DBG(6,
					  "MHS: %s(): Unstable, need_state=%s, SYN packet\n",
					  __func__,
					  *need_state ? "true" : "false");
			} else {
				/* Not SYN packet */
				final_dest = dests.dest;
				IP_VS_DBG(6,
					  "MHS: %s(): Unstable, need_state=%s, not SYN packet\n",
					  __func__,
					  *need_state ? "true" : "false");
			}
		} else if (iph->protocol == IPPROTO_UDP) {
			/* UDP */
			final_dest = dests.new_dest;
			IP_VS_DBG(6,
				  "MHS: %s(): Unstable, need_state=%s, UDP packet\n",
				  __func__,
				  *need_state ? "true" : "false");
		}
	} else {
		/* stable */
		final_dest = dests.dest;
		IP_VS_DBG(6,
			  "MHS: %s(): Stable, need_state=%s\n",
			  __func__,
			  *need_state ? "true" : "false");
	}
	return final_dest;
}

/* IPVS MHS Scheduler structure */
static struct ip_vs_scheduler ip_vs_mhs_scheduler = {
	.name =                "mhs",
	.refcnt =        ATOMIC_INIT(0),
	.module =        THIS_MODULE,
	.n_list =        LIST_HEAD_INIT(ip_vs_mhs_scheduler.n_list),
	.init_service =        ip_vs_mhs_init_svc,
	.done_service =        ip_vs_mhs_done_svc,
	.add_dest =        ip_vs_mhs_dest_changed,
	.del_dest =        ip_vs_mhs_dest_changed,
	.upd_dest =        ip_vs_mhs_dest_changed,
	.schedule_sl =        ip_vs_mhs_schedule,
};

static int __init
ip_vs_mhs_init(void)
{
	return register_ip_vs_scheduler(&ip_vs_mhs_scheduler);
}

static void __exit
ip_vs_mhs_cleanup(void)
{
	unregister_ip_vs_scheduler(&ip_vs_mhs_scheduler);
	rcu_barrier();
}

module_init(ip_vs_mhs_init);
module_exit(ip_vs_mhs_cleanup);
MODULE_DESCRIPTION("Stateless Maglev hashing ipvs scheduler");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lev Pantiukhin <kndrvt@yandex-team.ru>");
