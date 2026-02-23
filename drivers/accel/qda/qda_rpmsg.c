// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#include <linux/module.h>
#include <linux/rpmsg.h>
#include <linux/of_platform.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/completion.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include "qda_drv.h"
#include "qda_fastrpc.h"
#include "qda_rpmsg.h"
#include "qda_cb.h"

static int qda_rpmsg_init(struct qda_dev *qdev)
{
	dev_set_drvdata(&qdev->rpdev->dev, qdev);
	return 0;
}

static int validate_device_availability(struct qda_dev *qdev)
{
	struct rpmsg_device *rpdev;

	if (!qdev)
		return -ENODEV;

	if (atomic_read(&qdev->removing)) {
		qda_dbg(qdev, "RPMsg device unavailable: removing\n");
		return -ENODEV;
	}

	mutex_lock(&qdev->lock);
	rpdev = qdev->rpdev;
	mutex_unlock(&qdev->lock);

	if (!rpdev) {
		qda_dbg(qdev, "RPMsg device unavailable: rpdev is NULL\n");
		return -ENODEV;
	}

	return 0;
}

static struct fastrpc_invoke_context *get_and_validate_context(struct qda_msg *msg,
							       struct qda_dev *qdev)
{
	struct fastrpc_invoke_context *ctx = msg->fastrpc_ctx;

	if (!ctx) {
		qda_dbg(qdev, "FastRPC context not found in message\n");
		return ERR_PTR(-EINVAL);
	}

	kref_get(&ctx->refcount);
	return ctx;
}

static void populate_fastrpc_msg(struct fastrpc_msg *dst, struct qda_msg *src)
{
	dst->client_id = src->client_id;
	dst->tid = src->tid;
	dst->ctx = src->ctx;
	dst->handle = src->handle;
	dst->sc = src->sc;
	dst->addr = src->addr;
	dst->size = src->size;
}

static int validate_callback_params(struct qda_dev *qdev, void *data, int len)
{
	if (!qdev)
		return -ENODEV;

	if (atomic_read(&qdev->removing))
		return -ENODEV;

	if (len < sizeof(struct qda_invoke_rsp)) {
		qda_dbg(qdev, "Invalid message size from remote: %d\n", len);
		return -EINVAL;
	}

	return 0;
}

static unsigned long extract_context_id(struct qda_invoke_rsp *resp_msg)
{
	return (resp_msg->ctx & 0xFF0) >> 4;
}

static struct fastrpc_invoke_context *find_context_by_id(struct qda_dev *qdev,
							 unsigned long ctxid)
{
	struct fastrpc_invoke_context *ctx;

	{
		unsigned long flags;

		xa_lock_irqsave(&qdev->ctx_xa, flags);
		ctx = xa_load(&qdev->ctx_xa, ctxid);
		xa_unlock_irqrestore(&qdev->ctx_xa, flags);
	}

	if (!ctx) {
		qda_dbg(qdev, "FastRPC context not found for ctxid: %lu\n", ctxid);
		return ERR_PTR(-ENOENT);
	}

	return ctx;
}

static void complete_context_processing(struct fastrpc_invoke_context *ctx, int retval)
{
	ctx->retval = retval;
	complete(&ctx->work);
	kref_put(&ctx->refcount, fastrpc_context_free);
}

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

int qda_rpmsg_send_msg(struct qda_dev *qdev, struct qda_msg *msg)
{
	int ret;
	struct fastrpc_invoke_context *ctx;
	struct fastrpc_msg msg1;
	struct rpmsg_device *rpdev;

	ret = validate_device_availability(qdev);
	if (ret)
		return ret;

	ctx = get_and_validate_context(msg, qdev);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	populate_fastrpc_msg(&msg1, msg);

	mutex_lock(&qdev->lock);
	rpdev = qdev->rpdev;
	if (!rpdev) {
		mutex_unlock(&qdev->lock);
		kref_put(&ctx->refcount, fastrpc_context_free);
		return -ENODEV;
	}

	ret = rpmsg_send(rpdev->ept, (void *)&msg1, sizeof(msg1));
	mutex_unlock(&qdev->lock);

	if (ret) {
		qda_err(qdev, "rpmsg_send failed: %d\n", ret);
		kref_put(&ctx->refcount, fastrpc_context_free);
		return ret;
	}

	return 0;
}

int qda_rpmsg_wait_for_rsp(struct fastrpc_invoke_context *ctx)
{
	return wait_for_completion_interruptible(&ctx->work);
}

static int qda_rpmsg_cb(struct rpmsg_device *rpdev, void *data, int len, void *priv, u32 src)
{
	struct qda_dev *qdev = dev_get_drvdata(&rpdev->dev);
	struct qda_invoke_rsp *resp_msg = (struct qda_invoke_rsp *)data;
	struct fastrpc_invoke_context *ctx;
	unsigned long ctxid;
	int ret;

	ret = validate_callback_params(qdev, data, len);
	if (ret)
		return ret;

	ctxid = extract_context_id(resp_msg);

	ctx = find_context_by_id(qdev, ctxid);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	complete_context_processing(ctx, resp_msg->retval);

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

	qda_unregister_device(qdev);
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

	ret = qda_register_device(qdev);
	if (ret) {
		qda_deinit_device(qdev);
		qda_unpopulate_child_devices(qdev);
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
