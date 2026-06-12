// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/interconnect.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>

#include <media/v4l2-mem2mem.h>

#include "qcom_jenc_dev.h"

#include "qcom_jenc_defs.h"
#include "qcom_jenc_ops.h"
#include "qcom_jenc_res.h"
#include "qcom_jenc_v4l2.h"

static int qcom_jpeg_clk_init(struct qcom_jenc_dev *jenc)
{
	const struct qcom_dev_resources *res = jenc->res;
	int c_idx;

	jenc->clks = devm_kcalloc(jenc->dev, ARRAY_SIZE(res->clk_names), sizeof(*jenc->clks),
				  GFP_KERNEL);
	if (!jenc->clks)
		return -ENOMEM;

	for (c_idx = 0; c_idx < ARRAY_SIZE(res->clk_names); c_idx++) {
		if (!res->clk_names[c_idx])
			break;

		jenc->clks[c_idx].id = res->clk_names[c_idx];
		jenc->num_clks++;
	}

	if (!jenc->num_clks)
		return -EINVAL;

	return devm_clk_bulk_get(jenc->dev, jenc->num_clks, jenc->clks);
}

static int qcom_jpeg_clk_rate(struct qcom_jenc_dev *jenc, enum qcom_soc_perf_level level)
{
	const struct qcom_dev_resources	*res = jenc->res;
	const struct qcom_perf_resource	*perf = &res->perf_cfg[level];
	int c_idx;
	int rc = 0;

	for (c_idx = 0; c_idx < jenc->num_clks; c_idx++) {
		/* skip clocks with fixed or default frequency */
		if (!perf->clk_rate[c_idx])
			continue;

		/* setup frequency according to performance level */
		rc = clk_set_rate(jenc->clks[c_idx].clk, perf->clk_rate[c_idx]);
		if (rc < 0) {
			dev_err(jenc->dev, "clock set rate failed: %d\n", rc);
			return rc;
		}

		dev_dbg(jenc->dev, "clock %s current rate: %ld\n",
			jenc->clks[c_idx].id, clk_get_rate(jenc->clks[c_idx].clk));
	}

	return rc;
}

static int qcom_jpeg_clk_on(struct qcom_jenc_dev *jenc)
{
	int rc;

	rc = qcom_jpeg_clk_rate(jenc, jenc->perf);
	if (rc)
		return rc;

	rc = clk_bulk_prepare_enable(jenc->num_clks, jenc->clks);
	if (rc)
		return rc;

	return 0;
}

static void qcom_jpeg_clk_off(struct qcom_jenc_dev *jenc)
{
	clk_bulk_disable_unprepare(jenc->num_clks, jenc->clks);
}

static int qcom_jpeg_icc_on(struct qcom_jenc_dev *jenc)
{
	const struct qcom_dev_resources	*res = jenc->res;
	int p_idx;
	int rc;

	for (p_idx = 0; p_idx < res->num_of_icc; p_idx++) {
		rc = icc_set_bw(jenc->icc_paths[p_idx], res->icc_res[p_idx].pair.aggr,
				res->icc_res[p_idx].pair.peak);
		if (rc) {
			dev_err(jenc->dev, "%s failed for path %s: %d\n", __func__,
				res->icc_res[p_idx].icc_id, rc);
			goto err_icc_set_bw;
		}
	}

	return 0;

err_icc_set_bw:
	while (--p_idx >= 0)
		icc_set_bw(jenc->icc_paths[p_idx], 0, 0);

	return rc;
}

static void qcom_jpeg_icc_off(struct qcom_jenc_dev *jenc)
{
	const struct qcom_dev_resources	*res = jenc->res;
	int p_idx;

	for (p_idx = 0; p_idx < res->num_of_icc; p_idx++)
		icc_set_bw(jenc->icc_paths[p_idx], 0, 0);
}

static int qcom_jpeg_icc_init(struct qcom_jenc_dev *jenc)
{
	const struct qcom_dev_resources	*res = jenc->res;
	int p_idx;

	jenc->icc_paths = devm_kcalloc(jenc->dev, res->num_of_icc, sizeof(*jenc->icc_paths),
				       GFP_KERNEL);
	if (!jenc->icc_paths)
		return -ENOMEM;

	for (p_idx = 0; p_idx < res->num_of_icc; p_idx++) {
		jenc->icc_paths[p_idx] = devm_of_icc_get(jenc->dev, res->icc_res[p_idx].icc_id);
		if (IS_ERR(jenc->icc_paths[p_idx])) {
			return dev_err_probe(jenc->dev, PTR_ERR(jenc->icc_paths[p_idx]),
					     "failed to get ICC path: %pe\n",
					     jenc->icc_paths[p_idx]);
		}
	}

	return 0;
}

static __maybe_unused int qcom_jpeg_pm_suspend(struct device *dev)
{
	struct qcom_jenc_dev *jenc = dev_get_drvdata(dev);

	qcom_jpeg_clk_off(jenc);

	qcom_jpeg_icc_off(jenc);

	return 0;
}

static __maybe_unused int qcom_jpeg_pm_resume(struct device *dev)
{
	struct qcom_jenc_dev *jenc = dev_get_drvdata(dev);
	int rc;

	rc = qcom_jpeg_icc_on(jenc);
	if (rc)
		return rc;

	rc = qcom_jpeg_clk_on(jenc);
	if (rc) {
		qcom_jpeg_icc_off(jenc);
		return rc;
	}

	return 0;
}

static __maybe_unused int qcom_jpeg_suspend(struct device *dev)
{
	struct qcom_jenc_dev *jenc = dev_get_drvdata(dev);
	int rc;

	v4l2_m2m_suspend(jenc->m2m_dev);

	rc = pm_runtime_force_suspend(dev);
	if (rc)
		v4l2_m2m_resume(jenc->m2m_dev);

	return rc;
}

static __maybe_unused int qcom_jpeg_resume(struct device *dev)
{
	struct qcom_jenc_dev *jenc = dev_get_drvdata(dev);
	int rc;

	rc = pm_runtime_force_resume(dev);
	if (rc)
		return rc;

	v4l2_m2m_resume(jenc->m2m_dev);

	return 0;
}

static const struct dev_pm_ops qcom_jpeg_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(qcom_jpeg_suspend, qcom_jpeg_resume)
	SET_RUNTIME_PM_OPS(qcom_jpeg_pm_suspend, qcom_jpeg_pm_resume, NULL)
};

static int qcom_jpeg_probe(struct platform_device *pdev)
{
	const struct qcom_dev_resources *res;
	struct qcom_jenc_dev *jenc;
	int rc;

	jenc = devm_kzalloc(&pdev->dev, sizeof(*jenc), GFP_KERNEL);
	if (!jenc)
		return -ENOMEM;

	jenc->dev = &pdev->dev;
	mutex_init(&jenc->dev_mutex);
	spin_lock_init(&jenc->hw_lock);
	init_completion(&jenc->reset_complete);
	init_completion(&jenc->stop_complete);

	res = device_get_match_data(jenc->dev);
	if (!res)
		return dev_err_probe(jenc->dev, -ENODEV, "unsupported SoC\n");
	jenc->res = res;

	if (!jenc->res->hw_offs || !jenc->res->hw_ops)
		return dev_err_probe(jenc->dev, -EINVAL, "missing hw resources\n");

	rc = dma_set_mask_and_coherent(jenc->dev, DMA_BIT_MASK(32));
	if (rc)
		return dev_err_probe(jenc->dev, rc, "failed to set DMA mask\n");

	jenc->jpeg_base = devm_platform_ioremap_resource_byname(pdev, "jpeg");
	if (IS_ERR(jenc->jpeg_base))
		return dev_err_probe(jenc->dev, PTR_ERR(jenc->jpeg_base),
				     "failed to map JPEG resource\n");

	rc = qcom_jpeg_clk_init(jenc);
	if (rc)
		return rc;

	jenc->irq = platform_get_irq(pdev, 0);
	if (jenc->irq < 0)
		return dev_err_probe(jenc->dev, jenc->irq, "failed to get IRQ\n");

	rc = devm_request_threaded_irq(jenc->dev, jenc->irq,
				       jenc->res->hw_ops->hw_irq_top,
				       jenc->res->hw_ops->hw_irq_bot,
				       IRQF_ONESHOT, dev_name(jenc->dev), jenc);
	if (rc)
		return dev_err_probe(jenc->dev, rc, "failed to request IRQ\n");

	rc = qcom_jpeg_icc_init(jenc);
	if (rc)
		return rc;

	rc = v4l2_device_register(jenc->dev, &jenc->v4l2_dev);
	if (rc) {
		dev_err(jenc->dev, "failed to register V4L2 device\n");
		return rc;
	}

	jenc->perf = QCOM_SOC_PERF_NOMINAL;

	rc = qcom_jpeg_v4l2_register(jenc);
	if (rc) {
		dev_err(jenc->dev, "failed to register video device\n");
		goto err_v4l2_device_unregister;
	}

	rc = devm_pm_runtime_enable(jenc->dev);
	if (rc)
		goto err_v4l2_unregister;

	dev_dbg(jenc->dev, "Qualcomm JPEG encoder registered\n");

	platform_set_drvdata(pdev, jenc);

	return 0;

err_v4l2_unregister:
	qcom_jpeg_v4l2_unregister(jenc);
err_v4l2_device_unregister:
	v4l2_device_unregister(&jenc->v4l2_dev);
	return rc;
}

static void qcom_jpeg_remove(struct platform_device *pdev)
{
	struct qcom_jenc_dev *jenc = platform_get_drvdata(pdev);

	qcom_jpeg_v4l2_unregister(jenc);

	v4l2_device_unregister(&jenc->v4l2_dev);

	dev_dbg(jenc->dev, "Qualcomm JPEG encoder deregistered\n");
}

static const struct of_device_id qcom_jpeg_of_match[] = {
	{
		.compatible	= "qcom,sm8250-jenc",
		.data		= &qcom_t165_t480_jpeg_drvdata
	},
	{
		.compatible	= "qcom,qcm6490-jenc",
		.data		= &qcom_t680_jpeg_drvdata
	},
	{
		.compatible	= "qcom,sm8550-jenc",
		.data		= &qcom_t780_jpeg_drvdata
	},
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_jpeg_of_match);

static struct platform_driver qcom_jpeg_platform_driver = {
	.probe  = qcom_jpeg_probe,
	.remove = qcom_jpeg_remove,
	.driver = {
		.name = QCOM_JPEG_ENC_NAME,
		.of_match_table = qcom_jpeg_of_match,
		.pm             = &qcom_jpeg_pm_ops,
	},
};

module_platform_driver(qcom_jpeg_platform_driver);

MODULE_DESCRIPTION("QCOM JPEG mem2mem V4L2 encoder");
MODULE_LICENSE("GPL");
