// SPDX-License-Identifier: GPL-2.0
/*
 * brcmstb HWSEM driver
 *
 * Copyright (C) 2025 Broadcom
 *
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/hwspinlock.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include "hwspinlock_internal.h"

#define BRCMSTB_MAX_SEMAPHORES		16
#define RESET_SEMAPHORE			0

#define HWSPINLOCK_VAL			'L'

static int brcmstb_hwspinlock_trylock(struct hwspinlock *lock)
{
	void __iomem *lock_addr = lock->priv;

	writel(HWSPINLOCK_VAL, lock_addr);

	return (readl(lock_addr) == HWSPINLOCK_VAL);
}

static void brcmstb_hwspinlock_unlock(struct hwspinlock *lock)
{
	void __iomem *lock_addr = lock->priv;

	/* release the lock by writing 0 to it */
	writel(RESET_SEMAPHORE, lock_addr);
}

static void brcmstb_hwspinlock_relax(struct hwspinlock *lock)
{
	ndelay(50);
}

static const struct hwspinlock_ops brcmstb_hwspinlock_ops = {
	.trylock	= brcmstb_hwspinlock_trylock,
	.unlock		= brcmstb_hwspinlock_unlock,
	.relax		= brcmstb_hwspinlock_relax,
};

static int brcmstb_hwspinlock_probe(struct platform_device *pdev)
{
	struct hwspinlock_device *bank;
	struct hwspinlock *hwlock;
	void __iomem *io_base;
	int i, num_locks = BRCMSTB_MAX_SEMAPHORES;

	io_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(io_base)) {
		dev_err(&pdev->dev, "semaphore iobase mapping error\n");
		return PTR_ERR(io_base);
	}

	bank = devm_kzalloc(&pdev->dev, struct_size(bank, lock, num_locks),
			    GFP_KERNEL);
	if (!bank)
		return -ENOMEM;

	platform_set_drvdata(pdev, bank);

	for (i = 0, hwlock = &bank->lock[0]; i < num_locks; i++, hwlock++)
		hwlock->priv = io_base + sizeof(u32) * i;

	return devm_hwspin_lock_register(&pdev->dev, bank,
					 &brcmstb_hwspinlock_ops,
					 0, num_locks);
}

static const struct of_device_id brcmstb_hwspinlock_ids[] = {
	{ .compatible = "brcm,brcmstb-hwspinlock", },
	{ /* end */ },
};
MODULE_DEVICE_TABLE(of, brcmstb_hwspinlock_ids);

static struct platform_driver brcmstb_hwspinlock_driver = {
	.probe		= brcmstb_hwspinlock_probe,
	.driver		= {
		.name	= "brcmstb_hwspinlock",
		.of_match_table = brcmstb_hwspinlock_ids,
	},
};

module_platform_driver(brcmstb_hwspinlock_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Hardware Spinlock driver for brcmstb");
MODULE_AUTHOR("Kamal Dasu <kdasu@broadcom.com>");
