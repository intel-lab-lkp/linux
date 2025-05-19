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
	int i = 0, j = 0;

	ucounts = find_ucounts(ns, uid, hashent);
	if (ucounts)
		return ucounts;

	new = kzalloc(sizeof(*new), GFP_KERNEL);
	if (!new)
		return NULL;

	new->ns = ns;
	new->uid = uid;
	rcuref_init(&new->count, 1);
	for (i = 0; i < UCOUNT_RLIMIT_COUNTS; ++i) {
		if (percpu_counter_init(&new->rlimit[i], 0, GFP_KERNEL))
			goto failed;
	}
	spin_lock_irq(&ucounts_lock);
	ucounts = find_ucounts(ns, uid, hashent);
	if (ucounts) {
		spin_unlock_irq(&ucounts_lock);
		for (j = 0; j < UCOUNT_RLIMIT_COUNTS; ++j)
			percpu_counter_destroy(&new->rlimit[j]);
		kfree(new);
		return ucounts;
	}

	hlist_nulls_add_head_rcu(&new->node, hashent);
	get_user_ns(new->ns);
	spin_unlock_irq(&ucounts_lock);
	return new;

failed:
	for (j = 0; i > 0 && j < i - 1; ++j)
		percpu_counter_destroy(&new->rlimit[j]);
	kfree(new);
	return NULL;
}

/*
 * Whether all the rlimits are zero.
 * For now, only UCOUNT_RLIMIT_SIGPENDING is considered.
 * Other rlimit can be added.
 */
static bool rlimits_are_zero(struct ucounts *ucounts)
{
	int rtypes[] = { UCOUNT_RLIMIT_SIGPENDING };
	int rtype;

	for (int i = 0; i < sizeof(rtypes)/sizeof(int); ++i) {
		rtype = rtypes[i];
		if (get_rlimit_value(ucounts, rtype) > 0)
			return false;
	}
	return true;
}

/*
 * Ucounts can be freed only when the ucount->count is released
 * and the rlimits are zero.
 * The caller should hold rcu_read_lock();
 */
static bool ucounts_can_be_freed(struct ucounts *ucounts)
{
	if (rcuref_read(&ucounts->count) > 0)
		return false;
	if (!rlimits_are_zero(ucounts))
		return false;
	/* Prevent double free */
	return atomic_long_cmpxchg(&ucounts->freed, 0, 1) == 0;
}

static void free_ucounts(struct ucounts *ucounts)
{
	unsigned long flags;

	spin_lock_irqsave(&ucounts_lock, flags);
	hlist_nulls_del_rcu(&ucounts->node);
	spin_unlock_irqrestore(&ucounts_lock, flags);
	for (int i = 0; i < UCOUNT_RLIMIT_COUNTS; ++i)
		percpu_counter_destroy(&ucounts->rlimit[i]);
	put_user_ns(ucounts->ns);
	kfree_rcu(ucounts, rcu);
}

void put_ucounts(struct ucounts *ucounts)
{
	rcu_read_lock();
	if (rcuref_put(&ucounts->count) &&
	    ucounts_can_be_freed(ucounts)) {
		rcu_read_unlock();
		free_ucounts(ucounts);
		return;
	}
	rcu_read_unlock();
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

bool inc_rlimit_ucounts_limit(struct ucounts *ucounts, enum rlimit_type type,
					long v, long limit)
{
	struct ucounts *iter;
	long max = LONG_MAX;
	bool good = true;

	for (iter = ucounts; iter; iter = iter->ns->ucounts) {
		max = min(limit, max);
		if (!percpu_counter_limited_add(&iter->rlimit[type], max, v))
			good = false;

		max = get_userns_rlimit_max(iter->ns, type);
	}
	return good;
}

void dec_rlimit_ucounts(struct ucounts *ucounts, enum rlimit_type type, long v)
{
	struct ucounts *iter;

	for (iter = ucounts; iter; iter = iter->ns->ucounts)
		percpu_counter_sub(&iter->rlimit[type], v);
}

/*
 * The inc_rlimit_get_ucounts does not grab the refcount.
 * The rlimit_release should be called very time the rlimit is decremented.
 */
static void do_dec_rlimit_put_ucounts(struct ucounts *ucounts,
				struct ucounts *last, enum rlimit_type type)
{
	struct ucounts *iter, *next;
	for (iter = ucounts; iter != last; iter = next) {
		bool to_free;

		rcu_read_lock();
		percpu_counter_sub(&iter->rlimit[type], 1);
		next = iter->ns->ucounts;
		to_free = ucounts_can_be_freed(iter);
		rcu_read_unlock();
		/* If ucounts->count is zero and the rlimits are zero, free ucounts */
		if (to_free)
			free_ucounts(iter);
	}
}

void dec_rlimit_put_ucounts(struct ucounts *ucounts, enum rlimit_type type)
{
	do_dec_rlimit_put_ucounts(ucounts, NULL, type);
}

/*
 * Though this function does not grab the refcount, it is promised that the
 * ucounts will not be freed as long as there have any rlimit pins to it.
 * Caller must hold a reference to ucounts or under rcu_read_lock().
 *
 * Return 1 if increments successful, otherwise return 0.
 */
long inc_rlimit_get_ucounts(struct ucounts *ucounts, enum rlimit_type type,
			    bool override_rlimit, long limit)
{
	struct ucounts *iter;
	long max = LONG_MAX;
	long ret = 0;

	if (override_rlimit)
		limit = LONG_MAX;

	for (iter = ucounts; iter; iter = iter->ns->ucounts) {
		/* Can not exceed the limit(inputed) or the ns->rlimit_max */
		max = min(limit, max);

		if (!percpu_counter_limited_add(&iter->rlimit[type], max, 1))
			goto dec_unwind;

		if (!override_rlimit)
			max = get_userns_rlimit_max(iter->ns, type);
	}
	return 1;
dec_unwind:
	do_dec_rlimit_put_ucounts(ucounts, iter, type);
	return ret;
}

bool is_rlimit_overlimit(struct ucounts *ucounts, enum rlimit_type type, unsigned long rlimit)
{
	struct ucounts *iter;
	long max = rlimit;
	if (rlimit > LONG_MAX)
		max = LONG_MAX;

	for (iter = ucounts; iter; iter = iter->ns->ucounts) {
		/* iter->rlimit[type] > max return 1 */
		if (percpu_counter_compare(&iter->rlimit[type], max) > 0)
			return true;

		max = get_userns_rlimit_max(iter->ns, type);
	}
	return false;
}

void __init ucounts_init(void)
{
	for (int i = 0; i < UCOUNT_RLIMIT_COUNTS; ++i)
		percpu_counter_init(&init_ucounts.rlimit[i], 0, GFP_KERNEL);
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
