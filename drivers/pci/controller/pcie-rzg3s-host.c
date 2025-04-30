// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe driver for Renesas RZ/G3S SoCs
 *
 * Copyright (C) 2025 Renesas Electronics Corp.
 *
 * Based on:
 *  drivers/pci/controller/pcie-rcar-host.c
 *  Copyright (C) 2009 - 2011  Paul Mundt
 */

#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/device/devres.h>
#include <linux/iopoll.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/msi.h>
#include <linux/of_irq.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/slab.h>

/* AXI registers. */
#define RZG3S_PCI_REQDATA(id)			(0x80 + (id) * 0x4)
#define RZG3S_PCI_REQRCVDAT			0x8c
#define RZG3S_PCI_REQADR1			0x90
#define RZG3S_PCI_REQADR1_BUS			GENMASK(31, 24)
#define RZG3S_PCI_REQADR1_DEV			GENMASK(23, 19)
#define RZG3S_PCI_REQADR1_FUNC			GENMASK(18, 16)
#define RZG3S_PCI_REQADR1_REG			GENMASK(11, 0)
#define RZG3S_PCI_REQBE				0x98
#define RZG3S_PCI_REQBE_BYTE_EN			GENMASK(3, 0)
#define RZG3S_PCI_REQISS			0x9c
#define RZG3S_PCI_REQISS_REQ_ISSUE		BIT(0)
#define RZG3S_PCI_REQISS_TR_TYPE		GENMASK(11, 8)
#define RZG3S_PCI_REQISS_TR_TP0_RD		FIELD_PREP(RZG3S_PCI_REQISS_TR_TYPE, 0x4)
#define RZG3S_PCI_REQISS_TR_TP0_WR		FIELD_PREP(RZG3S_PCI_REQISS_TR_TYPE, 0x5)
#define RZG3S_PCI_REQISS_TR_TP1_RD		FIELD_PREP(RZG3S_PCI_REQISS_TR_TYPE, 0x6)
#define RZG3S_PCI_REQISS_TR_TP1_WR		FIELD_PREP(RZG3S_PCI_REQISS_TR_TYPE, 0x7)
#define RZG3S_PCI_REQISS_MOR_STATUS		GENMASK(18, 16)
#define RZG3S_PCI_MSIRCVWADRL			0x100
#define RZG3S_PCI_MSIRCVWADRL_ENA		BIT(0)
#define RZG3S_PCI_MSIRCVWADRL_MSG_DATA_ENA	BIT(1)
#define RZG3S_PCI_MSIRCVWADRU			0x104
#define RZG3S_PCI_MSIRCVWMSKL			0x108
#define RZG3S_PCI_MSIRCVWMSKU			0x10c
#define RZG3S_PCI_PINTRCVIE			0x110
#define RZG3S_PCI_PINTRCVIE_INTX(i)		BIT(i)
#define RZG3S_PCI_PINTRCVIE_MSI			BIT(4)
#define RZG3S_PCI_PINTRCVIS			0x114
#define RZG3S_PCI_PINTRCVIS_INTX(i)		BIT(i)
#define RZG3S_PCI_PINTRCVIS_MSI			BIT(4)
#define RZG3S_PCI_MSGRCVIE			0x120
#define RZG3S_PCI_MSGRCVIE_MSG_RCV		BIT(24)
#define RZG3S_PCI_MSGRCVIS			0x124
#define RZG3S_PCI_MSGRCVIS_MRI			BIT(24)
#define RZG3S_PCI_PEIE0				0x200
#define RZG3S_PCI_PEIS0				0x204
#define RZG3S_PCI_PEIS0_DL_UPDOWN		BIT(9)
#define RZG3S_PCI_PEIS0_RX_DLLP_PM_ENTER	BIT(12)
#define RZG3S_PCI_PEIE1				0x208
#define RZG3S_PCI_PEIS1				0x20c
#define RZG3S_PCI_AMEIE				0x210
#define RZG3S_PCI_AMEIS				0x214
#define RZG3S_PCI_ASEIE1			0x220
#define RZG3S_PCI_ASEIS1			0x224
#define RZG3S_PCI_PCSTAT1			0x408
#define RZG3S_PCI_PCSTAT1_DL_DOWN_STS		BIT(0)
#define RZG3S_PCI_PCSTAT1_LTSSM_STATE		GENMASK(14, 10)
#define RZG3S_PCI_PCCTRL2			0x410
#define RZG3S_PCI_PCCTRL2_LS_CHG_REQ		BIT(0)
#define RZG3S_PCI_PCCTRL2_LS_CHG		GENMASK(9, 8)
#define RZG3S_PCI_PCSTAT2			0x414
#define RZG3S_PCI_PCSTAT2_STATE_RX_DETECT	GENMASK(15, 8)
#define RZG3S_PCI_PCSTAT2_SDRIRE		GENMASK(7, 0)
#define RZG3S_PCI_PCSTAT2_LS_CHG_DONE		BIT(28)
#define RZG3S_PCI_PERM				0x300
#define RZG3S_PCI_PERM_PIPE_PHY_REG_EN		BIT(1)
#define RZG3S_PCI_PERM_CFG_HWINIT_EN		BIT(2)
#define RZG3S_PCI_MSIRE(id)			(0x600 + (id) * 0x10)
#define RZG3S_PCI_MSIRE_ENA			BIT(0)
#define RZG3S_PCI_MSIRM(id)			(0x608 + (id) * 0x10)
#define RZG3S_PCI_MSIRS(id)			(0x60c + (id) * 0x10)
#define RZG3S_PCI_AWBASEL(id)			(0x1000 + (id) * 0x20)
#define RZG3S_PCI_AWBASEL_WIN_ENA		BIT(0)
#define RZG3S_PCI_AWBASEU(id)			(0x1004 + (id) * 0x20)
#define RZG3S_PCI_AWMASKL(id)			(0x1008 + (id) * 0x20)
#define RZG3S_PCI_AWMASKU(id)			(0x100c + (id) * 0x20)
#define RZG3S_PCI_ADESTL(id)			(0x1010 + (id) * 0x20)
#define RZG3S_PCI_ADESTU(id)			(0x1014 + (id) * 0x20)
#define RZG3S_PCI_PWBASEL(id)			(0x1100 + (id) * 0x20)
#define RZG3S_PCI_PWBASEL_ENA			BIT(0)
#define RZG3S_PCI_PWBASEU(id)			(0x1104 + (id) * 0x20)
#define RZG3S_PCI_PDESTL(id)			(0x1110 + (id) * 0x20)
#define RZG3S_PCI_PDESTU(id)			(0x1114 + (id) * 0x20)
#define RZG3S_PCI_PWMASKL(id)			(0x1108 + (id) * 0x20)
#define RZG3S_PCI_PWMASKU(id)			(0x110c + (id) * 0x20)

/* PHY control registers. */
#define RZG3S_PCI_PHY_XCFGD(id)			(0x2000 + (id) * 0x10)
#define RZG3S_PCI_PHY_XCFGD_NUM			39
#define RZG3S_PCI_PHY_XCFGA_CMN(id)		(0x2400 + (id) * 0x10)
#define RZG3S_PCI_PHY_XCFGA_CMN_NUM		16
#define RZG3S_PCI_PHY_XCFGA_RX(id)		(0x2500 + (id) * 0x10)
#define RZG3S_PCI_PHY_XCFGA_RX_NUM		13
#define RZG3S_PCI_PHY_XCFGA_TX			0x25d0
#define RZG3S_PCI_PHY_XCFG_CTRL			0x2a20
#define RZG3S_PCI_PHY_XCFG_CTRL_PHYREG_SEL	BIT(0)

/* PCIe registers. */
#define RZG3S_PCI_CFG_BASE			0x6000
#define RZG3S_PCI_CFG_VID			0x0
#define RZG3S_PCI_CFG_VID_DEVICE		GENMASK(31, 16)
#define RZG3S_PCI_CFG_VID_VENDOR		GENMASK(15, 0)
#define RZG3S_PCI_CFG_BNR			0x18
#define RZG3S_PCI_CFG_BNR_SUBORDINATE_BUS	GENMASK(23, 16)
#define RZG3S_PCI_CFG_BNR_SECONDARY_BUS		GENMASK(15, 8)
#define RZG3S_PCI_CFG_BARMSK00L			0xa0
#define RZG3S_PCI_CFG_BARMSK00U			0xa4
#define RZG3S_PCI_CFG_LINKCS			0x70
#define RZG3S_PCI_CFG_LINKCS_CUR_LS		GENMASK(19, 16)
#define RZG3S_PCI_CFG_LINCS2			0x90
#define RZG3S_PCI_CFG_LINCS2_TARGET_LS		GENMASK(3, 0)

/* System controller registers. */
#define RZG3S_SYS_PCIE_RST_RSM_B		0xd74
#define RZG3S_SYS_PCIE_RST_RSM_B_MASK		BIT(0)

/* Maximum number of windows. */
#define RZG3S_MAX_WINDOWS			8

/* Number of MSI interrupts per register. */
#define RZG3S_PCI_MSI_INT_PER_REG		32
/* The number of MSI interrupts. */
#define RZG3S_PCI_MSI_INT_NR			(RZG3S_PCI_MSI_INT_PER_REG * 2)

/**
 * enum rzg3s_pcie_link_speed - RZ/G3S PCIe available link speeds
 * @RZG3S_PCIE_LINK_SPEED_2_5_GTS: 2.5 GT/s
 * @RZG3S_PCIE_LINK_SPEED_5_0_GTS: 5.0 GT/s
 */
enum rzg3s_pcie_link_speed {
	RZG3S_PCIE_LINK_SPEED_2_5_GTS = 1,
	RZG3S_PCIE_LINK_SPEED_5_0_GTS
};

/**
 * struct rzg3s_pcie_msi - RZ/G3S PCIe MSI data structure
 * @domain: IRQ domain
 * @map: bitmap with the allocated MSIs
 * @dma_addr: address of the allocated MSI window
 * @window_base: base address of the MSI window
 * @pages: allocated pages for MSI window mapping
 * @map_lock: lock for bitmap with the allocated MSIs
 */
struct rzg3s_pcie_msi {
	struct irq_domain *domain;
	DECLARE_BITMAP(map, RZG3S_PCI_MSI_INT_NR);
	dma_addr_t dma_addr;
	dma_addr_t window_base;
	unsigned long pages;
	struct mutex map_lock;
};

/**
 * struct rzg3s_pcie_host - RZ/G3S PCIe data structure
 * @axi: base address for AXI registers
 * @pcie: base address for PCIe registers
 * @dev: struct device
 * @power_resets: reset control signals that should be set after power up
 * @cfg_resets: reset control signals that should be set after configuration
 * @phy_init: PHY specific initialization function
 * @sysc: SYSC regmap
 * @intx_domain: INTx IRQ domain
 * @devres_group_id: devres group which allows unconfiguring everything at
 * the end of the driver remove function (or, on probe failure path, just after
 * leaving the driver probe function); otherwise, the dev_pm_domain_detach()
 * called in the platform driver remove function disable the PCIe clocks leaving
 * the other action or reset function being executed (later) with clocks disabled;
 * this leads to system being blocked and eventually restarted by watchdog
 * @msi: MSI data structure
 * @hw_lock: lock for access to the HW resources
 * @intx_irqs: INTx interrupts
 * @vendor_id: Vendor ID
 * @device_id: Device ID
 * @num_power_resets: the number of power_resets
 * @num_cfg_resets: the number of cfg_resets
 */
struct rzg3s_pcie_host {
	void __iomem *axi;
	void __iomem *pcie;
	struct device *dev;
	struct reset_control **power_resets;
	struct reset_control **cfg_resets;
	int (*phy_init)(struct rzg3s_pcie_host *host);
	struct regmap *sysc;
	struct irq_domain *intx_domain;
	void *devres_group_id;
	struct rzg3s_pcie_msi msi;
	raw_spinlock_t hw_lock;
	int intx_irqs[PCI_NUM_INTX];
	u32 vendor_id;
	u32 device_id;
	u8 num_power_resets;
	u8 num_cfg_resets;
};

#define rzg3s_msi_to_host(_msi)	container_of(_msi, struct rzg3s_pcie_host, msi)

/**
 * enum rzg3s_pcie_cfg_access_type - RZ/G3S PCIe access type
 * @RZG3S_PCIE_CFG_ACCESS_RD: read access
 * @RZG3S_PCIE_CFG_ACCESS_WR: write access
 */
enum rzg3s_pcie_cfg_access_type {
	RZG3S_PCIE_CFG_ACCESS_RD,
	RZG3S_PCIE_CFG_ACCESS_WR,
};

static void rzg3s_pcie_update_bits(void __iomem *base, u32 offset, u32 mask, u32 val)
{
	u32 tmp;

	tmp = readl(base + offset);
	tmp &= ~mask;
	tmp |= val & mask;
	writel(tmp, base + offset);
}

static bool rzg3s_pcie_issue_request(struct rzg3s_pcie_host *host)
{
	u32 val;
	int ret;

	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_REQISS, RZG3S_PCI_REQISS_REQ_ISSUE,
			       RZG3S_PCI_REQISS_REQ_ISSUE);
	ret = readl_poll_timeout_atomic(host->axi + RZG3S_PCI_REQISS, val,
					!(val & RZG3S_PCI_REQISS_REQ_ISSUE),
					5, 2500);

	return !!ret || (val & RZG3S_PCI_REQISS_MOR_STATUS);
}

static int rzg3s_pcie_config_access(struct rzg3s_pcie_host *host, struct pci_bus *bus,
				    unsigned int devfn, int where, u32 *data,
				    enum rzg3s_pcie_cfg_access_type access_type)
{
	bool type0 = pci_is_root_bus(bus->parent) ? true : false;
	unsigned int dev, func, reg, tr_type;
	int ret;

	dev = PCI_SLOT(devfn);
	func = PCI_FUNC(devfn);
	reg = where & ~0x3;

	/* Set the destination. */
	writel(FIELD_PREP(RZG3S_PCI_REQADR1_BUS, bus->number) |
	       FIELD_PREP(RZG3S_PCI_REQADR1_DEV, dev) |
	       FIELD_PREP(RZG3S_PCI_REQADR1_FUNC, func) |
	       FIELD_PREP(RZG3S_PCI_REQADR1_REG, reg),
	       host->axi + RZG3S_PCI_REQADR1);

	/* Set byte enable. */
	writel(RZG3S_PCI_REQBE_BYTE_EN, host->axi + RZG3S_PCI_REQBE);

	if (access_type == RZG3S_PCIE_CFG_ACCESS_RD) {
		tr_type = type0 ? RZG3S_PCI_REQISS_TR_TP0_RD : RZG3S_PCI_REQISS_TR_TP1_RD;
	} else {
		tr_type = type0 ? RZG3S_PCI_REQISS_TR_TP0_WR : RZG3S_PCI_REQISS_TR_TP1_WR;

		/* Set the write data.  */
		writel(0, host->axi + RZG3S_PCI_REQDATA(0));
		writel(0, host->axi + RZG3S_PCI_REQDATA(1));
		writel(*data, host->axi + RZG3S_PCI_REQDATA(2));
	}

	/* Set the type of request. */
	writel(tr_type, host->axi + RZG3S_PCI_REQISS);

	/* Issue the request and wait to finish. */
	ret = rzg3s_pcie_issue_request(host);
	if (ret) {
		if (access_type == RZG3S_PCIE_CFG_ACCESS_RD)
			PCI_SET_ERROR_RESPONSE(data);

		return PCIBIOS_SET_FAILED;
	}

	/* Read the data. */
	if (access_type == RZG3S_PCIE_CFG_ACCESS_RD)
		*data = readl(host->axi + RZG3S_PCI_REQRCVDAT);

	return PCIBIOS_SUCCESSFUL;
}

static int rzg3s_pcie_read_config_access(struct rzg3s_pcie_host *host, struct pci_bus *bus,
					 unsigned int devfn, int where, u32 *data)
{
	/* Controller can't address itself. Just read the registers. */
	if (pci_is_root_bus(bus) && !devfn) {
		u32 reg = where & ~0x3;

		*data = readl(host->pcie + reg);
		return PCIBIOS_SUCCESSFUL;
	}

	return rzg3s_pcie_config_access(host, bus, devfn, where, data, RZG3S_PCIE_CFG_ACCESS_RD);
}

static int rzg3s_pcie_read_conf(struct pci_bus *bus, unsigned int devfn, int where, int size,
				u32 *val)
{
	struct rzg3s_pcie_host *host = bus->sysdata;
	int ret;

	/*
	 * Bus number for root complex is set to zero. Skip other requests for
	 * vendor and device ID (where = 0x00).
	 */
	if (pci_is_root_bus(bus) && PCI_SLOT(devfn) && !where)
		return PCIBIOS_DEVICE_NOT_FOUND;

	ret = rzg3s_pcie_read_config_access(host, bus, devfn, where, val);
	if (ret != PCIBIOS_SUCCESSFUL)
		return ret;

	if (size == 1)
		*val = (*val >> (BITS_PER_BYTE * (where & 3))) & 0xff;
	else if (size == 2)
		*val = (*val >> (BITS_PER_BYTE * (where & 2))) & 0xffff;

	dev_dbg(&bus->dev, "%s(): bus=%3d devfn=0x%04x where=0x%04x size=%d val=0x%08x\n",
		__func__, bus->number, devfn, where, size, *val);

	return PCIBIOS_SUCCESSFUL;
}

static int rzg3s_pcie_write_config_access(struct rzg3s_pcie_host *host, struct pci_bus *bus,
					  unsigned int devfn, int where, u32 data)
{
	/* Controller can't target itself. */
	if (pci_is_root_bus(bus) && !devfn) {
		u32 reg = where & ~0x3;

		/* Enable access control to the CFGU. */
		writel(RZG3S_PCI_PERM_CFG_HWINIT_EN, host->axi + RZG3S_PCI_PERM);
		/* Write data. */
		writel(data, host->pcie + reg);
		/* Disable access control to the CFGU. */
		writel(0, host->axi + RZG3S_PCI_PERM);

		return PCIBIOS_SUCCESSFUL;
	}

	return rzg3s_pcie_config_access(host, bus, devfn, where, &data, RZG3S_PCIE_CFG_ACCESS_WR);
}

static int rzg3s_pcie_write_conf(struct pci_bus *bus, unsigned int devfn, int where, int size,
				 u32 val)
{
	struct rzg3s_pcie_host *host = bus->sysdata;
	u32 shift, data;
	int ret;

	ret = rzg3s_pcie_read_config_access(host, bus, devfn, where, &data);
	if (ret != PCIBIOS_SUCCESSFUL)
		return ret;

	dev_dbg(&bus->dev, "%s(): bus=%3d devfn=0x%04x where=0x%04x size=%d val=0x%08x\n",
		__func__, bus->number, devfn, where, size, val);

	if (size == 1) {
		shift = BITS_PER_BYTE * (where & 3);
		data &= ~(0xff << shift);
		data |= ((val & 0xff) << shift);
	} else if (size == 2) {
		shift = BITS_PER_BYTE * (where & 2);
		data &= ~(0xffff << shift);
		data |= ((val & 0xffff) << shift);
	} else {
		data = val;
	}

	return rzg3s_pcie_write_config_access(host, bus, devfn, where, data);
}

static struct pci_ops rzg3s_pcie_ops = {
	.read	= rzg3s_pcie_read_conf,
	.write	= rzg3s_pcie_write_conf,
};

static void rzg3s_pcie_intx_irq_handler(struct irq_desc *desc)
{
	struct rzg3s_pcie_host *host = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	unsigned int irq = irq_desc_get_irq(desc);
	u32 intx = irq - host->intx_irqs[0];

	chained_irq_enter(chip, desc);
	generic_handle_domain_irq(host->intx_domain, intx);
	chained_irq_exit(chip, desc);
}

static irqreturn_t rzg3s_pcie_msi_irq(int irq, void *data)
{
	DECLARE_BITMAP(bitmap, RZG3S_PCI_MSI_INT_NR);
	struct rzg3s_pcie_host *host = data;
	struct rzg3s_pcie_msi *msi = &host->msi;
	unsigned long bit;
	u32 status;

	status = readl(host->axi + RZG3S_PCI_PINTRCVIS);
	if (!(status & RZG3S_PCI_PINTRCVIS_MSI))
		return IRQ_NONE;

	/* Clear the MSI. */
	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_PINTRCVIS, RZG3S_PCI_PINTRCVIS_MSI,
			       RZG3S_PCI_PINTRCVIS_MSI);
	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_MSGRCVIS, RZG3S_PCI_MSGRCVIS_MRI,
			       RZG3S_PCI_MSGRCVIS_MRI);

	for (u8 reg_id = 0; reg_id < RZG3S_PCI_MSI_INT_NR / RZG3S_PCI_MSI_INT_PER_REG; reg_id++) {
		status = readl(host->axi + RZG3S_PCI_MSIRS(reg_id));
		bitmap_write(bitmap, status, reg_id * RZG3S_PCI_MSI_INT_PER_REG,
			     RZG3S_PCI_MSI_INT_PER_REG);
	}

	for_each_set_bit(bit, bitmap, RZG3S_PCI_MSI_INT_NR) {
		int ret;

		ret = generic_handle_domain_irq(msi->domain->parent, bit);
		if (ret) {
			u8 reg_bit = bit % RZG3S_PCI_MSI_INT_PER_REG;
			u8 reg_id = bit / RZG3S_PCI_MSI_INT_PER_REG;

			/* Unknown MSI, just clear it. */
			writel(BIT(reg_bit), host->axi + RZG3S_PCI_MSIRS(reg_id));
		}
	}

	return IRQ_HANDLED;
}

static void rzg3s_pcie_msi_top_irq_ack(struct irq_data *d)
{
	irq_chip_ack_parent(d);
}

static void rzg3s_pcie_msi_top_irq_mask(struct irq_data *d)
{
	pci_msi_mask_irq(d);
	irq_chip_mask_parent(d);
}

static void rzg3s_pcie_msi_top_irq_unmask(struct irq_data *d)
{
	pci_msi_unmask_irq(d);
	irq_chip_unmask_parent(d);
}

static struct irq_chip rzg3s_pcie_msi_top_chip = {
	.name		= "PCIe MSI",
	.irq_ack	= rzg3s_pcie_msi_top_irq_ack,
	.irq_mask	= rzg3s_pcie_msi_top_irq_mask,
	.irq_unmask	= rzg3s_pcie_msi_top_irq_unmask,
};

static void rzg3s_pcie_msi_irq_ack(struct irq_data *d)
{
	struct rzg3s_pcie_msi *msi = irq_data_get_irq_chip_data(d);
	struct rzg3s_pcie_host *host = rzg3s_msi_to_host(msi);
	u8 reg_bit = d->hwirq % RZG3S_PCI_MSI_INT_PER_REG;
	u8 reg_id = d->hwirq / RZG3S_PCI_MSI_INT_PER_REG;

	guard(raw_spinlock_irqsave)(&host->hw_lock);

	writel(BIT(reg_bit), host->axi + RZG3S_PCI_MSIRS(reg_id));
}

static void rzg3s_pcie_msi_irq_mask(struct irq_data *d)
{
	struct rzg3s_pcie_msi *msi = irq_data_get_irq_chip_data(d);
	struct rzg3s_pcie_host *host = rzg3s_msi_to_host(msi);
	u8 reg_bit = d->hwirq % RZG3S_PCI_MSI_INT_PER_REG;
	u8 reg_id = d->hwirq / RZG3S_PCI_MSI_INT_PER_REG;

	guard(raw_spinlock_irqsave)(&host->hw_lock);

	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_MSIRM(reg_id), BIT(reg_bit), BIT(reg_bit));
}

static void rzg3s_pcie_msi_irq_unmask(struct irq_data *d)
{
	struct rzg3s_pcie_msi *msi = irq_data_get_irq_chip_data(d);
	struct rzg3s_pcie_host *host = rzg3s_msi_to_host(msi);
	u8 reg_bit = d->hwirq % RZG3S_PCI_MSI_INT_PER_REG;
	u8 reg_id = d->hwirq / RZG3S_PCI_MSI_INT_PER_REG;

	guard(raw_spinlock_irqsave)(&host->hw_lock);

	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_MSIRM(reg_id), BIT(reg_bit), 0);
}

static void rzg3s_pcie_msi_compose_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct rzg3s_pcie_msi *msi = irq_data_get_irq_chip_data(data);
	struct rzg3s_pcie_host *host = rzg3s_msi_to_host(msi);
	u32 lo, hi;

	/* Enable and msg data enable bits are part of the address lo. Drop them. */
	lo = readl(host->axi + RZG3S_PCI_MSIRCVWADRL) &
		   ~(RZG3S_PCI_MSIRCVWADRL_ENA | RZG3S_PCI_MSIRCVWADRL_MSG_DATA_ENA);
	hi = readl(host->axi + RZG3S_PCI_MSIRCVWADRU);

	msg->address_lo = lo;
	msg->address_hi = hi;
	msg->data = data->hwirq;
}

static struct irq_chip rzg3s_pcie_msi_bottom_chip = {
	.name			= "rz-pcie-msi",
	.irq_ack		= rzg3s_pcie_msi_irq_ack,
	.irq_mask		= rzg3s_pcie_msi_irq_mask,
	.irq_unmask		= rzg3s_pcie_msi_irq_unmask,
	.irq_compose_msi_msg	= rzg3s_pcie_msi_compose_msg,
};

static int rzg3s_pcie_msi_domain_alloc(struct irq_domain *domain, unsigned int virq,
				       unsigned int nr_irqs, void *args)
{
	struct rzg3s_pcie_msi *msi = domain->host_data;
	int hwirq;

	scoped_guard(mutex, &msi->map_lock) {
		hwirq = bitmap_find_free_region(msi->map, RZG3S_PCI_MSI_INT_NR,
						order_base_2(nr_irqs));
	}

	if (hwirq < 0)
		return -ENOSPC;

	for (unsigned int i = 0; i < nr_irqs; i++) {
		irq_domain_set_info(domain, virq + i, hwirq + i, &rzg3s_pcie_msi_bottom_chip,
				    domain->host_data, handle_edge_irq, NULL, NULL);
	}

	return 0;
}

static void rzg3s_pcie_msi_domain_free(struct irq_domain *domain, unsigned int virq,
				       unsigned int nr_irqs)
{
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);
	struct rzg3s_pcie_msi *msi = domain->host_data;

	guard(mutex)(&msi->map_lock);

	bitmap_release_region(msi->map, d->hwirq, order_base_2(nr_irqs));
}

static const struct irq_domain_ops rzg3s_pcie_msi_domain_ops = {
	.alloc	= rzg3s_pcie_msi_domain_alloc,
	.free	= rzg3s_pcie_msi_domain_free,
};

static struct msi_domain_info rzg3s_pcie_msi_info = {
	.flags	= MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_USE_DEF_CHIP_OPS |
		  MSI_FLAG_MULTI_PCI_MSI | MSI_FLAG_NO_AFFINITY,
	.chip	= &rzg3s_pcie_msi_top_chip,
};

static int rzg3s_pcie_msi_allocate_domains(struct rzg3s_pcie_msi *msi)
{
	struct rzg3s_pcie_host *host = rzg3s_msi_to_host(msi);
	struct device *dev = host->dev;
	struct fwnode_handle *fwnode = dev_fwnode(dev);
	struct irq_domain *parent;

	parent = irq_domain_create_linear(fwnode, RZG3S_PCI_MSI_INT_NR,
					  &rzg3s_pcie_msi_domain_ops, msi);
	if (!parent)
		return dev_err_probe(dev, -ENOMEM, "failed to create IRQ domain\n");
	irq_domain_update_bus_token(parent, DOMAIN_BUS_NEXUS);

	msi->domain = pci_msi_create_irq_domain(fwnode, &rzg3s_pcie_msi_info, parent);
	if (!msi->domain) {
		irq_domain_remove(parent);
		return dev_err_probe(dev, -ENOMEM, "failed to create MSI domain\n");
	}

	return 0;
}

static void rzg3s_pcie_msi_free_domains(struct rzg3s_pcie_msi *msi)
{
	struct irq_domain *parent = msi->domain->parent;

	irq_domain_remove(msi->domain);
	irq_domain_remove(parent);
}

static int rzg3s_pcie_msi_hw_setup(struct rzg3s_pcie_host *host)
{
	struct rzg3s_pcie_msi *msi = &host->msi;

	/* Set MSI window size. HW will set the window to RZG3S_PCI_MSI_INT_NR * 4 bytes. */
	writel(RZG3S_PCI_MSI_INT_NR - 1, host->axi + RZG3S_PCI_MSIRCVWMSKL);

	/* Set MSI window address and enable MSI window. */
	writel(upper_32_bits(msi->window_base), host->axi + RZG3S_PCI_MSIRCVWADRU);
	writel(lower_32_bits(msi->window_base) | RZG3S_PCI_MSIRCVWADRL_ENA |
	       RZG3S_PCI_MSIRCVWADRL_MSG_DATA_ENA,
	       host->axi + RZG3S_PCI_MSIRCVWADRL);

	/* Set MSI receive enable. */
	for (u8 reg_id = 0; reg_id < RZG3S_PCI_MSI_INT_NR / RZG3S_PCI_MSI_INT_PER_REG; reg_id++)
		writel(RZG3S_PCI_MSIRE_ENA, host->axi + RZG3S_PCI_MSIRE(reg_id));

	/* Enable message receive interrupts. */
	writel(RZG3S_PCI_MSGRCVIE_MSG_RCV, host->axi + RZG3S_PCI_MSGRCVIE);

	/* Enable MSI. */
	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_PINTRCVIE, RZG3S_PCI_PINTRCVIE_MSI,
			       RZG3S_PCI_PINTRCVIE_MSI);

	return 0;
}

static int rzg3s_pcie_msi_setup(struct rzg3s_pcie_host *host)
{
	size_t size = RZG3S_PCI_MSI_INT_NR * sizeof(u32);
	struct rzg3s_pcie_msi *msi = &host->msi;
	struct device *dev = host->dev;
	int id, ret;

	msi->pages = __get_free_pages(GFP_KERNEL | GFP_DMA, 0);
	if (!msi->pages)
		return -ENOMEM;

	msi->dma_addr = dma_map_single(dev, (void *)msi->pages, size * 2, DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dev, msi->dma_addr)) {
		ret = -ENOMEM;
		goto free_pages;
	}

	/*
	 * According to the RZ/G3S HW manual (section 34.4.5.2 Setting the MSI Window)
	 * the MSI window need to be within any AXI window. Find an AXI window to setup
	 * the MSI window.
	 */
	for (id = 0; id < RZG3S_MAX_WINDOWS; id++) {
		u64 base, basel, baseu;
		u64 mask, maskl, masku;

		basel = readl(host->axi + RZG3S_PCI_AWBASEL(id));
		/* Skip checking this AXI window if it's not enabled. */
		if (!(basel & RZG3S_PCI_AWBASEL_WIN_ENA))
			continue;

		baseu = readl(host->axi + RZG3S_PCI_AWBASEU(id));
		base = baseu << 32 | basel;

		maskl = readl(host->axi + RZG3S_PCI_AWMASKL(id));
		masku = readl(host->axi + RZG3S_PCI_AWMASKU(id));
		mask = masku << 32 | maskl;

		if (msi->dma_addr < base || msi->dma_addr > base + mask)
			continue;

		break;
	}

	if (id == RZG3S_MAX_WINDOWS) {
		ret = -EINVAL;
		goto dma_unmap;
	}

	/* The MSI base address need to be aligned to the MSI size. */
	msi->window_base = ALIGN(msi->dma_addr, size);
	if (msi->window_base < msi->dma_addr) {
		ret = -EINVAL;
		goto dma_unmap;
	}

	rzg3s_pcie_msi_hw_setup(host);

	return 0;

dma_unmap:
	dma_unmap_single(dev, msi->dma_addr, size * 2, DMA_BIDIRECTIONAL);
free_pages:
	free_pages(msi->pages, 0);
	return ret;
}

static int rzg3s_pcie_msi_enable(struct rzg3s_pcie_host *host)
{
	struct rzg3s_pcie_msi *msi = &host->msi;
	struct device *dev = host->dev;
	const char *devname;
	int irq, ret;

	mutex_init(&msi->map_lock);

	irq = of_irq_get_byname(dev->of_node, "intmsi_rc");
	if (irq <= 0)
		return dev_err_probe(dev, irq ? irq : -EINVAL, "Failed to get MSI IRQ!\n");

	devname = devm_kasprintf(dev, GFP_KERNEL, "%s-msi", dev_name(dev));
	if (!devname)
		return -ENOMEM;

	ret = rzg3s_pcie_msi_allocate_domains(msi);
	if (ret)
		return ret;

	ret = devm_request_irq(dev, irq, rzg3s_pcie_msi_irq, 0, devname, host);
	if (ret) {
		dev_err_probe(dev, ret, "Failed to request IRQ: %d\n", ret);
		goto free_domains;
	}

	ret = rzg3s_pcie_msi_setup(host);
	if (ret) {
		dev_err_probe(dev, ret, "Failed to setup MSI!\n");
		goto free_domains;
	}

	return 0;

free_domains:
	rzg3s_pcie_msi_free_domains(msi);
	return ret;
}

static void rzg3s_pcie_msi_teardown(void *data)
{
	size_t size = RZG3S_PCI_MSI_INT_NR * sizeof(u32);
	struct rzg3s_pcie_host *host = data;
	struct rzg3s_pcie_msi *msi = &host->msi;

	/* Disable MSI. */
	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_PINTRCVIE, RZG3S_PCI_PINTRCVIE_MSI, 0);

	/* Disable message receive interrupts. */
	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_MSGRCVIE, RZG3S_PCI_MSGRCVIE_MSG_RCV, 0);

	/* Disable MSI receive enable. */
	for (u8 reg_id = 0; reg_id < RZG3S_PCI_MSI_INT_NR / RZG3S_PCI_MSI_INT_PER_REG; reg_id++)
		writel(0, host->axi + RZG3S_PCI_MSIRE(reg_id));

	/* Disable MSI window. */
	writel(0, host->axi + RZG3S_PCI_MSIRCVWADRL);

	/* Free unused memory. */
	dma_unmap_single(host->dev, msi->dma_addr, size * 2, DMA_BIDIRECTIONAL);
	free_pages(msi->pages, 0);

	rzg3s_pcie_msi_free_domains(msi);
}

static void rzg3s_pcie_intx_ack(struct irq_data *d)
{
	struct rzg3s_pcie_host *host = irq_data_get_irq_chip_data(d);

	guard(raw_spinlock_irqsave)(&host->hw_lock);

	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_PINTRCVIS, RZG3S_PCI_PINTRCVIS_INTX(d->hwirq),
			       RZG3S_PCI_PINTRCVIS_INTX(d->hwirq));
}

static void rzg3s_pcie_intx_mask(struct irq_data *d)
{
	struct rzg3s_pcie_host *host = irq_data_get_irq_chip_data(d);

	guard(raw_spinlock_irqsave)(&host->hw_lock);

	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_PINTRCVIE, RZG3S_PCI_PINTRCVIE_INTX(d->hwirq),
			       0);
}

static void rzg3s_pcie_intx_unmask(struct irq_data *d)
{
	struct rzg3s_pcie_host *host = irq_data_get_irq_chip_data(d);

	guard(raw_spinlock_irqsave)(&host->hw_lock);

	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_PINTRCVIE, RZG3S_PCI_PINTRCVIE_INTX(d->hwirq),
			       RZG3S_PCI_PINTRCVIE_INTX(d->hwirq));
}

static struct irq_chip rzg3s_pcie_intx_irq_chip = {
	.name = "PCIe INTx",
	.irq_ack = rzg3s_pcie_intx_ack,
	.irq_mask = rzg3s_pcie_intx_mask,
	.irq_unmask = rzg3s_pcie_intx_unmask,
};

static int rzg3s_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
			       irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &rzg3s_pcie_intx_irq_chip, handle_level_irq);
	irq_set_chip_data(irq, domain->host_data);

	return 0;
}

static const struct irq_domain_ops rzg3s_pcie_intx_domain_ops = {
	.map = rzg3s_pcie_intx_map,
	.xlate = irq_domain_xlate_onetwocell,
};

static int rzg3s_pcie_intx_setup(struct rzg3s_pcie_host *host)
{
	struct device *dev = host->dev;
	struct device_node *np = dev->of_node;
	struct device_node *intc_np __free(device_node) = of_get_child_by_name(np,
								"legacy-interrupt-controller");
	int irq_count;

	if (!intc_np) {
		dev_warn(dev, "legacy-interrupt-controller node is absent\n");
		return 0;
	}

	irq_count = of_irq_count(intc_np);
	if (irq_count != PCI_NUM_INTX)
		return dev_err_probe(dev, -EINVAL,
				     "Invalid IRQ entries in legacy-interrupt-controller\n");

	for (int i = 0; i < irq_count; i++) {
		int irq = irq_of_parse_and_map(intc_np, i);

		if (!irq)
			return dev_err_probe(dev, -EINVAL, "Failed to parse and map INTx IRQ\n");

		host->intx_irqs[i] = irq;
		irq_set_chained_handler_and_data(irq, rzg3s_pcie_intx_irq_handler, host);
	}

	host->intx_domain = irq_domain_add_linear(intc_np, irq_count,
						  &rzg3s_pcie_intx_domain_ops, host);
	if (!host->intx_domain)
		return dev_err_probe(dev, -EINVAL, "Failed to add irq domain for INTX irqs\n");
	irq_domain_update_bus_token(host->intx_domain, DOMAIN_BUS_WIRED);

	return 0;
}

static void rzg3s_pcie_intx_teardown(void *data)
{
	struct rzg3s_pcie_host *host = data;

	irq_domain_remove(host->intx_domain);
}

static int rzg3s_pcie_set_max_link_speed(struct rzg3s_pcie_host *host)
{
	u32 lcs, cs2, link_speed, remote_supported_link_speeds, tmp;
	u8 ltssm_state_l0 = 0xc;
	int ret;

	/*
	 * According to the RZ/G3S HW manual (section 34.6.3 Caution when Changing
	 * the Speed Spontaneously) link speed change can be done only when the
	 * link training and status state machine in the PCIe Core Link is L0.
	 */
	ret = readl_poll_timeout(host->axi + RZG3S_PCI_PCSTAT1, tmp,
				 FIELD_GET(RZG3S_PCI_PCSTAT1_LTSSM_STATE, tmp) == ltssm_state_l0,
				 100, 1000);
	if (ret) {
		dev_dbg(host->dev, "Could not set max link speed! LTSSM not in L0, state=%lx\n",
			FIELD_GET(RZG3S_PCI_PCSTAT1_LTSSM_STATE, tmp));
		return ret;
	}

	lcs = readl(host->pcie + RZG3S_PCI_CFG_LINKCS);
	cs2 = readl(host->axi + RZG3S_PCI_PCSTAT2);

	link_speed = FIELD_GET(RZG3S_PCI_CFG_LINKCS_CUR_LS, lcs);
	remote_supported_link_speeds = FIELD_GET(RZG3S_PCI_PCSTAT2_SDRIRE, cs2);

	/* Return if link is @ 5.0 GT/s or the connected device doesn't support it. */
	if (link_speed == RZG3S_PCIE_LINK_SPEED_5_0_GTS ||
	    !(remote_supported_link_speeds & BIT(RZG3S_PCIE_LINK_SPEED_5_0_GTS)))
		return 0;

	/* Set target Link speed to 5.0 GT/s. */
	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_CFG_LINCS2, RZG3S_PCI_CFG_LINCS2_TARGET_LS,
			       FIELD_PREP(RZG3S_PCI_CFG_LINCS2_TARGET_LS,
					  RZG3S_PCIE_LINK_SPEED_5_0_GTS));

	/* Request link speed change. */
	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_PCCTRL2,
			       RZG3S_PCI_PCCTRL2_LS_CHG_REQ | RZG3S_PCI_PCCTRL2_LS_CHG,
			       RZG3S_PCI_PCCTRL2_LS_CHG_REQ |
			       FIELD_PREP(RZG3S_PCI_PCCTRL2_LS_CHG,
					  RZG3S_PCIE_LINK_SPEED_5_0_GTS - 1));

	ret = readl_poll_timeout(host->axi + RZG3S_PCI_PCSTAT2, cs2,
				 (cs2 & RZG3S_PCI_PCSTAT2_LS_CHG_DONE), 100, 1000);

	/*
	 * According to the RZ/G3S HW manual (section 34.6.3 Caution when Changing
	 * the Speed Spontaneously) the PCI_PCCTRL2_LS_CHG_REQ should be
	 * de-asserted after checking for PCI_PCSTAT2_LS_CHG_DONE.
	 */
	rzg3s_pcie_update_bits(host->axi, RZG3S_PCI_PCCTRL2, RZG3S_PCI_PCCTRL2_LS_CHG_REQ, 0);

	return ret;
}

static void rzg3s_pcie_cfg_init(struct rzg3s_pcie_host *host)
{
	/* Enable access control to the CFGU. */
	writel(RZG3S_PCI_PERM_CFG_HWINIT_EN, host->axi + RZG3S_PCI_PERM);

	/* Update vendor ID and device ID. */
	writel(FIELD_PREP(RZG3S_PCI_CFG_VID_VENDOR, host->vendor_id) |
	       FIELD_PREP(RZG3S_PCI_CFG_VID_DEVICE, host->device_id),
	       host->pcie + RZG3S_PCI_CFG_VID);

	/* HW manual recommends to write 0xffffffff on initialization. */
	writel(0xffffffff, host->pcie + RZG3S_PCI_CFG_BARMSK00L);
	writel(0xffffffff, host->pcie + RZG3S_PCI_CFG_BARMSK00U);

	/* Primary bus = 0, secondary bus = 1, subordinate bus = 1. */
	writel(FIELD_PREP(RZG3S_PCI_CFG_BNR_SUBORDINATE_BUS, 1) |
	       FIELD_PREP(RZG3S_PCI_CFG_BNR_SECONDARY_BUS, 1),
	       host->pcie + RZG3S_PCI_CFG_BNR);

	/* Disable access control to the CFGU. */
	writel(0, host->axi + RZG3S_PCI_PERM);
}

static void rzg3s_pcie_irqs_init(struct rzg3s_pcie_host *host)
{
	/*
	 * According to the HW manual of the RZ/G3S (sections corresponding
	 * to all registers written with ~0U), the hardware ignores value
	 * written to unused bits. Writing ~0U to these registers should be
	 * safe.
	 */

	/* Clear the link state and PM transitions. */
	writel(RZG3S_PCI_PEIS0_DL_UPDOWN | RZG3S_PCI_PEIS0_RX_DLLP_PM_ENTER,
	       host->axi + RZG3S_PCI_PEIS0);

	/* Disable all interrupts. */
	writel(0, host->axi + RZG3S_PCI_PEIE0);

	/* Clear all parity and ecc error interrupts. */
	writel(~0U, host->axi + RZG3S_PCI_PEIS1);

	/* Disable all parity and ecc error interrupts. */
	writel(0, host->axi + RZG3S_PCI_PEIE1);

	/* Clear all AXI master error interrupts. */
	writel(~0U, host->axi + RZG3S_PCI_AMEIS);

	/* Clear all AXI slave error interrupts. */
	writel(~0U, host->axi + RZG3S_PCI_ASEIS1);

	/* Clear all message receive interrupts. */
	writel(~0U, host->axi + RZG3S_PCI_MSGRCVIS);
}

static int rzg3s_pcie_resets_bulk_set(int (*action)(int num, struct reset_control_bulk_data *rstcs),
				      struct reset_control **resets, u8 num_resets)
{
	struct reset_control_bulk_data *data __free(kfree) =
		kcalloc(num_resets, sizeof(*data), GFP_KERNEL);

	if (!data)
		return -ENOMEM;

	for (u8 i = 0; i < num_resets; i++)
		data[i].rstc = resets[i];

	return action(num_resets, data);
}

static int
rzg3s_pcie_resets_init(struct device *dev, struct reset_control ***resets,
		       struct reset_control *(*action)(struct device *dev, const char *id),
		       const char * const *reset_names, u8 num_resets)
{
	*resets = devm_kcalloc(dev, num_resets, sizeof(struct reset_control *), GFP_KERNEL);
	if (!*resets)
		return -ENOMEM;

	for (u8 i = 0; i < num_resets; i++) {
		(*resets)[i] = action(dev, reset_names[i]);
		if (IS_ERR((*resets)[i]))
			return PTR_ERR((*resets)[i]);
	}

	return 0;
}

static int rzg3s_pcie_resets_prepare(struct rzg3s_pcie_host *host)
{
	const char * const cfg_resets[] = { "rst_b", "rst_ps_b", "rst_gp_b", "rst_rsm_b", };
	const char * const power_resets[] = { "aresetn", "rst_cfg_b", "rst_load_b", };
	int ret;

	/*
	 * According to the RZ/G3S HW manual (section 34.5.1.2 De-asserting the Reset)
	 * the PCIe IP needs to wait 5ms from power on to the de-assertion of reset.
	 */
	usleep_range(5000, 5100);
	host->num_power_resets = ARRAY_SIZE(power_resets);
	ret = rzg3s_pcie_resets_init(host->dev, &host->power_resets,
				     devm_reset_control_get_exclusive_deasserted,
				     power_resets, ARRAY_SIZE(power_resets));
	if (ret)
		return ret;

	host->num_cfg_resets = ARRAY_SIZE(cfg_resets);
	return rzg3s_pcie_resets_init(host->dev, &host->cfg_resets,
				      devm_reset_control_get_exclusive,
				      cfg_resets, ARRAY_SIZE(cfg_resets));
}

static void rzg3s_pcie_cfg_resets_action(void *data)
{
	struct rzg3s_pcie_host *host = data;

	rzg3s_pcie_resets_bulk_set(reset_control_bulk_assert, host->cfg_resets,
				   host->num_cfg_resets);
}

static int rzg3s_pcie_hw_init(struct rzg3s_pcie_host *host, bool probe)
{
	u32 val;
	int ret;

	/* Set the PCIe related registers. */
	rzg3s_pcie_cfg_init(host);

	/* Set the interrupts. */
	rzg3s_pcie_irqs_init(host);

	ret = rzg3s_pcie_resets_bulk_set(reset_control_bulk_deassert, host->cfg_resets,
					 host->num_cfg_resets);
	if (ret)
		return ret;

	/* Wait for link up. */
	ret = readl_poll_timeout(host->axi + RZG3S_PCI_PCSTAT1, val,
				 !(val & RZG3S_PCI_PCSTAT1_DL_DOWN_STS), 5000, 500000);
	if (ret) {
		rzg3s_pcie_resets_bulk_set(reset_control_bulk_assert, host->cfg_resets,
					   host->num_cfg_resets);
		return ret;
	}

	val = readl(host->axi + RZG3S_PCI_PCSTAT2);
	dev_info(host->dev, "PCIe link status [0x%x]\n", val);

	val = FIELD_GET(RZG3S_PCI_PCSTAT2_STATE_RX_DETECT, val);
	dev_info(host->dev, "PCIe x%d: link up\n", hweight32(val));

	if (probe)
		ret = devm_add_action_or_reset(host->dev, rzg3s_pcie_cfg_resets_action, host);

	return ret;
}

static void rzg3s_pcie_set_inbound_window(struct rzg3s_pcie_host *host, u64 cpu_addr,
					  u64 pci_addr, u64 size, int id)
{
	/* Set CPU window base address. */
	writel(upper_32_bits(cpu_addr), host->axi + RZG3S_PCI_ADESTU(id));
	writel(lower_32_bits(cpu_addr), host->axi + RZG3S_PCI_ADESTL(id));

	/* Set window size. */
	writel(upper_32_bits(size), host->axi + RZG3S_PCI_AWMASKU(id));
	writel(lower_32_bits(size), host->axi + RZG3S_PCI_AWMASKL(id));

	/* Set PCIe window base address and enable the window.. */
	writel(upper_32_bits(pci_addr), host->axi + RZG3S_PCI_AWBASEU(id));
	writel(lower_32_bits(pci_addr) | RZG3S_PCI_AWBASEL_WIN_ENA,
	       host->axi + RZG3S_PCI_AWBASEL(id));
}

static int rzg3s_pcie_set_inbound_windows(struct rzg3s_pcie_host *host,
					  struct resource_entry *entry, int *index)
{
	u64 pci_addr = entry->res->start - entry->offset;
	u64 cpu_addr = entry->res->start;
	u64 cpu_end = entry->res->end;
	u64 size_id = 0;
	int id = *index;
	u64 size;

	while (cpu_addr < cpu_end) {
		if (id >= RZG3S_MAX_WINDOWS)
			return dev_err_probe(host->dev, -EINVAL,
					     "Failed to set inbound windows!\n");

		size = resource_size(entry->res) - size_id;

		/*
		 * According to the RZ/G3S HW manual (section 34.3.1.71 AXI Window Mask
		 * (Lower) Registers) the min size is 4K.
		 */
		size = max(size, 4096);

		/*
		 * According the RZ/G3S HW manual (sections:
		 * - 34.3.1.69 AXI Window Base (Lower) Registers
		 * - 34.3.1.71 AXI Window Mask (Lower) Registers
		 * - 34.3.1.73 AXI Destination (Lower) Registers)
		 * the CPU addr, PCIe addr, size should be 4K alined and be a power of 2.
		 */
		size = ALIGN(size, 4096);

		/*
		 * According to the RZ/G3S HW manual (section 34.3.1.71 AXI Window Mask
		 * (Lower) Registers) HW expects first 12 LSB bits to be 0xfff.
		 * Extract 1 from size for this.
		 */
		size = roundup_pow_of_two(size) - 1;

		cpu_addr = ALIGN(cpu_addr, 4096);
		pci_addr = ALIGN(pci_addr, 4096);

		rzg3s_pcie_set_inbound_window(host, cpu_addr, pci_addr, size, id);

		pci_addr += size;
		cpu_addr += size;
		size_id = size;
		id++;
	}
	*index = id;

	return 0;
}

static int rzg3s_pcie_parse_map_dma_ranges(struct rzg3s_pcie_host *host)
{
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(host);
	struct resource_entry *entry;
	int i = 0, ret;

	resource_list_for_each_entry(entry, &bridge->dma_ranges) {
		ret = rzg3s_pcie_set_inbound_windows(host, entry, &i);
		if (ret)
			return ret;
	}

	return 0;
}

static void rzg3s_pcie_set_outbound_window(struct rzg3s_pcie_host *host, struct resource_entry *win,
					   int id)
{
	struct resource *res = win->res;
	resource_size_t size = resource_size(res);
	resource_size_t res_start;

	if (res->flags & IORESOURCE_IO)
		res_start = pci_pio_to_address(res->start) - win->offset;
	else
		res_start = res->start - win->offset;

	/*
	 * According to the RZ/G3S HW manual (section 34.3.1.75 PCIe Window Base
	 * (Lower) Registers) the window base address need to be 4K aligned.
	 */
	res_start = ALIGN(res_start, 4096);

	size = ALIGN(size, 4096);
	size = roundup_pow_of_two(size) - 1;

	/* Set PCIe destination. */
	writel(upper_32_bits(res_start), host->axi + RZG3S_PCI_PDESTU(id));
	writel(lower_32_bits(res_start), host->axi + RZG3S_PCI_PDESTL(id));

	/* Set PCIe window mask. */
	writel(upper_32_bits(size), host->axi + RZG3S_PCI_PWMASKU(id));
	writel(lower_32_bits(size), host->axi + RZG3S_PCI_PWMASKL(id));

	/* Set PCIe window base and enable the window. */
	writel(upper_32_bits(res_start), host->axi + RZG3S_PCI_PWBASEU(id));
	writel(lower_32_bits(res_start) | RZG3S_PCI_PWBASEL_ENA,
	       host->axi + RZG3S_PCI_PWBASEL(id));
}

static int rzg3s_pcie_parse_map_ranges(struct rzg3s_pcie_host *host)
{
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(host);
	struct resource_entry *win;
	int i = 0;

	resource_list_for_each_entry(win, &bridge->windows) {
		struct resource *res = win->res;

		if (i >= RZG3S_MAX_WINDOWS)
			return dev_err_probe(host->dev, -EINVAL,
					     "Failed to set outbound windows!\n");

		if (!res->flags)
			continue;

		switch (resource_type(res)) {
		case IORESOURCE_IO:
		case IORESOURCE_MEM:
			rzg3s_pcie_set_outbound_window(host, win, i);
			i++;
			break;
		}
	}

	return 0;
}

static int rzg3s_pcie_phy_init(struct rzg3s_pcie_host *host)
{
	static const u32 xcfgd_settings[RZG3S_PCI_PHY_XCFGD_NUM] = {
		[8]  = 0xe0006801, 0x007f7e30, 0x183e0000, 0x978ff500,
		       0xec000000, 0x009f1400, 0x0000d009,
		[17] = 0x78000000,
		[19] = 0x00880000, 0x000005c0, 0x07000000, 0x00780920,
		       0xc9400ce2, 0x90000c0c, 0x000c1414, 0x00005034,
		       0x00006000, 0x00000001,
	};
	static const u32 xcfga_cmn_settings[RZG3S_PCI_PHY_XCFGA_CMN_NUM] = {
		0x00000d10, 0x08310100, 0x00c21404, 0x013c0010, 0x01874440,
		0x1a216082, 0x00103440, 0x00000080, 0x00000010, 0x0c1000c1,
		0x1000c100, 0x0222000c, 0x00640019, 0x00a00028, 0x01d11228,
		0x0201001d,
	};
	static const u32 xcfga_rx_settings[RZG3S_PCI_PHY_XCFGA_RX_NUM] = {
		0x07d55000, 0x030e3f00, 0x00000288, 0x102c5880, 0x0000000b,
		0x04141441, 0x00641641, 0x00d63d63, 0x00641641, 0x01970377,
		0x00190287, 0x00190028, 0x00000028,
	};

	writel(RZG3S_PCI_PERM_PIPE_PHY_REG_EN, host->axi + RZG3S_PCI_PERM);

	for (u8 i = 0; i < RZG3S_PCI_PHY_XCFGD_NUM; i++)
		writel(xcfgd_settings[i], host->axi + RZG3S_PCI_PHY_XCFGD(i));

	for (u8 i = 0; i < RZG3S_PCI_PHY_XCFGA_CMN_NUM; i++)
		writel(xcfga_cmn_settings[i], host->axi + RZG3S_PCI_PHY_XCFGA_CMN(i));

	for (u8 i = 0; i < RZG3S_PCI_PHY_XCFGA_RX_NUM; i++)
		writel(xcfga_rx_settings[i], host->axi + RZG3S_PCI_PHY_XCFGA_RX(i));

	writel(0x107, host->axi + RZG3S_PCI_PHY_XCFGA_TX);

	/* Select PHY settings values. */
	writel(RZG3S_PCI_PHY_XCFG_CTRL_PHYREG_SEL, host->axi + RZG3S_PCI_PHY_XCFG_CTRL);

	writel(0, host->axi + RZG3S_PCI_PERM);

	return 0;
}

static void rzg3s_pcie_pm_runtime_put(void *data)
{
	pm_runtime_put_sync(data);
}

static void rzg3s_pcie_sysc_signal_action(void *data)
{
	struct regmap *sysc = data;

	/*
	 * SYSC RST_RSM_B signal need to be asserted before turning off the power to
	 * the PHY.
	 */
	regmap_update_bits(sysc, RZG3S_SYS_PCIE_RST_RSM_B, RZG3S_SYS_PCIE_RST_RSM_B_MASK,
			   FIELD_PREP(RZG3S_SYS_PCIE_RST_RSM_B_MASK, 0));
}

static int rzg3s_pcie_setup(struct rzg3s_pcie_host *host,
			    int (*intx_setup)(struct rzg3s_pcie_host *host),
			    int (*msi_setup)(struct rzg3s_pcie_host *host),
			    bool probe)
{
	struct device *dev = host->dev;
	int ret;

	/* Set inbound windows. */
	ret = rzg3s_pcie_parse_map_dma_ranges(host);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to set inbound windows!\n");

	/* Set outbound windows. */
	ret = rzg3s_pcie_parse_map_ranges(host);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to set outbound windows!\n");

	/* Set the PHY, if any. */
	if (host->phy_init) {
		ret = host->phy_init(host);
		if (ret)
			return dev_err_probe(dev, ret, "Failed to set the PHY!\n");
	}

	if (intx_setup) {
		ret = intx_setup(host);
		if (ret)
			return dev_err_probe(dev, ret, "Failed to setup INTx\n");

		if (probe) {
			ret = devm_add_action_or_reset(dev, rzg3s_pcie_intx_teardown, host);
			if (ret)
				return dev_err_probe(dev, ret, "Failed to add INTx action\n");
		}
	}

	/* Set the MSIs. */
	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		ret = msi_setup(host);
		if (ret)
			return dev_err_probe(dev, ret, "Failed to setup MSIs\n");

		if (probe) {
			ret = devm_add_action_or_reset(dev, rzg3s_pcie_msi_teardown, host);
			if (ret)
				return dev_err_probe(dev, ret, "Failed to add MSI action\n");
		}
	}

	/* Initialize the HW. */
	ret = rzg3s_pcie_hw_init(host, probe);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to initialize the HW!\n");

	/* Try to set maximum supported link speed (5.0 GT/s). */
	ret = rzg3s_pcie_set_max_link_speed(host);
	if (ret)
		dev_info(dev, "Failed to set link speed to 5.0 GT/s\n");

	return 0;
}

static int rzg3s_pcie_probe_helper(struct platform_device *pdev, void *devres_group_id)
{
	struct pci_host_bridge *bridge;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *sysc_np __free(device_node) =
		of_parse_phandle(np, "renesas,sysc", 0);
	struct rzg3s_pcie_host *host;
	int ret;

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*host));
	if (!bridge)
		return -ENOMEM;

	host = pci_host_bridge_priv(bridge);
	host->dev = dev;
	host->phy_init = device_get_match_data(dev);
	host->devres_group_id = devres_group_id;
	platform_set_drvdata(pdev, host);

	host->axi = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(host->axi))
		return PTR_ERR(host->axi);
	host->pcie = host->axi + RZG3S_PCI_CFG_BASE;

	ret = of_property_read_u32(np, "vendor-id", &host->vendor_id);
	if (ret)
		return ret;

	ret = of_property_read_u32(np, "device-id", &host->device_id);
	if (ret)
		return ret;

	host->sysc = syscon_node_to_regmap(sysc_np);
	if (IS_ERR(host->sysc))
		return PTR_ERR(host->sysc);

	ret = regmap_update_bits(host->sysc, RZG3S_SYS_PCIE_RST_RSM_B,
				 RZG3S_SYS_PCIE_RST_RSM_B_MASK,
				 FIELD_PREP(RZG3S_SYS_PCIE_RST_RSM_B_MASK, 1));
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, rzg3s_pcie_sysc_signal_action, host->sysc);
	if (ret)
		return ret;

	ret = rzg3s_pcie_resets_prepare(host);
	if (ret)
		return ret;

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, rzg3s_pcie_pm_runtime_put, dev);
	if (ret)
		return ret;

	raw_spin_lock_init(&host->hw_lock);

	ret = rzg3s_pcie_setup(host, rzg3s_pcie_intx_setup, rzg3s_pcie_msi_enable, true);
	if (ret)
		return ret;

	bridge->sysdata = host;
	bridge->ops = &rzg3s_pcie_ops;
	return pci_host_probe(bridge);
}

static int rzg3s_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	void *devres_group_id;
	int ret;

	devres_group_id = devres_open_group(dev, NULL, GFP_KERNEL);
	if (!devres_group_id)
		return -ENOMEM;

	ret = rzg3s_pcie_probe_helper(pdev, devres_group_id);
	if (ret)
		devres_release_group(dev, devres_group_id);

	return ret;
}

static void rzg3s_pcie_remove(struct platform_device *pdev)
{
	struct rzg3s_pcie_host *host = dev_get_drvdata(&pdev->dev);
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(host);

	pci_lock_rescan_remove();
	pci_stop_root_bus(bridge->bus);
	pci_remove_root_bus(bridge->bus);
	pci_unlock_rescan_remove();

	devres_release_group(host->dev, host->devres_group_id);
}

static int rzg3s_pcie_suspend(struct device *dev)
{
	struct rzg3s_pcie_host *host = dev_get_drvdata(dev);
	struct regmap *sysc = host->sysc;
	int ret;

	ret = pm_runtime_put_sync(dev);
	if (ret)
		return ret;

	ret = rzg3s_pcie_resets_bulk_set(reset_control_bulk_assert, host->power_resets,
					 host->num_power_resets);
	if (ret)
		goto rpm_restore;

	ret = rzg3s_pcie_resets_bulk_set(reset_control_bulk_assert, host->cfg_resets,
					 host->num_cfg_resets);
	if (ret)
		goto power_resets_restore;

	ret = regmap_update_bits(sysc, RZG3S_SYS_PCIE_RST_RSM_B, RZG3S_SYS_PCIE_RST_RSM_B_MASK,
				 FIELD_PREP(RZG3S_SYS_PCIE_RST_RSM_B_MASK, 0));
	if (ret)
		goto cfg_resets_restore;

	return 0;

	/* Restore the previous state if any error happens. */
cfg_resets_restore:
	rzg3s_pcie_resets_bulk_set(reset_control_bulk_deassert, host->cfg_resets,
				   host->num_cfg_resets);
power_resets_restore:
	rzg3s_pcie_resets_bulk_set(reset_control_bulk_deassert, host->power_resets,
				   host->num_power_resets);
rpm_restore:
	pm_runtime_resume_and_get(dev);
	return ret;
}

static int rzg3s_pcie_resume(struct device *dev)
{
	struct rzg3s_pcie_host *host = dev_get_drvdata(dev);
	struct regmap *sysc = host->sysc;
	int ret;

	ret = regmap_update_bits(sysc, RZG3S_SYS_PCIE_RST_RSM_B, RZG3S_SYS_PCIE_RST_RSM_B_MASK,
				 FIELD_PREP(RZG3S_SYS_PCIE_RST_RSM_B_MASK, 1));
	if (ret)
		return ret;

	/*
	 * According to the RZ/G3S HW manual (section 34.5.1.2 De-asserting the Reset)
	 * the PCIe IP needs to wait 5ms from power on to the de-assertion of reset.
	 */
	usleep_range(5000, 5100);
	ret = rzg3s_pcie_resets_bulk_set(reset_control_bulk_deassert, host->power_resets,
					 host->num_power_resets);
	if (ret)
		goto assert_rst_rsm_b;

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		goto assert_power_resets;

	ret = rzg3s_pcie_setup(host, NULL, rzg3s_pcie_msi_hw_setup, false);
	if (ret)
		goto rpm_put;

	return 0;

	/*
	 * If any error happens there is no way to recover the IP. Put it in the
	 * lowest possible power state.
	 */
rpm_put:
	pm_runtime_put_sync(dev);
assert_power_resets:
	rzg3s_pcie_resets_bulk_set(reset_control_bulk_assert, host->power_resets,
				   host->num_power_resets);
assert_rst_rsm_b:
	regmap_update_bits(sysc, RZG3S_SYS_PCIE_RST_RSM_B, RZG3S_SYS_PCIE_RST_RSM_B_MASK,
			   FIELD_PREP(RZG3S_SYS_PCIE_RST_RSM_B_MASK, 0));
	return ret;
}

static DEFINE_NOIRQ_DEV_PM_OPS(rzg3s_pcie_pm_ops, rzg3s_pcie_suspend, rzg3s_pcie_resume);

static const struct of_device_id rzg3s_pcie_of_match[] = {
	{ .compatible = "renesas,r9a08g045s33-pcie", .data = rzg3s_pcie_phy_init },
	{},
};

static struct platform_driver rzg3s_pcie_driver = {
	.driver = {
		.name = "rz-pcie-host",
		.of_match_table = rzg3s_pcie_of_match,
		.pm = pm_ptr(&rzg3s_pcie_pm_ops),
	},
	.probe = rzg3s_pcie_probe,
	.remove = rzg3s_pcie_remove,
};
module_platform_driver(rzg3s_pcie_driver);

MODULE_DESCRIPTION("Renesas RZ/G3S PCIe host driver");
MODULE_AUTHOR("Claudiu Beznea <claudiu.beznea.uj@bp.renesas.com>");
MODULE_LICENSE("GPL");
