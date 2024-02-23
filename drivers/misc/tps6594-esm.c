// SPDX-License-Identifier: GPL-2.0
/*
 * ESM (Error Signal Monitor) driver for TI TPS65224/TPS6594/TPS6593/LP8764 PMICs
 *
 * Copyright (C) 2023 BayLibre Incorporated - https://www.baylibre.com/
 */

#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

#include <linux/mfd/tps6594.h>

#define TPS6594_DEV_REV_1 0x08

#define ESM_MODE_CFG_SET  0xff
#define ESM_START_SET     0xff
#define ESM_MODE_CFG_CLR  0x0
#define ESM_START_CLR     0x0

static struct reg_field tps65224_esm_mode_cfg = REG_FIELD(TPS6594_REG_ESM_MCU_MODE_CFG,  5, 6);
static struct reg_field tps65224_esm_start    = REG_FIELD(TPS6594_REG_ESM_MCU_START_REG, 0, 0);
static struct reg_field tps6594_esm_mode_cfg  = REG_FIELD(TPS6594_REG_ESM_SOC_MODE_CFG,  5, 6);
static struct reg_field tps6594_esm_start     = REG_FIELD(TPS6594_REG_ESM_SOC_START_REG, 0, 0);

struct tps6594_esm {
	struct regmap_field *esm_mode_cfg;
	struct regmap_field *esm_start;
};

static irqreturn_t tps6594_esm_isr(int irq, void *dev_id)
{
	struct platform_device *pdev = dev_id;
	int i;

	for (i = 0 ; i < pdev->num_resources ; i++) {
		if (irq == platform_get_irq_byname(pdev, pdev->resource[i].name)) {
			dev_err(pdev->dev.parent, "%s error detected\n", pdev->resource[i].name);
			return IRQ_HANDLED;
		}
	}

	return IRQ_NONE;
}

static int tps6594_esm_probe(struct platform_device *pdev)
{
	struct tps6594 *tps = dev_get_drvdata(pdev->dev.parent);
	struct device *dev = &pdev->dev;
	struct tps6594_esm *esm;
	unsigned int rev;
	int irq;
	int ret;
	int i;

	/*
	 * Due to a bug in revision 1 of the PMIC, the GPIO3 used for the
	 * SoC ESM function is used to power the load switch instead.
	 * As a consequence, ESM can not be used on those PMIC.
	 * Check the version and return an error in case of revision 1.
	 */
	if (tps->chip_id == TPS6594 ||
	    tps->chip_id == TPS6593 ||
	    tps->chip_id == LP8764) {
		ret = regmap_read(tps->regmap, TPS6594_REG_DEV_REV, &rev);
		if (ret)
			return dev_err_probe(dev, ret,
					     "Failed to read PMIC revision\n");
		if (rev == TPS6594_DEV_REV_1)
			return dev_err_probe(dev, -ENODEV,
				      "ESM not supported for revision 1 PMIC\n");
	}

	for (i = 0; i < pdev->num_resources; i++) {
		irq = platform_get_irq_byname(pdev, pdev->resource[i].name);
		if (irq < 0)
			return irq;

		ret = devm_request_threaded_irq(dev, irq, NULL,
						tps6594_esm_isr, IRQF_ONESHOT,
						pdev->resource[i].name, pdev);
		if (ret)
			return dev_err_probe(dev, ret, "Failed to request irq\n");
	}

	esm = devm_kzalloc(dev, sizeof(struct tps6594_esm), GFP_KERNEL);
	if (!esm)
		return -ENOMEM;

	if (tps->chip_id == TPS65224) {
		esm->esm_mode_cfg = devm_regmap_field_alloc(dev, tps->regmap,
							    tps65224_esm_mode_cfg);
		esm->esm_start = devm_regmap_field_alloc(dev, tps->regmap, tps65224_esm_start);
	} else {
		esm->esm_mode_cfg = devm_regmap_field_alloc(dev, tps->regmap, tps6594_esm_mode_cfg);
		esm->esm_start = devm_regmap_field_alloc(dev, tps->regmap, tps6594_esm_start);
	}

	if (IS_ERR(esm->esm_mode_cfg)) {
		dev_err(dev, "esm_mode_cfg reg field init failed\n");
		return PTR_ERR(esm->esm_mode_cfg);
	}

	if (IS_ERR(esm->esm_start)) {
		dev_err(dev, "esm_start reg field init failed\n");
		return PTR_ERR(esm->esm_start);
	}

	platform_set_drvdata(pdev, esm);

	ret = regmap_field_write(esm->esm_mode_cfg, ESM_MODE_CFG_SET);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to configure ESM\n");

	ret = regmap_field_write(esm->esm_start, ESM_START_SET);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to start ESM\n");

	pm_runtime_enable(dev);
	pm_runtime_get_sync(dev);

	return 0;
}

static void tps6594_esm_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct tps6594_esm *esm = platform_get_drvdata(pdev);
	int ret;

	ret = regmap_field_write(esm->esm_start, ESM_START_CLR);
	if (ret) {
		dev_err(dev, "Failed to stop ESM\n");
		goto out;
	}

	ret = regmap_field_write(esm->esm_mode_cfg, ESM_MODE_CFG_CLR);
	if (ret)
		dev_err(dev, "Failed to unconfigure ESM\n");

out:
	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);
}

static int tps6594_esm_suspend(struct device *dev)
{
	struct platform_device *pdev = container_of(dev, struct platform_device, dev);
	struct tps6594_esm *esm = platform_get_drvdata(pdev);

	int ret;

	ret = regmap_field_write(esm->esm_start, ESM_START_CLR);

	pm_runtime_put_sync(dev);

	return ret;
}

static int tps6594_esm_resume(struct device *dev)
{
	struct platform_device *pdev = container_of(dev, struct platform_device, dev);
	struct tps6594_esm *esm = platform_get_drvdata(pdev);

	pm_runtime_get_sync(dev);

	return regmap_field_write(esm->esm_start, ESM_START_SET);
}

static DEFINE_SIMPLE_DEV_PM_OPS(tps6594_esm_pm_ops, tps6594_esm_suspend, tps6594_esm_resume);

static struct platform_driver tps6594_esm_driver = {
	.driver	= {
		.name = "tps6594-esm",
		.pm = pm_sleep_ptr(&tps6594_esm_pm_ops),
	},
	.probe = tps6594_esm_probe,
	.remove_new = tps6594_esm_remove,
};

module_platform_driver(tps6594_esm_driver);

MODULE_ALIAS("platform:tps6594-esm");
MODULE_AUTHOR("Julien Panis <jpanis@baylibre.com>");
MODULE_AUTHOR("Bhargav Raviprakash <bhargav.r@ltts.com>");
MODULE_DESCRIPTION("TPS6594 Error Signal Monitor Driver");
MODULE_LICENSE("GPL");
