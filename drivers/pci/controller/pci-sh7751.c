// SPDX-License-Identifier: GPL-2.0
/*
 * SH7751 PCI driver
 * Copyright (C) 2023 Yoshinori Sato
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/of_platform.h>
#include <linux/pci-ecam.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <asm-generic/pci.h>
#include <asm/addrspace.h>
#include "pci-sh7751.h"

#define pcic_writel(val, base, reg) __raw_writel(val, base + (reg))
#define pcic_readl(base, reg) __raw_readl(base + (reg))

/*
 * PCIC fixups
 */

static inline void pci_fixup_write_regs(struct device_node *np,
					const char *prop,
					void __iomem *pcic, int reg,
					int nr_regs)
{
	int i;
	u32 val;

	for (i = 0; i < nr_regs; i++) {
		if (of_property_read_u32_index(np, prop, i, &val))
			pcic_writel(val, pcic, reg + i * 4);
	}
}

#define SH7751_NUM_CONFIG 18
static void pcic_fixups(struct device_node *np,
		       void __iomem *pcic, void __iomem *bcr)
{
	unsigned long bcr1, mcr;
	u32 val;
	int i, r;
	u32 pci_config[SH7751_NUM_CONFIG * 2];

	const struct {
		const char *name;
		int reg;
		int nr;
	} reg_prop[] = {
		/*
		 *  The bus timing uses the bootloader settings,
		 *  so do not change them here.
		 */
		{ "renesas,intm",  SH4_PCIINTM,  1, },
		{ "renesas,aintm", SH4_PCIAINTM, 1, },
		{ "renesas,lsr",   SH4_PCILSR0,  2, },
		{ "renesas,lar",   SH4_PCILAR0,  2, },
		{ "renesas,dmabt", SH4_PCIDMABT, 1, },
		{ "renesas,pintm", SH4_PCIPINTM, 1, },
	};

	if (of_property_read_u32(np, "sh7751-pci,bcr1", &val)) {
		bcr1 = ioread32(bcr + SH7751_BCR1);
		bcr1 |= val;
		pcic_writel(bcr1, pcic, SH4_PCIBCR1);
	}
	if (of_property_read_u32(np, "renesas,clkr", &val)) {
		val |= 0xa5 << 24;
		pcic_writel(val, pcic, SH4_PCIBCR1);
	}
	for (i = 0; i < ARRAY_SIZE(reg_prop); i++)
		pci_fixup_write_regs(np, reg_prop[i].name, pcic,
				     reg_prop[i].reg, reg_prop[i].nr);

	memset(pci_config, 0, sizeof(pci_config));
	if (of_property_read_u32_array(np, "renesas,config",
				       pci_config, SH7751_NUM_CONFIG) == 0) {
		for (i = 0; i < SH7751_NUM_CONFIG; i++) {
			r = pci_config[i * 2];
			/* CONFIG0 is read-only, so make it a sentinel. */
			if (r == 0)
				break;
			pcic_writel(pci_config[i * 2 + 1], pcic,
				    SH7751_PCICONF0 + r * 4);
		}
	}

	if (of_property_read_u32(np, "sh7751-pci,mcrmask", &val)) {
		mcr = ioread32(bcr + SH7751_MCR);
		mcr &= ~val;
		pcic_writel(mcr, pcic, SH4_PCIMCR);
	}
}

/*
 * Direct access to PCI hardware...
 */
#define CONFIG_CMD(bus, devfn, where) \
	(0x80000000 | (bus->number << 16) | (devfn << 8) | (where & ~3))

/*
 * We need to avoid collisions with `mirrored' VGA ports
 * and other strange ISA hardware, so we always want the
 * addresses to be allocated in the 0x000-0x0ff region
 * modulo 0x400.
 */
#define IO_REGION_BASE 0x1000
resource_size_t pcibios_align_resource(void *data, const struct resource *res,
				resource_size_t size, resource_size_t align)
{
	resource_size_t start = res->start;

	if (res->flags & IORESOURCE_IO) {
		if (start < PCIBIOS_MIN_IO + IO_REGION_BASE)
			start = PCIBIOS_MIN_IO + IO_REGION_BASE;

		/*
		 * Put everything into 0x00-0xff region modulo 0x400.
		 */
		if (start & 0x300)
			start = (start + 0x3ff) & ~0x3ff;
	}

	return start;
}

static int area_sdram_check(struct device *dev, void __iomem *pcic,
			    void __iomem *bcr, unsigned int area)
{
	unsigned long word;

	word = __raw_readl(bcr + SH7751_BCR1);
	/* check BCR for SDRAM in area */
	if (((word >> area) & 1) == 0) {
		dev_info(dev, "PCI: Area %d is not configured for SDRAM. BCR1=0x%lx\n",
		       area, word);
		return 0;
	}
	pcic_writel(word, pcic, SH4_PCIBCR1);

	word = __raw_readw(bcr + SH7751_BCR2);
	/* check BCR2 for 32bit SDRAM interface*/
	if (((word >> (area << 1)) & 0x3) != 0x3) {
		dev_info(dev, "PCI: Area %d is not 32 bit SDRAM. BCR2=0x%lx\n",
			area, word);
		return 0;
	}
	pcic_writel(word, pcic, SH4_PCIBCR2);

	return 1;
}

static void set_pci_window(void __iomem *pcic, int no, struct resource *res)
{
	u32 word;

	word = res->end - res->start - 1;
	pcic_writel(word, pcic, SH4_PCILSR0 + no * 4);
	word = P2SEGADDR(res->start);
	pcic_writel(word, pcic, SH4_PCILAR0 + no * 4);
	pcic_writel(word, pcic, SH7751_PCICONF5 + no * 4);
}

static int sh7751_pci_probe(struct platform_device *pdev)
{
	struct resource *res, *w0res;
	u32 id;
	u32 reg, word;
	void __iomem *pcic;
	void __iomem *bcr;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pcic = (void __iomem *)res->start;
	if (IS_ERR(pcic))
		return PTR_ERR(pcic);

	w0res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (IS_ERR(w0res))
		return PTR_ERR(w0res);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	bcr = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(bcr))
		return PTR_ERR(bcr);

	/* check for SH7751/SH7751R hardware */
	id = pcic_readl(pcic, SH7751_PCICONF0);
	if (id != ((SH7751_DEVICE_ID << 16) | SH7751_VENDOR_ID) &&
	    id != ((SH7751R_DEVICE_ID << 16) | SH7751_VENDOR_ID)) {
		dev_warn(&pdev->dev, "PCI: This is not an SH7751(R)\n");
		return -ENODEV;
	}
	dev_info(&pdev->dev, "PCI core found at %pR\n", pcic);

	/* Set the BCR's to enable PCI access */
	reg = __raw_readl(bcr);
	reg |= 0x80000;
	__raw_writel(reg, bcr);

	/* Turn the clocks back on (not done in reset)*/
	pcic_writel(0, pcic, SH4_PCICLKR);
	/* Clear Powerdown IRQ's (not done in reset) */
	word = SH4_PCIPINT_D3 | SH4_PCIPINT_D0;
	pcic_writel(word, pcic, SH4_PCIPINT);

	/* set the command/status bits to:
	 * Wait Cycle Control + Parity Enable + Bus Master +
	 * Mem space enable
	 */
	word = SH7751_PCICONF1_WCC | SH7751_PCICONF1_PER |
	       SH7751_PCICONF1_BUM | SH7751_PCICONF1_MES;
	pcic_writel(word, pcic, SH7751_PCICONF1);

	/* define this host as the host bridge */
	word = PCI_BASE_CLASS_BRIDGE << 24;
	pcic_writel(word, pcic, SH7751_PCICONF2);

	/* Set IO and Mem windows to local address
	 * Make PCI and local address the same for easy 1 to 1 mapping
	 */
	set_pci_window(pcic, 0, w0res);	/* memory */

	/* check BCR for SDRAM in specified area */
	area_sdram_check(&pdev->dev, pcic, bcr, (w0res->start >> 27) & 0x07);

	/* configure the wait control registers */
	word = __raw_readl(bcr + SH7751_WCR1);
	pcic_writel(word, pcic, SH4_PCIWCR1);
	word = __raw_readl(bcr + SH7751_WCR2);
	pcic_writel(word, pcic, SH4_PCIWCR2);
	word = __raw_readl(bcr + SH7751_WCR3);
	pcic_writel(word, pcic, SH4_PCIWCR3);
	word = __raw_readl(bcr + SH7751_MCR);
	pcic_writel(word, pcic, SH4_PCIMCR);

	/* Override register setting */
	pcic_fixups(pdev->dev.of_node, pcic, bcr);

	/* SH7751 init done, set central function init complete */
	/* use round robin mode to stop a device starving/overrunning */
	word = SH4_PCICR_PREFIX | SH4_PCICR_CFIN | SH4_PCICR_ARBM;
	pcic_writel(word, pcic, SH4_PCICR);

	return pci_host_common_probe(pdev);
}

static void __iomem *sh4_pci_map_bus(struct pci_bus *bus,
				     unsigned int devfn, int where)
{
	struct pci_config_window *cfg = bus->sysdata;
	void __iomem *pcic = (void __iomem *)cfg->res.start;

	pcic_writel(CONFIG_CMD(bus, devfn, where), pcic, SH4_PCIPAR);
	return pcic + SH4_PCIPDR;
}

static const struct pci_ecam_ops pci_sh7751_bus_ops = {
	.pci_ops	= {
		.map_bus = sh4_pci_map_bus,
		.read    = pci_generic_config_read32,
		.write   = pci_generic_config_write32,
	}
};

static const struct of_device_id sh7751_pci_of_match[] = {
	{ .compatible = "renesas,pci-sh7751",
	  .data = &pci_sh7751_bus_ops },
	{ }
};
MODULE_DEVICE_TABLE(of, sh7751_pci_of_match);

static struct platform_driver sh7751_pci_driver = {
	.driver = {
		.name = "pci-sh7751",
		.of_match_table = sh7751_pci_of_match,
	},
	.probe = sh7751_pci_probe,
};
module_platform_driver(sh7751_pci_driver);

MODULE_DESCRIPTION("SH7751 PCI driver");
