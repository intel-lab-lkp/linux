// SPDX-License-Identifier: GPL-2.0-only
/*
 * DM timer Capture Driver
 */

#include <clocksource/timer-ti-dm.h>
#include <linux/atomic.h>
#include <linux/counter.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_data/dmtimer-omap.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/time.h>

#define TIMER_DRV_NAME "CAP_OMAP_DMTIMER"
/* Timer signals */
#define TIMER_CLOCK_SIG 0
#define TIMER_INPUT_SIG 1

/**
 * struct cap_omap_dmtimer_counter - Structure representing a cap counter
 *				  corresponding to omap dm timer.
 * @counter:		Capture counter
 * @enabled:		Tracks the enable status of omap dm timer.
 * @mutex:			Mutex to protect cap apply state
 * @dm_timer:		Pointer to omap dm timer.
 * @pdata:			Pointer to omap dm timer ops.
 * @dm_timer_pdev:	Pointer to omap dm timer platform device
 */
struct cap_omap_dmtimer_counter {
	struct counter_device counter;
	bool enabled;
	/* Mutex to protect cap apply state */
	struct mutex mutex;
	struct omap_dm_timer *dm_timer;
	const struct omap_dm_timer_ops *pdata;
	struct platform_device *dm_timer_pdev;
};
/**
 * cap_omap_dmtimer_start() - Start the cap omap dm timer in capture mode
 * @omap:	Pointer to cap omap dm timer counter
 */
static void cap_omap_dmtimer_start(struct cap_omap_dmtimer_counter *omap)
{
	u32 ret;
	struct device *dev = &omap->dm_timer_pdev->dev;

	ret = omap->pdata->start(omap->dm_timer);
	if (ret)
		dev_err(dev, "%d: Failed to start timer.\n", ret);
}

/**
 * cap_omap_dmtimer_is_enabled() -  Detect if the timer capture is enabled.
 * @omap:	Pointer to cap omap dm timer counter
 *
 * Return true if capture is enabled else false.
 */
static bool cap_omap_dmtimer_is_enabled(struct cap_omap_dmtimer_counter *omap)
{
	u32 status;

	status = omap->pdata->get_cap_status(omap->dm_timer);

	return !!(status & OMAP_TIMER_CTRL_ST);
}

static int cap_omap_dmtimer_clk_get_freq(struct counter_device *counter,
				 struct counter_signal *signal, u64 *freq)
{
	struct cap_omap_dmtimer_counter *omap = counter_priv(counter);
	struct clk *fclk;

	fclk = omap->pdata->get_fclk(omap->dm_timer);
	if (!fclk) {
		dev_err(counter->parent, "invalid dmtimer fclk\n");
		return -EINVAL;
	}

	*freq = clk_get_rate(fclk);
	if (!(*freq)) {
		dev_err(counter->parent, "invalid dmtimer fclk rate\n");
		return -EINVAL;
	}

	return 0;
}
/**
 * cap_omap_dmtimer_apply() - Changes the state of the cap omap dm timer counter.
 * @counter:Pointer to capture counter.
 *
 * Return 0 if successfully changed the state else appropriate error.
 */
static int cap_omap_dmtimer_apply(struct counter_device *counter)
{
	struct cap_omap_dmtimer_counter *omap = counter_priv(counter);
	struct device *dev = &omap->dm_timer_pdev->dev;
	int ret = 0;

	/* Ensure that the timer is in stop mode so that the configs can be changed. */
	if (cap_omap_dmtimer_is_enabled(omap)) {
		ret = omap->pdata->stop(omap->dm_timer);
		if (ret)
			dev_err(dev, "%d: Failed to stop timer.\n", ret);
	}

	ret = omap->pdata->set_cap(omap->dm_timer, true, true);
	if (ret) {
		dev_err(dev, "%d: Failed to set timer capture configuration.\n", ret);
		return ret;
	}

	cap_omap_dmtimer_start(omap);

	return ret;
}

static int cap_omap_dmtimer_capture(struct counter_device *counter,
					struct counter_count *count, u64 *duty_cycle)
{
	struct cap_omap_dmtimer_counter *omap = counter_priv(counter);
	*duty_cycle = 0;

	if (!omap->enabled) {
		dev_err(counter->parent, "Timer is disabled.\n");
		omap->pdata->stop(omap->dm_timer);
		return 0;
	}

	*duty_cycle = omap->pdata->read_cap(omap->dm_timer, false);

	*duty_cycle = *duty_cycle > 0 ? *duty_cycle : 0;

	return *duty_cycle;
}

static int cap_omap_dmtimer_period(struct counter_device *counter,
					struct counter_signal *signal, u64 *freq)
{
	struct cap_omap_dmtimer_counter *omap = counter_priv(counter);
	u64 clk_freq = 0;
	u64 period = 0;
	*freq = 0;

	if (!omap->enabled) {
		dev_err(counter->parent, "Timer is disabled.\n");
		omap->pdata->stop(omap->dm_timer);
		return 0;
	}

	period = omap->pdata->read_cap(omap->dm_timer, true);
	cap_omap_dmtimer_clk_get_freq(counter, signal, &clk_freq);

	if (period > 0)
		*freq = clk_freq/period;

	return *freq+1;
}
static int cap_omap_dmtimer_enable_read(struct counter_device *counter,
				struct counter_count *count, u8 *enable)
{
	struct cap_omap_dmtimer_counter *omap = counter_priv(counter);

	*enable = omap->enabled;

	return 0;
}

static int cap_omap_dmtimer_count_read(struct counter_device *counter,
			       struct counter_count *count, u64 *val)
{
	struct cap_omap_dmtimer_counter *omap = counter_priv(counter);

	*val = omap->pdata->read_counter(omap->dm_timer);

	return 0;
}

static int cap_omap_dmtimer_count_write(struct counter_device *counter,
				struct counter_count *count, u64 val)
{
	struct cap_omap_dmtimer_counter *omap = counter_priv(counter);

	if (val > U32_MAX)
		return -ERANGE;

	omap->pdata->write_counter(omap->dm_timer, val);

	return 0;
}

static int cap_omap_dmtimer_enable_write(struct counter_device *counter,
				 struct counter_count *count, u8 enable)
{
	struct cap_omap_dmtimer_counter *omap = counter_priv(counter);

	if (enable == omap->enabled)
		goto out;

	if (enable)
		cap_omap_dmtimer_apply(counter);
	else
		omap->pdata->stop(omap->dm_timer);

	omap->enabled = enable;
out:
	return 0;
}

static int cap_omap_dmtimer_function_read(struct counter_device *counter,
				  struct counter_count *count,
				  enum counter_function *function)
{
	*function = COUNTER_FUNCTION_INCREASE;

	return 0;
}

static int cap_omap_dmtimer_action_read(struct counter_device *counter,
				struct counter_count *count,
				struct counter_synapse *synapse,
				enum counter_synapse_action *action)
{
	*action = (synapse->signal->id == TIMER_CLOCK_SIG) ?
		   COUNTER_SYNAPSE_ACTION_RISING_EDGE :
		   COUNTER_SYNAPSE_ACTION_NONE;

	return 0;
}

static const struct counter_ops cap_omap_dmtimer_ops = {
	.count_read = cap_omap_dmtimer_count_read,
	.count_write = cap_omap_dmtimer_count_write,
	.function_read = cap_omap_dmtimer_function_read,
	.action_read = cap_omap_dmtimer_action_read,
};

static const enum counter_function cap_omap_dmtimer_functions[] = {
	COUNTER_FUNCTION_INCREASE,
};

static struct counter_comp cap_omap_dmtimer_clock_ext[] = {
	COUNTER_COMP_SIGNAL_U64("frequency", cap_omap_dmtimer_clk_get_freq, NULL),
};

static struct counter_signal cap_omap_dmtimer_signals[] = {
	{
		.id = TIMER_CLOCK_SIG,
		.name = "Clock Signal",
		.ext = cap_omap_dmtimer_clock_ext,
		.num_ext = ARRAY_SIZE(cap_omap_dmtimer_clock_ext),
	},
	{
		.id = TIMER_INPUT_SIG,
		.name = "Input Signal",
	},
};

/* Counter will increase at rising edges of a clock */
static const enum counter_synapse_action cap_omap_dmtimer_clock_actions[] = {
	COUNTER_SYNAPSE_ACTION_RISING_EDGE,
};

/* No trigger here */
static const enum counter_synapse_action cap_omap_dmtimer_input_actions[] = {
	COUNTER_SYNAPSE_ACTION_NONE,
};

static struct counter_synapse cap_omap_dmtimer_synapses[] = {
	{
		.actions_list = cap_omap_dmtimer_clock_actions,
		.num_actions = ARRAY_SIZE(cap_omap_dmtimer_clock_actions),
		.signal = &cap_omap_dmtimer_signals[TIMER_CLOCK_SIG],
	},
	{
		.actions_list = cap_omap_dmtimer_input_actions,
		.num_actions = ARRAY_SIZE(cap_omap_dmtimer_input_actions),
		.signal = &cap_omap_dmtimer_signals[TIMER_INPUT_SIG],
	},
};

static struct counter_comp cap_omap_dmtimer_count_ext[] = {
	COUNTER_COMP_CAPTURE(cap_omap_dmtimer_capture, NULL),
	COUNTER_COMP_ENABLE(cap_omap_dmtimer_enable_read, cap_omap_dmtimer_enable_write),
	COUNTER_COMP_FREQUENCY(cap_omap_dmtimer_period),
};

static struct counter_count cap_omap_dmtimer_counts[] = {
	{
		.name = "Timestamp Counter",
		.functions_list = cap_omap_dmtimer_functions,
		.num_functions = ARRAY_SIZE(cap_omap_dmtimer_functions),
		.synapses = cap_omap_dmtimer_synapses,
		.num_synapses = ARRAY_SIZE(cap_omap_dmtimer_synapses),
		.ext = cap_omap_dmtimer_count_ext,
		.num_ext = ARRAY_SIZE(cap_omap_dmtimer_count_ext),
	},
};

static int cap_omap_dmtimer_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	struct dmtimer_platform_data *timer_pdata;
	const struct omap_dm_timer_ops *pdata;
	struct platform_device *timer_pdev;
	struct omap_dm_timer *dm_timer;
	struct device_node *timer;
	struct cap_omap_dmtimer_counter *omap;
	struct counter_device *counter_dev;
	int ret = 0;

	timer = of_parse_phandle(np, "ti,timers", 0);
	if (!timer) {
		dev_err(&pdev->dev, "Unable to find Timer node\n");
		return -ENODEV;
	}

	timer_pdev = of_find_device_by_node(timer);
	if (!timer_pdev) {
		dev_err(&pdev->dev, "Unable to find Timer pdev\n");
		ret = -ENODEV;
		goto err_find_timer_pdev;
	}
	timer_pdata = dev_get_platdata(&timer_pdev->dev);
	if (!timer_pdata) {
		dev_dbg(&pdev->dev,
			"dmtimer pdata structure NULL, deferring probe\n");
		ret = -EPROBE_DEFER;
		dev_err_probe(&pdev->dev, ret, "Probe deferred\n");
		goto err_platdata;
	}

	pdata = timer_pdata->timer_ops;

	if (!pdata || !pdata->request_by_node ||
	    !pdata->free ||
	    !pdata->enable ||
	    !pdata->disable ||
	    !pdata->get_fclk ||
	    !pdata->start ||
	    !pdata->stop ||
	    !pdata->set_load ||
	    !pdata->set_match ||
	    !pdata->set_cap ||
	    !pdata->get_cap_status ||
		!pdata->read_cap ||
	    !pdata->set_prescaler ||
		!pdata->write_counter) {
		dev_err(&pdev->dev, "Incomplete dmtimer pdata structure\n");
		ret = -EINVAL;
		goto err_platdata;
	}

	dm_timer = pdata->request_by_node(timer);
	if (!dm_timer) {
		ret = -EPROBE_DEFER;
		goto err_request_timer;
	}

    /* struct cap_omap_dmtimer_counter *omap */
	counter_dev = devm_counter_alloc(dev, sizeof(*omap));
	if (!counter_dev) {
		dev_err(&pdev->dev, "Unable to allocate dmtimercounter\n");
		ret = -ENOMEM;
		goto err_alloc_omap;
	}
	omap = counter_priv(counter_dev);

	counter_dev->name = TIMER_DRV_NAME;
	counter_dev->parent = dev;
	counter_dev->ops = &cap_omap_dmtimer_ops;
	counter_dev->signals = cap_omap_dmtimer_signals;
	counter_dev->num_signals = ARRAY_SIZE(cap_omap_dmtimer_signals);
	counter_dev->counts = cap_omap_dmtimer_counts;
	counter_dev->num_counts = ARRAY_SIZE(cap_omap_dmtimer_counts);
	mutex_init(&omap->mutex);
	omap->pdata = pdata;
	omap->dm_timer = dm_timer;
	omap->dm_timer_pdev = timer_pdev;

	if (pm_runtime_active(&omap->dm_timer_pdev->dev))
		omap->pdata->stop(omap->dm_timer);

	of_node_put(timer);

	platform_set_drvdata(pdev, counter_dev);

	ret = devm_counter_add(dev, counter_dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add counter\n");

	return 0;

err_alloc_omap:
	pdata->free(dm_timer);
err_request_timer:

err_platdata:
	put_device(&timer_pdev->dev);
err_find_timer_pdev:

	of_node_put(timer);

	return ret;
}

static void cap_omap_dmtimer_remove(struct platform_device *pdev)
{
	struct counter_device *counter = platform_get_drvdata(pdev);
	struct cap_omap_dmtimer_counter *omap = counter_priv(counter);

	counter_unregister(counter);

	if (pm_runtime_active(&omap->dm_timer_pdev->dev))
		omap->pdata->stop(omap->dm_timer);

	omap->pdata->free(omap->dm_timer);

	put_device(&omap->dm_timer_pdev->dev);

	mutex_destroy(&omap->mutex);

}

static const struct of_device_id cap_omap_dmtimer_of_match[] = {
	{.compatible = "ti,omap-dmtimer-cap"},
	{}
};
MODULE_DEVICE_TABLE(of, cap_omap_dmtimer_of_match);

static struct platform_driver cap_omap_dmtimer_driver = {
	.driver = {
		.name = "omap-dmtimer-cap",
		.of_match_table = cap_omap_dmtimer_of_match,
	},
	.probe = cap_omap_dmtimer_probe,
	.remove = cap_omap_dmtimer_remove,
};
module_platform_driver(cap_omap_dmtimer_driver);

MODULE_AUTHOR("Gokul Praveen <g-praveen@ti.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("OMAP CAP Driver using Dual-mode Timers");
MODULE_IMPORT_NS("COUNTER");
