/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) */
#ifndef _NTB_EDMA_H_
#define _NTB_EDMA_H_

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/interrupt.h>

#define EDMA_REG_SIZE		SZ_64K
#define DMA_LLP_MEM_SIZE	SZ_4K
#define EDMA_WR_CH_NUM		4
#define EDMA_RD_CH_NUM		4
#define NTB_EDMA_MAX_CH		8

#define NTB_EDMA_INFO_MAGIC	0x45444D41 /* "EDMA" */
#define NTB_EDMA_INFO_OFF	EDMA_REG_SIZE

#define NTB_EDMA_RING_ORDER	7
#define NTB_EDMA_RING_ENTRIES	(1U << NTB_EDMA_RING_ORDER)
#define NTB_EDMA_RING_MASK	(NTB_EDMA_RING_ENTRIES - 1)

typedef void (*ntb_edma_interrupt_cb_t)(void *data, int qp_num);

/*
 * REMOTE_EDMA_EP:
 *   Endpoint owns the eDMA engine and pushes descriptors into a shared MW.
 *
 * REMOTE_EDMA_RC:
 *   Root Complex controls the endpoint eDMA through the shared MW and
 *   drives reads/writes on behalf of the host.
 */
typedef enum {
	REMOTE_EDMA_UNKNOWN,
	REMOTE_EDMA_EP,
	REMOTE_EDMA_RC,
} remote_edma_mode_t;

typedef enum {
	REMOTE_EDMA_WRITE,
	REMOTE_EDMA_READ,
} remote_edma_dir_t;

/*
 * Layout of remote eDMA MW (EP local address space, RC sees via peer MW):
 *
 *  0 .. EDMA_REG_SIZE-1        : DesignWare eDMA registers
 *  EDMA_REG_SIZE .. +PAGE_SIZE : struct ntb_edma_info (EP writes, RC reads)
 *  +PAGE_SIZE ..               : LL ring buffers (EP allocates phys addresses,
 *                                RC configures via dw_edma)
 *
 * ntb_edma_setup_mws() on EP:
 *   - allocates ntb_edma_info and LLs in EP memory
 *   - programs inbound iATU so that RC peer MW[n] points at this block
 *
 * ntb_edma_setup_peer() on RC:
 *   - ioremaps peer MW[n]
 *   - reads ntb_edma_info
 *   - sets up dw_edma_chip ll_region_* from that info
 */
struct ntb_edma_info {
	u32 magic;
	u16 wr_cnt;
	u16 rd_cnt;
	u64 regs_phys;
	u32 ll_stride;
	u32 rsvd;
	u64 ll_wr_phys[NTB_EDMA_MAX_CH];
	u64 ll_rd_phys[NTB_EDMA_MAX_CH];

	u64 intr_dar_base;
} __packed;

struct ll_dma_addrs {
	dma_addr_t wr[EDMA_WR_CH_NUM];
	dma_addr_t rd[EDMA_RD_CH_NUM];
};

struct ntb_edma_chans {
	struct device *dev;

	struct dma_chan *wr_chan[EDMA_WR_CH_NUM];
	struct dma_chan *rd_chan[EDMA_RD_CH_NUM];
	struct dma_chan *intr_chan;

	unsigned int num_wr_chan;
	unsigned int num_rd_chan;
	atomic_t cur_wr_chan;
	atomic_t cur_rd_chan;
};

static __always_inline u32 ntb_edma_ring_idx(u32 v)
{
	return v & NTB_EDMA_RING_MASK;
}

static __always_inline u32 ntb_edma_ring_used_entry(u32 head, u32 tail)
{
	if (head >= tail) {
		WARN_ON_ONCE((head - tail) > (NTB_EDMA_RING_ENTRIES - 1));
		return head - tail;
	}

	WARN_ON_ONCE((U32_MAX - tail + head + 1) > (NTB_EDMA_RING_ENTRIES - 1));
	return U32_MAX - tail + head + 1;
}

static __always_inline u32 ntb_edma_ring_free_entry(u32 head, u32 tail)
{
	return NTB_EDMA_RING_ENTRIES - ntb_edma_ring_used_entry(head, tail) - 1;
}

static __always_inline bool ntb_edma_ring_full(u32 head, u32 tail)
{
	return ntb_edma_ring_free_entry(head, tail) == 0;
}

int ntb_edma_setup_isr(struct device *dev, struct device *epc_dev,
		       ntb_edma_interrupt_cb_t cb, void *data);
void ntb_edma_teardown_isr(struct device *dev);
int ntb_edma_setup_mws(struct ntb_dev *ndev);
int ntb_edma_setup_peer(struct ntb_dev *ndev);
int ntb_edma_setup_chans(struct device *dma_dev, struct ntb_edma_chans *edma);
struct dma_chan *ntb_edma_pick_chan(struct ntb_edma_chans *edma,
				    remote_edma_dir_t dir);
void ntb_edma_teardown_chans(struct ntb_edma_chans *edma);
int ntb_edma_notify_peer(struct ntb_edma_chans *edma, int qp_num);

#endif
