// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Broadcom BCM6362 integrated WLAN backplane host driver
 *
 * The backplane is a big-endian peripheral. Its cores have no DMP
 * wrappers: the agent registers of ChipCommon and of the 802.11 core
 * are emulated on top of the per-core control registers of the WLAN
 * control block.
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/slab.h>

#include <linux/bcma/bcma.h>

#include "bcma_private.h"

#define BCM6362_WLAN_MISC			0x00
#define  BCM6362_WLAN_MISC_FORCE_CLK_ON		BIT(2)
#define  BCM6362_WLAN_MISC_MACRO_DISABLE	BIT(1)
#define  BCM6362_WLAN_MISC_MACRO_SOFT_RESET	BIT(0)
#define BCM6362_WLAN_CC_CONTROL			0x08
#define BCM6362_WLAN_MAC_CONTROL		0x10
#define BCM6362_WLAN_CC_ID_A			0x18
#define BCM6362_WLAN_MAC_ID_A			0x24

/*
 * Per-core control register layout: BCMA_IOCTL in the low 16 bits,
 * BCMA_RESET_CTL_RESET in bit 16.
 */
#define BCM6362_WLAN_CTL_IOCTL			GENMASK(15, 0)
#define BCM6362_WLAN_CTL_RESET			BIT(16)

#define BCM6362_WLAN_IOST_2G_PHY		BIT(0)
#define BCM6362_WLAN_IOST_5G_PHY		BIT(1)

struct bcma_host_soc_bcm6362 {
	struct bcma_bus		bus;
	void __iomem		*wlan_ctrl;
	struct clk		*clk;
	struct reset_control	*rst_wlan;
	struct reset_control	*rst_wlan_ubus;
};

static struct bcma_host_soc_bcm6362 *to_bcm6362(struct bcma_bus *bus)
{
	return container_of(bus, struct bcma_host_soc_bcm6362, bus);
}

static u8 bcma_host_soc_bcm6362_read8(struct bcma_device *core, u16 offset)
{
	return readb(core->io_addr + offset);
}

static u16 bcma_host_soc_bcm6362_read16(struct bcma_device *core, u16 offset)
{
	return ioread16be(core->io_addr + offset);
}

static u32 bcma_host_soc_bcm6362_read32(struct bcma_device *core, u16 offset)
{
	return ioread32be(core->io_addr + offset);
}

static void bcma_host_soc_bcm6362_write8(struct bcma_device *core, u16 offset,
					 u8 value)
{
	writeb(value, core->io_addr + offset);
}

static void bcma_host_soc_bcm6362_write16(struct bcma_device *core, u16 offset,
					  u16 value)
{
	iowrite16be(value, core->io_addr + offset);
}

static void bcma_host_soc_bcm6362_write32(struct bcma_device *core, u16 offset,
					  u32 value)
{
	iowrite32be(value, core->io_addr + offset);
}

#ifdef CONFIG_BCMA_BLOCKIO
/* Buffers are kept in bus byte order, so copy them without swapping. */
static void bcma_host_soc_bcm6362_block_read(struct bcma_device *core,
					     void *buffer, size_t count,
					     u16 offset, u8 reg_width)
{
	void __iomem *addr = core->io_addr + offset;

	switch (reg_width) {
	case sizeof(u8): {
		u8 *buf = buffer;

		while (count) {
			*buf = __raw_readb(addr);
			buf++;
			count--;
		}
		break;
	}
	case sizeof(u16): {
		__be16 *buf = buffer;

		WARN_ON(count & 1);
		while (count) {
			*buf = (__force __be16)__raw_readw(addr);
			buf++;
			count -= 2;
		}
		break;
	}
	case sizeof(u32): {
		__be32 *buf = buffer;

		WARN_ON(count & 3);
		while (count) {
			*buf = (__force __be32)__raw_readl(addr);
			buf++;
			count -= 4;
		}
		break;
	}
	default:
		WARN_ON(1);
	}
}

static void bcma_host_soc_bcm6362_block_write(struct bcma_device *core,
					      const void *buffer, size_t count,
					      u16 offset, u8 reg_width)
{
	void __iomem *addr = core->io_addr + offset;

	switch (reg_width) {
	case sizeof(u8): {
		const u8 *buf = buffer;

		while (count) {
			__raw_writeb(*buf, addr);
			buf++;
			count--;
		}
		break;
	}
	case sizeof(u16): {
		const __be16 *buf = buffer;

		WARN_ON(count & 1);
		while (count) {
			__raw_writew((__force u16)*buf, addr);
			buf++;
			count -= 2;
		}
		break;
	}
	case sizeof(u32): {
		const __be32 *buf = buffer;

		WARN_ON(count & 3);
		while (count) {
			__raw_writel((__force u32)*buf, addr);
			buf++;
			count -= 4;
		}
		break;
	}
	default:
		WARN_ON(1);
	}
}
#endif /* CONFIG_BCMA_BLOCKIO */

static void __iomem *bcma_host_soc_bcm6362_ctl_reg(struct bcma_device *core)
{
	struct bcma_host_soc_bcm6362 *host = to_bcm6362(core->bus);

	switch (core->id.id) {
	case BCMA_CORE_CHIPCOMMON:
		return host->wlan_ctrl + BCM6362_WLAN_CC_CONTROL;
	case BCMA_CORE_80211:
		return host->wlan_ctrl + BCM6362_WLAN_MAC_CONTROL;
	default:
		return NULL;
	}
}

static u32 bcma_host_soc_bcm6362_aread32(struct bcma_device *core, u16 offset)
{
	void __iomem *ctl = bcma_host_soc_bcm6362_ctl_reg(core);

	switch (offset) {
	case BCMA_IOCTL:
		if (!ctl)
			return BCMA_IOCTL_CLK;
		return ioread32be(ctl) & BCM6362_WLAN_CTL_IOCTL;
	case BCMA_IOST:
		/*
		 * The core bits of MacStatus do not reflect the PHY while
		 * the 802.11 core is held in reset, so report what the
		 * macro supports: a dual-band PHY and 64-bit DMA. The bands
		 * actually usable depend on the board antennas.
		 */
		if (core->id.id == BCMA_CORE_80211)
			return BCM6362_WLAN_IOST_2G_PHY |
			       BCM6362_WLAN_IOST_5G_PHY | BCMA_IOST_DMA64;
		return 0;
	case BCMA_RESET_CTL:
		if (ctl && (ioread32be(ctl) & BCM6362_WLAN_CTL_RESET))
			return BCMA_RESET_CTL_RESET;
		return 0;
	case BCMA_RESET_ST:
		return 0;
	default:
		dev_warn_once(core->bus->dev,
			      "core 0x%03x: unhandled agent read at 0x%03x\n",
			      core->id.id, offset);
		return 0;
	}
}

static void bcma_host_soc_bcm6362_awrite32(struct bcma_device *core, u16 offset,
					   u32 value)
{
	void __iomem *ctl = bcma_host_soc_bcm6362_ctl_reg(core);
	u32 val;

	if (!ctl)
		goto unhandled;

	switch (offset) {
	case BCMA_IOCTL:
		val = ioread32be(ctl) & BCM6362_WLAN_CTL_RESET;
		val |= value & BCM6362_WLAN_CTL_IOCTL;
		iowrite32be(val, ctl);
		return;
	case BCMA_RESET_CTL:
		val = ioread32be(ctl);
		if (value & BCMA_RESET_CTL_RESET)
			val |= BCM6362_WLAN_CTL_RESET;
		else
			val &= ~BCM6362_WLAN_CTL_RESET;
		iowrite32be(val, ctl);
		return;
	}

unhandled:
	dev_warn_once(core->bus->dev,
		      "core 0x%03x: unhandled agent write 0x%08x at 0x%03x\n",
		      core->id.id, value, offset);
}

static const struct bcma_host_ops bcma_host_soc_bcm6362_ops = {
	.read8		= bcma_host_soc_bcm6362_read8,
	.read16		= bcma_host_soc_bcm6362_read16,
	.read32		= bcma_host_soc_bcm6362_read32,
	.write8		= bcma_host_soc_bcm6362_write8,
	.write16	= bcma_host_soc_bcm6362_write16,
	.write32	= bcma_host_soc_bcm6362_write32,
#ifdef CONFIG_BCMA_BLOCKIO
	.block_read	= bcma_host_soc_bcm6362_block_read,
	.block_write	= bcma_host_soc_bcm6362_block_write,
#endif
	.aread32	= bcma_host_soc_bcm6362_aread32,
	.awrite32	= bcma_host_soc_bcm6362_awrite32,
};

/*
 * Macro enable sequence from the vendor driver: with the clock forced
 * on and the macro in soft reset, enable the 802.11 core clock with
 * force gated clock set, then release the soft reset and the forces.
 */
static int bcma_host_soc_bcm6362_power_on(struct bcma_host_soc_bcm6362 *host)
{
	void __iomem *base = host->wlan_ctrl;
	int err;

	err = clk_prepare_enable(host->clk);
	if (err)
		return err;
	fsleep(10000);

	reset_control_assert(host->rst_wlan_ubus);
	reset_control_assert(host->rst_wlan);
	fsleep(1000);
	reset_control_deassert(host->rst_wlan_ubus);
	reset_control_deassert(host->rst_wlan);
	fsleep(1000);

	iowrite32be(BCM6362_WLAN_MISC_FORCE_CLK_ON |
		    BCM6362_WLAN_MISC_MACRO_SOFT_RESET,
		    base + BCM6362_WLAN_MISC);
	fsleep(1000);

	iowrite32be(BCMA_IOCTL_FGC | BCMA_IOCTL_CLK,
		    base + BCM6362_WLAN_MAC_CONTROL);
	iowrite32be(BCM6362_WLAN_MISC_FORCE_CLK_ON, base + BCM6362_WLAN_MISC);
	iowrite32be(0, base + BCM6362_WLAN_MISC);
	iowrite32be(BCMA_IOCTL_CLK, base + BCM6362_WLAN_MAC_CONTROL);

	dev_dbg(host->bus.dev, "CC ID A 0x%08x, MAC ID A 0x%08x\n",
		ioread32be(base + BCM6362_WLAN_CC_ID_A),
		ioread32be(base + BCM6362_WLAN_MAC_ID_A));

	return 0;
}

static void bcma_host_soc_bcm6362_power_off(struct bcma_host_soc_bcm6362 *host)
{
	void __iomem *base = host->wlan_ctrl;

	iowrite32be(0, base + BCM6362_WLAN_MAC_CONTROL);
	iowrite32be(BCM6362_WLAN_MISC_MACRO_DISABLE |
		    BCM6362_WLAN_MISC_MACRO_SOFT_RESET,
		    base + BCM6362_WLAN_MISC);
	reset_control_assert(host->rst_wlan);
	reset_control_assert(host->rst_wlan_ubus);
	clk_disable_unprepare(host->clk);
}

static int bcma_host_soc_bcm6362_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct bcma_host_soc_bcm6362 *host;
	struct bcma_bus *bus;
	int err;

	host = devm_kzalloc(dev, sizeof(*host), GFP_KERNEL);
	if (!host)
		return -ENOMEM;

	bus = &host->bus;
	bus->dev = dev;

	bus->mmio = devm_platform_ioremap_resource_byname(pdev, "chipcommon");
	if (IS_ERR(bus->mmio))
		return PTR_ERR(bus->mmio);

	host->wlan_ctrl = devm_platform_ioremap_resource_byname(pdev, "wlan-ctrl");
	if (IS_ERR(host->wlan_ctrl))
		return PTR_ERR(host->wlan_ctrl);

	host->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(host->clk))
		return dev_err_probe(dev, PTR_ERR(host->clk),
				     "failed to get clock\n");

	host->rst_wlan = devm_reset_control_get_exclusive(dev, "wlan");
	if (IS_ERR(host->rst_wlan))
		return dev_err_probe(dev, PTR_ERR(host->rst_wlan),
				     "failed to get wlan reset\n");

	host->rst_wlan_ubus = devm_reset_control_get_exclusive(dev, "wlan-ubus");
	if (IS_ERR(host->rst_wlan_ubus))
		return dev_err_probe(dev, PTR_ERR(host->rst_wlan_ubus),
				     "failed to get wlan-ubus reset\n");

	err = bcma_host_soc_bcm6362_power_on(host);
	if (err)
		return dev_err_probe(dev, err, "failed to enable clock\n");

	bus->hosttype = BCMA_HOSTTYPE_SOC;
	bus->ops = &bcma_host_soc_bcm6362_ops;
	bus->big_endian = true;
	bus->wrapperless = true;

	bcma_init_bus(bus);

	err = bcma_bus_register(bus);
	if (err) {
		bcma_host_soc_bcm6362_power_off(host);
		return err;
	}

	platform_set_drvdata(pdev, host);

	return 0;
}

static void bcma_host_soc_bcm6362_remove(struct platform_device *pdev)
{
	struct bcma_host_soc_bcm6362 *host = platform_get_drvdata(pdev);

	bcma_bus_unregister(&host->bus);
	bcma_host_soc_bcm6362_power_off(host);
}

static const struct of_device_id bcma_host_soc_bcm6362_of_match[] = {
	{ .compatible = "brcm,bcm6362-wlan-bus-axi" },
	{ }
};
MODULE_DEVICE_TABLE(of, bcma_host_soc_bcm6362_of_match);

static struct platform_driver bcma_host_soc_bcm6362_driver = {
	.driver = {
		.name = "bcma-host-soc-bcm6362",
		.of_match_table = bcma_host_soc_bcm6362_of_match,
	},
	.probe		= bcma_host_soc_bcm6362_probe,
	.remove		= bcma_host_soc_bcm6362_remove,
};

int __init bcma_host_soc_bcm6362_register_driver(void)
{
	return platform_driver_register(&bcma_host_soc_bcm6362_driver);
}

void __exit bcma_host_soc_bcm6362_unregister_driver(void)
{
	platform_driver_unregister(&bcma_host_soc_bcm6362_driver);
}
