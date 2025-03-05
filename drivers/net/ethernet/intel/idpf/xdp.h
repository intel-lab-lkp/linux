/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2024 Intel Corporation */

#ifndef _IDPF_XDP_H_
#define _IDPF_XDP_H_

#include <net/libeth/xdp.h>

#include "idpf_txrx.h"

int idpf_xdp_rxq_info_init_all(const struct idpf_vport *vport);
void idpf_xdp_rxq_info_deinit_all(const struct idpf_vport *vport);
void idpf_copy_xdp_prog_to_qs(const struct idpf_vport *vport,
			      struct bpf_prog *xdp_prog);

int idpf_vport_xdpq_get(const struct idpf_vport *vport);
void idpf_vport_xdpq_put(const struct idpf_vport *vport);

bool idpf_xdp_tx_flush_bulk(struct libeth_xdp_tx_bulk *bq, u32 flags);

/**
 * idpf_xdp_tx_xmit - produce a single HW Tx descriptor out of XDP desc
 * @desc: XDP descriptor to pull the DMA address and length from
 * @i: descriptor index on the queue to fill
 * @sq: XDP queue to produce the HW Tx descriptor on
 * @priv: &xsk_tx_metadata_ops on XSk xmit or %NULL
 */
static inline void idpf_xdp_tx_xmit(struct libeth_xdp_tx_desc desc, u32 i,
				    const struct libeth_xdpsq *sq, u64 priv)
{
	struct idpf_flex_tx_desc *tx_desc = sq->descs;
	u32 cmd;

	cmd = FIELD_PREP(IDPF_FLEX_TXD_QW1_DTYPE_M,
			 IDPF_TX_DESC_DTYPE_FLEX_L2TAG1_L2TAG2);
	if (desc.flags & LIBETH_XDP_TX_LAST)
		cmd |= FIELD_PREP(IDPF_FLEX_TXD_QW1_CMD_M,
				  IDPF_TX_DESC_CMD_EOP);
	if (priv && (desc.flags & LIBETH_XDP_TX_CSUM))
		cmd |= FIELD_PREP(IDPF_FLEX_TXD_QW1_CMD_M,
				  IDPF_TX_FLEX_DESC_CMD_CS_EN);

	tx_desc = &tx_desc[i];
	tx_desc->buf_addr = cpu_to_le64(desc.addr);
#ifdef __LIBETH_WORD_ACCESS
	*(u64 *)&tx_desc->qw1 = ((u64)desc.len << 48) | cmd;
#else
	tx_desc->qw1.buf_size = cpu_to_le16(desc.len);
	tx_desc->qw1.cmd_dtype = cpu_to_le16(cmd);
#endif
}

/**
 * idpf_set_rs_bit - set RS bit on last produced descriptor
 * @xdpq: XDP queue to produce the HW Tx descriptors on
 */
static inline void idpf_set_rs_bit(const struct idpf_tx_queue *xdpq)
{
	u32 ntu, cmd;

	ntu = xdpq->next_to_use;
	if (unlikely(!ntu))
		ntu = xdpq->desc_count;

	cmd = FIELD_PREP(IDPF_FLEX_TXD_QW1_CMD_M, IDPF_TX_DESC_CMD_RS);
#ifdef __LIBETH_WORD_ACCESS
	*(u64 *)&xdpq->flex_tx[ntu - 1].q.qw1 |= cmd;
#else
	xdpq->flex_tx[ntu - 1].q.qw1.cmd_dtype |= cpu_to_le16(cmd);
#endif
}

/**
 * idpf_xdpq_update_tail - update the XDP Tx queue tail register
 * @xdpq: XDP Tx queue
 */
static inline void idpf_xdpq_update_tail(const struct idpf_tx_queue *xdpq)
{
	dma_wmb();
	writel_relaxed(xdpq->next_to_use, xdpq->tail);
}

/**
 * idpf_xdp_tx_finalize - Update RS bit and bump XDP Tx tail
 * @_xdpq: XDP Tx queue
 * @sent: whether any frames were sent
 * @flush: whether to update RS bit and the tail register
 *
 * This function bumps XDP Tx tail and should be called when a batch of packets
 * has been processed in the napi loop.
 */
static inline void idpf_xdp_tx_finalize(void *_xdpq, bool sent, bool flush)
{
	struct idpf_tx_queue *xdpq = _xdpq;

	if ((!flush || unlikely(!sent)) &&
	    likely(xdpq->desc_count != xdpq->pending))
		return;

	libeth_xdpsq_lock(&xdpq->xdp_lock);

	idpf_set_rs_bit(xdpq);
	idpf_xdpq_update_tail(xdpq);

	libeth_xdpsq_queue_timer(xdpq->timer);

	libeth_xdpsq_unlock(&xdpq->xdp_lock);
}

struct idpf_xdp_rx_desc {
	aligned_u64		qw0;
#define IDPF_XDP_RX_BUFQ	BIT_ULL(47)
#define IDPF_XDP_RX_GEN		BIT_ULL(46)
#define IDPF_XDP_RX_LEN		GENMASK_ULL(45, 32)
#define IDPF_XDP_RX_PT		GENMASK_ULL(25, 16)

	aligned_u64		qw1;
#define IDPF_XDP_RX_BUF		GENMASK_ULL(47, 32)
#define IDPF_XDP_RX_EOP		BIT_ULL(1)

	aligned_u64		qw2;
#define IDPF_XDP_RX_HASH	GENMASK_ULL(31, 0)

	aligned_u64		qw3;
} __aligned(4 * sizeof(u64));
static_assert(sizeof(struct idpf_xdp_rx_desc) ==
	      sizeof(struct virtchnl2_rx_flex_desc_adv_nic_3));

#define idpf_xdp_rx_bufq(desc)	!!((desc)->qw0 & IDPF_XDP_RX_BUFQ)
#define idpf_xdp_rx_gen(desc)	!!((desc)->qw0 & IDPF_XDP_RX_GEN)
#define idpf_xdp_rx_len(desc)	FIELD_GET(IDPF_XDP_RX_LEN, (desc)->qw0)
#define idpf_xdp_rx_pt(desc)	FIELD_GET(IDPF_XDP_RX_PT, (desc)->qw0)
#define idpf_xdp_rx_buf(desc)	FIELD_GET(IDPF_XDP_RX_BUF, (desc)->qw1)
#define idpf_xdp_rx_eop(desc)	!!((desc)->qw1 & IDPF_XDP_RX_EOP)
#define idpf_xdp_rx_hash(desc)	FIELD_GET(IDPF_XDP_RX_HASH, (desc)->qw2)

static inline void
idpf_xdp_get_qw0(struct idpf_xdp_rx_desc *desc,
		 const struct virtchnl2_rx_flex_desc_adv_nic_3 *rxd)
{
#ifdef __LIBETH_WORD_ACCESS
	desc->qw0 = ((const typeof(desc))rxd)->qw0;
#else
	desc->qw0 = ((u64)le16_to_cpu(rxd->pktlen_gen_bufq_id) << 32) |
		    ((u64)le16_to_cpu(rxd->ptype_err_fflags0) << 16);
#endif
}

static inline void
idpf_xdp_get_qw1(struct idpf_xdp_rx_desc *desc,
		 const struct virtchnl2_rx_flex_desc_adv_nic_3 *rxd)
{
#ifdef __LIBETH_WORD_ACCESS
	desc->qw1 = ((const typeof(desc))rxd)->qw1;
#else
	desc->qw1 = ((u64)le16_to_cpu(rxd->buf_id) << 32) |
		    rxd->status_err0_qw1;
#endif
}

static inline void
idpf_xdp_get_qw2(struct idpf_xdp_rx_desc *desc,
		 const struct virtchnl2_rx_flex_desc_adv_nic_3 *rxd)
{
#ifdef __LIBETH_WORD_ACCESS
	desc->qw2 = ((const typeof(desc))rxd)->qw2;
#else
	desc->qw2 = ((u64)rxd->hash3 << 24) |
		    ((u64)rxd->ff2_mirrid_hash2.hash2 << 16) |
		    le16_to_cpu(rxd->hash1);
#endif
}

void idpf_xdp_set_features(const struct idpf_vport *vport);

int idpf_xdp(struct net_device *dev, struct netdev_bpf *xdp);
int idpf_xdp_xmit(struct net_device *dev, int n, struct xdp_frame **frames,
		  u32 flags);

#endif /* _IDPF_XDP_H_ */
