// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/atomic.h>
#include "qda_drv.h"
#include "qda_rpmsg.h"

static void cleanup_device_resources(struct qda_dev *qdev)
{
	mutex_destroy(&qdev->lock);
}

void qda_deinit_device(struct qda_dev *qdev)
{
	cleanup_device_resources(qdev);
}

/* Initialize device resources */
static void init_device_resources(struct qda_dev *qdev)
{
	qda_dbg(qdev, "Initializing device resources\n");

	mutex_init(&qdev->lock);
	atomic_set(&qdev->removing, 0);
}

int qda_init_device(struct qda_dev *qdev)
{
	init_device_resources(qdev);

	qda_dbg(qdev, "QDA device initialized successfully\n");
	return 0;
}

static int __init qda_core_init(void)
{
	int ret;

	ret = qda_rpmsg_register();
	if (ret)
		return ret;

	qda_info(NULL, "QDA driver initialization complete\n");
	return 0;
}

static void __exit qda_core_exit(void)
{
	qda_rpmsg_unregister();
	qda_info(NULL, "QDA driver exit complete\n");
}

module_init(qda_core_init);
module_exit(qda_core_exit);

MODULE_AUTHOR("Qualcomm AI Infra Team");
MODULE_DESCRIPTION("Qualcomm DSP Accelerator Driver");
MODULE_LICENSE("GPL");
