#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/pm_domain.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>

#define DRV_NAME "aips-bridge"

#define AIPSTZ_MPR0		0x0
#define AIPSTZ_MPR1		0x4

#define AIPSTZ_OPACR_NUM	(0x5)
#define OPACR_OFFSET(i)		((i) * 4 + 0x40)

struct aipstz_drv {
	void __iomem *base;
	struct notifier_block power_nb;
	struct aipstz_cfg *cfg;
};

struct aipstz_cfg {
	uint32_t mpr0;
	uint32_t mpr1;
	uint32_t opacr[AIPSTZ_OPACR_NUM];
};

static struct aipstz_cfg aipstz5 = {
	0x77777777,
	0x77777777,
	.opacr = {0x0, 0x0, 0x0, 0x0, 0x0}
};

static void imx_aipstz_config_init(const struct aipstz_drv *drv)

{
	const struct aipstz_cfg *aipstz = drv->cfg;

	writel(aipstz->mpr0, drv->base + AIPSTZ_MPR0);
	writel(aipstz->mpr1, drv->base + AIPSTZ_MPR1);

	for (int i = 0; i < AIPSTZ_OPACR_NUM; i++)
		writel(aipstz->opacr[i], drv->base + OPACR_OFFSET(i));
}

static int aipstz_power_notifier(struct notifier_block *nb,
				 unsigned long action, void *data)
{
	struct aipstz_drv *drv = container_of(nb, struct aipstz_drv, power_nb);

	if (action != GENPD_NOTIFY_ON)
		return NOTIFY_OK;

	imx_aipstz_config_init(drv);

	return NOTIFY_OK;
}

static int aipstz_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct aipstz_drv *drv;

	drv = devm_kzalloc(dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;

	drv->base = of_iomap(pdev->dev.of_node, 0);
	drv->power_nb.notifier_call = aipstz_power_notifier;
	drv->cfg = &aipstz5;

	imx_aipstz_config_init(drv);

	if (dev->pm_domain)
		dev_pm_genpd_add_notifier(dev, &drv->power_nb);

	dev_set_drvdata(dev, drv);

	return 0;
}

static const struct of_device_id aipstz_of_match[] = {
	{.compatible = "fsl,imx8mp-aipstz", },
	{}
};

static struct platform_driver aipstz_driver = {
	.probe =  aipstz_probe,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = of_match_ptr(aipstz_of_match),
	},
};

static int __init aipstz_driver_init(void)
{
	int ret;

	ret = platform_driver_register(&aipstz_driver);
	if (ret) {
		pr_err("Failed to register aipstz platform driver\n");
		return ret;
	}

	return 0;
}

device_initcall(aipstz_driver_init);

MODULE_DESCRIPTION("i.MX8 AIPS bus configuration driver");
MODULE_AUTHOR("Daniel Baluta <daniel.baluta@nxp.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
