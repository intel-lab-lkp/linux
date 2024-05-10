// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Broadcom Corporation
 * This file contains the functions to handle the Broadcom XGMAC PCI driver.
 *
 */

#include <linux/clk-provider.h>
#include <linux/dmi.h>
#include <linux/pci.h>
#include <linux/phy.h>
#include <linux/phy_fixed.h>

#include "stmmac.h"
#include "dwxgmac2.h"

#define PCI_DEVICE_ID_BROADCOM_BCM8958X		0xa00d
#define BRCM_MAX_MTU				1500
#define READ_POLL_DELAY_US			100
#define READ_POLL_TIMEOUT_US			10000
#define DWMAC_125MHZ				125000000
#define DWMAC_250MHZ				250000000

/* TX and RX Queue couts */
#define BRCM_TX_Q_COUNT				4
#define BRCM_RX_Q_COUNT				1

/* PDMA Channel counts */
#define PDMA_TX_CH_COUNT			8
#define PDMA_RX_CH_COUNT			10

/* PDMA register type */
#define PDMA_CH_TX_EXT_CFGR			0
#define PDMA_CH_RX_EXT_CFGR			1
#define PDMA_CH_TX_DBG_STSR			2
#define PDMA_CH_RX_DBG_STSR			3

/* VDMA register type */
#define VDMA_CH_TX_DESC_CTRLR			4
#define VDMA_CH_RX_DESC_CTRLR			5

/* VDMA channel count */
#define VDMA_TOTAL_CH_COUNT			32

#define DMA_CH_IND_CTRLR			0x3080
#define DMA_CH_IND_DATAR			0x3084

#define BRCM_XGMAC_RX_CFG			0x2000
#define BRCM_XGMAC_RXQ_CTRL1_CFG		0x8000

#define BRCM_XGMAC_DMA_TX_SIZE			4096
#define BRCM_XGMAC_DMA_RX_SIZE			4096
#define BRCM_XGMAC_BAR0_MASK			BIT(0)

#define BRCM_XGMAC_IOMEM_MISC_REG_OFFSET	0x0
#define BRCM_XGMAC_IOMEM_MBOX_REG_OFFSET	0x1000
#define BRCM_XGMAC_IOMEM_CFG_REG_OFFSET		0x3000

#define XGMAC_PCIE_CFG_MSIX_ADDR_MATCH_LOW	0x940
#define XGMAC_PCIE_CFG_MSIX_ADDR_MATCH_LO_VALUE	0x00000001
#define XGMAC_PCIE_CFG_MSIX_ADDR_MATCH_HIGH	0x944
#define XGMAC_PCIE_CFG_MSIX_ADDR_MATCH_HI_VALUE	0x88000000

#define XGMAC_PCIE_MISC_MII_CTRL			0x4
#define XGMAC_PCIE_MISC_MII_CTRL_VALUE			0x7
#define XGMAC_PCIE_MISC_PCIESS_CTRL			0x8
#define XGMAC_PCIE_MISC_PCIESS_CTRL_VALUE		0x200
#define XGMAC_PCIE_MISC_MSIX_ADDR_MATCH_LO		0x90
#define XGMAC_PCIE_MISC_MSIX_ADDR_MATCH_LO_VALUE	0x00000001
#define XGMAC_PCIE_MISC_MSIX_ADDR_MATCH_HI		0x94
#define XGMAC_PCIE_MISC_MSIX_ADDR_MATCH_HI_VALUE	0x88000000
#define XGMAC_PCIE_MISC_MSIX_VECTOR_MAP_EP2HOST0	0x700
#define XGMAC_PCIE_MISC_MSIX_VECTOR_MAP_EP2HOST0_VALUE	1
#define XGMAC_PCIE_MISC_MSIX_VECTOR_MAP_EP2HOST1	0x704
#define XGMAC_PCIE_MISC_MSIX_VECTOR_MAP_EP2HOST1_VALUE	1
#define XGMAC_PCIE_MISC_MSIX_VECTOR_MAP_EP2HOST_DBELL	0x728
#define XGMAC_PCIE_MISC_MSIX_VECTOR_MAP_EP2HOST_DBELL_VALUE	1
#define XGMAC_PCIE_MISC_MSIX_VECTOR_MAP_SBD_ALL		0x740
#define XGMAC_PCIE_MISC_MSIX_VECTOR_MAP_SBD_ALL_VALUE	0

#define DMA_CH_IND_CTRLR_MSEL_OFF		24
#define DMA_CH_IND_CTRLR_MSEL_MASK		GENMASK(27, 24)
#define DMA_CH_IND_CTRLR_AOFF_OFF		8
#define DMA_CH_IND_CTRLR_AOFF_MASK		GENMASK(14, 8)
#define DMA_CH_IND_CTRLR_AUTO_OFF		4
#define DMA_CH_IND_CTRLR_AUTO_MASK		GENMASK(5, 4)
#define DMA_CH_IND_CTRLR_CT_OFF			1
#define DMA_CH_IND_CTRLR_CT_MASK		BIT(1)
#define DMA_CH_IND_CTRLR_OB_OFF			0
#define DMA_CH_IND_CTRLR_OB_MASK		BIT(0)

/* DMA Descriptor configuration */
#define BRCM_PDMA_DESC_CTRL_CFG_VALUE		0x1B

#define XGMAC_PCIE_MISC_FUNC_RESOURCES_PF0	0x804

/* MSIX Vector map register starting offsets */
#define XGMAC_PCIE_MISC_MSIX_VECTOR_MAP_RX0_PF0	0x840
#define XGMAC_PCIE_MISC_MSIX_VECTOR_MAP_TX0_PF0	0x890
#define BRCM_MAX_DMA_CHANNEL_PAIRS		4

#define BRCM_XGMAC_MSI_MAC_VECTOR		0
#define BRCM_XGMAC_MSI_RX_VECTOR_START		9
#define BRCM_XGMAC_MSI_TX_VECTOR_START		10

static int num_instances;

struct brcm_priv_data {
	struct phy_device *phy_dev;
	void __iomem *mbox_regs;    /* MBOX */
	void __iomem *misc_regs;    /* MISC_cfg */
	u16	dev_id;
	u16	phy_addr;
};

static struct fixed_phy_status dwxgmac_brcm_fixed_phy_status = {
	.link	= 1,
	.speed	= SPEED_1000,
	.duplex	= DUPLEX_FULL,
};

struct dwxgmac_brcm_pci_info {
	int (*setup)(struct pci_dev *pdev, struct plat_stmmacenet_data *plat);
};

static inline void misc_iowrite(struct brcm_priv_data *brcm_priv,
				u32 reg, u32 val)
{
	iowrite32(val, brcm_priv->misc_regs + reg);
}

static void dwxgmac_brcm_pdma_set(void __iomem *ioaddr, u32 type, u32 chan,
				  u32 val)
{
	u32 var = 0;

	var |= FIELD_PREP(DMA_CH_IND_CTRLR_MSEL_MASK, type);
	var |= FIELD_PREP(DMA_CH_IND_CTRLR_AOFF_MASK, chan);
	var |= FIELD_PREP(DMA_CH_IND_CTRLR_CT_MASK, 0);
	var |= FIELD_PREP(DMA_CH_IND_CTRLR_OB_MASK, 1);

	if (!FIELD_GET(DMA_CH_IND_CTRLR_OB_MASK,
		       readl(ioaddr + DMA_CH_IND_CTRLR))) {
		writel(0x0, (ioaddr + DMA_CH_IND_CTRLR));
		writel(val, (ioaddr + DMA_CH_IND_DATAR));
	}

	writel(var, (ioaddr + DMA_CH_IND_CTRLR));
	readl_poll_timeout(ioaddr + DMA_CH_IND_CTRLR, var,
			   !(var & XGMAC_OB), READ_POLL_TIMEOUT_US,
			   READ_POLL_TIMEOUT_US);
}

static void dwxgmac_brcm_dma_init(void __iomem *ioaddr,
				  struct stmmac_dma_cfg *dma_cfg, int atds)
{
	u32 val = dma_cfg->pbl << 24;
	u32 i;

	if (dma_cfg->pblx8)
		val |= (1 << 19);

	dwxgmac2_dma_init(ioaddr, dma_cfg, atds);

	for (i = 0; i < PDMA_TX_CH_COUNT; i++)
		dwxgmac_brcm_pdma_set(ioaddr, PDMA_CH_TX_EXT_CFGR, i, val);

	for (i = 0; i < PDMA_RX_CH_COUNT; i++)
		dwxgmac_brcm_pdma_set(ioaddr, PDMA_CH_RX_EXT_CFGR, i, val);

	for (i = 0; i < VDMA_TOTAL_CH_COUNT; i++) {
		dwxgmac_brcm_pdma_set(ioaddr, VDMA_CH_TX_DESC_CTRLR, i,
				      BRCM_PDMA_DESC_CTRL_CFG_VALUE);
		dwxgmac_brcm_pdma_set(ioaddr, VDMA_CH_RX_DESC_CTRLR, i,
				      BRCM_PDMA_DESC_CTRL_CFG_VALUE);
	}
}

static void dwxgmac_brcm_dma_init_tx_chan(struct stmmac_priv *priv,
					  void __iomem *ioaddr,
					  struct stmmac_dma_cfg *dma_cfg,
					  dma_addr_t phy, u32 chan)
{
	u32 value;

	value = readl(ioaddr + XGMAC_DMA_CH_TX_CONTROL(chan));
	value &= ~XGMAC_TxPBL;
	value &= ~GENMASK(6, 4);
	writel(value, ioaddr + XGMAC_DMA_CH_TX_CONTROL(chan));

	writel(upper_32_bits(phy), ioaddr + XGMAC_DMA_CH_TxDESC_HADDR(chan));
	writel(lower_32_bits(phy), ioaddr + XGMAC_DMA_CH_TxDESC_LADDR(chan));
}

static void dwxgmac_brcm_dma_init_rx_chan(struct stmmac_priv *priv,
					  void __iomem *ioaddr,
					  struct stmmac_dma_cfg *dma_cfg,
					  dma_addr_t phy, u32 chan)
{
	u32 value;

	value = readl(ioaddr + XGMAC_DMA_CH_RX_CONTROL(chan));
	value &= ~XGMAC_RxPBL;
	writel(value, ioaddr + XGMAC_DMA_CH_RX_CONTROL(chan));

	writel(upper_32_bits(phy), ioaddr + XGMAC_DMA_CH_RxDESC_HADDR(chan));
	writel(lower_32_bits(phy), ioaddr + XGMAC_DMA_CH_RxDESC_LADDR(chan));
}

const struct stmmac_dma_ops dwxgmac_brcm_dma_ops = {
	.reset = dwxgmac2_dma_reset,
	.init = dwxgmac_brcm_dma_init,
	.init_chan = dwxgmac2_dma_init_chan,
	.init_rx_chan = dwxgmac_brcm_dma_init_rx_chan,
	.init_tx_chan = dwxgmac_brcm_dma_init_tx_chan,
	.axi = dwxgmac2_dma_axi,
	.dump_regs = dwxgmac2_dma_dump_regs,
	.dma_rx_mode = dwxgmac2_dma_rx_mode,
	.dma_tx_mode = dwxgmac2_dma_tx_mode,
	.enable_dma_irq = dwxgmac2_enable_dma_irq,
	.disable_dma_irq = dwxgmac2_disable_dma_irq,
	.start_tx = dwxgmac2_dma_start_tx,
	.stop_tx = dwxgmac2_dma_stop_tx,
	.start_rx = dwxgmac2_dma_start_rx,
	.stop_rx = dwxgmac2_dma_stop_rx,
	.dma_interrupt = dwxgmac2_dma_interrupt,
	.get_hw_feature = dwxgmac2_get_hw_feature,
	.rx_watchdog = dwxgmac2_rx_watchdog,
	.set_rx_ring_len = dwxgmac2_set_rx_ring_len,
	.set_tx_ring_len = dwxgmac2_set_tx_ring_len,
	.set_rx_tail_ptr = dwxgmac2_set_rx_tail_ptr,
	.set_tx_tail_ptr = dwxgmac2_set_tx_tail_ptr,
	.enable_tso = dwxgmac2_enable_tso,
	.qmode = dwxgmac2_qmode,
	.set_bfsize = dwxgmac2_set_bfsize,
	.enable_sph = dwxgmac2_enable_sph,
	.enable_tbs = dwxgmac2_enable_tbs,
};

static void dwxgmac_brcm_fix_speed(void *priv, unsigned int speed,
				   unsigned int mode)
{
}

static struct mac_device_info *dwxgmac_brcm_setup(void *ppriv)
{
	struct mac_device_info *mac;
	struct stmmac_priv *priv = ppriv;

	mac = devm_kzalloc(priv->device, sizeof(*mac), GFP_KERNEL);
	if (!mac)
		return NULL;

	mac->dma = &dwxgmac_brcm_dma_ops;

	priv->dma_conf.dma_tx_size = BRCM_XGMAC_DMA_TX_SIZE;
	priv->dma_conf.dma_rx_size = BRCM_XGMAC_DMA_RX_SIZE;
	priv->plat->rss_en = 1;
	mac->pcsr = priv->ioaddr;
	priv->dev->priv_flags |= IFF_UNICAST_FLT;
	mac->multicast_filter_bins = priv->plat->multicast_filter_bins;
	mac->unicast_filter_entries = priv->plat->unicast_filter_entries;
	mac->mcast_bits_log2 = 0;

	if (mac->multicast_filter_bins)
		mac->mcast_bits_log2 = ilog2(mac->multicast_filter_bins);

	mac->link.duplex = 0;
	mac->link.xgmii.speed10000 = XGMAC_CONFIG_SS_10000;
	mac->link.speed_mask = XGMAC_CONFIG_SS_MASK;
	return mac;
}

static void dwxgmac_brcm_common_default_data(struct plat_stmmacenet_data *plat)
{
	int i;

	plat->has_xgmac = 1;
	plat->force_sf_dma_mode = 1;
	plat->mac_port_sel_speed = SPEED_10000;
	plat->clk_ptp_rate = DWMAC_125MHZ;
	plat->clk_ref_rate = DWMAC_250MHZ;
	plat->setup = dwxgmac_brcm_setup;
	plat->tx_coe = 1;
	plat->rx_coe = 1;
	plat->max_speed = SPEED_10000;
	plat->fix_mac_speed = dwxgmac_brcm_fix_speed;

	/* Set default value for multicast hash bins */
	plat->multicast_filter_bins = HASH_TABLE_SIZE;

	/* Set default value for unicast filter entries */
	plat->unicast_filter_entries = 1;

	/* Set the maxmtu to device's default */
	plat->maxmtu = BRCM_MAX_MTU;

	/* Set default number of RX and TX queues to use */
	plat->tx_queues_to_use = BRCM_TX_Q_COUNT;
	plat->rx_queues_to_use = BRCM_RX_Q_COUNT;

	plat->tx_sched_algorithm = MTL_TX_ALGORITHM_SP;
	for (i = 0; i < plat->tx_queues_to_use; i++) {
		plat->tx_queues_cfg[i].use_prio = false;
		plat->tx_queues_cfg[i].prio = 0;
		plat->tx_queues_cfg[i].mode_to_use = MTL_QUEUE_DCB;
	}

	plat->rx_sched_algorithm = MTL_RX_ALGORITHM_SP;
	for (i = 0; i < plat->rx_queues_to_use; i++) {
		plat->rx_queues_cfg[i].use_prio = false;
		plat->rx_queues_cfg[i].mode_to_use = MTL_QUEUE_DCB;
		plat->rx_queues_cfg[i].pkt_route = 0x0;
		plat->rx_queues_cfg[i].chan = i;
	}
}

static int dwxgmac_brcm_default_data(struct pci_dev *pdev,
				     struct plat_stmmacenet_data *plat)
{
	struct brcm_priv_data *brcm_priv = plat->bsp_priv;

	/* Set common default data first */
	dwxgmac_brcm_common_default_data(plat);

	plat->bus_id = 0;
	plat->phy_addr = num_instances++;
	brcm_priv->phy_addr = num_instances;
	plat->phy_interface = PHY_INTERFACE_MODE_USXGMII;

	plat->dma_cfg->pbl = 32;
	plat->dma_cfg->pblx8 = 0;
	plat->dma_cfg->aal = 0;
	plat->dma_cfg->eame = 1;

	plat->axi->axi_wr_osr_lmt = 31;
	plat->axi->axi_rd_osr_lmt = 31;
	plat->axi->axi_fb = 0;
	plat->axi->axi_blen[0] = 4;
	plat->axi->axi_blen[1] = 8;
	plat->axi->axi_blen[2] = 16;
	plat->axi->axi_blen[3] = 32;
	plat->axi->axi_blen[4] = 64;
	plat->axi->axi_blen[5] = 128;
	plat->axi->axi_blen[6] = 256;

	plat->msi_mac_vec = BRCM_XGMAC_MSI_MAC_VECTOR;
	plat->msi_rx_base_vec = BRCM_XGMAC_MSI_RX_VECTOR_START;
	plat->msi_tx_base_vec = BRCM_XGMAC_MSI_TX_VECTOR_START;

	return 0;
}

static struct dwxgmac_brcm_pci_info dwxgmac_brcm_pci_info = {
	.setup = dwxgmac_brcm_default_data,
};

static int brcm_config_multi_msi(struct pci_dev *pdev,
				 struct plat_stmmacenet_data *plat,
				 struct stmmac_resources *res)
{
	int ret;
	int i;

	if (plat->msi_rx_base_vec >= STMMAC_MSI_VEC_MAX ||
	    plat->msi_tx_base_vec >= STMMAC_MSI_VEC_MAX) {
		dev_err(&pdev->dev, "%s: Invalid RX & TX vector defined\n",
			__func__);
		return -EINVAL;
	}

	ret = pci_alloc_irq_vectors(pdev, 2, STMMAC_MSI_VEC_MAX,
				    PCI_IRQ_MSI | PCI_IRQ_MSIX);
	if (ret < 0) {
		dev_err(&pdev->dev, "%s: multi MSI enablement failed\n",
			__func__);
		return ret;
	}

	/* For RX MSI */
	for (i = 0; i < plat->rx_queues_to_use; i++)
		res->rx_irq[i] = pci_irq_vector(pdev,
						plat->msi_rx_base_vec + i * 2);

	/* For TX MSI */
	for (i = 0; i < plat->tx_queues_to_use; i++)
		res->tx_irq[i] = pci_irq_vector(pdev,
						plat->msi_tx_base_vec + i * 2);

	if (plat->msi_mac_vec < STMMAC_MSI_VEC_MAX)
		res->irq = pci_irq_vector(pdev, plat->msi_mac_vec);

	plat->flags |= STMMAC_FLAG_MULTI_MSI_EN;
	plat->flags |= STMMAC_FLAG_TSO_EN;

	return 0;
}

/**
 * dwxgmac_brcm_pci_probe
 *
 * @pdev: pci device pointer
 * @id: pointer to table of device id/id's.
 *
 * Description: This probing function gets called for all PCI devices which
 * match the ID table and are not "owned" by other driver yet. This function
 * gets passed a "struct pci_dev *" for each device whose entry in the ID table
 * matches the device. The probe functions returns zero when the driver choose
 * to take "ownership" of the device or an error code(-ve no) otherwise.
 */
static int dwxgmac_brcm_pci_probe(struct pci_dev *pdev,
				  const struct pci_device_id *id)
{
	struct dwxgmac_brcm_pci_info *info =
		(struct dwxgmac_brcm_pci_info *)id->driver_data;
	struct plat_stmmacenet_data *plat;
	struct brcm_priv_data *brcm_priv;
	struct stmmac_resources res;
	struct net_device *ndev;
	struct stmmac_priv *priv;
	int rx_offset;
	int tx_offset;
	int vector;
	int ret;

	brcm_priv = devm_kzalloc(&pdev->dev, sizeof(*brcm_priv), GFP_KERNEL);
	if (!brcm_priv)
		return -ENOMEM;

	plat = devm_kzalloc(&pdev->dev, sizeof(*plat), GFP_KERNEL);
	if (!plat)
		return -ENOMEM;

	plat->dma_cfg = devm_kzalloc(&pdev->dev, sizeof(*plat->dma_cfg),
				     GFP_KERNEL);
	if (!plat->dma_cfg)
		return -ENOMEM;

	plat->axi = devm_kzalloc(&pdev->dev, sizeof(*plat->axi), GFP_KERNEL);
	if (!plat->axi)
		return -ENOMEM;

	pci_read_config_word(pdev, 2, &brcm_priv->dev_id);

	/* This device interface is directly attached to the switch chip on
	 *  the SoC. Since no MDIO is present, register fixed_phy.
	 */
	brcm_priv->phy_dev =
		 fixed_phy_register(PHY_POLL,
				    &dwxgmac_brcm_fixed_phy_status, NULL);
	if (IS_ERR(brcm_priv->phy_dev)) {
		dev_err(&pdev->dev, "%s\tNo PHY/fixed_PHY found\n", __func__);
		return -ENODEV;
	}
	phy_attached_info(brcm_priv->phy_dev);

	/* Disable D3COLD as our device does not support it */
	pci_d3cold_disable(pdev);

	/* Enable PCI device */
	ret = pcim_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "%s: ERROR: failed to enable device\n",
			__func__);
		return ret;
	}

	/* Get the base address of device */
	ret = pcim_iomap_regions(pdev, BRCM_XGMAC_BAR0_MASK, pci_name(pdev));
	if (ret)
		goto err_disable_device;
	pci_set_master(pdev);

	memset(&res, 0, sizeof(res));
	res.addr = pcim_iomap_table(pdev)[0];
	/* MISC Regs */
	brcm_priv->misc_regs = res.addr + BRCM_XGMAC_IOMEM_MISC_REG_OFFSET;
	/* MBOX Regs */
	brcm_priv->mbox_regs = res.addr + BRCM_XGMAC_IOMEM_MBOX_REG_OFFSET;
	/* XGMAC config Regs */
	res.addr += BRCM_XGMAC_IOMEM_CFG_REG_OFFSET;

	plat->bsp_priv = brcm_priv;

	/* Initialize all MSI vectors to invalid so that it can be set
	 * according to platform data settings below.
	 * Note: MSI vector takes value from 0 up to 31 (STMMAC_MSI_VEC_MAX)
	 */
	plat->msi_mac_vec = STMMAC_MSI_VEC_MAX;
	plat->msi_wol_vec = STMMAC_MSI_VEC_MAX;
	plat->msi_lpi_vec = STMMAC_MSI_VEC_MAX;
	plat->msi_sfty_ce_vec = STMMAC_MSI_VEC_MAX;
	plat->msi_sfty_ue_vec = STMMAC_MSI_VEC_MAX;
	plat->msi_rx_base_vec = STMMAC_MSI_VEC_MAX;
	plat->msi_tx_base_vec = STMMAC_MSI_VEC_MAX;

	ret = info->setup(pdev, plat);
	if (ret)
		goto err_disable_device;

	pci_write_config_dword(pdev, XGMAC_PCIE_CFG_MSIX_ADDR_MATCH_LOW,
			       XGMAC_PCIE_CFG_MSIX_ADDR_MATCH_LO_VALUE);
	pci_write_config_dword(pdev, XGMAC_PCIE_CFG_MSIX_ADDR_MATCH_HIGH,
			       XGMAC_PCIE_CFG_MSIX_ADDR_MATCH_HI_VALUE);

	misc_iowrite(brcm_priv, XGMAC_PCIE_MISC_MSIX_ADDR_MATCH_LO,
		     XGMAC_PCIE_MISC_MSIX_ADDR_MATCH_LO_VALUE);
	misc_iowrite(brcm_priv, XGMAC_PCIE_MISC_MSIX_ADDR_MATCH_HI,
		     XGMAC_PCIE_MISC_MSIX_ADDR_MATCH_HI_VALUE);

	/* SBD Interrupt */
	misc_iowrite(brcm_priv, XGMAC_PCIE_MISC_MSIX_VECTOR_MAP_SBD_ALL,
		     XGMAC_PCIE_MISC_MSIX_VECTOR_MAP_SBD_ALL_VALUE);
	/* EP_DOORBELL Interrupt */
	misc_iowrite(brcm_priv, XGMAC_PCIE_MISC_MSIX_VECTOR_MAP_EP2HOST_DBELL,
		     XGMAC_PCIE_MISC_MSIX_VECTOR_MAP_EP2HOST_DBELL_VALUE);
	/* EP_H0 Interrupt */
	misc_iowrite(brcm_priv, XGMAC_PCIE_MISC_MSIX_VECTOR_MAP_EP2HOST0,
		     XGMAC_PCIE_MISC_MSIX_VECTOR_MAP_EP2HOST0_VALUE);
	/* EP_H1 Interrupt */
	misc_iowrite(brcm_priv, XGMAC_PCIE_MISC_MSIX_VECTOR_MAP_EP2HOST1,
		     XGMAC_PCIE_MISC_MSIX_VECTOR_MAP_EP2HOST1_VALUE);

	rx_offset = XGMAC_PCIE_MISC_MSIX_VECTOR_MAP_RX0_PF0;
	tx_offset = XGMAC_PCIE_MISC_MSIX_VECTOR_MAP_TX0_PF0;
	vector = BRCM_XGMAC_MSI_RX_VECTOR_START;
	for (int i = 0; i < BRCM_MAX_DMA_CHANNEL_PAIRS; i++) {
		/* RX Interrupt */
		misc_iowrite(brcm_priv, rx_offset, vector++);
		/* TX Interrupt */
		misc_iowrite(brcm_priv, tx_offset, vector++);
		rx_offset += 4;
		tx_offset += 4;
	}

	/* Enable Switch Link */
	misc_iowrite(brcm_priv, XGMAC_PCIE_MISC_MII_CTRL,
		     XGMAC_PCIE_MISC_MII_CTRL_VALUE);
	/* Enable MSI-X */
	misc_iowrite(brcm_priv, XGMAC_PCIE_MISC_PCIESS_CTRL,
		     XGMAC_PCIE_MISC_PCIESS_CTRL_VALUE);

	ret = brcm_config_multi_msi(pdev, plat, &res);
	if (ret) {
		dev_err(&pdev->dev,
			"%s: ERROR: failed to enable IRQ\n", __func__);
		return ret;
	}

	ret = stmmac_dvr_probe(&pdev->dev, plat, &res);
	if (ret)
		goto err_disable_msi;

	/* The stmmac core driver doesn't have the infrastructure to
	 * support fixed-phy mdio bus for non-platform bus drivers.
	 * Until a better solution is implemented, initialize the
	 * following entries after priv structure is populated.
	 */
	ndev = dev_get_drvdata(&pdev->dev);
	priv = netdev_priv(ndev);
	priv->mii = mdio_find_bus("fixed-0");

	ndev->hw_features &= ~NETIF_F_HW_VLAN_CTAG_RX;
	priv->hw->hw_vlan_en = false;

	dev_info(&pdev->dev, "%s\tComplete\n", __func__);

	return ret;

err_disable_msi:
	pci_disable_msi(pdev);
err_disable_device:
	pci_disable_device(pdev);
	return ret;
}

/**
 * dwxgmac_brcm_pci_remove
 *
 * @pdev: platform device pointer
 * Description: this function calls the main to free the net resources
 * and releases the PCI resources.
 */
static void dwxgmac_brcm_pci_remove(struct pci_dev *pdev)
{
	struct net_device *ndev = dev_get_drvdata(&pdev->dev);
	struct stmmac_priv *priv = netdev_priv(ndev);
	struct brcm_priv_data *brcm_priv = priv->plat->bsp_priv;
	struct phy_device *phydev = brcm_priv->phy_dev;

	priv->mii = NULL;
	stmmac_dvr_remove(&pdev->dev);
	pcim_iounmap_regions(pdev, BRCM_XGMAC_BAR0_MASK);
	pci_clear_master(pdev);
	fixed_phy_unregister(phydev);
}

static int __maybe_unused dwxgmac_brcm_pci_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	int ret;

	ret = stmmac_suspend(dev);
	if (ret)
		return ret;

	ret = pci_save_state(pdev);
	if (ret)
		return ret;

	pci_disable_device(pdev);
	pci_wake_from_d3(pdev, true);
	return 0;
}

static int __maybe_unused dwxgmac_brcm_pci_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	int ret;

	pci_restore_state(pdev);
	pci_set_power_state(pdev, PCI_D0);

	ret = pci_enable_device(pdev);
	if (ret)
		return ret;

	pci_set_master(pdev);

	return stmmac_resume(dev);
}

static SIMPLE_DEV_PM_OPS(dwxgmac_brcm_pm_ops,
			 dwxgmac_brcm_pci_suspend,
			 dwxgmac_brcm_pci_resume);

static const struct pci_device_id dwxgmac_brcm_id_table[] = {
	{ PCI_DEVICE_DATA(BROADCOM, BCM8958X, &dwxgmac_brcm_pci_info) },
	{}
};

MODULE_DEVICE_TABLE(pci, dwxgmac_brcm_id_table);

static struct pci_driver dwxgmac_brcm_pci_driver = {
	.name = "brcm-bcm8958x",
	.id_table = dwxgmac_brcm_id_table,
	.probe	= dwxgmac_brcm_pci_probe,
	.remove = dwxgmac_brcm_pci_remove,
	.driver = {
		.pm = &dwxgmac_brcm_pm_ops,
	},
};

module_pci_driver(dwxgmac_brcm_pci_driver);

MODULE_DESCRIPTION("Broadcom 10G Automotive Ethernet PCIe driver");
MODULE_LICENSE("GPL");
