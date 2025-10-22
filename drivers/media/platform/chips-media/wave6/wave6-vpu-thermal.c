// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * Wave6 series multi-standard codec IP - wave6 thermal cooling interface
 *
 * Copyright (C) 2025 CHIPS&MEDIA INC
 *
 */

#include <linux/pm_domain.h>
#include <linux/pm_opp.h>
#include <linux/units.h>
#include <linux/slab.h>
#include "wave6-vpu-thermal.h"

static int wave6_vpu_thermal_cooling_update(struct vpu_thermal_cooling *thermal,
					    int state)
{
	unsigned long new_clock_rate;
	int ret;

	if (state > thermal->thermal_max || !thermal->cooling)
		return 0;

	new_clock_rate = DIV_ROUND_UP(thermal->freq_table[state], HZ_PER_KHZ);
	dev_dbg(thermal->dev, "receive cooling state: %d, new clock rate %ld\n",
		state, new_clock_rate);

	ret = dev_pm_genpd_set_performance_state(thermal->dev, new_clock_rate);
	if (ret && !((ret == -ENODEV) || (ret == -EOPNOTSUPP))) {
		dev_err(thermal->dev, "failed to set perf to %lu, ret = %d\n",
			new_clock_rate, ret);
		return ret;
	}

	return 0;
}

static int wave6_vpu_cooling_get_max_state(struct thermal_cooling_device *cdev,
					   unsigned long *state)
{
	struct vpu_thermal_cooling *thermal = cdev->devdata;

	*state = thermal->thermal_max;

	return 0;
}

static int wave6_vpu_cooling_get_cur_state(struct thermal_cooling_device *cdev,
					   unsigned long *state)
{
	struct vpu_thermal_cooling *thermal = cdev->devdata;

	*state = thermal->thermal_event;

	return 0;
}

static int wave6_vpu_cooling_set_cur_state(struct thermal_cooling_device *cdev,
					   unsigned long state)
{
	struct vpu_thermal_cooling *thermal = cdev->devdata;

	thermal->thermal_event = state;
	wave6_vpu_thermal_cooling_update(thermal, state);

	return 0;
}

static struct thermal_cooling_device_ops wave6_cooling_ops = {
	.get_max_state = wave6_vpu_cooling_get_max_state,
	.get_cur_state = wave6_vpu_cooling_get_cur_state,
	.set_cur_state = wave6_vpu_cooling_set_cur_state,
};

int wave6_vpu_cooling_init(struct vpu_thermal_cooling *thermal)
{
	int i;
	int num_opps;
	unsigned long freq;

	if (WARN_ON(!thermal || !thermal->dev))
		return -EINVAL;

	num_opps = dev_pm_opp_get_opp_count(thermal->dev);
	if (num_opps <= 0) {
		dev_err(thermal->dev, "fail to get pm opp count, ret = %d\n", num_opps);
		return -ENODEV;
	}

	thermal->freq_table = kcalloc(num_opps, sizeof(*thermal->freq_table), GFP_KERNEL);
	if (!thermal->freq_table)
		goto error;

	for (i = 0, freq = ULONG_MAX; i < num_opps; i++, freq--) {
		struct dev_pm_opp *opp;

		opp = dev_pm_opp_find_freq_floor(thermal->dev, &freq);
		if (IS_ERR(opp))
			break;

		dev_pm_opp_put(opp);

		dev_dbg(thermal->dev, "[%d] = %ld\n", i, freq);
		if (freq < 100 * HZ_PER_MHZ)
			break;

		thermal->freq_table[i] = freq;
		thermal->thermal_max = i;
	}

	if (!thermal->thermal_max)
		goto error;

	thermal->thermal_event = 0;
	thermal->cooling = thermal_of_cooling_device_register(thermal->dev->of_node,
							      dev_name(thermal->dev),
							      thermal,
							      &wave6_cooling_ops);
	if (IS_ERR(thermal->cooling)) {
		dev_err(thermal->dev, "register cooling device failed\n");
		goto error;
	}

	return 0;

error:
	wave6_vpu_cooling_remove(thermal);

	return -EINVAL;
}

void wave6_vpu_cooling_remove(struct vpu_thermal_cooling *thermal)
{
	thermal_cooling_device_unregister(thermal->cooling);
	kfree(thermal->freq_table);
	thermal->freq_table = NULL;
}
