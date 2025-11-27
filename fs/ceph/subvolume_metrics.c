// SPDX-License-Identifier: GPL-2.0
#include <linux/ceph/ceph_debug.h>

#include <linux/math64.h>
#include <linux/slab.h>
#include <linux/seq_file.h>

#include "subvolume_metrics.h"
#include "mds_client.h"
#include "super.h"

struct ceph_subvol_metric_rb_entry {
	struct rb_node node;
	u64 subvolume_id;
	u64 read_ops;
	u64 write_ops;
	u64 read_bytes;
	u64 write_bytes;
	u64 read_latency_us;
	u64 write_latency_us;
};

void ceph_subvolume_metrics_init(struct ceph_subvolume_metrics_tracker *tracker)
{
	spin_lock_init(&tracker->lock);
	tracker->tree = RB_ROOT_CACHED;
	tracker->nr_entries = 0;
	tracker->enabled = false;
	atomic64_set(&tracker->snapshot_attempts, 0);
	atomic64_set(&tracker->snapshot_empty, 0);
	atomic64_set(&tracker->snapshot_failures, 0);
	atomic64_set(&tracker->record_calls, 0);
	atomic64_set(&tracker->record_disabled, 0);
	atomic64_set(&tracker->record_no_subvol, 0);
	atomic64_set(&tracker->total_read_ops, 0);
	atomic64_set(&tracker->total_read_bytes, 0);
	atomic64_set(&tracker->total_write_ops, 0);
	atomic64_set(&tracker->total_write_bytes, 0);
}

static struct ceph_subvol_metric_rb_entry *
__lookup_entry(struct ceph_subvolume_metrics_tracker *tracker, u64 subvol_id)
{
	struct rb_node *node;

	node = tracker->tree.rb_root.rb_node;
	while (node) {
		struct ceph_subvol_metric_rb_entry *entry =
			rb_entry(node, struct ceph_subvol_metric_rb_entry, node);

		if (subvol_id < entry->subvolume_id)
			node = node->rb_left;
		else if (subvol_id > entry->subvolume_id)
			node = node->rb_right;
		else
			return entry;
	}

	return NULL;
}

static struct ceph_subvol_metric_rb_entry *
__insert_entry(struct ceph_subvolume_metrics_tracker *tracker,
	       struct ceph_subvol_metric_rb_entry *entry)
{
	struct rb_node **link = &tracker->tree.rb_root.rb_node;
	struct rb_node *parent = NULL;
	bool leftmost = true;

	while (*link) {
		struct ceph_subvol_metric_rb_entry *cur =
			rb_entry(*link, struct ceph_subvol_metric_rb_entry, node);

		parent = *link;
		if (entry->subvolume_id < cur->subvolume_id)
			link = &(*link)->rb_left;
		else if (entry->subvolume_id > cur->subvolume_id) {
			link = &(*link)->rb_right;
			leftmost = false;
		} else
			return cur;
	}

	rb_link_node(&entry->node, parent, link);
	rb_insert_color_cached(&entry->node, &tracker->tree, leftmost);
	tracker->nr_entries++;
	return entry;
}

static void ceph_subvolume_metrics_clear_locked(
		struct ceph_subvolume_metrics_tracker *tracker)
{
	struct rb_node *node = rb_first_cached(&tracker->tree);

	while (node) {
		struct ceph_subvol_metric_rb_entry *entry =
			rb_entry(node, struct ceph_subvol_metric_rb_entry, node);
		struct rb_node *next = rb_next(node);

		rb_erase_cached(&entry->node, &tracker->tree);
		tracker->nr_entries--;
		kfree(entry);
		node = next;
	}

	tracker->tree = RB_ROOT_CACHED;
}

void ceph_subvolume_metrics_destroy(struct ceph_subvolume_metrics_tracker *tracker)
{
	spin_lock(&tracker->lock);
	ceph_subvolume_metrics_clear_locked(tracker);
	tracker->enabled = false;
	spin_unlock(&tracker->lock);
}

void ceph_subvolume_metrics_enable(struct ceph_subvolume_metrics_tracker *tracker,
				   bool enable)
{
	spin_lock(&tracker->lock);
	if (enable) {
		tracker->enabled = true;
	} else {
		tracker->enabled = false;
		ceph_subvolume_metrics_clear_locked(tracker);
	}
	spin_unlock(&tracker->lock);
}

void ceph_subvolume_metrics_record(struct ceph_subvolume_metrics_tracker *tracker,
				   u64 subvol_id, bool is_write,
				   size_t size, u64 latency_us)
{
	struct ceph_subvol_metric_rb_entry *entry, *new_entry = NULL;
	bool retry = false;

	if (!READ_ONCE(tracker->enabled) || !subvol_id || !size || !latency_us)
		return;

	do {
		spin_lock(&tracker->lock);
		if (!tracker->enabled) {
			spin_unlock(&tracker->lock);
			kfree(new_entry);
			return;
		}

		entry = __lookup_entry(tracker, subvol_id);
		if (!entry) {
			if (!new_entry) {
				spin_unlock(&tracker->lock);
				new_entry = kzalloc(sizeof(*new_entry), GFP_NOFS);
				if (!new_entry)
					return;
				new_entry->subvolume_id = subvol_id;
				retry = true;
				continue;
			}
			entry = __insert_entry(tracker, new_entry);
			if (entry != new_entry) {
				/* raced with another insert */
				spin_unlock(&tracker->lock);
				kfree(new_entry);
				new_entry = NULL;
				retry = true;
				continue;
			}
			new_entry = NULL;
		}

		if (is_write) {
			entry->write_ops++;
			entry->write_bytes += size;
			entry->write_latency_us += latency_us;
			atomic64_inc(&tracker->total_write_ops);
			atomic64_add(size, &tracker->total_write_bytes);
		} else {
			entry->read_ops++;
			entry->read_bytes += size;
			entry->read_latency_us += latency_us;
			atomic64_inc(&tracker->total_read_ops);
			atomic64_add(size, &tracker->total_read_bytes);
		}
		spin_unlock(&tracker->lock);
		kfree(new_entry);
		return;
	} while (retry);
}

int ceph_subvolume_metrics_snapshot(struct ceph_subvolume_metrics_tracker *tracker,
				    struct ceph_subvol_metric_snapshot **out,
				    u32 *nr, bool consume)
{
	struct ceph_subvol_metric_snapshot *snap = NULL;
	struct rb_node *node;
	u32 count = 0, idx = 0;
	int ret = 0;

	*out = NULL;
	*nr = 0;

	if (!READ_ONCE(tracker->enabled))
		return 0;

	atomic64_inc(&tracker->snapshot_attempts);

	spin_lock(&tracker->lock);
	for (node = rb_first_cached(&tracker->tree); node; node = rb_next(node)) {
		struct ceph_subvol_metric_rb_entry *entry =
			rb_entry(node, struct ceph_subvol_metric_rb_entry, node);

		/* Include entries with ANY I/O activity (read OR write) */
		if (entry->read_ops || entry->write_ops)
			count++;
	}
	spin_unlock(&tracker->lock);

	if (!count) {
		atomic64_inc(&tracker->snapshot_empty);
		return 0;
	}

	snap = kcalloc(count, sizeof(*snap), GFP_NOFS);
	if (!snap) {
		atomic64_inc(&tracker->snapshot_failures);
		return -ENOMEM;
	}

	spin_lock(&tracker->lock);
	node = rb_first_cached(&tracker->tree);
	while (node) {
		struct ceph_subvol_metric_rb_entry *entry =
			rb_entry(node, struct ceph_subvol_metric_rb_entry, node);
		struct rb_node *next = rb_next(node);

		/* Skip entries with NO I/O activity at all */
		if (!entry->read_ops && !entry->write_ops) {
			rb_erase_cached(&entry->node, &tracker->tree);
			tracker->nr_entries--;
			kfree(entry);
			node = next;
			continue;
		}

		if (idx >= count) {
			pr_warn("ceph: subvol metrics snapshot race (idx=%u count=%u)\n",
				idx, count);
			break;
		}

		snap[idx].subvolume_id = entry->subvolume_id;
		snap[idx].read_ops = entry->read_ops;
		snap[idx].write_ops = entry->write_ops;
		snap[idx].read_bytes = entry->read_bytes;
		snap[idx].write_bytes = entry->write_bytes;
		snap[idx].read_latency_us = entry->read_latency_us;
		snap[idx].write_latency_us = entry->write_latency_us;
		idx++;

		if (consume) {
			entry->read_ops = 0;
			entry->write_ops = 0;
			entry->read_bytes = 0;
			entry->write_bytes = 0;
			entry->read_latency_us = 0;
			entry->write_latency_us = 0;
			rb_erase_cached(&entry->node, &tracker->tree);
			tracker->nr_entries--;
			kfree(entry);
		}
		node = next;
	}
	spin_unlock(&tracker->lock);

	if (!idx) {
		kfree(snap);
		snap = NULL;
		ret = 0;
	} else {
		*nr = idx;
		*out = snap;
	}

	return ret;
}

void ceph_subvolume_metrics_free_snapshot(struct ceph_subvol_metric_snapshot *snapshot)
{
	kfree(snapshot);
}

static u64 div_rem(u64 dividend, u64 divisor)
{
	return divisor ? div64_u64(dividend, divisor) : 0;
}

void ceph_subvolume_metrics_dump(struct ceph_subvolume_metrics_tracker *tracker,
				 struct seq_file *s)
{
	struct rb_node *node;
	struct ceph_subvol_metric_snapshot *snapshot = NULL;
	u32 count = 0, idx = 0;

	spin_lock(&tracker->lock);
	if (!tracker->enabled) {
		spin_unlock(&tracker->lock);
		seq_puts(s, "subvolume metrics disabled\n");
		return;
	}

	for (node = rb_first_cached(&tracker->tree); node; node = rb_next(node)) {
		struct ceph_subvol_metric_rb_entry *entry =
			rb_entry(node, struct ceph_subvol_metric_rb_entry, node);

		if (entry->read_ops || entry->write_ops)
			count++;
	}
	spin_unlock(&tracker->lock);

	if (!count) {
		seq_puts(s, "(no subvolume metrics collected)\n");
		return;
	}

	snapshot = kcalloc(count, sizeof(*snapshot), GFP_KERNEL);
	if (!snapshot) {
		seq_puts(s, "(unable to allocate memory for snapshot)\n");
		return;
	}

	spin_lock(&tracker->lock);
	for (node = rb_first_cached(&tracker->tree); node; node = rb_next(node)) {
		struct ceph_subvol_metric_rb_entry *entry =
			rb_entry(node, struct ceph_subvol_metric_rb_entry, node);

		if (!entry->read_ops && !entry->write_ops)
			continue;

		if (idx >= count)
			break;

		snapshot[idx].subvolume_id = entry->subvolume_id;
		snapshot[idx].read_ops = entry->read_ops;
		snapshot[idx].write_ops = entry->write_ops;
		snapshot[idx].read_bytes = entry->read_bytes;
		snapshot[idx].write_bytes = entry->write_bytes;
		snapshot[idx].read_latency_us = entry->read_latency_us;
		snapshot[idx].write_latency_us = entry->write_latency_us;
		idx++;
	}
	spin_unlock(&tracker->lock);

	seq_puts(s, "subvol_id       rd_ops    rd_bytes    rd_avg_lat_us  wr_ops    wr_bytes    wr_avg_lat_us\n");
	seq_puts(s, "------------------------------------------------------------------------------------------------\n");

	for (idx = 0; idx < count; idx++) {
		u64 avg_rd_lat = div_rem(snapshot[idx].read_latency_us,
					 snapshot[idx].read_ops);
		u64 avg_wr_lat = div_rem(snapshot[idx].write_latency_us,
					 snapshot[idx].write_ops);

		seq_printf(s, "%-15llu%-10llu%-12llu%-16llu%-10llu%-12llu%-16llu\n",
			   snapshot[idx].subvolume_id,
			   snapshot[idx].read_ops,
			   snapshot[idx].read_bytes,
			   avg_rd_lat,
			   snapshot[idx].write_ops,
			   snapshot[idx].write_bytes,
			   avg_wr_lat);
	}

	kfree(snapshot);
}

void ceph_subvolume_metrics_record_io(struct ceph_mds_client *mdsc,
				      struct ceph_inode_info *ci,
				      bool is_write, size_t bytes,
				      ktime_t start, ktime_t end)
{
	struct ceph_subvolume_metrics_tracker *tracker;
	u64 subvol_id;
	s64 delta_us;

	if (!mdsc || !ci || !bytes)
		return;

	tracker = &mdsc->subvol_metrics;
	atomic64_inc(&tracker->record_calls);

	if (!ceph_subvolume_metrics_enabled(tracker)) {
		atomic64_inc(&tracker->record_disabled);
		return;
	}

	subvol_id = READ_ONCE(ci->i_subvolume_id);
	if (!subvol_id) {
		atomic64_inc(&tracker->record_no_subvol);
		return;
	}

	delta_us = ktime_to_us(ktime_sub(end, start));
	if (delta_us <= 0)
		delta_us = 1;

	ceph_subvolume_metrics_record(tracker, subvol_id, is_write,
				      bytes, (u64)delta_us);
}
