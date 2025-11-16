/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ILITEK Touch IC driver
 *
 * Copyright (C) 2011 ILI Technology Corporation.
 *
 * Author: Luca Hsu <luca_hsu@ilitek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#ifndef _ILITEK_COMMON_H_
#define _ILITEK_COMMON_H_
/* Includes of headers ------------------------------------------------------*/
#include <linux/sched.h>
#include <linux/firmware.h>

#include "ilitek_ts.h"
#include "ilitek_protocol.h"
#include "ilitek_update.h"

#include "ilitek_crypto.h"
#include "ilitek_report.h"

/* Extern define ------------------------------------------------------------*/
//driver information
#define DRIVER_VERSION_0				5
#define DRIVER_VERSION_1				9
#define DRIVER_VERSION_2				3
#define DRIVER_VERSION_3				0
#define CUSTOMER_H_ID					0
#define CUSTOMER_L_ID					0
#define TEST_VERSION					0

#define ILITEK_IOCTL_MAX_TRANSFER			5000UL

#define set_arr(arr, idx, val)			\
	do {					\
		if ((idx) < ARRAY_SIZE(arr))	\
			(arr)[(idx)] = (val);	\
	} while (0)

/* i2c clock rate for rk3288 */
#if ILITEK_PLAT == ILITEK_PLAT_ROCKCHIP && \
	KERNEL_VERSION(4, 0, 0) > LINUX_VERSION_CODE
#define SCL_RATE(rate)	.scl_rate = (rate),
#else
#define SCL_RATE(rate)
#endif

/* netlink */
#if KERNEL_VERSION(3, 6, 0) <= LINUX_VERSION_CODE
#define NETLINK_KERNEL_CFG_DECLARE(cfg, func)	\
	struct netlink_kernel_cfg cfg = {	\
		.groups = 0,			\
		.input = func,			\
	}
#if KERNEL_VERSION(3, 7, 0) <= LINUX_VERSION_CODE
#define NETLINK_KERNEL_CREATE(unit, cfg_ptr, func)	\
	netlink_kernel_create(&init_net, (unit), (cfg_ptr))
#else
#define NETLINK_KERNEL_CREATE(unit, cfg_ptr, func)	\
	netlink_kernel_create(&init_net, (unit), THIS_MODULE, (cfg_ptr))
#endif
#else
#define NETLINK_KERNEL_CFG_DECLARE(cfg, func)
#define NETLINK_KERNEL_CREATE(unit, cfg_ptr, func)	\
	netlink_kernel_create(&init_net, (unit), 0, (func), NULL, THIS_MODULE)
#endif

/* input_dev */
#if KERNEL_VERSION(3, 7, 0) <= LINUX_VERSION_CODE
#define INPUT_MT_INIT_SLOTS(dev, num)	\
		input_mt_init_slots((dev), (num), INPUT_MT_DIRECT)
#else
#define INPUT_MT_INIT_SLOTS(dev, num)	input_mt_init_slots((dev), (num))
#endif

/* file_operations ioctl */
#if KERNEL_VERSION(2, 6, 36) <= LINUX_VERSION_CODE
#define FOPS_IOCTL	unlocked_ioctl
#define FOPS_IOCTL_FUNC(func, cmd, arg) \
		long func(struct file *fp, cmd, arg)
#else
#define FOPS_IOCTL	ioctl
#define FOPS_IOCTL_FUNC(func, cmd, arg) \
		s32 func(struct inode *np, struct file *fp,	cmd, arg)

#endif

#if KERNEL_VERSION(6, 3, 0) > LINUX_VERSION_CODE
#define I2C_PROBE_FUNC(func, client_arg)	\
	int func(client_arg, const struct i2c_device_id *id)
#else
#define I2C_PROBE_FUNC(func, client_arg)	int func(client_arg)
#endif

#if KERNEL_VERSION(6, 1, 0) > LINUX_VERSION_CODE
#define REMOVE_FUNC(func, client_arg)	int func(client_arg)
#define REMOVE_RETURN(val)		({ __typeof__(val) _val = (val); return _val; })
#else
#define REMOVE_FUNC(func, client_arg)	void func(client_arg)
#define REMOVE_RETURN(val)		(val)
#endif

#if KERNEL_VERSION(6, 4, 0) > LINUX_VERSION_CODE
#define CLASS_CREATE(name)	class_create(THIS_MODULE, (name))
#else
#define CLASS_CREATE(name)	class_create((name))
#endif

/* procfs */
#if KERNEL_VERSION(5, 6, 0) > LINUX_VERSION_CODE
#define PROC_FOPS_T	file_operations
#define PROC_READ	read
#define PROC_WRITE	write
#define PROC_IOCTL		FOPS_IOCTL
#define PROC_COMPAT_IOCTL	compat_ioctl
#define PROC_OPEN	open
#define PROC_RELEASE	release
#else
#define PROC_FOPS_T	proc_ops
#define PROC_READ	proc_read
#define PROC_WRITE	proc_write
#define PROC_IOCTL		proc_ioctl
#define PROC_COMPAT_IOCTL	proc_compat_ioctl
#define PROC_OPEN	proc_open
#define PROC_RELEASE	proc_release
#endif

#ifdef MTK_UNDTS
#define ISR_FUNC(func)	void func(void)
#define ISR_RETURN(val)
#else
#define ISR_FUNC(func)	irqreturn_t func(int irq, void *dev_id)
#define ISR_RETURN(val)	({ __typeof__(val) _val = (val); return _val; })
#endif

enum ilitek_irq_handle_type {
	irq_type_normal = 0,
	irq_type_debug,
	irq_type_c_model,
};

struct ilitek_ts_data {
	void *client;
	struct device *device;
	struct ilitek_ts_device *dev;

	/* should > 2K for C-Model */
	u8 buf[4096];

	struct input_dev *input_dev;
	struct input_dev *pen_input_dev;
	struct regulator *vdd;
	struct regulator *vdd_i2c;
	struct regulator *vcc_io;

	int irq;
	int irq_gpio;
	int reset_gpio;
	int test_gpio;

	bool system_suspend;
	bool power_key_triggered;

	u8 irq_trigger_type;

	bool is_touched;
	bool touch_key_hold_press;
	int touch_flag[40];

#if 0
	struct notifier_block fb_notif;
#elif defined(CONFIG_HAS_EARLYSUSPEND)
	struct early_suspend early_suspend;
#endif

	struct task_struct *update_thread;

	atomic_t firmware_updating;
	bool operation_protection;
	bool unhandle_irq;
	unsigned int irq_handle_type;
	unsigned int irq_read_len;

	u8 gesture_status;
	u8 low_power_status;

	bool esd_check;
	bool esd_skip;
	struct workqueue_struct *esd_workq;
	struct delayed_work esd_work;
	unsigned long esd_delay;

	/* Mutex for protecting concurrent access */
	struct mutex ilitek_mutex;

	atomic_t irq_enabled;
	atomic_t get_INT;

	bool wake_irq_enabled;

	bool irq_registered;
};

/* Extern macro -------------------------------------------------------------*/

#define CEIL(n, d) (((n) % (d)) ? ((n) / (d)) + 1 : ((n) / (d)))

/* Extern variables ---------------------------------------------------------*/

extern u8 driver_ver[];

extern struct ilitek_ts_data *ts;

#ifdef ILITEK_TUNING_MESSAGE
extern bool ilitek_debug_flag;
#endif
/* Extern function prototypes -----------------------------------------------*/
/* Extern functions ---------------------------------------------------------*/
void ilitek_resume(void);
void ilitek_suspend(void);
int ilitek_main_probe(void *client, struct device *dev);
int ilitek_main_remove(void *client);
void ilitek_reset(int delay);

int ilitek_write(u8 *cmd, int len);
int ilitek_read(u8 *buf, int len);
int ilitek_write_and_read(u8 *cmd, int w_len, int delay_ms,
			  u8 *buf, int r_len);

void ilitek_irq_enable(void);
void ilitek_irq_disable(void);

int ilitek_upgrade_firmware(char *filename);

int ilitek_create_tool_node(void);
int ilitek_remove_tool_node(void);

int ilitek_create_sysfsnode(void);
void ilitek_remove_sys_node(void);

int ilitek_netlink_init(u8 unit);
void ilitek_netlink_exit(void);

void ilitek_gpio_dbg(void);

void ilitek_register_gesture(struct ilitek_ts_data *ts, bool init);
void __maybe_unused ilitek_gesture_handle(bool touch, int idx, int x, int y);

int ilitek_create_esd_check_workqueue(void);
void ilitek_remove_esd_check_workqueue(void);

int ilitek_read_data_and_report(void);

#endif
