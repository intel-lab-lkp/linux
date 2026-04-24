// SPDX-License-Identifier: GPL-2.0
/*
 * Hardware spinlock driver for StarFive JHB100 SoC
 *
 * Copyright (C) 2026 StarFive Technology Co., Ltd.
 */

#include <linux/delay.h>
#include <linux/hwspinlock.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#include "hwspinlock_internal.h"

/* reg offset */
#define STARFIVE_REG_APP_LOCK_REQ		0x08
#define STARFIVE_REG_APP_LOCK_RLS		0x0C
#define STARFIVE_REG_LOCK_STA			0x10

/* macro STARFIVE_REG_LOCK_STA reg*/
#define STARFIVE_STA_APP_OWN			BIT(1)
#define STARFIVE_STA_OWN_MSK			0x3

#define STARFIVE_NUM_LOCKS			16

struct starfive_hwspinlock {
	void __iomem *base;
	struct reset_control *rst;
	struct hwspinlock_device bank;
};

static int starfive_hwspinlock_trylock(struct hwspinlock *lock)
{
	struct starfive_hwspinlock *priv = dev_get_drvdata(lock->bank->dev);
	int id = hwlock_to_id(lock);
	u32 status;

	writel(BIT(id), priv->base + STARFIVE_REG_APP_LOCK_REQ);
	status = (readl(priv->base + STARFIVE_REG_LOCK_STA) >> (2 * id)) &
		 STARFIVE_STA_OWN_MSK;

	return (status == STARFIVE_STA_APP_OWN);
}

static void starfive_hwspinlock_unlock(struct hwspinlock *lock)
{
	struct starfive_hwspinlock *priv = dev_get_drvdata(lock->bank->dev);
	int id = hwlock_to_id(lock);

	writel(BIT(id), priv->base + STARFIVE_REG_APP_LOCK_RLS);
}

static void starfive_hwspinlock_relax(struct hwspinlock *lock)
{
	ndelay(50);
}

static const struct hwspinlock_ops starfive_hwspinlock_ops = {
	.trylock	= starfive_hwspinlock_trylock,
	.unlock		= starfive_hwspinlock_unlock,
	.relax		= starfive_hwspinlock_relax,
};

static void starfive_hwspinlock_disable(void *data)
{
	struct starfive_hwspinlock *priv = data;

	reset_control_assert(priv->rst);
}

static int starfive_hwspinlock_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct starfive_hwspinlock *priv;
	int ret;

	priv = devm_kzalloc(dev, struct_size(priv, bank.lock, STARFIVE_NUM_LOCKS),
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	priv->rst = devm_reset_control_array_get_exclusive(dev);
	if (IS_ERR(priv->rst))
		return dev_err_probe(dev, PTR_ERR(priv->rst),
				     "failed to get reset\n");

	ret = reset_control_deassert(priv->rst);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, priv);

	ret = devm_add_action_or_reset(dev, starfive_hwspinlock_disable, priv);
	if (ret)
		goto fail_action;

	return devm_hwspin_lock_register(dev, &priv->bank, &starfive_hwspinlock_ops,
					 0, STARFIVE_NUM_LOCKS);

fail_action:
	reset_control_assert(priv->rst);
	return ret;
}

static const struct of_device_id starfive_hwpinlock_ids[] = {
	{ .compatible = "starfive,jhb100-hwspinlock", },
	{},
};
MODULE_DEVICE_TABLE(of, starfive_hwpinlock_ids);

static struct platform_driver starfive_hwspinlock_driver = {
	.probe		= starfive_hwspinlock_probe,
	.driver		= {
		.name	= "starfive_hwspinlock",
		.of_match_table = starfive_hwpinlock_ids,
	},
};
module_platform_driver(starfive_hwspinlock_driver);

MODULE_AUTHOR("Xingyu Wu <xingyu.wu@starfivetech.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Hardware spinlock driver for StarFive JHB100 SoC");
