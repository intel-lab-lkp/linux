// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2023 Linaro Limited
 *
 * Author: Daniel Lezcano <daniel.lezcano@linaro.org>
 *
 * Debug filesystem for the thermal framework
 */
#include <linux/debugfs.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/thermal.h>

static struct dentry *d_root;
static struct dentry *d_cdev;

/*
 * Length of the string containing the thermal zone id, including the
 * ending null character. We can reasonably assume there won't be more
 * than 256 thermal zones as the maximum observed today is around 32.
 */
#define IDSLENGTH 4

/*
 * The cooling device transition list is stored in a hash table where
 * the size is CDEVSTATS_HASH_SIZE. The majority of cooling devices
 * have dozen of states but some can have much more, so a hash table
 * is more adequate in this case because browsing the entire list when
 * storing the transitions could have a non neglictible cost
 */
#define CDEVSTATS_HASH_SIZE 16

struct cdev_value {
	struct list_head list;
	int id;
	u64 value;
};

/*
 * A cooling device can have a high number of states. Showing the
 * transitions on a matrix based representation can be overkill given
 * most of the transitions won't happen and we end up with a matrix
 * filled with zero. Instead, we show the transitions which actually
 * happened.
 */
struct cdev_debugfs {
	u32 total;
	int current_state;
	ktime_t timestamp;
	struct list_head trans_list[CDEVSTATS_HASH_SIZE];
	struct list_head duration_list[CDEVSTATS_HASH_SIZE];
};

/*
 * The thermal_debugfs structure is the common structure used by the
 * cooling device to compute the statistics and the thermal to measure
 * the temperature at mitigation time.
 */
struct thermal_debugfs {
	struct dentry *d_top;
	struct mutex lock;
	union {
		struct cdev_debugfs cdev;
	};
};

void thermal_debug_init(void)
{
	d_root = debugfs_create_dir("thermal", NULL);
	if (!d_root)
		return;

	d_cdev = debugfs_create_dir("cooling_devices", d_root);
}

static struct thermal_debugfs *thermal_debugfs_add_id(struct dentry *d, int id)
{
	struct thermal_debugfs *dfs;
	char ids[IDSLENGTH];

	dfs = kzalloc(sizeof(*dfs), GFP_KERNEL);
	if (!dfs)
		return NULL;

	mutex_init(&dfs->lock);
	
	snprintf(ids, IDSLENGTH, "%d", id);

	dfs->d_top = debugfs_create_dir(ids, d);
	if (!dfs->d_top) {
		kfree(dfs);
		return NULL;
	}

	return dfs;
}

static void thermal_debugfs_remove_id(struct thermal_debugfs *dfs)
{
	if (!dfs)
		return;

	debugfs_remove(dfs->d_top);

	kfree(dfs);
}

static struct cdev_value *thermal_debugfs_cdev_value_alloc(int id)
{
	struct cdev_value *cdev_value;

	cdev_value = kzalloc(sizeof(*cdev_value), GFP_KERNEL);
	if (cdev_value) {
		cdev_value->id = id;
		INIT_LIST_HEAD(&cdev_value->list);
	}

	return cdev_value;
}

static struct cdev_value *thermal_debugfs_cdev_value_find(struct thermal_debugfs *dfs,
							  struct list_head *list, int id)
{
	struct cdev_value *pos;

	list_for_each_entry(pos, &list[id % CDEVSTATS_HASH_SIZE], list)
		if (pos->id == id)
			return pos;

	return NULL;
}

static void thermal_debugfs_cdev_value_insert(struct thermal_debugfs *dfs,
					      struct list_head *list,
					      struct cdev_value *cdev_value)
{
	list_add_tail(&cdev_value->list, &list[cdev_value->id % CDEVSTATS_HASH_SIZE]);
}

struct cdev_value *thermal_debugfs_cdev_value_get(struct thermal_debugfs *dfs,
						  struct list_head *list, int id)
{
	struct cdev_value *cdev_value;

	cdev_value = thermal_debugfs_cdev_value_find(dfs, list, id);
	if (cdev_value)
		return cdev_value;

	cdev_value = thermal_debugfs_cdev_value_alloc(id);
	if (cdev_value)
		thermal_debugfs_cdev_value_insert(dfs, list, cdev_value);

	return cdev_value;
}

static void thermal_debugfs_cdev_reset(struct cdev_debugfs *cfs)
{
	int i;
	struct cdev_value *pos, *tmp;

	for (i = 0; i < CDEVSTATS_HASH_SIZE; i++) {

		list_for_each_entry_safe(pos, tmp, &cfs->trans_list[i], list) {
			list_del(&pos->list);
			kfree(pos);
		}

		list_for_each_entry_safe(pos, tmp, &cfs->duration_list[i], list) {
			list_del(&pos->list);
			kfree(pos);
		}
	}

	cfs->total = 0;
}

void thermal_debug_cdev_transition(struct thermal_cooling_device *cdev, int to)
{
	struct thermal_debugfs *dfs = cdev->debugfs;
	struct cdev_debugfs *cfs;
	struct cdev_value *cdev_value;
	ktime_t now = ktime_get();
	int transition, from;

	if (!dfs || (dfs->cdev.current_state == to))
		return;

	mutex_lock(&dfs->lock);

	cfs = &dfs->cdev;

	from = cfs->current_state;
	cfs->current_state = to;
	transition = (from << 16) | to;

	cdev_value = thermal_debugfs_cdev_value_get(dfs, cfs->duration_list, from);
	if (cdev_value) {
		cdev_value->value += ktime_ms_delta(now, cfs->timestamp);
		cfs->timestamp = now;
	}

	cdev_value = thermal_debugfs_cdev_value_get(dfs, cfs->trans_list, transition);
	if (cdev_value)
		cdev_value->value++;

	cfs->total++;

	mutex_unlock(&dfs->lock);
}

static void *cdev_seq_start(struct seq_file *s, loff_t *pos)
{
	struct thermal_debugfs *dfs = s->private;

	mutex_lock(&dfs->lock);

	return (*pos < CDEVSTATS_HASH_SIZE) ? pos : NULL;
}

static void *cdev_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	(*pos)++;

	return (*pos < CDEVSTATS_HASH_SIZE) ? pos : NULL;
}

static void cdev_seq_stop(struct seq_file *s, void *v)
{
	struct thermal_debugfs *dfs = s->private;

	mutex_unlock(&dfs->lock);
}

static int cdev_tt_seq_show(struct seq_file *s, void *v)
{
	struct thermal_debugfs *dfs = s->private;	
	struct cdev_debugfs *cfs = &dfs->cdev;
	struct list_head *trans_list = cfs->trans_list;
	struct cdev_value *pos;
	char buffer[11];
	int i = *(loff_t *)v;

	if (!i)
		seq_puts(s, "Transition\tHits\n");

	list_for_each_entry(pos, &trans_list[i], list) {

		snprintf(buffer, ARRAY_SIZE(buffer), "%d->%d",
			 pos->id >> 16, pos->id & 0xFFFF);

		seq_printf(s, "%-10s\t%-10llu\n", buffer, pos->value);
	}

	return 0;
}

static const struct seq_operations tt_sops = {
	.start = cdev_seq_start,
	.next = cdev_seq_next,
	.stop = cdev_seq_stop,
	.show = cdev_tt_seq_show,
};

DEFINE_SEQ_ATTRIBUTE(tt);

static int cdev_dt_seq_show(struct seq_file *s, void *v)
{
	struct cdev_debugfs *cfs = s->private;
	struct list_head *duration_list = cfs->duration_list;
	struct cdev_value *pos;
	int i = *(loff_t *)v;

	if (!i)
		seq_puts(s, "State\tTime\n");

	list_for_each_entry(pos, &duration_list[i], list) {
		s64 duration = pos->value;

		if (pos->id == cfs->current_state)
			duration += ktime_ms_delta(ktime_get(), cfs->timestamp);

		seq_printf(s, "%-5d\t%-10llu\n", pos->id, duration);
	}

	return 0;
}

static const struct seq_operations dt_sops = {
	.start = cdev_seq_start,
	.next = cdev_seq_next,
	.stop = cdev_seq_stop,
	.show = cdev_dt_seq_show,
};

DEFINE_SEQ_ATTRIBUTE(dt);

static int cdev_reset_set(void *data, u64 val)
{
	struct thermal_debugfs *dfs = data;

	if (!val)
		return -EINVAL;

	thermal_debugfs_cdev_reset(&dfs->cdev);

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(cdev_reset_fops, NULL, cdev_reset_set, "%llu\n");

void thermal_debug_cdev_add(struct thermal_cooling_device *cdev)
{
	struct thermal_debugfs *dfs;
	struct cdev_debugfs *cfs;
	int i;

	dfs = thermal_debugfs_add_id(d_cdev, cdev->id);
	if (!dfs)
		return;

	cfs = &dfs->cdev;

	for (i = 0; i < CDEVSTATS_HASH_SIZE; i++) {
		INIT_LIST_HEAD(&cfs->trans_list[i]);
		INIT_LIST_HEAD(&cfs->duration_list[i]);
	}

	cfs->current_state = 0;
	cfs->timestamp = ktime_get();

	debugfs_create_file("trans_table", 0400, dfs->d_top, dfs, &tt_fops);

	debugfs_create_file("time_in_state_ms", 0400, dfs->d_top, dfs, &dt_fops);

	debugfs_create_file("reset", 0200, dfs->d_top, dfs, &cdev_reset_fops);

	debugfs_create_u32("total_trans", 0400, dfs->d_top, &cfs->total);

	cdev->debugfs = dfs;
}

void thermal_debug_cdev_remove(struct thermal_cooling_device *cdev)
{
	struct thermal_debugfs *dfs = cdev->debugfs;

	if (!dfs)
		return;

	thermal_debugfs_cdev_reset(&dfs->cdev);
	cdev->debugfs = NULL;
	thermal_debugfs_remove_id(dfs);
}
