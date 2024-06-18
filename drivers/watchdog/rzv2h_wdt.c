// SPDX-License-Identifier: GPL-2.0
/*
 * Renesas RZ/V2H(P) WDT Watchdog Driver
 *
 * Copyright (C) 2024 Renesas Electronics Corporation.
 */
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/units.h>
#include <linux/watchdog.h>

#define WDTRR			0x00	/* RW, 8  */
#define WDTCR			0x02	/* RW, 16 */
#define WDTRCR			0x06	/* RW, 8  */

#define WDTCR_TOPS_1024		0x00
#define WDTCR_TOPS_16384	0x03

#define WDTCR_CKS_CLK_1		0x00
#define WDTCR_CKS_CLK_256	0x50

#define WDTCR_RPES_0		0x300
#define WDTCR_RPES_75		0x000

#define WDTCR_RPSS_25		0x00
#define WDTCR_RPSS_100		0x3000

#define WDTRCR_RSTIRQS         BIT(7)

#define CLOCK_DIV_BY_256	256

#define WDT_DEFAULT_TIMEOUT	60U

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default="
		 __MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

struct rzv2h_wdt_priv {
	void __iomem *base;
	struct watchdog_device wdev;
	struct reset_control *rstc;
	unsigned long oscclk_rate;
};

static u32 rzv2h_wdt_get_cycle_usec(struct rzv2h_wdt_priv *priv,
				    unsigned long cycle,
				    u16 wdttime)
{
	int clock_division_ratio;
	u64 timer_cycle_us;

	clock_division_ratio = CLOCK_DIV_BY_256;

	timer_cycle_us = clock_division_ratio * (wdttime + 1) * MICRO;

	return div64_ul(timer_cycle_us, cycle);
}

static int rzv2h_wdt_ping(struct watchdog_device *wdev)
{
	struct rzv2h_wdt_priv *priv = watchdog_get_drvdata(wdev);
	unsigned long delay;

	writeb(0x0, priv->base + WDTRR);
	writeb(0xFF, priv->base + WDTRR);

	/*
	 * Refreshing the down-counter requires up to 4 cycles
	 * of the signal for counting
	 */
	delay = 4 * rzv2h_wdt_get_cycle_usec(priv, priv->oscclk_rate, 0);
	udelay(delay);

	return 0;
}

static void rzv2h_wdt_setup(struct watchdog_device *wdev, u16 wdtcr)
{
	struct rzv2h_wdt_priv *priv = watchdog_get_drvdata(wdev);

	writew(wdtcr, priv->base + WDTCR);

	/* LSI needs RSTIRQS to be cleared */
	writeb(readb(priv->base + WDTRCR) & ~WDTRCR_RSTIRQS, priv->base + WDTRCR);
}

static int rzv2h_wdt_start(struct watchdog_device *wdev)
{
	pm_runtime_get_sync(wdev->parent);

	/*
	 * WDTCR
	 * - CKS[7:4] - Clock Division Ratio Select - 0101b: oscclk/256
	 * - RPSS[13:12] - Window Start Position Select - 11b: 100%
	 * - RPES[9:8] - Window End Position Select - 11b: 0%
	 * - TOPS[1:0] - Timeout Period Select - 11b: 16384 cycles (3FFFh)
	 */
	rzv2h_wdt_setup(wdev, WDTCR_CKS_CLK_256 | WDTCR_RPSS_100 |
			WDTCR_RPES_0 | WDTCR_TOPS_16384);

	rzv2h_wdt_ping(wdev);

	return 0;
}

static int rzv2h_wdt_stop(struct watchdog_device *wdev)
{
	struct rzv2h_wdt_priv *priv = watchdog_get_drvdata(wdev);

	pm_runtime_put(wdev->parent);
	reset_control_reset(priv->rstc);

	return 0;
}

static const struct watchdog_info rzv2h_wdt_ident = {
	.options = WDIOF_MAGICCLOSE | WDIOF_KEEPALIVEPING | WDIOF_SETTIMEOUT,
	.identity = "Renesas RZ/V2H WDT Watchdog",
};

static int rzv2h_wdt_restart(struct watchdog_device *wdev,
			     unsigned long action, void *data)
{
	rzv2h_wdt_stop(wdev);

	pm_runtime_get_sync(wdev->parent);

	/*
	 * WDTCR
	 * - CKS[7:4] - Clock Division Ratio Select - 0000b: oscclk/1
	 * - RPSS[13:12] - Window Start Position Select - 00b: 25%
	 * - RPES[9:8] - Window End Position Select - 00b: 75%
	 * - TOPS[1:0] - Timeout Period Select - 00b: 1024 cycles (03FFh)
	 */
	rzv2h_wdt_setup(wdev, WDTCR_CKS_CLK_1 | WDTCR_RPSS_25 |
			WDTCR_RPES_75 | WDTCR_TOPS_1024);

	rzv2h_wdt_ping(wdev);

	return 0;
}

static const struct watchdog_ops rzv2h_wdt_ops = {
	.owner = THIS_MODULE,
	.start = rzv2h_wdt_start,
	.stop = rzv2h_wdt_stop,
	.ping = rzv2h_wdt_ping,
	.restart = rzv2h_wdt_restart,
};

static void rzv2h_wdt_reset_assert(void *data)
{
	struct reset_control *rstc = data;

	reset_control_assert(rstc);
}

static int rzv2h_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rzv2h_wdt_priv *priv;
	struct clk *oscclk;
	unsigned long rate;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	/* Get watchdog oscclk clock */
	oscclk = clk_get(&pdev->dev, "oscclk");
	if (IS_ERR(oscclk))
		return dev_err_probe(&pdev->dev, PTR_ERR(oscclk),
				     "no oscclk");

	priv->oscclk_rate = clk_get_rate(oscclk);
	clk_put(oscclk);
	if (!priv->oscclk_rate)
		return dev_err_probe(&pdev->dev, -EINVAL, "oscclk rate is 0");

	priv->rstc = devm_reset_control_get_exclusive(&pdev->dev, NULL);
	if (IS_ERR(priv->rstc))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->rstc),
					"failed to get cpg reset");

	ret = reset_control_deassert(priv->rstc);
	if (ret)
		return dev_err_probe(dev, ret, "failed to deassert");
	ret = devm_add_action_or_reset(&pdev->dev,
				       rzv2h_wdt_reset_assert,
				       priv->rstc);
	if (ret < 0)
		return ret;

	rate = priv->oscclk_rate / 256;
	priv->wdev.max_hw_heartbeat_ms = (1000 * 16383) / rate;
	dev_dbg(dev, "max hw timeout of %dms\n",
		priv->wdev.max_hw_heartbeat_ms);

	priv->wdev.min_timeout = 1;
	priv->wdev.timeout = WDT_DEFAULT_TIMEOUT;
	priv->wdev.info = &rzv2h_wdt_ident;
	priv->wdev.ops = &rzv2h_wdt_ops;
	priv->wdev.parent = dev;
	watchdog_set_drvdata(&priv->wdev, priv);
	watchdog_set_nowayout(&priv->wdev, nowayout);
	watchdog_stop_on_unregister(&priv->wdev);

	ret = watchdog_init_timeout(&priv->wdev, 0, dev);
	if (ret)
		dev_warn(dev, "Specified timeout invalid, using default");

	ret = devm_pm_runtime_enable(&pdev->dev);
	if (ret)
		return ret;

	return devm_watchdog_register_device(&pdev->dev, &priv->wdev);
}

static const struct of_device_id rzv2h_wdt_ids[] = {
	{ .compatible = "renesas,r9a09g057-wdt", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rzv2h_wdt_ids);

static struct platform_driver rzv2h_wdt_driver = {
	.driver = {
		.name = "rzv2h_wdt",
		.of_match_table = rzv2h_wdt_ids,
	},
	.probe = rzv2h_wdt_probe,
};
module_platform_driver(rzv2h_wdt_driver);
MODULE_AUTHOR("Lad Prabhakar <prabhakar.mahadev-lad.rj@bp.renesas.com>");
MODULE_DESCRIPTION("Renesas RZ/V2H(P) WDT Watchdog Driver");
