/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __QDA_CB_H__
#define __QDA_CB_H__

#include <linux/device.h>
#include <linux/list.h>
#include <linux/of.h>
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

/*
 * Transport-agnostic CB device population/teardown.
 * Called by any transport layer (RPMsg, etc.) during probe/remove.
 */
int qda_cb_populate(struct qda_dev *qdev, struct device_node *parent_node);
void qda_cb_unpopulate(struct qda_dev *qdev);

#endif /* __QDA_CB_H__ */
