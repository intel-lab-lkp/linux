// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2025 Aspeed Technology Inc.
 */
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/mfd/syscon.h>
#include <linux/kernel.h>
#include <linux/msi.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_pci.h>
#include <linux/pci.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/gpio/consumer.h>
#include <linux/bitfield.h>
#include <linux/clk.h>

#define MAX_MSI_HOST_IRQS	64

/* AST2600 AHBC Registers */
#define AHBC_KEY		0x00
#define AHBC_UNLOCK_KEY			0xAEED1A03
#define AHBC_UNLOCK			0x01
#define AHBC_ADDR_MAPPING	0x8C
#define PCIE_RC_MEMORY_EN		BIT(5)

/* AST2600 PCIe Host Controller Registers */
#define PEHR_MISC_10		0x10
#define DATALINK_REPORT_CAPABLE		BIT(4)
#define PEHR_GLOBAL		0x30
#define RC_SYNC_RESET_DISABLE		BIT(20)
#define PCIE_RC_SLOT_ENABLE		BIT(1)
#define ROOT_COMPLEX_ID(x)		((x) << 4)
#define PEHR_LOCK		0x7C
#define PCIE_UNLOCK			0xa8
#define PEHR_LINK		0xC0
#define PCIE_LINK_STS			BIT(5)

/* AST2600 H2X Controller Registers */
/* Common Registers*/
#define H2X_INT_STS		0x08
#define PCIE_TX_IDLE_CLEAR		BIT(0)
#define H2X_TX_DESC0		0x10
#define H2X_TX_DESC1		0x14
#define H2X_TX_DESC2		0x18
#define H2X_TX_DESC3		0x1C
#define H2X_TX_DESC_DATA	0x20
#define H2X_STS			0x24
#define PCIE_TX_IDLE			BIT(31)
#define PCIE_STATUS_OF_TX		GENMASK(25, 24)
#define	PCIE_RC_L_TX_COMPLETE		BIT(24)
#define	PCIE_RC_H_TX_COMPLETE		BIT(25)
#define PCIE_TRIGGER_TX			BIT(0)
#define H2X_AHB_ADDR_CONFIG0	0x60
#define AHB_REMAP_ADDR_31_20(x)	FIELD_PREP(GENMASK(15, 4), x)
#define AHB_MASK_ADDR_31_20(x)	FIELD_PREP(GENMASK(31, 20), x)
#define H2X_AHB_ADDR_CONFIG1	0x64
#define AHB_REMAP_ADDR_64_32(x)	(x)
#define H2X_AHB_ADDR_CONFIG2	0x68
#define AHB_MASK_ADDR_64_32(x)	(x)
/* Device Registers */
#define H2X_DEV_CTRL		0x00
#define PCIE_RX_DMA_EN			BIT(9)
#define PCIE_RX_LINEAR			BIT(8)
#define PCIE_RX_MSI_SEL			BIT(7)
#define PCIE_RX_MSI_EN			BIT(6)
#define PCIE_UNLOCK_RX_BUFF		BIT(4)
#define PCIE_Wait_RX_TLP_CLR		BIT(2)
#define PCIE_RC_RX_ENABLE		BIT(1)
#define PCIE_RC_ENABLE			BIT(0)
#define H2X_DEV_STS		0x08
#define PCIE_RC_RX_DONE_ISR		BIT(4)
#define H2X_DEV_RX_DESC_DATA	0x0C
#define H2X_DEV_RX_DESC1	0x14
#define H2X_DEV_TX_TAG		0x3C

/* AST2700 H2X */
#define H2X_CTRL		0x00
#define H2X_BRIDGE_EN			BIT(0)
#define H2X_BRIDGE_DIRECT_EN		BIT(1)
#define H2X_CFGE_INT_STS	0x08
#define CFGE_TX_IDLE			BIT(0)
#define CFGE_RX_BUSY			BIT(1)
#define H2X_CFGI_TLP		0x20
#define H2X_CFGI_WR_DATA	0x24
#define CFGI_WRITE			BIT(20)
#define H2X_CFGI_CTRL		0x28
#define CFGI_TLP_FIRE			BIT(0)
#define H2X_CFGI_RET_DATA	0x2C
#define H2X_CFGE_TLP_1ST	0x30
#define H2X_CFGE_TLP_NEXT	0x34
#define H2X_CFGE_CTRL		0x38
#define CFGE_TLP_FIRE			BIT(0)
#define H2X_CFGE_RET_DATA	0x3C
#define H2X_REMAP_PREF_ADDR	0x70
#define REMAP_PREF_ADDR_63_32(x)	(x)
#define H2X_REMAP_DIRECT_ADDR	0x78
#define REMAP_BAR_BASE(x)		(x)

/* AST2700 PEHR */
#define PEHR_VID_DID		0x00
#define PEHR_MISC_58		0x58
#define LOCAL_SCALE_SUP			BIT(0)
#define PEHR_MISC_5C		0x5C
#define CONFIG_RC_DEVICE		BIT(30)
#define PEHR_MISC_60		0x60
#define PORT_TPYE			GENMASK(7, 4)
#define PORT_TYPE_ROOT			BIT(2)
#define PEHR_MISC_70		0x70
#define POSTED_DATA_CREDITS(x)		FIELD_PREP(GENMASK(15, 0), x)
#define POSTED_HEADER_CREDITS(x)	FIELD_PREP(GENMASK(27, 16), x)
#define PEHR_MISC_78		0x78
#define COMPLETION_DATA_CREDITS(x)	FIELD_PREP(GENMASK(15, 0), x)
#define COMPLETION_HEADER_CREDITS(x)	FIELD_PREP(GENMASK(27, 16), x)
#define PEHR_MISC_300		0x300
#define CAPABILITY_GEN2		BIT(0)
#define PEHR_MISC_344		0x344
#define LINK_STATUS_GEN2		BIT(18)
#define PEHR_MISC_358		0x358
#define LINK_STATUS_GEN4		BIT(8)

/* AST2700 SCU */
#define SCU_60			0x60
#define RC_E2M_PATH_EN			BIT(0)
#define RC_H2XS_PATH_EN			BIT(16)
#define RC_H2XD_PATH_EN			BIT(17)
#define RC_H2XX_PATH_EN			BIT(18)
#define RC_UPSTREAM_MEM_EN		BIT(19)
#define SCU_64			0x64
#define RC0_DECODE_DMA_BASE(x)		FIELD_PREP(GENMASK(7, 0), x)
#define RC0_DECODE_DMA_LIMIT(x)		FIELD_PREP(GENMASK(15, 8), x)
#define RC1_DECODE_DMA_BASE(x)		FIELD_PREP(GENMASK(23, 16), x)
#define RC1_DECODE_DMA_LIMIT(x)		FIELD_PREP(GENMASK(31, 24), x)
#define SCU_70			0x70
#define DISABLE_EP_FUNC			0x0

/* TLP configuration type 0 and type 1 */
#define CRG_READ_FMTTYPE(type)		(0x04000000 | (type << 24))
#define CRG_WRITE_FMTTYPE(type)		(0x44000000 | (type << 24))
#define CRG_PAYLOAD_SIZE		0x01 /* 1 DWORD */
#define TLP_COMP_STATUS(s)		(((s) >> 13) & 7)
#define TLP_BYTE_EN(x)			(x)
#define AST2600_TX_DESC1_VALUE		0x00002000
#define AST2700_TX_DESC1_VALUE		0x00401000

struct aspeed_pcie_rc_platform {
	int (*setup)(struct platform_device *pdev);
	/* Interrupt Register Offset */
	int reg_intx_en;
	int reg_intx_sts;
	int reg_msi_en;
	int reg_msi_sts;
};

struct aspeed_pcie {
	struct pci_host_bridge *host;
	struct device *dev;
	void __iomem *reg;
	struct regmap *ahbc;
	struct regmap *cfg;
	struct regmap *pciephy;
	struct clk *clock;
	const struct aspeed_pcie_rc_platform *platform;

	int domain;
	u32 msi_address;
	u8 tx_tag;

	struct reset_control *h2xrst;
	struct reset_control *perst;

	struct irq_domain *irq_domain;
	struct irq_domain *dev_domain;
	struct irq_domain *msi_domain;
	/* Protect bitmap variable */
	struct mutex lock;

	DECLARE_BITMAP(msi_irq_in_use, MAX_MSI_HOST_IRQS);
};

static void aspeed_pcie_intx_ack_irq(struct irq_data *d)
{
	struct aspeed_pcie *pcie = irq_data_get_irq_chip_data(d);
	int intx_en = pcie->platform->reg_intx_en;

	writel(readl(pcie->reg + intx_en) | BIT(d->hwirq), pcie->reg + intx_en);
}

static void aspeed_pcie_intx_mask_irq(struct irq_data *d)
{
	struct aspeed_pcie *pcie = irq_data_get_irq_chip_data(d);
	int intx_en = pcie->platform->reg_intx_en;

	writel(readl(pcie->reg + intx_en) & ~BIT(d->hwirq), pcie->reg + intx_en);
}

static void aspeed_pcie_intx_unmask_irq(struct irq_data *d)
{
	struct aspeed_pcie *pcie = irq_data_get_irq_chip_data(d);
	int intx_en = pcie->platform->reg_intx_en;

	writel(readl(pcie->reg + intx_en) | BIT(d->hwirq), pcie->reg + intx_en);
}

static struct irq_chip aspeed_intx_irq_chip = {
	.name = "ASPEED:IntX",
	.irq_ack = aspeed_pcie_intx_ack_irq,
	.irq_mask = aspeed_pcie_intx_mask_irq,
	.irq_unmask = aspeed_pcie_intx_unmask_irq,
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

	intx = readl(pcie->reg + platform->reg_intx_sts) & 0xf;
	if (intx) {
		for_each_set_bit(bit, &intx, PCI_NUM_INTX)
			generic_handle_domain_irq(pcie->irq_domain, bit);
	}

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

static int aspeed_ast2600_rd_conf(struct pci_bus *bus, unsigned int devfn,
				  int where, int size, u32 *val)
{
	struct aspeed_pcie *pcie = bus->sysdata;
	u32 bdf_offset;
	int rx_done_fail = 0, slot = PCI_SLOT(devfn);
	u32 cfg_val, isr, type = 0;
	u32 link_sts = 0;
	int ret;

	/* Driver may set unlock RX buffere before triggering next TX config */
	writel(PCIE_UNLOCK_RX_BUFF | readl(pcie->reg + H2X_DEV_CTRL),
	       pcie->reg + H2X_DEV_CTRL);

	if (bus->number == 128 && slot != 0 && slot != 8)
		return PCIBIOS_DEVICE_NOT_FOUND;
	type = (bus->number > 128);

	if (type) {
		regmap_read(pcie->pciephy, PEHR_LINK, &link_sts);
		if (!(link_sts & PCIE_LINK_STS))
			return PCIBIOS_DEVICE_NOT_FOUND;
	}

	bdf_offset = aspeed_pcie_get_bdf_offset(bus, devfn, where);

	regmap_write(pcie->cfg, H2X_TX_DESC0, CRG_READ_FMTTYPE(type) | CRG_PAYLOAD_SIZE);
	regmap_write(pcie->cfg, H2X_TX_DESC1,
		     AST2600_TX_DESC1_VALUE | (pcie->tx_tag << 8) | TLP_BYTE_EN(0xf));
	regmap_write(pcie->cfg, H2X_TX_DESC2, bdf_offset);
	regmap_write(pcie->cfg, H2X_TX_DESC3, 0);

	regmap_write_bits(pcie->cfg, H2X_STS, PCIE_TRIGGER_TX, PCIE_TRIGGER_TX);

	ret = regmap_read_poll_timeout(pcie->cfg, H2X_STS, cfg_val,
				       (cfg_val & PCIE_TX_IDLE), 0, 50);
	if (ret) {
		dev_err(pcie->dev,
			"[%X:%02X:%02X.%02X]CR tx timeout sts: 0x%08x\n",
			pcie->domain, bus->number, PCI_SLOT(devfn),
			PCI_FUNC(devfn), cfg_val);
		ret = PCIBIOS_SET_FAILED;
		*val = ~0;
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
				"[%X:%02X:%02X.%02X]CR rx timeoutsts: 0x%08x\n",
				pcie->domain, bus->number, PCI_SLOT(devfn),
				PCI_FUNC(devfn), isr);
			rx_done_fail = 1;
			*val = ~0;
		}
		if (!rx_done_fail) {
			if (readl(pcie->reg + H2X_DEV_RX_DESC1) & BIT(13))
				*val = ~0;
			else
				*val = readl(pcie->reg + H2X_DEV_RX_DESC_DATA);
		}

		writel(PCIE_UNLOCK_RX_BUFF | readl(pcie->reg + H2X_DEV_CTRL),
		       pcie->reg + H2X_DEV_CTRL);
		break;
	case PCIE_STATUS_OF_TX:
		*val = ~0;
		break;
	default:
		regmap_read(pcie->cfg, H2X_DEV_RX_DESC_DATA, &cfg_val);
		*val = cfg_val;
		break;
	}

	switch (size) {
	case 1:
		*val = (*val >> ((where & 3) * 8)) & 0xff;
		break;
	case 2:
		*val = (*val >> ((where & 2) * 8)) & 0xffff;
		break;
	case 4:
	default:
		break;
	}

	ret = PCIBIOS_SUCCESSFUL;
out:
	writel(readl(pcie->reg + H2X_DEV_STS), pcie->reg + H2X_DEV_STS);
	pcie->tx_tag = (pcie->tx_tag + 1) % 0x8;
	return ret;
}

static int aspeed_ast2600_wr_conf(struct pci_bus *bus, unsigned int devfn,
				  int where, int size, u32 val)
{
	u32 type = 0;
	u32 shift = 8 * (where & 3);
	u32 bdf_offset;
	u8 byte_en = 0;
	struct aspeed_pcie *pcie = bus->sysdata;
	u32 isr, cfg_val;
	int ret;

	/* Driver may set unlock RX buffere before triggering next TX config */
	writel(PCIE_UNLOCK_RX_BUFF | readl(pcie->reg + H2X_DEV_CTRL),
	       pcie->reg + H2X_DEV_CTRL);

	switch (size) {
	case 1:
		byte_en = 1 << (where % 4);
		val = (val & 0xff) << shift;
		break;
	case 2:
		byte_en = 0x3 << (2 * ((where >> 1) % 2));
		val = (val & 0xffff) << shift;
		break;
	case 4:
	default:
		byte_en = 0xf;
		break;
	}

	type = (bus->number > 128);

	bdf_offset = aspeed_pcie_get_bdf_offset(bus, devfn, where);

	regmap_write(pcie->cfg, H2X_TX_DESC0, CRG_WRITE_FMTTYPE(type) | CRG_PAYLOAD_SIZE);
	regmap_write(pcie->cfg, H2X_TX_DESC1,
		     AST2600_TX_DESC1_VALUE | (pcie->tx_tag << 8) | TLP_BYTE_EN(byte_en));
	regmap_write(pcie->cfg, H2X_TX_DESC2, bdf_offset);
	regmap_write(pcie->cfg, H2X_TX_DESC3, 0);
	regmap_write(pcie->cfg, H2X_TX_DESC_DATA, val);

	regmap_write_bits(pcie->cfg, H2X_STS, PCIE_TRIGGER_TX, PCIE_TRIGGER_TX);

	ret = regmap_read_poll_timeout(pcie->cfg, H2X_STS, cfg_val,
				       (cfg_val & PCIE_TX_IDLE), 0, 50);
	if (ret) {
		dev_err(pcie->dev,
			"[%X:%02X:%02X.%02X]CT tx timeout sts: 0x%08x\n",
			pcie->domain, bus->number, PCI_SLOT(devfn),
			PCI_FUNC(devfn), cfg_val);
		ret = PCIBIOS_SET_FAILED;
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
				"[%X:%02X:%02X.%02X]CT rx timeout sts: 0x%08x\n",
				pcie->domain, bus->number, PCI_SLOT(devfn),
				PCI_FUNC(devfn), isr);
			ret = PCIBIOS_SET_FAILED;
			goto out;
		}
		break;
	}

	ret = PCIBIOS_SUCCESSFUL;
out:
	writel(readl(pcie->reg + H2X_DEV_STS), pcie->reg + H2X_DEV_STS);
	pcie->tx_tag = (pcie->tx_tag + 1) % 0x8;
	return ret;
}

static bool aspeed_ast2700_get_link(struct aspeed_pcie *pcie)
{
	u32 reg;
	bool link;

	regmap_read(pcie->pciephy, PEHR_MISC_300, &reg);
	if (reg & CAPABILITY_GEN2) {
		regmap_read(pcie->pciephy, PEHR_MISC_344, &reg);
		link = !!(reg & LINK_STATUS_GEN2);
	} else {
		regmap_read(pcie->pciephy, PEHR_MISC_358, &reg);
		link = !!(reg & LINK_STATUS_GEN4);
	}

	return link;
}

static int aspeed_ast2700_rd_conf(struct pci_bus *bus, unsigned int devfn,
				  int where, int size, u32 *val)
{
	struct aspeed_pcie *pcie = bus->sysdata;
	u32 bdf_offset, status;
	u8 type;
	int ret;

	if ((bus->number == 0 && devfn != 0))
		return PCIBIOS_DEVICE_NOT_FOUND;

	if (bus->number == 0) {
		/* Internal access to bridge */
		writel(TLP_BYTE_EN(0xf) << 16 | (where & ~3), pcie->reg + H2X_CFGI_TLP);
		writel(CFGI_TLP_FIRE, pcie->reg + H2X_CFGI_CTRL);
		*val = readl(pcie->reg + H2X_CFGI_RET_DATA);
	} else {
		if (!aspeed_ast2700_get_link(pcie))
			return PCIBIOS_DEVICE_NOT_FOUND;

		bdf_offset = aspeed_pcie_get_bdf_offset(bus, devfn, where);

		type = (bus->number == 1) ? PCI_HEADER_TYPE_NORMAL : PCI_HEADER_TYPE_BRIDGE;

		writel(CRG_READ_FMTTYPE(type) | CRG_PAYLOAD_SIZE, pcie->reg + H2X_CFGE_TLP_1ST);
		writel(AST2700_TX_DESC1_VALUE | (pcie->tx_tag << 8) | TLP_BYTE_EN(0xf),
		       pcie->reg + H2X_CFGE_TLP_NEXT);
		writel(bdf_offset, pcie->reg + H2X_CFGE_TLP_NEXT);
		writel(CFGE_TX_IDLE | CFGE_RX_BUSY, pcie->reg + H2X_CFGE_INT_STS);
		writel(CFGE_TLP_FIRE, pcie->reg + H2X_CFGE_CTRL);

		ret = readl_poll_timeout(pcie->reg + H2X_CFGE_INT_STS, status,
					 (status & CFGE_TX_IDLE), 0, 50);
		if (ret) {
			dev_err(pcie->dev,
				"[%X:%02X:%02X.%02X]CR tx timeout sts: 0x%08x\n",
				pcie->domain, bus->number, PCI_SLOT(devfn),
				PCI_FUNC(devfn), status);
			ret = PCIBIOS_SET_FAILED;
			*val = ~0;
			goto out;
		}

		ret = readl_poll_timeout(pcie->reg + H2X_CFGE_INT_STS, status,
					 (status & CFGE_RX_BUSY), 0, 50000);
		if (ret) {
			dev_err(pcie->dev,
				"[%X:%02X:%02X.%02X]CR rx timeoutsts: 0x%08x\n",
				pcie->domain, bus->number, PCI_SLOT(devfn),
				PCI_FUNC(devfn), status);
			ret = PCIBIOS_SET_FAILED;
			*val = ~0;
			goto out;
		}
		*val = readl(pcie->reg + H2X_CFGE_RET_DATA);
	}

	switch (size) {
	case 1:
		*val = (*val >> ((where & 3) * 8)) & 0xff;
		break;
	case 2:
		*val = (*val >> ((where & 2) * 8)) & 0xffff;
		break;
	case 4:
	default:
		break;
	}

	ret = PCIBIOS_SUCCESSFUL;
out:
	writel(status, pcie->reg + H2X_CFGE_INT_STS);
	pcie->tx_tag = (pcie->tx_tag + 1) % 0xF;
	return ret;
}

static int aspeed_ast2700_wr_conf(struct pci_bus *bus, unsigned int devfn,
				  int where, int size, u32 val)
{
	struct aspeed_pcie *pcie = bus->sysdata;
	u32 shift = 8 * (where & 3);
	u8 byte_en;
	u32 bdf_offset, status, type;
	int ret;

	if ((bus->number == 0 && devfn != 0))
		return PCIBIOS_DEVICE_NOT_FOUND;

	switch (size) {
	case 1:
		byte_en = 1 << (where % 4);
		val = (val & 0xff) << shift;
		break;
	case 2:
		byte_en = 0x3 << (2 * ((where >> 1) % 2));
		val = (val & 0xffff) << shift;
		break;
	case 4:
	default:
		byte_en = 0xf;
		break;
	}

	if (bus->number == 0) {
		/* Internal access to bridge */
		writel(CFGI_WRITE | TLP_BYTE_EN(byte_en) << 16 | (where & ~3),
		       pcie->reg + H2X_CFGI_TLP);
		writel(val, pcie->reg + H2X_CFGI_WR_DATA);
		writel(CFGI_TLP_FIRE, pcie->reg + H2X_CFGI_CTRL);
	} else {
		if (!aspeed_ast2700_get_link(pcie))
			return PCIBIOS_SET_FAILED;

		bdf_offset = aspeed_pcie_get_bdf_offset(bus, devfn, where);

		type = (bus->number == 1) ? PCI_HEADER_TYPE_NORMAL : PCI_HEADER_TYPE_BRIDGE;

		writel(CRG_WRITE_FMTTYPE(type) | CRG_PAYLOAD_SIZE, pcie->reg + H2X_CFGE_TLP_1ST);
		writel(AST2700_TX_DESC1_VALUE | (pcie->tx_tag << 8) | byte_en,
		       pcie->reg + H2X_CFGE_TLP_NEXT);
		writel(bdf_offset, pcie->reg + H2X_CFGE_TLP_NEXT);
		writel(val, pcie->reg + H2X_CFGE_TLP_NEXT);
		writel(CFGE_TX_IDLE | CFGE_RX_BUSY, pcie->reg + H2X_CFGE_INT_STS);
		writel(CFGE_TLP_FIRE, pcie->reg + H2X_CFGE_CTRL);

		ret = readl_poll_timeout(pcie->reg + H2X_CFGE_INT_STS, status,
					 (status & CFGE_TX_IDLE), 0, 50);
		if (ret) {
			dev_err(pcie->dev,
				"[%X:%02X:%02X.%02X]CT tx timeout sts: 0x%08x\n",
				pcie->domain, bus->number, PCI_SLOT(devfn),
				PCI_FUNC(devfn), status);
			ret = PCIBIOS_SET_FAILED;
			goto out;
		}

		ret = readl_poll_timeout(pcie->reg + H2X_CFGE_INT_STS, status,
					 (status & CFGE_RX_BUSY), 0, 50000);
		if (ret) {
			dev_err(pcie->dev,
				"[%X:%02X:%02X.%02X]CT rx timeout sts: 0x%08x\n",
				pcie->domain, bus->number, PCI_SLOT(devfn),
				PCI_FUNC(devfn), status);
			ret = PCIBIOS_SET_FAILED;
			goto out;
		}

		(void)readl(pcie->reg + H2X_CFGE_RET_DATA);
	}

	ret = PCIBIOS_SUCCESSFUL;
out:
	writel(status, pcie->reg + H2X_CFGE_INT_STS);
	pcie->tx_tag = (pcie->tx_tag + 1) % 0xF;
	return ret;
}

static struct pci_ops aspeed_ast2600_pcie_ops = {
	.read = aspeed_ast2600_rd_conf,
	.write = aspeed_ast2600_wr_conf,
};

static struct pci_ops aspeed_ast2700_pcie_ops = {
	.read = aspeed_ast2700_rd_conf,
	.write = aspeed_ast2700_wr_conf,
};

#ifdef CONFIG_PCI_MSI
static void aspeed_msi_compose_msi_msg(struct irq_data *data,
				       struct msi_msg *msg)
{
	struct aspeed_pcie *pcie = irq_data_get_irq_chip_data(data);

	msg->address_hi = 0;
	msg->address_lo = pcie->msi_address;
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
#endif

static void aspeed_pcie_irq_domain_free(struct aspeed_pcie *pcie)
{
	if (pcie->irq_domain) {
		irq_domain_remove(pcie->irq_domain);
		pcie->irq_domain = NULL;
	}
#ifdef CONFIG_PCI_MSI
	if (pcie->msi_domain) {
		irq_domain_remove(pcie->msi_domain);
		pcie->msi_domain = NULL;
	}

	if (pcie->dev_domain) {
		irq_domain_remove(pcie->dev_domain);
		pcie->dev_domain = NULL;
	}
#endif
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

#ifdef CONFIG_PCI_MSI
	pcie->dev_domain =
		irq_domain_add_linear(NULL, MAX_MSI_HOST_IRQS, &aspeed_msi_domain_ops, pcie);
	if (!pcie->dev_domain) {
		ret = dev_err_probe(pcie->dev, -ENOMEM, "failed to create IRQ domain\n");
		goto err;
	}

	pcie->msi_domain = pci_msi_create_irq_domain(dev_fwnode(pcie->dev), &aspeed_msi_domain_info,
						     pcie->dev_domain);
	if (!pcie->msi_domain) {
		ret = dev_err_probe(pcie->dev, -ENOMEM, "failed to create MSI domain\n");
		goto err;
	}

	writel(~0, pcie->reg + pcie->platform->reg_msi_en);
	writel(~0, pcie->reg + pcie->platform->reg_msi_en + 0x04);
	writel(~0, pcie->reg + pcie->platform->reg_msi_sts);
	writel(~0, pcie->reg + pcie->platform->reg_msi_sts + 0x04);
#endif
	return 0;
err:
	aspeed_pcie_irq_domain_free(pcie);
	return ret;
}

static void aspeed_pcie_port_init(struct aspeed_pcie *pcie)
{
	u32 link_sts = 0;

	regmap_write(pcie->pciephy, PEHR_LOCK, PCIE_UNLOCK);
	regmap_write(pcie->pciephy, PEHR_GLOBAL, ROOT_COMPLEX_ID(0x3));

	reset_control_deassert(pcie->perst);
	mdelay(500);

	writel(PCIE_RX_DMA_EN | PCIE_RX_LINEAR | PCIE_RX_MSI_SEL | PCIE_RX_MSI_EN |
	       PCIE_Wait_RX_TLP_CLR | PCIE_RC_RX_ENABLE | PCIE_RC_ENABLE,
	       pcie->reg + H2X_DEV_CTRL);

	writel(0x28, pcie->reg + H2X_DEV_TX_TAG);

	regmap_read(pcie->pciephy, PEHR_LINK, &link_sts);
	if (link_sts & PCIE_LINK_STS)
		dev_info(pcie->dev, "PCIe Link UP");
	else
		dev_info(pcie->dev, "PCIe Link DOWN");
}

static int aspeed_ast2600_setup(struct platform_device *pdev)
{
	struct aspeed_pcie *pcie = platform_get_drvdata(pdev);
	struct device *dev = pcie->dev;

	pcie->ahbc = syscon_regmap_lookup_by_phandle(dev->of_node, "aspeed,ahbc");
	if (IS_ERR(pcie->ahbc))
		return dev_err_probe(dev, PTR_ERR(pcie->ahbc), "failed to map ahbc base\n");

	reset_control_assert(pcie->h2xrst);
	mdelay(5);
	reset_control_deassert(pcie->h2xrst);

	regmap_write(pcie->ahbc, AHBC_KEY, AHBC_UNLOCK_KEY);
	regmap_update_bits(pcie->ahbc, AHBC_ADDR_MAPPING, PCIE_RC_MEMORY_EN, PCIE_RC_MEMORY_EN);
	regmap_write(pcie->ahbc, AHBC_KEY, AHBC_UNLOCK);

	regmap_write(pcie->cfg, H2X_AHB_ADDR_CONFIG0,
		     AHB_REMAP_ADDR_31_20(0x600) | AHB_MASK_ADDR_31_20(0xE00));
	regmap_write(pcie->cfg, H2X_AHB_ADDR_CONFIG1, AHB_REMAP_ADDR_64_32(0));
	regmap_write(pcie->cfg, H2X_AHB_ADDR_CONFIG2, AHB_MASK_ADDR_64_32(~0));

	regmap_write(pcie->cfg, H2X_CTRL, H2X_BRIDGE_EN);

	aspeed_pcie_port_init(pcie);

	pcie->host->ops = &aspeed_ast2600_pcie_ops;

	return 0;
}

static int aspeed_ast2700_setup(struct platform_device *pdev)
{
	struct aspeed_pcie *pcie = platform_get_drvdata(pdev);
	u32 cfg_val;

	reset_control_assert(pcie->perst);

	regmap_write(pcie->pciephy, PEHR_MISC_70,
		     POSTED_DATA_CREDITS(0xc0) | POSTED_HEADER_CREDITS(0xa));
	regmap_write(pcie->pciephy, PEHR_MISC_78,
		     COMPLETION_DATA_CREDITS(0x30) | COMPLETION_HEADER_CREDITS(0x8));
	regmap_write(pcie->pciephy, PEHR_MISC_58, LOCAL_SCALE_SUP);

	regmap_update_bits(pcie->cfg, SCU_60,
			   RC_E2M_PATH_EN | RC_H2XS_PATH_EN | RC_H2XD_PATH_EN | RC_H2XX_PATH_EN |
			   RC_UPSTREAM_MEM_EN,
			   RC_E2M_PATH_EN | RC_H2XS_PATH_EN | RC_H2XD_PATH_EN | RC_H2XX_PATH_EN |
			   RC_UPSTREAM_MEM_EN);
	regmap_write(pcie->cfg, SCU_64,
		     RC0_DECODE_DMA_BASE(0) | RC0_DECODE_DMA_LIMIT(0xFF) | RC1_DECODE_DMA_BASE(0) |
		     RC1_DECODE_DMA_LIMIT(0xFF));
	regmap_write(pcie->cfg, SCU_70, DISABLE_EP_FUNC);

	reset_control_assert(pcie->h2xrst);
	mdelay(10);
	reset_control_deassert(pcie->h2xrst);

	regmap_write(pcie->pciephy, PEHR_MISC_5C, CONFIG_RC_DEVICE);
	regmap_read(pcie->pciephy, PEHR_MISC_60, &cfg_val);
	regmap_write(pcie->pciephy, PEHR_MISC_60,
		     (cfg_val & ~PORT_TPYE) | FIELD_PREP(PORT_TPYE, PORT_TYPE_ROOT));

	writel(0, pcie->reg + H2X_CTRL);
	writel(H2X_BRIDGE_EN | H2X_BRIDGE_DIRECT_EN, pcie->reg + H2X_CTRL);

	/* The BAR mapping:
	 * CPU Node0(domain 0): 0x60000000
	 * CPU Node1(domain 1): 0x80000000
	 * IO       (domain 2): 0xa0000000
	 */
	writel(REMAP_BAR_BASE(0x60000000 + (0x20000000 * pcie->domain)),
	       pcie->reg + H2X_REMAP_DIRECT_ADDR);

	/* Prepare for 64-bit BAR pref */
	writel(REMAP_PREF_ADDR_63_32(0x3), pcie->reg + H2X_REMAP_PREF_ADDR);

	reset_control_deassert(pcie->perst);
	mdelay(1000);

	pcie->host->ops = &aspeed_ast2700_pcie_ops;

	if (aspeed_ast2700_get_link(pcie))
		dev_info(pcie->dev, "PCIe Link UP");
	else
		dev_info(pcie->dev, "PCIe Link DOWN");

	return 0;
}

static int aspeed_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pci_host_bridge *host;
	struct aspeed_pcie *pcie;
	struct device_node *node = dev->of_node;
	const void *md = of_device_get_match_data(dev);
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

	pcie->reg = devm_platform_ioremap_resource(pdev, 0);

	of_property_read_u32(node, "msi_address", &pcie->msi_address);
	of_property_read_u32(node, "linux,pci-domain", &pcie->domain);

	pcie->cfg = syscon_regmap_lookup_by_phandle(dev->of_node, "aspeed,pciecfg");
	if (IS_ERR(pcie->cfg))
		return dev_err_probe(dev, PTR_ERR(pcie->cfg), "Failed to map pciecfg base\n");

	pcie->pciephy = syscon_regmap_lookup_by_phandle(node, "aspeed,pciephy");
	if (IS_ERR(pcie->pciephy))
		return dev_err_probe(dev, PTR_ERR(pcie->pciephy), "Failed to map pciephy base\n");

	pcie->h2xrst = devm_reset_control_get_exclusive(dev, "h2x");
	if (IS_ERR(pcie->h2xrst))
		return dev_err_probe(dev, PTR_ERR(pcie->h2xrst), "Failed to get h2x reset\n");

	pcie->perst = devm_reset_control_get_exclusive(dev, "perst");
	if (IS_ERR(pcie->perst))
		return dev_err_probe(dev, PTR_ERR(pcie->perst), "Failed to get perst reset\n");

	ret = pcie->platform->setup(pdev);
	if (ret)
		goto err_setup;

	host->sysdata = pcie;

	ret = aspeed_pcie_init_irq_domain(pcie);
	if (ret)
		goto err_irq_init;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		goto err_irq;

	ret = devm_request_irq(dev, irq, aspeed_pcie_intr_handler, IRQF_SHARED, dev_name(dev),
			       pcie);
	if (ret)
		goto err_irq;

	pcie->clock = clk_get(dev, NULL);
	if (IS_ERR(pcie->clock))
		goto err_clk;
	ret = clk_prepare_enable(pcie->clock);
	if (ret)
		goto err_clk_enable;

	ret = pci_host_probe(host);
	if (ret)
		goto err_clk_enable;

	return 0;

err_clk_enable:
	clk_put(pcie->clock);
err_clk:
err_irq:
	aspeed_pcie_irq_domain_free(pcie);
err_irq_init:
err_setup:
	return dev_err_probe(dev, ret, "Failed to setup PCIe RC\n");
}

static void aspeed_pcie_remove(struct platform_device *pdev)
{
	struct aspeed_pcie *pcie = platform_get_drvdata(pdev);

	if (pcie->clock) {
		clk_disable_unprepare(pcie->clock);
		clk_put(pcie->clock);
	}

	pci_stop_root_bus(pcie->host->bus);
	pci_remove_root_bus(pcie->host->bus);
	aspeed_pcie_irq_domain_free(pcie);
}

static struct aspeed_pcie_rc_platform pcie_rc_ast2600 = {
	.setup = aspeed_ast2600_setup,
	.reg_intx_en = 0x04,
	.reg_intx_sts = 0x08,
	.reg_msi_en = 0x20,
	.reg_msi_sts = 0x28,
};

static struct aspeed_pcie_rc_platform pcie_rc_ast2700 = {
	.setup = aspeed_ast2700_setup,
	.reg_intx_en = 0x40,
	.reg_intx_sts = 0x48,
	.reg_msi_en = 0x50,
	.reg_msi_sts = 0x58,
};

static const struct of_device_id aspeed_pcie_of_match[] = {
	{ .compatible = "aspeed,ast2600-pcie", .data = &pcie_rc_ast2600 },
	{ .compatible = "aspeed,ast2700-pcie", .data = &pcie_rc_ast2700 },
	{}
};

static struct platform_driver aspeed_pcie_driver = {
	.driver = {
		.name = "aspeed-pcie",
		.suppress_bind_attrs = true,
		.of_match_table = aspeed_pcie_of_match,
	},
	.probe = aspeed_pcie_probe,
	.remove = aspeed_pcie_remove,
};

module_platform_driver(aspeed_pcie_driver);

MODULE_AUTHOR("Jacky Chou <jacky_chou@aspeedtech.com>");
MODULE_DESCRIPTION("ASPEED PCIe Root Complex");
MODULE_LICENSE("GPL");
