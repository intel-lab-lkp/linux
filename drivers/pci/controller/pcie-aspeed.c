// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2025 Aspeed Technology Inc.
 */
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_pci.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#define MAX_MSI_HOST_IRQS	64
#define PCIE_RESET_CONFIG_DEVICE_WAIT_MS	500
#define PCIE_RESET_CONFIG_RC_WAIT_MS		10

/* AST2600 AHBC Registers */
#define AHBC_KEY		0x00
#define  AHBC_UNLOCK_KEY		0xAEED1A03
#define  AHBC_UNLOCK			0x01
#define AHBC_ADDR_MAPPING	0x8C
#define  PCIE_RC_MEMORY_EN		BIT(5)

/* AST2600 PCIe Host Controller Registers */
#define PEHR_GLOBAL		0x30
#define  AST2600_PORT_TYPE_MASK		GENMASK(5, 4)
#define  AST2600_PORT_TYPE(x)		FIELD_PREP(AST2600_PORT_TYPE_MASK, x)
#define PEHR_LOCK		0x7C
#define  PCIE_UNLOCK			0xa8
#define PEHR_LINK		0xC0
#define  PCIE_LINK_UP			BIT(5)

/* AST2600 H2X Controller Registers */
/* Common Registers*/
#define H2X_INT_STS		0x08
#define  PCIE_TX_IDLE_CLEAR		BIT(0)
#define  PCIE_INTX_STS			GENMASK(3, 0)
#define H2X_TX_DESC0		0x10
#define H2X_TX_DESC1		0x14
#define H2X_TX_DESC2		0x18
#define H2X_TX_DESC3		0x1C
#define H2X_TX_DESC_DATA	0x20
#define H2X_STS			0x24
#define  PCIE_TX_IDLE			BIT(31)
#define  PCIE_STATUS_OF_TX		GENMASK(25, 24)
#define	 PCIE_RC_L_TX_COMPLETE		BIT(24)
#define	 PCIE_RC_H_TX_COMPLETE		BIT(25)
#define  PCIE_TRIGGER_TX		BIT(0)
#define H2X_AHB_ADDR_CONFIG0	0x60
#define  AHB_REMAP_LO_ADDR(x)		FIELD_PREP(GENMASK(15, 4), x)
#define  AHB_MASK_LO_ADDR(x)		FIELD_PREP(GENMASK(31, 20), x)
#define H2X_AHB_ADDR_CONFIG1	0x64
#define  AHB_REMAP_HI_ADDR(x)		(x)
#define H2X_AHB_ADDR_CONFIG2	0x68
#define  AHB_MASK_HI_ADDR(x)		(x)

/* Device Registers */
#define H2X_DEV_CTRL		0x00
#define  PCIE_RX_DMA_EN			BIT(9)
#define  PCIE_RX_LINEAR			BIT(8)
#define  PCIE_RX_MSI_SEL		BIT(7)
#define  PCIE_RX_MSI_EN			BIT(6)
#define  PCIE_UNLOCK_RX_BUFF		BIT(4)
#define  PCIE_Wait_RX_TLP_CLR		BIT(2)
#define  PCIE_RC_RX_ENABLE		BIT(1)
#define  PCIE_RC_ENABLE			BIT(0)
#define H2X_DEV_STS		0x08
#define  PCIE_RC_RX_DONE_ISR		BIT(4)
#define H2X_DEV_RX_DESC_DATA	0x0C
#define H2X_DEV_RX_DESC1	0x14
#define H2X_DEV_TX_TAG		0x3C

/* AST2700 H2X */
#define H2X_CTRL		0x00
#define  H2X_BRIDGE_EN			BIT(0)
#define  H2X_BRIDGE_DIRECT_EN		BIT(1)
#define H2X_CFGE_INT_STS	0x08
#define  CFGE_TX_IDLE			BIT(0)
#define  CFGE_RX_BUSY			BIT(1)
#define H2X_CFGI_TLP		0x20
#define  CFGI_BYTE_EN_MASK		GENMASK(19, 16)
#define  CFGI_BYTE_EN(x)		FIELD_PREP(CFGI_BYTE_EN_MASK, (x))
#define H2X_CFGI_WR_DATA	0x24
#define  CFGI_WRITE			BIT(20)
#define H2X_CFGI_CTRL		0x28
#define  CFGI_TLP_FIRE			BIT(0)
#define H2X_CFGI_RET_DATA	0x2C
#define H2X_CFGE_TLP_1ST	0x30
#define H2X_CFGE_TLP_NEXT	0x34
#define H2X_CFGE_CTRL		0x38
#define  CFGE_TLP_FIRE			BIT(0)
#define H2X_CFGE_RET_DATA	0x3C
#define H2X_REMAP_PREF_ADDR	0x70
#define  REMAP_PREF_ADDR_63_32(x)	(x)
#define H2X_REMAP_DIRECT_ADDR	0x78
#define  REMAP_BAR_BASE(x)		(x)

/* AST2700 PEHR */
#define PEHR_MISC_58		0x58
#define  LOCAL_SCALE_SUP		BIT(0)
#define PEHR_MISC_5C		0x5C
#define  CONFIG_RC_DEVICE		BIT(30)
#define PEHR_MISC_60		0x60
#define  AST2700_PORT_TYPE_MASK		GENMASK(7, 4)
#define  PORT_TYPE_ROOT			BIT(2)
#define PEHR_MISC_70		0x70
#define  POSTED_DATA_CREDITS(x)		FIELD_PREP(GENMASK(15, 0), x)
#define  POSTED_HEADER_CREDITS(x)	FIELD_PREP(GENMASK(27, 16), x)
#define PEHR_MISC_78		0x78
#define  COMPLETION_DATA_CREDITS(x)	FIELD_PREP(GENMASK(15, 0), x)
#define  COMPLETION_HEADER_CREDITS(x)	FIELD_PREP(GENMASK(27, 16), x)
#define PEHR_MISC_300		0x300
#define  RC_GEN2		BIT(0)
#define PEHR_MISC_344		0x344
#define  LINK_UP_GEN2			BIT(18)
#define PEHR_MISC_358		0x358
#define  LINK_UP_GEN4			BIT(8)

/* AST2700 SCU */
#define SCU_60			0x60
#define  RC_E2M_PATH_EN			BIT(0)
#define  RC_H2XS_PATH_EN		BIT(16)
#define  RC_H2XD_PATH_EN		BIT(17)
#define  RC_H2XX_PATH_EN		BIT(18)
#define  RC_UPSTREAM_MEM_EN		BIT(19)
#define SCU_64			0x64
#define  RC0_DECODE_DMA_BASE(x)		FIELD_PREP(GENMASK(7, 0), x)
#define  RC0_DECODE_DMA_LIMIT(x)	FIELD_PREP(GENMASK(15, 8), x)
#define  RC1_DECODE_DMA_BASE(x)		FIELD_PREP(GENMASK(23, 16), x)
#define  RC1_DECODE_DMA_LIMIT(x)	FIELD_PREP(GENMASK(31, 24), x)
#define SCU_70			0x70
#define  DISABLE_EP_FUNC		0

/* TLP configuration type 0 and type 1 */
#define CRG0_READ_FMTTYPE                                                      \
	FIELD_PREP(GENMASK(31, 24), PCIE_TLP_FMT_TYPE(PCI_TLP_FMT_3DW_NO_DATA, \
						      PCI_TLP_TYPE_CFG0_RD))
#define CRG0_WRITE_FMTTYPE                                                  \
	FIELD_PREP(GENMASK(31, 24), PCIE_TLP_FMT_TYPE(PCI_TLP_FMT_3DW_DATA, \
						      PCI_TLP_TYPE_CFG0_WR))
#define CRG1_READ_FMTTYPE                                                      \
	FIELD_PREP(GENMASK(31, 24), PCIE_TLP_FMT_TYPE(PCI_TLP_FMT_3DW_NO_DATA, \
						      PCI_TLP_TYPE_CFG1_RD))
#define CRG1_WRITE_FMTTYPE                                                  \
	FIELD_PREP(GENMASK(31, 24), PCIE_TLP_FMT_TYPE(PCI_TLP_FMT_3DW_DATA, \
						      PCI_TLP_TYPE_CFG1_WR))
#define CRG_PAYLOAD_SIZE		0x01 /* 1 DWORD */
#define TLP_HEADER_BYTE_EN(x, y)	((GENMASK((x) - 1, 0) << ((y) % 4)))
#define TLP_GET_VALUE(x, y, z)		(((x) >> ((((z) % 4)) * 8)) & GENMASK((8 * (y)) - 1, 0))
#define TLP_SET_VALUE(x, y, z)		((((x) & GENMASK((8 * (y)) - 1, 0)) << ((((z) % 4)) * 8)))
#define AST2600_TX_DESC1_VALUE		0x00002000
#define AST2700_TX_DESC1_VALUE		0x00401000

/**
 * struct aspeed_pcie_port - PCIe port information
 * @list: port list
 * @pcie: pointer to PCIe host info
 * @clk: pointer to the port clock gate
 * @phy: pointer to PHY control block
 * @perst: pointer to port reset control
 * @slot: port slot
 */
struct aspeed_pcie_port {
	struct list_head list;
	struct aspeed_pcie *pcie;
	struct clk *clk;
	struct regmap *pciephy;
	struct reset_control *perst;
	u32 slot;
};

/**
 * struct aspeed_pcie - PCIe port information
 * @host: pointer to pcie host bridge
 * @dev: pointer to device structure
 * @reg: PCIe Host register base address
 * @ahbc: pointer to AHHC register map
 * @cfg: pointer to Aspeed PCIe configuration register map
 * @platform: platform specific information
 * @ports: list of PCIe ports
 * @domain: PCI domain number
 * @tx_tag: current TX tag for the port
 * @h2xrst: pointer to H2X reset control
 * @irq_domain: IRQ domain for INTx interrupts
 * @dev_domain: IRQ domain for device interrupts
 * @msi_domain: IRQ domain for MSI interrupts
 * @lock: mutex to protect MSI bitmap variable
 * @msi_irq_in_use: bitmap to track used MSI host IRQs
 */
struct aspeed_pcie {
	struct pci_host_bridge *host;
	struct device *dev;
	void __iomem *reg;
	struct regmap *ahbc;
	struct regmap *cfg;
	const struct aspeed_pcie_rc_platform *platform;
	struct list_head ports;

	int domain;
	u8 tx_tag;

	struct reset_control *h2xrst;

	struct irq_domain *irq_domain;
	struct irq_domain *dev_domain;
	struct irq_domain *msi_domain;
	struct mutex lock;		/* Protect MSI bitmap variable */
	DECLARE_BITMAP(msi_irq_in_use, MAX_MSI_HOST_IRQS);
};

/**
 * struct aspeed_pcie_rc_platform - Platform information
 * @setup: initialization function
 * @get_link: port link status function
 * @port_init: port initialization function
 * @reg_intx_en: INTx enable register offset
 * @reg_intx_sts: INTx status register offset
 * @reg_msi_en: MSI enable register offset
 * @reg_msi_sts: MSI enable register offset
 * @msi_address: HW fixed MSI address
 */
struct aspeed_pcie_rc_platform {
	int (*setup)(struct platform_device *pdev);
	bool (*get_link)(struct aspeed_pcie_port *port);
	void (*port_init)(struct aspeed_pcie_port *port);
	int reg_intx_en;
	int reg_intx_sts;
	int reg_msi_en;
	int reg_msi_sts;
	int msi_address;
};

static void aspeed_pcie_intx_irq_ack(struct irq_data *d)
{
	struct aspeed_pcie *pcie = irq_data_get_irq_chip_data(d);
	int intx_en = pcie->platform->reg_intx_en;
	u32 en;

	en = readl(pcie->reg + intx_en);
	en |= BIT(d->hwirq);
	writel(en, pcie->reg + intx_en);
}

static void aspeed_pcie_intx_irq_mask(struct irq_data *d)
{
	struct aspeed_pcie *pcie = irq_data_get_irq_chip_data(d);
	int intx_en = pcie->platform->reg_intx_en;
	u32 en;

	en = readl(pcie->reg + intx_en);
	en |= BIT(d->hwirq);
	writel(en, pcie->reg + intx_en);
}

static void aspeed_pcie_intx_irq_unmask(struct irq_data *d)
{
	struct aspeed_pcie *pcie = irq_data_get_irq_chip_data(d);
	int intx_en = pcie->platform->reg_intx_en;
	u32 en;

	en = readl(pcie->reg + intx_en);
	en |= BIT(d->hwirq);
	writel(en, pcie->reg + intx_en);
}

static struct irq_chip aspeed_intx_irq_chip = {
	.name = "IntX",
	.irq_ack = aspeed_pcie_intx_irq_ack,
	.irq_mask = aspeed_pcie_intx_irq_mask,
	.irq_unmask = aspeed_pcie_intx_irq_unmask,
};

static int aspeed_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
				irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &aspeed_intx_irq_chip, handle_level_irq);
	irq_set_chip_data(irq, domain->host_data);
	irq_set_status_flags(irq, IRQ_LEVEL);

	return 0;
}

static const struct irq_domain_ops aspeed_intx_domain_ops = {
	.map = aspeed_pcie_intx_map,
};

static irqreturn_t aspeed_pcie_intr_handler(int irq, void *dev_id)
{
	struct aspeed_pcie *pcie = dev_id;
	const struct aspeed_pcie_rc_platform *platform = pcie->platform;
	unsigned long status;
	unsigned long intx;
	u32 bit;
	int i;

	intx = readl(pcie->reg + platform->reg_intx_sts) & PCIE_INTX_STS;
	for_each_set_bit(bit, &intx, PCI_NUM_INTX)
		generic_handle_domain_irq(pcie->irq_domain, bit);

	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		for (i = 0; i < 2; i++) {
			status = readl(pcie->reg + platform->reg_msi_sts + (i * 4));
			writel(status, pcie->reg + platform->reg_msi_sts + (i * 4));

			for_each_set_bit(bit, &status, 32) {
				bit += (i * 32);
				generic_handle_domain_irq(pcie->dev_domain, bit);
			}
		}
	}

	return IRQ_HANDLED;
}

static u32 aspeed_pcie_get_bdf_offset(struct pci_bus *bus, unsigned int devfn,
				      int where)
{
	return ((bus->number) << 24) | (PCI_SLOT(devfn) << 19) |
		(PCI_FUNC(devfn) << 16) | (where & ~3);
}

static bool aspeed_ast2600_get_link(struct aspeed_pcie_port *port)
{
	u32 link_sts;

	regmap_read(port->pciephy, PEHR_LINK, &link_sts);

	return !!(link_sts & PCIE_LINK_UP);
}

static int aspeed_ast2600_conf(struct pci_bus *bus, unsigned int devfn,
			       int where, int size, u32 *val, u32 fmt_type,
			       bool write)
{
	struct aspeed_pcie *pcie = bus->sysdata;
	u32 bdf_offset, cfg_val, isr;
	int ret;

	bdf_offset = aspeed_pcie_get_bdf_offset(bus, devfn, where);

	/* Driver may set unlock RX buffer before triggering next TX config */
	writel(PCIE_UNLOCK_RX_BUFF | readl(pcie->reg + H2X_DEV_CTRL),
	       pcie->reg + H2X_DEV_CTRL);

	cfg_val = fmt_type | CRG_PAYLOAD_SIZE;
	regmap_write(pcie->cfg, H2X_TX_DESC0, cfg_val);

	cfg_val = AST2600_TX_DESC1_VALUE | FIELD_PREP(GENMASK(11, 8), pcie->tx_tag) |
		  TLP_HEADER_BYTE_EN(size, where);
	regmap_write(pcie->cfg, H2X_TX_DESC1, cfg_val);

	regmap_write(pcie->cfg, H2X_TX_DESC2, bdf_offset);
	regmap_write(pcie->cfg, H2X_TX_DESC3, 0);
	if (write)
		regmap_write(pcie->cfg, H2X_TX_DESC_DATA, TLP_SET_VALUE(*val, size, where));

	regmap_write_bits(pcie->cfg, H2X_STS, PCIE_TRIGGER_TX, PCIE_TRIGGER_TX);

	ret = regmap_read_poll_timeout(pcie->cfg, H2X_STS, cfg_val,
				       (cfg_val & PCIE_TX_IDLE), 0, 50);
	if (ret) {
		dev_err(pcie->dev,
			"%04x:%02x:%02x.%d CR tx timeout sts: 0x%08x\n",
			pcie->domain, bus->number, PCI_SLOT(devfn),
			PCI_FUNC(devfn), cfg_val);
		ret = PCIBIOS_SET_FAILED;
		PCI_SET_ERROR_RESPONSE(val);
		goto out;
	}

	regmap_write_bits(pcie->cfg, H2X_INT_STS, PCIE_TX_IDLE_CLEAR,
			  PCIE_TX_IDLE_CLEAR);

	regmap_read(pcie->cfg, H2X_STS, &cfg_val);
	switch (cfg_val & PCIE_STATUS_OF_TX) {
	case PCIE_RC_L_TX_COMPLETE:
	case PCIE_RC_H_TX_COMPLETE:
		ret = readl_poll_timeout(pcie->reg + H2X_DEV_STS, isr,
					 (isr & PCIE_RC_RX_DONE_ISR), 0, 50);
		if (ret) {
			dev_err(pcie->dev,
				"%04x:%02x:%02x.%d CR rx timeout sts: 0x%08x\n",
				pcie->domain, bus->number, PCI_SLOT(devfn),
				PCI_FUNC(devfn), isr);
			ret = PCIBIOS_SET_FAILED;
			PCI_SET_ERROR_RESPONSE(val);
			goto out;
		}
		if (!write) {
			if (readl(pcie->reg + H2X_DEV_RX_DESC1) & BIT(13)) {
				ret = PCIBIOS_SET_FAILED;
				PCI_SET_ERROR_RESPONSE(val);
				goto out;
			} else {
				*val = readl(pcie->reg + H2X_DEV_RX_DESC_DATA);
			}
		}
		break;
	case PCIE_STATUS_OF_TX:
		ret = PCIBIOS_SET_FAILED;
		PCI_SET_ERROR_RESPONSE(val);
		goto out;
	default:
		regmap_read(pcie->cfg, H2X_DEV_RX_DESC_DATA, &cfg_val);
		*val = cfg_val;
		break;
	}

	writel(PCIE_UNLOCK_RX_BUFF | readl(pcie->reg + H2X_DEV_CTRL),
	       pcie->reg + H2X_DEV_CTRL);

	*val = TLP_GET_VALUE(*val, size, where);

	ret = PCIBIOS_SUCCESSFUL;
out:
	writel(readl(pcie->reg + H2X_DEV_STS), pcie->reg + H2X_DEV_STS);
	pcie->tx_tag = (pcie->tx_tag + 1) % 0x8;
	return ret;
}

static int aspeed_ast2600_rd_conf(struct pci_bus *bus, unsigned int devfn,
				  int where, int size, u32 *val)
{
	int slot = PCI_SLOT(devfn);

	if (slot != 0 && slot != 8)
		return PCIBIOS_DEVICE_NOT_FOUND;

	return aspeed_ast2600_conf(bus, devfn, where, size, val, CRG0_READ_FMTTYPE, false);
}

static int aspeed_ast2600_child_rd_conf(struct pci_bus *bus, unsigned int devfn,
					int where, int size, u32 *val)
{
	return aspeed_ast2600_conf(bus, devfn, where, size, val, CRG1_READ_FMTTYPE, false);
}

static int aspeed_ast2600_wr_conf(struct pci_bus *bus, unsigned int devfn,
				  int where, int size, u32 val)
{
	int slot = PCI_SLOT(devfn);

	if (slot != 0 && slot != 8)
		return PCIBIOS_DEVICE_NOT_FOUND;

	return aspeed_ast2600_conf(bus, devfn, where, size, &val, CRG0_WRITE_FMTTYPE, true);
}

static int aspeed_ast2600_child_wr_conf(struct pci_bus *bus, unsigned int devfn,
					int where, int size, u32 val)
{
	return aspeed_ast2600_conf(bus, devfn, where, size, &val, CRG1_WRITE_FMTTYPE, true);
}

static bool aspeed_ast2700_get_link(struct aspeed_pcie_port *port)
{
	u32 reg;

	/* AST2700 has Gen2 and Gen4 RCs.
	 * Read register to distinguish between Gen2 or Gen4.
	 * Then read the corresonding register that is from Aspeed
	 * design to get whether it linked up or not.
	 */
	regmap_read(port->pciephy, PEHR_MISC_300, &reg);
	if (reg & RC_GEN2) {
		regmap_read(port->pciephy, PEHR_MISC_344, &reg);
		return !!(reg & LINK_UP_GEN2);
	}

	regmap_read(port->pciephy, PEHR_MISC_358, &reg);
	return !!(reg & LINK_UP_GEN4);
}

static int aspeed_ast2700_config(struct pci_bus *bus, unsigned int devfn,
				 int where, int size, u32 *val, bool write)
{
	struct aspeed_pcie *pcie = bus->sysdata;
	u32 cfg_val;

	cfg_val = CFGI_BYTE_EN(TLP_HEADER_BYTE_EN(size, where)) | (where & ~3);
	if (write)
		cfg_val |= CFGI_WRITE;
	writel(cfg_val, pcie->reg + H2X_CFGI_TLP);

	writel(TLP_SET_VALUE(*val, size, where), pcie->reg + H2X_CFGI_WR_DATA);
	writel(CFGI_TLP_FIRE, pcie->reg + H2X_CFGI_CTRL);
	*val = readl(pcie->reg + H2X_CFGI_RET_DATA);
	*val = TLP_GET_VALUE(*val, size, where);

	return PCIBIOS_SUCCESSFUL;
}

static int aspeed_ast2700_child_config(struct pci_bus *bus, unsigned int devfn,
				       int where, int size, u32 *val,
				       bool write)
{
	struct aspeed_pcie *pcie = bus->sysdata;
	u32 bdf_offset, status, cfg_val;
	int ret;

	bdf_offset = aspeed_pcie_get_bdf_offset(bus, devfn, where);

	cfg_val = CRG_PAYLOAD_SIZE;
	if (write)
		cfg_val |= (bus->number == 1) ? CRG0_WRITE_FMTTYPE : CRG1_WRITE_FMTTYPE;
	else
		cfg_val |= (bus->number == 1) ? CRG0_READ_FMTTYPE : CRG1_READ_FMTTYPE;
	writel(cfg_val, pcie->reg + H2X_CFGE_TLP_1ST);

	cfg_val = AST2700_TX_DESC1_VALUE |
		  FIELD_PREP(GENMASK(11, 8), pcie->tx_tag) |
		  TLP_HEADER_BYTE_EN(size, where);
	writel(cfg_val, pcie->reg + H2X_CFGE_TLP_NEXT);

	writel(bdf_offset, pcie->reg + H2X_CFGE_TLP_NEXT);
	if (write)
		writel(TLP_SET_VALUE(*val, size, where), pcie->reg + H2X_CFGE_TLP_NEXT);
	writel(CFGE_TX_IDLE | CFGE_RX_BUSY, pcie->reg + H2X_CFGE_INT_STS);
	writel(CFGE_TLP_FIRE, pcie->reg + H2X_CFGE_CTRL);

	ret = readl_poll_timeout(pcie->reg + H2X_CFGE_INT_STS, status,
				 (status & CFGE_TX_IDLE), 0, 50);
	if (ret) {
		dev_err(pcie->dev,
			"%04x:%02x:%02x.%d CR tx timeout sts: 0x%08x\n",
			pcie->domain, bus->number, PCI_SLOT(devfn),
			PCI_FUNC(devfn), status);
		ret = PCIBIOS_SET_FAILED;
		PCI_SET_ERROR_RESPONSE(val);
		goto out;
	}

	ret = readl_poll_timeout(pcie->reg + H2X_CFGE_INT_STS, status,
				 (status & CFGE_RX_BUSY), 0, 50);
	if (ret) {
		dev_err(pcie->dev,
			"%04x:%02x:%02x.%d CR rx timeoutsts: 0x%08x\n",
			pcie->domain, bus->number, PCI_SLOT(devfn),
			PCI_FUNC(devfn), status);
		ret = PCIBIOS_SET_FAILED;
		PCI_SET_ERROR_RESPONSE(val);
		goto out;
	}
	*val = readl(pcie->reg + H2X_CFGE_RET_DATA);
	*val = TLP_GET_VALUE(*val, size, where);

	ret = PCIBIOS_SUCCESSFUL;
out:
	writel(status, pcie->reg + H2X_CFGE_INT_STS);
	pcie->tx_tag = (pcie->tx_tag + 1) % 0xF;
	return ret;
}

static int aspeed_ast2700_rd_conf(struct pci_bus *bus, unsigned int devfn,
				  int where, int size, u32 *val)
{
	if (devfn != 0)
		return PCIBIOS_DEVICE_NOT_FOUND;

	return aspeed_ast2700_config(bus, devfn, where, size, val, false);
}

static int aspeed_ast2700_child_rd_conf(struct pci_bus *bus, unsigned int devfn,
					int where, int size, u32 *val)
{
	return aspeed_ast2700_child_config(bus, devfn, where, size, val, false);
}

static int aspeed_ast2700_wr_conf(struct pci_bus *bus, unsigned int devfn,
				  int where, int size, u32 val)
{
	if (devfn != 0)
		return PCIBIOS_DEVICE_NOT_FOUND;

	return aspeed_ast2700_config(bus, devfn, where, size, &val, true);
}

static int aspeed_ast2700_child_wr_conf(struct pci_bus *bus, unsigned int devfn,
					int where, int size, u32 val)
{
	return aspeed_ast2700_child_config(bus, devfn, where, size, &val, true);
}

static struct pci_ops aspeed_ast2600_pcie_ops = {
	.read = aspeed_ast2600_rd_conf,
	.write = aspeed_ast2600_wr_conf,
};

static struct pci_ops aspeed_ast2600_pcie_child_ops = {
	.read = aspeed_ast2600_child_rd_conf,
	.write = aspeed_ast2600_child_wr_conf,
};

static struct pci_ops aspeed_ast2700_pcie_ops = {
	.read = aspeed_ast2700_rd_conf,
	.write = aspeed_ast2700_wr_conf,
};

static struct pci_ops aspeed_ast2700_pcie_child_ops = {
	.read = aspeed_ast2700_child_rd_conf,
	.write = aspeed_ast2700_child_wr_conf,
};

#ifdef CONFIG_PCI_MSI
static void aspeed_msi_compose_msi_msg(struct irq_data *data,
				       struct msi_msg *msg)
{
	struct aspeed_pcie *pcie = irq_data_get_irq_chip_data(data);

	msg->address_hi = 0;
	msg->address_lo = pcie->platform->msi_address;
	msg->data = data->hwirq;
}

static int aspeed_msi_set_affinity(struct irq_data *irq_data,
				   const struct cpumask *mask, bool force)
{
	return -EINVAL;
}

static struct irq_chip aspeed_msi_bottom_irq_chip = {
	.name = "ASPEED MSI",
	.irq_compose_msi_msg = aspeed_msi_compose_msi_msg,
	.irq_set_affinity = aspeed_msi_set_affinity,
};

static int aspeed_irq_msi_domain_alloc(struct irq_domain *domain,
				       unsigned int virq, unsigned int nr_irqs,
				       void *args)
{
	struct aspeed_pcie *pcie = domain->host_data;
	int bit;
	int i;

	mutex_lock(&pcie->lock);

	bit = bitmap_find_free_region(pcie->msi_irq_in_use, MAX_MSI_HOST_IRQS,
				      get_count_order(nr_irqs));

	mutex_unlock(&pcie->lock);

	if (bit < 0)
		return -ENOSPC;

	for (i = 0; i < nr_irqs; i++) {
		irq_domain_set_info(domain, virq + i, bit + i,
				    &aspeed_msi_bottom_irq_chip,
				    domain->host_data, handle_simple_irq, NULL,
				    NULL);
	}

	return 0;
}

static void aspeed_irq_msi_domain_free(struct irq_domain *domain,
				       unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *data = irq_domain_get_irq_data(domain, virq);
	struct aspeed_pcie *pcie = irq_data_get_irq_chip_data(data);

	mutex_lock(&pcie->lock);

	bitmap_release_region(pcie->msi_irq_in_use, data->hwirq,
			      get_count_order(nr_irqs));

	mutex_unlock(&pcie->lock);
}

static const struct irq_domain_ops aspeed_msi_domain_ops = {
	.alloc = aspeed_irq_msi_domain_alloc,
	.free = aspeed_irq_msi_domain_free,
};

static struct irq_chip aspeed_msi_irq_chip = {
	.name = "PCIe MSI",
	.irq_enable = pci_msi_unmask_irq,
	.irq_disable = pci_msi_mask_irq,
	.irq_mask = pci_msi_mask_irq,
	.irq_unmask = pci_msi_unmask_irq,
};

static struct msi_domain_info aspeed_msi_domain_info = {
	.flags = (MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_USE_DEF_CHIP_OPS |
		  MSI_FLAG_MULTI_PCI_MSI | MSI_FLAG_PCI_MSIX),
	.chip = &aspeed_msi_irq_chip,
};

static int aspeed_pcie_msi_init(struct aspeed_pcie *pcie)
{
	int ret = 0;

	writel(~0, pcie->reg + pcie->platform->reg_msi_en);
	writel(~0, pcie->reg + pcie->platform->reg_msi_en + 0x04);
	writel(~0, pcie->reg + pcie->platform->reg_msi_sts);
	writel(~0, pcie->reg + pcie->platform->reg_msi_sts + 0x04);

	pcie->dev_domain =
		irq_domain_add_linear(NULL, MAX_MSI_HOST_IRQS, &aspeed_msi_domain_ops, pcie);
	if (!pcie->dev_domain)
		return dev_err_probe(pcie->dev, -ENOMEM, "failed to create IRQ domain\n");

	pcie->msi_domain = pci_msi_create_irq_domain(dev_fwnode(pcie->dev), &aspeed_msi_domain_info,
						     pcie->dev_domain);
	if (!pcie->msi_domain)
		return dev_err_probe(pcie->dev, -ENOMEM, "failed to create MSI domain\n");

	return ret;
}

static void aspeed_pcie_msi_free(struct aspeed_pcie *pcie)
{
	if (pcie->msi_domain) {
		irq_domain_remove(pcie->msi_domain);
		pcie->msi_domain = NULL;
	}

	if (pcie->dev_domain) {
		irq_domain_remove(pcie->dev_domain);
		pcie->dev_domain = NULL;
	}
}
#else
static int aspeed_pcie_msi_init(struct aspeed_pcie *pcie)
{
	return 0;
}

static void aspeed_pcie_msi_free(struct aspeed_pcie *pcie) {}
#endif

static void aspeed_pcie_irq_domain_free(struct aspeed_pcie *pcie)
{
	if (pcie->irq_domain) {
		irq_domain_remove(pcie->irq_domain);
		pcie->irq_domain = NULL;
	}
	aspeed_pcie_msi_free(pcie);
}

static int aspeed_pcie_init_irq_domain(struct aspeed_pcie *pcie)
{
	struct device *dev = pcie->dev;
	struct device_node *node = dev->of_node;
	struct device_node *pcie_intc_node;
	int ret;

	pcie_intc_node = of_get_next_child(node, NULL);
	if (!pcie_intc_node)
		return dev_err_probe(dev, -ENODEV, "No PCIe Intc node found\n");

	pcie->irq_domain =
		irq_domain_add_linear(pcie_intc_node, PCI_NUM_INTX, &aspeed_intx_domain_ops, pcie);
	of_node_put(pcie_intc_node);
	if (!pcie->irq_domain) {
		ret = dev_err_probe(dev, -ENOMEM, "failed to get an INTx IRQ domain\n");
		goto err;
	}

	writel(0, pcie->reg + pcie->platform->reg_intx_en);
	writel(~0, pcie->reg + pcie->platform->reg_intx_sts);

	ret = aspeed_pcie_msi_init(pcie);
	if (ret)
		goto err;

	return 0;
err:
	aspeed_pcie_irq_domain_free(pcie);
	return ret;
}

static void aspeed_ast2600_port_init(struct aspeed_pcie_port *port)
{
	regmap_write(port->pciephy, PEHR_LOCK, PCIE_UNLOCK);
	regmap_write(port->pciephy, PEHR_GLOBAL, AST2600_PORT_TYPE(0x3));
}

static void aspeed_ast2700_port_init(struct aspeed_pcie_port *port)
{
	u32 cfg_val;

	regmap_write(port->pciephy, PEHR_MISC_70,
		     POSTED_DATA_CREDITS(0xc0) | POSTED_HEADER_CREDITS(0xa));
	regmap_write(port->pciephy, PEHR_MISC_78,
		     COMPLETION_DATA_CREDITS(0x30) | COMPLETION_HEADER_CREDITS(0x8));
	regmap_write(port->pciephy, PEHR_MISC_58, LOCAL_SCALE_SUP);

	regmap_write(port->pciephy, PEHR_MISC_5C, CONFIG_RC_DEVICE);
	regmap_read(port->pciephy, PEHR_MISC_60, &cfg_val);
	cfg_val &= ~AST2700_PORT_TYPE_MASK;
	cfg_val |= FIELD_PREP(AST2700_PORT_TYPE_MASK, PORT_TYPE_ROOT);
	regmap_write(port->pciephy, PEHR_MISC_60, cfg_val);
}

static int aspeed_pcie_port_init(struct aspeed_pcie_port *port)
{
	struct aspeed_pcie *pcie = port->pcie;
	struct device *dev = pcie->dev;
	int ret;

	ret = clk_prepare_enable(port->clk);
	if (ret)
		return dev_err_probe(dev, ret, "enabling clk pcie%d\n", port->slot);

	pcie->platform->port_init(port);

	reset_control_deassert(port->perst);
	mdelay(PCIE_RESET_CONFIG_DEVICE_WAIT_MS);

	if (pcie->platform->get_link(port))
		dev_dbg(dev, "PCIe%d port %d link up\n", pcie->domain, port->slot);
	else
		dev_dbg(dev, "PCIe%d port %d link down\n", pcie->domain, port->slot);

	return 0;
}

static int aspeed_pcie_init_ports(struct aspeed_pcie *pcie)
{
	struct device *dev = pcie->dev;
	struct aspeed_pcie_port *port, *tmp;
	u8 num_enabled = 0;

	list_for_each_entry_safe(port, tmp, &pcie->ports, list) {
		u32 slot = port->slot;
		int ret;

		ret = aspeed_pcie_port_init(port);
		if (ret) {
			dev_err(dev, "initializing port %d failed\n", slot);
			list_del(&port->list);
		} else {
			num_enabled++;
		}
	}

	return (num_enabled > 0) ? 0 : -ENODEV;
}

static void aspeed_host_reset(struct aspeed_pcie *pcie)
{
	reset_control_assert(pcie->h2xrst);
	mdelay(PCIE_RESET_CONFIG_RC_WAIT_MS);
	reset_control_deassert(pcie->h2xrst);
}

static int aspeed_ast2600_setup(struct platform_device *pdev)
{
	struct aspeed_pcie *pcie = platform_get_drvdata(pdev);
	struct device *dev = pcie->dev;

	pcie->ahbc = syscon_regmap_lookup_by_phandle(dev->of_node, "aspeed,ahbc");
	if (IS_ERR(pcie->ahbc))
		return dev_err_probe(dev, PTR_ERR(pcie->ahbc), "failed to map ahbc base\n");

	aspeed_host_reset(pcie);

	regmap_write(pcie->ahbc, AHBC_KEY, AHBC_UNLOCK_KEY);
	regmap_update_bits(pcie->ahbc, AHBC_ADDR_MAPPING, PCIE_RC_MEMORY_EN, PCIE_RC_MEMORY_EN);
	regmap_write(pcie->ahbc, AHBC_KEY, AHBC_UNLOCK);

	regmap_write(pcie->cfg, H2X_AHB_ADDR_CONFIG0,
		     AHB_REMAP_LO_ADDR(0x600) | AHB_MASK_LO_ADDR(0xE00));
	regmap_write(pcie->cfg, H2X_AHB_ADDR_CONFIG1, AHB_REMAP_HI_ADDR(0));
	regmap_write(pcie->cfg, H2X_AHB_ADDR_CONFIG2, AHB_MASK_HI_ADDR(~0));

	regmap_write(pcie->cfg, H2X_CTRL, H2X_BRIDGE_EN);

	writel(PCIE_RX_DMA_EN | PCIE_RX_LINEAR | PCIE_RX_MSI_SEL | PCIE_RX_MSI_EN |
	       PCIE_Wait_RX_TLP_CLR | PCIE_RC_RX_ENABLE | PCIE_RC_ENABLE,
	       pcie->reg + H2X_DEV_CTRL);

	writel(0x28, pcie->reg + H2X_DEV_TX_TAG);

	pcie->host->ops = &aspeed_ast2600_pcie_ops;
	pcie->host->child_ops = &aspeed_ast2600_pcie_child_ops;

	return 0;
}

static int aspeed_ast2700_bar_assign(struct aspeed_pcie *pcie)
{
	struct resource_entry *win, *tmp;
	struct pci_host_bridge *bridge = pcie->host;

	resource_list_for_each_entry_safe(win, tmp, &bridge->windows) {
		struct resource *res = win->res;

		if (resource_type(res) == IORESOURCE_MEM &&
		    !(res->flags & IORESOURCE_MEM_64)) {
			writel(REMAP_BAR_BASE(res->start), pcie->reg + H2X_REMAP_DIRECT_ADDR);
			return 0;
		}
	}

	return -ENODEV;
}

static int aspeed_ast2700_setup(struct platform_device *pdev)
{
	struct aspeed_pcie *pcie = platform_get_drvdata(pdev);
	int ret;

	regmap_update_bits(pcie->cfg, SCU_60,
			   RC_E2M_PATH_EN | RC_H2XS_PATH_EN | RC_H2XD_PATH_EN | RC_H2XX_PATH_EN |
			   RC_UPSTREAM_MEM_EN,
			   RC_E2M_PATH_EN | RC_H2XS_PATH_EN | RC_H2XD_PATH_EN | RC_H2XX_PATH_EN |
			   RC_UPSTREAM_MEM_EN);
	regmap_write(pcie->cfg, SCU_64,
		     RC0_DECODE_DMA_BASE(0) | RC0_DECODE_DMA_LIMIT(0xFF) | RC1_DECODE_DMA_BASE(0) |
		     RC1_DECODE_DMA_LIMIT(0xFF));
	regmap_write(pcie->cfg, SCU_70, DISABLE_EP_FUNC);

	aspeed_host_reset(pcie);

	writel(0, pcie->reg + H2X_CTRL);
	writel(H2X_BRIDGE_EN | H2X_BRIDGE_DIRECT_EN, pcie->reg + H2X_CTRL);

	ret = aspeed_ast2700_bar_assign(pcie);
	if (ret)
		return dev_err_probe(pcie->dev, ret, "Failed to assign bar\n");

	/* Prepare for 64-bit BAR pref */
	writel(REMAP_PREF_ADDR_63_32(0x3), pcie->reg + H2X_REMAP_PREF_ADDR);

	pcie->host->ops = &aspeed_ast2700_pcie_ops;
	pcie->host->child_ops = &aspeed_ast2700_pcie_child_ops;

	return 0;
}

static int aspeed_pcie_parse_port(struct aspeed_pcie *pcie,
				  struct device_node *node,
				  int slot)
{
	struct aspeed_pcie_port *port;
	struct device *dev = pcie->dev;

	port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;

	port->pciephy = syscon_regmap_lookup_by_phandle(node, "aspeed,pciephy");
	if (IS_ERR(port->pciephy))
		return dev_err_probe(dev, PTR_ERR(port->pciephy),
				     "Failed to map pcie%d pciephy base\n", slot);

	port->clk = devm_get_clk_from_child(dev, node, NULL);
	if (IS_ERR(port->clk))
		return dev_err_probe(dev, PTR_ERR(port->clk),
				     "Failed to get pcie%d clock\n", slot);

	port->perst = of_reset_control_get_exclusive(node, "perst");
	if (IS_ERR(port->perst))
		return dev_err_probe(dev, PTR_ERR(port->perst),
				     "Failed to get pcie%d reset control\n", slot);
	reset_control_assert(port->perst);

	port->slot = slot;
	port->pcie = pcie;

	INIT_LIST_HEAD(&port->list);
	list_add_tail(&port->list, &pcie->ports);

	return 0;
}

static int aspeed_pcie_parse_dt(struct aspeed_pcie *pcie)
{
	struct device *dev = pcie->dev;
	struct device_node *node = dev->of_node;
	int ret;

	for_each_available_child_of_node_scoped(node, child) {
		int slot;
		const char *type;

		ret = of_property_read_string(child, "device_type", &type);
		if (ret || strcmp(type, "pci"))
			continue;

		ret = of_pci_get_devfn(child);
		if (ret < 0)
			return dev_err_probe(dev, ret, "failed to parse devfn\n");

		slot = PCI_SLOT(ret);

		ret = aspeed_pcie_parse_port(pcie, child, slot);
		if (ret)
			return ret;
	}

	if (list_empty(&pcie->ports))
		return dev_err_probe(dev, -ENODEV, "No PCIe port found in DT\n");

	return 0;
}

static int aspeed_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pci_host_bridge *host;
	struct aspeed_pcie *pcie;
	struct aspeed_pcie_port *port;
	struct device_node *node = dev->of_node;
	const struct aspeed_pcie_rc_platform *md = of_device_get_match_data(dev);
	int irq, ret;

	if (!md)
		return -ENODEV;

	host = devm_pci_alloc_host_bridge(dev, sizeof(*pcie));
	if (!host)
		return -ENOMEM;

	pcie = pci_host_bridge_priv(host);
	pcie->dev = dev;
	pcie->tx_tag = 0;
	platform_set_drvdata(pdev, pcie);

	pcie->platform = md;
	pcie->host = host;
	INIT_LIST_HEAD(&pcie->ports);

	pcie->reg = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(pcie->reg))
		return PTR_ERR(pcie->reg);

	of_property_read_u32(node, "linux,pci-domain", &pcie->domain);

	pcie->cfg = syscon_regmap_lookup_by_phandle(dev->of_node, "aspeed,pciecfg");
	if (IS_ERR(pcie->cfg))
		return dev_err_probe(dev, PTR_ERR(pcie->cfg), "Failed to map pciecfg base\n");

	pcie->h2xrst = devm_reset_control_get_exclusive(dev, "h2x");
	if (IS_ERR(pcie->h2xrst))
		return dev_err_probe(dev, PTR_ERR(pcie->h2xrst), "Failed to get h2x reset\n");

	ret = devm_mutex_init(dev, &pcie->lock);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to init mutex\n");

	ret = pcie->platform->setup(pdev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to setup PCIe RC\n");

	ret = aspeed_pcie_parse_dt(pcie);
	if (ret)
		return ret;

	ret = aspeed_pcie_init_ports(pcie);
	if (ret)
		goto err_remove_resets;

	host->sysdata = pcie;

	ret = aspeed_pcie_init_irq_domain(pcie);
	if (ret)
		goto err_irq_init;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto err_irq;
	}

	ret = devm_request_irq(dev, irq, aspeed_pcie_intr_handler, IRQF_SHARED, dev_name(dev),
			       pcie);
	if (ret)
		goto err_irq;

	ret = pci_host_probe(host);
	if (ret)
		goto err_irq;

	return 0;
err_irq:
	aspeed_pcie_irq_domain_free(pcie);
err_irq_init:
err_remove_resets:
	list_for_each_entry(port, &pcie->ports, list)
		reset_control_put(port->perst);
	dev_err_probe(dev, ret, "Failed to initial RC\n");
	return ret;
}

const struct aspeed_pcie_rc_platform pcie_rc_ast2600 = {
	.setup = aspeed_ast2600_setup,
	.get_link = aspeed_ast2600_get_link,
	.port_init = aspeed_ast2600_port_init,
	.reg_intx_en = 0x04,
	.reg_intx_sts = 0x08,
	.reg_msi_en = 0x20,
	.reg_msi_sts = 0x28,
	.msi_address = 0x1e77005c,
};

const struct aspeed_pcie_rc_platform pcie_rc_ast2700 = {
	.setup = aspeed_ast2700_setup,
	.get_link = aspeed_ast2700_get_link,
	.port_init = aspeed_ast2700_port_init,
	.reg_intx_en = 0x40,
	.reg_intx_sts = 0x48,
	.reg_msi_en = 0x50,
	.reg_msi_sts = 0x58,
	.msi_address = 0x000000F0,
};

static const struct of_device_id aspeed_pcie_of_match[] = {
	{ .compatible = "aspeed,ast2600-pcie", .data = &pcie_rc_ast2600 },
	{ .compatible = "aspeed,ast2700-pcie", .data = &pcie_rc_ast2700 },
	{}
};

static struct platform_driver aspeed_pcie_driver = {
	.driver = {
		.name = "aspeed-pcie",
		.of_match_table = aspeed_pcie_of_match,
	},
	.probe = aspeed_pcie_probe,
};

module_platform_driver(aspeed_pcie_driver);

MODULE_AUTHOR("Jacky Chou <jacky_chou@aspeedtech.com>");
MODULE_DESCRIPTION("ASPEED PCIe Root Complex");
MODULE_LICENSE("GPL");
