// SPDX-License-Identifier: GPL-2.0-only

#include <linux/stat.h>
#include <linux/sysctl.h>
#include <linux/slab.h>
#include <linux/cred.h>
#include <linux/hash.h>
#include <linux/kmemleak.h>
#include <linux/user_namespace.h>

struct ucounts init_ucounts = {
	.ns    = &init_user_ns,
	.uid   = GLOBAL_ROOT_UID,
	.count = RCUREF_INIT(1),
};

#define UCOUNTS_HASHTABLE_BITS 10
#define UCOUNTS_HASHTABLE_ENTRIES (1 << UCOUNTS_HASHTABLE_BITS)
#define UCOUNT_BATCH_SIZE 16

static struct hlist_nulls_head ucounts_hashtable[UCOUNTS_HASHTABLE_ENTRIES] = {
	[0 ... UCOUNTS_HASHTABLE_ENTRIES - 1] = HLIST_NULLS_HEAD_INIT(0)
};
static DEFINE_SPINLOCK(ucounts_lock);

#define ucounts_hashfn(ns, uid)						\
	hash_long((unsigned long)__kuid_val(uid) + (unsigned long)(ns), \
		  UCOUNTS_HASHTABLE_BITS)
#define ucounts_hashentry(ns, uid)	\
	(ucounts_hashtable + ucounts_hashfn(ns, uid))

#ifdef CONFIG_SYSCTL
static struct ctl_table_set *
set_lookup(struct ctl_table_root *root)
{
	return &current_user_ns()->set;
}

static int set_is_seen(struct ctl_table_set *set)
{
	return &current_user_ns()->set == set;
}

static int set_permissions(struct ctl_table_header *head,
			   const struct ctl_table *table)
{
	struct user_namespace *user_ns =
		container_of(head->set, struct user_namespace, set);
	int mode;

	/* Allow users with CAP_SYS_RESOURCE unrestrained access */
	if (ns_capable(user_ns, CAP_SYS_RESOURCE))
		mode = (table->mode & S_IRWXU) >> 6;
	else
	/* Allow all others at most read-only access */
		mode = table->mode & S_IROTH;
	return (mode << 6) | (mode << 3) | mode;
}

static struct ctl_table_root set_root = {
	.lookup = set_lookup,
	.permissions = set_permissions,
};

static long ue_zero = 0;
static long ue_int_max = INT_MAX;

#define UCOUNT_ENTRY(name)					\
	{							\
		.procname	= name,				\
		.maxlen		= sizeof(long),			\
		.mode		= 0644,				\
		.proc_handler	= proc_doulongvec_minmax,	\
		.extra1		= &ue_zero,			\
		.extra2		= &ue_int_max,			\
	}
static const struct ctl_table user_table[] = {
	UCOUNT_ENTRY("max_user_namespaces"),
	UCOUNT_ENTRY("max_pid_namespaces"),
	UCOUNT_ENTRY("max_uts_namespaces"),
	UCOUNT_ENTRY("max_ipc_namespaces"),
	UCOUNT_ENTRY("max_net_namespaces"),
	UCOUNT_ENTRY("max_mnt_namespaces"),
	UCOUNT_ENTRY("max_cgroup_namespaces"),
	UCOUNT_ENTRY("max_time_namespaces"),
#ifdef CONFIG_INOTIFY_USER
	UCOUNT_ENTRY("max_inotify_instances"),
	UCOUNT_ENTRY("max_inotify_watches"),
#endif
#ifdef CONFIG_FANOTIFY
	UCOUNT_ENTRY("max_fanotify_groups"),
	UCOUNT_ENTRY("max_fanotify_marks"),
#endif
};
#endif /* CONFIG_SYSCTL */

bool setup_userns_sysctls(struct user_namespace *ns)
{
#ifdef CONFIG_SYSCTL
	struct ctl_table *tbl;

	BUILD_BUG_ON(ARRAY_SIZE(user_table) != UCOUNT_COUNTS);
	setup_sysctl_set(&ns->set, &set_root, set_is_seen);
	tbl = kmemdup(user_table, sizeof(user_table), GFP_KERNEL);
	if (tbl) {
		int i;
		for (i = 0; i < UCOUNT_COUNTS; i++) {
			tbl[i].data = &ns->ucount_max[i];
		}
		ns->sysctls = __register_sysctl_table(&ns->set, "user", tbl,
						      ARRAY_SIZE(user_table));
	}
	if (!ns->sysctls) {
		kfree(tbl);
		retire_sysctl_set(&ns->set);
		return false;
	}
#endif
	return true;
}

void retire_userns_sysctls(struct user_namespace *ns)
{
#ifdef CONFIG_SYSCTL
	const struct ctl_table *tbl;

	tbl = ns->sysctls->ctl_table_arg;
	unregister_sysctl_table(ns->sysctls);
	retire_sysctl_set(&ns->set);
	kfree(tbl);
#endif
}

static struct ucounts *find_ucounts(struct user_namespace *ns, kuid_t uid,
				    struct hlist_nulls_head *hashent)
{
	struct ucounts *ucounts;
	struct hlist_nulls_node *pos;

	guard(rcu)();
	hlist_nulls_for_each_entry_rcu(ucounts, pos, hashent, node) {
		if (uid_eq(ucounts->uid, uid) && (ucounts->ns == ns)) {
			if (rcuref_get(&ucounts->count))
				return ucounts;
		}
	}
	return NULL;
}

static void hlist_add_ucounts(struct ucounts *ucounts)
{
	struct hlist_nulls_head *hashent = ucounts_hashentry(ucounts->ns, ucounts->uid);

	spin_lock_irq(&ucounts_lock);
	hlist_nulls_add_head_rcu(&ucounts->node, hashent);
	spin_unlock_irq(&ucounts_lock);
}

struct ucounts *alloc_ucounts(struct user_namespace *ns, kuid_t uid)
{
	struct hlist_nulls_head *hashent = ucounts_hashentry(ns, uid);
	struct ucounts *ucounts, *new;

	ucounts = find_ucounts(ns, uid, hashent);
	if (ucounts)
		return ucounts;

	new = kzalloc(sizeof(*new), GFP_KERNEL);
	if (!new)
		return NULL;

	new->ns = ns;
	new->uid = uid;
	rcuref_init(&new->count, 1);

	spin_lock_irq(&ucounts_lock);
	ucounts = find_ucounts(ns, uid, hashent);
	if (ucounts) {
		spin_unlock_irq(&ucounts_lock);
		kfree(new);
		return ucounts;
	}

	hlist_nulls_add_head_rcu(&new->node, hashent);
	get_user_ns(new->ns);
	spin_unlock_irq(&ucounts_lock);
	return new;
}

void put_ucounts(struct ucounts *ucounts)
{
	unsigned long flags;

	if (rcuref_put(&ucounts->count)) {
		spin_lock_irqsave(&ucounts_lock, flags);
		hlist_nulls_del_rcu(&ucounts->node);
		spin_unlock_irqrestore(&ucounts_lock, flags);

		put_user_ns(ucounts->ns);
		kfree_rcu(ucounts, rcu);
	}
}

static inline bool atomic_long_inc_below(atomic_long_t *v, int u)
{
	long c, old;
	c = atomic_long_read(v);
	for (;;) {
		if (unlikely(c >= u))
			return false;
		old = atomic_long_cmpxchg(v, c, c+1);
		if (likely(old == c))
			return true;
		c = old;
	}
}

struct ucounts *inc_ucount(struct user_namespace *ns, kuid_t uid,
			   enum ucount_type type)
{
	struct ucounts *ucounts, *iter, *bad;
	struct user_namespace *tns;
	ucounts = alloc_ucounts(ns, uid);
	for (iter = ucounts; iter; iter = tns->ucounts) {
		long max;
		tns = iter->ns;
		max = READ_ONCE(tns->ucount_max[type]);
		if (!atomic_long_inc_below(&iter->ucount[type], max))
			goto fail;
	}
	return ucounts;
fail:
	bad = iter;
	for (iter = ucounts; iter != bad; iter = iter->ns->ucounts)
		atomic_long_dec(&iter->ucount[type]);

	put_ucounts(ucounts);
	return NULL;
}

void dec_ucount(struct ucounts *ucounts, enum ucount_type type)
{
	struct ucounts *iter;
	for (iter = ucounts; iter; iter = iter->ns->ucounts) {
		long dec = atomic_long_dec_if_positive(&iter->ucount[type]);
		WARN_ON_ONCE(dec < 0);
	}
	put_ucounts(ucounts);
}

long inc_rlimit_ucounts(struct ucounts *ucounts, enum rlimit_type type, long v)
{
	struct ucounts *iter;
	long max = LONG_MAX;
	long ret = 0;

	for (iter = ucounts; iter; iter = iter->ns->ucounts) {
		long new = atomic_long_add_return(v, &iter->rlimit[type]);
		if (new < 0 || new > max)
			ret = LONG_MAX;
		else if (iter == ucounts)
			ret = new;
		max = get_userns_rlimit_max(iter->ns, type);
	}
	return ret;
}

bool dec_rlimit_ucounts(struct ucounts *ucounts, enum rlimit_type type, long v)
{
	struct ucounts *iter;
	long new = -1; /* Silence compiler warning */
	for (iter = ucounts; iter; iter = iter->ns->ucounts) {
		long dec = atomic_long_sub_return(v, &iter->rlimit[type]);
		WARN_ON_ONCE(dec < 0);
		if (iter == ucounts)
			new = dec;
	}
	return (new == 0);
}

static void __dec_rlimit_put_ucounts(struct ucounts *ucounts,
				enum rlimit_type type, long v)
{
	long dec = atomic_long_sub_return(v, &ucounts->rlimit[type]);

	WARN_ON_ONCE(dec < 0);
	if (dec == 0)
		put_ucounts(ucounts);
}

static long __inc_rlimit_get_ucounts(struct ucounts *ucounts, enum rlimit_type type, long v)
{
	long new = atomic_long_add_return(v, &ucounts->rlimit[type]);

	/*
	 * Grab an extra ucount reference for the caller when
	 * the rlimit count was previously 0.
	 */
	if (new == v && !get_ucounts(ucounts)) {
		long dec = atomic_long_sub_return(v, &ucounts->rlimit[type]);

		WARN_ON_ONCE(dec < 0);
		return 0;
	}
	return new;
}

static void do_dec_rlimit_put_ucounts(struct ucounts *ucounts,
				struct ucounts *last, enum rlimit_type type, long v)
{
	struct ucounts *iter, *next;
	for (iter = ucounts; iter != last; iter = next) {
		next = iter->ns->ucounts;
		__dec_rlimit_put_ucounts(ucounts, type, v);
	}
}

void dec_rlimit_put_ucounts(struct ucounts *ucounts, enum rlimit_type type)
{
	struct user_namespace *ns = ucounts->ns;
	int cache;

	if (ns != &init_user_ns) {
		__dec_rlimit_put_ucounts(ucounts, type, 1);
		cache = atomic_add_return(1, &ns->rlimit_cache[type]);
		if (cache > UCOUNT_BATCH_SIZE) {
			cache = atomic_sub_return(UCOUNT_BATCH_SIZE,
						  &ns->rlimit_cache[type]);
			if (cache > 0)
				do_dec_rlimit_put_ucounts(ns->ucounts, NULL,
							  type, UCOUNT_BATCH_SIZE);
			else
				atomic_add(UCOUNT_BATCH_SIZE, &ns->rlimit_cache[type]);
		}
	} else {
		do_dec_rlimit_put_ucounts(ucounts, NULL, type, 1);
	}
}

/* Drain the root cache, return how many cache have been relcaimed */
static int rlimit_drain_type_cache(struct user_namespace *root, enum rlimit_type type)
{
	struct user_namespace *child;
	int reclaim_cache = 0;

	rcu_read_lock();
	ns_for_each_child_pre(child, root) {
		int cache;
retry:
		cache = atomic_read(&child->rlimit_cache[type]);
		if (cache > 0) {
			int old = atomic_cmpxchg(&child->rlimit_cache[type], cache, 0);

			if (cache == old) {
				reclaim_cache += cache;
				do_dec_rlimit_put_ucounts(child->ucounts, NULL, type, cache);
			} else {
				goto retry;
			}
		}
	}
	rcu_read_unlock();
	return reclaim_cache;
}

void rlimit_drain_cache(struct user_namespace *root)
{
	for (int i = 0; i < UCOUNT_RLIMIT_COUNTS; i++)
		rlimit_drain_type_cache(root, i);
}

static bool rlimit_charge_cache(struct ucounts *ucounts, enum rlimit_type type)
{
	struct ucounts *iter;
	long max = LONG_MAX;
	long new;
	struct user_namespace *ns = ucounts->ns;

	for (iter = ns->ucounts; iter; iter = iter->ns->ucounts) {
		max = get_userns_rlimit_max(iter->ns, type);
		new = __inc_rlimit_get_ucounts(iter, type, UCOUNT_BATCH_SIZE);
		if (new <= 0 || new > max)
			goto dec_unwind;
	}

	/* charge ok, add the ns's cache */
	atomic_add_return(UCOUNT_BATCH_SIZE, &ucounts->ns->rlimit_cache[type]);
	return true;

dec_unwind:
	do_dec_rlimit_put_ucounts(ns->ucounts, iter, type, UCOUNT_BATCH_SIZE);
	return false;
}

long inc_rlimit_get_ucounts(struct ucounts *ucounts, enum rlimit_type type,
			    bool override_rlimit, long tlimit)
{
	/* Caller must hold a reference to ucounts */
	struct ucounts *iter;
	long max = LONG_MAX;
	long ret = 0;
	struct user_namespace *ns = ucounts->ns;
	bool is_trying = false;
	bool non_cache = false;
	long new;

try_cache:
	/* If the ucounts.ns is not init_user_ns, and it has cache in its ns, consume cache */
	if (ns != &init_user_ns) {
		if (atomic_dec_return(&ns->rlimit_cache[type]) >= 0) {
			new =  __inc_rlimit_get_ucounts(ucounts, type, 1);
			/*
			 * If new is below tlimit, return success
			 * Otherwise, goto non-cache logic. It should keep the
			 * rlimit below the tlimit as much as possible
			 */
			if (new <= tlimit)
				return new;
			non_cache = true;
		}
		/* Restore the previously incremented value */
		atomic_inc(&ns->rlimit_cache[type]);

		if (!non_cache && !is_trying &&
		    rlimit_charge_cache(ucounts, type)) {
			is_trying = true;
			goto try_cache;
		}
	}

	for (iter = ucounts; iter; iter = iter->ns->ucounts) {
retry_inc:
		new = __inc_rlimit_get_ucounts(iter, type, 1);

		/*
		 * When the 'iter' is equal to 'ucounts', the 'new' value is what will be returned.
		 *
		 * Case 1: If the return value is larger than 'tlimit'.
		 * Case 2: If the 'new' value is larger than the maximum of 'rlimit_max'.
		 *
		 * In both cases, we need to drain the cache. This is because when the cache is
		 * present, the value might exceed the acceptable threshold. However, when the
		 * cache is removed,the value should fall within the allowed limit
		 */
		if (iter == ucounts)
			ret = new;

		if ((new > max || ret > tlimit) &&
			rlimit_drain_type_cache(iter->ns, type) > 0) {
			__dec_rlimit_put_ucounts(iter, type, 1);
			goto retry_inc;
		}

		if (new <= 0 || new > max)
			goto dec_unwind;

		if (!override_rlimit)
			max = get_userns_rlimit_max(iter->ns, type);
	}
	return ret;

dec_unwind:
	do_dec_rlimit_put_ucounts(ucounts, iter, type, 1);
	return 0;
}

bool is_rlimit_overlimit(struct ucounts *ucounts, enum rlimit_type type, unsigned long rlimit)
{
	struct ucounts *iter;
	long max = rlimit;
	if (rlimit > LONG_MAX)
		max = LONG_MAX;
	for (iter = ucounts; iter; iter = iter->ns->ucounts) {
		long val = get_rlimit_value(iter, type);
		if (val < 0 || val > max)
			return true;
		max = get_userns_rlimit_max(iter->ns, type);
	}
	return false;
}

static __init int user_namespace_sysctl_init(void)
{
#ifdef CONFIG_SYSCTL
	static struct ctl_table_header *user_header;
	static struct ctl_table empty[1];
	/*
	 * It is necessary to register the user directory in the
	 * default set so that registrations in the child sets work
	 * properly.
	 */
	user_header = register_sysctl_sz("user", empty, 0);
	kmemleak_ignore(user_header);
	BUG_ON(!user_header);
	BUG_ON(!setup_userns_sysctls(&init_user_ns));
#endif
	hlist_add_ucounts(&init_ucounts);
	inc_rlimit_ucounts(&init_ucounts, UCOUNT_RLIMIT_NPROC, 1);
	return 0;
}
subsys_initcall(user_namespace_sysctl_init);
