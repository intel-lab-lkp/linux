// SPDX-License-Identifier: GPL-2.0
/*
 * CIX Sky1 DMA-350 integration driver
 */

#include <linux/clk.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#define SKY1_DMA350_CRU_DMAC_AP_IRQ	0x54
#define SKY1_DMA350_IRQ_ROUTE_MASK	0xff

struct cix_sky1_dma350 {
	struct clk_bulk_data *clks;
	struct reset_control *reset;
	struct regmap *irq_router;
	struct device *rmem_dev;
	int num_clks;
};

static int cix_sky1_dma350_route_irqs(struct device *dev)
{
	struct cix_sky1_dma350 *data = dev_get_drvdata(dev);

	if (!data->irq_router)
		return 0;

	return regmap_update_bits(data->irq_router,
				  SKY1_DMA350_CRU_DMAC_AP_IRQ,
				  SKY1_DMA350_IRQ_ROUTE_MASK,
				  SKY1_DMA350_IRQ_ROUTE_MASK);
}

static int cix_sky1_dma350_enable_resources(struct device *dev)
{
	struct cix_sky1_dma350 *data = dev_get_drvdata(dev);
	int ret;

	ret = clk_bulk_prepare_enable(data->num_clks, data->clks);
	if (ret)
		return ret;

	ret = reset_control_reset(data->reset);
	if (ret)
		goto err_disable_clks;

	ret = cix_sky1_dma350_route_irqs(dev);
	if (ret)
		goto err_disable_clks;

	return 0;

err_disable_clks:
	clk_bulk_disable_unprepare(data->num_clks, data->clks);
	return ret;
}

static void cix_sky1_dma350_disable_resources(struct device *dev)
{
	struct cix_sky1_dma350 *data = dev_get_drvdata(dev);

	reset_control_assert(data->reset);
	clk_bulk_disable_unprepare(data->num_clks, data->clks);
}

static int cix_sky1_dma350_attach_reserved_mem(struct device *dev)
{
	struct cix_sky1_dma350 *data = dev_get_drvdata(dev);
	struct platform_device *child_pdev;
	struct device_node *child_np;
	int ret;

	if (!of_property_present(dev->of_node, "memory-region"))
		return 0;

	child_np = of_get_compatible_child(dev->of_node, "arm,dma-350");
	if (!child_np)
		return -ENODEV;

	child_pdev = of_find_device_by_node(child_np);
	of_node_put(child_np);
	if (!child_pdev)
		return -EPROBE_DEFER;

	/*
	 * Reserved memory is attached after the child has probed. This relies
	 * on arm-dma350 not allocating coherent command buffers in probe;
	 * those allocations happen per descriptor at prep time.
	 */
	ret = of_reserved_mem_device_init_by_idx(&child_pdev->dev,
						 dev->of_node, 0);
	if (ret) {
		put_device(&child_pdev->dev);
		return ret;
	}

	data->rmem_dev = &child_pdev->dev;

	return 0;
}

static void cix_sky1_dma350_release_reserved_mem(struct device *dev)
{
	struct cix_sky1_dma350 *data = dev_get_drvdata(dev);

	if (data->rmem_dev) {
		of_reserved_mem_device_release(data->rmem_dev);
		put_device(data->rmem_dev);
		data->rmem_dev = NULL;
	}
}

static int cix_sky1_dma350_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cix_sky1_dma350 *data;
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	platform_set_drvdata(pdev, data);

	data->num_clks = devm_clk_bulk_get_all(dev, &data->clks);
	if (data->num_clks < 0)
		return dev_err_probe(dev, data->num_clks,
				     "failed to get clocks\n");

	data->reset = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(data->reset))
		return dev_err_probe(dev, PTR_ERR(data->reset),
				     "failed to get reset\n");

	data->irq_router = syscon_regmap_lookup_by_phandle_optional(dev->of_node,
								    "cix,irq-router");
	if (IS_ERR(data->irq_router))
		return dev_err_probe(dev, PTR_ERR(data->irq_router),
				     "failed to get IRQ router\n");

	ret = cix_sky1_dma350_enable_resources(dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable resources\n");

	ret = of_platform_populate(dev->of_node, NULL, NULL, dev);
	if (ret)
		goto err_disable_resources;

	ret = cix_sky1_dma350_attach_reserved_mem(dev);
	if (ret)
		goto err_depopulate;

	return 0;

err_depopulate:
	of_platform_depopulate(dev);
err_disable_resources:
	cix_sky1_dma350_disable_resources(dev);
	return dev_err_probe(dev, ret, "failed to initialize child devices\n");
}

static void cix_sky1_dma350_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	of_platform_depopulate(dev);
	cix_sky1_dma350_release_reserved_mem(dev);
	cix_sky1_dma350_disable_resources(dev);
}

static int __maybe_unused cix_sky1_dma350_resume_noirq(struct device *dev)
{
	return cix_sky1_dma350_route_irqs(dev);
}

static const struct dev_pm_ops cix_sky1_dma350_pm = {
	SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(NULL, cix_sky1_dma350_resume_noirq)
};

static const struct of_device_id cix_sky1_dma350_of_match[] = {
	{ .compatible = "cix,sky1-dma350" },
	{}
};
MODULE_DEVICE_TABLE(of, cix_sky1_dma350_of_match);

static struct platform_driver cix_sky1_dma350_driver = {
	.probe = cix_sky1_dma350_probe,
	.remove = cix_sky1_dma350_remove,
	.driver = {
		.name = "cix-sky1-dma350",
		.of_match_table = cix_sky1_dma350_of_match,
		.pm = pm_sleep_ptr(&cix_sky1_dma350_pm),
	},
};
module_platform_driver(cix_sky1_dma350_driver);

MODULE_AUTHOR("Jelly Jia <Jelly.Jia@cixtech.com>");
MODULE_DESCRIPTION("CIX Sky1 DMA-350 integration driver");
MODULE_LICENSE("GPL");
