// SPDX-License-Identifier: GPL-2.0-only
/*
 * SBSA(Server Base System Architecture) Generic Watchdog driver
 *
 * Copyright (c) 2015, Linaro Ltd.
 * Author: Fu Wei <fu.wei@linaro.org>
 *         Suravee Suthikulpanit <Suravee.Suthikulpanit@amd.com>
 *         Al Stone <al.stone@linaro.org>
 *         Timur Tabi <timur@codeaurora.org>
 *
 * ARM SBSA Generic Watchdog has two stage timeouts:
 * the first signal (WS0) is for alerting the system by interrupt,
 * the second one (WS1) is a real hardware reset.
 * More details about the hardware specification of this device:
 * ARM DEN0029B - Server Base System Architecture (SBSA)
 *
 * This driver can operate ARM SBSA Generic Watchdog as a single stage watchdog
 * or a two stages watchdog, it's set up by the module parameter "action".
 * In the single stage mode, when the timeout is reached, your system
 * will be reset by WS1. The first signal (WS0) is ignored.
 * In the two stages mode, when the timeout is reached, the first signal (WS0)
 * will trigger panic. If the system is getting into trouble and cannot be reset
 * by panic or restart properly by the kdump kernel(if supported), then the
 * second stage (as long as the first stage) will be reached, system will be
 * reset by WS1. This function can help administrator to backup the system
 * context info by panic console output or kdump.
 *
 * SBSA GWDT:
 * if action is 1 (the two stages mode):
 * |--------WOR-------WS0--------WOR-------WS1
 * |----timeout-----(panic)----timeout-----reset
 *
 * if action is 0 (the single stage mode):
 * |------WOR-----WS0(ignored)-----WOR------WS1
 * |--------------timeout-------------------reset
 *
 * Note: Since this watchdog timer has two stages, and each stage is determined
 * by WOR, in the single stage mode, the timeout is (WOR * 2); in the two
 * stages mode, the timeout is WOR. The maximum timeout in the two stages mode
 * is half of that in the single stage mode.
 */

#include <linux/io.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/watchdog.h>
#include <asm/arch_timer.h>
#include <linux/arm-smccc.h>

#define DRV_NAME		"sbsa-gwdt"
#define WATCHDOG_NAME		"SBSA Generic Watchdog"

/* SBSA Generic Watchdog register definitions */
/* refresh frame */
#define SBSA_GWDT_WRR		0x000

/* control frame */
#define SBSA_GWDT_WCS		0x000
#define SBSA_GWDT_WOR		0x008
#define SBSA_GWDT_WCV		0x010

/* refresh/control frame */
#define SBSA_GWDT_W_IIDR	0xfcc
#define SBSA_GWDT_IDR		0xfd0

/* Watchdog Control and Status Register */
#define SBSA_GWDT_WCS_EN	BIT(0)
#define SBSA_GWDT_WCS_WS0	BIT(1)
#define SBSA_GWDT_WCS_WS1	BIT(2)

#define SBSA_GWDT_VERSION_MASK  0xF
#define SBSA_GWDT_VERSION_SHIFT 16

/* Marvell AC5/X SMCs, taken from arm trusted firmware */
#define SMC_FID_READ_REG	0x80007FFE
#define SMC_FID_WRITE_REG	0x80007FFD

/* Marvell registers offsets: */
#define SBSA_GWDT_MARVELL_CPU_WD_RST_EN_REG	0x30
#define SBSA_GWDT_MARVELL_MNG_ID_REG		0x4C
#define SBSA_GWDT_MARVELL_RST_CTRL_REG		0x0C

#define SBSA_GWDT_MARVELL_ID_MASK	GENMASK(19, 12)
#define SBSA_GWDT_MARVELL_AC5_ID	0xB4000
#define SBSA_GWDT_MARVELL_AC5X_ID	0x98000
#define SBSA_GWDT_MARVELL_IML_ID	0xA0000
#define SBSA_GWDT_MARVELL_IMM_ID	0xA2000

#define SBSA_GWDT_MARVELL_AC5_RST_UNIT_WD_BIT		BIT(6)
/* The following applies to AC5X, IronMan L and M: */
#define SBSA_GWDT_MARVELL_IRONMAN_RST_UNIT_WD_BIT	BIT(7)

/*
 * Action to perform after watchdog gets WS1 (watchdog signal 1) interrupt
 * PWD = Private Watchdog, GWD - Global Watchdog, mpp - multi purpose pin
 *
 * 0 = Enable  1 = Disable (Default)
 *
 * BIT  0: CPU 0 reset by PWD 0
 * BIT  1: CPU 1 reset by PWD 1
 * BIT  2: CPU 0 reset by GWD
 * BIT  3: CPU 1 reset by GWD
 * BIT  4: PWD 0 sys reset out
 * BIT  5: PWD 1 sys reset out
 * BIT  6: GWD sys reset out
 * BIT  7: Reserved
 * BIT  8: PWD 0 mpp reset out
 * BIT  9: PWD 1 mpp reset out
 * BIT 10: GWD mpp reset out
 *
 */
#define SBSA_GWDT_MARVELL_RST_CPU0_BY_PWD0	BIT(0)
#define SBSA_GWDT_MARVELL_RST_CPU1_BY_PWD1	BIT(1)
#define SBSA_GWDT_MARVELL_RST_CPU0_BY_GWD	BIT(2)
#define SBSA_GWDT_MARVELL_RST_CPU1_BY_GWD	BIT(3)
#define SBSA_GWDT_MARVELL_RST_SYSRST_BY_PWD0	BIT(4)
#define SBSA_GWDT_MARVELL_RST_SYSRST_BY_PWD1	BIT(5)
#define SBSA_GWDT_MARVELL_RST_SYSRST_BY_GWD	BIT(6)
#define SBSA_GWDT_MARVELL_RST_RESERVED		BIT(7)
#define SBSA_GWDT_MARVELL_RST_MPP_BY_PWD0	BIT(8)
#define SBSA_GWDT_MARVELL_RST_MPP_BY_PWD1	BIT(9)
#define SBSA_GWDT_MARVELL_RST_MPP_BY_GWD	BIT(10)

/**
 * struct sbsa_gwdt_regs_ops - ops for register read/write, depending on SOC
 * @reg_read:			register read ops function
 * @read_write:			register write ops function
 */
struct sbsa_gwdt_regs_ops {
	u32 (*reg_read32)(void __iomem *ptr);
	__u64 (*reg_read64)(void __iomem *ptr);
	void (*reg_write32)(u32 val, void __iomem *ptr);
	void (*reg_write64)(__u64 val, void __iomem *ptr);
};

/**
 * struct sbsa_gwdt - Internal representation of the SBSA GWDT
 * @wdd:		kernel watchdog_device structure
 * @clk:		store the System Counter clock frequency, in Hz.
 * @version:            store the architecture version
 * @refresh_base:	Virtual address of the watchdog refresh frame
 * @control_base:	Virtual address of the watchdog control frame
 */
struct sbsa_gwdt {
	struct watchdog_device	wdd;
	u32			clk;
	int			version;
	void __iomem		*refresh_base;
	void __iomem		*control_base;
	const struct sbsa_gwdt_regs_ops *soc_reg_ops;
};

#define DEFAULT_TIMEOUT		10 /* seconds */

static unsigned int timeout;
module_param(timeout, uint, 0);
MODULE_PARM_DESC(timeout,
		 "Watchdog timeout in seconds. (>=0, default="
		 __MODULE_STRING(DEFAULT_TIMEOUT) ")");

/*
 * action refers to action taken when watchdog gets WS0
 * 0 = skip
 * 1 = panic
 * defaults to skip (0)
 */
static int action;
module_param(action, int, 0);
MODULE_PARM_DESC(action, "after watchdog gets WS0 interrupt, do: "
		 "0 = skip(*)  1 = panic");

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, S_IRUGO);
MODULE_PARM_DESC(nowayout,
		 "Watchdog cannot be stopped once started (default="
		 __MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

/*
 * By default, have the Global watchdog cause System Reset:
 */
static unsigned int reset = 0xFFFFFFFF ^ SBSA_GWDT_MARVELL_RST_SYSRST_BY_GWD;
module_param(reset, uint, 0);
MODULE_PARM_DESC(reset, "Action to perform after watchdog gets WS1 interrupt");

/*
 * Marvell AC5/X use SMC, while others use direct register access
 */
static u32 sbsa_gwdt_smc_readl(void __iomem *addr)
{
	struct arm_smccc_res smc_res;

	arm_smccc_smc(SMC_FID_READ_REG, (unsigned long)addr,
		      0, 0, 0, 0, 0, 0, &smc_res);
	return (u32)smc_res.a0;
}

static void sbsa_gwdt_smc_writel(u32 val, void __iomem *addr)
{
	struct arm_smccc_res smc_res;

	arm_smccc_smc(SMC_FID_WRITE_REG, (unsigned long)addr,
		      (unsigned long)val, 0, 0, 0, 0, 0, &smc_res);
}

static inline u64 sbsa_gwdt_lo_hi_smc_readq(void __iomem *addr)
{
	u32 low, high;

	low = sbsa_gwdt_smc_readl(addr);
	high = sbsa_gwdt_smc_readl(addr + 4);
	/* read twice, as a workaround to HW limitation */
	low = sbsa_gwdt_smc_readl(addr);

	return low + ((u64)high << 32);
}

static inline void sbsa_gwdt_lo_hi_smc_writeq(__u64 val, void __iomem *addr)
{
	u32 low, high;

	low = val & 0xffffffff;
	high = val >> 32;
	sbsa_gwdt_smc_writel(low, addr);
	sbsa_gwdt_smc_writel(high, addr + 4);
	/* write twice, as a workaround to HW limitation */
	sbsa_gwdt_smc_writel(low, addr);
}

static u32 sbsa_gwdt_direct_reg_readl(void __iomem *addr)
{
	return readl(addr);
}

static void sbsa_gwdt_direct_reg_writel(u32 val, void __iomem *addr)
{
	writel(val, addr);
}

static inline u64 sbsa_gwdt_lo_hi_direct_readq(void __iomem *addr)
{
	return lo_hi_readq(addr);
}

static inline void sbsa_gwdt_lo_hi_direct_writeq(__u64 val, void __iomem *addr)
{
	lo_hi_writeq(val, addr);
}

static const struct sbsa_gwdt_regs_ops smc_reg_ops = {
	.reg_read32 = sbsa_gwdt_smc_readl,
	.reg_read64 = sbsa_gwdt_lo_hi_smc_readq,
	.reg_write32 = sbsa_gwdt_smc_writel,
	.reg_write64 = sbsa_gwdt_lo_hi_smc_writeq
};

static const struct sbsa_gwdt_regs_ops direct_reg_ops = {
	.reg_read32 = sbsa_gwdt_direct_reg_readl,
	.reg_read64 = sbsa_gwdt_lo_hi_direct_readq,
	.reg_write32 = sbsa_gwdt_direct_reg_writel,
	.reg_write64 = sbsa_gwdt_lo_hi_smc_writeq
};

/*
 * Arm Base System Architecture 1.0 introduces watchdog v1 which
 * increases the length watchdog offset register to 48 bits.
 * - For version 0: WOR is 32 bits;
 * - For version 1: WOR is 48 bits which comprises the register
 * offset 0x8 and 0xC, and the bits [63:48] are reserved which are
 * Read-As-Zero and Writes-Ignored.
 */
static u64 sbsa_gwdt_reg_read(struct sbsa_gwdt *gwdt)
{
	if (gwdt->version == 0)
		return gwdt->soc_reg_ops->reg_read32(gwdt->control_base + SBSA_GWDT_WOR);
	else
		return gwdt->soc_reg_ops->reg_read64(gwdt->control_base + SBSA_GWDT_WOR);
}

static void sbsa_gwdt_reg_write(u64 val, struct sbsa_gwdt *gwdt)
{
	if (gwdt->version == 0)
		gwdt->soc_reg_ops->reg_write32((u32)val, gwdt->control_base + SBSA_GWDT_WOR);
	else
		gwdt->soc_reg_ops->reg_write64(val, gwdt->control_base + SBSA_GWDT_WOR);
}

/*
 * watchdog operation functions
 */
static int sbsa_gwdt_set_timeout(struct watchdog_device *wdd,
				 unsigned int timeout)
{
	struct sbsa_gwdt *gwdt = watchdog_get_drvdata(wdd);

	wdd->timeout = timeout;
	timeout = clamp_t(unsigned int, timeout, 1, wdd->max_hw_heartbeat_ms / 1000);

	if (action)
		sbsa_gwdt_reg_write((u64)gwdt->clk * timeout, gwdt);
	else
		/*
		 * In the single stage mode, The first signal (WS0) is ignored,
		 * the timeout is (WOR * 2), so the WOR should be configured
		 * to half value of timeout.
		 */
		sbsa_gwdt_reg_write(((u64)gwdt->clk / 2) * timeout, gwdt);

	return 0;
}

static unsigned int sbsa_gwdt_get_timeleft(struct watchdog_device *wdd)
{
	struct sbsa_gwdt *gwdt = watchdog_get_drvdata(wdd);
	u64 timeleft = 0;

	/*
	 * In the single stage mode, if WS0 is deasserted
	 * (watchdog is in the first stage),
	 * timeleft = WOR + (WCV - system counter)
	 */
	if (!action &&
	    !(gwdt->soc_reg_ops->reg_read32(gwdt->control_base + SBSA_GWDT_WCS)
					    & SBSA_GWDT_WCS_WS0))
		timeleft += sbsa_gwdt_reg_read(gwdt);

	timeleft += gwdt->soc_reg_ops->reg_read64(gwdt->control_base + SBSA_GWDT_WCV) -
		    arch_timer_read_counter();

	do_div(timeleft, gwdt->clk);

	return timeleft;
}

static int sbsa_gwdt_keepalive(struct watchdog_device *wdd)
{
	struct sbsa_gwdt *gwdt = watchdog_get_drvdata(wdd);

	/*
	 * Writing WRR for an explicit watchdog refresh.
	 * You can write anyting (like 0).
	 */
	gwdt->soc_reg_ops->reg_write32(0, gwdt->refresh_base + SBSA_GWDT_WRR);

	return 0;
}

static void sbsa_gwdt_get_version(struct watchdog_device *wdd)
{
	struct sbsa_gwdt *gwdt = watchdog_get_drvdata(wdd);
	int ver;

	ver = gwdt->soc_reg_ops->reg_read32(gwdt->control_base + SBSA_GWDT_W_IIDR);
	ver = (ver >> SBSA_GWDT_VERSION_SHIFT) & SBSA_GWDT_VERSION_MASK;

	gwdt->version = ver;
}

static int sbsa_gwdt_start(struct watchdog_device *wdd)
{
	struct sbsa_gwdt *gwdt = watchdog_get_drvdata(wdd);

	/* writing WCS will cause an explicit watchdog refresh */
	gwdt->soc_reg_ops->reg_write32(SBSA_GWDT_WCS_EN, gwdt->control_base + SBSA_GWDT_WCS);

	return 0;
}

static int sbsa_gwdt_stop(struct watchdog_device *wdd)
{
	struct sbsa_gwdt *gwdt = watchdog_get_drvdata(wdd);

	/* Simply write 0 to WCS to clean WCS_EN bit */
	gwdt->soc_reg_ops->reg_write32(0, gwdt->control_base + SBSA_GWDT_WCS);

	return 0;
}

static irqreturn_t sbsa_gwdt_interrupt(int irq, void *dev_id)
{
	panic(WATCHDOG_NAME " timeout");

	return IRQ_HANDLED;
}

static const struct watchdog_info sbsa_gwdt_info = {
	.identity	= WATCHDOG_NAME,
	.options	= WDIOF_SETTIMEOUT |
			  WDIOF_KEEPALIVEPING |
			  WDIOF_MAGICCLOSE |
			  WDIOF_CARDRESET,
};

static const struct watchdog_ops sbsa_gwdt_ops = {
	.owner		= THIS_MODULE,
	.start		= sbsa_gwdt_start,
	.stop		= sbsa_gwdt_stop,
	.ping		= sbsa_gwdt_keepalive,
	.set_timeout	= sbsa_gwdt_set_timeout,
	.get_timeleft	= sbsa_gwdt_get_timeleft,
};

static int sbsa_gwdt_probe(struct platform_device *pdev)
{
	void __iomem *rf_base, *cf_base;
	void __iomem *cpu_ctrl_base = NULL, *mng_base = NULL,
		     *rst_ctrl_base = NULL;
	struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node;
	struct watchdog_device *wdd;
	struct sbsa_gwdt *gwdt;
	struct resource *res;
	int ret, irq;
	bool marvell = false;
	u32 status, id, val;

	gwdt = devm_kzalloc(dev, sizeof(*gwdt), GFP_KERNEL);
	if (!gwdt)
		return -ENOMEM;
	platform_set_drvdata(pdev, gwdt);

	if (of_device_is_compatible(np, "marvell,ac5-wd")) {
		marvell = true;
		gwdt->soc_reg_ops = &smc_reg_ops;
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		if (IS_ERR(res))
			return PTR_ERR(res);
		cf_base = res->start;

		res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
		if (IS_ERR(res))
			return PTR_ERR(res);
		rf_base = res->start;

		res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
		if (IS_ERR(res))
			return PTR_ERR(res);
		cpu_ctrl_base = res->start;
		mng_base = devm_platform_ioremap_resource(pdev, 3);
		if (IS_ERR(mng_base))
			return PTR_ERR(mng_base);
		rst_ctrl_base = devm_platform_ioremap_resource(pdev, 4);
		if (IS_ERR(rst_ctrl_base))
			return PTR_ERR(rst_ctrl_base);
	} else {
		gwdt->soc_reg_ops = &direct_reg_ops;
		cf_base = devm_platform_ioremap_resource(pdev, 0);
		if (IS_ERR(cf_base))
			return PTR_ERR(cf_base);

		rf_base = devm_platform_ioremap_resource(pdev, 1);
		if (IS_ERR(rf_base))
			return PTR_ERR(rf_base);
	}

	/*
	 * Get the frequency of system counter from the cp15 interface of ARM
	 * Generic timer. We don't need to check it, because if it returns "0",
	 * system would panic in very early stage.
	 */
	gwdt->clk = arch_timer_get_cntfrq();
	gwdt->refresh_base = rf_base;
	gwdt->control_base = cf_base;

	wdd = &gwdt->wdd;
	wdd->parent = dev;
	wdd->info = &sbsa_gwdt_info;
	wdd->ops = &sbsa_gwdt_ops;
	wdd->min_timeout = 1;
	wdd->timeout = DEFAULT_TIMEOUT;
	watchdog_set_drvdata(wdd, gwdt);
	watchdog_set_nowayout(wdd, nowayout);
	sbsa_gwdt_get_version(wdd);
	if (gwdt->version == 0)
		wdd->max_hw_heartbeat_ms = U32_MAX / gwdt->clk * 1000;
	else
		wdd->max_hw_heartbeat_ms = GENMASK_ULL(47, 0) / gwdt->clk * 1000;

	status = gwdt->soc_reg_ops->reg_read32(cf_base + SBSA_GWDT_WCS);
	if (status & SBSA_GWDT_WCS_WS1) {
		dev_warn(dev, "System reset by WDT.\n");
		wdd->bootstatus |= WDIOF_CARDRESET;
	}
	if (status & SBSA_GWDT_WCS_EN)
		set_bit(WDOG_HW_RUNNING, &wdd->status);

	if (action) {
		irq = platform_get_irq(pdev, 0);
		if (irq < 0) {
			action = 0;
			dev_warn(dev, "unable to get ws0 interrupt.\n");
		} else {
			/*
			 * In case there is a pending ws0 interrupt, just ping
			 * the watchdog before registering the interrupt routine
			 */
			gwdt->soc_reg_ops->reg_write32(0, rf_base + SBSA_GWDT_WRR);
			if (devm_request_irq(dev, irq, sbsa_gwdt_interrupt, 0,
					     pdev->name, gwdt)) {
				action = 0;
				dev_warn(dev, "unable to request IRQ %d.\n",
					 irq);
			}
		}
		if (!action)
			dev_warn(dev, "falling back to single stage mode.\n");
	}
	/*
	 * In the single stage mode, The first signal (WS0) is ignored,
	 * the timeout is (WOR * 2), so the maximum timeout should be doubled.
	 */
	if (!action)
		wdd->max_hw_heartbeat_ms *= 2;

	watchdog_init_timeout(wdd, timeout, dev);
	/*
	 * Update timeout to WOR.
	 * Because of the explicit watchdog refresh mechanism,
	 * it's also a ping, if watchdog is enabled.
	 */
	sbsa_gwdt_set_timeout(wdd, wdd->timeout);

	watchdog_stop_on_reboot(wdd);
	ret = devm_watchdog_register_device(dev, wdd);
	if (ret)
		return ret;
	/*
	 * Marvell AC5/X/IM: need to configure the watchdog
	 * HW to trigger reset on WS1 (Watchdog Signal 1):
	 *
	 * 1. Configure the watchdog signal enable (routing)
	 *    according to configuration
	 * 2. Unmask the wd_rst input signal to the reset unit
	 */
	if (marvell) {
		gwdt->soc_reg_ops->reg_write32(reset, cpu_ctrl_base +
					       SBSA_GWDT_MARVELL_CPU_WD_RST_EN_REG);
		id = readl(mng_base + SBSA_GWDT_MARVELL_MNG_ID_REG) &
			   SBSA_GWDT_MARVELL_ID_MASK;

		if (id == SBSA_GWDT_MARVELL_AC5_ID)
			val = SBSA_GWDT_MARVELL_AC5_RST_UNIT_WD_BIT;
		else
			val = SBSA_GWDT_MARVELL_IRONMAN_RST_UNIT_WD_BIT;

		writel(readl(rst_ctrl_base + SBSA_GWDT_MARVELL_RST_CTRL_REG) & ~val,
		       rst_ctrl_base + SBSA_GWDT_MARVELL_RST_CTRL_REG);
	}
	dev_info(dev, "Initialized with %ds timeout @ %u Hz, action=%d.%s\n",
		 wdd->timeout, gwdt->clk, action,
		 status & SBSA_GWDT_WCS_EN ? " [enabled]" : "");

	return 0;
}

/* Disable watchdog if it is active during suspend */
static int __maybe_unused sbsa_gwdt_suspend(struct device *dev)
{
	struct sbsa_gwdt *gwdt = dev_get_drvdata(dev);

	if (watchdog_hw_running(&gwdt->wdd))
		sbsa_gwdt_stop(&gwdt->wdd);

	return 0;
}

/* Enable watchdog if necessary */
static int __maybe_unused sbsa_gwdt_resume(struct device *dev)
{
	struct sbsa_gwdt *gwdt = dev_get_drvdata(dev);

	if (watchdog_hw_running(&gwdt->wdd))
		sbsa_gwdt_start(&gwdt->wdd);

	return 0;
}

static const struct dev_pm_ops sbsa_gwdt_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(sbsa_gwdt_suspend, sbsa_gwdt_resume)
};

static const struct of_device_id sbsa_gwdt_of_match[] = {
	{ .compatible = "arm,sbsa-gwdt", },
	{ .compatible = "marvell,ac5-wd", },
	{},
};
MODULE_DEVICE_TABLE(of, sbsa_gwdt_of_match);

static const struct platform_device_id sbsa_gwdt_pdev_match[] = {
	{ .name = DRV_NAME, },
	{},
};
MODULE_DEVICE_TABLE(platform, sbsa_gwdt_pdev_match);

static struct platform_driver sbsa_gwdt_driver = {
	.driver = {
		.name = DRV_NAME,
		.pm = &sbsa_gwdt_pm_ops,
		.of_match_table = sbsa_gwdt_of_match,
	},
	.probe = sbsa_gwdt_probe,
	.id_table = sbsa_gwdt_pdev_match,
};

module_platform_driver(sbsa_gwdt_driver);

MODULE_DESCRIPTION("SBSA Generic Watchdog Driver");
MODULE_AUTHOR("Fu Wei <fu.wei@linaro.org>");
MODULE_AUTHOR("Suravee Suthikulpanit <Suravee.Suthikulpanit@amd.com>");
MODULE_AUTHOR("Al Stone <al.stone@linaro.org>");
MODULE_AUTHOR("Timur Tabi <timur@codeaurora.org>");
MODULE_LICENSE("GPL v2");
