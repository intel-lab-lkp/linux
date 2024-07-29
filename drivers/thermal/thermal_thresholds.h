/* SPDX-License-Identifier: GPL-2.0 */

struct threshold {
	int temperature;
	int direction;
	struct list_head list;
};

#ifdef CONFIG_THERMAL_THRESHOLDS
int thermal_thresholds_init(struct thermal_zone_device *tz);
void thermal_thresholds_exit(struct thermal_zone_device *tz);
void thermal_thresholds_flush(struct thermal_zone_device *tz, pid_t pid);
int thermal_thresholds_add(struct thermal_zone_device *tz, int temperature, int direction, pid_t pid);
int thermal_thresholds_delete(struct thermal_zone_device *tz, int temperature, int direction, pid_t pid);
int thermal_thresholds_handle(struct thermal_zone_device *tz, int *low, int *high);
int thermal_thresholds_for_each(struct thermal_zone_device *tz,
				int (*cb)(struct threshold *, void *arg), void *arg);
#else
static inline int thermal_thresholds_init(struct thermal_zone_device *tz)
{
	return 0;
}

static inline void thermal_thresholds_exit(struct thermal_zone_device *tz)
{
	;
}

static inline void thermal_thresholds_flush(struct thermal_zone_device *tz, pid_t pid)
{
	;
}

static inline int thermal_thresholds_add(struct thermal_zone_device *tz, int temperature, int direction, pid_t pid)
{
	return 0;
}

static inline int thermal_thresholds_delete(struct thermal_zone_device *tz, int temperature, int direction, pid_t pid)
{
	return 0;
}

static inline int thermal_thresholds_handle(struct thermal_zone_device *tz, int *low, int *high)
{
	return 0;
}

static inline int thermal_thresholds_for_each(struct thermal_zone_device *tz,
					      int (*cb)(struct threshold *, void *arg), void *arg)
{
	return 0;
}
#endif
