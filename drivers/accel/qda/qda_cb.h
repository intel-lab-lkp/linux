/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __QDA_CB_H__
#define __QDA_CB_H__

#include <linux/device.h>
#include <linux/of.h>
#include <linux/list.h>
#include <linux/qda_compute_bus.h>
#include "qda_drv.h"

struct qda_cb_dev {
	struct list_head node;
	struct device *dev;
};

/*
 * Compute bus (CB) device management
 */
int qda_create_cb_device(struct qda_dev *qdev, struct device_node *cb_node);
void qda_destroy_cb_device(struct device *cb_dev);

#endif /* __QDA_CB_H__ */
