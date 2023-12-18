// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2023 Linaro Limited
 *
 * Author: Daniel Lezcano <daniel.lezcano@linaro.org>
 *
 * Thermal subsystem debug support
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
 * Length of the string containing the thermal zone id or the cooling
 * device id, including the ending nul character. We can reasonably
 * assume there won't be more than 256 thermal zones as the maximum
 * observed today is around 32.
 */
#define IDSLENGTH 4

/*
 * The cooling device transition list is stored in a hash table where
 * the size is CDEVSTATS_HASH_SIZE. The majority of cooling devices
 * have dozen of states but some can have much more, so a hash table
 * is more adequate in this case, because the cost of browsing the entire
 * list when storing the transitions may not be negligible.
 */
#define CDEVSTATS_HASH_SIZE 16

/**
 * struct cdev_debugfs - per cooling device statistics structure
 * A cooling device can have a high number of states. Showing the
 * transitions on a matrix based representation can be overkill given
 * most of the transitions won't happen and we end up with a matrix
 * filled with zero. Instead, we show the transitions which actually
 * happened.
 *
 * Every transition updates the current_state and the timestamp. The
 * transitions and the durations are stored in lists.
 *
 * @total: the number of transitions for this cooling device
 * @current_state: the current cooling device state
 * @timestamp: the state change timestamp
 * @durations: an array of lists containing the residencies of each state
 * @transitions: an array of lists containing the state transitions
 */
struct cdev_debugfs {
	u32 total;
	int current_state;
	ktime_t timestamp;
	struct list_head transitions[CDEVSTATS_HASH_SIZE];
	struct list_head durations[CDEVSTATS_HASH_SIZE];
};

/**
 * cdev_value - Common structure for cooling device entry
 *
 * The following common structure allows to store the information
 * related to the transitions and to the state residencies. They are
 * identified with a id which is associated to a value. It is used as
 * nodes for the "transitions" and "durations" above.
 *
 * @node: node to insert the structure in a list
 * @id: identifier of the value which can be a state or a transition
 * @value: the id associated value which can be a duration or an occurrence
 */
struct cdev_value {
	struct list_head node;
	int id;
	u64 value;
};

/**
 * thermal_debugfs - High level structure for a thermal object in
 * debugfs
 *
 * The thermal_debugfs structure is the common structure used by the
 * cooling device to compute the statistics.
 *
 * @d_top: top directory of the thermal object directory
 * @lock: per object lock to protect the internals
 *
 * @cdev: a cooling device debug structure
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

static struct cdev_value *thermal_debugfs_cdev_value_alloc(struct thermal_debugfs *dfs,
							   struct list_head *list, int id)
{
	struct cdev_value *cdev_value;

	cdev_value = kzalloc(sizeof(*cdev_value), GFP_KERNEL);
	if (cdev_value) {
		cdev_value->id = id;
		INIT_LIST_HEAD(&cdev_value->node);
		list_add_tail(&cdev_value->node, &list[cdev_value->id % CDEVSTATS_HASH_SIZE]);
	}

	return cdev_value;
}

static struct cdev_value *thermal_debugfs_cdev_value_find(struct thermal_debugfs *dfs,
							  struct list_head *lists, int id)
{
	struct cdev_value *entry;

	list_for_each_entry(entry, &lists[id % CDEVSTATS_HASH_SIZE], node)
		if (entry->id == id)
			return entry;

	return NULL;
}

struct cdev_value *thermal_debugfs_cdev_value_get(struct thermal_debugfs *dfs,
						  struct list_head *list, int id)
{
	struct cdev_value *cdev_value;

	cdev_value = thermal_debugfs_cdev_value_find(dfs, list, id);
	if (cdev_value)
		return cdev_value;

	return thermal_debugfs_cdev_value_alloc(dfs, list, id);
}

static void thermal_debugfs_cdev_clear(struct cdev_debugfs *cfs)
{
	int i;
	struct cdev_value *entry, *tmp;

	for (i = 0; i < CDEVSTATS_HASH_SIZE; i++) {

		list_for_each_entry_safe(entry, tmp, &cfs->transitions[i], node) {
			list_del(&entry->node);
			kfree(entry);
		}

		list_for_each_entry_safe(entry, tmp, &cfs->durations[i], node) {
			list_del(&entry->node);
			kfree(entry);
		}
	}

	cfs->total = 0;
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
	struct list_head *transitions = cfs->transitions;
	struct cdev_value *entry;
	int i = *(loff_t *)v;

	if (!i)
		seq_puts(s, "Transition\tOccurences\n");

	list_for_each_entry(entry, &transitions[i], node) {
		/*
		 * Assuming maximum cdev states is 1024, the longer
		 * string for a transition would be "1024->1024\0"
		 */
		char buffer[11];
		
		snprintf(buffer, ARRAY_SIZE(buffer), "%d->%d",
			 entry->id >> 16, entry->id & 0xFFFF);

		seq_printf(s, "%-10s\t%-10llu\n", buffer, entry->value);
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
	struct thermal_debugfs *dfs = s->private;
	struct cdev_debugfs *cfs = &dfs->cdev;
	struct list_head *durations = cfs->durations;
	struct cdev_value *entry;
	int i = *(loff_t *)v;

	if (!i)
		seq_puts(s, "State\tResidency\n");

	list_for_each_entry(entry, &durations[i], node) {
		s64 duration = entry->value;

		if (entry->id == cfs->current_state)
			duration += ktime_ms_delta(ktime_get(), cfs->timestamp);

		seq_printf(s, "%-5d\t%-10llu\n", entry->id, duration);
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

static int cdev_clear_set(void *data, u64 val)
{
	struct thermal_debugfs *dfs = data;

	if (!val)
		return -EINVAL;

	mutex_lock(&dfs->lock);
	
	thermal_debugfs_cdev_clear(&dfs->cdev);

	mutex_unlock(&dfs->lock);

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(cdev_clear_fops, NULL, cdev_clear_set, "%llu\n");

/**
 * thermal_debug_cdev_state_update - Update a cooling device state change
 *
 * Computes a transition and the duration of the previous state residency.
 *
 * @cdev : a pointer to a cooling device
 * @new_state: an integer corresponding to the new cooling device state
 */
void thermal_debug_cdev_state_update(const struct thermal_cooling_device *cdev,
				     int new_state)
{
	struct thermal_debugfs *dfs = cdev->debugfs;
	struct cdev_debugfs *cfs;
	struct cdev_value *cdev_value;
	ktime_t now = ktime_get();
	int transition, old_state;

	if (!dfs || (dfs->cdev.current_state == new_state))
		return;

	mutex_lock(&dfs->lock);

	cfs = &dfs->cdev;

	old_state = cfs->current_state;
	cfs->current_state = new_state;
	transition = (old_state << 16) | new_state;

	/*
	 * Get the old state information in the durations list. If
	 * this one does not exist, a new allocated one will be
	 * returned. Recompute the total duration in the old state and
	 * get a new timestamp for the new state.
	 */
	cdev_value = thermal_debugfs_cdev_value_get(dfs, cfs->durations, old_state);
	if (cdev_value) {
		cdev_value->value += ktime_ms_delta(now, cfs->timestamp);
		cfs->timestamp = now;
	}

	/*
	 * Get the transition in the transitions list. If this one
	 * does not exist, a new allocated one will be returned.
	 * Increment the occurrence of this transition which is stored
	 * in the value field.
	 */
	cdev_value = thermal_debugfs_cdev_value_get(dfs, cfs->transitions,
						    transition);
	if (cdev_value)
		cdev_value->value++;

	cfs->total++;

	mutex_unlock(&dfs->lock);
}

/**
 * thermal_debug_cdev_add - Add a cooling device debugfs entry
 *
 * Allocates a cooling device object for debug, initializes the
 * statistics and create the entries in sysfs.

 * @cdev: a pointer to a cooling device
 */
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
		INIT_LIST_HEAD(&cfs->transitions[i]);
		INIT_LIST_HEAD(&cfs->durations[i]);
	}

	cfs->current_state = 0;
	cfs->timestamp = ktime_get();

	debugfs_create_file("trans_table", 0400, dfs->d_top, dfs, &tt_fops);

	debugfs_create_file("time_in_state_ms", 0400, dfs->d_top, dfs, &dt_fops);

	debugfs_create_file("clear", 0200, dfs->d_top, dfs, &cdev_clear_fops);

	debugfs_create_u32("total_trans", 0400, dfs->d_top, &cfs->total);

	cdev->debugfs = dfs;
}

/**
 * thermal_debug_cdev_remove - Remove a cooling device debugfs entry
 *
 * Frees the statistics memory data and remove the debugfs entry
 *
 * @cdev: a pointer to a cooling device
 */
void thermal_debug_cdev_remove(struct thermal_cooling_device *cdev)
{
	struct thermal_debugfs *dfs = cdev->debugfs;

	if (!dfs)
		return;

	mutex_lock(&dfs->lock);
	
	thermal_debugfs_cdev_clear(&dfs->cdev);
	cdev->debugfs = NULL;
	thermal_debugfs_remove_id(dfs);

	mutex_unlock(&dfs->lock);
}
