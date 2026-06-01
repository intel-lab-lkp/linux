/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Header file for Andes ATCDMAC300 DMA controller driver
 *
 * Copyright (C) 2025 Andes Technology Corporation
 */
#ifndef ATCDMAC300_H
#define ATCDMAC300_H

#include <linux/bitfield.h>
#include <linux/dmaengine.h>
#include <linux/dmapool.h>
#include <linux/interrupt.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>

/*
 * Register Map Definitions for ATCDMAC300 DMA Controller
 *
 * These macros define the offsets and bit masks for various registers
 * within the ATCDMAC300 DMA controller.
 */

/* Global DMAC Registers */
#define REG_CFG			0x10
#define CH_NUM			GENMASK(3, 0)
#define DATA_WIDTH		GENMASK(25, 24)

#define REG_CTL			0x20
#define DMAC_RESET		BIT(0)

#define REG_CH_ABT		0x24

#define REG_INT_STA		0x30
#define TC_OFFSET		16
#define ABT_OFFSET		8
#define ERR_OFFSET		0
#define DMA_TC(val)		BIT(TC_OFFSET + (val))
#define DMA_ABT(val)		BIT(ABT_OFFSET + (val))
#define DMA_ERR(val)		BIT(ERR_OFFSET + (val))
#define DMA_TC_FIELD		GENMASK(TC_OFFSET + 7, TC_OFFSET)
#define DMA_ABT_FIELD		GENMASK(ABT_OFFSET + 7, ABT_OFFSET)
#define DMA_ERR_FIELD		GENMASK(ERR_OFFSET + 7, ERR_OFFSET)
#define DMA_INT_ALL(val)	(FIELD_GET(DMA_TC_FIELD, (val)) |	\
				 FIELD_GET(DMA_ABT_FIELD, (val)) |	\
				 FIELD_GET(DMA_ERR_FIELD, (val)))
#define DMA_INT_CLR(val)	(FIELD_PREP(DMA_TC_FIELD, (val)) |	\
				 FIELD_PREP(DMA_ABT_FIELD, (val)) |	\
				 FIELD_PREP(DMA_ERR_FIELD, (val)))

#define REG_CH_EN		0x34

#define REG_CH_OFF(ch)		((ch) * 0x20 + 0x40)
#define REG_CH_CTL_OFF		0x0
#define REG_CH_SIZE_OFF		0x4
#define REG_CH_SRC_LOW_OFF	0x8
#define REG_CH_SRC_HIGH_OFF	0xC
#define REG_CH_DST_LOW_OFF	0x10
#define REG_CH_DST_HIGH_OFF	0x14
#define REG_CH_LLP_LOW_OFF	0x18
#define REG_CH_LLP_HIGH_OFF	0x1C

#define SRC_BURST_SIZE_1	BIT(0)
#define SRC_BURST_SIZE_2	BIT(1)
#define SRC_BURST_SIZE_4	BIT(2)
#define SRC_BURST_SIZE_8	BIT(3)
#define SRC_BURST_SIZE_16	BIT(4)
#define SRC_BURST_SIZE_32	BIT(5)
#define SRC_BURST_SIZE_64	BIT(6)
#define SRC_BURST_SIZE_128	BIT(7)
#define SRC_BURST_SIZE_256	BIT(8)
#define SRC_BURST_SIZE_512	BIT(9)
#define SRC_BURST_SIZE_1024	BIT(10)
#define SRC_BURST_SIZE_MASK	GENMASK(27, 24)
#define SRC_BURST_SIZE(size)	FIELD_PREP(SRC_BURST_SIZE_MASK, size)

#define WIDTH_1_BYTE		0x0
#define WIDTH_2_BYTES		0x1
#define WIDTH_4_BYTES		0x2
#define WIDTH_8_BYTES		0x3
#define WIDTH_16_BYTES		0x4
#define WIDTH_32_BYTES		0x5
#define SRC_WIDTH_MASK		GENMASK(23, 21)
#define SRC_WIDTH(width)	FIELD_PREP(SRC_WIDTH_MASK, width)
#define SRC_WIDTH_GET(val)	FIELD_GET(SRC_WIDTH_MASK, val)
#define DST_WIDTH_MASK		GENMASK(20, 18)
#define DST_WIDTH(width)	FIELD_PREP(DST_WIDTH_MASK, width)
#define DST_WIDTH_GET(val)	FIELD_GET(DST_WIDTH_MASK, val)

/* DMA handshake mode */
#define SRC_HS			BIT(17)
#define DST_HS			BIT(16)

/* Address control */
#define SRC_ADDR_CTRL_MASK	GENMASK(15, 14)
#define SRC_ADDR_MODE_INCR	FIELD_PREP(SRC_ADDR_CTRL_MASK, 0x0)
#define SRC_ADDR_MODE_DECR	FIELD_PREP(SRC_ADDR_CTRL_MASK, 0x1)
#define SRC_ADDR_MODE_FIXED	FIELD_PREP(SRC_ADDR_CTRL_MASK, 0x2)
#define DST_ADDR_CTRL_MASK	GENMASK(13, 12)
#define DST_ADDR_MODE_INCR	FIELD_PREP(DST_ADDR_CTRL_MASK, 0x0)
#define DST_ADDR_MODE_DECR	FIELD_PREP(DST_ADDR_CTRL_MASK, 0x1)
#define DST_ADDR_MODE_FIXED	FIELD_PREP(DST_ADDR_CTRL_MASK, 0x2)

/* DMA request select */
#define SRC_REQ_SEL_MASK	GENMASK(11, 8)
#define SRC_REQ(req_num)	FIELD_PREP(SRC_REQ_SEL_MASK, (req_num))
#define DST_REQ_SEL_MASK	GENMASK(7, 4)
#define DST_REQ(req_num)	FIELD_PREP(DST_REQ_SEL_MASK, (req_num))

/* Channel abort interrupt mask */
#define INT_ABT_MASK		BIT(3)
/* Channel error interrupt mask */
#define INT_ERR_MASK		BIT(2)
/* Channel terminal count interrupt mask */
#define INT_TC_MASK		BIT(1)

/* Channel Enable */
#define CHEN			BIT(0)

#define IOCP_CACHE_DMAC0_AW	GENMASK(3, 0)
#define IOCP_CACHE_DMAC0_AR	GENMASK(7, 4)
#define IOCP_CACHE_DMAC1_AW	GENMASK(11, 8)
#define IOCP_CACHE_DMAC1_AR	GENMASK(15, 12)

#define ATCDMAC_DESC_PER_CHAN	64
#define ATCDMAC_CHAN_TIMEOUT_US	100000
/*
 * Status for bottom-half processing.
 */
enum dma_sta {
	ATCDMAC_STA_TC = 0,
	ATCDMAC_STA_ERR,
	ATCDMAC_STA_ABORT
};

/**
 * struct atcdmac_regs - Hardware DMA descriptor registers.
 *
 * @ctrl: Channel Control Register.
 * @trans_size: Transfer Size Register.
 * @src_addr_lo: Source Address Register (low 32-bit).
 * @src_addr_hi: Source Address Register (high 32-bit).
 * @dst_addr_lo: Destination Address Register (low 32-bit).
 * @dst_addr_hi: Destination Address Register (high 32-bit).
 * @ll_ptr_lo: Linked List Pointer Register (low 32-bit).
 * @ll_ptr_hi: Linked List Pointer Register (high 32-bit).
 */
struct atcdmac_regs {
	unsigned int ctrl;
	unsigned int trans_size;
	unsigned int src_addr_lo;
	unsigned int src_addr_hi;
	unsigned int dst_addr_lo;
	unsigned int dst_addr_hi;
	unsigned int ll_ptr_lo;
	unsigned int ll_ptr_hi;
};

/**
 * struct atcdmac_desc - Internal representation of a DMA descriptor.
 *
 * @regs: Hardware registers for this descriptor.
 * @txd: DMA transaction descriptor for dmaengine framework.
 * @desc_node: Node for linking descriptors in software lists.
 * @tx_list: List head for chaining multiple descriptors in a single transfer.
 * @at: Next descriptor in a cyclic transfer (for internal use).
 * @num_sg: Number of scatterlist entries this descriptor handles.
 */
struct atcdmac_desc {
	struct atcdmac_regs		regs;
	struct dma_async_tx_descriptor	txd;
	struct list_head		desc_node;
	struct list_head		tx_list;
	struct list_head		*at;
	unsigned short			num_sg;
};

/**
 * struct atcdmac_chan - Private data for each DMA channel.
 *
 * @dma_chan: Common DMA engine channel object members
 * @dma_dev: Pointer to the struct atcdmac_dmac
 * @dma_sconfig: DMA transfer config for device-to-memory or memory-to-device
 * @regmap: Regmap for the DMA channel register
 * @active_list: List of descriptors being processed by the DMA engine
 * @queue_list: List of descriptors ready to be submitted to the DMA engine
 * @free_list: List of descriptors available for reuse by the channel
 * @lock: Protects data access in atomic operations
 * @status: Transmit status info, shared between top-half and threaded IRQ
 * @descs_allocated: Number of descriptors currently allocated in the pool
 * @chan_id: Channel ID number
 * @req_num: Request number assigned to the channel
 * @chan_used: Indicates whether the DMA channel is currently in use
 * @cyclic: Indicates if the transfer operates in cyclic mode
 * @dev_chan: Indicates if the DMA channel transfers data between device
 *            and memory
 */
struct atcdmac_chan {
	struct dma_chan		dma_chan;
	struct atcdmac_dmac	*dma_dev;
	struct dma_slave_config	dma_sconfig;
	struct regmap		*regmap;

	struct list_head	active_list;
	struct list_head	queue_list;
	struct list_head	free_list;

	spinlock_t		lock;		/* protects active_list, queue_list, status */
	unsigned long		status;
	unsigned short		descs_allocated;
	unsigned char		chan_id;
	unsigned char		req_num;
	bool			chan_used;
	bool			cyclic;
	bool			dev_chan;
};

/**
 * struct atcdmac_dmac - Representation of the ATCDMAC300 DMA controller
 * @dma_device: DMA device object for integration with DMA engine framework
 * @regmap: Regmap for main DMA controller registers
 * @regmap_iocp: Regmap for IOCP registers
 * @dma_desc_pool: DMA descriptor pool for allocating and managing descriptors
 * @regs: Memory-mapped base address of the main DMA controller registers
 * @lock: Protects data access in atomic operations
 * @used_chan: Bitmask of DMA channels actively issuing DMA descriptors
 * @stop_mask: Stops the DMA channel after the current transaction completes
 * @data_width: Max data bus width supported by the DMA controller
 * @num_ch: Total number of DMA channels available in the controller
 * @chan: Array of DMA channel structures, sized by the controller's channel
 *        count
 */
struct atcdmac_dmac {
	struct dma_device	dma_device;
	struct regmap		*regmap;
	struct regmap		*regmap_iocp;
	struct dma_pool		*dma_desc_pool;
	void __iomem		*regs;
	spinlock_t		lock;		/* protects used_chan, stop_mask */
	unsigned short		used_chan;
	unsigned short		stop_mask;
	unsigned char		data_width;
	unsigned char		num_ch;
	struct atcdmac_chan	chan[] __counted_by(num_ch);
};

/**
 * struct atcdmac_slave_cfg - Pre-computed per-transfer slave DMA configuration
 *
 * @reg: Device-side register address (source for DEV_TO_MEM, destination
 *       for MEM_TO_DEV).
 * @burst_bytes: Burst size in bytes (addr_width * maxburst from
 *               dma_slave_config). Used to derive the hardware burst-count
 *               field and to select the optimal transfer width.
 * @dev_width: Device-side bus width in bytes (src_addr_width for DEV_TO_MEM,
 *             dst_addr_width for MEM_TO_DEV). Used to convert @burst_bytes
 *             into a hardware burst-count for the device side.
 * @width_src: Hardware-encoded source transfer width (output of
 *             atcdmac_map_buswidth() on the source bus width).
 * @width_dst: Hardware-encoded destination transfer width (output of
 *             atcdmac_map_buswidth() on the destination bus width).
 */
struct atcdmac_slave_cfg {
	dma_addr_t	reg;
	unsigned int	burst_bytes;
	unsigned int	dev_width;
	unsigned int	width_src;
	unsigned int	width_dst;
};

/*
 * Helper functions to convert between dmaengine and internal structures.
 */
static inline struct atcdmac_desc *
atcdmac_txd_to_dma_desc(struct dma_async_tx_descriptor *txd)
{
	return container_of(txd, struct atcdmac_desc, txd);
}

static inline struct atcdmac_chan *
atcdmac_chan_to_dmac_chan(struct dma_chan *chan)
{
	return container_of(chan, struct atcdmac_chan, dma_chan);
}

static inline struct atcdmac_dmac *atcdmac_dev_to_dmac(struct dma_device *dev)
{
	return container_of(dev, struct atcdmac_dmac, dma_device);
}

static inline struct device *atcdmac_chan_to_dev(struct dma_chan *chan)
{
	return &chan->dev->device;
}

#endif
