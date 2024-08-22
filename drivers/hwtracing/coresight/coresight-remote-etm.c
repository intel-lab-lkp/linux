// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/coresight.h>
#include "coresight-qmi.h"
#include "coresight-priv.h"

DEFINE_CORESIGHT_DEVLIST(remote_etm_devs, "remote-etm");

#define MAX_INSTANCE_ID		0xFF

struct remote_etm_drvdata {
	struct device			*dev;
	struct coresight_device		*csdev;
	struct mutex			mutex;
	struct qmi_handle		handle;
	u32				qmi_id;
	bool				enabled;
	bool				service_connected;
	struct sockaddr_qrtr		s_addr;
};

static int service_remote_etm_new_server(struct qmi_handle *qmi,
		struct qmi_service *svc)
{
	struct remote_etm_drvdata *drvdata = container_of(qmi,
					struct remote_etm_drvdata, handle);

	drvdata->s_addr.sq_family = AF_QIPCRTR;
	drvdata->s_addr.sq_node = svc->node;
	drvdata->s_addr.sq_port = svc->port;
	drvdata->service_connected = true;
	dev_info(drvdata->dev,
		"Connection established between QMI handle and %d service\n",
		drvdata->qmi_id);

	return 0;
}

static void service_remote_etm_del_server(struct qmi_handle *qmi,
		struct qmi_service *svc)
{
	struct remote_etm_drvdata *drvdata = container_of(qmi,
					struct remote_etm_drvdata, handle);
	drvdata->service_connected = false;
	dev_info(drvdata->dev,
		"Connection disconnected between QMI handle and %d service\n",
		drvdata->qmi_id);
}

static struct qmi_ops server_ops = {
	.new_server = service_remote_etm_new_server,
	.del_server = service_remote_etm_del_server,
};

static int remote_etm_enable(struct coresight_device *csdev,
			     struct perf_event *event, u32 mode)
{
	struct remote_etm_drvdata *drvdata =
		dev_get_drvdata(csdev->dev.parent);
	struct coresight_set_etm_req_msg_v01 req;
	struct coresight_set_etm_resp_msg_v01 resp = { { 0, 0 } };
	struct qmi_txn txn;
	int ret;

	mutex_lock(&drvdata->mutex);

	if (!drvdata->service_connected) {
		dev_err(drvdata->dev, "QMI service not connected!\n");
		ret = -EINVAL;
		goto err;
	}
	/*
	 * The QMI handle may be NULL in the following scenarios:
	 * 1. QMI service is not present
	 * 2. QMI service is present but attempt to enable remote ETM is earlier
	 *    than service is ready to handle request
	 * 3. Connection between QMI client and QMI service failed
	 *
	 * Enable CoreSight without processing further QMI commands which
	 * provides the option to enable remote ETM by other means.
	 */
	req.state = CORESIGHT_ETM_STATE_ENABLED_V01;

	ret = qmi_txn_init(&drvdata->handle, &txn,
			coresight_set_etm_resp_msg_v01_ei,
			&resp);

	if (ret < 0) {
		dev_err(drvdata->dev, "QMI tx init failed , ret:%d\n",
				ret);
		goto err;
	}

	ret = qmi_send_request(&drvdata->handle, &drvdata->s_addr,
			&txn, CORESIGHT_QMI_SET_ETM_REQ_V01,
			CORESIGHT_QMI_SET_ETM_REQ_MAX_LEN,
			coresight_set_etm_req_msg_v01_ei,
			&req);
	if (ret < 0) {
		dev_err(drvdata->dev, "QMI send ACK failed, ret:%d\n",
				ret);
		qmi_txn_cancel(&txn);
		goto err;
	}

	ret = qmi_txn_wait(&txn, msecs_to_jiffies(TIMEOUT_MS));
	if (ret < 0) {
		dev_err(drvdata->dev, "QMI qmi txn wait failed, ret:%d\n",
				ret);
		goto err;
	}

	/* Check the response */
	if (resp.resp.result != QMI_RESULT_SUCCESS_V01)
		dev_err(drvdata->dev, "QMI request failed 0x%x\n",
				resp.resp.error);

	drvdata->enabled = true;
	mutex_unlock(&drvdata->mutex);

	dev_dbg(drvdata->dev, "Remote ETM tracing enabled for instance %d\n",
				drvdata->qmi_id);
	return 0;
err:
	mutex_unlock(&drvdata->mutex);
	return ret;
}

static void remote_etm_disable(struct coresight_device *csdev,
			       struct perf_event *event)
{
	struct remote_etm_drvdata *drvdata =
		 dev_get_drvdata(csdev->dev.parent);
	struct coresight_set_etm_req_msg_v01 req;
	struct coresight_set_etm_resp_msg_v01 resp = { { 0, 0 } };
	struct qmi_txn txn;
	int ret;

	mutex_lock(&drvdata->mutex);
	if (!drvdata->service_connected) {
		dev_err(drvdata->dev, "QMI service not connected!\n");
		goto err;
	}

	req.state = CORESIGHT_ETM_STATE_DISABLED_V01;

	ret = qmi_txn_init(&drvdata->handle, &txn,
			coresight_set_etm_resp_msg_v01_ei,
			&resp);

	if (ret < 0) {
		dev_err(drvdata->dev, "QMI tx init failed , ret:%d\n",
				ret);
		goto err;
	}

	ret = qmi_send_request(&drvdata->handle, &drvdata->s_addr,
			&txn, CORESIGHT_QMI_SET_ETM_REQ_V01,
			CORESIGHT_QMI_SET_ETM_REQ_MAX_LEN,
			coresight_set_etm_req_msg_v01_ei,
			&req);
	if (ret < 0) {
		dev_err(drvdata->dev, "QMI send req failed, ret:%d\n",
				 ret);
		qmi_txn_cancel(&txn);
		goto err;
	}

	ret = qmi_txn_wait(&txn, msecs_to_jiffies(TIMEOUT_MS));
	if (ret < 0) {
		dev_err(drvdata->dev, "QMI qmi txn wait failed, ret:%d\n",
				ret);
		goto err;
	}

	/* Check the response */
	if (resp.resp.result != QMI_RESULT_SUCCESS_V01) {
		dev_err(drvdata->dev, "QMI request failed 0x%x\n",
				resp.resp.error);
		goto err;
	}

	drvdata->enabled = false;
	dev_info(drvdata->dev, "Remote ETM tracing disabled for instance %d\n",
				drvdata->qmi_id);
err:
	mutex_unlock(&drvdata->mutex);
}

static const struct coresight_ops_source remote_etm_source_ops = {
	.enable		= remote_etm_enable,
	.disable	= remote_etm_disable,
};

static const struct coresight_ops remote_cs_ops = {
	.source_ops	= &remote_etm_source_ops,
};

static int remote_etm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct coresight_platform_data *pdata;
	struct remote_etm_drvdata *drvdata;
	struct coresight_desc desc = {0 };
	int ret;

	desc.name = coresight_alloc_device_name(&remote_etm_devs, dev);
	if (!desc.name)
		return -ENOMEM;
	pdata = coresight_get_platform_data(dev);
	if (IS_ERR(pdata))
		return PTR_ERR(pdata);
	pdev->dev.platform_data = pdata;

	pm_runtime_enable(dev);
	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->dev = &pdev->dev;
	platform_set_drvdata(pdev, drvdata);

	ret = of_property_read_u32(pdev->dev.of_node, "qcom,qmi-id",
			&drvdata->qmi_id);
	if (ret)
		return ret;

	if (drvdata->qmi_id >= MAX_INSTANCE_ID) {
		dev_err(dev, "qmi_id is invalid.\n");
		return -EINVAL;
	}

	mutex_init(&drvdata->mutex);

	ret = qmi_handle_init(&drvdata->handle,
			CORESIGHT_QMI_SET_ETM_REQ_MAX_LEN,
			&server_ops, NULL);
	if (ret < 0) {
		dev_err_probe(dev, ret, "Remote ETM client init failed.\n");
		return ret;
	}

	qmi_add_lookup(&drvdata->handle,
			CORESIGHT_QMI_SVC_ID,
			CORESIGHT_QMI_VERSION,
			drvdata->qmi_id);

	desc.type = CORESIGHT_DEV_TYPE_SOURCE;
	desc.subtype.source_subtype = CORESIGHT_DEV_SUBTYPE_SOURCE_OTHERS;
	desc.ops = &remote_cs_ops;
	desc.pdata = pdev->dev.platform_data;
	desc.dev = &pdev->dev;
	drvdata->csdev = coresight_register(&desc);
	if (IS_ERR(drvdata->csdev)) {
		ret = PTR_ERR(drvdata->csdev);
		goto err;
	}

	return 0;
err:
	qmi_handle_release(&drvdata->handle);
	return ret;
}

static void remote_etm_remove(struct platform_device *pdev)
{
	struct remote_etm_drvdata *drvdata = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;

	pm_runtime_disable(dev);
	qmi_handle_release(&drvdata->handle);
	coresight_unregister(drvdata->csdev);
}

static const struct of_device_id remote_etm_match[] = {
	{.compatible = "qcom,coresight-remote-etm"},
	{}
};

static struct platform_driver remote_etm_driver = {
	.probe          = remote_etm_probe,
	.remove         = remote_etm_remove,
	.driver         = {
		.name   = "coresight-remote-etm",
		.of_match_table = remote_etm_match,
	},
};

module_platform_driver(remote_etm_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CoreSight Remote ETM driver");
