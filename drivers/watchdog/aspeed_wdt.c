// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2016 IBM Corporation
 *
 * Joel Stanley <joel@jms.id.au>
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default="
				__MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

struct aspeed_wdt_config {
	u32 ext_pulse_width_mask;
	u32 irq_shift;
	u32 irq_mask;
	const struct attribute_group *reset_ctrl_group;
};

struct aspeed_wdt {
	struct watchdog_device	wdd;
	void __iomem		*base;
	u32			ctrl;
	const struct aspeed_wdt_config *cfg;
	const struct attribute_group *groups[3]; /* bswitch_group, reset ctrl, NULL terminator */
	spinlock_t		lock;
};

#define WDT_STATUS		0x00
#define WDT_RELOAD_VALUE	0x04
#define WDT_RESTART		0x08
#define WDT_CTRL		0x0C
#define   WDT_CTRL_BOOT_SECONDARY	BIT(7)
#define   WDT_CTRL_RESET_MODE_SOC	(0x00 << 5)
#define   WDT_CTRL_RESET_MODE_FULL_CHIP	(0x01 << 5)
#define   WDT_CTRL_RESET_MODE_ARM_CPU	(0x10 << 5)
#define   WDT_CTRL_1MHZ_CLK		BIT(4)
#define   WDT_CTRL_WDT_EXT		BIT(3)
#define   WDT_CTRL_WDT_INTR		BIT(2)
#define   WDT_CTRL_RESET_SYSTEM		BIT(1)
#define   WDT_CTRL_ENABLE		BIT(0)
#define WDT_TIMEOUT_STATUS	0x10
#define   WDT_TIMEOUT_STATUS_IRQ		BIT(2)
#define   WDT_TIMEOUT_STATUS_BOOT_SECONDARY	BIT(1)
#define WDT_CLEAR_TIMEOUT_STATUS	0x14
#define   WDT_CLEAR_TIMEOUT_AND_BOOT_CODE_SELECTION	BIT(0)
#define WDT_RESET_MASK1		0x1c
#define WDT_RESET_MASK2		0x20

/*
 * WDT_RESET_WIDTH controls the characteristics of the external pulse (if
 * enabled), specifically:
 *
 * * Pulse duration
 * * Drive mode: push-pull vs open-drain
 * * Polarity: Active high or active low
 *
 * Pulse duration configuration is available on both the AST2400 and AST2500,
 * though the field changes between SoCs:
 *
 * AST2400: Bits 7:0
 * AST2500: Bits 19:0
 *
 * This difference is captured in struct aspeed_wdt_config.
 *
 * The AST2500 exposes the drive mode and polarity options, but not in a
 * regular fashion. For read purposes, bit 31 represents active high or low,
 * and bit 30 represents push-pull or open-drain. With respect to write, magic
 * values need to be written to the top byte to change the state of the drive
 * mode and polarity bits. Any other value written to the top byte has no
 * effect on the state of the drive mode or polarity bits. However, the pulse
 * width value must be preserved (as desired) if written.
 */
#define WDT_RESET_WIDTH		0x18
#define   WDT_RESET_WIDTH_ACTIVE_HIGH	BIT(31)
#define     WDT_ACTIVE_HIGH_MAGIC	(0xA5 << 24)
#define     WDT_ACTIVE_LOW_MAGIC	(0x5A << 24)
#define   WDT_RESET_WIDTH_PUSH_PULL	BIT(30)
#define     WDT_PUSH_PULL_MAGIC		(0xA8 << 24)
#define     WDT_OPEN_DRAIN_MAGIC	(0x8A << 24)

#define WDT_RESTART_MAGIC	0x4755

/* 32 bits at 1MHz, in milliseconds */
#define WDT_MAX_TIMEOUT_MS	4294967
#define WDT_DEFAULT_TIMEOUT	30
#define WDT_RATE_1MHZ		1000000

static struct aspeed_wdt *to_aspeed_wdt(struct watchdog_device *wdd)
{
	return container_of(wdd, struct aspeed_wdt, wdd);
}

static void aspeed_wdt_enable(struct aspeed_wdt *wdt, int count)
{
	wdt->ctrl |= WDT_CTRL_ENABLE;

	writel(0, wdt->base + WDT_CTRL);
	writel(count, wdt->base + WDT_RELOAD_VALUE);
	writel(WDT_RESTART_MAGIC, wdt->base + WDT_RESTART);
	writel(wdt->ctrl, wdt->base + WDT_CTRL);
}

static int aspeed_wdt_start(struct watchdog_device *wdd)
{
	struct aspeed_wdt *wdt = to_aspeed_wdt(wdd);

	aspeed_wdt_enable(wdt, wdd->timeout * WDT_RATE_1MHZ);

	return 0;
}

static int aspeed_wdt_stop(struct watchdog_device *wdd)
{
	struct aspeed_wdt *wdt = to_aspeed_wdt(wdd);

	wdt->ctrl &= ~WDT_CTRL_ENABLE;
	writel(wdt->ctrl, wdt->base + WDT_CTRL);

	return 0;
}

static int aspeed_wdt_ping(struct watchdog_device *wdd)
{
	struct aspeed_wdt *wdt = to_aspeed_wdt(wdd);

	writel(WDT_RESTART_MAGIC, wdt->base + WDT_RESTART);

	return 0;
}

static int aspeed_wdt_set_timeout(struct watchdog_device *wdd,
				  unsigned int timeout)
{
	struct aspeed_wdt *wdt = to_aspeed_wdt(wdd);
	u32 actual;

	wdd->timeout = timeout;

	actual = min(timeout, wdd->max_hw_heartbeat_ms / 1000);

	writel(actual * WDT_RATE_1MHZ, wdt->base + WDT_RELOAD_VALUE);
	writel(WDT_RESTART_MAGIC, wdt->base + WDT_RESTART);

	return 0;
}

static int aspeed_wdt_set_pretimeout(struct watchdog_device *wdd,
				     unsigned int pretimeout)
{
	struct aspeed_wdt *wdt = to_aspeed_wdt(wdd);
	u32 actual = pretimeout * WDT_RATE_1MHZ;
	u32 s = wdt->cfg->irq_shift;
	u32 m = wdt->cfg->irq_mask;

	wdd->pretimeout = pretimeout;
	wdt->ctrl &= ~m;
	if (pretimeout)
		wdt->ctrl |= ((actual << s) & m) | WDT_CTRL_WDT_INTR;
	else
		wdt->ctrl &= ~WDT_CTRL_WDT_INTR;

	writel(wdt->ctrl, wdt->base + WDT_CTRL);

	return 0;
}

static int aspeed_wdt_restart(struct watchdog_device *wdd,
			      unsigned long action, void *data)
{
	struct aspeed_wdt *wdt = to_aspeed_wdt(wdd);

	wdt->ctrl &= ~WDT_CTRL_BOOT_SECONDARY;
	aspeed_wdt_enable(wdt, 128 * WDT_RATE_1MHZ / 1000);

	mdelay(1000);

	return 0;
}

/* access_cs0 shows if cs0 is accessible, hence the reverted bit */
static ssize_t access_cs0_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct aspeed_wdt *wdt = dev_get_drvdata(dev);
	u32 status = readl(wdt->base + WDT_TIMEOUT_STATUS);

	return sysfs_emit(buf, "%u\n",
			  !(status & WDT_TIMEOUT_STATUS_BOOT_SECONDARY));
}

static ssize_t access_cs0_store(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t size)
{
	struct aspeed_wdt *wdt = dev_get_drvdata(dev);
	unsigned long val;

	if (kstrtoul(buf, 10, &val))
		return -EINVAL;

	if (val)
		writel(WDT_CLEAR_TIMEOUT_AND_BOOT_CODE_SELECTION,
		       wdt->base + WDT_CLEAR_TIMEOUT_STATUS);

	return size;
}

/*
 * This attribute exists only if the system has booted from the alternate
 * flash with 'alt-boot' option.
 *
 * At alternate flash the 'access_cs0' sysfs node provides:
 *   ast2400: a way to get access to the primary SPI flash chip at CS0
 *            after booting from the alternate chip at CS1.
 *   ast2500: a way to restore the normal address mapping from
 *            (CS0->CS1, CS1->CS0) to (CS0->CS0, CS1->CS1).
 *
 * Clearing the boot code selection and timeout counter also resets to the
 * initial state the chip select line mapping. When the SoC is in normal
 * mapping state (i.e. booted from CS0), clearing those bits does nothing for
 * both versions of the SoC. For alternate boot mode (booted from CS1 due to
 * wdt2 expiration) the behavior differs as described above.
 *
 * This option can be used with wdt2 (watchdog1) only.
 */
static DEVICE_ATTR_RW(access_cs0);

static struct attribute *bswitch_attrs[] = {
	&dev_attr_access_cs0.attr,
	NULL
};

static const struct attribute_group bswitch_group = {
	.attrs = bswitch_attrs,
};

struct aspeed_wdt_rstctrl_bit {
	struct device_attribute dev_attr;
	u8 reg, bit;
};

static ssize_t aspeed_wdt_reset_ctrl_show(struct device *dev, struct device_attribute *attr,
					  char *buf)
{
	struct aspeed_wdt *wdt = dev_get_drvdata(dev);
	struct aspeed_wdt_rstctrl_bit *bit = container_of(attr, struct aspeed_wdt_rstctrl_bit,
							  dev_attr);
	u32 mask = readl(wdt->base + bit->reg);

	return sysfs_emit(buf, "%u\n", !!(mask & BIT(bit->bit)));
}

static ssize_t aspeed_wdt_reset_ctrl_store(struct device *dev, struct device_attribute *attr,
					   const char *buf, size_t size)
{
	struct aspeed_wdt *wdt = dev_get_drvdata(dev);
	struct aspeed_wdt_rstctrl_bit *bit = container_of(attr, struct aspeed_wdt_rstctrl_bit,
							  dev_attr);
	u32 mask;
	bool val;

	if (kstrtobool(buf, &val))
		return -EINVAL;

	spin_lock(&wdt->lock);
	mask = readl(wdt->base + bit->reg);
	if (val)
		mask |= BIT(bit->bit);
	else
		mask &= ~BIT(bit->bit);
	writel(mask, wdt->base + bit->reg);
	spin_unlock(&wdt->lock);

	return size;
}

#define ASPEED_WDT_RSTCTRL_BIT(chip, name, regnum, bitnum)			\
	static struct aspeed_wdt_rstctrl_bit chip##_##name##_reset_ctrl = {	\
		.dev_attr = __ATTR(name, 0644, aspeed_wdt_reset_ctrl_show,	\
				   aspeed_wdt_reset_ctrl_store),		\
		.reg = regnum,							\
		.bit = bitnum,							\
	}

#define AST2500_WDT_RESET_CTRL(name, bit) \
	ASPEED_WDT_RSTCTRL_BIT(ast2500, name, WDT_RESET_MASK1, bit)

AST2500_WDT_RESET_CTRL(spi, 24);
AST2500_WDT_RESET_CTRL(xdma, 23);
AST2500_WDT_RESET_CTRL(mctp, 22);
AST2500_WDT_RESET_CTRL(gpio, 21);
AST2500_WDT_RESET_CTRL(adc, 20);
AST2500_WDT_RESET_CTRL(jtag, 19);
AST2500_WDT_RESET_CTRL(peci, 18);
AST2500_WDT_RESET_CTRL(pwm, 17);
AST2500_WDT_RESET_CTRL(crt, 16);
AST2500_WDT_RESET_CTRL(mic, 15);
AST2500_WDT_RESET_CTRL(sdio, 14);
AST2500_WDT_RESET_CTRL(lpc, 13);
AST2500_WDT_RESET_CTRL(hac, 12);
AST2500_WDT_RESET_CTRL(video, 11);
AST2500_WDT_RESET_CTRL(hid_ehci, 10);
AST2500_WDT_RESET_CTRL(usb_host, 9);
AST2500_WDT_RESET_CTRL(usb2_host_hub, 8);
AST2500_WDT_RESET_CTRL(graphics, 7);
AST2500_WDT_RESET_CTRL(mac1, 6);
AST2500_WDT_RESET_CTRL(mac0, 5);
AST2500_WDT_RESET_CTRL(i2c, 4);
AST2500_WDT_RESET_CTRL(ahb, 3);
AST2500_WDT_RESET_CTRL(sdram, 2);
AST2500_WDT_RESET_CTRL(coproc, 1);

static struct attribute *ast2500_reset_ctrl_attrs[] = {
	&ast2500_spi_reset_ctrl.dev_attr.attr,
	&ast2500_xdma_reset_ctrl.dev_attr.attr,
	&ast2500_mctp_reset_ctrl.dev_attr.attr,
	&ast2500_gpio_reset_ctrl.dev_attr.attr,
	&ast2500_adc_reset_ctrl.dev_attr.attr,
	&ast2500_jtag_reset_ctrl.dev_attr.attr,
	&ast2500_peci_reset_ctrl.dev_attr.attr,
	&ast2500_pwm_reset_ctrl.dev_attr.attr,
	&ast2500_crt_reset_ctrl.dev_attr.attr,
	&ast2500_mic_reset_ctrl.dev_attr.attr,
	&ast2500_sdio_reset_ctrl.dev_attr.attr,
	&ast2500_lpc_reset_ctrl.dev_attr.attr,
	&ast2500_hac_reset_ctrl.dev_attr.attr,
	&ast2500_video_reset_ctrl.dev_attr.attr,
	&ast2500_hid_ehci_reset_ctrl.dev_attr.attr,
	&ast2500_usb_host_reset_ctrl.dev_attr.attr,
	&ast2500_usb2_host_hub_reset_ctrl.dev_attr.attr,
	&ast2500_graphics_reset_ctrl.dev_attr.attr,
	&ast2500_mac1_reset_ctrl.dev_attr.attr,
	&ast2500_mac0_reset_ctrl.dev_attr.attr,
	&ast2500_i2c_reset_ctrl.dev_attr.attr,
	&ast2500_ahb_reset_ctrl.dev_attr.attr,
	&ast2500_sdram_reset_ctrl.dev_attr.attr,
	&ast2500_coproc_reset_ctrl.dev_attr.attr,
	NULL
};

static const struct attribute_group ast2500_reset_ctrl_group = {
	.name = "reset_ctrl",
	.attrs = ast2500_reset_ctrl_attrs,
};

#define AST2600_WDT_RESET_CTRL(name, reg, bit) \
	ASPEED_WDT_RSTCTRL_BIT(ast2600, name, reg, bit)

AST2600_WDT_RESET_CTRL(rvas, WDT_RESET_MASK1, 25);
AST2600_WDT_RESET_CTRL(gpio0, WDT_RESET_MASK1, 24);
AST2600_WDT_RESET_CTRL(xdma1, WDT_RESET_MASK1, 23);
AST2600_WDT_RESET_CTRL(xdma0, WDT_RESET_MASK1, 22);
AST2600_WDT_RESET_CTRL(mctp1, WDT_RESET_MASK1, 21);
AST2600_WDT_RESET_CTRL(mctp0, WDT_RESET_MASK1, 20);
AST2600_WDT_RESET_CTRL(jtag0, WDT_RESET_MASK1, 19);
AST2600_WDT_RESET_CTRL(sdio0, WDT_RESET_MASK1, 18);
AST2600_WDT_RESET_CTRL(mac1, WDT_RESET_MASK1, 17);
AST2600_WDT_RESET_CTRL(mac0, WDT_RESET_MASK1, 16);
AST2600_WDT_RESET_CTRL(gp_mcu, WDT_RESET_MASK1, 15);
AST2600_WDT_RESET_CTRL(dp_mcu, WDT_RESET_MASK1, 14);
AST2600_WDT_RESET_CTRL(dp, WDT_RESET_MASK1, 13);
AST2600_WDT_RESET_CTRL(hac, WDT_RESET_MASK1, 12);
AST2600_WDT_RESET_CTRL(video, WDT_RESET_MASK1, 11);
AST2600_WDT_RESET_CTRL(crt, WDT_RESET_MASK1, 10);
AST2600_WDT_RESET_CTRL(graphics, WDT_RESET_MASK1, 9);
AST2600_WDT_RESET_CTRL(uhci, WDT_RESET_MASK1, 8);
AST2600_WDT_RESET_CTRL(usb_b, WDT_RESET_MASK1, 7);
AST2600_WDT_RESET_CTRL(usb_a, WDT_RESET_MASK1, 6);
AST2600_WDT_RESET_CTRL(coproc, WDT_RESET_MASK1, 5);
AST2600_WDT_RESET_CTRL(sli, WDT_RESET_MASK1, 3);
AST2600_WDT_RESET_CTRL(ahb, WDT_RESET_MASK1, 2);
AST2600_WDT_RESET_CTRL(sdram, WDT_RESET_MASK1, 1);

AST2600_WDT_RESET_CTRL(espi, WDT_RESET_MASK2, 26);
AST2600_WDT_RESET_CTRL(i3c5, WDT_RESET_MASK2, 23);
AST2600_WDT_RESET_CTRL(i3c4, WDT_RESET_MASK2, 22);
AST2600_WDT_RESET_CTRL(i3c3, WDT_RESET_MASK2, 21);
AST2600_WDT_RESET_CTRL(i3c2, WDT_RESET_MASK2, 20);
AST2600_WDT_RESET_CTRL(i3c1, WDT_RESET_MASK2, 19);
AST2600_WDT_RESET_CTRL(i3c0, WDT_RESET_MASK2, 18);
AST2600_WDT_RESET_CTRL(i3c_global, WDT_RESET_MASK2, 17);
AST2600_WDT_RESET_CTRL(i2c, WDT_RESET_MASK2, 16);
AST2600_WDT_RESET_CTRL(fsi, WDT_RESET_MASK2, 15);
AST2600_WDT_RESET_CTRL(adc, WDT_RESET_MASK2, 14);
AST2600_WDT_RESET_CTRL(pwm, WDT_RESET_MASK2, 13);
AST2600_WDT_RESET_CTRL(peci, WDT_RESET_MASK2, 12);
AST2600_WDT_RESET_CTRL(lpc, WDT_RESET_MASK2, 11);
AST2600_WDT_RESET_CTRL(mdio, WDT_RESET_MASK2, 10);
AST2600_WDT_RESET_CTRL(gpio1, WDT_RESET_MASK2, 9);
AST2600_WDT_RESET_CTRL(jtag1, WDT_RESET_MASK2, 8);
AST2600_WDT_RESET_CTRL(sdio1, WDT_RESET_MASK2, 7);
AST2600_WDT_RESET_CTRL(mac3, WDT_RESET_MASK2, 6);
AST2600_WDT_RESET_CTRL(mac2, WDT_RESET_MASK2, 5);
AST2600_WDT_RESET_CTRL(sli2, WDT_RESET_MASK2, 3);
AST2600_WDT_RESET_CTRL(ahb2, WDT_RESET_MASK2, 2);
AST2600_WDT_RESET_CTRL(spi, WDT_RESET_MASK2, 1);

static struct attribute *ast2600_reset_ctrl_attrs[] = {
	&ast2600_rvas_reset_ctrl.dev_attr.attr,
	&ast2600_gpio0_reset_ctrl.dev_attr.attr,
	&ast2600_xdma1_reset_ctrl.dev_attr.attr,
	&ast2600_xdma0_reset_ctrl.dev_attr.attr,
	&ast2600_mctp1_reset_ctrl.dev_attr.attr,
	&ast2600_mctp0_reset_ctrl.dev_attr.attr,
	&ast2600_jtag0_reset_ctrl.dev_attr.attr,
	&ast2600_sdio0_reset_ctrl.dev_attr.attr,
	&ast2600_mac1_reset_ctrl.dev_attr.attr,
	&ast2600_mac0_reset_ctrl.dev_attr.attr,
	&ast2600_gp_mcu_reset_ctrl.dev_attr.attr,
	&ast2600_dp_mcu_reset_ctrl.dev_attr.attr,
	&ast2600_dp_reset_ctrl.dev_attr.attr,
	&ast2600_hac_reset_ctrl.dev_attr.attr,
	&ast2600_video_reset_ctrl.dev_attr.attr,
	&ast2600_crt_reset_ctrl.dev_attr.attr,
	&ast2600_graphics_reset_ctrl.dev_attr.attr,
	&ast2600_uhci_reset_ctrl.dev_attr.attr,
	&ast2600_usb_b_reset_ctrl.dev_attr.attr,
	&ast2600_usb_a_reset_ctrl.dev_attr.attr,
	&ast2600_coproc_reset_ctrl.dev_attr.attr,
	&ast2600_sli_reset_ctrl.dev_attr.attr,
	&ast2600_ahb_reset_ctrl.dev_attr.attr,
	&ast2600_sdram_reset_ctrl.dev_attr.attr,
	&ast2600_espi_reset_ctrl.dev_attr.attr,
	&ast2600_i3c5_reset_ctrl.dev_attr.attr,
	&ast2600_i3c4_reset_ctrl.dev_attr.attr,
	&ast2600_i3c3_reset_ctrl.dev_attr.attr,
	&ast2600_i3c2_reset_ctrl.dev_attr.attr,
	&ast2600_i3c1_reset_ctrl.dev_attr.attr,
	&ast2600_i3c0_reset_ctrl.dev_attr.attr,
	&ast2600_i3c_global_reset_ctrl.dev_attr.attr,
	&ast2600_i2c_reset_ctrl.dev_attr.attr,
	&ast2600_fsi_reset_ctrl.dev_attr.attr,
	&ast2600_adc_reset_ctrl.dev_attr.attr,
	&ast2600_pwm_reset_ctrl.dev_attr.attr,
	&ast2600_peci_reset_ctrl.dev_attr.attr,
	&ast2600_lpc_reset_ctrl.dev_attr.attr,
	&ast2600_mdio_reset_ctrl.dev_attr.attr,
	&ast2600_gpio1_reset_ctrl.dev_attr.attr,
	&ast2600_jtag1_reset_ctrl.dev_attr.attr,
	&ast2600_sdio1_reset_ctrl.dev_attr.attr,
	&ast2600_mac3_reset_ctrl.dev_attr.attr,
	&ast2600_mac2_reset_ctrl.dev_attr.attr,
	&ast2600_sli2_reset_ctrl.dev_attr.attr,
	&ast2600_ahb2_reset_ctrl.dev_attr.attr,
	&ast2600_spi_reset_ctrl.dev_attr.attr,
	NULL
};

static const struct attribute_group ast2600_reset_ctrl_group = {
	.name = "reset_ctrl",
	.attrs = ast2600_reset_ctrl_attrs,
};

static const struct watchdog_ops aspeed_wdt_ops = {
	.start		= aspeed_wdt_start,
	.stop		= aspeed_wdt_stop,
	.ping		= aspeed_wdt_ping,
	.set_timeout	= aspeed_wdt_set_timeout,
	.set_pretimeout = aspeed_wdt_set_pretimeout,
	.restart	= aspeed_wdt_restart,
	.owner		= THIS_MODULE,
};

static const struct watchdog_info aspeed_wdt_info = {
	.options	= WDIOF_KEEPALIVEPING
			| WDIOF_MAGICCLOSE
			| WDIOF_SETTIMEOUT,
	.identity	= KBUILD_MODNAME,
};

static const struct watchdog_info aspeed_wdt_pretimeout_info = {
	.options	= WDIOF_KEEPALIVEPING
			| WDIOF_PRETIMEOUT
			| WDIOF_MAGICCLOSE
			| WDIOF_SETTIMEOUT,
	.identity	= KBUILD_MODNAME,
};

static irqreturn_t aspeed_wdt_irq(int irq, void *arg)
{
	struct watchdog_device *wdd = arg;
	struct aspeed_wdt *wdt = to_aspeed_wdt(wdd);
	u32 status = readl(wdt->base + WDT_TIMEOUT_STATUS);

	if (status & WDT_TIMEOUT_STATUS_IRQ)
		watchdog_notify_pretimeout(wdd);

	return IRQ_HANDLED;
}

static const struct aspeed_wdt_config ast2400_config = {
	.ext_pulse_width_mask = 0xff,
	.irq_shift = 0,
	.irq_mask = 0,
};

static const struct aspeed_wdt_config ast2500_config = {
	.ext_pulse_width_mask = 0xfffff,
	.irq_shift = 12,
	.irq_mask = GENMASK(31, 12),
	.reset_ctrl_group = &ast2500_reset_ctrl_group,
};

static const struct aspeed_wdt_config ast2600_config = {
	.ext_pulse_width_mask = 0xfffff,
	.irq_shift = 0,
	.irq_mask = GENMASK(31, 10),
	.reset_ctrl_group = &ast2600_reset_ctrl_group,
};

static const struct of_device_id aspeed_wdt_of_table[] = {
	{ .compatible = "aspeed,ast2400-wdt", .data = &ast2400_config },
	{ .compatible = "aspeed,ast2500-wdt", .data = &ast2500_config },
	{ .compatible = "aspeed,ast2600-wdt", .data = &ast2600_config },
	{ },
};
MODULE_DEVICE_TABLE(of, aspeed_wdt_of_table);

static int aspeed_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct of_device_id *ofdid;
	struct aspeed_wdt *wdt;
	struct device_node *np;
	const char *reset_type;
	u32 duration;
	u32 status;
	int ret;
	int ngroups = 0;

	wdt = devm_kzalloc(dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;

	np = dev->of_node;

	ofdid = of_match_node(aspeed_wdt_of_table, np);
	if (!ofdid)
		return -EINVAL;
	wdt->cfg = ofdid->data;

	wdt->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(wdt->base))
		return PTR_ERR(wdt->base);

	spin_lock_init(&wdt->lock);

	wdt->wdd.info = &aspeed_wdt_info;

	if (wdt->cfg->irq_mask) {
		int irq = platform_get_irq_optional(pdev, 0);

		if (irq > 0) {
			ret = devm_request_irq(dev, irq, aspeed_wdt_irq,
					       IRQF_SHARED, dev_name(dev),
					       wdt);
			if (ret)
				return ret;

			wdt->wdd.info = &aspeed_wdt_pretimeout_info;
		}
	}

	wdt->wdd.ops = &aspeed_wdt_ops;
	wdt->wdd.max_hw_heartbeat_ms = WDT_MAX_TIMEOUT_MS;
	wdt->wdd.parent = dev;
	wdt->wdd.groups = wdt->groups;

	wdt->wdd.timeout = WDT_DEFAULT_TIMEOUT;
	watchdog_init_timeout(&wdt->wdd, 0, dev);

	watchdog_set_nowayout(&wdt->wdd, nowayout);

	/*
	 * On clock rates:
	 *  - ast2400 wdt can run at PCLK, or 1MHz
	 *  - ast2500 only runs at 1MHz, hard coding bit 4 to 1
	 *  - ast2600 always runs at 1MHz
	 *
	 * Set the ast2400 to run at 1MHz as it simplifies the driver.
	 */
	if (of_device_is_compatible(np, "aspeed,ast2400-wdt"))
		wdt->ctrl = WDT_CTRL_1MHZ_CLK;

	/*
	 * Control reset on a per-device basis to ensure the
	 * host is not affected by a BMC reboot
	 */
	ret = of_property_read_string(np, "aspeed,reset-type", &reset_type);
	if (ret) {
		wdt->ctrl |= WDT_CTRL_RESET_MODE_SOC | WDT_CTRL_RESET_SYSTEM;
	} else {
		if (!strcmp(reset_type, "cpu"))
			wdt->ctrl |= WDT_CTRL_RESET_MODE_ARM_CPU |
				     WDT_CTRL_RESET_SYSTEM;
		else if (!strcmp(reset_type, "soc"))
			wdt->ctrl |= WDT_CTRL_RESET_MODE_SOC |
				     WDT_CTRL_RESET_SYSTEM;
		else if (!strcmp(reset_type, "system"))
			wdt->ctrl |= WDT_CTRL_RESET_MODE_FULL_CHIP |
				     WDT_CTRL_RESET_SYSTEM;
		else if (strcmp(reset_type, "none"))
			return -EINVAL;
	}
	if (of_property_read_bool(np, "aspeed,external-signal"))
		wdt->ctrl |= WDT_CTRL_WDT_EXT;
	if (of_property_read_bool(np, "aspeed,alt-boot"))
		wdt->ctrl |= WDT_CTRL_BOOT_SECONDARY;

	if (readl(wdt->base + WDT_CTRL) & WDT_CTRL_ENABLE)  {
		/*
		 * The watchdog is running, but invoke aspeed_wdt_start() to
		 * write wdt->ctrl to WDT_CTRL to ensure the watchdog's
		 * configuration conforms to the driver's expectations.
		 * Primarily, ensure we're using the 1MHz clock source.
		 */
		aspeed_wdt_start(&wdt->wdd);
		set_bit(WDOG_HW_RUNNING, &wdt->wdd.status);
	}

	if ((of_device_is_compatible(np, "aspeed,ast2500-wdt")) ||
		(of_device_is_compatible(np, "aspeed,ast2600-wdt"))) {
		u32 reg = readl(wdt->base + WDT_RESET_WIDTH);

		reg &= wdt->cfg->ext_pulse_width_mask;
		if (of_property_read_bool(np, "aspeed,ext-active-high"))
			reg |= WDT_ACTIVE_HIGH_MAGIC;
		else
			reg |= WDT_ACTIVE_LOW_MAGIC;

		writel(reg, wdt->base + WDT_RESET_WIDTH);

		reg &= wdt->cfg->ext_pulse_width_mask;
		if (of_property_read_bool(np, "aspeed,ext-push-pull"))
			reg |= WDT_PUSH_PULL_MAGIC;
		else
			reg |= WDT_OPEN_DRAIN_MAGIC;

		writel(reg, wdt->base + WDT_RESET_WIDTH);
	}

	if (!of_property_read_u32(np, "aspeed,ext-pulse-duration", &duration)) {
		u32 max_duration = wdt->cfg->ext_pulse_width_mask + 1;

		if (duration == 0 || duration > max_duration) {
			dev_err(dev, "Invalid pulse duration: %uus\n",
				duration);
			duration = max(1U, min(max_duration, duration));
			dev_info(dev, "Pulse duration set to %uus\n",
				 duration);
		}

		/*
		 * The watchdog is always configured with a 1MHz source, so
		 * there is no need to scale the microsecond value. However we
		 * need to offset it - from the datasheet:
		 *
		 * "This register decides the asserting duration of wdt_ext and
		 * wdt_rstarm signal. The default value is 0xFF. It means the
		 * default asserting duration of wdt_ext and wdt_rstarm is
		 * 256us."
		 *
		 * This implies a value of 0 gives a 1us pulse.
		 */
		writel(duration - 1, wdt->base + WDT_RESET_WIDTH);
	}

	status = readl(wdt->base + WDT_TIMEOUT_STATUS);
	if (status & WDT_TIMEOUT_STATUS_BOOT_SECONDARY) {
		wdt->wdd.bootstatus = WDIOF_CARDRESET;

		if (of_device_is_compatible(np, "aspeed,ast2400-wdt") ||
		    of_device_is_compatible(np, "aspeed,ast2500-wdt"))
			wdt->groups[ngroups++] = &bswitch_group;
	}

	if (wdt->cfg->reset_ctrl_group)
		wdt->groups[ngroups++] = wdt->cfg->reset_ctrl_group;

	dev_set_drvdata(dev, wdt);

	return devm_watchdog_register_device(dev, &wdt->wdd);
}

static struct platform_driver aspeed_watchdog_driver = {
	.probe = aspeed_wdt_probe,
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = aspeed_wdt_of_table,
	},
};

static int __init aspeed_wdt_init(void)
{
	return platform_driver_register(&aspeed_watchdog_driver);
}
arch_initcall(aspeed_wdt_init);

static void __exit aspeed_wdt_exit(void)
{
	platform_driver_unregister(&aspeed_watchdog_driver);
}
module_exit(aspeed_wdt_exit);

MODULE_DESCRIPTION("Aspeed Watchdog Driver");
MODULE_LICENSE("GPL");
