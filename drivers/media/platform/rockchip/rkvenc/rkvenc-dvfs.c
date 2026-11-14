// SPDX-License-Identifier: GPL-2.0-only
/*
 * Frequency scaling and cooling of the RKVENC v1 encoder. devfreq is
 * evaluated once per round of the streaming contexts, so no polling
 * interval is involved. Below the lowest operating point, cooling
 * stretches the period between jobs.
 */

#include <linux/devfreq.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/pm_opp.h>
#include <linux/pm_qos.h>
#include <linux/thermal.h>
#include <linux/units.h>

#include "rkvenc.h"

#define RKVENC_THROTTLE_STEPS 3
#define RKVENC_THROTTLE_STEP_PCT 25

static unsigned long rkvenc_dvfs_us(ktime_t t)
{
	return min_t(u64, ktime_to_us(t), ULONG_MAX);
}

static int rkvenc_dvfs_target(struct device *dev, unsigned long *freq,
			      u32 flags)
{
	struct rkvenc_dev *rkvenc = dev_get_drvdata(dev);
	struct rkvenc_dvfs *dvfs = &rkvenc->dvfs;
	struct dev_pm_opp *opp;
	int ret;

	opp = devfreq_recommended_opp(dev, freq, flags);
	if (IS_ERR(opp))
		return PTR_ERR(opp);

	*freq = dev_pm_opp_get_freq(opp);
	dev_pm_opp_put(opp);

	mutex_lock(&dvfs->freq_lock);
	wait_event(dvfs->idle_wq, !READ_ONCE(dvfs->busy));
	ret = dev_pm_opp_set_rate(dev, *freq);
	mutex_unlock(&dvfs->freq_lock);

	return ret;
}

static int rkvenc_dvfs_cur_freq(struct device *dev, unsigned long *freq)
{
	struct rkvenc_dev *rkvenc = dev_get_drvdata(dev);

	*freq = clk_get_rate(rkvenc->clocks[RKVENC_CLK_CORE].clk);
	return 0;
}

static int rkvenc_dvfs_status(struct device *dev,
			      struct devfreq_dev_status *stat)
{
	struct rkvenc_dev *rkvenc = dev_get_drvdata(dev);
	struct rkvenc_dvfs *dvfs = &rkvenc->dvfs;
	unsigned long flags;
	ktime_t now, busy;

	spin_lock_irqsave(&dvfs->lock, flags);
	now = ktime_get();
	busy = dvfs->busy_total;
	if (dvfs->busy) {
		busy = ktime_add(busy, ktime_sub(now, dvfs->busy_since));
		dvfs->busy_since = now;
	}
	stat->total_time = rkvenc_dvfs_us(ktime_sub(now, dvfs->last_eval));
	stat->busy_time = min(rkvenc_dvfs_us(busy), stat->total_time);
	dvfs->busy_total = 0;
	dvfs->last_eval = now;
	spin_unlock_irqrestore(&dvfs->lock, flags);

	rkvenc_dvfs_cur_freq(dev, &stat->current_frequency);
	return 0;
}

void rkvenc_dvfs_job_begin(struct rkvenc_dev *rkvenc)
{
	struct rkvenc_dvfs *dvfs = &rkvenc->dvfs;
	unsigned long flags;

	mutex_lock(&dvfs->freq_lock);
	spin_lock_irqsave(&dvfs->lock, flags);
	dvfs->busy = true;
	dvfs->job_start = ktime_get();
	dvfs->busy_since = dvfs->job_start;
	spin_unlock_irqrestore(&dvfs->lock, flags);
	mutex_unlock(&dvfs->freq_lock);
}

bool rkvenc_dvfs_start_allowed(struct rkvenc_dev *rkvenc)
{
	struct rkvenc_dvfs *dvfs = &rkvenc->dvfs;
	unsigned long flags, delay;
	s64 delay_us;
	bool allowed;

	spin_lock_irqsave(&dvfs->lock, flags);
	delay_us = ktime_to_us(ktime_sub(dvfs->next_start, ktime_get()));
	allowed = dvfs->duty_pct >= 100 || delay_us <= 0;
	delay = usecs_to_jiffies(clamp_t(s64, delay_us, 0, UINT_MAX));
	if (!allowed)
		queue_delayed_work(system_wq, &dvfs->throttle_work, delay);
	spin_unlock_irqrestore(&dvfs->lock, flags);

	return allowed;
}

void rkvenc_dvfs_job_end(struct rkvenc_dev *rkvenc, struct rkvenc_ctx *ctx)
{
	struct rkvenc_dvfs *dvfs = &rkvenc->dvfs;
	struct devfreq *devfreq = dvfs->devfreq;
	unsigned long flags;
	bool evaluate;
	ktime_t now;

	spin_lock_irqsave(&dvfs->lock, flags);
	now = ktime_get();
	dvfs->busy_total =
		ktime_add(dvfs->busy_total, ktime_sub(now, dvfs->busy_since));
	dvfs->next_start = ktime_add_ns(
		dvfs->job_start,
		div_u64(ktime_to_ns(ktime_sub(now, dvfs->job_start)) * 100,
			dvfs->duty_pct));
	dvfs->busy = false;
	/* A round ends with the context that opened it, covering every other one */
	evaluate = dvfs->round_ctx == ctx;
	if (!dvfs->round_ctx)
		dvfs->round_ctx = ctx;
	spin_unlock_irqrestore(&dvfs->lock, flags);
	wake_up(&dvfs->idle_wq);

	if (!devfreq || !evaluate)
		return;

	mutex_lock(&devfreq->lock);
	update_devfreq(devfreq);
	mutex_unlock(&devfreq->lock);
}

static int rkvenc_dvfs_freqs_get(struct rkvenc_dev *rkvenc)
{
	struct rkvenc_dvfs *dvfs = &rkvenc->dvfs;
	struct device *dev = rkvenc->dev;
	struct dev_pm_opp *opp;
	unsigned long freq = 0;
	int num, i;

	num = dev_pm_opp_get_opp_count(dev);
	if (num <= 0)
		return num ? num : -ENODEV;

	dvfs->freqs = devm_kcalloc(dev, num, sizeof(*dvfs->freqs), GFP_KERNEL);
	if (!dvfs->freqs)
		return -ENOMEM;

	for (i = 0; i < num; i++, freq++) {
		opp = dev_pm_opp_find_freq_ceil(dev, &freq);
		if (IS_ERR(opp))
			return PTR_ERR(opp);
		dvfs->freqs[i] = freq;
		dev_pm_opp_put(opp);
	}

	dvfs->num_freqs = num;
	return 0;
}

static int rkvenc_cdev_max_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	struct rkvenc_dvfs *dvfs = cdev->devdata;

	*state = dvfs->num_freqs - 1 + RKVENC_THROTTLE_STEPS;
	return 0;
}

static int rkvenc_cdev_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	struct rkvenc_dvfs *dvfs = cdev->devdata;

	*state = dvfs->cooling_state;
	return 0;
}

static int rkvenc_cdev_set_state(struct thermal_cooling_device *cdev,
				 unsigned long state)
{
	struct rkvenc_dvfs *dvfs = cdev->devdata;
	unsigned long idx = min_t(unsigned long, state, dvfs->num_freqs - 1);
	unsigned long freq = dvfs->freqs[dvfs->num_freqs - 1 - idx];
	unsigned long flags;
	int ret;

	ret = dev_pm_qos_update_request(&dvfs->max_freq_req, freq / HZ_PER_KHZ);
	if (ret < 0)
		return ret;

	spin_lock_irqsave(&dvfs->lock, flags);
	dvfs->duty_pct = 100 - RKVENC_THROTTLE_STEP_PCT * (state - idx);
	spin_unlock_irqrestore(&dvfs->lock, flags);
	dvfs->cooling_state = state;

	return 0;
}

static const struct thermal_cooling_device_ops rkvenc_cdev_ops = {
	.get_max_state = rkvenc_cdev_max_state,
	.get_cur_state = rkvenc_cdev_cur_state,
	.set_cur_state = rkvenc_cdev_set_state,
};

static void rkvenc_dvfs_throttle_work(struct work_struct *work)
{
	struct rkvenc_dvfs *dvfs = container_of(
		to_delayed_work(work), struct rkvenc_dvfs, throttle_work);
	struct rkvenc_ctx *ctx;

	mutex_lock(&dvfs->ctx_lock);
	list_for_each_entry(ctx, &dvfs->ctx_list, dvfs_node)
		v4l2_m2m_try_schedule(ctx->m2m_ctx);
	mutex_unlock(&dvfs->ctx_lock);
}

void rkvenc_dvfs_ctx_add(struct rkvenc_dev *rkvenc, struct rkvenc_ctx *ctx)
{
	struct rkvenc_dvfs *dvfs = &rkvenc->dvfs;

	mutex_lock(&dvfs->ctx_lock);
	list_add_tail(&ctx->dvfs_node, &dvfs->ctx_list);
	mutex_unlock(&dvfs->ctx_lock);
}

void rkvenc_dvfs_ctx_idle(struct rkvenc_dev *rkvenc, struct rkvenc_ctx *ctx)
{
	struct rkvenc_dvfs *dvfs = &rkvenc->dvfs;
	unsigned long flags;

	spin_lock_irqsave(&dvfs->lock, flags);
	if (dvfs->round_ctx == ctx)
		dvfs->round_ctx = NULL;
	spin_unlock_irqrestore(&dvfs->lock, flags);
}

void rkvenc_dvfs_ctx_del(struct rkvenc_dev *rkvenc, struct rkvenc_ctx *ctx)
{
	struct rkvenc_dvfs *dvfs = &rkvenc->dvfs;

	rkvenc_dvfs_ctx_idle(rkvenc, ctx);
	mutex_lock(&dvfs->ctx_lock);
	list_del(&ctx->dvfs_node);
	mutex_unlock(&dvfs->ctx_lock);
}

void rkvenc_dvfs_exit(struct rkvenc_dev *rkvenc)
{
	struct rkvenc_dvfs *dvfs = &rkvenc->dvfs;

	thermal_cooling_device_unregister(dvfs->cdev);
	dvfs->cdev = NULL;

	if (dvfs->qos_added)
		dev_pm_qos_remove_request(&dvfs->max_freq_req);
	dvfs->qos_added = false;

	if (dvfs->devfreq)
		devfreq_remove_device(dvfs->devfreq);
	dvfs->devfreq = NULL;

	cancel_delayed_work_sync(&dvfs->throttle_work);
}

static int rkvenc_dvfs_cooling_init(struct rkvenc_dev *rkvenc)
{
	struct rkvenc_dvfs *dvfs = &rkvenc->dvfs;
	struct device *dev = rkvenc->dev;
	int ret;

	if (!of_property_present(dev->of_node, "#cooling-cells"))
		return 0;

	ret = dev_pm_qos_add_request(dev, &dvfs->max_freq_req,
				     DEV_PM_QOS_MAX_FREQUENCY,
				     PM_QOS_MAX_FREQUENCY_DEFAULT_VALUE);
	if (ret < 0)
		return ret;
	dvfs->qos_added = true;

	dvfs->cdev = thermal_of_cooling_device_register(
		dev->of_node, 0, "rkvenc", dvfs, &rkvenc_cdev_ops);
	if (IS_ERR(dvfs->cdev)) {
		ret = PTR_ERR(dvfs->cdev);
		dvfs->cdev = NULL;
		return dev_err_probe(dev, ret,
				     "could not add the cooling device\n");
	}

	return 0;
}

int rkvenc_dvfs_init(struct rkvenc_dev *rkvenc)
{
	static const char *const clk_names[] = { "core", NULL };
	static const char *const supply_names[] = { "venc", NULL };
	struct dev_pm_opp_config opp_config = { .clk_names = clk_names };
	struct rkvenc_dvfs *dvfs = &rkvenc->dvfs;
	struct device *dev = rkvenc->dev;
	struct devfreq *devfreq;
	int ret;

	spin_lock_init(&dvfs->lock);
	mutex_init(&dvfs->freq_lock);
	mutex_init(&dvfs->ctx_lock);
	INIT_LIST_HEAD(&dvfs->ctx_list);
	INIT_DELAYED_WORK(&dvfs->throttle_work, rkvenc_dvfs_throttle_work);
	init_waitqueue_head(&dvfs->idle_wq);
	dvfs->last_eval = ktime_get();
	dvfs->duty_pct = 100;

	if (!of_property_present(dev->of_node, "operating-points-v2"))
		return 0;

	if (of_property_present(dev->of_node, "venc-supply"))
		opp_config.regulator_names = supply_names;

	ret = devm_pm_opp_set_config(dev, &opp_config);
	if (ret)
		return ret;

	ret = devm_pm_opp_of_add_table(dev);
	if (ret)
		return ret;

	ret = rkvenc_dvfs_freqs_get(rkvenc);
	if (ret)
		return ret;

	dvfs->profile.target = rkvenc_dvfs_target;
	dvfs->profile.get_dev_status = rkvenc_dvfs_status;
	dvfs->profile.get_cur_freq = rkvenc_dvfs_cur_freq;
	dvfs->profile.freq_table = dvfs->freqs;
	dvfs->profile.max_state = dvfs->num_freqs;
	rkvenc_dvfs_cur_freq(dev, &dvfs->profile.initial_freq);

	devfreq = devfreq_add_device(dev, &dvfs->profile,
				     DEVFREQ_GOV_SIMPLE_ONDEMAND, NULL);
	if (IS_ERR(devfreq))
		return dev_err_probe(dev, PTR_ERR(devfreq),
				     "could not add the devfreq device\n");

	dvfs->devfreq = devfreq;
	ret = rkvenc_dvfs_cooling_init(rkvenc);
	if (ret)
		rkvenc_dvfs_exit(rkvenc);

	return ret;
}
