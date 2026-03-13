// SPDX-License-Identifier: GPL-2.0+
/*
 * Unified Aspeed eSPI driver framework for different generation SoCs
 */

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#include "aspeed-espi.h"
#include "ast2600-espi.h"

struct aspeed_espi_ops {
	void (*espi_pre_init)(struct aspeed_espi *espi);
	void (*espi_post_init)(struct aspeed_espi *espi);
	void (*espi_deinit)(struct aspeed_espi *espi);
	int (*espi_perif_probe)(struct aspeed_espi *espi);
	int (*espi_perif_remove)(struct aspeed_espi *espi);
	irqreturn_t (*espi_isr)(int irq, void *espi);
};

static const struct aspeed_espi_ops aspeed_espi_ast2600_ops = {
	.espi_pre_init = ast2600_espi_pre_init,
	.espi_post_init = ast2600_espi_post_init,
	.espi_deinit = ast2600_espi_deinit,
	.espi_perif_probe = ast2600_espi_perif_probe,
	.espi_perif_remove = ast2600_espi_perif_remove,
	.espi_isr = ast2600_espi_isr,
};

static const struct of_device_id aspeed_espi_of_matches[] = {
	{ .compatible = "aspeed,ast2600-espi", .data = &aspeed_espi_ast2600_ops },
	{ }
};
MODULE_DEVICE_TABLE(of, aspeed_espi_of_matches);

static int aspeed_espi_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	struct aspeed_espi *espi;
	struct resource *res;
	struct device *dev;
	int rc;

	dev = &pdev->dev;
	espi = devm_kzalloc(dev, sizeof(*espi), GFP_KERNEL);
	if (!espi)
		return -ENOMEM;

	espi->dev = dev;
	match = of_match_device(aspeed_espi_of_matches, dev);
	if (!match)
		return -ENODEV;

	espi->pdev = pdev;
	espi->ops = match->data;
	if (!espi->ops || !espi->ops->espi_isr)
		return -EINVAL;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(dev, "cannot get resource\n");
		return -ENODEV;
	}

	espi->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(espi->regs)) {
		dev_err(dev, "cannot map registers\n");
		return PTR_ERR(espi->regs);
	}

	espi->irq = platform_get_irq(pdev, 0);
	if (espi->irq < 0) {
		dev_err(dev, "cannot get IRQ number\n");
		return espi->irq;
	}

	espi->rst = devm_reset_control_get_optional(dev, NULL);
	if (IS_ERR(espi->rst)) {
		dev_err(dev, "cannot get reset control\n");
		return PTR_ERR(espi->rst);
	}

	espi->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(espi->clk)) {
		dev_err(dev, "cannot get clock control\n");
		return PTR_ERR(espi->clk);
	}

	rc = clk_prepare_enable(espi->clk);
	if (rc) {
		dev_err(dev, "cannot enable clocks\n");
		return rc;
	}

	if (espi->ops->espi_pre_init)
		espi->ops->espi_pre_init(espi);

	if (espi->ops->espi_perif_probe) {
		rc = espi->ops->espi_perif_probe(espi);
		if (rc) {
			dev_err(dev, "cannot init peripheral channel, rc=%d\n", rc);
			goto err_deinit;
		}
	}

	rc = devm_request_irq(dev, espi->irq, espi->ops->espi_isr, 0,
			      dev_name(dev), espi);
	if (rc) {
		dev_err(dev, "cannot request IRQ\n");
		goto err_deinit;
	}

	if (espi->ops->espi_post_init)
		espi->ops->espi_post_init(espi);

	platform_set_drvdata(pdev, espi);

	dev_info(dev, "module loaded\n");

	return 0;

err_deinit:
	if (espi->ops->espi_deinit)
		espi->ops->espi_deinit(espi);
	clk_disable_unprepare(espi->clk);

	return rc;
}

static void aspeed_espi_remove(struct platform_device *pdev)
{
	struct aspeed_espi *espi;

	espi = platform_get_drvdata(pdev);

	if (!espi)
		return;

	if (espi->ops->espi_perif_remove)
		espi->ops->espi_perif_remove(espi);

	if (espi->ops->espi_deinit)
		espi->ops->espi_deinit(espi);

	clk_disable_unprepare(espi->clk);
}

static struct platform_driver aspeed_espi_driver = {
	.driver = {
		.name = "aspeed-espi",
		.of_match_table = aspeed_espi_of_matches,
	},
	.probe = aspeed_espi_probe,
	.remove = aspeed_espi_remove,
};

module_platform_driver(aspeed_espi_driver);

MODULE_AUTHOR("Aspeed Technology Inc.");
MODULE_DESCRIPTION("Aspeed eSPI controller");
MODULE_LICENSE("GPL");
