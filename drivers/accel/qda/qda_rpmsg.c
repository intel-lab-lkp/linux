// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#include <linux/module.h>
#include <linux/rpmsg.h>
#include <linux/of_platform.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include "qda_drv.h"
#include "qda_rpmsg.h"
#include "qda_cb.h"

static int qda_rpmsg_init(struct qda_dev *qdev)
{
	dev_set_drvdata(&qdev->rpdev->dev, qdev);
	return 0;
}

/* Utility function to allocate and initialize qda_dev */
static struct qda_dev *alloc_and_init_qdev(struct rpmsg_device *rpdev)
{
	struct qda_dev *qdev;

	qdev = devm_kzalloc(&rpdev->dev, sizeof(*qdev), GFP_KERNEL);
	if (!qdev)
		return ERR_PTR(-ENOMEM);

	qdev->dev = &rpdev->dev;
	qdev->rpdev = rpdev;
	INIT_LIST_HEAD(&qdev->cb_devs);

	qda_dbg(qdev, "Allocated and initialized qda_dev\n");
	return qdev;
}

static void qda_unpopulate_child_devices(struct qda_dev *qdev)
{
	struct qda_cb_dev *entry, *tmp;

	list_for_each_entry_safe(entry, tmp, &qdev->cb_devs, node) {
		list_del(&entry->node);
		qda_destroy_cb_device(entry->dev);
		kfree(entry);
	}
}

static int qda_populate_child_devices(struct qda_dev *qdev, struct device_node *parent_node)
{
	struct device_node *child;
	int count = 0, success = 0;

	for_each_child_of_node(parent_node, child) {
		if (of_device_is_compatible(child, "qcom,fastrpc-compute-cb")) {
			count++;
			if (qda_create_cb_device(qdev, child) == 0) {
				success++;
				qda_dbg(qdev, "Created CB device for node: %s\n", child->name);
			} else {
				qda_err(qdev, "Failed to create CB device for: %s\n", child->name);
			}
		}
	}
	return success > 0 ? 0 : (count > 0 ? -ENODEV : 0);
}

static int qda_rpmsg_cb(struct rpmsg_device *rpdev, void *data, int len, void *priv, u32 src)
{
	/* Dummy function for rpmsg driver */
	return 0;
}

static void qda_rpmsg_remove(struct rpmsg_device *rpdev)
{
	struct qda_dev *qdev = dev_get_drvdata(&rpdev->dev);

	qda_info(qdev, "Removing RPMsg device\n");

	atomic_set(&qdev->removing, 1);

	mutex_lock(&qdev->lock);
	qdev->rpdev = NULL;
	mutex_unlock(&qdev->lock);

	qda_unpopulate_child_devices(qdev);
	qda_deinit_device(qdev);

	qda_info(qdev, "RPMsg device removed\n");
}

static int qda_rpmsg_probe(struct rpmsg_device *rpdev)
{
	struct qda_dev *qdev;
	int ret;
	const char *label;

	qda_dbg(NULL, "QDA RPMsg probe starting\n");

	qdev = alloc_and_init_qdev(rpdev);
	if (IS_ERR(qdev))
		return PTR_ERR(qdev);

	ret = of_property_read_string(rpdev->dev.of_node, "label", &label);
	if (!ret) {
		strscpy(qdev->dsp_name, label, sizeof(qdev->dsp_name));
	} else {
		qda_info(qdev, "QDA DSP label not found in DT\n");
		return ret;
	}

	ret = qda_rpmsg_init(qdev);
	if (ret) {
		qda_err(qdev, "RPMsg init failed: %d\n", ret);
		return ret;
	}

	ret = qda_init_device(qdev);
	if (ret)
		return ret;

	ret = qda_populate_child_devices(qdev, rpdev->dev.of_node);
	if (ret) {
		qda_err(qdev, "Failed to populate child devices: %d\n", ret);
		qda_deinit_device(qdev);
		return ret;
	}

	qda_info(qdev, "QDA RPMsg probe completed successfully for %s\n", qdev->dsp_name);
	return 0;
}

static const struct of_device_id qda_rpmsg_id_table[] = {
	{ .compatible = "qcom,fastrpc" },
	{},
};
MODULE_DEVICE_TABLE(of, qda_rpmsg_id_table);

static struct rpmsg_driver qda_rpmsg_driver = {
	.probe = qda_rpmsg_probe,
	.remove = qda_rpmsg_remove,
	.callback = qda_rpmsg_cb,
	.drv = {
		.name = "qcom,fastrpc",
		.of_match_table = qda_rpmsg_id_table,
	},
};

int qda_rpmsg_register(void)
{
	int ret = register_rpmsg_driver(&qda_rpmsg_driver);

	if (ret)
		qda_err(NULL, "Failed to register RPMsg driver: %d\n", ret);

	return ret;
}

void qda_rpmsg_unregister(void)
{
	unregister_rpmsg_driver(&qda_rpmsg_driver);
}
