/*
 * Broadcom specific AMBA
 * System on Chip (SoC) Host
 *
 * Licensed under the GNU/GPL. See COPYING for details.
 */

#include "bcma_private.h"
#include "scan.h"
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/platform_data/bcma_host_soc.h>
#include <linux/bcma/bcma.h>
#include <linux/bcma/bcma_soc.h>

static u8 bcma_host_soc_read8(struct bcma_device *core, u16 offset)
{
	return readb(core->io_addr + offset);
}

static u16 bcma_host_soc_read16(struct bcma_device *core, u16 offset)
{
	return readw(core->io_addr + offset);
}

static u32 bcma_host_soc_read32(struct bcma_device *core, u16 offset)
{
	return readl(core->io_addr + offset);
}

static void bcma_host_soc_write8(struct bcma_device *core, u16 offset,
				 u8 value)
{
	writeb(value, core->io_addr + offset);
}

static void bcma_host_soc_write16(struct bcma_device *core, u16 offset,
				 u16 value)
{
	writew(value, core->io_addr + offset);
}

static void bcma_host_soc_write32(struct bcma_device *core, u16 offset,
				 u32 value)
{
	writel(value, core->io_addr + offset);
}

#ifdef CONFIG_BCMA_BLOCKIO
static void bcma_host_soc_block_read(struct bcma_device *core, void *buffer,
				     size_t count, u16 offset, u8 reg_width)
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
		__le16 *buf = buffer;

		WARN_ON(count & 1);
		while (count) {
			*buf = (__force __le16)__raw_readw(addr);
			buf++;
			count -= 2;
		}
		break;
	}
	case sizeof(u32): {
		__le32 *buf = buffer;

		WARN_ON(count & 3);
		while (count) {
			*buf = (__force __le32)__raw_readl(addr);
			buf++;
			count -= 4;
		}
		break;
	}
	default:
		WARN_ON(1);
	}
}

static void bcma_host_soc_block_write(struct bcma_device *core,
				      const void *buffer,
				      size_t count, u16 offset, u8 reg_width)
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
		const __le16 *buf = buffer;

		WARN_ON(count & 1);
		while (count) {
			__raw_writew((__force u16)(*buf), addr);
			buf++;
			count -= 2;
		}
		break;
	}
	case sizeof(u32): {
		const __le32 *buf = buffer;

		WARN_ON(count & 3);
		while (count) {
			__raw_writel((__force u32)(*buf), addr);
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

static u32 bcma_host_soc_aread32(struct bcma_device *core, u16 offset)
{
	if (WARN_ONCE(!core->io_wrap, "Accessed core has no wrapper/agent\n"))
		return ~0;
	return readl(core->io_wrap + offset);
}

static void bcma_host_soc_awrite32(struct bcma_device *core, u16 offset,
				  u32 value)
{
	if (WARN_ONCE(!core->io_wrap, "Accessed core has no wrapper/agent\n"))
		return;
	writel(value, core->io_wrap + offset);
}

static const struct bcma_host_ops bcma_host_soc_ops = {
	.read8		= bcma_host_soc_read8,
	.read16		= bcma_host_soc_read16,
	.read32		= bcma_host_soc_read32,
	.write8		= bcma_host_soc_write8,
	.write16	= bcma_host_soc_write16,
	.write32	= bcma_host_soc_write32,
#ifdef CONFIG_BCMA_BLOCKIO
	.block_read	= bcma_host_soc_block_read,
	.block_write	= bcma_host_soc_block_write,
#endif
	.aread32	= bcma_host_soc_aread32,
	.awrite32	= bcma_host_soc_awrite32,
};

/* SHIM peephole layout, subset of the OEM "WlanShimRegs" struct: only
 * the per-core Control registers are needed for IOCTL / RESET_CTL
 * routing. The low 16 bits of each Control register map bit-for-bit to
 * BCMA_IOCTL; bit 16 (SICF_WOC_CORE_RESET) is the per-core wrapper
 * BCMA_RESET_CTL bit 0 promoted into the SHIM Control register.
 */
#define BCMA_SHIM_CC_CONTROL		0x08
#define BCMA_SHIM_MAC_CONTROL		0x10
#define   SICF_WOC_CORE_RESET		0x10000

/* Resolve the SHIM Control register for a given core: ChipCommon and
 * the IEEE 802.11 core. Returns NULL for any other core, including the
 * SHIM core itself - the SHIM has been running since boot and needs no
 * gating from bcma_core_enable().
 */
static void __iomem *bcma_host_soc_shim_ctrl_reg(struct bcma_device *core)
{
	void __iomem *shim = core->bus->shim_iomem;

	if (!shim)
		return NULL;

	switch (core->id.id) {
	case BCMA_CORE_CHIPCOMMON:
		return shim + BCMA_SHIM_CC_CONTROL;
	case BCMA_CORE_80211:
		return shim + BCMA_SHIM_MAC_CONTROL;
	}
	return NULL;
}

/* Synthesize wrapper-register responses for cores whose DMP wrapper
 * space does not exist in the standard bcma layout. On SoCs that
 * publish a SHIM-style mini-EROM (BMIPS xDSL family: BCM6362, ...)
 * ChipCommon and the 802.11 core report NMW=NSW=0; clock and reset
 * gating happens in the SHIM's per-core Control register, which is
 * where this synth routes BCMA_IOCTL and BCMA_RESET_CTL accesses.
 */
static u32 bcma_host_soc_synth_aread32(struct bcma_device *core, u16 offset)
{
	void __iomem *ctrl_reg = bcma_host_soc_shim_ctrl_reg(core);

	switch (offset) {
	case BCMA_IOCTL:
		/* Low 16 bits of the SHIM Control register map bit-for-bit
		 * to BCMA_IOCTL. Returning the live value lets
		 * bcma_core_is_enabled() observe a prior disable that
		 * cleared CLOCK_EN/FGC. For cores not in the SHIM map
		 * (e.g. the SHIM core itself) return BCMA_IOCTL_CLK so
		 * the core is treated as already-up; the SHIM has been
		 * running since boot and has nothing to enable.
		 */
		if (ctrl_reg)
			return ioread32be(ctrl_reg) & 0xFFFF;
		return BCMA_IOCTL_CLK;

	case BCMA_IOST:
		/* IOST is synthesized rather than read from the SHIM
		 * Status register: while the d11 is in reset, MacStatus's
		 * SISF_CORE_BITS field is unreliable (observed: 0x1008 on
		 * a disabled d11, where the "2G_PHY" indicator bit 0 is
		 * clear, which would steer b43 down a nonexistent 5 GHz
		 * path on a 2.4 GHz-only single-die part).
		 *
		 * Synthesize a stable IOST for the 802.11 core:
		 *   bit 0  (2G_PHY)         = 1   single-die 2.4 GHz
		 *   bit 1  (5G_PHY)         = 0   no 5 GHz radio wired
		 *   bit 12 (BCMA_IOST_DMA64)= 1   corerev 22 is DMA64
		 *
		 * Other cores have no defined IOST bits of interest.
		 */
		if (core->id.id == BCMA_CORE_80211)
			return 0x01 | BCMA_IOST_DMA64;
		return 0;

	case BCMA_RESET_CTL:
		/* SICF_WOC_CORE_RESET is the wrapper RESET_CTL bit 0 in
		 * the SHIM Control register.
		 */
		if (ctrl_reg)
			return (ioread32be(ctrl_reg) & SICF_WOC_CORE_RESET) ? 1 : 0;
		return 0;

	case BCMA_RESET_ST:
		/* No "reset pending" semantics in the SHIM Control reg. */
		return 0;

	default:
		pr_info("bcma: synth aread32 unhandled offset 0x%03x on core idx=%u id=0x%x\n",
			offset, core->core_index, core->id.id);
		return 0;
	}
}

static void bcma_host_soc_synth_awrite32(struct bcma_device *core,
					 u16 offset, u32 value)
{
	void __iomem *ctrl_reg = bcma_host_soc_shim_ctrl_reg(core);
	u32 cur, new_val;

	if (ctrl_reg) {
		switch (offset) {
		case BCMA_IOCTL:
			/* SICF low 16 bits == BCMA_IOCTL. Preserve
			 * SICF_WOC_CORE_RESET (the RESET_CTL view) so an
			 * IOCTL write does not accidentally release reset.
			 */
			cur = ioread32be(ctrl_reg);
			new_val = (value & 0xFFFF) |
				  (cur & SICF_WOC_CORE_RESET);
			iowrite32be(new_val, ctrl_reg);
			pr_debug("bcma: synth IOCTL core=0x%x SHIM %08x->%08x (req %08x)\n",
				 core->id.id, cur, new_val, value);
			return;
		case BCMA_RESET_CTL:
			cur = ioread32be(ctrl_reg);
			if (value & 1)
				new_val = cur | SICF_WOC_CORE_RESET;
			else
				new_val = cur & ~SICF_WOC_CORE_RESET;
			iowrite32be(new_val, ctrl_reg);
			pr_debug("bcma: synth RESET_CTL core=0x%x SHIM %08x->%08x (req %08x)\n",
				 core->id.id, cur, new_val, value);
			return;
		}
	}

	pr_info("bcma: synth awrite32 dropped on core idx=%u id=0x%x offset=0x%03x value=0x%08x\n",
		core->core_index, core->id.id, offset, value);
}

/* Big-endian accessor variants for SoCs whose AXI backplane sits on a
 * big-endian peripheral bus (BMIPS xDSL family). read8/write8 are
 * endian-agnostic byte accesses and reuse the LE helpers above.
 * CONFIG_BCMA_BLOCKIO block_read/write are intentionally omitted: those
 * targets do not enable block I/O. aread32/awrite32 dispatch to the
 * synthesizer when core->io_wrap is NULL (legitimate on SHIM-attached
 * cores; that NULL state is allow-listed in scan.c).
 */
static u32 bcma_host_soc_read32_be(struct bcma_device *core, u16 offset)
{
	return ioread32be(core->io_addr + offset);
}

static u16 bcma_host_soc_read16_be(struct bcma_device *core, u16 offset)
{
	return ioread16be(core->io_addr + offset);
}

static void bcma_host_soc_write32_be(struct bcma_device *core, u16 offset,
				     u32 value)
{
	iowrite32be(value, core->io_addr + offset);
}

static void bcma_host_soc_write16_be(struct bcma_device *core, u16 offset,
				     u16 value)
{
	iowrite16be(value, core->io_addr + offset);
}

static u32 bcma_host_soc_aread32_be(struct bcma_device *core, u16 offset)
{
	if (likely(core->io_wrap))
		return ioread32be(core->io_wrap + offset);
	return bcma_host_soc_synth_aread32(core, offset);
}

static void bcma_host_soc_awrite32_be(struct bcma_device *core, u16 offset,
				      u32 value)
{
	if (likely(core->io_wrap)) {
		iowrite32be(value, core->io_wrap + offset);
		return;
	}
	bcma_host_soc_synth_awrite32(core, offset, value);
}

static const struct bcma_host_ops bcma_host_soc_ops_brcm_shim = {
	.read8		= bcma_host_soc_read8,
	.read16		= bcma_host_soc_read16_be,
	.read32		= bcma_host_soc_read32_be,
	.write8		= bcma_host_soc_write8,
	.write16	= bcma_host_soc_write16_be,
	.write32	= bcma_host_soc_write32_be,
	.aread32	= bcma_host_soc_aread32_be,
	.awrite32	= bcma_host_soc_awrite32_be,
};

int __init bcma_host_soc_register(struct bcma_soc *soc)
{
	struct bcma_bus *bus = &soc->bus;

	/* iomap only first core. We have to read some register on this core
	 * to scan the bus.
	 */
	bus->mmio = ioremap(BCMA_ADDR_BASE, BCMA_CORE_SIZE * 1);
	if (!bus->mmio)
		return -ENOMEM;

	/* Host specific */
	bus->hosttype = BCMA_HOSTTYPE_SOC;
	bus->ops = &bcma_host_soc_ops;

	/* Initialize struct, detect chip */
	bcma_init_bus(bus);

	return 0;
}

int __init bcma_host_soc_init(struct bcma_soc *soc)
{
	struct bcma_bus *bus = &soc->bus;
	int err;

	/* Scan bus and initialize it */
	err = bcma_bus_early_register(bus);
	if (err)
		iounmap(bus->mmio);

	return err;
}

#ifdef CONFIG_OF
static int bcma_host_soc_probe(struct platform_device *pdev)
{
	struct bcma_host_soc_pdata *pdata = dev_get_platdata(&pdev->dev);
	struct device *dev = &pdev->dev;
	struct bcma_bus *bus;
	int err;

	/* Alloc */
	bus = devm_kzalloc(dev, sizeof(*bus), GFP_KERNEL);
	if (!bus)
		return -ENOMEM;

	bus->dev = dev;

	/* Map MMIO. devm_platform_ioremap_resource() consumes the first
	 * IORESOURCE_MEM regardless of whether it came from a DT reg
	 * property (legacy brcm,bus-axi path) or from a synthesized
	 * platform_device_info::res (SHIM-attached path).
	 */
	bus->mmio = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(bus->mmio))
		return PTR_ERR(bus->mmio);

	/* Host specific */
	bus->hosttype = BCMA_HOSTTYPE_SOC;
	if (pdata) {
		bus->big_endian    = pdata->big_endian;
		bus->shim_attached = pdata->shim_attached;
		bus->shim_iomem    = pdata->shim_iomem;
		bus->ops = pdata->big_endian ? &bcma_host_soc_ops_brcm_shim
					     : &bcma_host_soc_ops;
	} else {
		bus->ops = &bcma_host_soc_ops;
	}

	/* Initialize struct, detect chip */
	bcma_init_bus(bus);

	/* Register */
	err = bcma_bus_register(bus);
	if (err)
		return err;

	platform_set_drvdata(pdev, bus);

	return err;
}

static void bcma_host_soc_remove(struct platform_device *pdev)
{
	struct bcma_bus *bus = platform_get_drvdata(pdev);

	bcma_bus_unregister(bus);
	/* bus->mmio is devm-managed; shim_iomem is borrowed from the
	 * parent bridge driver and must not be unmapped here.
	 */
	platform_set_drvdata(pdev, NULL);
}

static const struct of_device_id bcma_host_soc_of_match[] = {
	{ .compatible = "brcm,bus-axi", },
	{},
};
MODULE_DEVICE_TABLE(of, bcma_host_soc_of_match);

static struct platform_driver bcma_host_soc_driver = {
	.driver = {
		.name = "bcma-host-soc",
		.of_match_table = bcma_host_soc_of_match,
	},
	.probe		= bcma_host_soc_probe,
	.remove		= bcma_host_soc_remove,
};

int __init bcma_host_soc_register_driver(void)
{
	return platform_driver_register(&bcma_host_soc_driver);
}

void __exit bcma_host_soc_unregister_driver(void)
{
	platform_driver_unregister(&bcma_host_soc_driver);
}
#endif /* CONFIG_OF */
