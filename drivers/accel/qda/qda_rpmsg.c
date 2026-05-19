// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#include <linux/module.h>
#include <linux/of.h>
#include <linux/rpmsg.h>
#include <drm/drm_print.h>

#include "qda_drv.h"
#include "qda_rpmsg.h"

static struct qda_dev *alloc_and_init_qdev(struct rpmsg_device *rpdev)
{
	struct qda_dev *qdev;

	qdev = qda_alloc_device(&rpdev->dev);
	if (IS_ERR(qdev))
		return qdev;

	qdev->dev = &rpdev->dev;
	qdev->rpdev = rpdev;
	dev_set_drvdata(&rpdev->dev, qdev);

	return qdev;
}

static int qda_rpmsg_cb(struct rpmsg_device *rpdev, void *data, int len,
			void *priv, u32 src)
{
	/* Placeholder: responses will be dispatched here */
	return 0;
}

static void qda_rpmsg_remove(struct rpmsg_device *rpdev)
{
	struct qda_dev *qdev = dev_get_drvdata(&rpdev->dev);

	drm_dev_unplug(&qdev->drm_dev);
	qdev->rpdev = NULL;
	qda_unregister_device(qdev);
	dev_info(qdev->dev, "RPMsg device removed\n");
}

static int qda_rpmsg_probe(struct rpmsg_device *rpdev)
{
	struct qda_dev *qdev;
	const char *label;
	int ret;

	dev_dbg(&rpdev->dev, "QDA RPMsg probe starting\n");

	qdev = alloc_and_init_qdev(rpdev);
	if (IS_ERR(qdev))
		return PTR_ERR(qdev);

	ret = of_property_read_string(rpdev->dev.of_node, "label", &label);
	if (ret) {
		dev_err(qdev->dev, "Missing 'label' property in DT node: %d\n", ret);
		return ret;
	}
	qdev->dsp_name = label;

	ret = qda_register_device(qdev);
	if (ret)
		return ret;

	drm_info(&qdev->drm_dev, "QDA RPMsg probe complete for %s\n", qdev->dsp_name);
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
		pr_err("qda: Failed to register RPMsg driver: %d\n", ret);

	return ret;
}

void qda_rpmsg_unregister(void)
{
	unregister_rpmsg_driver(&qda_rpmsg_driver);
}
