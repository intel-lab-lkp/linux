/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Time Sensitive Networking (TSN) Ethernet MAC driver
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#ifndef XILINX_TSN_H
#define XILINX_TSN_H

#include <linux/bitfield.h>
#include <linux/circ_buf.h>
#include <linux/clk.h>
#include <linux/dma/xilinx_dma.h>
#include <linux/dmaengine.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_dma.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/u64_stats_sync.h>
#include <net/netdev_queues.h>

#define TSN_NUM_CLOCKS		6

#define TSN_DMA_CH_INVALID	GENMASK(7, 0)
#define TSN_DMA_MAX_TX_CH	GENMASK(3, 0)
#define TSN_MAX_TX_QUEUE	8
#define TSN_MIN_PRIORITIES	2
#define TSN_MAX_PRIORITIES	8
/* Descriptors defines for Tx and Rx DMA */
#define TX_BD_NUM_DEFAULT	64
#define RX_BD_NUM_DEFAULT	128

#define TSN_MAX_FRAME_SIZE        (ETH_DATA_LEN + ETH_HLEN + ETH_FCS_LEN)
#define TSN_MAX_VLAN_FRAME_SIZE   (ETH_DATA_LEN + VLAN_ETH_HLEN + ETH_FCS_LEN)

/* TUSER Input Port ID field definitions (bits [5:4]) */
#define TSN_TUSER_PORT_ID_MASK		GENMASK(5, 4)
#define TSN_TUSER_PORT_EP		0x0  /* Endpoint Port */
#define TSN_TUSER_PORT_MAC1		0x1  /* MAC-1 Port */
#define TSN_TUSER_PORT_MAC2		0x2  /* MAC-2 Port */

/* TSN MAC Registers */
#define TSN_RAF_OFFSET		0x00000000 /* Reset and Address filter */
#define TSN_STATS_OFFSET	0x00000200 /* Statistics counters */
#define TSN_RCW0_OFFSET		0x00000400 /* Rx Configuration Word 0 */
#define TSN_RCW1_OFFSET		0x00000404 /* Rx Configuration Word 1 */
#define TSN_TC_OFFSET		0x00000408 /* Tx Configuration */
#define TSN_FCC_OFFSET		0x0000040C /* Flow Control Configuration */
#define TSN_EMMC_OFFSET		0x00000410 /* MAC speed configuration */
#define TSN_PHYC_OFFSET		0x00000414 /* RX Max Frame Configuration */
#define TSN_ID_OFFSET		0x000004F8 /* Identification register */
#define TSN_ABILITY_OFFSET	0x000004FC /* Ability Register offset */
#define TSN_MDIO_MC_OFFSET	0x00000500 /* MDIO Setup */
#define TSN_MDIO_MCR_OFFSET	0x00000504 /* MDIO Control */
#define TSN_MDIO_MWD_OFFSET	0x00000508 /* MDIO Write Data */
#define TSN_MDIO_MRD_OFFSET	0x0000050C /* MDIO Read Data */

/* Bit masks for TSN Ethernet MDIO interface MC register */
#define TSN_MDIO_MC_MDIOEN		BIT(6)	   /* MII management enable */
#define TSN_MDIO_MC_CLOCK_DIVIDE_MAX	0x3F	   /* Maximum MDIO divisor */

/* Bit masks for TSN Ethernet MDIO interface MCR register */
#define TSN_MDIO_MCR_PHYAD_SHIFT	24         /* Phy Address Shift */
#define TSN_MDIO_MCR_PHYAD_MASK		GENMASK(28, 24) /* Phy Address Mask */
#define TSN_MDIO_MCR_REGAD_SHIFT	16         /* Reg Address Shift */
#define TSN_MDIO_MCR_REGAD_MASK		GENMASK(20, 16) /* Reg Address Mask */
#define TSN_MDIO_MCR_OP_SHIFT		14         /* Operation Code Shift */
#define TSN_MDIO_MCR_OP_MASK		GENMASK(15, 14) /* Operation Code Mask */
#define TSN_MDIO_MCR_OP_READ		BIT(15)    /* Op Code Read */
#define TSN_MDIO_MCR_OP_WRITE		BIT(14)    /* Op Code Write */
#define TSN_MDIO_MCR_INITIATE		BIT(11)    /* Initiate MDIO transaction */
#define TSN_MDIO_MCR_READY		BIT(7)     /* MDIO Ready */

/* Bit masks for TSN Ethernet MDIO Write Data Register */
#define TSN_MDIO_MWD_SHIFT		0          /* Write Data Shift */
#define TSN_MDIO_MWD_MASK		GENMASK(15, 0) /* Write Data Mask */

/* Bit masks for TSN Ethernet MDIO Read Data Register */
#define TSN_MDIO_MRD_SHIFT		0          /* Read Data Shift */
#define TSN_MDIO_MRD_MASK		GENMASK(15, 0) /* Read Data Mask */

/* Bit masks for Ethernet UAW1 register */
/* Station address bits [47:32]; Station address
 * bits [31:0] are stored in register UAW0
 */
#define TSN_UAW1_UNICASTADDR_MASK	GENMASK(15, 0)

/* Bit masks for TSN Ethernet EMMC register */
#define TSN_EMMC_LINKSPEED_SHIFT	30	   /* Link speed shift */
#define TSN_EMMC_LINKSPEED_MASK		GENMASK(31, 30) /* Link speed mask */
#define TSN_EMMC_LINKSPEED_10		0x0	   /* 10 Mbit */
#define TSN_EMMC_LINKSPEED_100		BIT(30)    /* 100 Mbit */
#define TSN_EMMC_LINKSPEED_1000		BIT(31)    /* 1000 Mbit */

#define TSN_MAX_EMAC_NO			2

/*
 * struct tsn_emac - TSN Ethernet MAC configuration structure
 * @ndev: Network device associated with this EMAC instance
 * @common: Pointer to the main TSN private data structure
 * @phy_node: Device tree node for the connected PHY device
 * @phy_mode: PHY interface mode (RGMII, SGMII, etc.)
 * @phy_flags: PHY-specific configuration flags
 * @regs: Virtual address mapping of EMAC register space
 * @regs_start: Physical start address of EMAC register space
 * @mii_bus: MDIO bus controller for PHY management
 * @last_link: Previous link state for change detection
 * @mii_clk_div: MDIO clock divider value
 * @emac_num: EMAC instance number (1 or 2)
 * @irq: Interrupt number for this EMAC
 */
struct tsn_emac {
	struct net_device *ndev;
	struct tsn_priv *common;
	struct device_node *phy_node;
	phy_interface_t phy_mode;
	u32 phy_flags;
	void __iomem *regs;
	resource_size_t regs_start;
	struct mii_bus *mii_bus;
	u32 last_link;
	u8 mii_clk_div;
	int emac_num;
	int irq;
};

/*
 * struct skbuf_dma_descriptor - skb for each dma descriptor
 * @sgl: Pointer for sglist.
 * @desc: Pointer to dma descriptor.
 * @dma_address: dma address of sglist.
 * @skb: Pointer to SKB transferred using DMA
 * @sg_len: number of entries in the sglist.
 */
struct skbuf_dma_descriptor {
	struct scatterlist sgl[MAX_SKB_FRAGS + 1];
	struct dma_async_tx_descriptor *desc;
	dma_addr_t dma_address;
	struct sk_buff *skb;
	int sg_len;
};

/**
 * struct tsn_dma_chan - TSN DMA channel management structure
 * @skb_ring: Ring buffer of SKB DMA descriptors
 * @common: Pointer to common TSN private data
 * @chan: DMA engine channel handle
 * @ring_head: Head index of the ring buffer
 * @ring_tail: Tail index of the ring buffer
 * @is_tx: Flag indicating if this is a TX channel (true) or RX (false)
 */
struct tsn_dma_chan {
	struct skbuf_dma_descriptor **skb_ring;
	struct tsn_priv *common;
	struct dma_chan *chan;
	int ring_head;
	int ring_tail;
	bool is_tx;
};

/*
 * struct tsn_endpoint - TSN endpoint configuration structure
 * @ndev: Network device associated with the TSN endpoint
 * @regs: Virtual address mapping of endpoint register space
 * @common: Pointer to the main TSN private data structure
 */
struct tsn_endpoint {
	struct net_device *ndev;
	void __iomem *regs;
	struct tsn_priv *common;
};

/**
 * struct tsn_priv - Main TSN private data structure
 * @pdev: Platform device handle
 * @dev: Device pointer for this TSN instance
 * @res: Platform resource information
 * @regs_start: Start address (physical) of mapped region
 * @regs: ioremap()'d base pointer
 * @ep: Pointer to TSN endpoint structure
 * @emacs: Array of EMAC instances (up to 2)
 * @clks: Bulk clock data for all required clocks
 * @tx_lock: Spinlock protecting TX rings and related TX state
 * @rx_lock: Spinlock protecting RX rings and related RX state
 * @mdio_lock: Mutex placeholder for future MDIO serialization
 * @num_priorities: Number of priority queues configured
 * @num_tx_queues: Number of TX DMA queues
 * @num_rx_queues: Number of RX DMA queues
 * @tx_dma_chan_map: Logical TX queue index to DMA channel number mapping.
 * @max_frm_size: Maximum frame size supported
 * @tx_chans: Array of TX DMA channels
 * @rx_chans: Array of RX DMA channels
 * @num_emacs: Number of EMAC instances
 */
struct tsn_priv {
	struct platform_device *pdev;
	struct device *dev;
	struct resource *res;
	resource_size_t regs_start;
	void __iomem *regs;
	struct tsn_endpoint *ep;
	struct tsn_emac *emacs[TSN_MAX_EMAC_NO];
	struct clk_bulk_data clks[TSN_NUM_CLOCKS];
	spinlock_t tx_lock;	/* Protects TX ring buffers */
	spinlock_t rx_lock;	/* Protects RX ring buffers */
	struct mutex mdio_lock; /* Serializes MDIO access across all EMACs */
	u32 num_priorities;
	u32 num_tx_queues;
	u32 num_rx_queues;
	u32 tx_dma_chan_map[TSN_MAX_TX_QUEUE];
	u32 max_frm_size;
	struct tsn_dma_chan **tx_chans;
	struct tsn_dma_chan **rx_chans;
	u32 num_emacs;
};

/**
 * tsn_ndo_set_mac_address - ndo_set_mac_address handler for TSN devices
 * @ndev: Pointer to the net_device structure
 * @p: Pointer to sockaddr structure containing MAC address
 *
 * This is the common implementation for net_device_ops.ndo_set_mac_address
 * callback used by both EP and EMAC interfaces.
 *
 * Validates the provided MAC address and sets it only if valid.
 *
 * Return: 0 on success, -EADDRNOTAVAIL if address is invalid
 */
static inline int tsn_ndo_set_mac_address(struct net_device *ndev, void *p)
{
	struct sockaddr *addr = p;

	/* Validate address before setting */
	if (!addr || !is_valid_ether_addr(addr->sa_data)) {
		netdev_err(ndev, "Invalid MAC address provided\n");
		return -EADDRNOTAVAIL;
	}

	eth_hw_addr_set(ndev, addr->sa_data);

	return 0;
}

netdev_tx_t tsn_start_xmit_dmaengine(struct tsn_priv *common,
				     struct sk_buff *skb,
				     struct net_device *ndev);

/**
 * emac_iow - Memory mapped TSN EMAC register write
 * @emac: Pointer to TSN EMAC structure
 * @off: Address offset from the base address of EMAC registers
 * @val: Value to be written into the EMAC register
 *
 * This function writes the desired value into the corresponding TSN
 * EMAC register.
 */
static inline void emac_iow(struct tsn_emac *emac, off_t off, u32 val)
{
	iowrite32(val, emac->regs + off);
}

/**
 * emac_ior - Memory mapped TSN EMAC register read
 * @emac: Pointer to TSN EMAC structure
 * @off: Address offset from the base address of EMAC registers
 *
 * This function reads a value from the corresponding TSN EMAC
 * register.
 *
 * Return: Value read from the EMAC register
 */
static inline u32 emac_ior(struct tsn_emac *emac, u32 off)
{
	return ioread32(emac->regs + off);
}

int tsn_ep_init(struct platform_device *pdev);
void tsn_ep_exit(struct platform_device *pdev);
int tsn_emac_init(struct platform_device *pdev);
void tsn_emac_exit(struct platform_device *pdev);
int tsn_mdio_setup(struct tsn_emac *emac, struct device_node *mac_np);
void tsn_mdio_teardown(struct tsn_emac *emac);
#endif /* XILINX_TSN_H */
