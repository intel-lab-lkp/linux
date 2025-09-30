// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/drivers/devfreq/governor_simpleondemand.c
 *
 *  Copyright (C) 2011 Samsung Electronics
 *	MyungJoo Ham <myungjoo.ham@samsung.com>
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/devfreq.h>
#include <linux/math64.h>
#include <linux/slab.h>
#include "governor.h"

/* Default constants for DevFreq-Simple-Ondemand (DFSO) */
#define DFSO_UPTHRESHOLD	(90)
#define DFSO_DOWNDIFFERENCTIAL	(5)
static int devfreq_simple_ondemand_func(struct devfreq *df,
					unsigned long *freq)
{
	int err;
	struct devfreq_dev_status *stat;
	unsigned long long a, b;
	unsigned int dfso_upthreshold, dfso_downdifferential;
	struct devfreq_simple_ondemand_data *data = df->governor_data;

	if (unlikely(!data))
		return -ENOMEM;

	err = devfreq_update_stats(df);
	if (err)
		return err;

	stat = &df->last_status;

	dfso_upthreshold = data->upthreshold;
	dfso_downdifferential = data->downdifferential;

	if (dfso_upthreshold > 100 ||
	    dfso_upthreshold < dfso_downdifferential)
		return -EINVAL;

	/* Assume MAX if it is going to be divided by zero */
	if (stat->total_time == 0) {
		*freq = DEVFREQ_MAX_FREQ;
		return 0;
	}

	/* Prevent overflow */
	if (stat->busy_time >= (1 << 24) || stat->total_time >= (1 << 24)) {
		stat->busy_time >>= 7;
		stat->total_time >>= 7;
	}

	/* Set MAX if it's busy enough */
	if (stat->busy_time * 100 >
	    stat->total_time * dfso_upthreshold) {
		*freq = DEVFREQ_MAX_FREQ;
		return 0;
	}

	/* Set MAX if we do not know the initial frequency */
	if (stat->current_frequency == 0) {
		*freq = DEVFREQ_MAX_FREQ;
		return 0;
	}

	/* Keep the current frequency */
	if (stat->busy_time * 100 >
	    stat->total_time * (dfso_upthreshold - dfso_downdifferential)) {
		*freq = stat->current_frequency;
		return 0;
	}

	/* Set the desired frequency based on the load */
	a = stat->busy_time;
	a *= stat->current_frequency;
	b = div_u64(a, stat->total_time);
	b *= 100;
	b = div_u64(b, (dfso_upthreshold - dfso_downdifferential / 2));
	*freq = (unsigned long) b;

	return 0;
}

static int simple_ondemand_init(struct devfreq *devfreq)
{
	struct devfreq_simple_ondemand_data *gov_data, *df_data;


	gov_data = kzalloc(sizeof(struct devfreq_simple_ondemand_data),
					      GFP_KERNEL);

	if (!gov_data) {
		dev_err(&devfreq->dev, "Can't alloc devfreq_simple_ondemand_data\n");
		return -ENOMEM;
	}


	df_data = devfreq->data;
	gov_data->upthreshold = DFSO_UPTHRESHOLD;
	gov_data->downdifferential = DFSO_DOWNDIFFERENCTIAL;

	if (df_data) {
		if (df_data->upthreshold)
			gov_data->upthreshold = df_data->upthreshold;

		if (df_data->downdifferential)
			gov_data->downdifferential = df_data->downdifferential;
	}

	devfreq->governor_data = gov_data;
	devfreq_monitor_start(devfreq);

	return 0;
}

static void simple_ondemand_exit(struct devfreq *devfreq)
{
	devfreq_monitor_stop(devfreq);
	kfree(devfreq->governor_data);
	devfreq->governor_data = NULL;
}

static int devfreq_simple_ondemand_handler(struct devfreq *devfreq,
				unsigned int event, void *data)
{
	int ret = 0;

	switch (event) {
	case DEVFREQ_GOV_START:
		ret = simple_ondemand_init(devfreq);
		break;

	case DEVFREQ_GOV_STOP:
		simple_ondemand_exit(devfreq);
		break;

	case DEVFREQ_GOV_UPDATE_INTERVAL:
		devfreq_update_interval(devfreq, (unsigned int *)data);
		break;

	case DEVFREQ_GOV_SUSPEND:
		devfreq_monitor_suspend(devfreq);
		break;

	case DEVFREQ_GOV_RESUME:
		devfreq_monitor_resume(devfreq);
		break;

	default:
		break;
	}

	return ret;
}

static struct devfreq_governor devfreq_simple_ondemand = {
	.name = DEVFREQ_GOV_SIMPLE_ONDEMAND,
	.attrs = DEVFREQ_GOV_ATTR_POLLING_INTERVAL
		| DEVFREQ_GOV_ATTR_TIMER,
	.get_target_freq = devfreq_simple_ondemand_func,
	.event_handler = devfreq_simple_ondemand_handler,
};

static int __init devfreq_simple_ondemand_init(void)
{
	return devfreq_add_governor(&devfreq_simple_ondemand);
}
subsys_initcall(devfreq_simple_ondemand_init);

static void __exit devfreq_simple_ondemand_exit(void)
{
	int ret;

	ret = devfreq_remove_governor(&devfreq_simple_ondemand);
	if (ret)
		pr_err("%s: failed remove governor %d\n", __func__, ret);

	return;
}
module_exit(devfreq_simple_ondemand_exit);
MODULE_DESCRIPTION("DEVFREQ Simple On-demand governor");
MODULE_LICENSE("GPL");
