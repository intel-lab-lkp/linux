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

#include "thermal_core.h"

static struct dentry *d_root;
static struct dentry *d_cdev;
static struct dentry *d_tz;

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
 * The trip_stats structure has the relevant information to show the
 * statistics related to a trip point violation during a mitigation
 * episode.
 */
struct trip_stats {
	ktime_t timestamp;
	ktime_t duration;
	int count;
	int max;
	int min;
	int avg;
};

/*
 * The tz_events structure describes a mitigation episode. A
 * mitigation episode is when the mitigation begins and ends. During
 * this episode we can have multiple trip points crossed the way up
 * and down if there are multiple trip describes in the firmware.
 */
struct tz_events {
	struct list_head list;
	struct trip_stats *trip_stats;
	ktime_t timestamp;
	ktime_t duration;
};

/*
 * The tz_debugfs structure contains the list of the mitigation
 * episodes and has to track which trip point has been crossed in
 * order to handle correctly nested trip point mitigation episodes.
 */
struct tz_debugfs {
	struct list_head tz_events;
	int *trips_crossed;
	int trip_index;
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
		struct tz_debugfs tz;
	};
};

void thermal_debug_init(void)
{
	d_root = debugfs_create_dir("thermal", NULL);
	if (!d_root)
		return;

	d_cdev = debugfs_create_dir("cooling_devices", d_root);
	if (!d_cdev)
		return;

	d_tz = debugfs_create_dir("thermal_zones", d_root);
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

	debugfs_remove(dfs->d_top);
	thermal_debugfs_cdev_reset(&dfs->cdev);
	cdev->debugfs = NULL;
	kfree(dfs);
}

static struct tz_events *thermal_debugfs_tz_event_alloc(struct thermal_zone_device *tz,
							ktime_t now)
{
	struct tz_events *tze;
	struct trip_stats *trip_stats;
	int i;

	tze = kzalloc(sizeof(*tze), GFP_KERNEL);
	if (!tze)
		return NULL;

	INIT_LIST_HEAD(&tze->list);
	tze->timestamp = now;

	trip_stats = kzalloc(sizeof(struct trip_stats) * tz->num_trips, GFP_KERNEL);
	if (!trip_stats) {
		kfree(tze);
		return NULL;
	}

	for (i = 0; i < tz->num_trips; i++) {
		trip_stats[i].min = INT_MAX;
		trip_stats[i].max = INT_MIN;
	}
	
	tze->trip_stats = trip_stats;

	return tze;
}

void thermal_debug_tz_trip_up(struct thermal_zone_device *tz, struct thermal_trip *trip)
{
	struct tz_events *tze;
	struct thermal_debugfs *dfs = tz->debugfs;
	int temperature = tz->temperature;
	int trip_id = thermal_zone_trip_id(tz, trip);
	ktime_t now = ktime_get();

	if (!dfs)
		return;

	mutex_lock(&dfs->lock);

	/*
	 * The mitigation is starting. A mitigation can contain
	 * several episodes where each of them is related to a
	 * temperature crossing a trip point. The episodes are
	 * nested. That means when the temperature is crossing the
	 * first trip point, the duration begins to be measured. If
	 * the temperature continues to increase and reaches the
	 * second trip point, the duration of the first trip must be
	 * also accumulated.
	 *
	 * eg.
	 *
	 * temp
	 *   ^
	 *   |             --------
	 * trip 2         /        \         ------
	 *   |           /|        |\      /|      |\
	 * trip 1       / |        | `----  |      | \
	 *   |         /| |        |        |      | |\
	 * trip 0     / | |        |        |      | | \
	 *   |       /| | |        |        |      | | |\
	 *   |      / | | |        |        |      | | | `--
	 *   |     /  | | |        |        |      | | |     
	 *   |-----   | | |        |        |      | | |      
	 *   |        | | |        |        |      | | |
	 *    --------|-|-|--------|--------|------|-|-|------------------> time
	 *            | | |<--t2-->|        |<-t2'>| | |
	 *            | |                            | |
	 *            | |<------------t1------------>| |
	 *            |                                |
	 *            |<-------------t0--------------->|
	 *
	 */
	if (dfs->tz.trip_index < 0) {
		tze = thermal_debugfs_tz_event_alloc(tz, now);
		if (!tze)
			return;

		list_add(&tze->list, &dfs->tz.tz_events);
	}

	dfs->tz.trip_index++;
	dfs->tz.trips_crossed[dfs->tz.trip_index] = trip_id;

	tze = list_first_entry(&dfs->tz.tz_events, struct tz_events, list);
	tze->trip_stats[trip_id].timestamp = now;
        tze->trip_stats[trip_id].max = max(tze->trip_stats[trip_id].max, temperature);
	tze->trip_stats[trip_id].min = min(tze->trip_stats[trip_id].min, temperature);
	tze->trip_stats[trip_id].avg = tze->trip_stats[trip_id].avg +
		(temperature - tze->trip_stats[trip_id].avg) / tze->trip_stats[trip_id].count;

	mutex_unlock(&dfs->lock);
}

void thermal_debug_tz_trip_down(struct thermal_zone_device *tz, struct thermal_trip *trip)
{
	struct thermal_debugfs *dfs = tz->debugfs;
	struct tz_events *tze;
	int trip_id = thermal_zone_trip_id(tz, trip);
	ktime_t delta, now = ktime_get();

	if (!dfs)
		return;

	/*
	 * The temperature crosses the way down but there was not
	 * mitigation detected before. That may happen when the
	 * temperature is greater than a trip point when registering a
	 * thermal zone, which is a common use case as the kernel has
	 * no mitigation mechanism yet at boot time.
	 */
	if (dfs->tz.trip_index < 0)
		return;
	
	mutex_lock(&dfs->lock);
	
	tze = list_first_entry(&dfs->tz.tz_events, struct tz_events, list);

	delta = ktime_sub(now, tze->trip_stats[trip_id].timestamp);
	tze->trip_stats[trip_id].duration = ktime_add(delta, tze->trip_stats[trip_id].duration);

	dfs->tz.trip_index--;

	/*
	 * This event closes the mitigation as we are crossing the
	 * last trip point the way down.
	 */
	if (dfs->tz.trip_index < 0)
		tze->duration = ktime_sub(now, tze->timestamp);

	mutex_unlock(&dfs->lock);
}

void thermal_debug_update_temp(struct thermal_zone_device *tz)
{
	struct thermal_debugfs *dfs = tz->debugfs;
	struct tz_events *tze;
	int trip;

	if (!dfs)
		return;

	mutex_lock(&dfs->lock);

	if (dfs->tz.trip_index >= 0) {
		trip = dfs->tz.trip_index;
		tze = list_first_entry(&dfs->tz.tz_events, struct tz_events, list);
		tze->trip_stats[trip].count++;
		tze->trip_stats[trip].max = max(tze->trip_stats[trip].max, tz->temperature);
		tze->trip_stats[trip].min = min(tze->trip_stats[trip].min, tz->temperature);
		tze->trip_stats[trip].avg = tze->trip_stats[trip].avg +
			(tz->temperature - tze->trip_stats[trip].avg) / tze->trip_stats[trip].count;
	}

	mutex_unlock(&dfs->lock);
}

static void *tze_seq_start(struct seq_file *s, loff_t *pos)
{
	struct thermal_zone_device *tz = s->private;
	struct thermal_debugfs *dfs = tz->debugfs;
	struct tz_debugfs *tzd = &dfs->tz;

	mutex_lock(&dfs->lock);

	return seq_list_start(&tzd->tz_events, *pos);
}

static void *tze_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	struct thermal_zone_device *tz = s->private;
	struct thermal_debugfs *dfs = tz->debugfs;
        struct tz_debugfs *tzd = &dfs->tz;

	return seq_list_next(v, &tzd->tz_events, pos);
}

static void tze_seq_stop(struct seq_file *s, void *v)
{
	struct thermal_zone_device *tz = s->private;
	struct thermal_debugfs *dfs = tz->debugfs;

	mutex_unlock(&dfs->lock);
}

static int tze_seq_show(struct seq_file *s, void *v)
{
	struct thermal_zone_device *tz = s->private;
	struct tz_events *tze;
	int i;

	tze = list_entry((struct list_head *)v, struct tz_events, list);

	seq_printf(s, ",-Mitigation at %lluus, duration=%llums\n", ktime_to_us(tze->timestamp),
		   ktime_to_ms(tze->duration));

	seq_printf(s, "| trip |     type | temp(°mC) | hyst(°mC) |  duration  |  avg(°mC) |  min(°mC) |  max(°mC) |\n");
	
	for (i = 0; i < tz->num_trips; i++) {

		struct thermal_trip trip;
		const char *type;
		
		if (__thermal_zone_get_trip(tz, i, &trip))
			continue;

		/*
		 * There is no possible mitigation happening at the
		 * critical trip point, so the stats will be always
		 * zero, skip this trip point
		 */
		if (trip.type == THERMAL_TRIP_CRITICAL)
			continue;

		if (trip.type == THERMAL_TRIP_PASSIVE)
			type = "passive";
		else if (trip.type == THERMAL_TRIP_ACTIVE)
			type = "active";
		else
			type = "hot";

		seq_printf(s, "| %*d | %*s | %*d | %*d | %*lld | %*d | %*d | %*d |\n",
			   4 , i,
			   8, type,
			   9, trip.temperature,
			   9, trip.hysteresis,
			   10, ktime_to_ms(tze->trip_stats[i].duration),
			   9, tze->trip_stats[i].avg,
			   9, tze->trip_stats[i].min,
			   9, tze->trip_stats[i].max);
	}

	return 0;
}

static const struct seq_operations tze_sops = {
	.start = tze_seq_start,
	.next = tze_seq_next,
	.stop = tze_seq_stop,
	.show = tze_seq_show,
};

DEFINE_SEQ_ATTRIBUTE(tze);

void thermal_debug_tz_add(struct thermal_zone_device *tz)
{
	struct thermal_debugfs *dfs;
	struct tz_debugfs *tzd;

	dfs = thermal_debugfs_add_id(d_tz, tz->id);
	if (!dfs)
		return;

	tzd = &dfs->tz;

	tzd->trips_crossed = kzalloc(sizeof(int) * tz->num_trips, GFP_KERNEL);
	if (!tzd->trips_crossed) {
		thermal_debugfs_remove_id(dfs);
		return;
	}

	/*
	 * Trip index '-1' means no mitigation
	 */
	tzd->trip_index = -1;
	INIT_LIST_HEAD(&tzd->tz_events);

	debugfs_create_file("mitigations", 0400, dfs->d_top, tz, &tze_fops);
	
	tz->debugfs = dfs;
}

void thermal_debug_tz_remove(struct thermal_zone_device *tz)
{
	thermal_debugfs_remove_id(tz->debugfs);
	tz->debugfs = NULL;
}
