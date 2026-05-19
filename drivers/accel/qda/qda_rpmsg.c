// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#include <linux/completion.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/rpmsg.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <drm/drm_print.h>

#include "qda_cb.h"
#include "qda_drv.h"
#include "qda_fastrpc.h"
#include "qda_rpmsg.h"

static int validate_device_availability(struct qda_dev *qdev)
{
	if (!qdev)
		return -ENODEV;

	if (!qdev->rpdev) {
		drm_dbg_driver(&qdev->drm_dev, "RPMsg device unavailable: rpdev is NULL\n");
		return -ENODEV;
	}
	return 0;
}

static struct fastrpc_invoke_context *get_and_validate_context(struct qda_msg *msg,
							       struct qda_dev *qdev)
{
	struct fastrpc_invoke_context *ctx = msg->fastrpc_ctx;

	if (!ctx) {
		drm_dbg_driver(&qdev->drm_dev, "FastRPC context not found in message\n");
		return ERR_PTR(-EINVAL);
	}

	kref_get(&ctx->refcount);
	return ctx;
}

static int validate_callback_params(struct qda_dev *qdev, void *data, int len)
{
	if (!qdev)
		return -ENODEV;

	if (len < sizeof(struct qda_invoke_rsp)) {
		drm_dbg_driver(&qdev->drm_dev, "Invalid message size from remote: %d\n", len);
		return -EINVAL;
	}
	return 0;
}

static unsigned long extract_context_id(struct qda_invoke_rsp *resp_msg)
{
	return resp_msg->ctx >> 4;
}

static struct fastrpc_invoke_context *find_context_by_id(struct qda_dev *qdev,
							 unsigned long ctxid)
{
	struct fastrpc_invoke_context *ctx;

	ctx = xa_load(&qdev->ctx_xa, ctxid);
	if (!ctx) {
		drm_dbg_driver(&qdev->drm_dev, "FastRPC context not found for ctxid: %lu\n", ctxid);
		return ERR_PTR(-ENOENT);
	}
	return ctx;
}

static void complete_context_processing(struct fastrpc_invoke_context *ctx, int retval)
{
	ctx->retval = retval;
	complete(&ctx->work);
	kref_put(&ctx->refcount, qda_fastrpc_context_free);
}

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

int qda_rpmsg_send_msg(struct qda_dev *qdev, struct qda_msg *msg)
{
	int ret, idx;
	struct fastrpc_invoke_context *ctx;

	if (!qdev)
		return -ENODEV;

	if (!drm_dev_enter(&qdev->drm_dev, &idx))
		return -ENODEV;

	ret = validate_device_availability(qdev);
	if (ret)
		goto out_exit;

	ctx = get_and_validate_context(msg, qdev);
	if (IS_ERR(ctx)) {
		ret = PTR_ERR(ctx);
		goto out_exit;
	}

	ret = rpmsg_send(qdev->rpdev->ept, &msg->fastrpc, sizeof(msg->fastrpc));
	if (ret) {
		drm_err(&qdev->drm_dev, "rpmsg_send failed: %d\n", ret);
		kref_put(&ctx->refcount, qda_fastrpc_context_free);
	}

out_exit:
	drm_dev_exit(idx);
	return ret;
}

int qda_rpmsg_wait_for_rsp(struct fastrpc_invoke_context *ctx)
{
	return wait_for_completion_interruptible(&ctx->work);
}

static int qda_rpmsg_cb(struct rpmsg_device *rpdev, void *data, int len,
			void *priv, u32 src)
{
	struct qda_dev *qdev = dev_get_drvdata(&rpdev->dev);
	struct qda_invoke_rsp *resp_msg = (struct qda_invoke_rsp *)data;
	struct fastrpc_invoke_context *ctx;
	unsigned long ctxid;
	int ret, idx;

	if (!qdev)
		return -ENODEV;

	if (!drm_dev_enter(&qdev->drm_dev, &idx))
		return -ENODEV;

	ret = validate_callback_params(qdev, data, len);
	if (ret)
		goto out_exit;

	ctxid = extract_context_id(resp_msg);

	ctx = find_context_by_id(qdev, ctxid);
	if (IS_ERR(ctx)) {
		ret = PTR_ERR(ctx);
		goto out_exit;
	}

	complete_context_processing(ctx, resp_msg->retval);
	ret = 0;

out_exit:
	drm_dev_exit(idx);
	return ret;
}

static void qda_rpmsg_remove(struct rpmsg_device *rpdev)
{
	struct qda_dev *qdev = dev_get_drvdata(&rpdev->dev);

	qda_cb_unpopulate(qdev);
	drm_dev_unplug(&qdev->drm_dev);
	qdev->rpdev = NULL;
	qda_unregister_device(qdev);
	qda_deinit_device(qdev);
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

	ret = qda_init_device(qdev);
	if (ret)
		return ret;

	ret = qda_cb_populate(qdev, rpdev->dev.of_node);
	if (ret) {
		dev_err(qdev->dev, "Failed to populate child devices: %d\n", ret);
		qda_deinit_device(qdev);
		return ret;
	}

	ret = qda_register_device(qdev);
	if (ret) {
		qda_deinit_device(qdev);
		qda_cb_unpopulate(qdev);
		return ret;
	}

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
