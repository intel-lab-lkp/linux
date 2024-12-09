// SPDX-License-Identifier: GPL-2.0-only
/*
 * SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * Tegra host1x actmon driver
 */

#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/host1x.h>
#include <linux/units.h>

#include "dev.h"
#include "actmon.h"
#include "hw/actmon.h"

static void actmon_writel(struct host1x_actmon *actmon, u32 val, u32 offset)
{
	writel(val, actmon->regs + offset);
}

static u32 actmon_readl(struct host1x_actmon *actmon, u32 offset)
{
	return readl(actmon->regs + offset);
}

static void actmon_module_writel(struct host1x_actmon_module *module, u32 val, u32 offset)
{
	writel(val, module->regs + offset);
}

static u32 actmon_module_readl(struct host1x_actmon_module *module, u32 offset)
{
	return readl(module->regs + offset);
}

static void host1x_actmon_update_sample_period(struct host1x_actmon *actmon)
{
	unsigned long actmon_mhz;
	u32 actmon_clks_per_sample, sample_period, val = 0;

	actmon_mhz = actmon->rate / HZ_PER_MHZ;
	actmon_clks_per_sample = actmon_mhz * actmon->usecs_per_sample;

	val |= HOST1X_ACTMON_CTRL_SOURCE(2);

	if (actmon_clks_per_sample > 65536) {
		val |= HOST1X_ACTMON_CTRL_SAMPLE_TICK(1);
		sample_period = actmon_clks_per_sample / 65536;
	} else {
		val &= ~HOST1X_ACTMON_CTRL_SAMPLE_TICK(1);
		sample_period = actmon_clks_per_sample / 256;
	}

	val &= ~HOST1X_ACTMON_CTRL_SAMPLE_PERIOD_MASK;
	val |= HOST1X_ACTMON_CTRL_SAMPLE_PERIOD(sample_period);
	actmon_writel(actmon, val, HOST1X_ACTMON_CTRL_REG);
}

static int host1x_actmon_sample_period_get(void *data, u64 *val)
{
	struct host1x_actmon *actmon = (struct host1x_actmon *)data;

	*val = (u64)actmon->usecs_per_sample;

	return 0;
}

static int host1x_actmon_sample_period_set(void *data, u64 val)
{
	struct host1x_actmon *actmon = (struct host1x_actmon *)data;

	actmon->usecs_per_sample = (u32)val;
	host1x_actmon_update_sample_period(actmon);

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(host1x_actmon_sample_period_fops,
			host1x_actmon_sample_period_get,
		host1x_actmon_sample_period_set,
		"%lld\n");

/**
 * host1x_actmon_debug_init - Initialize actmon debugfs
 * @actmon: the actmon instance being configured
 * @name: an unique name of the actmon
 *
 * There are multiple modules available inside the actmon, and they perform the
 * signal sampling at the same rate. The debugfs of an actmon will expose this
 * shared configuration, sample_period, via a debugfs node:
 * - sample_period:
 *   Sampling period in micro-second of modules inside the actmon
 */
static void host1x_actmon_debug_init(struct host1x_actmon *actmon, const char *name)
{
	struct host1x *host = dev_get_drvdata(actmon->client->host->parent);

	if (!host->debugfs) {
		dev_warn(host->dev, "debugfs is unavailable\n");
		return;
	}

	if (!host->actmon_debugfs)
		host->actmon_debugfs = debugfs_create_dir("actmon", host->debugfs);

	actmon->debugfs = debugfs_create_dir(name, host->actmon_debugfs);

	/* R/W files */
	debugfs_create_file("sample_period", 0644, actmon->debugfs, actmon,
			    &host1x_actmon_sample_period_fops);
}

static int host1x_actmon_module_k_get(void *data, u64 *val)
{
	struct host1x_actmon_module *module = (struct host1x_actmon_module *)data;

	*val = (u64)module->k;

	return 0;
}

static int host1x_actmon_module_k_set(void *data, u64 val)
{
	struct host1x_actmon_module *module = (struct host1x_actmon_module *)data;
	u32 val32;

	module->k = (u32)val;

	val32 = actmon_module_readl(module, HOST1X_ACTMON_MODULE_CTRL_REG);
	val32 &= ~HOST1X_ACTMON_MODULE_CTRL_K_VAL_MASK;
	val32 |= HOST1X_ACTMON_MODULE_CTRL_K_VAL(module->k);
	actmon_module_writel(module, val32, HOST1X_ACTMON_MODULE_CTRL_REG);

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(host1x_actmon_module_k_fops,
			host1x_actmon_module_k_get,
		host1x_actmon_module_k_set,
		"%lld\n");

static int host1x_actmon_module_consec_upper_num_get(void *data, u64 *val)
{
	struct host1x_actmon_module *module = (struct host1x_actmon_module *)data;

	*val = (u64)module->consec_upper_num;

	return 0;
}

static int host1x_actmon_module_consec_upper_num_set(void *data, u64 val)
{
	struct host1x_actmon_module *module = (struct host1x_actmon_module *)data;
	u32 val32;

	module->consec_upper_num = (u32)val;

	val32 = actmon_module_readl(module, HOST1X_ACTMON_MODULE_CTRL_REG);
	val32 &= ~HOST1X_ACTMON_MODULE_CTRL_CONSEC_UPPER_NUM_MASK;
	val32 |= HOST1X_ACTMON_MODULE_CTRL_CONSEC_UPPER_NUM(module->consec_upper_num);
	actmon_module_writel(module, val32, HOST1X_ACTMON_MODULE_CTRL_REG);

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(host1x_actmon_module_consec_upper_num_fops,
			host1x_actmon_module_consec_upper_num_get,
		host1x_actmon_module_consec_upper_num_set,
		"%lld\n");

static int host1x_actmon_module_consec_lower_num_get(void *data, u64 *val)
{
	struct host1x_actmon_module *module = (struct host1x_actmon_module *)data;

	*val = (u64)module->consec_lower_num;

	return 0;
}

static int host1x_actmon_module_consec_lower_num_set(void *data, u64 val)
{
	struct host1x_actmon_module *module = (struct host1x_actmon_module *)data;
	u32 val32;

	module->consec_lower_num = (u32)val;

	val32 = actmon_module_readl(module, HOST1X_ACTMON_MODULE_CTRL_REG);
	val32 &= ~HOST1X_ACTMON_MODULE_CTRL_CONSEC_LOWER_NUM_MASK;
	val32 |= HOST1X_ACTMON_MODULE_CTRL_CONSEC_LOWER_NUM(module->consec_lower_num);
	actmon_module_writel(module, val32, HOST1X_ACTMON_MODULE_CTRL_REG);

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(host1x_actmon_module_consec_lower_num_fops,
			host1x_actmon_module_consec_lower_num_get,
		host1x_actmon_module_consec_lower_num_set,
		"%lld\n");

static int host1x_actmon_module_avg_norm_get(void *data, u64 *val)
{
	struct host1x_actmon_module *module = (struct host1x_actmon_module *)data;
	struct host1x_actmon *actmon = module->actmon;
	struct host1x_client *client = actmon->client;
	unsigned long client_freq;
	u32 active_clks, client_clks;

	if (!client->ops || !client->ops->get_rate)
		return -EOPNOTSUPP;

	active_clks = actmon_module_readl(module, HOST1X_ACTMON_MODULE_AVG_COUNT_REG);

	client_freq = client->ops->get_rate(client);
	client_clks = ((client_freq / 1000) * actmon->usecs_per_sample) / 1000;

	*val = (u64)(active_clks * 1000) / client_clks;

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(host1x_actmon_module_avg_norm_fops,
			host1x_actmon_module_avg_norm_get, NULL,
		"%lld\n");

/**
 * host1x_actmon_module_debug_init - Initialize debugfs for module inside actmon
 * @module: the actmon module being configured
 *
 * Each module inside the actmon is used for monitoring the utilization of the
 * underlying host1x client. The debugfs of an actmon module will expose the
 * following debugfs nodes:
 * - k:
 *   It is a programmable factor affecting the exponential-moving-average filter
 *   inside the actmon module for calculating the utilization of the engine over
 *   the time. The formula of the filter is as following:
 *
 *	K = 1 / 2^(k+1)
 *	A_t = (K * C_t) + ((1-K) * A_{t-1})
 *
 *   where A_t is the average utilization at time t. C_t is the sampled actmon
 *   counter value at time t.
 *
 * - consec_upper_num:
 *   "`consec_upper_num` + 1" of consecutive upper watermark breaches that need
 *   to occur before actmon module asserts the interrupt to CPU.
 *
 * - consec_lower_num:
 *   "`consec_lower_num` + 1" of consecutive lower watermark breaches that need
 *   to occur before actmon module asserts the interrupt to CPU.
 *
 * - usage:
 *   A normalized value representing the utilization of the engine ranges from
 *   0 to 1000.
 */
static void host1x_actmon_module_debug_init(struct host1x_actmon_module *module)
{
	struct host1x *host = dev_get_drvdata(module->actmon->client->host->parent);
	struct device *dev = module->actmon->client->dev;
	struct dentry *debugfs = module->actmon->debugfs;
	char dirname[8];

	if (!debugfs) {
		dev_warn(host->dev,
			 "actmon debugfs entry for %s was not found\n",
			 dev_name(dev));
		return;
	}

	snprintf(dirname, sizeof(dirname), "module%d", module->type);
	module->debugfs = debugfs_create_dir(dirname, debugfs);

	/* R/W files */
	debugfs_create_file("k", 0644, module->debugfs, module,
			    &host1x_actmon_module_k_fops);
	debugfs_create_file("consec_upper_num", 0644, module->debugfs, module,
			    &host1x_actmon_module_consec_upper_num_fops);
	debugfs_create_file("consec_lower_num", 0644, module->debugfs, module,
			    &host1x_actmon_module_consec_lower_num_fops);

	/* R files */
	debugfs_create_file("usage", 0444, module->debugfs, module,
			    &host1x_actmon_module_avg_norm_fops);
}

static void host1x_actmon_init(struct host1x_actmon *actmon)
{
	u32 val;

	/* Global control register */
	host1x_actmon_update_sample_period(actmon);

	/* Global interrupt enable register */
	val = (1 << actmon->num_modules) - 1;
	actmon_writel(actmon, val, HOST1X_ACTMON_INTR_ENB_REG);
}

static void host1x_actmon_deinit(struct host1x_actmon *actmon)
{
	actmon_writel(actmon, 0, HOST1X_ACTMON_CTRL_REG);
	actmon_writel(actmon, 0, HOST1X_ACTMON_INTR_ENB_REG);
}

static void host1x_actmon_module_init(struct host1x_actmon_module *module)
{
	/* Local control register */
	actmon_module_writel(module,
			     HOST1X_ACTMON_MODULE_CTRL_ACTMON_ENB(0) |
			     HOST1X_ACTMON_MODULE_CTRL_ENB_PERIODIC(1) |
			     HOST1X_ACTMON_MODULE_CTRL_K_VAL(module->k) |
			     HOST1X_ACTMON_MODULE_CTRL_CONSEC_UPPER_NUM(module->consec_upper_num) |
			     HOST1X_ACTMON_MODULE_CTRL_CONSEC_LOWER_NUM(module->consec_lower_num),
			     HOST1X_ACTMON_MODULE_CTRL_REG);

	/* Interrupt enable register (disable interrupts by default) */
	actmon_module_writel(module, 0, HOST1X_ACTMON_MODULE_INTR_ENB_REG);

	/* Interrupt status register */
	actmon_module_writel(module, ~0, HOST1X_ACTMON_MODULE_INTR_STATUS_REG);

	/* Consecutive watermark registers */
	actmon_module_writel(module, ~0, HOST1X_ACTMON_MODULE_UPPER_WMARK_REG);
	actmon_module_writel(module, 0, HOST1X_ACTMON_MODULE_LOWER_WMARK_REG);

	/* Moving-average watermark registers */
	actmon_module_writel(module, ~0, HOST1X_ACTMON_MODULE_AVG_UPPER_WMARK_REG);
	actmon_module_writel(module, 0, HOST1X_ACTMON_MODULE_AVG_LOWER_WMARK_REG);

	/* Init average value register */
	actmon_module_writel(module, 0, HOST1X_ACTMON_MODULE_INIT_AVG_REG);
}

static void host1x_actmon_module_deinit(struct host1x_actmon_module *module)
{
	actmon_module_writel(module, 0, HOST1X_ACTMON_MODULE_CTRL_REG);
	actmon_module_writel(module, 0, HOST1X_ACTMON_MODULE_INTR_ENB_REG);
	actmon_module_writel(module, ~0, HOST1X_ACTMON_MODULE_INTR_STATUS_REG);
	actmon_module_writel(module, 0, HOST1X_ACTMON_MODULE_UPPER_WMARK_REG);
	actmon_module_writel(module, 0, HOST1X_ACTMON_MODULE_LOWER_WMARK_REG);
	actmon_module_writel(module, 0, HOST1X_ACTMON_MODULE_AVG_UPPER_WMARK_REG);
	actmon_module_writel(module, 0, HOST1X_ACTMON_MODULE_AVG_LOWER_WMARK_REG);
	actmon_module_writel(module, 0, HOST1X_ACTMON_MODULE_INIT_AVG_REG);
	actmon_module_writel(module, 0, HOST1X_ACTMON_MODULE_COUNT_WEIGHT_REG);
}

void host1x_actmon_handle_interrupt(struct host1x *host, int classid)
{
	unsigned long actmon_status, module_status;
	struct host1x_actmon_module *module;
	struct host1x_actmon *actmon, *tmp_actmon;
	struct host1x_client *client;

	list_for_each_entry_safe(actmon, tmp_actmon, &host->actmons, list) {
		if (actmon->client->class == classid)
			break;
	}

	client = actmon->client;
	module = &actmon->modules[HOST1X_ACTMON_MODULE_ACTIVE];

	actmon_status = actmon_readl(actmon, HOST1X_ACTMON_INTR_STATUS_REG);
	module_status = actmon_module_readl(module, HOST1X_ACTMON_MODULE_INTR_STATUS_REG);

	/* Trigger DFS if client supports it */
	if (client->ops && client->ops->actmon_event) {
		if (module_status & HOST1X_ACTMON_MODULE_INTR_CONSEC_WMARK_ABOVE)
			client->ops->actmon_event(client, HOST1X_ACTMON_CONSEC_WMARK_ABOVE);

		if (module_status & HOST1X_ACTMON_MODULE_INTR_CONSEC_WMARK_BELOW)
			client->ops->actmon_event(client, HOST1X_ACTMON_CONSEC_WMARK_BELOW);

		if (module_status & HOST1X_ACTMON_MODULE_INTR_AVG_WMARK_ABOVE)
			client->ops->actmon_event(client, HOST1X_ACTMON_AVG_WMARK_ABOVE);

		if (module_status & HOST1X_ACTMON_MODULE_INTR_AVG_WMARK_BELOW)
			client->ops->actmon_event(client, HOST1X_ACTMON_AVG_WMARK_BELOW);
	}

	actmon_module_writel(module, module_status, HOST1X_ACTMON_MODULE_INTR_STATUS_REG);
	actmon_writel(actmon, actmon_status, HOST1X_ACTMON_INTR_STATUS_REG);
}

int host1x_actmon_register(struct host1x_client *client)
{
	struct host1x *host = dev_get_drvdata(client->host->parent);
	const struct host1x_info *info = host->info;
	const struct host1x_actmon_entry *entry = NULL;
	struct host1x_actmon_module *module;
	struct host1x_actmon *actmon;
	int i;

	if (!info->has_actmon) {
		dev_dbg(host->dev, "actmon is not supported\n");
		return 0;
	}

	if (!host->actmon_regs) {
		dev_warn(host->dev,
			 "skip registration since actmon resource is not defined\n");
		return 0;
	}

	if (!host->actmon_clk) {
		dev_warn(host->dev,
			 "skip registration since actmon clock is unavailable\n");
		return 0;
	}

	if (client->actmon) {
		dev_warn(host->dev,
			 "%s has already registered actmon\n",
			 dev_name(client->dev));
		return 0;
	}

	for (i = 0; i < info->num_actmon_entries; i++) {
		if (info->actmon_table[i].classid == client->class)
			entry = &info->actmon_table[i];
	}
	if (!entry)
		return -ENODEV;

	actmon = devm_kzalloc(client->dev, sizeof(*actmon), GFP_KERNEL);
	if (!actmon)
		return -ENOMEM;

	INIT_LIST_HEAD(&actmon->list);
	mutex_lock(&host->actmons_lock);
	list_add_tail(&actmon->list, &host->actmons);
	mutex_unlock(&host->actmons_lock);

	actmon->client = client;
	actmon->rate = clk_get_rate(host->actmon_clk);
	actmon->regs = host->actmon_regs + entry->offset;
	actmon->irq = entry->irq;
	actmon->num_modules = entry->num_modules;
	actmon->usecs_per_sample = 1500;

	/* Configure actmon registers */
	host1x_actmon_init(actmon);

	/* Create debugfs for the actmon */
	host1x_actmon_debug_init(actmon, entry->name);

	/* Configure actmon module registers */
	for (i = 0; i < actmon->num_modules; i++) {
		module = &actmon->modules[i];
		module->actmon = actmon;
		module->type = i;
		module->regs = actmon->regs + (i * HOST1X_ACTMON_MODULE_OFFSET);

		module->k = 2;
		module->consec_upper_num = 7;
		module->consec_lower_num = 7;
		host1x_actmon_module_init(module);

		/* Create debugfs for the actmon module */
		host1x_actmon_module_debug_init(module);
	}

	client->actmon = actmon;

	return 0;
}
EXPORT_SYMBOL(host1x_actmon_register);

void host1x_actmon_unregister(struct host1x_client *client)
{
	struct host1x_actmon_module *module;
	struct host1x *host = dev_get_drvdata(client->host->parent);
	struct host1x_actmon *actmon = client->actmon;
	int i;

	if (!actmon)
		return;

	for (i = 0; i < actmon->num_modules; i++) {
		module = &actmon->modules[i];
		host1x_actmon_module_deinit(module);
		debugfs_remove_recursive(module->debugfs);
	}

	debugfs_remove_recursive(actmon->debugfs);

	host1x_actmon_deinit(actmon);

	mutex_lock(&host->actmons_lock);
	list_del(&actmon->list);
	mutex_unlock(&host->actmons_lock);

	client->actmon = NULL;
}
EXPORT_SYMBOL(host1x_actmon_unregister);

void host1x_actmon_enable(struct host1x_client *client)
{
	struct host1x_actmon *actmon = client->actmon;
	struct host1x_actmon_module *module;
	int i;

	if (!actmon)
		return;

	for (i = 0; i < actmon->num_modules; i++) {
		module = &actmon->modules[i];
		actmon_module_writel(module,
				     actmon_module_readl(module, HOST1X_ACTMON_MODULE_CTRL_REG) |
				     HOST1X_ACTMON_MODULE_CTRL_ACTMON_ENB(1),
				     HOST1X_ACTMON_MODULE_CTRL_REG);
	}
}
EXPORT_SYMBOL(host1x_actmon_enable);

void host1x_actmon_disable(struct host1x_client *client)
{
	struct host1x_actmon *actmon = client->actmon;
	struct host1x_actmon_module *module;
	int i;

	if (!actmon)
		return;

	for (i = 0; i < actmon->num_modules; i++) {
		module = &actmon->modules[i];
		actmon_module_writel(module,
				     actmon_module_readl(module, HOST1X_ACTMON_MODULE_CTRL_REG) &
				     ~HOST1X_ACTMON_MODULE_CTRL_ACTMON_ENB(1),
				     HOST1X_ACTMON_MODULE_CTRL_REG);
	}
}
EXPORT_SYMBOL(host1x_actmon_disable);

void host1x_actmon_update_client_rate(struct host1x_client *client,
				      unsigned long rate,
				      u32 *weight)
{
	struct host1x_actmon *actmon = client->actmon;
	struct host1x_actmon_module *module;
	u32 val;
	int i;

	if (!actmon) {
		*weight = 0;
		return;
	}

	val = (rate / actmon->rate) << 2;

	for (i = 0; i < actmon->num_modules; i++) {
		module = &actmon->modules[i];
		actmon_module_writel(module, val, HOST1X_ACTMON_MODULE_COUNT_WEIGHT_REG);
	}

	*weight = val;
}
EXPORT_SYMBOL(host1x_actmon_update_client_rate);
