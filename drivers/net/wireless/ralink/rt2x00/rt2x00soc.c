// SPDX-License-Identifier: GPL-2.0-or-later
/*
	Copyright (C) 2004 - 2009 Ivo van Doorn <IvDoorn@gmail.com>
	Copyright (C) 2004 - 2009 Felix Fietkau <nbd@openwrt.org>
	<http://rt2x00.serialmonkey.com>

 */

/*
	Module: rt2x00soc
	Abstract: rt2x00 generic soc device routines.
 */

#include <linux/bug.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "rt2x00.h"
#include "rt2x00soc.h"

#ifdef CONFIG_PM
int rt2x00soc_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct ieee80211_hw *hw = platform_get_drvdata(pdev);
	struct rt2x00_dev *rt2x00dev = hw->priv;

	return rt2x00lib_suspend(rt2x00dev);
}
EXPORT_SYMBOL_GPL(rt2x00soc_suspend);

int rt2x00soc_resume(struct platform_device *pdev)
{
	struct ieee80211_hw *hw = platform_get_drvdata(pdev);
	struct rt2x00_dev *rt2x00dev = hw->priv;

	return rt2x00lib_resume(rt2x00dev);
}
EXPORT_SYMBOL_GPL(rt2x00soc_resume);
#endif /* CONFIG_PM */

/*
 * rt2x00soc module information.
 */
MODULE_AUTHOR(DRV_PROJECT);
MODULE_VERSION(DRV_VERSION);
MODULE_DESCRIPTION("rt2x00 soc library");
MODULE_LICENSE("GPL");
