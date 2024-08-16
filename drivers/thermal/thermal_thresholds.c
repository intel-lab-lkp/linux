// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2024 Linaro Limited
 *
 * Author: Daniel Lezcano <daniel.lezcano@linaro.org>
 *
 * Thermal thresholds
 */
#include <linux/list.h>
#include <linux/list_sort.h>
#include <linux/slab.h>

#include "thermal_core.h"

struct thresholds {
	struct list_head list;
};

int thermal_thresholds_init(struct thermal_zone_device *tz)
{
	struct thresholds *thresholds;

	thresholds = kmalloc(sizeof(*thresholds), GFP_KERNEL);
	if (!thresholds)
		return -ENOMEM;

	INIT_LIST_HEAD(&thresholds->list);
	tz->thresholds = thresholds;

	return 0;
}

void thermal_thresholds_exit(struct thermal_zone_device *tz)
{
	thermal_thresholds_flush(tz, 0);
	kfree(tz->thresholds);
	tz->thresholds = NULL;
}

static int __thermal_thresholds_cmp(void *data,
				    const struct list_head *l1,
				    const struct list_head *l2)
{
	struct threshold *t1 = container_of(l1, struct threshold, list);
	struct threshold *t2 = container_of(l2, struct threshold, list);

	return t1->temperature - t2->temperature;
}

static struct threshold *__thermal_thresholds_find(const struct thresholds *thresholds, int temperature)
{
	struct threshold *t;

	list_for_each_entry(t, &thresholds->list, list)
		if (t->temperature == temperature)
			return t;

	return NULL;
}

static bool __thermal_threshold_is_crossed(struct threshold *threshold, int temperature,
					   int last_temperature, int direction,
					   int *low, int *high)
{
	if (temperature > threshold->temperature && threshold->temperature > *low &&
	    (THERMAL_THRESHOLD_WAY_DOWN & threshold->direction))
		*low = threshold->temperature;

	if (temperature < threshold->temperature && threshold->temperature < *high &&
	    (THERMAL_THRESHOLD_WAY_UP & threshold->direction))
		*high = threshold->temperature;

	if (temperature < threshold->temperature &&
	    last_temperature >= threshold->temperature &&
	    (threshold->direction & direction))
		return true;

	if (temperature >= threshold->temperature &&
	    last_temperature < threshold->temperature &&
	    (threshold->direction & direction))
		return true;

	return false;
}

static bool thermal_thresholds_handle_raising(struct thresholds *thresholds, int temperature,
					      int last_temperature, int *low, int *high)
{
	struct threshold *t;

	list_for_each_entry(t, &thresholds->list, list) {
		if (__thermal_threshold_is_crossed(t, temperature, last_temperature,
						   THERMAL_THRESHOLD_WAY_UP, low, high))
			return true;
	}

	return false;
}

static bool thermal_thresholds_handle_dropping(struct thresholds *thresholds, int temperature,
					       int last_temperature, int *low, int *high)
{
	struct threshold *t;

	list_for_each_entry_reverse(t, &thresholds->list, list) {
		if (__thermal_threshold_is_crossed(t, temperature, last_temperature,
						   THERMAL_THRESHOLD_WAY_DOWN, low, high))
			return true;
	}

	return false;
}

void thermal_thresholds_flush(struct thermal_zone_device *tz, pid_t pid)
{
	struct thresholds *thresholds = tz->thresholds;
	struct threshold *entry, *tmp;

	lockdep_assert_held(&tz->lock);

	list_for_each_entry_safe(entry, tmp, &thresholds->list, list) {
		list_del(&entry->list);
		kfree(entry);
	}

	thermal_notify_threshold_flush(tz, pid);

	__thermal_zone_device_update(tz, THERMAL_THRESHOLD_FLUSHED);
}

int thermal_thresholds_handle(struct thermal_zone_device *tz, int *low, int *high)
{
	struct thresholds *thresholds = tz->thresholds;

	int temperature = tz->temperature;
	int last_temperature = tz->last_temperature;

	lockdep_assert_held(&tz->lock);

	/*
	 * We need a second update in order to detect a threshold being crossed
	 */
	if (last_temperature == THERMAL_TEMP_INVALID)
		return 0;

	/*
	 * The temperature is stable, so obviously we can not have
	 * crossed a threshold.
	 */
	if (last_temperature == temperature)
		return 0;

	/*
	 * Since last update the temperature:
	 * - increased : thresholds are crossed the way up
	 * - decreased : thresholds are crossed the way down
	 */
	if (temperature > last_temperature) {
		if (thermal_thresholds_handle_raising(thresholds, temperature,
						      last_temperature, low, high))
			thermal_notify_threshold_up(tz);
	} else {
		if (thermal_thresholds_handle_dropping(thresholds, temperature,
						       last_temperature, low, high))
			thermal_notify_threshold_down(tz);
	}

	return 0;
}

int thermal_thresholds_add(struct thermal_zone_device *tz,
			   int temperature, int direction, pid_t pid)
{
	struct thresholds *thresholds = tz->thresholds;
	struct threshold *t;

	lockdep_assert_held(&tz->lock);

	t = __thermal_thresholds_find(thresholds, temperature);
	if (t) {
		if (t->direction == direction)
			return -EEXIST;

		t->direction |= direction;
	} else {

		t = kmalloc(sizeof(*t), GFP_KERNEL);
		if (!t)
			return -ENOMEM;

		INIT_LIST_HEAD(&t->list);
		t->temperature = temperature;
		t->direction = direction;
		list_add(&t->list, &thresholds->list);
		list_sort(NULL, &thresholds->list, __thermal_thresholds_cmp);
	}

	thermal_notify_threshold_add(tz, temperature, direction, pid);

	__thermal_zone_device_update(tz, THERMAL_THRESHOLD_ADDED);

	return 0;
}

int thermal_thresholds_delete(struct thermal_zone_device *tz,
			      int temperature, int direction, pid_t pid)
{
	struct thresholds *thresholds = tz->thresholds;
	struct threshold *t;

	lockdep_assert_held(&tz->lock);

	t = __thermal_thresholds_find(thresholds, temperature);
	if (!t)
		return -ENOENT;

	if (t->direction == direction) {
		list_del(&t->list);
		kfree(t);
	} else {
		t->direction &= ~direction;
	}

	__thermal_zone_device_update(tz, THERMAL_THRESHOLD_DELETED);

	thermal_notify_threshold_delete(tz, temperature, direction, pid);

	return 0;
}

int thermal_thresholds_for_each(struct thermal_zone_device *tz,
				int (*cb)(struct threshold *, void *arg), void *arg)
{
	struct thresholds *thresholds = tz->thresholds;
	struct threshold *entry;
	int ret;

	lockdep_assert_held(&tz->lock);

	list_for_each_entry(entry, &thresholds->list, list) {
		ret = cb(entry, arg);
		if (ret)
			return ret;
	}

	return 0;
}
