// SPDX-License-Identifier: GPL-2.0+
/*
 * BCM2835 DMA engine support
 *
 * Author:      Florian Meier <florian.meier@koalo.de>
 *              Copyright 2013
 *
 * Based on
 *	OMAP DMAengine support by Russell King
 *
 *	BCM2708 DMA Driver
 *	Copyright (C) 2010 Broadcom
 *
 *	Raspberry Pi PCM I2S ALSA Driver
 *	Copyright (c) by Phil Poole 2013
 *
 *	MARVELL MMP Peripheral DMA Driver
 *	Copyright 2012 Marvell International Ltd.
 */
#include <linux/bitfield.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/spinlock.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_dma.h>

#include "virt-dma.h"

#define BCM2835_DMA_MAX_DMA_CHAN_SUPPORTED 14
#define BCM2835_DMA_CHAN_NAME_SIZE 8

/**
 * struct bcm2835_dmadev - BCM2835 DMA controller
 * @ddev: DMA device
 * @base: base address of register map
 * @chan_start: index of the first channel in the register map
 * @zero_page: bus address of zero page (to detect transactions copying from
 *	zero page and avoid accessing memory if so)
 * @cfg: platform specific description of the DMA channels
 * @map_resource: the peripheral window is described by dma-ranges, so slave
 *	addresses can be translated with dma_map_resource()
 */
struct bcm2835_dmadev {
	struct dma_device ddev;
	void __iomem *base;
	unsigned int chan_start;
	dma_addr_t zero_page;
	const struct bcm2835_dma_cfg *cfg;
	bool map_resource;
};

struct bcm_dma_cb {
	u32 rsvd[8];
};

struct bcm2835_dma_cb {
	u32 info;
	u32 src;
	u32 dst;
	u32 length;
	u32 stride;
	u32 next;
	u32 pad[2];
};

struct bcm2711_dma40_scb {
	u32 ti;
	u32 src;
	u32 srci;
	u32 dst;
	u32 dsti;
	u32 len;
	u32 next_cb;
	u32 rsvd;
};

struct bcm2835_cb_entry {
	struct bcm_dma_cb *cb;
	dma_addr_t paddr;
};

/**
 * struct bcm2835_slave_addr - bus address of a slave FIFO
 * @addr: address as seen by the DMA controller
 * @size: size of the mapping
 * @mapped: @addr has been obtained from dma_map_resource()
 */
struct bcm2835_slave_addr {
	dma_addr_t addr;
	size_t size;
	bool mapped;
};

struct bcm2835_chan {
	struct virt_dma_chan vc;

	struct dma_slave_config	cfg;
	struct bcm2835_slave_addr slave_src;
	struct bcm2835_slave_addr slave_dst;
	unsigned int dreq;

	int ch;
	struct bcm2835_desc *desc;
	struct dma_pool *cb_pool;

	void __iomem *chan_base;
	int irq_number;
	unsigned int irq_flags;

	bool is_lite_channel;
};

/**
 * struct bcm2835_dma_cfg - platform specific description of the DMA channels
 * @cs_reg: offset of the control and status register
 * @cb_reg: offset of the control block address register
 * @int_mask: interrupt status bit in the CS register
 * @active_mask: channel active bit in the CS register
 * @dma_mask: addressing capability of the channels
 * @cb_get_length: get the transfer length of a control block
 * @cb_get_addr: get the memory side address of a control block
 * @cb_init: initialize a control block
 * @cb_set_src: set the source address of a control block
 * @cb_set_dst: set the destination address of a control block
 * @cb_set_next: link a control block to the next one
 * @cb_set_length: set the transfer length of a control block
 * @cb_append_extra: add the interrupt and completion bits to a control block
 * @to_cb_addr: convert a control block bus address to its register format
 * @chan_plat_init: platform specific channel initialization
 * @read_addr: read the current memory side address of a channel
 * @cs_flags: per-channel flags for the CS register
 * @abort: stop and reset a channel
 * @legacy_bus_addr: translate a slave address for device trees which do not
 *	describe the peripheral window in dma-ranges, NULL if they cannot be
 *	supported
 * @chan_mask: channels of the DMA block using this programming model
 */
struct bcm2835_dma_cfg {
	u32 cs_reg;
	u32 cb_reg;

	u32 int_mask;
	u32 active_mask;

	u64 dma_mask;

	u32 (*cb_get_length)(void *data);
	dma_addr_t (*cb_get_addr)(void *data, enum dma_transfer_direction);

	void (*cb_init)(void *data, struct bcm2835_chan *c,
			enum dma_transfer_direction direction, dma_addr_t src,
			dma_addr_t dst, bool zero_page);
	void (*cb_set_src)(void *data, enum dma_transfer_direction direction,
			   dma_addr_t src);
	void (*cb_set_dst)(void *data, enum dma_transfer_direction direction,
			   dma_addr_t dst);
	void (*cb_set_next)(void *data, u32 next);
	void (*cb_set_length)(void *data, u32 length);
	void (*cb_append_extra)(void *data,
				struct bcm2835_chan *c,
				enum dma_transfer_direction direction,
				bool cyclic, bool final, unsigned long flags);

	u32 (*to_cb_addr)(dma_addr_t addr);

	void (*chan_plat_init)(struct bcm2835_chan *c);
	dma_addr_t (*read_addr)(struct bcm2835_chan *c,
				enum dma_transfer_direction direction);
	u32 (*cs_flags)(struct bcm2835_chan *c);
	void (*abort)(struct bcm2835_chan *c);

	u64 (*legacy_bus_addr)(struct device *dev, phys_addr_t phys);
	u32 chan_mask;
};

struct bcm2835_desc {
	struct bcm2835_chan *c;
	struct virt_dma_desc vd;
	enum dma_transfer_direction dir;

	unsigned int frames;
	size_t size;

	bool cyclic;

	struct bcm2835_cb_entry cb_list[];
};

#define BCM2835_DMA_CS		0x00
#define BCM2835_DMA_ADDR	0x04
#define BCM2835_DMA_TI		0x08
#define BCM2835_DMA_SOURCE_AD	0x0c
#define BCM2835_DMA_DEST_AD	0x10
#define BCM2835_DMA_LEN		0x14
#define BCM2835_DMA_STRIDE	0x18
#define BCM2835_DMA_NEXTCB	0x1c
#define BCM2835_DMA_DEBUG	0x20

/* DMA CS Control and Status bits */
#define BCM2835_DMA_ACTIVE	BIT(0)  /* activate the DMA */
#define BCM2835_DMA_END		BIT(1)  /* current CB has ended */
#define BCM2835_DMA_INT		BIT(2)  /* interrupt status */
#define BCM2835_DMA_DREQ	BIT(3)  /* DREQ state */
#define BCM2835_DMA_ISPAUSED	BIT(4)  /* Pause requested or not active */
#define BCM2835_DMA_ISHELD	BIT(5)  /* Is held by DREQ flow control */
#define BCM2835_DMA_WAITING_FOR_WRITES BIT(6) /* waiting for last
					       * AXI-write to ack
					       */
#define BCM2835_DMA_ERR		BIT(8)
#define BCM2835_DMA_PRIORITY(x) (((x) & 15) << 16) /* AXI priority */
#define BCM2835_DMA_PANIC_PRIORITY(x) (((x) & 15) << 20) /* panic priority */
/* current value of TI.BCM2835_DMA_WAIT_RESP */
#define BCM2835_DMA_WAIT_FOR_WRITES BIT(28)
#define BCM2835_DMA_DIS_DEBUG	BIT(29) /* disable debug pause signal */
#define BCM2835_DMA_ABORT	BIT(30) /* Stop current CB, go to next, WO */
#define BCM2835_DMA_RESET	BIT(31) /* WO, self clearing */

/* Transfer information bits - also bcm2835_cb.info field */
#define BCM2835_DMA_INT_EN	BIT(0)
#define BCM2835_DMA_TDMODE	BIT(1) /* 2D-Mode */
#define BCM2835_DMA_WAIT_RESP	BIT(3) /* wait for AXI-write to be acked */
#define BCM2835_DMA_D_INC	BIT(4)
#define BCM2835_DMA_D_WIDTH	BIT(5) /* 128bit writes if set */
#define BCM2835_DMA_D_DREQ	BIT(6) /* enable DREQ for destination */
#define BCM2835_DMA_D_IGNORE	BIT(7) /* ignore destination writes */
#define BCM2835_DMA_S_INC	BIT(8)
#define BCM2835_DMA_S_WIDTH	BIT(9) /* 128bit writes if set */
#define BCM2835_DMA_S_DREQ	BIT(10) /* enable SREQ for source */
#define BCM2835_DMA_S_IGNORE	BIT(11) /* ignore source reads - read 0 */
#define BCM2835_DMA_BURST_LENGTH(x) (((x) & 15) << 12)
#define BCM2835_DMA_GET_BURST_LENGTH(x) (((x) >> 12) & 15)
#define BCM2835_DMA_CS_FLAGS(x) ((x) & (BCM2835_DMA_PRIORITY(15) | \
				      BCM2835_DMA_PANIC_PRIORITY(15) | \
				      BCM2835_DMA_WAIT_FOR_WRITES | \
				      BCM2835_DMA_DIS_DEBUG))
#define BCM2835_DMA_PER_MAP(x)	(((x) & 31) << 16) /* REQ source */
#define BCM2835_DMA_WAIT(x)	(((x) & 31) << 21) /* add DMA-wait cycles */
#define BCM2835_DMA_NO_WIDE_BURSTS BIT(26) /* no 2 beat write bursts */

/* A fake bit to request that the driver doesn't set the WAIT_RESP bit. */
#define BCM2835_DMA_NO_WAIT_RESP BIT(27)
#define WAIT_RESP(x) (((x) & BCM2835_DMA_NO_WAIT_RESP) ? \
		      0 : BCM2835_DMA_WAIT_RESP)

/* A fake bit to request that the driver requires wide reads */
#define BCM2835_DMA_WIDE_SOURCE BIT(24)
#define WIDE_SOURCE(x) (((x) & BCM2835_DMA_WIDE_SOURCE) ? \
		      BCM2835_DMA_S_WIDTH : 0)

/* A fake bit to request that the driver requires wide writes */
#define BCM2835_DMA_WIDE_DEST BIT(25)
#define WIDE_DEST(x) (((x) & BCM2835_DMA_WIDE_DEST) ? \
		      BCM2835_DMA_D_WIDTH : 0)

/* A fake bit to request that the driver requires multi-beat burst */
#define BCM2835_DMA_BURST BIT(30)
#define BURST_LENGTH(x) (((x) & BCM2835_DMA_BURST) ? \
			 BCM2835_DMA_BURST_LENGTH(3) : 0)

/* debug register bits */
#define BCM2835_DMA_DEBUG_LAST_NOT_SET_ERR	BIT(0)
#define BCM2835_DMA_DEBUG_FIFO_ERR		BIT(1)
#define BCM2835_DMA_DEBUG_READ_ERR		BIT(2)
#define BCM2835_DMA_DEBUG_OUTSTANDING_WRITES_SHIFT 4
#define BCM2835_DMA_DEBUG_OUTSTANDING_WRITES_BITS 4
#define BCM2835_DMA_DEBUG_ID_SHIFT		16
#define BCM2835_DMA_DEBUG_ID_BITS		9
#define BCM2835_DMA_DEBUG_STATE_SHIFT		16
#define BCM2835_DMA_DEBUG_STATE_BITS		9
#define BCM2835_DMA_DEBUG_VERSION_SHIFT		25
#define BCM2835_DMA_DEBUG_VERSION_BITS		3
#define BCM2835_DMA_DEBUG_LITE			BIT(28)

/* shared registers for all dma channels */
#define BCM2835_DMA_INT_STATUS         0xfe0
#define BCM2835_DMA_ENABLE             0xff0

#define BCM2835_DMA_DATA_TYPE_S8	1
#define BCM2835_DMA_DATA_TYPE_S16	2
#define BCM2835_DMA_DATA_TYPE_S32	4
#define BCM2835_DMA_DATA_TYPE_S128	16

/* Valid only for channels 0 - 14, 15 has its own base address */
#define BCM2835_DMA_CHAN_SIZE	0x100
#define BCM2835_DMA_CHAN(n)	((n) * BCM2835_DMA_CHAN_SIZE) /* Base address */
#define BCM2835_DMA_CHANIO(base, n) ((base) + BCM2835_DMA_CHAN(n))

/* the max dma length for different channels */
#define MAX_DMA_LEN SZ_1G
#define MAX_LITE_DMA_LEN (SZ_64K - 4)

/* 40-bit DMA channel registers */
#define BCM2711_DMA40_CS		0x00
#define BCM2711_DMA40_CB		0x04
#define BCM2711_DMA40_DEBUG		0x0c
#define BCM2711_DMA40_TI		0x10
#define BCM2711_DMA40_SRC		0x14
#define BCM2711_DMA40_SRCI		0x18
#define BCM2711_DMA40_DEST		0x1c
#define BCM2711_DMA40_DESTI		0x20
#define BCM2711_DMA40_LEN		0x24
#define BCM2711_DMA40_NEXT_CB		0x28
#define BCM2711_DMA40_DEBUG2		0x2c

/* 40-bit DMA CS Control and Status bits */
#define BCM2711_DMA40_ACTIVE		BIT(0)
#define BCM2711_DMA40_END		BIT(1)
#define BCM2711_DMA40_INT		BIT(2)
#define BCM2711_DMA40_DREQ		BIT(3)  /* DREQ state */
#define BCM2711_DMA40_RD_PAUSED		BIT(4)  /* Reading is paused */
#define BCM2711_DMA40_WR_PAUSED		BIT(5)  /* Writing is paused */
#define BCM2711_DMA40_DREQ_PAUSED	BIT(6)  /* Is paused by DREQ flow control */
#define BCM2711_DMA40_WAITING_FOR_WRITES BIT(7) /* Waiting for last write */
#define BCM2711_DMA40_PROT		GENMASK(9, 8) /* AXI protection, supervisor mode */
#define BCM2711_DMA40_ERR		BIT(10)
#define BCM2711_DMA40_QOS(x)		FIELD_PREP(GENMASK(19, 16), x)
#define BCM2711_DMA40_PANIC_QOS(x)	FIELD_PREP(GENMASK(23, 20), x)
#define BCM2711_DMA40_TRANSACTIONS	BIT(25) /* AXI transactions outstanding */
#define BCM2711_DMA40_WAIT_FOR_WRITES	BIT(28)
#define BCM2711_DMA40_DISDEBUG		BIT(29)
#define BCM2711_DMA40_ABORT		BIT(30)
#define BCM2711_DMA40_HALT		BIT(31)
#define BCM2711_DMA40_CS_FLAGS(x) ((x) & (BCM2711_DMA40_QOS(15) | \
					  BCM2711_DMA40_PANIC_QOS(15) | \
					  BCM2711_DMA40_WAIT_FOR_WRITES | \
					  BCM2711_DMA40_DISDEBUG))

/* 40-bit DMA transfer information bits */
#define BCM2711_DMA40_INTEN		BIT(0)
#define BCM2711_DMA40_TDMODE		BIT(1) /* 2D-Mode */
#define BCM2711_DMA40_WAIT_RESP		BIT(2) /* wait for AXI write to be acked */
#define BCM2711_DMA40_WAIT_RD_RESP	BIT(3) /* wait for AXI read to complete */
#define BCM2711_DMA40_PER_MAP(x)	FIELD_PREP(GENMASK(13, 9), x) /* REQ source */
#define BCM2711_DMA40_S_DREQ		BIT(14) /* enable SREQ for source */
#define BCM2711_DMA40_D_DREQ		BIT(15) /* enable DREQ for destination */
#define BCM2711_DMA40_S_WAIT(x)		FIELD_PREP(GENMASK(23, 16), x) /* read-wait cycles */
#define BCM2711_DMA40_D_WAIT(x)		FIELD_PREP(GENMASK(31, 24), x) /* write-wait cycles */

/* 40-bit DMA source and destination information bits */
#define BCM2711_DMA40_ADDR_HI		GENMASK(7, 0) /* bits 39:32 of the address */
#define BCM2711_DMA40_BURST_LEN(x)	FIELD_PREP(GENMASK(11, 8), x)
#define BCM2711_DMA40_INC		BIT(12)
#define BCM2711_DMA40_SIZE_32		FIELD_PREP(GENMASK(14, 13), 0)
#define BCM2711_DMA40_SIZE_64		FIELD_PREP(GENMASK(14, 13), 1)
#define BCM2711_DMA40_SIZE_128		FIELD_PREP(GENMASK(14, 13), 2)
#define BCM2711_DMA40_SIZE_256		FIELD_PREP(GENMASK(14, 13), 3)
#define BCM2711_DMA40_IGNORE		BIT(15)
#define BCM2711_DMA40_STRIDE(x)		FIELD_PREP(GENMASK(31, 16), x) /* For 2D mode */

/* 40-bit DMA debug register bits */
#define BCM2711_DMA40_DEBUG_RESET	BIT(23)

static inline size_t bcm2835_dma_max_frame_length(struct bcm2835_chan *c)
{
	/* lite and normal channels have different max frame length */
	return c->is_lite_channel ? MAX_LITE_DMA_LEN : MAX_DMA_LEN;
}

/* how many frames of max_len size do we need to transfer len bytes */
static inline size_t bcm2835_dma_frames_for_length(size_t len,
						   size_t max_len)
{
	return DIV_ROUND_UP(len, max_len);
}

static inline struct bcm2835_dmadev *to_bcm2835_dma_dev(struct dma_device *d)
{
	return container_of(d, struct bcm2835_dmadev, ddev);
}

static inline const struct bcm2835_dma_cfg *to_bcm2835_cfg(struct dma_device *d)
{
	struct bcm2835_dmadev *od = container_of(d, struct bcm2835_dmadev, ddev);

	return od->cfg;
}

static inline struct bcm2835_chan *to_bcm2835_dma_chan(struct dma_chan *c)
{
	return container_of(c, struct bcm2835_chan, vc.chan);
}

static inline struct bcm2835_desc *to_bcm2835_dma_desc(
		struct dma_async_tx_descriptor *t)
{
	return container_of(t, struct bcm2835_desc, vd.tx);
}

static u32 bcm2835_dma_prepare_cb_info(struct bcm2835_chan *c,
				       enum dma_transfer_direction direction,
				       bool zero_page)
{
	u32 result = WAIT_RESP(c->dreq) | WIDE_SOURCE(c->dreq) |
		     WIDE_DEST(c->dreq) | BURST_LENGTH(c->dreq);

	if (direction == DMA_MEM_TO_MEM)
		return result | BCM2835_DMA_D_INC | BCM2835_DMA_S_INC;

	/* Setup DREQ channel */
	if (c->dreq != 0)
		result |= BCM2835_DMA_PER_MAP(c->dreq);

	if (direction == DMA_DEV_TO_MEM) {
		result |= BCM2835_DMA_S_DREQ | BCM2835_DMA_D_INC;
	} else {
		result |= BCM2835_DMA_D_DREQ | BCM2835_DMA_S_INC;

		/* non-lite channels can write zeroes w/o accessing memory */
		if (zero_page && !c->is_lite_channel)
			result |= BCM2835_DMA_S_IGNORE;
	}

	return result;
}

static u32 bcm2835_dma_prepare_cb_extra(struct bcm2835_chan *c,
					enum dma_transfer_direction direction,
					bool cyclic, bool final,
					unsigned long flags)
{
	u32 result = 0;

	if (cyclic) {
		if (flags & DMA_PREP_INTERRUPT)
			result |= BCM2835_DMA_INT_EN;
	} else {
		if (!final)
			return 0;

		result |= BCM2835_DMA_INT_EN;
	}

	return result;
}

static inline bool need_src_incr(enum dma_transfer_direction direction)
{
	return direction != DMA_DEV_TO_MEM;
}

static inline bool need_dst_incr(enum dma_transfer_direction direction)
{
	switch (direction) {
	case DMA_MEM_TO_MEM:
	case DMA_DEV_TO_MEM:
		return true;
	default:
		break;
	}

	return false;
}

static inline u32 bcm2835_dma_cb_get_length(void *data)
{
	struct bcm2835_dma_cb *cb = data;

	return cb->length;
}

static inline dma_addr_t
bcm2835_dma_cb_get_addr(void *data, enum dma_transfer_direction direction)
{
	struct bcm2835_dma_cb *cb = data;

	if (direction == DMA_DEV_TO_MEM)
		return cb->dst;

	return cb->src;
}

static inline void
bcm2835_dma_cb_init(void *data, struct bcm2835_chan *c,
		    enum dma_transfer_direction direction, dma_addr_t src,
		    dma_addr_t dst, bool zero_page)
{
	struct bcm2835_dma_cb *cb = data;

	cb->info = bcm2835_dma_prepare_cb_info(c, direction, zero_page);
	cb->src = src;
	cb->dst = dst;
	cb->stride = 0;
	cb->next = 0;
}

static inline void
bcm2835_dma_cb_set_src(void *data, enum dma_transfer_direction direction,
		       dma_addr_t src)
{
	struct bcm2835_dma_cb *cb = data;

	cb->src = src;
}

static inline void
bcm2835_dma_cb_set_dst(void *data, enum dma_transfer_direction direction,
		       dma_addr_t dst)
{
	struct bcm2835_dma_cb *cb = data;

	cb->dst = dst;
}

static inline void bcm2835_dma_cb_set_next(void *data, u32 next)
{
	struct bcm2835_dma_cb *cb = data;

	cb->next = next;
}

static inline void bcm2835_dma_cb_set_length(void *data, u32 length)
{
	struct bcm2835_dma_cb *cb = data;

	cb->length = length;
}

static inline void
bcm2835_dma_cb_append_extra(void *data, struct bcm2835_chan *c,
			    enum dma_transfer_direction direction,
			    bool cyclic, bool final, unsigned long flags)
{
	struct bcm2835_dma_cb *cb = data;

	cb->info |= bcm2835_dma_prepare_cb_extra(c, direction, cyclic, final,
						 flags);
}

static inline u32 bcm2835_dma_to_cb_addr(dma_addr_t addr)
{
	return addr;
}

static void bcm2835_dma_chan_plat_init(struct bcm2835_chan *c)
{
	/* check in DEBUG register if this is a LITE channel */
	if (readl(c->chan_base + BCM2835_DMA_DEBUG) & BCM2835_DMA_DEBUG_LITE)
		c->is_lite_channel = true;
}

static dma_addr_t bcm2835_dma_read_addr(struct bcm2835_chan *c,
					enum dma_transfer_direction direction)
{
	if (direction == DMA_MEM_TO_DEV)
		return readl(c->chan_base + BCM2835_DMA_SOURCE_AD);
	else if (direction == DMA_DEV_TO_MEM)
		return readl(c->chan_base + BCM2835_DMA_DEST_AD);

	return 0;
}

static u32 bcm2835_dma_cs_flags(struct bcm2835_chan *c)
{
	return BCM2835_DMA_CS_FLAGS(c->dreq);
}

static inline u32 to_bcm2711_ti(u32 info)
{
	return ((info & BCM2835_DMA_INT_EN) ? BCM2711_DMA40_INTEN : 0) |
		((info & BCM2835_DMA_WAIT_RESP) ? BCM2711_DMA40_WAIT_RESP : 0) |
		((info & BCM2835_DMA_S_DREQ) ?
		 (BCM2711_DMA40_S_DREQ | BCM2711_DMA40_WAIT_RD_RESP) : 0) |
		((info & BCM2835_DMA_D_DREQ) ? BCM2711_DMA40_D_DREQ : 0) |
		BCM2711_DMA40_PER_MAP((info >> 16) & 0x1f);
}

static inline u32 to_bcm2711_srci(u32 info)
{
	return ((info & BCM2835_DMA_S_INC) ? BCM2711_DMA40_INC : 0) |
	       ((info & BCM2835_DMA_S_WIDTH) ? BCM2711_DMA40_SIZE_128 : 0) |
	       BCM2711_DMA40_BURST_LEN(BCM2835_DMA_GET_BURST_LENGTH(info));
}

static inline u32 to_bcm2711_dsti(u32 info)
{
	return ((info & BCM2835_DMA_D_INC) ? BCM2711_DMA40_INC : 0) |
	       ((info & BCM2835_DMA_D_WIDTH) ? BCM2711_DMA40_SIZE_128 : 0) |
	       BCM2711_DMA40_BURST_LEN(BCM2835_DMA_GET_BURST_LENGTH(info));
}

static inline dma_addr_t bcm2711_dma40_addr(u32 lo, u32 info)
{
	return (dma_addr_t)(((u64)FIELD_GET(BCM2711_DMA40_ADDR_HI, info) << 32) | lo);
}

static inline u32 bcm2711_dma40_info_addr(u32 info, dma_addr_t addr)
{
	return (info & ~BCM2711_DMA40_ADDR_HI) |
	       FIELD_PREP(BCM2711_DMA40_ADDR_HI, upper_32_bits(addr));
}

static u32 bcm2711_dma_prepare_cb_extra(struct bcm2835_chan *c,
					enum dma_transfer_direction direction,
					bool cyclic, bool final,
					unsigned long flags)
{
	u32 result = 0;

	if (cyclic) {
		if (flags & DMA_PREP_INTERRUPT)
			result |= BCM2711_DMA40_INTEN;
	} else {
		if (!final)
			return 0;

		result |= BCM2711_DMA40_INTEN;

		if (direction == DMA_MEM_TO_MEM)
			result |= BCM2711_DMA40_WAIT_RESP;
	}

	return result;
}

static inline u32 bcm2711_dma_cb_get_length(void *data)
{
	struct bcm2711_dma40_scb *scb = data;

	return scb->len;
}

static inline dma_addr_t
bcm2711_dma_cb_get_addr(void *data, enum dma_transfer_direction direction)
{
	struct bcm2711_dma40_scb *scb = data;

	if (direction == DMA_DEV_TO_MEM)
		return bcm2711_dma40_addr(scb->dst, scb->dsti);

	return bcm2711_dma40_addr(scb->src, scb->srci);
}

static inline void
bcm2711_dma_cb_init(void *data, struct bcm2835_chan *c,
		    enum dma_transfer_direction direction, dma_addr_t src,
		    dma_addr_t dst, bool zero_page)
{
	struct bcm2711_dma40_scb *scb = data;
	u32 info = bcm2835_dma_prepare_cb_info(c, direction, zero_page);

	scb->ti = to_bcm2711_ti(info);
	scb->src = lower_32_bits(src);
	scb->srci = bcm2711_dma40_info_addr(to_bcm2711_srci(info), src);
	scb->dst = lower_32_bits(dst);
	scb->dsti = bcm2711_dma40_info_addr(to_bcm2711_dsti(info), dst);
	scb->len = 0;
	scb->next_cb = 0;
	scb->rsvd = 0;
}

static inline void
bcm2711_dma_cb_set_src(void *data, enum dma_transfer_direction direction,
		       dma_addr_t src)
{
	struct bcm2711_dma40_scb *scb = data;

	scb->src = lower_32_bits(src);
	scb->srci = bcm2711_dma40_info_addr(scb->srci, src);
}

static inline void
bcm2711_dma_cb_set_dst(void *data, enum dma_transfer_direction direction,
		       dma_addr_t dst)
{
	struct bcm2711_dma40_scb *scb = data;

	scb->dst = lower_32_bits(dst);
	scb->dsti = bcm2711_dma40_info_addr(scb->dsti, dst);
}

static inline void bcm2711_dma_cb_set_next(void *data, u32 next)
{
	struct bcm2711_dma40_scb *scb = data;

	scb->next_cb = next;
}

static inline void bcm2711_dma_cb_set_length(void *data, u32 length)
{
	struct bcm2711_dma40_scb *scb = data;

	scb->len = length;
}

static inline void
bcm2711_dma_cb_append_extra(void *data, struct bcm2835_chan *c,
			    enum dma_transfer_direction direction,
			    bool cyclic, bool final, unsigned long flags)
{
	struct bcm2711_dma40_scb *scb = data;

	scb->ti |= bcm2711_dma_prepare_cb_extra(c, direction, cyclic, final,
						flags);
}

/* control blocks are 32 byte aligned, the register holds bits 36:5 */
static inline u32 bcm2711_dma_to_cb_addr(dma_addr_t addr)
{
	WARN_ON_ONCE(addr & 0x1f);

	return (u64)addr >> 5;
}

static dma_addr_t bcm2711_dma_read_addr(struct bcm2835_chan *c,
					enum dma_transfer_direction direction)
{
	if (direction == DMA_MEM_TO_DEV)
		return bcm2711_dma40_addr(readl(c->chan_base + BCM2711_DMA40_SRC),
					  readl(c->chan_base + BCM2711_DMA40_SRCI));
	else if (direction == DMA_DEV_TO_MEM)
		return bcm2711_dma40_addr(readl(c->chan_base + BCM2711_DMA40_DEST),
					  readl(c->chan_base + BCM2711_DMA40_DESTI));

	return 0;
}

static u32 bcm2711_dma_cs_flags(struct bcm2835_chan *c)
{
	return BCM2711_DMA40_CS_FLAGS(c->dreq) | BCM2711_DMA40_PROT;
}

static void bcm2711_dma_abort_chan(struct bcm2835_chan *c)
{
	void __iomem *chan_base = c->chan_base;
	long timeout = 100;

	/* Pause the current DMA */
	writel(readl(chan_base + BCM2711_DMA40_CS) & ~BCM2711_DMA40_ACTIVE,
	       chan_base + BCM2711_DMA40_CS);

	/* wait for outstanding transactions to complete */
	while ((readl(chan_base + BCM2711_DMA40_CS) & BCM2711_DMA40_TRANSACTIONS) &&
	       --timeout)
		cpu_relax();

	/* Peripheral might be stuck and fail to complete */
	if (!timeout)
		dev_err(c->vc.chan.device->dev,
			"failed to complete pause on dma %d (CS:%08x)\n", c->ch,
			readl(chan_base + BCM2711_DMA40_CS));

	/* Set CS back to default state */
	writel(BCM2711_DMA40_PROT, chan_base + BCM2711_DMA40_CS);

	/* Reset the DMA */
	writel(readl(chan_base + BCM2711_DMA40_DEBUG) | BCM2711_DMA40_DEBUG_RESET,
	       chan_base + BCM2711_DMA40_DEBUG);
}

static void bcm2835_dma_free_cb_chain(struct bcm2835_desc *desc)
{
	size_t i;

	for (i = 0; i < desc->frames; i++)
		dma_pool_free(desc->c->cb_pool, desc->cb_list[i].cb,
			      desc->cb_list[i].paddr);

	kfree(desc);
}

static void bcm2835_dma_desc_free(struct virt_dma_desc *vd)
{
	bcm2835_dma_free_cb_chain(container_of(vd, struct bcm2835_desc, vd));
}

static bool bcm2835_dma_create_cb_set_length(struct dma_chan *chan,
					     void *data,
					     size_t len,
					     size_t period_len,
					     size_t *total_len)
{
	const struct bcm2835_dma_cfg *cfg = to_bcm2835_cfg(chan->device);
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);
	size_t max_len = bcm2835_dma_max_frame_length(c);

	/* set the length taking lite-channel limitations into account */
	u32 length = min_t(u32, len, max_len);

	cfg->cb_set_length(data, length);

	/* finished if we have no period_length */
	if (!period_len)
		return false;

	/*
	 * period_len means: that we need to generate
	 * transfers that are terminating at every
	 * multiple of period_len - this is typically
	 * used to set the interrupt flag in info
	 * which is required during cyclic transfers
	 */

	/* have we filled in period_length yet? */
	if (*total_len + length < period_len) {
		/* update number of bytes in this period so far */
		*total_len += length;
		return false;
	}

	/* calculate the length that remains to reach period_length */
	cfg->cb_set_length(data, period_len - *total_len);

	/* reset total_length for next period */
	*total_len = 0;

	return true;
}

/**
 * bcm2835_dma_create_cb_chain - create a control block and fills data in
 *
 * @chan:           the @dma_chan for which we run this
 * @direction:      the direction in which we transfer
 * @cyclic:         it is a cyclic transfer
 * @frames:         number of controlblocks to allocate
 * @src:            the src address to assign
 * @dst:            the dst address to assign
 * @buf_len:        the full buffer length (may also be 0)
 * @period_len:     the period length when to apply @finalextrainfo
 *                  in addition to the last transfer
 *                  this will also break some control-blocks early
 * @gfp:            the GFP flag to use for allocation
 * @flags:          the dmaengine prep flags (DMA_PREP_*) of the transfer
 */
static struct bcm2835_desc *bcm2835_dma_create_cb_chain(
					struct dma_chan *chan,
					enum dma_transfer_direction direction,
					bool cyclic, size_t frames,
					dma_addr_t src,	dma_addr_t dst,
					size_t buf_len, size_t period_len,
					gfp_t gfp, unsigned long flags)
{
	const struct bcm2835_dma_cfg *cfg = to_bcm2835_cfg(chan->device);
	struct bcm2835_dmadev *od = to_bcm2835_dma_dev(chan->device);
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);
	size_t len = buf_len, total_len;
	size_t frame;
	struct bcm2835_desc *d;
	struct bcm2835_cb_entry *cb_entry;
	struct bcm_dma_cb *control_block;
	bool zero_page = false;

	if (!frames)
		return NULL;

	/* allocate and setup the descriptor. */
	d = kzalloc_flex(*d, cb_list, frames, gfp);
	if (!d)
		return NULL;

	d->c = c;
	d->dir = direction;
	d->cyclic = cyclic;

	switch (direction) {
	case DMA_MEM_TO_MEM:
	case DMA_DEV_TO_MEM:
		break;
	default:
		zero_page = src == od->zero_page;
	}

	/*
	 * Iterate over all frames, create a control block
	 * for each frame and link them together.
	 */
	for (frame = 0, total_len = 0; frame < frames; d->frames++, frame++) {
		cb_entry = &d->cb_list[frame];
		cb_entry->cb = dma_pool_alloc(c->cb_pool, gfp,
					      &cb_entry->paddr);
		if (!cb_entry->cb)
			goto error_cb;

		/* fill in the control block */
		control_block = cb_entry->cb;
		cfg->cb_init(control_block, c, direction, src, dst, zero_page);

		/* set up length in control_block if requested */
		if (buf_len) {
			/* calculate length honoring period_length */
			if (bcm2835_dma_create_cb_set_length(chan, control_block,
							     len, period_len,
							     &total_len)) {
				/* add extrainfo bits in info */
				cfg->cb_append_extra(control_block, c,
						     direction, cyclic,
						     false, flags);
			}

			/* calculate new remaining length */
			len -= cfg->cb_get_length(control_block);
		}

		/* link this the last controlblock */
		if (frame)
			cfg->cb_set_next(d->cb_list[frame - 1].cb,
					 cfg->to_cb_addr(cb_entry->paddr));

		/* update src and dst and length */
		if (src && need_src_incr(direction))
			src += cfg->cb_get_length(control_block);
		if (dst && need_dst_incr(direction))
			dst += cfg->cb_get_length(control_block);

		/* Length of total transfer */
		d->size += cfg->cb_get_length(control_block);
	}

	/* the last frame requires extra flags */
	cfg->cb_append_extra(d->cb_list[d->frames - 1].cb, c, direction, cyclic,
			     true, flags);

	/* detect a size mismatch */
	if (buf_len && d->size != buf_len)
		goto error_cb;

	return d;
error_cb:
	bcm2835_dma_free_cb_chain(d);

	return NULL;
}

static void bcm2835_dma_fill_cb_chain_with_sg(
	struct dma_chan *chan,
	enum dma_transfer_direction direction,
	struct bcm2835_cb_entry *cb,
	struct scatterlist *sgl,
	unsigned int sg_len)
{
	const struct bcm2835_dma_cfg *cfg = to_bcm2835_cfg(chan->device);
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);
	size_t len, max_len;
	unsigned int i;
	dma_addr_t addr;
	struct scatterlist *sgent;

	max_len = bcm2835_dma_max_frame_length(c);
	for_each_sg(sgl, sgent, sg_len, i) {
		for (addr = sg_dma_address(sgent), len = sg_dma_len(sgent);
		     len > 0;
		     addr += cfg->cb_get_length(cb->cb), len -= cfg->cb_get_length(cb->cb), cb++) {
			if (direction == DMA_DEV_TO_MEM)
				cfg->cb_set_dst(cb->cb, direction, addr);
			else
				cfg->cb_set_src(cb->cb, direction, addr);
			cfg->cb_set_length(cb->cb, min(len, max_len));
		}
	}
}

static void bcm2835_dma_abort_chan(struct bcm2835_chan *c)
{
	void __iomem *chan_base = c->chan_base;
	long timeout = 100;

	/* We need to clear the next DMA block pending */
	writel(0, chan_base + BCM2835_DMA_NEXTCB);

	/* Abort the DMA, which needs to be enabled to complete */
	writel(readl(chan_base + BCM2835_DMA_CS) | BCM2835_DMA_ABORT | BCM2835_DMA_ACTIVE,
	       chan_base + BCM2835_DMA_CS);

	/* wait for DMA to be aborted */
	while ((readl(chan_base + BCM2835_DMA_CS) & BCM2835_DMA_ABORT) && --timeout)
		cpu_relax();

	/* Write 0 to the active bit - Pause the DMA */
	writel(readl(chan_base + BCM2835_DMA_CS) & ~BCM2835_DMA_ACTIVE,
	       chan_base + BCM2835_DMA_CS);

	/*
	 * Peripheral might be stuck and fail to complete
	 * This is expected when dreqs are enabled but not asserted
	 * so only report error in non dreq case
	 */
	if (!timeout && !(readl(chan_base + BCM2835_DMA_TI) &
	   (BCM2835_DMA_S_DREQ | BCM2835_DMA_D_DREQ)))
		dev_err(c->vc.chan.device->dev,
			"failed to complete pause on dma %d (CS:%08x)\n", c->ch,
			readl(chan_base + BCM2835_DMA_CS));

	/* Set CS back to default state and reset the DMA */
	writel(BCM2835_DMA_RESET, chan_base + BCM2835_DMA_CS);
}

static void bcm2835_dma_abort(struct dma_chan *chan)
{
	const struct bcm2835_dma_cfg *cfg = to_bcm2835_cfg(chan->device);
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);

	/*
	 * A zero control block address means the channel is idle.
	 * (The ACTIVE flag in the CS register is not a reliable indicator.)
	 */
	if (!readl(c->chan_base + cfg->cb_reg))
		return;

	cfg->abort(c);
}

static void bcm2835_dma_start_desc(struct dma_chan *chan)
{
	const struct bcm2835_dma_cfg *cfg = to_bcm2835_cfg(chan->device);
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);
	struct virt_dma_desc *vd = vchan_next_desc(&c->vc);

	if (!vd) {
		c->desc = NULL;
		return;
	}

	list_del(&vd->node);

	c->desc = to_bcm2835_dma_desc(&vd->tx);

	writel(cfg->to_cb_addr(c->desc->cb_list[0].paddr), c->chan_base + cfg->cb_reg);
	writel(cfg->active_mask | cfg->cs_flags(c),
	       c->chan_base + cfg->cs_reg);
}

static irqreturn_t bcm2835_dma_callback(int irq, void *data)
{
	struct dma_chan *chan = data;
	const struct bcm2835_dma_cfg *cfg = to_bcm2835_cfg(chan->device);
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);
	struct bcm2835_desc *d;
	unsigned long flags;

	/* check the shared interrupt */
	if (c->irq_flags & IRQF_SHARED) {
		/* check if the interrupt is enabled */
		flags = readl(c->chan_base + cfg->cs_reg);
		/* if not set then we are not the reason for the irq */
		if (!(flags & cfg->int_mask))
			return IRQ_NONE;
	}

	spin_lock_irqsave(&c->vc.lock, flags);

	/*
	 * Clear the INT flag to receive further interrupts. Keep the channel
	 * active in case the descriptor is cyclic or in case the client has
	 * already terminated the descriptor and issued a new one. (May happen
	 * if this IRQ handler is threaded.) If the channel is finished, it
	 * will remain idle despite the ACTIVE flag being set.
	 */
	writel(cfg->int_mask | cfg->active_mask | cfg->cs_flags(c), c->chan_base + cfg->cs_reg);

	d = c->desc;

	if (d) {
		if (d->cyclic) {
			/* call the cyclic callback */
			vchan_cyclic_callback(&d->vd);
		} else if (!readl(c->chan_base + cfg->cb_reg)) {
			vchan_cookie_complete(&c->desc->vd);
			bcm2835_dma_start_desc(chan);
		}
	}

	spin_unlock_irqrestore(&c->vc.lock, flags);

	return IRQ_HANDLED;
}

static int bcm2835_dma_alloc_chan_resources(struct dma_chan *chan)
{
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);
	struct device *dev = c->vc.chan.device->dev;

	dev_dbg(dev, "Allocating DMA channel %d\n", c->ch);

	/*
	 * Control blocks are 256 bit in length and must start at a 256 bit
	 * (32 byte) aligned address (BCM2835 ARM Peripherals, sec. 4.2.1.1).
	 */
	c->cb_pool = dma_pool_create(dev_name(dev), dev,
				     sizeof(struct bcm_dma_cb), 32, 0);
	if (!c->cb_pool) {
		dev_err(dev, "unable to allocate descriptor pool\n");
		return -ENOMEM;
	}

	return request_irq(c->irq_number, bcm2835_dma_callback,
			   c->irq_flags, "DMA IRQ", chan);
}

static void bcm2835_dma_unmap_slave_addr(struct dma_chan *chan,
					 struct bcm2835_slave_addr *sa,
					 enum dma_data_direction dir)
{
	if (sa->mapped)
		dma_unmap_resource(chan->device->dev, sa->addr, sa->size, dir, 0);

	memset(sa, 0, sizeof(*sa));
}

static void bcm2835_dma_unmap_slave_addrs(struct dma_chan *chan)
{
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);

	bcm2835_dma_unmap_slave_addr(chan, &c->slave_src, DMA_FROM_DEVICE);
	bcm2835_dma_unmap_slave_addr(chan, &c->slave_dst, DMA_TO_DEVICE);
}

static void bcm2835_dma_free_chan_resources(struct dma_chan *chan)
{
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);

	vchan_free_chan_resources(&c->vc);
	bcm2835_dma_unmap_slave_addrs(chan);
	free_irq(c->irq_number, chan);
	dma_pool_destroy(c->cb_pool);

	dev_dbg(c->vc.chan.device->dev, "Freeing DMA channel %u\n", c->ch);
}

static size_t bcm2835_dma_desc_size(struct bcm2835_desc *d)
{
	return d->size;
}

static size_t bcm2835_dma_desc_size_pos(const struct bcm2835_dma_cfg *cfg,
					struct bcm2835_desc *d, dma_addr_t addr)
{
	unsigned int i;
	size_t size;

	for (size = i = 0; i < d->frames; i++) {
		struct bcm_dma_cb *control_block = d->cb_list[i].cb;
		size_t this_size = cfg->cb_get_length(control_block);
		dma_addr_t dma = cfg->cb_get_addr(control_block, d->dir);

		if (size)
			size += this_size;
		else if (addr >= dma && addr < dma + this_size)
			size += dma + this_size - addr;
	}

	return size;
}

static enum dma_status bcm2835_dma_tx_status(struct dma_chan *chan,
					     dma_cookie_t cookie,
					     struct dma_tx_state *txstate)
{
	const struct bcm2835_dma_cfg *cfg = to_bcm2835_cfg(chan->device);
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);
	struct virt_dma_desc *vd;
	enum dma_status ret;
	unsigned long flags;

	ret = dma_cookie_status(chan, cookie, txstate);
	if (ret == DMA_COMPLETE || !txstate)
		return ret;

	spin_lock_irqsave(&c->vc.lock, flags);
	vd = vchan_find_desc(&c->vc, cookie);
	if (vd) {
		txstate->residue =
			bcm2835_dma_desc_size(to_bcm2835_dma_desc(&vd->tx));
	} else if (c->desc && c->desc->vd.tx.cookie == cookie) {
		struct bcm2835_desc *d = c->desc;
		dma_addr_t pos;

		pos = cfg->read_addr(c, d->dir);
		txstate->residue = bcm2835_dma_desc_size_pos(cfg, d, pos);
	} else {
		txstate->residue = 0;
	}

	spin_unlock_irqrestore(&c->vc.lock, flags);

	return ret;
}

static void bcm2835_dma_issue_pending(struct dma_chan *chan)
{
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);
	unsigned long flags;

	spin_lock_irqsave(&c->vc.lock, flags);
	if (vchan_issue_pending(&c->vc) && !c->desc)
		bcm2835_dma_start_desc(chan);

	spin_unlock_irqrestore(&c->vc.lock, flags);
}

static struct dma_async_tx_descriptor *bcm2835_dma_prep_dma_memcpy(
	struct dma_chan *chan, dma_addr_t dst, dma_addr_t src,
	size_t len, unsigned long flags)
{
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);
	struct bcm2835_desc *d;
	size_t max_len = bcm2835_dma_max_frame_length(c);
	size_t frames;

	/* if src, dst or len is not given return with an error */
	if (!src || !dst || !len)
		return NULL;

	/* calculate number of frames */
	frames = bcm2835_dma_frames_for_length(len, max_len);

	/* allocate the CB chain - this also fills in the pointers */
	d = bcm2835_dma_create_cb_chain(chan, DMA_MEM_TO_MEM, false, frames,
					src, dst, len, 0, GFP_KERNEL, 0);
	if (!d)
		return NULL;

	return vchan_tx_prep(&c->vc, &d->vd, flags);
}

static struct dma_async_tx_descriptor *bcm2835_dma_prep_slave_sg(
	struct dma_chan *chan,
	struct scatterlist *sgl, unsigned int sg_len,
	enum dma_transfer_direction direction,
	unsigned long flags, void *context)
{
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);
	struct bcm2835_desc *d;
	dma_addr_t src = 0, dst = 0;
	size_t frames;

	if (!is_slave_direction(direction)) {
		dev_err(chan->device->dev,
			"%s: bad direction?\n", __func__);
		return NULL;
	}

	if (direction == DMA_DEV_TO_MEM) {
		if (c->cfg.src_addr_width != DMA_SLAVE_BUSWIDTH_4_BYTES)
			return NULL;
		src = c->slave_src.addr;
	} else {
		if (c->cfg.dst_addr_width != DMA_SLAVE_BUSWIDTH_4_BYTES)
			return NULL;
		dst = c->slave_dst.addr;
	}

	/* count frames in sg list */
	frames = sg_nents_for_dma(sgl, sg_len, bcm2835_dma_max_frame_length(c));

	/* allocate the CB chain */
	d = bcm2835_dma_create_cb_chain(chan, direction, false, frames, src,
					dst, 0, 0, GFP_NOWAIT, 0);
	if (!d)
		return NULL;

	/* fill in frames with scatterlist pointers */
	bcm2835_dma_fill_cb_chain_with_sg(chan, direction, d->cb_list,
					  sgl, sg_len);

	return vchan_tx_prep(&c->vc, &d->vd, flags);
}

static struct dma_async_tx_descriptor *bcm2835_dma_prep_dma_cyclic(
	struct dma_chan *chan, dma_addr_t buf_addr, size_t buf_len,
	size_t period_len, enum dma_transfer_direction direction,
	unsigned long flags)
{
	const struct bcm2835_dma_cfg *cfg = to_bcm2835_cfg(chan->device);
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);
	struct bcm2835_desc *d;
	dma_addr_t src, dst;
	size_t max_len = bcm2835_dma_max_frame_length(c);
	size_t frames;

	/* Grab configuration */
	if (!is_slave_direction(direction)) {
		dev_err(chan->device->dev, "%s: bad direction?\n", __func__);
		return NULL;
	}

	if (!buf_len) {
		dev_err(chan->device->dev,
			"%s: bad buffer length (= 0)\n", __func__);
		return NULL;
	}

	if (!(flags & DMA_PREP_INTERRUPT))
		period_len = buf_len;

	/*
	 * warn if buf_len is not a multiple of period_len - this may leed
	 * to unexpected latencies for interrupts and thus audiable clicks
	 */
	if (buf_len % period_len)
		dev_warn_once(chan->device->dev,
			      "%s: buffer_length (%zd) is not a multiple of period_len (%zd)\n",
			      __func__, buf_len, period_len);

	if (direction == DMA_DEV_TO_MEM) {
		if (c->cfg.src_addr_width != DMA_SLAVE_BUSWIDTH_4_BYTES)
			return NULL;
		src = c->slave_src.addr;
		dst = buf_addr;
	} else {
		if (c->cfg.dst_addr_width != DMA_SLAVE_BUSWIDTH_4_BYTES)
			return NULL;
		dst = c->slave_dst.addr;
		src = buf_addr;
	}

	/* calculate number of frames */
	frames = /* number of periods */
		 DIV_ROUND_UP(buf_len, period_len) *
		 /* number of frames per period */
		 bcm2835_dma_frames_for_length(period_len, max_len);

	/*
	 * allocate the CB chain
	 * note that we need to use GFP_NOWAIT, as the ALSA i2s dmaengine
	 * implementation calls prep_dma_cyclic with interrupts disabled.
	 */
	d = bcm2835_dma_create_cb_chain(chan, direction, true, frames, src, dst,
					buf_len, period_len, GFP_NOWAIT, flags);
	if (!d)
		return NULL;

	/* wrap around into a loop */
	cfg->cb_set_next(d->cb_list[d->frames - 1].cb,
			 cfg->to_cb_addr(d->cb_list[0].paddr));

	return vchan_tx_prep(&c->vc, &d->vd, flags);
}

/*
 * Device trees which do not describe the peripheral window in dma-ranges
 * are handled by assuming that the legacy DMA channels see the peripherals
 * at the same address as the bus they sit on, i.e. by reverting the
 * translation described by the ranges property of the parent bus. This is
 * how the VideoCore bus addresses have always been derived. Addresses which
 * are not CPU physical addresses of that bus are passed through unchanged,
 * as legacy clients used to provide bus addresses.
 */
static u64 bcm2835_dma_legacy_bus_addr(struct device *dev, phys_addr_t phys)
{
	struct device_node *bus = of_get_parent(dev->of_node);
	struct of_range_parser parser;
	struct of_range range;
	u64 addr = phys;

	if (bus && !of_range_parser_init(&parser, bus)) {
		for_each_of_range(&parser, &range) {
			if (phys >= range.cpu_addr &&
			    phys - range.cpu_addr < range.size) {
				addr = range.bus_addr + (phys - range.cpu_addr);
				break;
			}
		}
	}

	of_node_put(bus);

	return addr;
}

static int bcm2835_dma_map_slave_addr(struct dma_chan *chan,
				      struct bcm2835_slave_addr *sa,
				      phys_addr_t phys,
				      enum dma_slave_buswidth width,
				      enum dma_data_direction dir)
{
	struct bcm2835_dmadev *od = to_bcm2835_dma_dev(chan->device);
	struct device *dev = chan->device->dev;
	size_t size = width ? width : DMA_SLAVE_BUSWIDTH_4_BYTES;

	if (!phys)
		return 0;

	if (!od->map_resource) {
		sa->addr = od->cfg->legacy_bus_addr(dev, phys);
		return 0;
	}

	sa->addr = dma_map_resource(dev, phys, size, dir, 0);
	if (dma_mapping_error(dev, sa->addr)) {
		dev_err(dev, "failed to map slave address %pa\n", &phys);
		sa->addr = 0;
		return -EINVAL;
	}

	sa->size = size;
	sa->mapped = true;

	return 0;
}

static int bcm2835_dma_slave_config(struct dma_chan *chan,
				    struct dma_slave_config *cfg)
{
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);
	int ret;

	bcm2835_dma_unmap_slave_addrs(chan);

	ret = bcm2835_dma_map_slave_addr(chan, &c->slave_src, cfg->src_addr,
					 cfg->src_addr_width, DMA_FROM_DEVICE);
	if (ret)
		return ret;

	ret = bcm2835_dma_map_slave_addr(chan, &c->slave_dst, cfg->dst_addr,
					 cfg->dst_addr_width, DMA_TO_DEVICE);
	if (ret) {
		bcm2835_dma_unmap_slave_addrs(chan);
		return ret;
	}

	c->cfg = *cfg;

	return 0;
}

static int bcm2835_dma_terminate_all(struct dma_chan *chan)
{
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);
	unsigned long flags;
	LIST_HEAD(head);

	spin_lock_irqsave(&c->vc.lock, flags);

	/* stop DMA activity */
	if (c->desc) {
		vchan_terminate_vdesc(&c->desc->vd);
		c->desc = NULL;
		bcm2835_dma_abort(chan);
	}

	vchan_get_all_descriptors(&c->vc, &head);
	spin_unlock_irqrestore(&c->vc.lock, flags);
	vchan_dma_desc_free_list(&c->vc, &head);

	return 0;
}

static void bcm2835_dma_synchronize(struct dma_chan *chan)
{
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);

	vchan_synchronize(&c->vc);
}

static int bcm2835_dma_chan_init(struct bcm2835_dmadev *d, int chan_id,
				 int irq, unsigned int irq_flags)
{
	struct bcm2835_chan *c;

	c = devm_kzalloc(d->ddev.dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;

	c->vc.desc_free = bcm2835_dma_desc_free;
	vchan_init(&c->vc, &d->ddev);

	c->chan_base = BCM2835_DMA_CHANIO(d->base, chan_id - d->chan_start);
	c->ch = chan_id;
	c->irq_number = irq;
	c->irq_flags = irq_flags;

	if (d->cfg->chan_plat_init)
		d->cfg->chan_plat_init(c);

	return 0;
}

static void bcm2835_dma_free(struct bcm2835_dmadev *od)
{
	struct bcm2835_chan *c, *next;

	list_for_each_entry_safe(c, next, &od->ddev.channels,
				 vc.chan.device_node) {
		list_del(&c->vc.chan.device_node);
		tasklet_kill(&c->vc.task);
	}

	dma_unmap_page_attrs(od->ddev.dev, od->zero_page, PAGE_SIZE,
			     DMA_TO_DEVICE, DMA_ATTR_SKIP_CPU_SYNC);
}

static const struct bcm2835_dma_cfg bcm2835_data = {
	.cs_reg = BCM2835_DMA_CS,
	.cb_reg = BCM2835_DMA_ADDR,

	.int_mask = BCM2835_DMA_INT,
	.active_mask = BCM2835_DMA_ACTIVE,

	.dma_mask = DMA_BIT_MASK(32),

	.cb_get_length = bcm2835_dma_cb_get_length,
	.cb_get_addr = bcm2835_dma_cb_get_addr,
	.cb_init = bcm2835_dma_cb_init,
	.cb_set_src = bcm2835_dma_cb_set_src,
	.cb_set_dst = bcm2835_dma_cb_set_dst,
	.cb_set_next = bcm2835_dma_cb_set_next,
	.cb_set_length = bcm2835_dma_cb_set_length,
	.cb_append_extra = bcm2835_dma_cb_append_extra,

	.to_cb_addr = bcm2835_dma_to_cb_addr,

	.chan_plat_init = bcm2835_dma_chan_plat_init,
	.read_addr = bcm2835_dma_read_addr,
	.cs_flags = bcm2835_dma_cs_flags,
	.abort = bcm2835_dma_abort_chan,

	.legacy_bus_addr = bcm2835_dma_legacy_bus_addr,
	.chan_mask = GENMASK(BCM2835_DMA_MAX_DMA_CHAN_SUPPORTED, 0),
};

static const struct bcm2835_dma_cfg bcm2711_data = {
	.cs_reg = BCM2711_DMA40_CS,
	.cb_reg = BCM2711_DMA40_CB,

	.int_mask = BCM2711_DMA40_INT,
	.active_mask = BCM2711_DMA40_ACTIVE,

	.dma_mask = DMA_BIT_MASK(40),

	.cb_get_length = bcm2711_dma_cb_get_length,
	.cb_get_addr = bcm2711_dma_cb_get_addr,
	.cb_init = bcm2711_dma_cb_init,
	.cb_set_src = bcm2711_dma_cb_set_src,
	.cb_set_dst = bcm2711_dma_cb_set_dst,
	.cb_set_next = bcm2711_dma_cb_set_next,
	.cb_set_length = bcm2711_dma_cb_set_length,
	.cb_append_extra = bcm2711_dma_cb_append_extra,

	.to_cb_addr = bcm2711_dma_to_cb_addr,

	.read_addr = bcm2711_dma_read_addr,
	.cs_flags = bcm2711_dma_cs_flags,
	.abort = bcm2711_dma_abort_chan,

	.chan_mask = GENMASK(14, 11),
};

static const struct of_device_id bcm2835_dma_of_match[] = {
	{ .compatible = "brcm,bcm2711-dma", .data = &bcm2711_data },
	{ .compatible = "brcm,bcm2835-dma", .data = &bcm2835_data },
	{},
};
MODULE_DEVICE_TABLE(of, bcm2835_dma_of_match);

static struct dma_chan *bcm2835_dma_xlate(struct of_phandle_args *spec,
					  struct of_dma *ofdma)
{
	struct bcm2835_dmadev *d = ofdma->of_dma_data;
	struct dma_chan *chan;

	chan = dma_get_any_slave_channel(&d->ddev);
	if (!chan)
		return NULL;

	/* Set DREQ from param */
	to_bcm2835_dma_chan(chan)->dreq = spec->args[0];

	return chan;
}

static int bcm2835_dma_suspend_late(struct device *dev)
{
	struct bcm2835_dmadev *od = dev_get_drvdata(dev);
	struct bcm2835_chan *c, *next;

	list_for_each_entry_safe(c, next, &od->ddev.channels,
				 vc.chan.device_node) {
		void __iomem *chan_base = c->chan_base;

		/* Check if DMA channel is busy */
		if (readl(chan_base + od->cfg->cb_reg))
			return -EBUSY;
	}

	return 0;
}

/*
 * The DMA controller registers sit in the same peripheral window as the
 * FIFOs of its clients, so the window is described by dma-ranges if and
 * only if the controller registers are.
 */
static bool bcm2835_dma_mmio_in_dma_ranges(struct device *dev,
					   phys_addr_t mmio)
{
	struct of_range_parser parser;
	struct of_range range;
	struct device_node *np;
	bool found = false;

	for (np = of_get_parent(dev->of_node); np; np = of_get_next_parent(np))
		if (of_property_present(np, "dma-ranges"))
			break;

	/*
	 * Without any dma-ranges the translation is unknown: the peripherals
	 * are not at their CPU physical address on any of these SoCs.
	 */
	if (!np)
		return false;

	if (!of_pci_dma_range_parser_init(&parser, np)) {
		/* An empty dma-ranges describes a 1:1 translation too */
		found = !of_range_count(&parser);

		for_each_of_range(&parser, &range) {
			if (mmio >= range.cpu_addr &&
			    mmio - range.cpu_addr < range.size) {
				found = true;
				break;
			}
		}
	}

	of_node_put(np);

	return found;
}

static const struct dev_pm_ops bcm2835_dma_pm_ops = {
	LATE_SYSTEM_SLEEP_PM_OPS(bcm2835_dma_suspend_late, NULL)
};

static int bcm2835_dma_probe(struct platform_device *pdev)
{
	struct bcm2835_dmadev *od;
	struct resource *res;
	void __iomem *base;
	int rc;
	int i, j;
	int irq[BCM2835_DMA_MAX_DMA_CHAN_SUPPORTED + 1];
	int irq_flags;
	u32 chans_available;
	char chan_name[BCM2835_DMA_CHAN_NAME_SIZE];
	int chan_count, chan_start, chan_end;

	od = devm_kzalloc(&pdev->dev, sizeof(*od), GFP_KERNEL);
	if (!od)
		return -ENOMEM;

	od->cfg = device_get_match_data(&pdev->dev);
	if (!od->cfg)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "Failed to match compatible string\n");

	/* the peripherals are only reachable above 4 GiB by 40-bit channels */
	if (!IS_ENABLED(CONFIG_ARCH_DMA_ADDR_T_64BIT) &&
	    od->cfg->dma_mask > DMA_BIT_MASK(32))
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "40-bit DMA channels need a 64-bit dma_addr_t\n");

	if (!pdev->dev.dma_mask)
		pdev->dev.dma_mask = &pdev->dev.coherent_dma_mask;

	/*
	 * The control blocks are addressed through 32-bit registers holding
	 * at most bits 36:5 of their address.
	 */
	rc = dma_set_mask(&pdev->dev, od->cfg->dma_mask);
	if (!rc)
		rc = dma_set_coherent_mask(&pdev->dev,
					   min(od->cfg->dma_mask, DMA_BIT_MASK(37)));
	if (rc) {
		dev_err(&pdev->dev, "Unable to set DMA mask\n");
		return rc;
	}

	dma_set_max_seg_size(&pdev->dev, 0x3FFFFFFF);

	base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	/*
	 * The channels of a DMA block can be split across multiple instances,
	 * e.g. because they have a different programming model. The block
	 * itself is 4 KiB aligned, so the position of the first channel of
	 * this instance can be derived from the physical register address.
	 */
	chan_start = (res->start / BCM2835_DMA_CHAN_SIZE) & 0xf;
	chan_count = resource_size(res) / BCM2835_DMA_CHAN_SIZE;
	chan_end = min(chan_start + chan_count,
		       BCM2835_DMA_MAX_DMA_CHAN_SUPPORTED + 1);

	od->base = base;
	od->chan_start = chan_start;

	od->map_resource = bcm2835_dma_mmio_in_dma_ranges(&pdev->dev,
							  res->start);
	if (!od->map_resource) {
		if (!od->cfg->legacy_bus_addr)
			return dev_err_probe(&pdev->dev, -EINVAL,
					     "peripheral window missing in dma-ranges\n");

		dev_info(&pdev->dev,
			 "peripheral window missing in dma-ranges, please update DT blob\n");
	}

	dma_cap_set(DMA_SLAVE, od->ddev.cap_mask);
	dma_cap_set(DMA_PRIVATE, od->ddev.cap_mask);
	dma_cap_set(DMA_CYCLIC, od->ddev.cap_mask);
	dma_cap_set(DMA_MEMCPY, od->ddev.cap_mask);
	od->ddev.device_alloc_chan_resources = bcm2835_dma_alloc_chan_resources;
	od->ddev.device_free_chan_resources = bcm2835_dma_free_chan_resources;
	od->ddev.device_tx_status = bcm2835_dma_tx_status;
	od->ddev.device_issue_pending = bcm2835_dma_issue_pending;
	od->ddev.device_prep_dma_cyclic = bcm2835_dma_prep_dma_cyclic;
	od->ddev.device_prep_slave_sg = bcm2835_dma_prep_slave_sg;
	od->ddev.device_prep_dma_memcpy = bcm2835_dma_prep_dma_memcpy;
	od->ddev.device_config = bcm2835_dma_slave_config;
	od->ddev.device_terminate_all = bcm2835_dma_terminate_all;
	od->ddev.device_synchronize = bcm2835_dma_synchronize;
	od->ddev.src_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);
	od->ddev.dst_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);
	od->ddev.directions = BIT(DMA_DEV_TO_MEM) | BIT(DMA_MEM_TO_DEV) |
			      BIT(DMA_MEM_TO_MEM);
	od->ddev.residue_granularity = DMA_RESIDUE_GRANULARITY_BURST;
	od->ddev.descriptor_reuse = true;
	od->ddev.dev = &pdev->dev;
	INIT_LIST_HEAD(&od->ddev.channels);

	platform_set_drvdata(pdev, od);

	od->zero_page = dma_map_page_attrs(od->ddev.dev, ZERO_PAGE(0), 0,
					   PAGE_SIZE, DMA_TO_DEVICE,
					   DMA_ATTR_SKIP_CPU_SYNC);
	if (dma_mapping_error(od->ddev.dev, od->zero_page)) {
		dev_err(&pdev->dev, "Failed to map zero page\n");
		return -ENOMEM;
	}

	/* Request DMA channel mask from device tree */
	rc = of_property_read_u32(pdev->dev.of_node, "dma-channel-mask",
				  &chans_available);

	if (rc) {
		/* Try deprecated property */
		if (of_property_read_u32(pdev->dev.of_node,
					 "brcm,dma-channel-mask",
					 &chans_available)) {
			dev_err(&pdev->dev, "Failed to get channel mask\n");
			goto err_no_dma;
		}

		dev_warn(&pdev->dev, "Please update DT blob\n");
	}

	/*
	 * Never drive channels with a different programming model, which
	 * some device trees put in the same node.
	 */
	if (chans_available & ~od->cfg->chan_mask) {
		dev_warn(&pdev->dev, "ignoring unsupported channels %#x\n",
			 chans_available & ~od->cfg->chan_mask);
		chans_available &= od->cfg->chan_mask;
		if (!chans_available) {
			rc = -ENODEV;
			goto err_no_dma;
		}
	}

	for (i = 0; i <= BCM2835_DMA_MAX_DMA_CHAN_SUPPORTED; i++)
		irq[i] = -1;

	/* get irqs for each channel that we support */
	for (i = chan_start; i < chan_end; i++) {
		int index = i - chan_start;

		/* skip masked out channels */
		if (!(chans_available & (1 << i)))
			continue;

		/* get the named irq */
		snprintf(chan_name, sizeof(chan_name), "dma%i", i);
		irq[i] = platform_get_irq_byname(pdev, chan_name);
		if (irq[i] >= 0)
			continue;

		/* legacy device tree case handling */
		dev_warn_once(&pdev->dev,
			      "missing interrupt-names property in device tree - legacy interpretation is used\n");
		/*
		 * in case of channel >= 11
		 * use the 11th interrupt and that is shared
		 */
		irq[i] = platform_get_irq(pdev, index < 11 ? index : 11);
	}

	chan_count = 0;

	/* get irqs for each channel */
	for (i = chan_start; i < chan_end; i++) {
		/* skip channels without irq */
		if (irq[i] < 0)
			continue;

		/* check if there are other channels that also use this irq */
		irq_flags = 0;
		for (j = 0; j <= BCM2835_DMA_MAX_DMA_CHAN_SUPPORTED; j++)
			if (i != j && irq[j] == irq[i]) {
				irq_flags = IRQF_SHARED;
				break;
			}

		/* initialize the channel */
		rc = bcm2835_dma_chan_init(od, i, irq[i], irq_flags);
		if (rc)
			goto err_no_dma;

		chan_count++;
	}

	dev_dbg(&pdev->dev, "Initialized %i DMA channels\n", chan_count);

	/* Device-tree DMA controller registration */
	rc = of_dma_controller_register(pdev->dev.of_node,
					bcm2835_dma_xlate, od);
	if (rc) {
		dev_err(&pdev->dev, "Failed to register DMA controller\n");
		goto err_no_dma;
	}

	rc = dma_async_device_register(&od->ddev);
	if (rc) {
		dev_err(&pdev->dev,
			"Failed to register slave DMA engine device: %d\n", rc);
		goto err_no_dma;
	}

	dev_dbg(&pdev->dev, "Load BCM2835 DMA engine driver\n");

	return 0;

err_no_dma:
	bcm2835_dma_free(od);
	return rc;
}

static void bcm2835_dma_remove(struct platform_device *pdev)
{
	struct bcm2835_dmadev *od = platform_get_drvdata(pdev);

	dma_async_device_unregister(&od->ddev);
	bcm2835_dma_free(od);
}

static struct platform_driver bcm2835_dma_driver = {
	.probe	= bcm2835_dma_probe,
	.remove = bcm2835_dma_remove,
	.driver = {
		.name = "bcm2835-dma",
		.of_match_table = of_match_ptr(bcm2835_dma_of_match),
		.pm = pm_ptr(&bcm2835_dma_pm_ops),
	},
};

module_platform_driver(bcm2835_dma_driver);

MODULE_DESCRIPTION("BCM2835 DMA engine driver");
MODULE_AUTHOR("Florian Meier <florian.meier@koalo.de>");
MODULE_LICENSE("GPL");
