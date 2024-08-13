// SPDX-License-Identifier: GPL-2.0
#include <linux/export.h>
#include <linux/lockdep.h>
#include <linux/jiffies.h>
#include <linux/kernfs.h>
#include <linux/memcontrol.h>
#include <linux/rcupdate.h>
#include <linux/mutex.h>
#include <linux/err.h>
#include <linux/atomic.h>
#include <linux/node.h>
#include <linux/mmzone.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/workingset_report.h>

#include "internal.h"

void wsr_init_pgdat(struct pglist_data *pgdat)
{
	mutex_init(&pgdat->wsr_update_mutex);
	RCU_INIT_POINTER(pgdat->wsr_page_age_bins, NULL);
}

void wsr_destroy_pgdat(struct pglist_data *pgdat)
{
	struct wsr_report_bins __rcu *bins;

	mutex_lock(&pgdat->wsr_update_mutex);
	bins = rcu_replace_pointer(pgdat->wsr_page_age_bins, NULL,
			    lockdep_is_held(&pgdat->wsr_update_mutex));
	kfree_rcu(bins, rcu);
	mutex_unlock(&pgdat->wsr_update_mutex);
	mutex_destroy(&pgdat->wsr_update_mutex);
}

void wsr_init_lruvec(struct lruvec *lruvec)
{
	struct wsr_state *wsr = &lruvec->wsr;

	memset(wsr, 0, sizeof(*wsr));
	mutex_init(&wsr->page_age_lock);
}

void wsr_destroy_lruvec(struct lruvec *lruvec)
{
	struct wsr_state *wsr = &lruvec->wsr;

	mutex_destroy(&wsr->page_age_lock);
	kfree(wsr->page_age);
	memset(wsr, 0, sizeof(*wsr));
}

static int workingset_report_intervals_parse(char *src,
					     struct wsr_report_bins *bins)
{
	int err = 0, i = 0;
	char *cur, *next = strim(src);

	if (*next == '\0')
		return 0;

	while ((cur = strsep(&next, ","))) {
		unsigned int interval;

		err = kstrtouint(cur, 0, &interval);
		if (err)
			goto out;

		bins->idle_age[i] = msecs_to_jiffies(interval);
		if (i > 0 && bins->idle_age[i] <= bins->idle_age[i - 1]) {
			err = -EINVAL;
			goto out;
		}

		if (++i == WORKINGSET_REPORT_MAX_NR_BINS) {
			err = -ERANGE;
			goto out;
		}
	}

	if (i && i < WORKINGSET_REPORT_MIN_NR_BINS - 1) {
		err = -ERANGE;
		goto out;
	}

	bins->nr_bins = i;
	bins->idle_age[i] = WORKINGSET_INTERVAL_MAX;
out:
	return err ?: i;
}

static unsigned long get_gen_start_time(const struct lru_gen_folio *lrugen,
					unsigned long seq,
					unsigned long max_seq,
					unsigned long curr_timestamp)
{
	int younger_gen;

	if (seq == max_seq)
		return curr_timestamp;
	younger_gen = lru_gen_from_seq(seq + 1);
	return READ_ONCE(lrugen->timestamps[younger_gen]);
}

static void collect_page_age_type(const struct lru_gen_folio *lrugen,
				  struct wsr_report_bin *bin,
				  unsigned long max_seq, unsigned long min_seq,
				  unsigned long curr_timestamp, int type)
{
	unsigned long seq;

	for (seq = max_seq; seq + 1 > min_seq; seq--) {
		int gen, zone;
		unsigned long gen_end, gen_start, size = 0;

		gen = lru_gen_from_seq(seq);

		for (zone = 0; zone < MAX_NR_ZONES; zone++)
			size += max(
				READ_ONCE(lrugen->nr_pages[gen][type][zone]),
				0L);

		gen_start = get_gen_start_time(lrugen, seq, max_seq,
					       curr_timestamp);
		gen_end = READ_ONCE(lrugen->timestamps[gen]);

		while (bin->idle_age != WORKINGSET_INTERVAL_MAX &&
		       time_before(gen_end + bin->idle_age, curr_timestamp)) {
			unsigned long gen_in_bin = (long)gen_start -
						   (long)curr_timestamp +
						   (long)bin->idle_age;
			unsigned long gen_len = (long)gen_start - (long)gen_end;

			if (!gen_len)
				break;
			if (gen_in_bin) {
				unsigned long split_bin =
					size / gen_len * gen_in_bin;

				bin->nr_pages[type] += split_bin;
				size -= split_bin;
			}
			gen_start = curr_timestamp - bin->idle_age;
			bin++;
		}
		bin->nr_pages[type] += size;
	}
}

/*
 * proportionally aggregate Multi-gen LRU bins into a working set report
 * MGLRU generations:
 * current time
 * |         max_seq timestamp
 * |         |     max_seq - 1 timestamp
 * |         |     |               unbounded
 * |         |     |               |
 * --------------------------------
 * | max_seq | ... | ... | min_seq
 * --------------------------------
 *
 * Bins:
 *
 * current time
 * |       current - idle_age[0]
 * |       |     current - idle_age[1]
 * |       |     |               unbounded
 * |       |     |               |
 * ------------------------------
 * | bin 0 | ... | ... | bin n-1
 * ------------------------------
 *
 * Assume the heuristic that pages are in the MGLRU generation
 * through uniform accesses, so we can aggregate them
 * proportionally into bins.
 */
static void collect_page_age(struct wsr_page_age_histo *page_age,
			     const struct lruvec *lruvec)
{
	int type;
	const struct lru_gen_folio *lrugen = &lruvec->lrugen;
	unsigned long curr_timestamp = jiffies;
	unsigned long max_seq = READ_ONCE((lruvec)->lrugen.max_seq);
	unsigned long min_seq[ANON_AND_FILE] = {
		READ_ONCE(lruvec->lrugen.min_seq[LRU_GEN_ANON]),
		READ_ONCE(lruvec->lrugen.min_seq[LRU_GEN_FILE]),
	};
	struct wsr_report_bin *bin = &page_age->bins[0];

	for (type = 0; type < ANON_AND_FILE; type++)
		collect_page_age_type(lrugen, bin, max_seq, min_seq[type],
				      curr_timestamp, type);
}

/* First step: hierarchically scan child memcgs. */
static void refresh_scan(struct wsr_state *wsr, struct mem_cgroup *root,
			 struct pglist_data *pgdat,
			 unsigned long refresh_interval)
{
	struct mem_cgroup *memcg;
	unsigned int flags;
	struct reclaim_state rs = { 0 };

	set_task_reclaim_state(current, &rs);
	flags = memalloc_noreclaim_save();

	memcg = mem_cgroup_iter(root, NULL, NULL);
	do {
		struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);
		unsigned long max_seq = READ_ONCE((lruvec)->lrugen.max_seq);
		int gen = lru_gen_from_seq(max_seq);
		unsigned long birth = READ_ONCE(lruvec->lrugen.timestamps[gen]);

		/*
		 * setting can_swap=true and force_scan=true ensures
		 * proper workingset stats when the system cannot swap.
		 */
		if (time_is_before_jiffies(birth + refresh_interval))
			try_to_inc_max_seq(lruvec, max_seq, true, true);
		cond_resched();
	} while ((memcg = mem_cgroup_iter(root, memcg, NULL)));

	memalloc_noreclaim_restore(flags);
	set_task_reclaim_state(current, NULL);
}

/* Second step: aggregate child memcgs into the page age histogram. */
static void refresh_aggregate(struct wsr_page_age_histo *page_age,
			      struct mem_cgroup *root,
			      struct pglist_data *pgdat)
{
	struct mem_cgroup *memcg;
	struct wsr_report_bin *bin;

	for (bin = page_age->bins;
	     bin->idle_age != WORKINGSET_INTERVAL_MAX; bin++) {
		bin->nr_pages[0] = 0;
		bin->nr_pages[1] = 0;
	}
	/* the last used bin has idle_age == WORKINGSET_INTERVAL_MAX. */
	bin->nr_pages[0] = 0;
	bin->nr_pages[1] = 0;

	memcg = mem_cgroup_iter(root, NULL, NULL);
	do {
		struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);

		collect_page_age(page_age, lruvec);
		cond_resched();
	} while ((memcg = mem_cgroup_iter(root, memcg, NULL)));
	WRITE_ONCE(page_age->timestamp, jiffies);
}

static void copy_node_bins(struct pglist_data *pgdat,
			   struct wsr_page_age_histo *page_age)
{
	struct wsr_report_bins *node_page_age_bins;
	int i = 0;

	rcu_read_lock();
	node_page_age_bins = rcu_dereference(pgdat->wsr_page_age_bins);
	if (!node_page_age_bins)
		goto nocopy;
	for (i = 0; i < node_page_age_bins->nr_bins; ++i)
		page_age->bins[i].idle_age = node_page_age_bins->idle_age[i];

nocopy:
	page_age->bins[i].idle_age = WORKINGSET_INTERVAL_MAX;
	rcu_read_unlock();
}

bool wsr_refresh_report(struct wsr_state *wsr, struct mem_cgroup *root,
			struct pglist_data *pgdat)
{
	struct wsr_page_age_histo *page_age;
	unsigned long refresh_interval = READ_ONCE(wsr->refresh_interval);

	if (!READ_ONCE(wsr->page_age))
		return false;

	if (!refresh_interval)
		return false;

	mutex_lock(&wsr->page_age_lock);
	page_age = READ_ONCE(wsr->page_age);
	if (!page_age)
		goto unlock;
	if (page_age->timestamp &&
	    time_is_after_jiffies(page_age->timestamp + refresh_interval))
		goto unlock;
	refresh_scan(wsr, root, pgdat, refresh_interval);
	copy_node_bins(pgdat, page_age);
	refresh_aggregate(page_age, root, pgdat);
unlock:
	mutex_unlock(&wsr->page_age_lock);
	return !!page_age;
}
EXPORT_SYMBOL_GPL(wsr_refresh_report);

static struct pglist_data *kobj_to_pgdat(struct kobject *kobj)
{
	int nid = IS_ENABLED(CONFIG_NUMA) ? kobj_to_dev(kobj)->id :
					    first_memory_node;

	return NODE_DATA(nid);
}

static struct wsr_state *kobj_to_wsr(struct kobject *kobj)
{
	return &mem_cgroup_lruvec(NULL, kobj_to_pgdat(kobj))->wsr;
}

static ssize_t refresh_interval_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	struct wsr_state *wsr = kobj_to_wsr(kobj);
	unsigned int interval = READ_ONCE(wsr->refresh_interval);

	return sysfs_emit(buf, "%u\n", jiffies_to_msecs(interval));
}

static ssize_t refresh_interval_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t len)
{
	unsigned int interval;
	int err;
	struct wsr_state *wsr = kobj_to_wsr(kobj);

	err = kstrtouint(buf, 0, &interval);
	if (err)
		return err;

	mutex_lock(&wsr->page_age_lock);
	if (interval && !wsr->page_age) {
		struct wsr_page_age_histo *page_age =
			kzalloc(sizeof(struct wsr_page_age_histo), GFP_KERNEL);

		if (!page_age) {
			err = -ENOMEM;
			goto unlock;
		}
		wsr->page_age = page_age;
	}
	if (!interval && wsr->page_age) {
		kfree(wsr->page_age);
		wsr->page_age = NULL;
	}

	WRITE_ONCE(wsr->refresh_interval, msecs_to_jiffies(interval));
unlock:
	mutex_unlock(&wsr->page_age_lock);
	return err ?: len;
}

static struct kobj_attribute refresh_interval_attr =
	__ATTR_RW(refresh_interval);

static ssize_t page_age_intervals_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	struct wsr_report_bins *bins;
	int len = 0;
	struct pglist_data *pgdat = kobj_to_pgdat(kobj);

	rcu_read_lock();
	bins = rcu_dereference(pgdat->wsr_page_age_bins);
	if (bins) {
		int i;
		int nr_bins = bins->nr_bins;

		for (i = 0; i < bins->nr_bins; ++i) {
			len += sysfs_emit_at(
				buf, len, "%u",
				jiffies_to_msecs(bins->idle_age[i]));
			if (i + 1 < nr_bins)
				len += sysfs_emit_at(buf, len, ",");
		}
	}
	len += sysfs_emit_at(buf, len, "\n");
	rcu_read_unlock();

	return len;
}

static ssize_t page_age_intervals_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *src, size_t len)
{
	struct wsr_report_bins *bins = NULL, __rcu *old;
	char *buf = NULL;
	int err = 0;
	struct pglist_data *pgdat = kobj_to_pgdat(kobj);

	buf = kstrdup(src, GFP_KERNEL);
	if (!buf) {
		err = -ENOMEM;
		goto failed;
	}

	bins =
		kzalloc(sizeof(struct wsr_report_bins), GFP_KERNEL);

	if (!bins) {
		err = -ENOMEM;
		goto failed;
	}

	err = workingset_report_intervals_parse(buf, bins);
	if (err < 0)
		goto failed;

	if (err == 0) {
		kfree(bins);
		bins = NULL;
	}

	mutex_lock(&pgdat->wsr_update_mutex);
	old = rcu_replace_pointer(pgdat->wsr_page_age_bins, bins,
				  lockdep_is_held(&pgdat->wsr_update_mutex));
	mutex_unlock(&pgdat->wsr_update_mutex);
	kfree_rcu(old, rcu);
	kfree(buf);
	return len;
failed:
	kfree(bins);
	kfree(buf);

	return err;
}

static struct kobj_attribute page_age_intervals_attr =
	__ATTR_RW(page_age_intervals);

static ssize_t page_age_show(struct kobject *kobj, struct kobj_attribute *attr,
			     char *buf)
{
	struct wsr_report_bin *bin;
	int ret = 0;
	struct wsr_state *wsr = kobj_to_wsr(kobj);

	wsr_refresh_report(wsr, NULL, kobj_to_pgdat(kobj));

	mutex_lock(&wsr->page_age_lock);
	if (!wsr->page_age)
		goto unlock;
	for (bin = wsr->page_age->bins;
	     bin->idle_age != WORKINGSET_INTERVAL_MAX; bin++)
		ret += sysfs_emit_at(buf, ret, "%u anon=%lu file=%lu\n",
				     jiffies_to_msecs(bin->idle_age),
				     bin->nr_pages[0] * PAGE_SIZE,
				     bin->nr_pages[1] * PAGE_SIZE);

	ret += sysfs_emit_at(buf, ret, "%lu anon=%lu file=%lu\n",
			     WORKINGSET_INTERVAL_MAX,
			     bin->nr_pages[0] * PAGE_SIZE,
			     bin->nr_pages[1] * PAGE_SIZE);

unlock:
	mutex_unlock(&wsr->page_age_lock);
	return ret;
}

static struct kobj_attribute page_age_attr = __ATTR_RO(page_age);

static struct attribute *workingset_report_attrs[] = {
	&refresh_interval_attr.attr,
	&page_age_intervals_attr.attr,
	&page_age_attr.attr,
	NULL
};

static const struct attribute_group workingset_report_attr_group = {
	.name = "workingset_report",
	.attrs = workingset_report_attrs,
};

void wsr_init_sysfs(struct node *node)
{
	struct kobject *kobj = node ? &node->dev.kobj : mm_kobj;
	struct wsr_state *wsr;

	if (IS_ENABLED(CONFIG_NUMA) && !node)
		return;

	wsr = kobj_to_wsr(kobj);

	if (sysfs_create_group(kobj, &workingset_report_attr_group))
		pr_warn("Workingset report failed to create sysfs files\n");
}
EXPORT_SYMBOL_GPL(wsr_init_sysfs);

void wsr_remove_sysfs(struct node *node)
{
	struct kobject *kobj = &node->dev.kobj;
	struct wsr_state *wsr;

	if (IS_ENABLED(CONFIG_NUMA) && !node)
		return;

	wsr = kobj_to_wsr(kobj);
	sysfs_remove_group(kobj, &workingset_report_attr_group);
}
EXPORT_SYMBOL_GPL(wsr_remove_sysfs);
