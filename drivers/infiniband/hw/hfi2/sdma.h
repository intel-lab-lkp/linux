/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/*
 * Copyright(c) 2015 - 2018 Intel Corporation.
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */

#ifndef _HFI2_SDMA_H
#define _HFI2_SDMA_H

#include <linux/types.h>
#include <linux/list.h>
#include <asm/byteorder.h>
#include <linux/workqueue.h>
#include <linux/rculist.h>

#include "sdma_defs.h"
#include "hfi2.h"
#include "verbs.h"
#include "sdma_txreq.h"

static inline bool wfr_sdma_qw_get_first_desc(u64 *qw)
{
	return !!(qw[0] & WFR_SDMA_DESC0_FIRST_DESC_FLAG);
}

static inline void wfr_sdma_qw_set_first_desc(u64 *qw)
{
	qw[0] |= WFR_SDMA_DESC0_FIRST_DESC_FLAG;
}

static inline bool wfr_sdma_qw_get_last_desc(u64 *qw)
{
	return !!(qw[0] & WFR_SDMA_DESC0_LAST_DESC_FLAG);
}

static inline void wfr_sdma_qw_set_last_desc(u64 *qw)
{
	qw[0] |= WFR_SDMA_DESC0_LAST_DESC_FLAG;
}

static inline u32 wfr_sdma_qw_get_byte_count(u64 *qw)
{
	return (qw[0] >> WFR_SDMA_DESC0_BYTE_COUNT_SHIFT) &
	       WFR_SDMA_DESC0_BYTE_COUNT_MASK;
}

/* assumes starting field is zero */
static inline void wfr_sdma_qw_set_byte_count(u64 *qw, u32 bytes)
{
	qw[0] |= ((u64)bytes & WFR_SDMA_DESC0_BYTE_COUNT_MASK)
		 << WFR_SDMA_DESC0_BYTE_COUNT_SHIFT;
}

static inline u64 wfr_sdma_qw_get_phy_addr(u64 *qw)
{
	return (qw[0] >> WFR_SDMA_DESC0_PHY_ADDR_SHIFT) &
	       WFR_SDMA_DESC0_PHY_ADDR_MASK;
}

/* assumes starting field is zero */
static inline void wfr_sdma_qw_set_phy_addr(u64 *qw, u64 phy_addr)
{
	qw[0] |= (phy_addr & WFR_SDMA_DESC0_PHY_ADDR_MASK)
		 << WFR_SDMA_DESC0_PHY_ADDR_SHIFT;
}

static inline bool jkr_sdma_qw_get_first_desc(u64 *qw)
{
	return !!(qw[1] & JKR_SDMA_DESC1_FIRST_DESC_FLAG);
}

static inline void jkr_sdma_qw_set_first_desc(u64 *qw)
{
	qw[1] |= JKR_SDMA_DESC1_FIRST_DESC_FLAG;
}

static inline bool jkr_sdma_qw_get_last_desc(u64 *qw)
{
	return !!(qw[1] & JKR_SDMA_DESC1_LAST_DESC_FLAG);
}

static inline void jkr_sdma_qw_set_last_desc(u64 *qw)
{
	qw[1] |= JKR_SDMA_DESC1_LAST_DESC_FLAG;
}

static inline u32 jkr_sdma_qw_get_byte_count(u64 *qw)
{
	return (qw[1] >> JKR_SDMA_DESC1_BYTE_COUNT_SHIFT) &
	       JKR_SDMA_DESC1_BYTE_COUNT_MASK;
}

/* assumes starting field is zero */
static inline void jkr_sdma_qw_set_byte_count(u64 *qw, u32 bytes)
{
	qw[1] |= ((u64)bytes & JKR_SDMA_DESC1_BYTE_COUNT_MASK)
		 << JKR_SDMA_DESC1_BYTE_COUNT_SHIFT;
}

static inline u64 jkr_sdma_qw_get_phy_addr(u64 *qw)
{
	return (qw[0] >> JKR_SDMA_DESC0_PHY_ADDR_SHIFT) &
	       JKR_SDMA_DESC0_PHY_ADDR_MASK;
}

/* assumes starting field is zero */
static inline void jkr_sdma_qw_set_phy_addr(u64 *qw, u64 phy_addr)
{
	qw[0] |= (phy_addr & JKR_SDMA_DESC0_PHY_ADDR_MASK)
		 << JKR_SDMA_DESC0_PHY_ADDR_SHIFT;
}

/* Per-chip setter inlining wrapper */
#define sdma_qw_set(dd, field, ...)                              \
	do {                                                     \
		if ((dd)->params->chip_type == CHIP_WFR)         \
			wfr_sdma_qw_set_##field(__VA_ARGS__);    \
		else if ((dd)->params->chip_type == CHIP_JKR)    \
			jkr_sdma_qw_set_##field(__VA_ARGS__);    \
		else                                             \
			WARN_ONCE(1, "Unsupported chip type %u", \
				  (dd)->params->chip_type);      \
	} while (0)

/* Per-chip getter inlining wrapper */
#define sdma_qw_get(dd, fn, p)                          \
	((dd)->params->chip_type == CHIP_WFR ?          \
		 wfr_sdma_qw_get_##fn((p)) :            \
		 ((dd)->params->chip_type == CHIP_JKR ? \
			  jkr_sdma_qw_get_##fn((p)) :   \
			  0))

enum sdma_states {
	sdma_state_s00_hw_down,
	sdma_state_s10_hw_start_up_halt_wait,
	sdma_state_s15_hw_start_up_clean_wait,
	sdma_state_s20_idle,
	sdma_state_s30_sw_clean_up_wait,
	sdma_state_s40_hw_clean_up_wait,
	sdma_state_s50_hw_halt_wait,
	sdma_state_s60_idle_halt_wait,
	sdma_state_s80_hw_freeze,
	sdma_state_s82_freeze_sw_clean,
	sdma_state_s99_running,
};

enum sdma_events {
	sdma_event_e00_go_hw_down,
	sdma_event_e10_go_hw_start,
	sdma_event_e15_hw_halt_done,
	sdma_event_e25_hw_clean_up_done,
	sdma_event_e30_go_running,
	sdma_event_e40_sw_cleaned,
	sdma_event_e50_hw_cleaned,
	sdma_event_e60_hw_halted,
	sdma_event_e70_go_idle,
	sdma_event_e80_hw_freeze,
	sdma_event_e81_hw_frozen,
	sdma_event_e82_hw_unfreeze,
	sdma_event_e85_link_down,
	sdma_event_e90_sw_halted,
};

struct sdma_set_state_action {
	unsigned op_enable : 1;
	unsigned op_intenable : 1;
	unsigned op_halt : 1;
	unsigned op_cleanup : 1;
	unsigned go_s99_running_tofalse : 1;
	unsigned go_s99_running_totrue : 1;
};

struct sdma_state {
	struct kref kref;
	struct completion comp;
	enum sdma_states current_state;
	unsigned int current_op;
	unsigned int go_s99_running;
	/* debugging/development */
	enum sdma_states previous_state;
	unsigned int previous_op;
	enum sdma_events last_event;
};

/**
 * DOC: sdma exported routines
 *
 * These sdma routines fit into three categories:
 * - The SDMA API for building and submitting packets
 *   to the ring
 *
 * - Initialization and tear down routines to buildup
 *   and tear down SDMA
 *
 * - ISR entrances to handle interrupts, state changes
 *   and errors
 */

/**
 * DOC: sdma PSM/verbs API
 *
 * The sdma API is designed to be used by both PSM
 * and verbs to supply packets to the SDMA ring.
 *
 * The usage of the API is as follows:
 *
 * Embed a struct iowait in the QP or
 * PQ.  The iowait should be initialized with a
 * call to hfi2_iowait_init().
 *
 * The user of the API should create an allocation method
 * for their version of the txreq. slabs, pre-allocated lists,
 * and dma pools can be used.  Once the user's overload of
 * the sdma_txreq has been allocated, the sdma_txreq member
 * must be initialized with sdma_txinit() or sdma_txinit_ahg().
 *
 * The txreq must be declared with the sdma_txreq first.
 *
 * The tx request, once initialized,  is manipulated with calls to
 * sdma_txadd_daddr(), sdma_txadd_page(), or sdma_txadd_kvaddr()
 * for each disjoint memory location.  It is the user's responsibility
 * to understand the packet boundaries and page boundaries to do the
 * appropriate number of sdma_txadd_* calls..  The user
 * must be prepared to deal with failures from these routines due to
 * either memory allocation or dma_mapping failures.
 *
 * The mapping specifics for each memory location are recorded
 * in the tx. Memory locations added with sdma_txadd_page()
 * and sdma_txadd_kvaddr() are automatically mapped when added
 * to the tx and nmapped as part of the progress processing in the
 * SDMA interrupt handling.
 *
 * sdma_txadd_daddr() is used to add an dma_addr_t memory to the
 * tx.   An example of a use case would be a pre-allocated
 * set of headers allocated via dma_pool_alloc() or
 * dma_alloc_coherent().  For these memory locations, it
 * is the responsibility of the user to handle that unmapping.
 * (This would usually be at an unload or job termination.)
 *
 * The routine hfi2_sdma_send_txreq() is used to submit
 * a tx to the ring after the appropriate number of
 * sdma_txadd_* have been done.
 *
 * If it is desired to send a burst of sdma_txreqs, hfi2_sdma_send_txlist()
 * can be used to submit a list of packets.
 *
 * The user is free to use the link overhead in the struct sdma_txreq as
 * long as the tx isn't in flight.
 *
 * The extreme degenerate case of the number of descriptors
 * exceeding the ring size is automatically handled as
 * memory locations are added.  An overflow of the descriptor
 * array that is part of the sdma_txreq is also automatically
 * handled.
 *
 */

/**
 * DOC: Infrastructure calls
 *
 * hfi2_sdma_init() is used to initialize data structures and
 * CSRs for the desired number of SDMA engines.
 *
 * hfi2_sdma_start() is used to kick the SDMA engines initialized
 * with hfi2_sdma_init().   Interrupts must be enabled at this
 * point since aspects of the state machine are interrupt
 * driven.
 *
 * hfi2_sdma_engine_error() and hfi2_sdma_engine_interrupt() are
 * entrances for interrupts.
 *
 * hfi2_sdma_map_init() is for the management of the mapping
 * table when the number of vls is changed.
 *
 */

/*
 * struct hw_sdma_desc - raw 128 bit SDMA descriptor
 *
 * This is the raw descriptor in the SDMA ring
 */
struct hw_sdma_desc {
	/* private:  don't use directly */
	__le64 qw[2];
};

/**
 * struct sdma_engine - Data pertaining to each SDMA engine.
 * @dd: a back-pointer to the device data
 * @ppd: per port back-pointer
 * @imask: mask for irq manipulation
 * @idle_mask: mask for determining if an interrupt is due to sdma_idle
 *
 * This structure has the state for each sdma_engine.
 *
 * Accessing to non public fields are not supported
 * since the private members are subject to change.
 */
struct sdma_engine {
	/* read mostly */
	struct hfi2_devdata *dd;
	/* private: */
	void __iomem *tail_csr;
	u64 imask; /* clear interrupt mask */
	u64 idle_mask;
	u64 progress_mask;
	u64 int_mask;
	/* private: */
	volatile __le64 *head_dma; /* DMA'ed by chip */
	/* private: */
	dma_addr_t head_phys;
	/* private: */
	struct hw_sdma_desc *descq;
	/* private: */
	unsigned int descq_full_count;
	struct sdma_txreq **tx_ring;
	/* private: */
	dma_addr_t descq_phys;
	/* private */
	u32 sdma_mask;
	/* private */
	struct sdma_state state;
	/* private */
	int cpu;
	/* private: */
	u8 sdma_shift;
	/* private: */
	u8 this_idx; /* zero relative engine */
	/* protect changes to senddmactrl shadow */
	spinlock_t senddmactrl_lock;
	/* private: */
	u64 p_senddmactrl; /* shadow per-engine SendDmaCtrl */

	/* read/write using tail_lock */
	spinlock_t tail_lock ____cacheline_aligned_in_smp;
#ifdef CONFIG_HFI2_DEBUG_SDMA_ORDER
	/* private: */
	u64 tail_sn;
#endif
	/* private: */
	u32 descq_tail;
	/* private: */
	unsigned long ahg_bits;
	/* private: */
	u16 desc_avail;
	/* private: */
	u16 tx_tail;
	/* private: */
	u16 descq_cnt;

	/* read/write using head_lock */
	/* private: */
	seqlock_t head_lock ____cacheline_aligned_in_smp;
#ifdef CONFIG_HFI2_DEBUG_SDMA_ORDER
	/* private: */
	u64 head_sn;
#endif
	/* private: */
	u32 descq_head;
	/* private: */
	u16 tx_head;
	/* private: */
	u64 last_status;
	/* private */
	u64 err_cnt;
	/* private */
	u64 sdma_int_cnt;
	u64 idle_int_cnt;
	u64 progress_int_cnt;

	/* private: */
	seqlock_t waitlock;
	struct list_head dmawait;

	/* CONFIG SDMA for now, just blindly duplicate */
	/* private: */
	struct work_struct sdma_hw_clean_up_work;
	struct work_struct sdma_sw_clean_up_work;

	/* private: */
	struct work_struct err_halt_worker;
	/* private */
	struct timer_list err_progress_check_timer;
	u32 progress_check_head;
	/* private: */
	struct work_struct flush_worker;
	/* protect flush list */
	spinlock_t flushlist_lock;
	/* private: */
	struct list_head flushlist;
	struct cpumask cpu_mask;
	struct kobject kobj;
	u32 msix_intr;
};

int hfi2_sdma_init(struct hfi2_devdata *dd);
void hfi2_sdma_start(struct hfi2_devdata *dd);
void hfi2_sdma_exit(struct hfi2_devdata *dd);
void hfi2_sdma_clean(struct hfi2_devdata *dd);
void hfi2_sdma_all_running(struct hfi2_devdata *dd);
void hfi2_sdma_freeze_notify(struct hfi2_devdata *dd, int go_idle);
void hfi2_sdma_freeze(struct hfi2_devdata *dd);
void hfi2_sdma_unfreeze(struct hfi2_devdata *dd);
void hfi2_sdma_wait(struct hfi2_devdata *dd);

/**
 * sdma_empty() - idle engine test
 * @engine: sdma engine
 *
 * Currently used by verbs as a latency optimization.
 *
 * Return:
 * 1 - empty, 0 - non-empty
 */
static inline int sdma_empty(struct sdma_engine *sde)
{
	return sde->descq_tail == sde->descq_head;
}

/*
 * Return the number of descriptors in use.  Expects descq_cnt is a
 * power-of-two.  See hfi2_sdma_get_descq_cnt().
 */
static inline u16 sdma_descq_inprocess(struct sdma_engine *sde)
{
	return (sde->descq_cnt + sde->descq_tail - READ_ONCE(sde->descq_head)) &
	       (sde->descq_cnt - 1);
}

/* return the number of descriptors available */
static inline u16 sdma_descq_freecnt(struct sdma_engine *sde)
{
	return sde->descq_cnt - 1 - sdma_descq_inprocess(sde);
}

/*
 * Either head_lock or tail lock required to see
 * a steady state.
 */
static inline int __sdma_running(struct sdma_engine *engine)
{
	return engine->state.current_state == sdma_state_s99_running;
}

/**
 * sdma_running() - state suitability test
 * @engine: sdma engine
 *
 * sdma_running probes the internal state to determine if it is suitable
 * for submitting packets.
 *
 * Return:
 * 1 - ok to submit, 0 - not ok to submit
 *
 */
static inline int sdma_running(struct sdma_engine *engine)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&engine->tail_lock, flags);
	ret = __sdma_running(engine);
	spin_unlock_irqrestore(&engine->tail_lock, flags);
	return ret;
}

void _hfi2_sdma_txreq_ahgadd(struct sdma_txreq *tx, u8 num_ahg, u8 ahg_entry,
			     u32 *ahg, u8 ahg_hlen);

/*
 * Padding is needed if the data is not a multiple of 4 bytes.  This will
 * only possibly be true for 9B packets. Non QWORD aligned 16B packets
 * will have already been dropped.
 */
static inline int needs_pad(u16 packet_len)
{
	return (packet_len & 0x3) != 0;
}

/* return the number of bytes needed to pad to a multiple of 4 */
static inline int pad_length(struct sdma_txreq *tx)
{
	return (4 - (tx->packet_len & 0x3)) & 0x3;
}

#define ALIGN_NONE 0
#define ALIGN_256_ALL 1
#define ALIGN_256_HEAD_TAIL 2
#define ALIGN_256_TAIL 3

/* return the max number of descriptors a segment might need */
static inline int calc_num_desc(struct hfi2_devdata *dd, u16 packet_len)
{
	if (dd->sdma_align == ALIGN_256_HEAD_TAIL)
		return 3; /* head, mid, tail */
	if (dd->sdma_align == ALIGN_NONE)
		return 1; /* all */
	if (dd->sdma_align == ALIGN_256_TAIL)
		return 2; /* head+mid, tail */
	/*
	 * Conservative ALIGN_ALL case - split on every 256 byte fetch
	 * boundary.  The returned number must be the same for the whole
	 * packet - not just the current segment.
	 *
	 * A 10240 sized packet could have at most 41 descriptors.
	 */
	return (round_up(packet_len, 256) >> 8) + 1;
}

/**
 * sdma_txinit_ahg() - initialize an sdma_txreq struct with AHG
 * @dd: device data
 * @tx: tx request to initialize
 * @flags: flags to key last descriptor additions
 * @tlen: total packet length (pbc + headers + data)
 * @ahg_entry: ahg entry to use  (0 - 31)
 * @num_ahg: ahg descriptor for first descriptor (0 - 9)
 * @ahg: array of AHG descriptors (up to 9 entries)
 * @ahg_hlen: number of bytes from ASIC entry to use
 * @cb: callback
 *
 * The allocation of the sdma_txreq and it enclosing structure is user
 * dependent.  This routine must be called to initialize the user independent
 * fields.
 *
 * The currently supported flags are SDMA_TXREQ_F_URGENT,
 * SDMA_TXREQ_F_AHG_COPY, and SDMA_TXREQ_F_USE_AHG.
 *
 * SDMA_TXREQ_F_URGENT is used for latency sensitive situations where the
 * completion is desired as soon as possible.
 *
 * SDMA_TXREQ_F_AHG_COPY causes the header in the first descriptor to be
 * copied to chip entry. SDMA_TXREQ_F_USE_AHG causes the code to add in
 * the AHG descriptors into the first 1 to 3 descriptors.
 *
 * Completions of submitted requests can be gotten on selected
 * txreqs by giving a completion routine callback to sdma_txinit() or
 * sdma_txinit_ahg().  The environment in which the callback runs
 * can be from an ISR, a tasklet, or a thread, so no sleeping
 * kernel routines can be used.   Aspects of the sdma ring may
 * be locked so care should be taken with locking.
 *
 * The callback pointer can be NULL to avoid any callback for the packet
 * being submitted. The callback will be provided this tx, a status, and a flag.
 *
 * The status will be one of SDMA_TXREQ_S_OK, SDMA_TXREQ_S_SENDERROR,
 * SDMA_TXREQ_S_ABORTED, or SDMA_TXREQ_S_SHUTDOWN.
 *
 * The flag, if the is the iowait had been used, indicates the iowait
 * sdma_busy count has reached zero.
 *
 * user data portion of tlen should be precise.   The sdma_txadd_* entrances
 * will pad with a descriptor references 1 - 3 bytes when the number of bytes
 * specified in tlen have been supplied to the sdma_txreq.
 *
 * ahg_hlen is used to determine the number of on-chip entry bytes to
 * use as the header.   This is for cases where the stored header is
 * larger than the header to be used in a packet.  This is typical
 * for verbs where an RDMA_WRITE_FIRST is larger than the packet in
 * and RDMA_WRITE_MIDDLE.
 *
 */
static inline int sdma_txinit_ahg(struct hfi2_devdata *dd,
				  struct sdma_txreq *tx, u16 flags, u16 tlen,
				  u8 ahg_entry, u8 num_ahg, u32 *ahg,
				  u8 ahg_hlen,
				  void (*cb)(struct sdma_txreq *, int))
{
	if (tlen == 0)
		return -ENODATA;
	if (tlen > MAX_SDMA_PKT_SIZE)
		return -EMSGSIZE;
	tx->desc_limit = ARRAY_SIZE(tx->descs);
	tx->descp = &tx->descs[0];
	INIT_LIST_HEAD(&tx->list);
	tx->num_desc = 0;
	tx->flags = flags;
	tx->complete = cb;
	tx->coalesce_buf = NULL;
	tx->wait = NULL;
	tx->packet_len = tlen;
	tx->tlen = tx->packet_len;
	tx->desc_margin =
		calc_num_desc(dd, tx->packet_len) + needs_pad(tx->packet_len);
	tx->descs[0].qw[0] = 0;
	tx->descs[0].qw[1] = 0;
	/*
	 * Do not bother with initializing .map_type; sdma_set_map_type()
	 * should overwrite per-desc bits. sdma_get_map_type() should not be
	 * called for per-desc bits that haven't been set with
	 * sdma_set_map_type().
	 */
	sdma_qw_set(dd, first_desc, tx->descs[0].qw);
	if (flags & SDMA_TXREQ_F_AHG_COPY)
		tx->descs[0].qw[1] |=
			(((u64)ahg_entry & SDMA_DESC1_HEADER_INDEX_MASK)
			 << SDMA_DESC1_HEADER_INDEX_SHIFT) |
			(((u64)SDMA_AHG_COPY & SDMA_DESC1_HEADER_MODE_MASK)
			 << SDMA_DESC1_HEADER_MODE_SHIFT);
	else if (flags & SDMA_TXREQ_F_USE_AHG && num_ahg)
		_hfi2_sdma_txreq_ahgadd(tx, num_ahg, ahg_entry, ahg, ahg_hlen);
	return 0;
}

/**
 * sdma_txinit() - initialize an sdma_txreq struct (no AHG)
 * @dd: device data
 * @tx: tx request to initialize
 * @flags: flags to key last descriptor additions
 * @tlen: total packet length (pbc + headers + data)
 * @cb: callback pointer
 *
 * The allocation of the sdma_txreq and it enclosing structure is user
 * dependent.  This routine must be called to initialize the user
 * independent fields.
 *
 * The currently supported flags is SDMA_TXREQ_F_URGENT.
 *
 * SDMA_TXREQ_F_URGENT is used for latency sensitive situations where the
 * completion is desired as soon as possible.
 *
 * Completions of submitted requests can be gotten on selected
 * txreqs by giving a completion routine callback to sdma_txinit() or
 * sdma_txinit_ahg().  The environment in which the callback runs
 * can be from an ISR, a tasklet, or a thread, so no sleeping
 * kernel routines can be used.   The head size of the sdma ring may
 * be locked so care should be taken with locking.
 *
 * The callback pointer can be NULL to avoid any callback for the packet
 * being submitted.
 *
 * The callback, if non-NULL,  will be provided this tx and a status.  The
 * status will be one of SDMA_TXREQ_S_OK, SDMA_TXREQ_S_SENDERROR,
 * SDMA_TXREQ_S_ABORTED, or SDMA_TXREQ_S_SHUTDOWN.
 *
 */
static inline int sdma_txinit(struct hfi2_devdata *dd, struct sdma_txreq *tx,
			      u16 flags, u16 tlen,
			      void (*cb)(struct sdma_txreq *, int))
{
	return sdma_txinit_ahg(dd, tx, flags, tlen, 0, 0, NULL, 0, cb);
}

static inline size_t sdma_mapping_len(struct hfi2_devdata *dd,
				      struct sdma_desc *d)
{
	return sdma_qw_get(dd, byte_count, d->qw);
}

static inline dma_addr_t sdma_mapping_addr(struct hfi2_devdata *dd,
					   struct sdma_desc *d)
{
	return sdma_qw_get(dd, phy_addr, d->qw);
}

static inline void sdma_set_map_type(struct sdma_txreq *tx, u8 i, u8 type)
{
	int w = BIT_WORD(i * SDMA_MAP_BITS);
	int offset = (i * SDMA_MAP_BITS) % BITS_PER_LONG;
	unsigned long shftmask = SDMA_MAP_MASK << offset;
	unsigned long *mw = &tx->map_type[w];
	unsigned long new = (long)type << offset;

	/* Per-desc SDMA_MAP_BITS-sized field must never cross word boundary */
	static_assert(BITS_PER_LONG % SDMA_MAP_BITS == 0);
	bitmap_replace(mw, mw, &new, &shftmask, SDMA_MAP_BITS);
}

static inline u8 sdma_get_map_type(struct sdma_txreq *tx, u8 i)
{
	int idx = i * SDMA_MAP_BITS;

	/* Per-desc field must always fit in u8 */
	static_assert(SDMA_MAP_BITS <= 8);
	return bitmap_get_value8(tx->map_type, idx) & SDMA_MAP_MASK;
}

/* do the work, caller is responsible for all checking */
static inline void _make_tx_sdma_desc(struct hfi2_devdata *dd,
				      struct sdma_txreq *tx, int type,
				      dma_addr_t addr, size_t len)
{
	struct sdma_desc *desc;

	if (WARN_ON_ONCE(tx->num_desc >= tx->desc_limit))
		return;
	desc = &tx->descp[tx->num_desc];

	sdma_set_map_type(tx, tx->num_desc, type);
	if (!tx->num_desc) {
		/* qw[0] zero; qw[1] first, ahg mode already in from init */
	} else {
		desc->qw[0] = 0;
		desc->qw[1] = 0;
	}

	sdma_qw_set(dd, phy_addr, desc->qw, addr);
	sdma_qw_set(dd, byte_count, desc->qw, len);
	tx->num_desc++;
}

/*
 * Create one or more descriptors to fetch len bytes.  Enough descriptor room
 * is guaranteed by the time this function is called.
 */
static inline void make_tx_sdma_desc(struct hfi2_devdata *dd,
				     struct sdma_txreq *tx, int type,
				     dma_addr_t addr, size_t len)
{
#define ALIGN_SIZE 256
#define ALIGN_MASK (ALIGN_SIZE - 1)
	switch (dd->sdma_align) {
	case ALIGN_256_ALL:
		/* align head */
		if (addr & ALIGN_MASK) {
			size_t clen = ALIGN_SIZE - (addr & ALIGN_MASK);

			if (clen > len)
				clen = len;
			_make_tx_sdma_desc(dd, tx, type, addr, clen);
			len -= clen;
			addr += clen;
		}
		/* now aligned, split into full sized chunks */
		while (len >= ALIGN_SIZE) {
			_make_tx_sdma_desc(dd, tx, type, addr, ALIGN_SIZE);
			len -= ALIGN_SIZE;
			addr += ALIGN_SIZE;
		}
		/* tail overhang */
		if (len)
			_make_tx_sdma_desc(dd, tx, type, addr, len);
		break;

	case ALIGN_256_HEAD_TAIL: {
		size_t clen;

		/* align head */
		if (addr & ALIGN_MASK) {
			clen = ALIGN_SIZE - (addr & ALIGN_MASK);
			if (clen > len)
				clen = len;
			_make_tx_sdma_desc(dd, tx, type, addr, clen);
			len -= clen;
			addr += clen;
		}
		/* aligned start to aligned tail */
		clen = len & ~ALIGN_MASK;
		if (clen) {
			_make_tx_sdma_desc(dd, tx, type, addr, clen);
			len -= clen;
			addr += clen;
		}
		/* tail overhang */
		if (len)
			_make_tx_sdma_desc(dd, tx, type, addr, len);
		break;
	}

	case ALIGN_256_TAIL: {
		dma_addr_t end = addr + len;
		bool same = (addr & ~ALIGN_MASK) == (end & ~ALIGN_MASK);
		size_t tail_len = end & ALIGN_MASK;
		size_t clen = len - tail_len;

		/* start to aligned tail */
		if (clen && !same) {
			_make_tx_sdma_desc(dd, tx, type, addr, clen);
			len -= clen;
			addr += clen;
		}
		/* tail overhang */
		if (len)
			_make_tx_sdma_desc(dd, tx, type, addr, len);
		break;
	}

	default:
		_make_tx_sdma_desc(dd, tx, type, addr, len);
		break;
	}
#undef ALIGN_SIZE
#undef ALIGN_MASK
}

void __hfi2_sdma_txclean(struct hfi2_devdata *, struct sdma_txreq *);

static inline void sdma_txclean(struct hfi2_devdata *dd, struct sdma_txreq *tx)
{
	if (tx->num_desc)
		__hfi2_sdma_txclean(dd, tx);
}

/* calculate the number of no-op descriptors to add */
static inline int sdma_desc_pad_count(struct hfi2_devdata *dd,
				      struct sdma_txreq *tx)
{
	if (dd->pad_sdma_desc)
		return round_up(tx->num_desc, dd->pad_sdma_desc) - tx->num_desc;
	return 0;
}

/* helpers used by public routines */
static inline void _sdma_close_tx(struct hfi2_devdata *dd,
				  struct sdma_txreq *tx)
{
	u16 last_desc = tx->num_desc - 1;

	sdma_qw_set(dd, last_desc, tx->descp[last_desc].qw);
	tx->descp[last_desc].qw[1] |= dd->default_desc1;
	if (tx->flags & SDMA_TXREQ_F_URGENT)
		tx->descp[last_desc].qw[1] |= (SDMA_DESC1_HEAD_TO_HOST_FLAG |
					       SDMA_DESC1_INT_REQ_FLAG);
	tx->num_pad = sdma_desc_pad_count(dd, tx);
}

/* return true if the current buffer must coalesce */
static inline bool must_coalesce(struct sdma_txreq *tx)
{
	return tx->num_desc + tx->desc_margin >= MAX_DESC;
}

/* return true if the current buffer forces an extension */
static inline bool need_desc_extension(struct sdma_txreq *tx)
{
	return (tx->desc_limit == ARRAY_SIZE(tx->descs)) &&
	       (tx->num_desc + tx->desc_margin >= ARRAY_SIZE(tx->descs));
}
int _hfi2_extend_sdma_tx_descs(struct hfi2_devdata *dd, struct sdma_txreq *tx);

static inline int _sdma_txadd_daddr(struct hfi2_devdata *dd, int type,
				    struct sdma_txreq *tx, dma_addr_t addr,
				    u16 len)
{
	int rval;

	WARN_ON(len > tx->tlen);
	if (need_desc_extension(tx)) {
		rval = _hfi2_extend_sdma_tx_descs(dd, tx);
		if (rval)
			return rval;
	}

	make_tx_sdma_desc(dd, tx, type, addr, len);
	tx->tlen -= len;

	/* special case for last */
	if (!tx->tlen) {
		int pad_len = pad_length(tx);

		if (pad_len) {
			make_tx_sdma_desc(dd, tx, SDMA_MAP_NONE,
					  dd->sdma_pad_phys, pad_len);
		}
		_sdma_close_tx(dd, tx);
	}
	return 0;
}

int hfi2_do_coalesce(struct hfi2_devdata *dd, struct sdma_txreq *tx, int type,
		     void *kvaddr, struct page *page, unsigned long offset,
		     u16 len);

/**
 * sdma_txadd_page() - add a page to the sdma_txreq
 * @dd: the device to use for mapping
 * @tx: tx request to which the page is added
 * @page: page to map
 * @offset: offset within the page
 * @len: length in bytes
 *
 * This is used to add a page/offset/length descriptor.
 *
 * The mapping/unmapping of the page/offset/len is automatically handled.
 *
 * Return:
 * 0 - success, -ENOSPC - mapping fail, -ENOMEM - couldn't
 * extend/coalesce descriptor array
 */
static inline int sdma_txadd_page(struct hfi2_devdata *dd,
				  struct sdma_txreq *tx, struct page *page,
				  unsigned long offset, u16 len)
{
	dma_addr_t addr;

	if (unlikely(must_coalesce(tx)))
		return hfi2_do_coalesce(dd, tx, SDMA_MAP_PAGE, NULL, page,
					offset, len);

	addr = dma_map_page(&dd->pcidev->dev, page, offset, len, DMA_TO_DEVICE);

	if (unlikely(dma_mapping_error(&dd->pcidev->dev, addr))) {
		__hfi2_sdma_txclean(dd, tx);
		return -ENOSPC;
	}

	return _sdma_txadd_daddr(dd, SDMA_MAP_PAGE, tx, addr, len);
}

/**
 * sdma_txadd_daddr() - add a dma address to the sdma_txreq
 * @dd: the device to use for mapping
 * @tx: sdma_txreq to which the page is added
 * @addr: dma address mapped by caller
 * @len: length in bytes
 *
 * This is used to add a descriptor for memory that is already dma mapped.
 *
 * In this case, there is no unmapping as part of the progress processing for
 * this memory location.
 *
 * Return:
 * 0 - success, -ENOMEM - couldn't extend descriptor array
 */

static inline int sdma_txadd_daddr(struct hfi2_devdata *dd,
				   struct sdma_txreq *tx, dma_addr_t addr,
				   u16 len)
{
	if (unlikely(must_coalesce(tx)))
		return hfi2_do_coalesce(dd, tx, SDMA_MAP_NONE, NULL, NULL, 0,
					0);

	return _sdma_txadd_daddr(dd, SDMA_MAP_NONE, tx, addr, len);
}

/**
 * sdma_txadd_kvaddr() - add a kernel virtual address to sdma_txreq
 * @dd: the device to use for mapping
 * @tx: sdma_txreq to which the page is added
 * @kvaddr: the kernel virtual address
 * @len: length in bytes
 *
 * This is used to add a descriptor referenced by the indicated kvaddr and
 * len.
 *
 * The mapping/unmapping of the kvaddr and len is automatically handled.
 *
 * Return:
 * 0 - success, -ENOSPC - mapping fail, -ENOMEM - couldn't extend/coalesce
 * descriptor array
 */
static inline int sdma_txadd_kvaddr(struct hfi2_devdata *dd,
				    struct sdma_txreq *tx, void *kvaddr,
				    u16 len)
{
	dma_addr_t addr;

	if (unlikely(must_coalesce(tx)))
		return hfi2_do_coalesce(dd, tx, SDMA_MAP_SINGLE, kvaddr, NULL,
					0, len);

	addr = dma_map_single(&dd->pcidev->dev, kvaddr, len, DMA_TO_DEVICE);

	if (unlikely(dma_mapping_error(&dd->pcidev->dev, addr))) {
		__hfi2_sdma_txclean(dd, tx);
		return -ENOSPC;
	}

	return _sdma_txadd_daddr(dd, SDMA_MAP_SINGLE, tx, addr, len);
}

struct iowait_work;

int hfi2_sdma_send_txreq(struct sdma_engine *sde, struct iowait_work *wait,
			 struct sdma_txreq *tx, bool pkts_sent);
int hfi2_sdma_send_txlist(struct sdma_engine *sde, struct iowait_work *wait,
			  struct list_head *tx_list, u16 *count_out);

int hfi2_sdma_ahg_alloc(struct sdma_engine *sde);
void hfi2_sdma_ahg_free(struct sdma_engine *sde, int ahg_index);

/**
 * sdma_build_ahg - build ahg descriptor
 * @data
 * @dwindex
 * @startbit
 * @bits
 *
 * Build and return a 32 bit descriptor.
 */
static inline u32 sdma_build_ahg_descriptor(u16 data, u8 dwindex, u8 startbit,
					    u8 bits)
{
	return (u32)(1UL << SDMA_AHG_UPDATE_ENABLE_SHIFT |
		     ((startbit & SDMA_AHG_FIELD_START_MASK)
		      << SDMA_AHG_FIELD_START_SHIFT) |
		     ((bits & SDMA_AHG_FIELD_LEN_MASK)
		      << SDMA_AHG_FIELD_LEN_SHIFT) |
		     ((dwindex & SDMA_AHG_INDEX_MASK) << SDMA_AHG_INDEX_SHIFT) |
		     ((data & SDMA_AHG_VALUE_MASK) << SDMA_AHG_VALUE_SHIFT));
}

/**
 * sdma_progress - use seq number of detect head progress
 * @sde: sdma_engine to check
 * @seq: base seq count
 * @tx: txreq for which we need to check descriptor availability
 *
 * This is used in the appropriate spot in the sleep routine
 * to check for potential ring progress.  This routine gets the
 * seqcount before queuing the iowait structure for progress.
 *
 * If the seqcount indicates that progress needs to be checked,
 * re-submission is detected by checking whether the descriptor
 * queue has enough descriptor for the txreq.
 */
static inline unsigned int
sdma_progress(struct sdma_engine *sde, unsigned int seq, struct sdma_txreq *tx)
{
	if (read_seqretry(&sde->head_lock, seq)) {
		sde->desc_avail = sdma_descq_freecnt(sde);
		if (tx->num_desc + tx->num_pad > sde->desc_avail)
			return 0;
		return 1;
	}
	return 0;
}

/* for use by interrupt handling */
void hfi2_sdma_engine_error(struct sdma_engine *sde, u64 status);
void hfi2_sdma_engine_interrupt(struct sdma_engine *sde, u64 status);
bool hfi2_sdma_work_pending(struct sdma_engine *sde);

/*
 *
 * The diagram below details the relationship of the mapping structures
 *
 * Since the mapping now allows for non-uniform engines per vl, the
 * number of engines for a vl is either the vl_engines[vl] or
 * a computation based on num_sdma/hfi2_num_vls:
 *
 * For example:
 * nactual = vl_engines ? vl_engines[vl] : num_sdma/hfi2_num_vls
 *
 * n = roundup to next highest power of 2 using nactual
 *
 * In the case where there are num_sdma/hfi2_num_vls doesn't divide
 * evenly, the extras are added from the last vl downward.
 *
 * For the case where n > nactual, the engines are assigned
 * in a round robin fashion wrapping back to the first engine
 * for a particular vl.
 *
 *               dd->sdma_map
 *                    |                                   sdma_map_elem[0]
 *                    |                                +--------------------+
 *                    v                                |       mask         |
 *               sdma_vl_map                           |--------------------|
 *      +--------------------------+                   | sde[0] -> eng 1    |
 *      |    list (RCU)            |                   |--------------------|
 *      |--------------------------|                 ->| sde[1] -> eng 2    |
 *      |    mask                  |              --/  |--------------------|
 *      |--------------------------|            -/     |        *           |
 *      |    actual_vls (max 8)    |          -/       |--------------------|
 *      |--------------------------|       --/         | sde[n-1] -> eng n  |
 *      |    vls (max 8)           |     -/            +--------------------+
 *      |--------------------------|  --/
 *      |    map[0]                |-/
 *      |--------------------------|                   +---------------------+
 *      |    map[1]                |---                |       mask          |
 *      |--------------------------|   \----           |---------------------|
 *      |           *              |        \--        | sde[0] -> eng 1+n   |
 *      |           *              |           \----   |---------------------|
 *      |           *              |                \->| sde[1] -> eng 2+n   |
 *      |--------------------------|                   |---------------------|
 *      |   map[vls - 1]           |-                  |         *           |
 *      +--------------------------+ \-                |---------------------|
 *                                     \-              | sde[m-1] -> eng m+n |
 *                                       \             +---------------------+
 *                                        \-
 *                                          \
 *                                           \-        +----------------------+
 *                                             \-      |       mask           |
 *                                               \     |----------------------|
 *                                                \-   | sde[0] -> eng 1+m+n  |
 *                                                  \- |----------------------|
 *                                                    >| sde[1] -> eng 2+m+n  |
 *                                                     |----------------------|
 *                                                     |         *            |
 *                                                     |----------------------|
 *                                                     | sde[o-1] -> eng o+m+n|
 *                                                     +----------------------+
 *
 */

/**
 * struct sdma_map_elem - mapping for a vl
 * @mask - selector mask
 * @sde - array of engines for this vl
 *
 * The mask is used to "mod" the selector
 * to produce index into the trailing
 * array of sdes.
 */
struct sdma_map_elem {
	u32 mask;
	struct sdma_engine *sde[];
};

/**
 * struct sdma_map_el - mapping for a vl
 * @engine_to_vl - map of an engine to a vl
 * @list - rcu head for free callback
 * @mask - vl mask to "mod" the vl to produce an index to map array
 * @actual_vls - number of vls
 * @vls - number of vls rounded to next power of 2
 * @map - array of sdma_map_elem entries
 *
 * This is the parent mapping structure.  The trailing
 * members of the struct point to sdma_map_elem entries, which
 * in turn point to an array of sde's for that vl.
 */
struct sdma_vl_map {
	s8 engine_to_vl[TXE_NUM_SDMA_ENGINES];
	struct rcu_head list;
	u32 mask;
	u8 actual_vls;
	u8 vls;
	struct sdma_map_elem *map[];
};

int hfi2_sdma_map_init(struct hfi2_pportdata *ppd, u8 hfi2_num_vls,
		       u8 *vl_engines);

/* slow path */
void _hfi2_sdma_engine_progress_schedule(struct sdma_engine *sde);

/**
 * sdma_engine_progress_schedule() - schedule progress on engine
 * @sde: sdma_engine to schedule progress
 *
 * This is the fast path.
 *
 */
static inline void sdma_engine_progress_schedule(struct sdma_engine *sde)
{
	if (!sde || sdma_descq_inprocess(sde) < (sde->descq_cnt / 8))
		return;
	_hfi2_sdma_engine_progress_schedule(sde);
}

struct sdma_engine *hfi2_sdma_select_engine_sc(struct hfi2_pportdata *ppd,
					       u32 selector, u8 sc5);
struct sdma_engine *hfi2_sdma_select_engine_vl(struct hfi2_pportdata *ppd,
					       u32 selector, u8 vl);
struct sdma_engine *hfi2_sdma_select_user_engine(struct hfi2_pportdata *ppd,
						 u32 selector, u8 vl);
ssize_t hfi2_sdma_get_cpu_to_sde_map(struct sdma_engine *sde, char *buf);
ssize_t hfi2_sdma_set_cpu_to_sde_map(struct sdma_engine *sde, const char *buf,
				     size_t count);
int hfi2_sdma_engine_get_vl(struct hfi2_pportdata *ppd,
			    struct sdma_engine *sde);
void hfi2_sdma_seqfile_dump_sde(struct seq_file *s, struct sdma_engine *sde);
void hfi2_sdma_seqfile_dump_cpu_list(struct seq_file *s,
				     struct hfi2_devdata *dd,
				     unsigned long cpuid);

#ifdef CONFIG_HFI2_SDMA_VERBOSITY
void sdma_dumpstate(struct sdma_engine *);
#endif
static inline char *slashstrip(char *s)
{
	char *r = s;

	while (*s)
		if (*s++ == '/')
			r = s;
	return r;
}

u16 hfi2_sdma_get_descq_cnt(void);

extern uint hfi2_mod_num_sdma;

void hfi2_sdma_update_lmc(struct hfi2_devdata *dd, u64 mask, u32 lid);
#endif
