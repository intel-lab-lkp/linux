// SPDX-License-Identifier: GPL-2.0
//
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

void wsr_init(struct lruvec *lruvec)
{
	struct wsr_state *wsr = &lruvec->wsr;
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);

	memset(wsr, 0, sizeof(*wsr));
	mutex_init(&wsr->page_age_lock);
	if (memcg && !mem_cgroup_is_root(memcg))
		wsr->page_age_cgroup_file = &memcg->workingset_page_age_file;
}

void wsr_destroy(struct lruvec *lruvec)
{
	struct wsr_state *wsr = &lruvec->wsr;

	mutex_destroy(&wsr->page_age_lock);
	kfree(wsr->page_age);
	kfree_rcu(wsr->reaccess, rcu);
	memset(wsr, 0, sizeof(*wsr));
}

int workingset_report_intervals_parse(char *src,
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

		bins->bins[i].idle_age = msecs_to_jiffies(interval);
		if (i > 0 && bins->bins[i].idle_age <= bins->bins[i - 1].idle_age) {
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
	bins->bins[i].idle_age = WORKINGSET_INTERVAL_MAX;
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
	struct wsr_report_bins *bins = &page_age->bins;

	for (type = 0; type < ANON_AND_FILE; type++) {
		struct wsr_report_bin *bin = &bins->bins[0];

		collect_page_age_type(lrugen, bin, max_seq, min_seq[type],
				      curr_timestamp, type);
	}
}

/* First step: hierarchically scan child memcgs. */
static void refresh_scan(struct wsr_state *wsr, struct mem_cgroup *root,
			 struct pglist_data *pgdat,
			 unsigned long refresh_interval)
{
	struct mem_cgroup *memcg;

	memcg = mem_cgroup_iter(root, NULL, NULL);
	do {
		struct lruvec *lruvec = mem_cgroup_lruvec(memcg, pgdat);

		wsr_refresh_scan(lruvec, refresh_interval);
		cond_resched();
	} while ((memcg = mem_cgroup_iter(root, memcg, NULL)));
}

/* Second step: aggregate child memcgs into the page age histogram. */
static void refresh_aggregate(struct wsr_page_age_histo *page_age,
			      struct mem_cgroup *root,
			      struct pglist_data *pgdat)
{
	struct mem_cgroup *memcg;
	struct wsr_report_bin *bin;

	/*
	 * page_age_intervals should free the page_age struct
	 * if no intervals are provided.
	 */
	VM_WARN_ON_ONCE(page_age->bins.bins[0].idle_age ==
			WORKINGSET_INTERVAL_MAX);

	for (bin = page_age->bins.bins;
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

bool wsr_refresh_report(struct wsr_state *wsr, struct mem_cgroup *root,
			struct pglist_data *pgdat)
{
	struct wsr_page_age_histo *page_age = NULL;
	unsigned long refresh_interval = READ_ONCE(wsr->refresh_interval);

	if (!READ_ONCE(wsr->page_age))
		return false;

	if (!refresh_interval)
		return false;

	mutex_lock(&wsr->page_age_lock);
	page_age = READ_ONCE(wsr->page_age);
	if (!page_age)
		goto unlock;
	if (time_is_after_jiffies(page_age->timestamp + refresh_interval))
		goto unlock;
	refresh_scan(wsr, root, pgdat, refresh_interval);
	refresh_aggregate(page_age, root, pgdat);

unlock:
	mutex_unlock(&wsr->page_age_lock);
	return !!page_age;
}
EXPORT_SYMBOL_GPL(wsr_refresh_report);

static void lru_gen_collect_reaccess_refault(struct wsr_report_bins *bins,
					     unsigned long timestamp, int type,
					     int nr_pages)
{
	unsigned long curr_timestamp = jiffies;
	struct wsr_report_bin *bin = &bins->bins[0];

	while (bin->idle_age != WORKINGSET_INTERVAL_MAX &&
	       time_before(timestamp + bin->idle_age, curr_timestamp))
		bin++;

	bin->nr_pages[type] += nr_pages;
}

static void collect_reaccess_type(struct lru_gen_mm_walk *walk,
				  const struct lru_gen_folio *lrugen,
				  struct wsr_report_bin *bin,
				  unsigned long max_seq, unsigned long min_seq,
				  unsigned long curr_timestamp, int type)
{
	unsigned long seq;

	/* Skip max_seq because a reaccess moves a page from another seq
	 * to max_seq. We use the negative change in page count from
	 * other seqs to track the number of reaccesses.
	 */
	for (seq = max_seq - 1; seq + 1 > min_seq; seq--) {
		int younger_gen, gen, zone;
		unsigned long gen_end, gen_start;
		long delta = 0;

		gen = lru_gen_from_seq(seq);

		for (zone = 0; zone < MAX_NR_ZONES; zone++) {
			long nr_pages = walk->nr_pages[gen][type][zone];

			if (nr_pages < 0)
				delta += -nr_pages;
		}

		gen_end = READ_ONCE(lrugen->timestamps[gen]);
		younger_gen = lru_gen_from_seq(seq + 1);
		gen_start = READ_ONCE(lrugen->timestamps[younger_gen]);

		/* ensure gen_start is within idle_age of bin */
		while (bin->idle_age != WORKINGSET_INTERVAL_MAX &&
		       time_before(gen_start + bin->idle_age, curr_timestamp))
			bin++;

		while (bin->idle_age != WORKINGSET_INTERVAL_MAX &&
		       time_before(gen_end + bin->idle_age, curr_timestamp)) {
			unsigned long proportion = (long)gen_start -
						   (long)curr_timestamp +
						   (long)bin->idle_age;
			unsigned long gen_len = (long)gen_start - (long)gen_end;

			if (!gen_len)
				break;
			if (proportion) {
				unsigned long split_bin =
					delta / gen_len * proportion;
				bin->nr_pages[type] += split_bin;
				delta -= split_bin;
			}
			gen_start = curr_timestamp - bin->idle_age;
			bin++;
		}
		bin->nr_pages[type] += delta;
	}
}

/*
 * Reaccesses are propagated up the memcg hierarchy during scanning/refault.
 * Collect the reaccess information from a multi-gen LRU walk.
 */
static void lru_gen_collect_reaccess(struct wsr_report_bins *bins,
				     struct lru_gen_folio *lrugen,
				     struct lru_gen_mm_walk *walk)
{
	int type;
	unsigned long curr_timestamp = jiffies;
	unsigned long max_seq = READ_ONCE(walk->max_seq);
	unsigned long min_seq[ANON_AND_FILE] = {
		READ_ONCE(lrugen->min_seq[LRU_GEN_ANON]),
		READ_ONCE(lrugen->min_seq[LRU_GEN_FILE]),
	};

	for (type = 0; type < ANON_AND_FILE; type++) {
		struct wsr_report_bin *bin = &bins->bins[0];

		collect_reaccess_type(walk, lrugen, bin, max_seq,
				      min_seq[type], curr_timestamp, type);
	}
}

void lru_gen_report_reaccess(struct lruvec *lruvec, struct lru_gen_mm_walk *walk)
{
	struct lru_gen_folio *lrugen = &lruvec->lrugen;
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);

	for (memcg = lruvec_memcg(lruvec); memcg;
	     memcg = parent_mem_cgroup(memcg)) {
		struct lruvec *memcg_lruvec =
			mem_cgroup_lruvec(memcg, lruvec_pgdat(lruvec));
		struct wsr_state *wsr = &memcg_lruvec->wsr;
		struct wsr_reaccess_histo *reaccess;

		rcu_read_lock();
		reaccess = rcu_dereference(wsr->reaccess);
		if (!reaccess) {
			rcu_read_unlock();
			continue;
		}
		lru_gen_collect_reaccess(&reaccess->bins, lrugen, walk);
		rcu_read_unlock();
	}
}

static inline int evicted_gen_from_seq(unsigned long seq)
{
	return seq % MAX_NR_EVICTED_GENS;
}

void report_lru_gen_eviction(struct lruvec *lruvec, int type, int min_seq)
{
	int seq;
	struct wsr_reaccess_histo *reaccess = NULL;
	struct lru_gen_folio *lrugen = &lruvec->lrugen;
	struct wsr_state *wsr = &lruvec->wsr;

	/*
	 * Since file can go ahead of anon, min_seq[file] >= min_seq[anon]
	 * only record evictions when anon moves forward.
	 */
	if (type != LRU_GEN_ANON)
		return;

	/*
	 * lru_lock is held during eviction, so reaccess accounting
	 * can be serialized.
	 */
	lockdep_assert_held(&lruvec->lru_lock);

	rcu_read_lock();
	reaccess = rcu_dereference(wsr->reaccess);
	if (!reaccess)
		goto unlock;

	for (seq = READ_ONCE(lrugen->min_seq[LRU_GEN_ANON]); seq < min_seq;
	     ++seq) {
		int evicted_gen = evicted_gen_from_seq(seq);
		int gen = lru_gen_from_seq(seq);

		WRITE_ONCE(reaccess->gens[evicted_gen].seq, seq);
		WRITE_ONCE(reaccess->gens[evicted_gen].timestamp,
			   READ_ONCE(lrugen->timestamps[gen]));
	}

unlock:
	rcu_read_unlock();
}

/*
 * May yield an incorrect timestamp if the token collides with
 * a recently evicted generation.
 */
static int timestamp_from_workingset_token(struct lruvec *lruvec,
					   unsigned long token,
					   unsigned long *timestamp)
{
	int type, err = -EEXIST;
	unsigned long seq, evicted_min_seq;
	struct wsr_reaccess_histo *reaccess = NULL;
	struct lru_gen_folio *lrugen = &lruvec->lrugen;
	struct wsr_state *wsr = &lruvec->wsr;
	unsigned long min_seq[ANON_AND_FILE] = {
		READ_ONCE(lrugen->min_seq[LRU_GEN_ANON]),
		READ_ONCE(lrugen->min_seq[LRU_GEN_FILE])
	};

	token >>= LRU_REFS_WIDTH;

	/* recent eviction */
	for (type = 0; type < ANON_AND_FILE; ++type) {
		if (token ==
		    (min_seq[type] & (EVICTION_MASK >> LRU_REFS_WIDTH))) {
			int gen = lru_gen_from_seq(min_seq[type]);

			*timestamp = READ_ONCE(lrugen->timestamps[gen]);
			return 0;
		}
	}

	rcu_read_lock();
	reaccess = rcu_dereference(wsr->reaccess);
	if (!reaccess)
		goto unlock;

	/* look up in evicted gen buffer */
	evicted_min_seq = min_seq[LRU_GEN_ANON] - MAX_NR_EVICTED_GENS;
	if (min_seq[LRU_GEN_ANON] < MAX_NR_EVICTED_GENS)
		evicted_min_seq = 0;
	for (seq = min_seq[LRU_GEN_ANON]; seq > evicted_min_seq; --seq) {
		int gen = evicted_gen_from_seq(seq - 1);

		if (token == (reaccess->gens[gen].seq &
			      (EVICTION_MASK >> LRU_REFS_WIDTH))) {
			*timestamp = reaccess->gens[gen].timestamp;

			goto unlock;
		}
	}

unlock:
	rcu_read_unlock();
	return err;
}

void report_reaccess_refault(struct lruvec *lruvec, unsigned long token,
			     int type, int nr_pages)
{
	unsigned long timestamp;
	int err;
	struct mem_cgroup *memcg = lruvec_memcg(lruvec);

	err = timestamp_from_workingset_token(lruvec, token, &timestamp);
	if (err)
		return;

	for (memcg = lruvec_memcg(lruvec); memcg;
	     memcg = parent_mem_cgroup(memcg)) {
		struct lruvec *memcg_lruvec =
			mem_cgroup_lruvec(memcg, lruvec_pgdat(lruvec));
		struct wsr_state *wsr = &memcg_lruvec->wsr;
		struct wsr_reaccess_histo *reaccess = NULL;

		rcu_read_lock();
		reaccess = rcu_dereference(wsr->reaccess);
		if (!reaccess) {
			rcu_read_unlock();
			continue;
		}
		lru_gen_collect_reaccess_refault(&reaccess->bins, timestamp,
						 type, nr_pages);
		rcu_read_unlock();
	}
}

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

static ssize_t report_threshold_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	struct wsr_state *wsr = kobj_to_wsr(kobj);
	unsigned int threshold = READ_ONCE(wsr->report_threshold);

	return sysfs_emit(buf, "%u\n", jiffies_to_msecs(threshold));
}

static ssize_t report_threshold_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t len)
{
	unsigned int threshold;
	struct wsr_state *wsr = kobj_to_wsr(kobj);

	if (kstrtouint(buf, 0, &threshold))
		return -EINVAL;

	WRITE_ONCE(wsr->report_threshold, msecs_to_jiffies(threshold));

	return len;
}

static struct kobj_attribute report_threshold_attr =
	__ATTR_RW(report_threshold);

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

	WRITE_ONCE(wsr->refresh_interval, msecs_to_jiffies(interval));

	return len;
}

static struct kobj_attribute refresh_interval_attr =
	__ATTR_RW(refresh_interval);

static ssize_t page_age_intervals_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	int len = 0;
	struct wsr_state *wsr = kobj_to_wsr(kobj);

	mutex_lock(&wsr->page_age_lock);

	if (wsr->page_age) {
		int i;
		int nr_bins = wsr->page_age->bins.nr_bins;

		for (i = 0; i < nr_bins; ++i) {
			struct wsr_report_bin *bin =
				&wsr->page_age->bins.bins[i];

			len += sysfs_emit_at(buf, len, "%u",
					     jiffies_to_msecs(bin->idle_age));
			if (i + 1 < nr_bins)
				len += sysfs_emit_at(buf, len, ",");
		}
	}
	len += sysfs_emit_at(buf, len, "\n");

	mutex_unlock(&wsr->page_age_lock);
	return len;
}

static ssize_t page_age_intervals_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *src, size_t len)
{
	struct wsr_page_age_histo *page_age = NULL, *old;
	char *buf = NULL;
	int err = 0;
	struct wsr_state *wsr = kobj_to_wsr(kobj);

	buf = kstrdup(src, GFP_KERNEL);
	if (!buf) {
		err = -ENOMEM;
		goto failed;
	}

	page_age =
		kzalloc(sizeof(struct wsr_page_age_histo), GFP_KERNEL_ACCOUNT);

	if (!page_age) {
		err = -ENOMEM;
		goto failed;
	}

	err = workingset_report_intervals_parse(buf, &page_age->bins);
	if (err < 0)
		goto failed;

	if (err == 0) {
		kfree(page_age);
		page_age = NULL;
	}

	mutex_lock(&wsr->page_age_lock);
	old = xchg(&wsr->page_age, page_age);
	mutex_unlock(&wsr->page_age_lock);
	kfree(old);
	kfree(buf);
	return len;
failed:
	kfree(page_age);
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

	if (!READ_ONCE(wsr->page_age))
		return -EINVAL;

	wsr_refresh_report(wsr, NULL, kobj_to_pgdat(kobj));

	mutex_lock(&wsr->page_age_lock);
	if (!wsr->page_age) {
		ret = -EINVAL;
		goto unlock;
	}

	for (bin = wsr->page_age->bins.bins;
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
	&report_threshold_attr.attr,
	&refresh_interval_attr.attr,
	&page_age_intervals_attr.attr,
	&page_age_attr.attr,
	NULL
};

static const struct attribute_group workingset_report_attr_group = {
	.name = "workingset_report",
	.attrs = workingset_report_attrs,
};

void wsr_register_node(struct node *node)
{
	struct kobject *kobj = node ? &node->dev.kobj : mm_kobj;
	struct wsr_state *wsr;

	if (IS_ENABLED(CONFIG_NUMA) && !node)
		return;

	wsr = kobj_to_wsr(kobj);

	if (sysfs_create_group(kobj, &workingset_report_attr_group)) {
		pr_warn("WSR failed to created group");
		return;
	}

	wsr->page_age_sys_file =
		kernfs_walk_and_get(kobj->sd, "workingset_report/page_age");
}
EXPORT_SYMBOL_GPL(wsr_register_node);

void wsr_unregister_node(struct node *node)
{
	struct kobject *kobj = &node->dev.kobj;
	struct wsr_state *wsr;

	if (IS_ENABLED(CONFIG_NUMA) && !node)
		return;

	wsr = kobj_to_wsr(kobj);
	sysfs_remove_group(kobj, &workingset_report_attr_group);
	kernfs_put(wsr->page_age_sys_file);
	wsr_destroy(mem_cgroup_lruvec(NULL, kobj_to_pgdat(kobj)));
}
EXPORT_SYMBOL_GPL(wsr_unregister_node);

void notify_workingset(struct mem_cgroup *memcg, struct pglist_data *pgdat)
{
	struct wsr_state *wsr = &mem_cgroup_lruvec(memcg, pgdat)->wsr;

	if (mem_cgroup_is_root(memcg))
		kernfs_notify(wsr->page_age_sys_file);
	else
		cgroup_file_notify(wsr->page_age_cgroup_file);
}
