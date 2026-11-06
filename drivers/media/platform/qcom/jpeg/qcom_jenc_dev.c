// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_opp.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <media/v4l2-mem2mem.h>

#include "qcom_jenc_dev.h"
#include "qcom_jenc_v420.h"
#include "qcom_jenc_v4l2.h"

struct qcom_jpeg_data {
	const struct dev_pm_domain_attach_data *opp_pd_data;
	unsigned long camnoc_axi_min_rate;
};

static const char * const qcom_jpeg_pd_names[] = { "titan_top" };

static const struct dev_pm_domain_attach_data qcom_jpeg_pd_data = {
	.pd_names = qcom_jpeg_pd_names,
	.num_pd_names = ARRAY_SIZE(qcom_jpeg_pd_names),
};

static const char * const sc7280_opp_pd_names[] = { "cx" };

static const struct dev_pm_domain_attach_data sc7280_opp_pd_data = {
	.pd_names = sc7280_opp_pd_names,
	.num_pd_names = ARRAY_SIZE(sc7280_opp_pd_names),
	.pd_flags = PD_FLAG_REQUIRED_OPP,
};

static const struct qcom_jpeg_data sc7280_data = {
	.opp_pd_data = &sc7280_opp_pd_data,
	.camnoc_axi_min_rate = 150000000,
};

static const char * const sm8250_opp_pd_names[] = { "mx", "mmcx" };

static const struct dev_pm_domain_attach_data sm8250_opp_pd_data = {
	.pd_names = sm8250_opp_pd_names,
	.num_pd_names = ARRAY_SIZE(sm8250_opp_pd_names),
	.pd_flags = PD_FLAG_REQUIRED_OPP,
};

static const struct qcom_jpeg_data sm8250_data = {
	.opp_pd_data = &sm8250_opp_pd_data,
	.camnoc_axi_min_rate = 300000000,
};

/*
 * qcom_jpeg_pd_init - attach the power domains of the JPEG encoder
 *
 * TITAN_TOP_GDSC powers the block. The OPP table votes the performance
 * levels of the SoC domains through required-opps, in the order of
 * the opp_pd_data names.
 */
static int qcom_jpeg_pd_init(struct qcom_jenc_dev *jenc)
{
	const struct qcom_jpeg_data *data = device_get_match_data(jenc->dev);
	int rc;

	rc = devm_pm_domain_attach_list(jenc->dev, &qcom_jpeg_pd_data, &jenc->pd_list);
	if (rc < 0)
		return rc;

	rc = devm_pm_domain_attach_list(jenc->dev, data->opp_pd_data, &jenc->opp_pd_list);
	if (rc < 0)
		return rc;

	return 0;
}

static int qcom_jpeg_opp_init(struct qcom_jenc_dev *jenc)
{
	struct dev_pm_opp *opp;
	int rc;

	rc = devm_pm_opp_set_clkname(jenc->dev, "core");
	if (rc)
		return rc;

	rc = devm_pm_opp_of_add_table(jenc->dev);
	if (rc)
		return rc;

	/* initialize the maximum available frequency for the JPEG core */
	jenc->max_freq = ULONG_MAX;
	opp = dev_pm_opp_find_freq_floor_indexed(jenc->dev, &jenc->max_freq, 0);
	if (IS_ERR(opp))
		return PTR_ERR(opp);

	dev_pm_opp_put(opp);

	/* initialize the default optimized frequency for the JPEG core */
	jenc->opt_freq = jenc->max_freq;

	return 0;
}

static int qcom_jpeg_clk_init(struct qcom_jenc_dev *jenc)
{
	const struct qcom_jpeg_data *data = device_get_match_data(jenc->dev);
	int i;

	jenc->num_clks = devm_clk_bulk_get_all(jenc->dev, &jenc->clks);
	if (jenc->num_clks < 0)
		return jenc->num_clks;

	for (i = 0; i < jenc->num_clks; i++) {
		if (!strcmp(jenc->clks[i].id, "camnoc_axi_src"))
			jenc->camnoc_axi_src = jenc->clks[i].clk;
	}

	if (!jenc->camnoc_axi_src)
		return -ENOENT;

	jenc->camnoc_axi_min_rate = data->camnoc_axi_min_rate;

	return 0;
}

int qcom_jpeg_clk_retune(struct qcom_jenc_dev *jenc, unsigned long *freq)
{
	struct dev_pm_opp *opp;
	int rc;

	/* setup the OPP according to the calculated optimal frequency */
	opp = dev_pm_opp_find_freq_ceil_indexed(jenc->dev, freq, 0);
	if (IS_ERR(opp))
		return PTR_ERR(opp);

	rc = dev_pm_opp_set_opp(jenc->dev, opp);
	if (rc) {
		dev_pm_opp_put(opp);
		return rc;
	}

	dev_pm_opp_put(opp);

	return 0;
}

static int qcom_jpeg_clk_on(struct qcom_jenc_dev *jenc)
{
	int rc;

	rc = clk_bulk_prepare(jenc->num_clks, jenc->clks);
	if (rc)
		return rc;

	rc = clk_set_rate_range(jenc->camnoc_axi_src, jenc->camnoc_axi_min_rate, ULONG_MAX);
	if (rc)
		goto err_clk_unprepare;

	rc = qcom_jpeg_clk_retune(jenc, &jenc->opt_freq);
	if (rc)
		goto err_camnoc_release;

	rc = clk_bulk_enable(jenc->num_clks, jenc->clks);
	if (rc)
		goto err_opp_clear;

	return 0;

err_opp_clear:
	dev_pm_opp_set_opp(jenc->dev, NULL);
err_camnoc_release:
	clk_set_rate_range(jenc->camnoc_axi_src, 0, ULONG_MAX);
err_clk_unprepare:
	clk_bulk_unprepare(jenc->num_clks, jenc->clks);

	return rc;
}

static void qcom_jpeg_clk_off(struct qcom_jenc_dev *jenc)
{
	clk_bulk_disable(jenc->num_clks, jenc->clks);
	dev_pm_opp_set_opp(jenc->dev, NULL);
	clk_set_rate_range(jenc->camnoc_axi_src, 0, ULONG_MAX);
	clk_bulk_unprepare(jenc->num_clks, jenc->clks);
	jenc->opt_freq = jenc->max_freq;
}

static int qcom_jpeg_pm_suspend(struct device *dev)
{
	struct qcom_jenc_dev *jenc = dev_get_drvdata(dev);

	qcom_jpeg_clk_off(jenc);

	return 0;
}

static int qcom_jpeg_pm_resume(struct device *dev)
{
	struct qcom_jenc_dev *jenc = dev_get_drvdata(dev);
	int rc;

	rc = qcom_jpeg_clk_on(jenc);
	if (rc)
		return rc;

	return 0;
}

static int qcom_jpeg_pm_system_suspend(struct device *dev)
{
	struct qcom_jenc_dev *jenc = dev_get_drvdata(dev);
	int rc;

	v4l2_m2m_suspend(jenc->m2m_dev);

	rc = pm_runtime_force_suspend(dev);
	if (rc)
		v4l2_m2m_resume(jenc->m2m_dev);

	return rc;
}

static int qcom_jpeg_pm_system_resume(struct device *dev)
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
	SYSTEM_SLEEP_PM_OPS(qcom_jpeg_pm_system_suspend, qcom_jpeg_pm_system_resume)
	RUNTIME_PM_OPS(qcom_jpeg_pm_suspend, qcom_jpeg_pm_resume, NULL)
};

static void qcom_jpeg_v4l2_dev_unregister(void *data)
{
	v4l2_device_unregister(data);
}

static int qcom_jpeg_probe(struct platform_device *pdev)
{
	struct qcom_jenc_dev *jenc;
	int rc;

	jenc = devm_kzalloc(&pdev->dev, sizeof(*jenc), GFP_KERNEL);
	if (!jenc)
		return -ENOMEM;

	jenc->dev = &pdev->dev;
	platform_set_drvdata(pdev, jenc);

	rc = devm_mutex_init(&pdev->dev, &jenc->dev_mutex);
	if (rc)
		goto err_free_jenc;
	rc = devm_mutex_init(&pdev->dev, &jenc->hw_ref_lock);
	if (rc)
		goto err_free_jenc;
	spin_lock_init(&jenc->hw_lock);
	init_completion(&jenc->reset_complete);
	init_completion(&jenc->stop_complete);

	rc = dma_set_mask_and_coherent(jenc->dev, DMA_BIT_MASK(32));
	if (rc) {
		dev_err_probe(jenc->dev, rc, "failed to set DMA mask\n");
		goto err_free_jenc;
	}

	dma_set_max_seg_size(jenc->dev, UINT_MAX);

	jenc->jpeg_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(jenc->jpeg_base)) {
		rc = dev_err_probe(jenc->dev, PTR_ERR(jenc->jpeg_base),
				   "failed to map JPEG resource\n");
		goto err_free_jenc;
	}

	rc = qcom_jpeg_pd_init(jenc);
	if (rc) {
		dev_err_probe(jenc->dev, rc, "failed to attach power domains\n");
		goto err_free_jenc;
	}

	rc = qcom_jpeg_opp_init(jenc);
	if (rc) {
		dev_err_probe(jenc->dev, rc, "failed to init OPP\n");
		goto err_free_jenc;
	}

	rc = qcom_jpeg_clk_init(jenc);
	if (rc) {
		dev_err_probe(jenc->dev, rc, "failed to init clocks\n");
		goto err_free_jenc;
	}

	jenc->irq = platform_get_irq(pdev, 0);
	if (jenc->irq < 0) {
		rc = dev_err_probe(jenc->dev, jenc->irq, "failed to get IRQ\n");
		goto err_free_jenc;
	}

	rc = devm_request_threaded_irq(jenc->dev, jenc->irq,
				       qcom_jenc_v420_irq_top,
				       qcom_jenc_v420_irq_bot,
				       IRQF_ONESHOT | IRQF_NO_AUTOEN, dev_name(jenc->dev), jenc);
	if (rc) {
		dev_err_probe(jenc->dev, rc, "failed to request IRQ\n");
		goto err_free_jenc;
	}

	rc = v4l2_device_register(jenc->dev, &jenc->v4l2_dev);
	if (rc) {
		dev_err_probe(jenc->dev, rc, "failed to register V4L2 device\n");
		goto err_free_jenc;
	}

	rc = devm_add_action_or_reset(jenc->dev, qcom_jpeg_v4l2_dev_unregister, &jenc->v4l2_dev);
	if (rc)
		goto err_free_jenc;

	rc = devm_pm_runtime_enable(jenc->dev);
	if (rc)
		goto err_free_jenc;

	rc = qcom_jpeg_v4l2_register(jenc);
	if (rc) {
		dev_err_probe(jenc->dev, rc, "failed to register video device\n");
		goto err_free_jenc;
	}

	return 0;

err_free_jenc:
	return rc;
}

static const struct of_device_id qcom_jpeg_of_match[] = {
	{
		.compatible	= "qcom,sc7280-jpeg-enc",
		.data		= &sc7280_data
	},
	{
		.compatible	= "qcom,sm8250-jpeg-enc",
		.data		= &sm8250_data
	},
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_jpeg_of_match);

static struct platform_driver qcom_jpeg_platform_driver = {
	.probe  = qcom_jpeg_probe,
	.driver = {
		.name = QCOM_JPEG_ENC_NAME,
		.of_match_table = qcom_jpeg_of_match,
		.pm             = pm_ptr(&qcom_jpeg_pm_ops),
	},
};

module_platform_driver(qcom_jpeg_platform_driver);

MODULE_DESCRIPTION("QCOM JPEG mem2mem V4L2 encoder");
MODULE_LICENSE("GPL");
