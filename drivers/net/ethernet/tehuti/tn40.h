/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (c) Tehuti Networks Ltd. */

#ifndef _TN40_H_
#define _TN40_H_

#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/firmware.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/in.h>
#include <linux/interrupt.h>
#include <linux/ip.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/pci.h>
#include <linux/phy.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/vmalloc.h>
#include <linux/version.h>

#include "tn40_regs.h"

#define BDX_DRV_NAME "tn40xx"
#define BDX_DRV_VERSION "0.3.6.17.2"

#define PCI_VENDOR_ID_EDIMAX 0x1432

#define MDIO_SPEED_1MHZ (1)
#define MDIO_SPEED_6MHZ (6)

/* netdev tx queue len for Luxor. The default value is 1000.
 * ifconfig eth1 txqueuelen 3000 - to change it at runtime.
 */
#define BDX_NDEV_TXQ_LEN 3000

#define FIFO_SIZE 4096
#define FIFO_EXTRA_SPACE 1024

#if BITS_PER_LONG == 64
#define H32_64(x) ((u32)((u64)(x) >> 32))
#define L32_64(x) ((u32)((u64)(x) & 0xffffffff))
#elif BITS_PER_LONG == 32
#define H32_64(x) 0
#define L32_64(x) ((u32)(x))
#else /* BITS_PER_LONG == ?? */
#error BITS_PER_LONG is undefined. Must be 64 or 32
#endif /* BITS_PER_LONG */

#define BDX_TXF_DESC_SZ 16
#define BDX_MAX_TX_LEVEL (priv->txd_fifo0.m.memsz - 16)
#define BDX_MIN_TX_LEVEL 256
#define BDX_NO_UPD_PACKETS 40
#define BDX_MAX_MTU BIT(14)

#define PCK_TH_MULT 128
#define INT_COAL_MULT 2

#define BITS_MASK(nbits) ((1 << (nbits)) - 1)
#define GET_BITS_SHIFT(x, nbits, nshift) (((x) >> (nshift)) & BITS_MASK(nbits))
#define BITS_SHIFT_MASK(nbits, nshift) (BITS_MASK(nbits) << (nshift))
#define BITS_SHIFT_VAL(x, nbits, nshift) (((x) & BITS_MASK(nbits)) << (nshift))
#define BITS_SHIFT_CLEAR(x, nbits, nshift) \
	((x) & (~BITS_SHIFT_MASK(nbits, (nshift))))

#define GET_INT_COAL(x) GET_BITS_SHIFT(x, 15, 0)
#define GET_INT_COAL_RC(x) GET_BITS_SHIFT(x, 1, 15)
#define GET_RXF_TH(x) GET_BITS_SHIFT(x, 4, 16)
#define GET_PCK_TH(x) GET_BITS_SHIFT(x, 4, 20)

#define INT_REG_VAL(coal, coal_rc, rxf_th, pck_th) \
	((coal) | ((coal_rc) << 15) | ((rxf_th) << 16) | ((pck_th) << 20))

struct fifo {
	dma_addr_t da; /* Physical address of fifo (used by HW) */
	char *va; /* Virtual address of fifo (used by SW) */
	u32 rptr, wptr;
	 /* Cached values of RPTR and WPTR registers,
	  * they're 32 bits on both 32 and 64 archs.
	  */
	u16 reg_cfg0;
	u16 reg_cfg1;
	u16 reg_rptr;
	u16 reg_wptr;
	u16 memsz; /* Memory size allocated for fifo */
	u16 size_mask;
	u16 pktsz; /* Skb packet size to allocate */
	u16 rcvno; /* Number of buffers that come from this RXF */
};

struct txf_fifo {
	struct fifo m; /* The minimal set of variables used by all fifos */
};

struct txd_fifo {
	struct fifo m; /* The minimal set of variables used by all fifos */
};

struct rxf_fifo {
	struct fifo m; /* The minimal set of variables used by all fifos */
};

struct rxd_fifo {
	struct fifo m; /* The minimal set of variables used by all fifos */
};

struct bdx_page {
	struct page *page;
	u64 dma;
};

struct rx_map {
	struct bdx_page bdx_page;
	u64 dma;
	u32 off;
	u32 size; /* Mapped area (i.e. page) size */
};

struct rxdb {
	int *stack;
	struct rx_map *elems;
	int nelem;
	int top;
};

union bdx_dma_addr {
	dma_addr_t dma;
	struct sk_buff *skb;
};

/* Entry in the db.
 * if len == 0 addr is dma
 * if len != 0 addr is skb
 */
struct tx_map {
	union bdx_dma_addr addr;
	int len;
};

/* tx database - implemented as circular fifo buffer */
struct txdb {
	struct tx_map *start; /* Points to the first element */
	struct tx_map *end; /* Points just AFTER the last element */
	struct tx_map *rptr; /* Points to the next element to read */
	struct tx_map *wptr; /* Points to the next element to write */
	int size; /* Number of elements in the db */
};

struct bdx_rx_page_table {
	int page_size;
	int buf_size;
	struct bdx_page bdx_pages;
};

struct bdx_priv {
	struct net_device *ndev;
	struct pci_dev *pdev;

	struct napi_struct napi;
	/* RX FIFOs: 1 for data (full) descs, and 2 for free descs */
	struct rxd_fifo rxd_fifo0;
	struct rxf_fifo rxf_fifo0;
	struct rxdb *rxdb0; /* Rx dbs to store skb pointers */
	int napi_stop;
	struct vlan_group *vlgrp;
	/* Tx FIFOs: 1 for data desc, 1 for empty (acks) desc */
	struct txd_fifo txd_fifo0;
	struct txf_fifo txf_fifo0;
	struct txdb txdb;
	int tx_level;
	int tx_update_mark;
	int tx_noupd;

	int stats_flag;
	struct net_device_stats net_stats;

	u8 txd_size;
	u8 txf_size;
	u8 rxd_size;
	u8 rxf_size;
	u32 rdintcm;
	u32 tdintcm;

	u32 isr_mask;
	int link;
	u32 link_loop_cnt;

	void __iomem *regs;

	/* SHORT_PKT_FIX */
	u32 b0_len;
	dma_addr_t b0_dma; /* Physical address of buffer */
	char *b0_va; /* Virtual address of buffer */

	struct bdx_rx_page_table rx_page_table;

	struct mii_bus *mdio;
	struct phy_device *phydev;
};

/* RX FREE descriptor - 64bit */
struct rxf_desc {
	u32 info; /* Buffer Count + Info - described below */
	u32 va_lo; /* VAdr[31:0] */
	u32 va_hi; /* VAdr[63:32] */
	u32 pa_lo; /* PAdr[31:0] */
	u32 pa_hi; /* PAdr[63:32] */
	u32 len; /* Buffer Length */
};

#define GET_RXD_BC(x) GET_BITS_SHIFT((x), 5, 0)
#define GET_RXD_RXFQ(x) GET_BITS_SHIFT((x), 2, 8)
#define GET_RXD_TO(x) GET_BITS_SHIFT((x), 1, 15)
#define GET_RXD_TYPE(x) GET_BITS_SHIFT((x), 4, 16)
#define GET_RXD_ERR(x) GET_BITS_SHIFT((x), 6, 21)
#define GET_RXD_RXP(x) GET_BITS_SHIFT((x), 1, 27)
#define GET_RXD_PKT_ID(x) GET_BITS_SHIFT((x), 3, 28)
#define GET_RXD_VTAG(x) GET_BITS_SHIFT((x), 1, 31)
#define GET_RXD_VLAN_ID(x) GET_BITS_SHIFT((x), 12, 0)
#define GET_RXD_VLAN_TCI(x) GET_BITS_SHIFT((x), 16, 0)
#define GET_RXD_CFI(x) GET_BITS_SHIFT((x), 1, 12)
#define GET_RXD_PRIO(x) GET_BITS_SHIFT((x), 3, 13)

struct rxd_desc {
	u32 rxd_val1;
	u16 len;
	u16 rxd_vlan;
	u32 va_lo;
	u32 va_hi;
	u32 rss_lo;
	u32 rss_hash;
};

#define MAX_PBL (19)
/* PBL describes each virtual buffer to be transmitted from the host. */
struct pbl {
	u32 pa_lo;
	u32 pa_hi;
	u32 len;
};

/* First word for TXD descriptor. It means: type = 3 for regular Tx packet,
 * hw_csum = 7 for IP+UDP+TCP HW checksums.
 */
#define TXD_W1_VAL(bc, checksum, vtag, lgsnd, vlan_id)               \
	((bc) | ((checksum) << 5) | ((vtag) << 8) | ((lgsnd) << 9) | \
	 (0x30000) | ((vlan_id & 0x0fff) << 20) |                    \
	 (((vlan_id >> 13) & 7) << 13))

struct txd_desc {
	u32 txd_val1;
	u16 mss;
	u16 length;
	u32 va_lo;
	u32 va_hi;
	struct pbl pbl[0]; /* Fragments */
} __packed;

struct txf_desc {
	u32 status;
	u32 va_lo; /* VAdr[31:0] */
	u32 va_hi; /* VAdr[63:32] */
	u32 pad;
} __packed;

/* 32 bit kernels use 16 bits for page_offset. Do not increase
 * LUXOR__MAX_PAGE_SIZE beyind 64K!
 */
#if BITS_PER_LONG > 32
#define LUXOR__MAX_PAGE_SIZE 0x40000
#else
#define LUXOR__MAX_PAGE_SIZE 0x10000
#endif

static inline u32 read_reg(struct bdx_priv *priv, u32 reg)
{
	return readl(priv->regs + reg);
}

static inline void write_reg(struct bdx_priv *priv, u32 reg, u32 val)
{
	writel(val, priv->regs + reg);
}

int bdx_mdiobus_init(struct bdx_priv *priv);

#endif /* _TN40XX_H */
