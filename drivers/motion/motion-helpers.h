/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MOTION_HELPERS_H__
#define __MOTION_HELPERS_H__

#include "motion-core.h"

struct motion_timed_speed_ops {
	void (*set_speed)(struct motion_device *mdev, unsigned int dir,
			unsigned int speed);
	int (*check_speed)(struct motion_device *mdev, unsigned int dir,
			unsigned int speed);
	void (*startup)(struct motion_device *mdev);
	void (*powerdown)(struct motion_device *mdev);
};

int motion_timed_speed_init(struct motion_device *mdev,
		struct motion_timed_speed_ops *ops, unsigned int full_scale);
void motion_fwnode_get_capabilities(struct motion_device *mdev,
		struct fwnode_handle *fwnode);
long motion_distance_to_time(struct motion_device *mdev,
		unsigned int index, int distance, ktime_t *t);

#endif /* __MOTION_HELPERS_H__ */
