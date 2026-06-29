// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/clk.h>
#include <linux/interconnect.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
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
	int c_idx;

	jenc->num_clks = devm_clk_bulk_get_all(jenc->dev, &jenc->clks);
	if (jenc->num_clks < 0)
		return jenc->num_clks;

	for (c_idx = 0; c_idx < jenc->num_clks; c_idx++) {
		if (!strcmp(jenc->clks[c_idx].id, "jpeg")) {
			jenc->core_clk = jenc->clks[c_idx].clk;
			return 0;
		}
	}

	return -ENODEV;
}

static int qcom_jpeg_clk_on(struct qcom_jenc_dev *jenc)
{
	struct dev_pm_opp *opp;
	unsigned long freq;
	int rc;

	/* look up the OPP entry by level to obtain the core clock frequency */
	opp = dev_pm_opp_find_level_exact(jenc->dev, jenc->perf);
	if (IS_ERR(opp)) {
		dev_err(jenc->dev, "OPP not found for perf level %u: %pe\n", jenc->perf, opp);
		return PTR_ERR(opp);
	}
	freq = dev_pm_opp_get_freq(opp);
	dev_pm_opp_put(opp);

	rc = clk_set_rate(jenc->core_clk, freq);
	if (rc) {
		dev_err(jenc->dev, "core clock set rate failed: %d\n", rc);
		return rc;
	}

	rc = clk_bulk_prepare_enable(jenc->num_clks, jenc->clks);
	if (rc) {
		clk_set_rate(jenc->core_clk, 0);
		return rc;
	}

	dev_dbg(jenc->dev, "JPEG core clock: %ld\n", clk_get_rate(jenc->core_clk));

	return 0;
}

static void qcom_jpeg_clk_off(struct qcom_jenc_dev *jenc)
{
	clk_set_rate(jenc->core_clk, 0);
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
			dev_err(jenc->dev, "icc_set_bw failed for path %s: %d\n",
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

static int qcom_jpeg_pm_suspend(struct device *dev)
{
	struct qcom_jenc_dev *jenc = dev_get_drvdata(dev);

	qcom_jpeg_clk_off(jenc);

	qcom_jpeg_icc_off(jenc);

	return 0;
}

static int qcom_jpeg_pm_resume(struct device *dev)
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

static int qcom_jpeg_suspend(struct device *dev)
{
	struct qcom_jenc_dev *jenc = dev_get_drvdata(dev);
	int rc;

	v4l2_m2m_suspend(jenc->m2m_dev);

	rc = pm_runtime_force_suspend(dev);
	if (rc)
		v4l2_m2m_resume(jenc->m2m_dev);

	return rc;
}

static int qcom_jpeg_resume(struct device *dev)
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
	SYSTEM_SLEEP_PM_OPS(qcom_jpeg_suspend, qcom_jpeg_resume)
	RUNTIME_PM_OPS(qcom_jpeg_pm_suspend, qcom_jpeg_pm_resume, NULL)
};

static int qcom_jpeg_probe(struct platform_device *pdev)
{
	const struct qcom_dev_resources *res;
	struct qcom_jenc_dev *jenc;
	int rc;

	jenc = kzalloc_obj(*jenc, GFP_KERNEL);
	if (!jenc)
		return -ENOMEM;

	jenc->dev = &pdev->dev;
	platform_set_drvdata(pdev, jenc);
	rc = devm_mutex_init(&pdev->dev, &jenc->dev_mutex);
	if (rc)
		goto err_free_jenc;
	spin_lock_init(&jenc->hw_lock);
	init_completion(&jenc->reset_complete);
	init_completion(&jenc->stop_complete);

	res = device_get_match_data(jenc->dev);
	if (!res) {
		rc = dev_err_probe(jenc->dev, -ENODEV, "unsupported SoC\n");
		goto err_free_jenc;
	}
	jenc->res = res;

	if (!jenc->res->hw_offs || !jenc->res->hw_ops) {
		rc = dev_err_probe(jenc->dev, -EINVAL, "missing hw resources\n");
		goto err_free_jenc;
	}

	rc = dma_set_mask_and_coherent(jenc->dev, DMA_BIT_MASK(32));
	if (rc) {
		dev_err_probe(jenc->dev, rc, "failed to set DMA mask\n");
		goto err_free_jenc;
	}

	jenc->jpeg_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(jenc->jpeg_base)) {
		rc = dev_err_probe(jenc->dev, PTR_ERR(jenc->jpeg_base),
				   "failed to map JPEG resource\n");
		goto err_free_jenc;
	}

	rc = devm_pm_opp_of_add_table(jenc->dev);
	if (rc && rc != -ENODEV) {
		dev_err_probe(jenc->dev, rc, "failed to add OPP table\n");
		goto err_free_jenc;
	}

	rc = qcom_jpeg_clk_init(jenc);
	if (rc) {
		dev_err_probe(jenc->dev, rc, "failed to get clocks\n");
		goto err_free_jenc;
	}

	jenc->irq = platform_get_irq(pdev, 0);
	if (jenc->irq < 0) {
		rc = dev_err_probe(jenc->dev, jenc->irq, "failed to get IRQ\n");
		goto err_free_jenc;
	}

	rc = devm_request_threaded_irq(jenc->dev, jenc->irq,
				       jenc->res->hw_ops->hw_irq_top,
				       jenc->res->hw_ops->hw_irq_bot,
				       IRQF_ONESHOT, dev_name(jenc->dev), jenc);
	if (rc) {
		dev_err_probe(jenc->dev, rc, "failed to request IRQ\n");
		goto err_free_jenc;
	}

	rc = qcom_jpeg_icc_init(jenc);
	if (rc)
		goto err_free_jenc;

	rc = v4l2_device_register(jenc->dev, &jenc->v4l2_dev);
	if (rc) {
		dev_err_probe(jenc->dev, rc, "failed to register V4L2 device\n");
		goto err_free_jenc;
	}

	jenc->perf = QCOM_SOC_PERF_NOMINAL;

	rc = devm_pm_runtime_enable(jenc->dev);
	if (rc)
		goto err_v4l2_device_unregister;

	rc = qcom_jpeg_v4l2_register(jenc);
	if (rc) {
		dev_err_probe(jenc->dev, rc, "failed to register video device\n");
		goto err_v4l2_device_unregister;
	}

	dev_dbg(jenc->dev, "Qualcomm JPEG encoder registered\n");

	return 0;

err_v4l2_device_unregister:
	v4l2_device_unregister(&jenc->v4l2_dev);
	kfree(jenc);
	return rc;
err_free_jenc:
	kfree(jenc);
	return rc;
}

static void qcom_jpeg_remove(struct platform_device *pdev)
{
	struct qcom_jenc_dev *jenc = platform_get_drvdata(pdev);

	/* v4l2_device_unregister() is called from the release callback. */
	qcom_jpeg_v4l2_unregister(jenc);

	dev_dbg(jenc->dev, "Qualcomm JPEG encoder deregistered\n");
}

static const struct of_device_id qcom_jpeg_of_match[] = {
	{
		.compatible	= "qcom,sm8250-jenc",
		.data		= &qcom_t165_t480_jpeg_drvdata
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
		.pm             = pm_ptr(&qcom_jpeg_pm_ops),
	},
};

module_platform_driver(qcom_jpeg_platform_driver);

MODULE_DESCRIPTION("QCOM JPEG mem2mem V4L2 encoder");
MODULE_LICENSE("GPL");
