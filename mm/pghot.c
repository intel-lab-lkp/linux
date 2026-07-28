// SPDX-License-Identifier: GPL-2.0
/*
 * Maintains information about hot pages from slower tier nodes and
 * promotes them.
 *
 * Per-PFN hotness information is stored for lower tier nodes in
 * mem_section.
 *
 * In the default mode, a single byte (u8) is used to store
 * the frequency of access and last access time. Promotions are done
 * to a default toptier NID.
 *
 * A kernel thread named kmigrated is provided to migrate or promote
 * the hot pages. kmigrated runs for each lower tier node. It iterates
 * over the node's PFNs and  migrates pages marked for migration into
 * their targeted nodes.
 */
#include <linux/mm.h>
#include <linux/migrate.h>
#include <linux/memory.h>
#include <linux/memory-tiers.h>
#include <linux/debugfs.h>
#include <linux/rcupdate.h>
#include <linux/pghot.h>

unsigned int pghot_freq_window_min = PGHOT_FREQ_WINDOW_MIN;
unsigned int pghot_freq_window_max = PGHOT_FREQ_WINDOW_MAX;
unsigned int pghot_freq_threshold_max = PGHOT_FREQ_MAX;
unsigned int kmigrated_sleep_ms = KMIGRATED_SLEEP_MS_DEFAULT;
unsigned int kmigrated_batch_nr = KMIGRATED_BATCH_NR_DEFAULT;

unsigned int sysctl_pghot_src_enabled;
unsigned int sysctl_pghot_target_nid = PGHOT_DEFAULT_NODE;
unsigned int sysctl_pghot_freq_window = PGHOT_FREQ_WINDOW_DEFAULT;
unsigned int sysctl_pghot_freq_threshold = PGHOT_FREQ_THRESHOLD_DEFAULT;

DEFINE_STATIC_KEY_FALSE(pghot_src_hwhints);
DEFINE_STATIC_KEY_FALSE(pghot_src_hintfaults);
DEFINE_MUTEX(pghot_tunables_lock);

#ifdef CONFIG_SYSCTL
static void pghot_src_enabled_update(unsigned int enabled)
{
	unsigned int changed = sysctl_pghot_src_enabled ^ enabled;

	if (changed & PGHOT_HINTFAULTS_ENABLED) {
		if (enabled & PGHOT_HINTFAULTS_ENABLED)
			static_branch_enable(&pghot_src_hintfaults);
		else
			static_branch_disable(&pghot_src_hintfaults);
	}

	if (changed & PGHOT_HWHINTS_ENABLED) {
		if (enabled & PGHOT_HWHINTS_ENABLED)
			static_branch_enable(&pghot_src_hwhints);
		else
			static_branch_disable(&pghot_src_hwhints);
	}
}

static int sysctl_src_enabled_handler(const struct ctl_table *table, int write,
				      void *buffer, size_t *lenp, loff_t *ppos)
{
	struct ctl_table t;
	int err;
	unsigned int enabled;

	guard(mutex)(&pghot_tunables_lock);

	enabled = sysctl_pghot_src_enabled;
	t = *table;
	t.data = &enabled;
	err = proc_dointvec_minmax(&t, write, buffer, lenp, ppos);
	if (err < 0)
		return err;

	if (write) {
		if (enabled & ~PGHOT_SRC_ENABLED_MASK)
			return -EINVAL;
		pghot_src_enabled_update(enabled);
		sysctl_pghot_src_enabled = enabled;
	}
	return err;
}

static int sysctl_target_nid_handler(const struct ctl_table *table, int write,
				     void *buffer, size_t *lenp, loff_t *ppos)
{
	struct ctl_table t;
	int err;
	unsigned int nid;

	guard(mutex)(&pghot_tunables_lock);

	nid = sysctl_pghot_target_nid;
	t = *table;
	t.data = &nid;
	err = proc_dointvec_minmax(&t, write, buffer, lenp, ppos);
	if (err < 0)
		return err;

	if (write) {
		if (!numa_valid_node(nid) || nid > PGHOT_NID_MAX ||
		    !node_online(nid) || !node_is_toptier(nid))
			return -EINVAL;
		sysctl_pghot_target_nid = nid;
	}
	return err;
}

static const struct ctl_table pghot_sysctls[] = {
	{
		.procname	= "pghot_enabled_sources",
		.data		= &sysctl_pghot_src_enabled,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= sysctl_src_enabled_handler,
	},
	{
		.procname       = "pghot_promote_freq_window_ms",
		.data           = &sysctl_pghot_freq_window,
		.maxlen         = sizeof(unsigned int),
		.mode           = 0644,
		.proc_handler   = proc_dointvec_minmax,
		.extra1         = &pghot_freq_window_min,
		.extra2         = &pghot_freq_window_max,
	},
	{
		.procname	= "pghot_target_nid",
		.data		= &sysctl_pghot_target_nid,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= sysctl_target_nid_handler,
	},
	{
		.procname	= "pghot_freq_threshold",
		.data		= &sysctl_pghot_freq_threshold,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ONE,
		.extra2		= &pghot_freq_threshold_max,
	},
};

static void __init pghot_sysctl_init(void)
{
	register_sysctl_init("vm", pghot_sysctls);
}
#else
static inline void pghot_sysctl_init(void) { }
#endif

static bool kmigrated_started __ro_after_init;

/**
 * pghot_record_access() - Record page accesses from lower tier memory
 * for the purpose of tracking page hotness and subsequent promotion.
 *
 * @pfn: PFN of the page
 * @nid: Unused
 * @src: The identifier of the sub-system that reports the access
 * @now: Access time in jiffies
 *
 * Updates the frequency and time of access and marks the page as
 * ready for migration if the frequency crosses a threshold. The pages
 * marked for migration are migrated by kmigrated kernel thread.
 *
 * Return: 0 on success and -EINVAL on failure to record the access.
 */
int pghot_record_access(unsigned long pfn, int nid, int src, unsigned long now)
{
	struct mem_section *ms;
	struct folio *folio;
	struct pghot_hot_map *hot_map;
	phi_t *phi;
	struct page *page;
	int src_nid;

	if (!kmigrated_started)
		return 0;

	if (!pghot_nid_valid(nid))
		return -EINVAL;

	switch (src) {
	case PGHOT_HINTFAULTS:
		if (!static_branch_unlikely(&pghot_src_hintfaults))
			return 0;
		count_vm_event(PGHOT_RECORDED_HINTFAULTS);
		break;
	case PGHOT_HWHINTS:
		if (!static_branch_unlikely(&pghot_src_hwhints))
			return 0;
		count_vm_event(PGHOT_RECORDED_HWHINTS);
		break;
	default:
		return -EINVAL;
	}

	src_nid = pfn_to_nid(pfn);
	if (src_nid == nid)
		return 0;

	/*
	 * Record only accesses from lower tiers.
	 */
	if (node_is_toptier(src_nid))
		return 0;

	/*
	 * Reject the non-migratable pages right away.
	 */
	page = pfn_to_online_page(pfn);
	if (!page || is_zone_device_page(page))
		return 0;

	folio = page_folio(page);
	if (!folio_try_get(folio))
		return 0;

	if (unlikely(page_folio(page) != folio))
		goto out;

	if (!folio_test_lru(folio))
		goto out;

	/* Get the hotness slot corresponding to the 1st PFN of the folio */
	pfn = folio_pfn(folio);
	ms = __pfn_to_section(pfn);

	rcu_read_lock();
	hot_map = ms ? rcu_dereference(ms->hot_map) : NULL;
	if (!hot_map) {
		rcu_read_unlock();
		goto out;
	}

	phi = &hot_map->phi[pfn % PAGES_PER_SECTION];

	count_vm_event(PGHOT_RECORDED_ACCESSES);

	/*
	 * Update the hotness parameters.
	 */
	if (pghot_update_record(phi, nid, now)) {
		set_bit(PGHOT_SECTION_HOT_BIT, &hot_map->flags);
		set_bit(PGDAT_KMIGRATED_ACTIVATE, &page_pgdat(page)->flags);
	}
	rcu_read_unlock();
out:
	folio_put(folio);
	return 0;
}

static int pghot_get_hotness(unsigned long pfn, int *nid, int *freq,
			     unsigned long *time)
{
	struct pghot_hot_map *hot_map;
	struct mem_section *ms;
	phi_t *phi;
	int ret;

	ms = __pfn_to_section(pfn);

	rcu_read_lock();
	hot_map = ms ? rcu_dereference(ms->hot_map) : NULL;
	if (!hot_map) {
		rcu_read_unlock();
		return -EINVAL;
	}

	phi = &hot_map->phi[pfn % PAGES_PER_SECTION];
	ret = pghot_get_record(phi, nid, freq, time);
	rcu_read_unlock();

	return ret;
}

/*
 * Walks the PFNs of the zone, isolates and migrates them in batches.
 */
static void kmigrated_walk_zone(unsigned long start_pfn, unsigned long end_pfn,
				int src_nid)
{
	struct mem_cgroup *cur_memcg = NULL;
	int cur_nid = NUMA_NO_NODE;
	LIST_HEAD(migrate_list);
	int batch_count = 0;
	struct folio *folio;
	struct page *page;
	unsigned long pfn;

	pfn = start_pfn;
	do {
		int nid = NUMA_NO_NODE, nr = 1;
		struct mem_cgroup *memcg;
		unsigned long time = 0;
		int freq = 0;

		if (!pfn_valid(pfn))
			goto out_next;

		page = pfn_to_online_page(pfn);
		if (!page)
			goto out_next;

		folio = page_folio(page);
		if (!folio_try_get(folio))
			goto out_next;

		if (unlikely(page_folio(page) != folio)) {
			folio_put(folio);
			goto out_next;
		}

		nr = folio_nr_pages(folio);
		if (folio_nid(folio) != src_nid) {
			folio_put(folio);
			goto out_next;
		}

		if (!folio_test_lru(folio)) {
			folio_put(folio);
			goto out_next;
		}

		if (pghot_get_hotness(pfn, &nid, &freq, &time)) {
			folio_put(folio);
			goto out_next;
		}

		if (folio_nid(folio) == nid) {
			folio_put(folio);
			goto out_next;
		}

		if (migrate_misplaced_folio_prepare(folio, NULL, nid)) {
			folio_put(folio);
			goto out_next;
		}

		rcu_read_lock();
		memcg = folio_memcg(folio);
		rcu_read_unlock();
		if (cur_nid == NUMA_NO_NODE) {
			cur_nid = nid;
			cur_memcg = memcg;
		}

		/* If NID or memcg changed, flush the previous batch first */
		if (cur_nid != nid || cur_memcg != memcg) {
			if (!list_empty(&migrate_list))
				promote_misplaced_memcg_folios(&migrate_list, cur_nid);
			cur_nid = nid;
			cur_memcg = memcg;
			batch_count = 0;
			cond_resched();
		}

		list_add(&folio->lru, &migrate_list);
		folio_put(folio);

		if (++batch_count > READ_ONCE(kmigrated_batch_nr)) {
			promote_misplaced_memcg_folios(&migrate_list, cur_nid);
			batch_count = 0;
			cond_resched();
		}
out_next:
		pfn += nr;
	} while (pfn < end_pfn);
	if (!list_empty(&migrate_list))
		promote_misplaced_memcg_folios(&migrate_list, cur_nid);
}

static void kmigrated_do_work(pg_data_t *pgdat)
{
	unsigned long section_nr, s_begin, start_pfn;
	struct pghot_hot_map *hot_map;
	struct mem_section *ms;
	bool hot;
	int nid;

	clear_bit(PGDAT_KMIGRATED_ACTIVATE, &pgdat->flags);
	s_begin = next_present_section_nr(-1);
	for_each_present_section_nr(s_begin, section_nr) {
		start_pfn = section_nr_to_pfn(section_nr);
		ms = __nr_to_section(section_nr);

		if (!pfn_valid(start_pfn))
			continue;

		nid = pfn_to_nid(start_pfn);
		if (node_is_toptier(nid) || nid != pgdat->node_id)
			continue;

		rcu_read_lock();
		hot_map = rcu_dereference(ms->hot_map);
		hot = hot_map &&
		      test_and_clear_bit(PGHOT_SECTION_HOT_BIT, &hot_map->flags);
		rcu_read_unlock();

		if (!hot)
			continue;

		kmigrated_walk_zone(start_pfn, start_pfn + PAGES_PER_SECTION,
				    pgdat->node_id);
	}
}

static inline bool kmigrated_work_requested(pg_data_t *pgdat)
{
	return test_bit(PGDAT_KMIGRATED_ACTIVATE, &pgdat->flags);
}

/*
 * Per-node kthread that iterates over its PFNs and migrates the
 * pages that have been marked for migration.
 */
static int kmigrated(void *p)
{
	pg_data_t *pgdat = p;

	while (!kthread_should_stop()) {
		long timeout = msecs_to_jiffies(READ_ONCE(kmigrated_sleep_ms));

		if (wait_event_timeout(pgdat->kmigrated_wait, kmigrated_work_requested(pgdat),
				       timeout))
			kmigrated_do_work(pgdat);
	}
	return 0;
}

static int kmigrated_run(int nid)
{
	pg_data_t *pgdat = NODE_DATA(nid);
	int ret;

	if (!pgdat->kmigrated) {
		pgdat->kmigrated = kthread_create_on_node(kmigrated, pgdat, nid,
							  "kmigrated%d", nid);
		if (IS_ERR(pgdat->kmigrated)) {
			ret = PTR_ERR(pgdat->kmigrated);
			pgdat->kmigrated = NULL;
			pr_err("Failed to start kmigrated%d, ret %d\n", nid, ret);
			return ret;
		}
		pr_info("pghot: Started kmigrated thread for node %d\n", nid);
	}
	wake_up_process(pgdat->kmigrated);
	return 0;
}

static void pghot_free_hot_map(struct mem_section *ms)
{
	struct pghot_hot_map *hot_map;

	hot_map = rcu_dereference_protected(ms->hot_map, 1);
	if (!hot_map)
		return;

	RCU_INIT_POINTER(ms->hot_map, NULL);
	kfree_rcu(hot_map, rcu);
}

static int pghot_alloc_hot_map(struct mem_section *ms, int nid)
{
	struct pghot_hot_map *hot_map;

	hot_map = kzalloc_node(struct_size(hot_map, phi, PAGES_PER_SECTION),
			       GFP_KERNEL, nid);
	if (!hot_map)
		return -ENOMEM;

	rcu_assign_pointer(ms->hot_map, hot_map);
	return 0;
}

static void pghot_offline_sec_hotmap(unsigned long start_pfn,
				     unsigned long nr_pages)
{
	unsigned long start, end, pfn;
	struct mem_section *ms;

	start = SECTION_ALIGN_DOWN(start_pfn);
	end = SECTION_ALIGN_UP(start_pfn + nr_pages);

	for (pfn = start; pfn < end; pfn += PAGES_PER_SECTION) {
		ms = __pfn_to_section(pfn);
		if (!ms)
			continue;

		pghot_free_hot_map(ms);
	}
}

static int pghot_online_sec_hotmap(unsigned long start_pfn,
				   unsigned long nr_pages)
{
	int nid = pfn_to_nid(start_pfn);
	unsigned long start, end, pfn;
	struct mem_section *ms;
	int fail = 0;

	/*
	 * No section hot maps for toptier sections.
	 */
	if (node_is_toptier(nid))
		return 0;

	start = SECTION_ALIGN_DOWN(start_pfn);
	end = SECTION_ALIGN_UP(start_pfn + nr_pages);

	for (pfn = start; !fail && pfn < end; pfn += PAGES_PER_SECTION) {
		ms = __pfn_to_section(pfn);
		if (!ms || rcu_access_pointer(ms->hot_map))
			continue;

		fail = pghot_alloc_hot_map(ms, nid);
	}

	if (!fail)
		return 0;

	/* rollback: free the sections allocated before the failure */
	end = pfn - PAGES_PER_SECTION;
	for (pfn = start; pfn < end; pfn += PAGES_PER_SECTION) {
		ms = __pfn_to_section(pfn);
		if (ms)
			pghot_free_hot_map(ms);
	}
	return -ENOMEM;
}

static int pghot_memhp_callback(struct notifier_block *self,
				unsigned long action, void *arg)
{
	struct memory_notify *mn = arg;
	int ret = 0;

	switch (action) {
	case MEM_GOING_ONLINE:
		ret = pghot_online_sec_hotmap(mn->start_pfn, mn->nr_pages);
		break;
	case MEM_OFFLINE:
	case MEM_CANCEL_ONLINE:
		pghot_offline_sec_hotmap(mn->start_pfn, mn->nr_pages);
		break;
	}

	return notifier_from_errno(ret);
}

static struct notifier_block pghot_mem_notifier = {
	.notifier_call = pghot_memhp_callback,
	.priority = DEFAULT_CALLBACK_PRI,
};

static void pghot_destroy_hot_map(void)
{
	unsigned long section_nr, s_begin;
	struct mem_section *ms;

	s_begin = next_present_section_nr(-1);
	for_each_present_section_nr(s_begin, section_nr) {
		ms = __nr_to_section(section_nr);
		pghot_free_hot_map(ms);
	}

	unregister_memory_notifier(&pghot_mem_notifier);
}

static int pghot_setup_hot_map(void)
{
	unsigned long section_nr, s_begin, start_pfn;
	struct mem_section *ms;
	int nid, ret;

	ret = register_memory_notifier(&pghot_mem_notifier);
	if (ret)
		return ret;

	s_begin = next_present_section_nr(-1);
	for_each_present_section_nr(s_begin, section_nr) {
		ms = __nr_to_section(section_nr);
		start_pfn = section_nr_to_pfn(section_nr);
		nid = pfn_to_nid(start_pfn);

		if (node_is_toptier(nid) || !pfn_valid(start_pfn))
			continue;

		if (pghot_alloc_hot_map(ms, nid))
			goto out_free_hot_map;
	}
	return 0;

out_free_hot_map:
	pghot_destroy_hot_map();
	return -ENOMEM;
}

static int pghot_parse_uint(const char __user *ubuf, size_t cnt, unsigned int *val)
{
	char buf[16];

	if (cnt > sizeof(buf) - 1)
		cnt = sizeof(buf) - 1;
	if (copy_from_user(buf, ubuf, cnt))
		return -EFAULT;
	buf[cnt] = '\0';
	if (kstrtouint(buf, 0, val))
		return -EINVAL;
	return 0;
}

static ssize_t pghot_kmigrated_sleep_ms_write(struct file *filp, const char __user *ubuf,
					      size_t cnt, loff_t *ppos)
{
	unsigned int val;
	int ret;

	guard(mutex)(&pghot_tunables_lock);

	ret = pghot_parse_uint(ubuf, cnt, &val);
	if (ret)
		return ret;
	if (val < KMIGRATED_SLEEP_MS_MIN  || val > KMIGRATED_SLEEP_MS_MAX)
		return -EINVAL;

	WRITE_ONCE(kmigrated_sleep_ms, val);
	*ppos += cnt;
	return cnt;
}

static ssize_t pghot_kmigrated_batch_nr_write(struct file *filp, const char __user *ubuf,
					      size_t cnt, loff_t *ppos)
{
	unsigned int val;
	int ret;

	guard(mutex)(&pghot_tunables_lock);

	ret = pghot_parse_uint(ubuf, cnt, &val);
	if (ret)
		return ret;
	if (val < KMIGRATED_BATCH_NR_MIN  || val > KMIGRATED_BATCH_NR_MAX)
		return -EINVAL;

	WRITE_ONCE(kmigrated_batch_nr, val);
	*ppos += cnt;
	return cnt;
}

#define PGHOT_SHOW(name)						\
static int pghot_##name##_show(struct seq_file *m, void *v)		\
{									\
	seq_printf(m, "%u\n", name);				\
	return 0;							\
}									\
static int pghot_##name##_open(struct inode *inode, struct file *filp)	\
{									\
	return single_open(filp, pghot_##name##_show, NULL);		\
}

PGHOT_SHOW(kmigrated_sleep_ms)
PGHOT_SHOW(kmigrated_batch_nr)

#define PGHOT_FOPS(name)						\
static const struct file_operations pghot_##name##_fops = {		\
	.open		= pghot_##name##_open,				\
	.read		= seq_read,					\
	.write		= pghot_##name##_write,				\
	.llseek		= seq_lseek,					\
	.release	= single_release,				\
}

PGHOT_FOPS(kmigrated_sleep_ms);
PGHOT_FOPS(kmigrated_batch_nr);

static void pghot_debug_init(void)
{
	struct dentry *dir = debugfs_create_dir("pghot", NULL);

	debugfs_create_file("kmigrated_sleep_ms", 0644, dir, NULL,
			    &pghot_kmigrated_sleep_ms_fops);
	debugfs_create_file("kmigrated_batch_nr", 0644, dir, NULL,
			    &pghot_kmigrated_batch_nr_fops);
}

static int __init pghot_init(void)
{
	pg_data_t *pgdat;
	int nid, ret;

	ret = pghot_setup_hot_map();
	if (ret)
		return ret;

	for_each_node_state(nid, N_MEMORY) {
		if (node_is_toptier(nid))
			continue;

		ret = kmigrated_run(nid);
		if (ret)
			goto out_stop_kthread;
	}
	pghot_sysctl_init();
	pghot_debug_init();

	kmigrated_started = true;
	return 0;

out_stop_kthread:
	for_each_node_state(nid, N_MEMORY) {
		pgdat = NODE_DATA(nid);
		if (pgdat->kmigrated) {
			kthread_stop(pgdat->kmigrated);
			pgdat->kmigrated = NULL;
		}
	}
	pghot_destroy_hot_map();
	return ret;
}

late_initcall_sync(pghot_init)
