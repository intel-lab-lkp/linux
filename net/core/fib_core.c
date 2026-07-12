// SPDX-License-Identifier: GPL-2.0-only
#include <linux/types.h>
#include <net/lwtunnel.h>
#include <net/route.h>

static void rt_fibinfo_free(struct rtable __rcu **rtp)
{
	struct rtable *rt = rcu_dereference_protected(*rtp, 1);

	if (!rt)
		return;

	/* Not even needed : RCU_INIT_POINTER(*rtp, NULL);
	 * because we waited an RCU grace period before calling
	 * free_fib_info_rcu()
	 */

	dst_dev_put(&rt->dst);
	dst_release_immediate(&rt->dst);
}

static void free_nh_exceptions(struct fib_nh_common *nhc)
{
	struct fnhe_hash_bucket *hash;
	int i;

	hash = rcu_dereference_protected(nhc->nhc_exceptions, 1);
	if (!hash)
		return;
	for (i = 0; i < FNHE_HASH_SIZE; i++) {
		struct fib_nh_exception *fnhe;

		fnhe = rcu_dereference_protected(hash[i].chain, 1);
		while (fnhe) {
			struct fib_nh_exception *next;

			next = rcu_dereference_protected(fnhe->fnhe_next, 1);

			rt_fibinfo_free(&fnhe->fnhe_rth_input);
			rt_fibinfo_free(&fnhe->fnhe_rth_output);

			kfree(fnhe);

			fnhe = next;
		}
	}
	kfree(hash);
}

static void rt_fibinfo_free_cpus(struct rtable __rcu * __percpu *rtp)
{
	int cpu;

	if (!rtp)
		return;

	for_each_possible_cpu(cpu) {
		struct rtable *rt;

		rt = rcu_dereference_protected(*per_cpu_ptr(rtp, cpu), 1);
		if (rt) {
			dst_dev_put(&rt->dst);
			dst_release_immediate(&rt->dst);
		}
	}
	free_percpu(rtp);
}

void fib_nh_common_release(struct fib_nh_common *nhc)
{
	netdev_put(nhc->nhc_dev, &nhc->nhc_dev_tracker);
	lwtstate_put(nhc->nhc_lwtstate);
	rt_fibinfo_free_cpus(nhc->nhc_pcpu_rth_output);
	rt_fibinfo_free(&nhc->nhc_rth_input);
	free_nh_exceptions(nhc);
}

int fib_nh_common_init(struct net *net, struct fib_nh_common *nhc,
		       struct nlattr *encap, u16 encap_type,
		       void *cfg, gfp_t gfp_flags,
		       struct netlink_ext_ack *extack)
{
	int err;

	nhc->nhc_pcpu_rth_output = alloc_percpu_gfp(struct rtable __rcu *,
						    gfp_flags);
	if (!nhc->nhc_pcpu_rth_output)
		return -ENOMEM;

	if (encap) {
		struct lwtunnel_state *lwtstate;

		err = lwtunnel_build_state(net, encap_type, encap,
					   nhc->nhc_family, cfg, &lwtstate,
					   extack);
		if (err)
			goto lwt_failure;

		nhc->nhc_lwtstate = lwtstate_get(lwtstate);
	}

	return 0;

lwt_failure:
	rt_fibinfo_free_cpus(nhc->nhc_pcpu_rth_output);
	nhc->nhc_pcpu_rth_output = NULL;
	return err;
}

/* Update the PMTU of exceptions when:
 * - the new MTU of the first hop becomes smaller than the PMTU
 * - the old MTU was the same as the PMTU, and it limited discovery of
 *   larger MTUs on the path. With that limit raised, we can now
 *   discover larger MTUs
 * A special case is locked exceptions, for which the PMTU is smaller
 * than the minimal accepted PMTU:
 * - if the new MTU is greater than the PMTU, don't make any change
 * - otherwise, unlock and set PMTU
 */
void fib_nhc_update_mtu(struct fib_nh_common *nhc, u32 new, u32 orig)
{
	struct fnhe_hash_bucket *bucket;
	int i;

	bucket = rcu_dereference_protected(nhc->nhc_exceptions, 1);
	if (!bucket)
		return;

	for (i = 0; i < FNHE_HASH_SIZE; i++) {
		struct fib_nh_exception *fnhe;

		for (fnhe = rcu_dereference_protected(bucket[i].chain, 1);
		     fnhe;
		     fnhe = rcu_dereference_protected(fnhe->fnhe_next, 1)) {
			if (fnhe->fnhe_mtu_locked) {
				if (new <= fnhe->fnhe_pmtu) {
					fnhe->fnhe_pmtu = new;
					fnhe->fnhe_mtu_locked = false;
				}
			} else if (new < fnhe->fnhe_pmtu ||
				   orig == fnhe->fnhe_pmtu) {
				fnhe->fnhe_pmtu = new;
			}
		}
	}
}
