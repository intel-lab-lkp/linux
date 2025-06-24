// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/coresight.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/of_irq.h>
#include <linux/uaccess.h>

#include "coresight-ctcu.h"
#include "coresight-priv.h"
#include "coresight-tmc.h"

static irqreturn_t byte_cntr_handler(int irq, void *data)
{
	struct ctcu_byte_cntr *byte_cntr_data = (struct ctcu_byte_cntr *)data;

	atomic_inc(&byte_cntr_data->irq_cnt);
	wake_up(&byte_cntr_data->wq);

	byte_cntr_data->irq_num++;

	return IRQ_HANDLED;
}

/* Start the byte-cntr function when the path is enabled. */
void ctcu_byte_cntr_start(struct coresight_device *csdev, struct coresight_path *path)
{
	struct ctcu_drvdata *drvdata = dev_get_drvdata(csdev->dev.parent);
	struct coresight_device *sink = coresight_get_sink(path);
	struct ctcu_byte_cntr *byte_cntr_data;
	int port_num;

	if (!sink)
		return;

	port_num = coresight_get_port_helper(sink, csdev);
	if (port_num < 0)
		return;

	byte_cntr_data = &drvdata->byte_cntr_data[port_num];
	/* Don't start byte-cntr function when threshold is not set. */
	if (!byte_cntr_data->thresh_val || byte_cntr_data->enable)
		return;

	guard(raw_spinlock_irqsave)(&byte_cntr_data->spin_lock);
	byte_cntr_data->enable = true;
	byte_cntr_data->reading_buf = false;
}

/* Stop the byte-cntr function when the path is disabled. */
void ctcu_byte_cntr_stop(struct coresight_device *csdev, struct coresight_path *path)
{
	struct ctcu_drvdata *drvdata = dev_get_drvdata(csdev->dev.parent);
	struct coresight_device *sink = coresight_get_sink(path);
	struct ctcu_byte_cntr *byte_cntr_data;
	int port_num;

	if (!sink || coresight_get_mode(sink) == CS_MODE_SYSFS)
		return;

	port_num = coresight_get_port_helper(sink, csdev);
	if (port_num < 0)
		return;

	byte_cntr_data = &drvdata->byte_cntr_data[port_num];
	guard(raw_spinlock_irqsave)(&byte_cntr_data->spin_lock);
	byte_cntr_data->enable = false;
}

void ctcu_byte_cntr_init(struct device *dev, struct ctcu_drvdata *drvdata, int etr_num)
{
	struct ctcu_byte_cntr *byte_cntr_data;
	struct device_node *nd = dev->of_node;
	int byte_cntr_irq, ret, i;

	for (i = 0; i < etr_num; i++) {
		byte_cntr_data = &drvdata->byte_cntr_data[i];
		byte_cntr_irq = of_irq_get_byname(nd, byte_cntr_data->irq_name);
		if (byte_cntr_irq < 0) {
			dev_err(dev, "Failed to get IRQ from DT for %s\n",
				byte_cntr_data->irq_name);
			continue;
		}

		ret = devm_request_irq(dev, byte_cntr_irq, byte_cntr_handler,
				       IRQF_TRIGGER_RISING | IRQF_SHARED,
				       dev_name(dev), byte_cntr_data);
		if (ret) {
			dev_err(dev, "Failed to register IRQ for %s\n",
				byte_cntr_data->irq_name);
			continue;
		}

		byte_cntr_data->byte_cntr_irq = byte_cntr_irq;
		disable_irq(byte_cntr_data->byte_cntr_irq);
		init_waitqueue_head(&byte_cntr_data->wq);
	}
}
