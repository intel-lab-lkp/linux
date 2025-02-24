// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2020 Samsung Electronics Co., Ltd.
 * Copyright 2020 Google LLC.
 * Copyright 2025 Linaro Ltd.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/container_of.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/workqueue.h>

#include "exynos-acpm.h"

#define ACPM_DEBUG_CMD				BIT(14)

#define ACPM_PRINT_CONFIG			GENMASK(15, 14)
#define ACPM_PRINT_CMD				BIT(13)
#define ACPM_PRINT_SET_LOGB_GPRIO_LEVEL		1
#define ACPM_PRINT_GET_LOGB_GPRIO_LEVEL		3

#define ACPM_LOG_LEVEL_MAX			2
#define ACPM_LOG_POLL_PERIOD_US			500

/* Tick runs at 49.152 MHz, the period below is in picoseconds. */
#define ACPM_APM_SYSTICK_PERIOD_PS		20345

#define ACPM_DEBUGFS_ROOT "acpm_framework"

enum acpm_debug_commands {
	ACPM_DEBUG_DISABLE_WATCHDOG,
	ACPM_DEBUG_ENABLE_WATCHDOG,
	ACPM_DEBUG_SOFT_LOCKUP,
	ACPM_DEBUG_HARD_LOCKUP,
	ACPM_DBUG_EXCEPTION,
	ACPM_DEBUG_NOTIFY_SHUTDOWN,
	ACPM_DEBUG_RAMDUMP_ON,
	ACPM_DEBUG_MAX,
};

struct acpm_log_buf {
	struct acpm_queue q;
	unsigned int qlen;
	unsigned int mlen;
	unsigned int rear_index;
};

struct acpm_log_info {
	struct workqueue_struct *wq;
	struct acpm_info *acpm;
	struct delayed_work work;
	struct acpm_log_buf normal;
	struct acpm_log_buf preempt;
	unsigned int level;
	unsigned int poll_period;
};

union acpm_log_entry {
	u32 raw[4];
	struct {
		u32 systicks0 : 24;
		u32 dummy : 2;
		u32 is_err : 1;
		u32 is_raw : 1;
		u32 plugin_id : 4;
		u32 systicks24;
		u32 msg : 24;
		u32 systicks56 : 8;
		u32 data;
	} __packed;
};

static struct dentry *rootdir;

static DEFINE_MUTEX(acpm_log_level_mutex);

static void acpm_log_print_entry(struct acpm_info *acpm,
				 const union acpm_log_entry *log_entry)
{
	u64 systicks, time, msg;

	if (log_entry->is_err)
		return;

	if (log_entry->is_raw) {
		dev_info(acpm->dev, "[ACPM_FW raw] : id:%u, %x, %x, %x\n",
			 log_entry->plugin_id, log_entry->raw[1],
			 log_entry->raw[2], log_entry->raw[3]);
	} else {
		systicks = ((u64)(log_entry->systicks56) << 56) +
			   ((u64)(log_entry->systicks24) << 24) +
			   log_entry->systicks0;

		/* report time in ns */
		time = mul_u64_u32_div(systicks, ACPM_APM_SYSTICK_PERIOD_PS,
				       1000);

		msg = readl(acpm->sram_base + log_entry->msg);

		dev_info(acpm->dev, "[ACPM_FW] : %llu id:%u, %s, %x\n", time,
			 log_entry->plugin_id, (char *)&msg, log_entry->data);
	}
}

static void acpm_log_print_entries(struct acpm_info *acpm,
				   struct acpm_log_buf *lbuf)
{
	union acpm_log_entry log_entry = {0};
	u32 front, rear;

	front = readl(lbuf->q.front);
	rear = lbuf->rear_index;

	while (rear != front) {
		__ioread32_copy(&log_entry, lbuf->q.base + lbuf->mlen * rear,
				sizeof(log_entry) / 4);

		acpm_log_print_entry(acpm, &log_entry);

		if (lbuf->qlen == rear + 1)
			rear = 0;
		else
			rear++;

		lbuf->rear_index = rear;
		front = readl(lbuf->q.front);
	}
}

static void acpm_log_print(struct acpm_info *acpm)
{
	struct acpm_log_info *acpm_log = acpm->log;

	guard(mutex)(&acpm_log_level_mutex);

	if (acpm_log->level == 0)
		return;

	if (acpm_log->level == ACPM_LOG_LEVEL_MAX)
		acpm_log_print_entries(acpm, &acpm_log->preempt);

	acpm_log_print_entries(acpm, &acpm_log->normal);
}

static void acpm_work_fn(struct work_struct *work)
{
	struct acpm_log_info *acpm_log =
		container_of(work, struct acpm_log_info, work.work);
	struct acpm_info *acpm = acpm_log->acpm;

	acpm_log_print(acpm);

	queue_delayed_work(acpm_log->wq, &acpm_log->work,
			   msecs_to_jiffies(acpm_log->poll_period));
}

static int acpm_log_level_get(void *data, u64 *val)
{
	struct acpm_info *acpm = data;

	*val = acpm->log->level;

	return 0;
}

static int acpm_log_level_set(void *data, u64 val)
{
	struct acpm_info *acpm = data;
	struct acpm_log_info *acpm_log = acpm->log;

	if (val > ACPM_LOG_LEVEL_MAX) {
		dev_err(acpm->dev, "Log level %llu out of range [0:%u]!\n",
			val, ACPM_LOG_LEVEL_MAX);
		return -EINVAL;
	}

	scoped_guard(mutex, &acpm_log_level_mutex)
		acpm_log->level = val;

	if (acpm_log->level == 0)
		cancel_delayed_work_sync(&acpm_log->work);
	else
		queue_delayed_work(acpm_log->wq, &acpm_log->work,
				   msecs_to_jiffies(acpm_log->poll_period));
	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(acpm_log_level_fops, acpm_log_level_get,
			 acpm_log_level_set, "0%llu\n");

/**
 * acpm_logb_gprio_level_get() - get ACPM Log Buffer Group Priority logging
 * level.
 * @data:	pointer to the driver data.
 * @val:	pointer where the ACPM Log Buffer Group Priority logging level
 *		will be saved.
 *
 * The 64 bit hex value encodes the plugin ID log level request on 4 bits,
 * supporting a maximum of 16 plugin IDs. Plugin ID 0 is described by
 * GENMASK(3, 0), followed by the other plugin IDs in ascending order, up to
 * plugin ID 15 which is described by GENMASK(63, 60).
 * Value 0xf is log error level, and 0x0 is log debug level.
 */
static int acpm_logb_gprio_level_get(void *data, u64 *val)
{
	struct acpm_info *acpm = data;
	struct acpm_xfer xfer;
	u32 cmd[4] = {0};
	int ret;

	cmd[0] = ACPM_PRINT_CMD |
		 FIELD_PREP(ACPM_PRINT_CONFIG, ACPM_PRINT_GET_LOGB_GPRIO_LEVEL);

	xfer.txd = cmd;
	xfer.txlen = sizeof(cmd);
	xfer.rxd = cmd;
	xfer.rxlen = sizeof(cmd);
	xfer.acpm_chan_id = acpm->mbox_dbg_chan;

	ret = acpm_do_xfer(&acpm->handle, &xfer);
	if (!ret)
		*val = (((u64)xfer.rxd[2]) << 32) | xfer.rxd[1];

	return ret;
}

/**
 * acpm_logb_gprio_level_set() - set ACPM Log Buffer Group Priority logging
 * level.
 * @data:	pointer to the driver data.
 * @val:	64 bit hex value to set.
 * The 64 bit hex value encodes the plugin ID log level request on 4 bits,
 * supporting a maximum of 16 plugin IDs. Plugin ID 0 is described by
 * GENMASK(3, 0), followed by the other plugin IDs in ascending order, up to
 * plugin ID 15 which is described by GENMASK(63, 60).
 * Value 0xf is log error level, and 0x0 is log debug level.
 */
static int acpm_logb_gprio_level_set(void *data, u64 val)
{
	struct acpm_info *acpm = data;
	struct acpm_xfer xfer = {0};
	u32 cmd[4] = {0};

	cmd[0] = ACPM_PRINT_CMD |
		 FIELD_PREP(ACPM_PRINT_CONFIG, ACPM_PRINT_SET_LOGB_GPRIO_LEVEL);
	cmd[1] = val;
	cmd[2] = val >> 32;

	xfer.txd = cmd;
	xfer.txlen = sizeof(cmd);
	xfer.acpm_chan_id = acpm->mbox_dbg_chan;

	return acpm_do_xfer(&acpm->handle, &xfer);
}

DEFINE_DEBUGFS_ATTRIBUTE(acpm_logb_gprio_level_fops, acpm_logb_gprio_level_get,
			 acpm_logb_gprio_level_set, "0x%016llx\n");

static int acpm_debug_cmd_set(void *data, u64 val)
{
	struct acpm_info *acpm = data;
	struct acpm_xfer xfer = {0};
	u32 cmd[4] = {0};

	if (val >= ACPM_DEBUG_MAX) {
		dev_err(acpm->dev, "sub-cmd:%llu out of range!\n", val);
		return 0;
	}

	cmd[0] = val | ACPM_DEBUG_CMD;

	xfer.txd = cmd;
	xfer.txlen = sizeof(cmd);
	xfer.acpm_chan_id = acpm->mbox_dbg_chan;

	return acpm_do_xfer(&acpm->handle, &xfer);
}

DEFINE_DEBUGFS_ATTRIBUTE(acpm_debug_cmd_fops, NULL, acpm_debug_cmd_set,
			 "0x%016llx\n");

static void acpm_debugfs_init(struct acpm_info *acpm)
{
	rootdir = debugfs_create_dir(ACPM_DEBUGFS_ROOT, NULL);

	debugfs_create_file("log_level", 0644, rootdir, acpm,
			    &acpm_log_level_fops);
	debugfs_create_file("logb_gprio_level", 0644, rootdir, acpm,
			    &acpm_logb_gprio_level_fops);
	debugfs_create_file("acpm_debug_cmd", 0644, rootdir, acpm,
			    &acpm_debug_cmd_fops);
}

/**
 * acpm_debug_get_params() - get debug parameters of the normal and preempt
 * queues.
 * @acpm:	pointer to the driver data.
 */
static void acpm_debug_get_params(struct acpm_info *acpm)
{
	struct acpm_shmem __iomem *shmem = acpm->shmem;
	void __iomem *base = acpm->sram_base;
	struct acpm_log_info *acpm_log = acpm->log;
	struct acpm_log_buf *lbuf;

	lbuf = &acpm_log->normal;
	lbuf->q.base = base + readl(&shmem->log_base);
	lbuf->q.rear = base + readl(&shmem->log_rear);
	lbuf->q.front = base + readl(&shmem->log_front);
	lbuf->qlen = readl(&shmem->log_qlen);
	lbuf->mlen = readl(&shmem->log_mlen);

	lbuf = &acpm_log->preempt;
	lbuf->q.base = base + readl(&shmem->preempt_log_base);
	lbuf->q.rear = base + readl(&shmem->preempt_log_rear);
	lbuf->q.front = base + readl(&shmem->preempt_log_front);
	lbuf->qlen = readl(&shmem->preempt_log_qlen);
	lbuf->mlen = acpm_log->normal.mlen;
}

/**
 * acpm_debugfs_register() - register ACPM debug capabilities via debugfs.
 * @acpm:	pointer to the driver data.
 */
int acpm_debugfs_register(struct acpm_info *acpm)
{
	struct acpm_log_info *acpm_log;

	acpm_log = devm_kzalloc(acpm->dev, sizeof(*acpm_log), GFP_KERNEL);
	if (!acpm_log)
		return -ENOMEM;

	acpm->log = acpm_log;
	acpm_log->acpm = acpm;

	acpm_log->wq = alloc_workqueue("exynos-acpm-log-wq", 0, 0);
	if (!acpm_log->wq)
		return -ENOMEM;

	INIT_DELAYED_WORK(&acpm_log->work, acpm_work_fn);
	acpm_log->poll_period = ACPM_LOG_POLL_PERIOD_US;

	acpm_debug_get_params(acpm);

	acpm_debugfs_init(acpm);

	return 0;
}

void acpm_debugfs_remove(void)
{
	debugfs_remove_recursive(rootdir);
}
