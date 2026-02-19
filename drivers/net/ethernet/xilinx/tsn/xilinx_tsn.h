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
 */
struct tsn_priv {
	struct platform_device *pdev;
	struct device *dev;
	struct resource *res;
	resource_size_t regs_start;
	void __iomem *regs;
	struct tsn_endpoint *ep;
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
int tsn_ep_init(struct platform_device *pdev);
void tsn_ep_exit(struct platform_device *pdev);
#endif /* XILINX_TSN_H */
