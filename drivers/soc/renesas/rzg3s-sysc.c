// SPDX-License-Identifier: GPL-2.0
/*
 * RZ/G3S System controller driver
 *
 * Copyright (C) 2024 Renesas Electronics Corp.
 */

#include <linux/auxiliary_bus.h>
#include <linux/platform_device.h>

#include <linux/soc/renesas/rzg3s-sysc-reset.h>

/**
 * struct rzg3s_sysc - SYSC private data structure
 * @base: base address
 * @dev: device
 * @lock: lock
 */
struct rzg3s_sysc {
	void __iomem *base;
	struct device *dev;
	spinlock_t lock;
};

static void rzg3s_sysc_reset_adev_release(struct device *dev)
{
	struct auxiliary_device *adev = to_auxiliary_dev(dev);
	struct rzg3s_sysc_reset_adev *reset_adev = to_rzg3s_sysc_reset_adev(adev);

	kfree(reset_adev);
}

static void rzg3s_sysc_reset_unregister_adev(void *adev)
{
	auxiliary_device_delete(adev);
	auxiliary_device_uninit(adev);
}

static int rzg3s_sysc_reset_probe(struct rzg3s_sysc *sysc, const char *adev_name,
				  u32 adev_id)
{
	struct rzg3s_sysc_reset_adev *radev;
	struct auxiliary_device *adev;
	int ret;

	radev = kzalloc(sizeof(*radev), GFP_KERNEL);
	if (!radev)
		return -ENOMEM;

	radev->base = sysc->base;
	radev->lock = &sysc->lock;

	adev = &radev->adev;
	adev->name = adev_name;
	adev->dev.parent = sysc->dev;
	adev->dev.release = rzg3s_sysc_reset_adev_release;
	adev->id = adev_id;

	ret = auxiliary_device_init(adev);
	if (ret)
		return ret;

	ret = auxiliary_device_add(adev);
	if (ret) {
		auxiliary_device_uninit(adev);
		return ret;
	}

	return devm_add_action_or_reset(sysc->dev, rzg3s_sysc_reset_unregister_adev, adev);
}

static int rzg3s_sysc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rzg3s_sysc *sysc;

	sysc = devm_kzalloc(dev, sizeof(*sysc), GFP_KERNEL);
	if (!sysc)
		return -ENOMEM;

	sysc->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sysc->base))
		return PTR_ERR(sysc->base);

	sysc->dev = dev;
	spin_lock_init(&sysc->lock);

	return rzg3s_sysc_reset_probe(sysc, "reset", 0);
}

static const struct of_device_id rzg3s_sysc_match[] = {
	{ .compatible = "renesas,r9a08g045-sysc" },
	{ }
};
MODULE_DEVICE_TABLE(of, rzg3s_sysc_match);

static struct platform_driver rzg3s_sysc_driver = {
	.driver = {
		.name = "renesas-rzg3s-sysc",
		.of_match_table = rzg3s_sysc_match
	},
	.probe = rzg3s_sysc_probe
};

static int __init rzg3s_sysc_init(void)
{
	return platform_driver_register(&rzg3s_sysc_driver);
}
subsys_initcall(rzg3s_sysc_init);

MODULE_DESCRIPTION("Renesas RZ/G3S System Controller Driver");
MODULE_AUTHOR("Claudiu Beznea <claudiu.beznea.uj@bp.renesas.com>");
MODULE_LICENSE("GPL");
