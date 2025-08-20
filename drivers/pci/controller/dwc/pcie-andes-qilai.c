// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the PCIe Controller in QiLai from Andes
 *
 * Copyright (C) 2025 Andes Technology Corporation
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/types.h>

#include "pcie-designware.h"

#define PCIE_INTR_CONTROL1			0x15c
#define PCIE_MSI_CTRL_INT_EN			BIT(28)

#define PCIE_LOGIC_COHERENCY_CONTROL3		0x8e8
/* Write-Back, Read and Write Allocate */
#define IOCP_ARCACHE				0xf
/* Write-Back, Read and Write Allocate */
#define IOCP_AWCACHE				0xf
#define PCIE_CFG_MSTR_ARCACHE_MODE		GENMASK(6, 3)
#define PCIE_CFG_MSTR_AWCACHE_MODE		GENMASK(14, 11)
#define PCIE_CFG_MSTR_ARCACHE_VALUE		GENMASK(22, 19)
#define PCIE_CFG_MSTR_AWCACHE_VALUE		GENMASK(30, 27)

#define PCIE_GEN_CONTROL2			0x54
#define PCIE_CFG_LTSSM_EN			BIT(0)

#define PCIE_REGS_PCIE_SII_PM_STATE		0xc0
#define SMLH_LINK_UP				BIT(6)
#define RDLH_LINK_UP				BIT(7)
#define PCIE_REGS_PCIE_SII_LINK_UP		(SMLH_LINK_UP | RDLH_LINK_UP)

struct qilai_pcie {
	struct dw_pcie dw;
	struct platform_device *pdev;
	void __iomem *apb_base;
};

#define to_qilai_pcie(_dw) container_of(_dw, struct qilai_pcie, dw)

static u64 qilai_pcie_cpu_addr_fixup(struct dw_pcie *pci, u64 cpu_addr)
{
	struct dw_pcie_rp *pp = &pci->pp;

	return cpu_addr - pp->cfg0_base;
}

static u32 qilai_pcie_outbound_atu_check(struct dw_pcie *pci,
					 const struct dw_pcie_ob_atu_cfg *atu,
					 u64 *limit_addr)
{
	u64 parent_bus_addr = atu->parent_bus_addr;

	*limit_addr = parent_bus_addr + atu->size - 1;

	/*
	 * Addresses below 4 GB are not 1:1 mapped; therefore, range checks
	 * only need to ensure addresses below 4 GB match pci->region_limit.
	 */
	if (lower_32_bits(*limit_addr & ~pci->region_limit) !=
	    lower_32_bits(parent_bus_addr & ~pci->region_limit) ||
	    !IS_ALIGNED(parent_bus_addr, pci->region_align) ||
	    !IS_ALIGNED(atu->pci_addr, pci->region_align) || !atu->size)
		return -EINVAL;
	else
		return 0;
}

static bool qilai_pcie_link_up(struct dw_pcie *pci)
{
	struct qilai_pcie *qlpci = to_qilai_pcie(pci);
	u32 val;

	/* Read smlh & rdlh link up by checking debug port */
	dw_pcie_read(qlpci->apb_base + PCIE_REGS_PCIE_SII_PM_STATE, 0x4, &val);

	return (val & PCIE_REGS_PCIE_SII_LINK_UP) == PCIE_REGS_PCIE_SII_LINK_UP;
}

static int qilai_pcie_start_link(struct dw_pcie *pci)
{
	struct qilai_pcie *qlpci = to_qilai_pcie(pci);
	u32 val;

	/* Do phy link up */
	dw_pcie_read(qlpci->apb_base + PCIE_GEN_CONTROL2, 0x4, &val);
	val |= PCIE_CFG_LTSSM_EN;
	dw_pcie_write(qlpci->apb_base + PCIE_GEN_CONTROL2, 0x4, val);

	return 0;
}

static const struct dw_pcie_ops qilai_pcie_ops = {
	.cpu_addr_fixup = qilai_pcie_cpu_addr_fixup,
	.outbound_atu_check = qilai_pcie_outbound_atu_check,
	.link_up = qilai_pcie_link_up,
	.start_link = qilai_pcie_start_link,
};

static struct qilai_pcie *qilai_pcie_create_data(struct platform_device *pdev)
{
	struct qilai_pcie *qlpci;

	qlpci = devm_kzalloc(&pdev->dev, sizeof(*qlpci), GFP_KERNEL);
	if (!qlpci)
		return ERR_PTR(-ENOMEM);

	qlpci->pdev = pdev;
	platform_set_drvdata(pdev, qlpci);

	return qlpci;
}

/*
 * Setup the Qilai PCIe IOCP (IO Coherence Port) Read/Write Behaviors to the
 * Write-Back, Read and Write Allocate mode.
 */
static void qilai_pcie_iocp_cache_setup(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	u32 val;

	dw_pcie_dbi_ro_wr_en(pci);

	dw_pcie_read(pci->dbi_base + PCIE_LOGIC_COHERENCY_CONTROL3,
		     sizeof(val), &val);
	val |= FIELD_PREP(PCIE_CFG_MSTR_ARCACHE_MODE, IOCP_ARCACHE);
	val |= FIELD_PREP(PCIE_CFG_MSTR_AWCACHE_MODE, IOCP_AWCACHE);
	val |= FIELD_PREP(PCIE_CFG_MSTR_ARCACHE_VALUE, IOCP_ARCACHE);
	val |= FIELD_PREP(PCIE_CFG_MSTR_AWCACHE_VALUE, IOCP_AWCACHE);
	dw_pcie_write(pci->dbi_base + PCIE_LOGIC_COHERENCY_CONTROL3,
		      sizeof(val), val);

	dw_pcie_dbi_ro_wr_dis(pci);
}

static void qilai_pcie_enable_msi(struct qilai_pcie *qlpci)
{
	u32 val;

	dw_pcie_read(qlpci->apb_base + PCIE_INTR_CONTROL1,
		     sizeof(val), &val);
	val |= PCIE_MSI_CTRL_INT_EN;
	dw_pcie_write(qlpci->apb_base + PCIE_INTR_CONTROL1,
		      sizeof(val), val);
}

static int qilai_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct qilai_pcie *qlpci = to_qilai_pcie(pci);

	qilai_pcie_enable_msi(qlpci);

	return 0;
}

static const struct dw_pcie_host_ops qilai_pcie_host_ops = {
	.init = qilai_pcie_host_init,
};

static int qilai_pcie_add_port(struct qilai_pcie *qlpci)
{
	struct device *dev = &qlpci->pdev->dev;
	struct platform_device *pdev = qlpci->pdev;
	int ret;

	qlpci->dw.dev = dev;
	qlpci->dw.ops = &qilai_pcie_ops;
	qlpci->dw.pp.num_vectors = MAX_MSI_IRQS;
	qlpci->dw.pp.ops = &qilai_pcie_host_ops;

	dw_pcie_cap_set(&qlpci->dw, REQ_RES);

	qlpci->apb_base = devm_platform_ioremap_resource_byname(pdev, "apb");
	if (IS_ERR(qlpci->apb_base))
		return PTR_ERR(qlpci->apb_base);

	ret = dw_pcie_host_init(&qlpci->dw.pp);
	if (ret) {
		dev_err_probe(dev, ret, "Failed to initialize PCIe host\n");
		return ret;
	}

	qilai_pcie_iocp_cache_setup(&qlpci->dw.pp);

	return 0;
}

static int qilai_pcie_probe(struct platform_device *pdev)
{
	struct qilai_pcie *qlpci;

	qlpci = qilai_pcie_create_data(pdev);
	if (IS_ERR(qlpci))
		return PTR_ERR(qlpci);

	platform_set_drvdata(pdev, qlpci);

	return qilai_pcie_add_port(qlpci);
}

static const struct of_device_id qilai_pcie_of_match[] = {
	{ .compatible = "andestech,qilai-pcie" },
	{},
};
MODULE_DEVICE_TABLE(of, qilai_pcie_of_match);

static struct platform_driver qilai_pcie_driver = {
	.probe = qilai_pcie_probe,
	.driver = {
		.name	= "qilai-pcie",
		.of_match_table = qilai_pcie_of_match,
	},
};

module_platform_driver(qilai_pcie_driver);

MODULE_AUTHOR("Randolph Lin <randolph@andestech.com>");
MODULE_DESCRIPTION("Andes Qilai PCIe driver");
MODULE_LICENSE("GPL");
