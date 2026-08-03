// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Copyright(c) 2015 - 2018 Intel Corporation.
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */

#include <linux/spinlock.h>
#include <linux/seqlock.h>
#include <linux/netdevice.h>
#include <linux/moduleparam.h>
#include <linux/bitops.h>
#include <linux/timer.h>
#include <linux/vmalloc.h>
#include <linux/highmem.h>

#include "hfi2.h"
#include "common.h"
#include "qp.h"
#include "sdma.h"
#include "iowait.h"
#include "trace.h"

/* must be a power of 2 >= 64 <= 32768 */
#define SDMA_DESCQ_CNT 2048
#define SDMA_DESC_INTR 64
#define INVALID_TAIL 0xffff
#define SDMA_PAD max_t(size_t, MAX_16B_PADDING, sizeof(u32))

static uint sdma_descq_cnt = SDMA_DESCQ_CNT;

static uint sdma_idle_cnt = 250;

uint hfi2_mod_num_sdma;

static uint sdma_desct_intr = SDMA_DESC_INTR;

static uint enable_jkr_sdma_mem_init;

static uint sdma_single_descriptor;

static int sdma_threshold = -1;

static int pad_sdma_desc = -1;

static int sdma_align = -1;

#define JKR_DEFAULT_SDMA_CREDITS_LIMIT 1568
/*
 * -1 => module parameter not set during load, use
 * JKR_DEFAULT_SDMA_CREDITS_LIMIT.
 *
 * Use this to distinguish between user intentionally setting
 * jkr_sdma_credits_limit > 6272/num_sdma and the per-SDMA credit limit
 * happening to be > 6272/num_sdma because of value of num_sdma.
 */
static int jkr_sdma_credits_limit = -1;

#define SDMA_WAIT_BATCH_SIZE 20
/* max wait time for a SDMA engine to indicate it has halted */
#define SDMA_ERR_HALT_TIMEOUT 10 /* ms */
#define SIM_SDMA_ERR_HALT_TIMEOUT 1000 /* ms */

#define SD(name) SEND_DMA_##name
/* all SDMA engine errors that cause a halt */
#define ALL_SDMA_ENG_HALT_ERRS                                    \
	(SD(ENG_ERR_STATUS_SDMA_WRONG_DW_ERR_SMASK) |             \
	 SD(ENG_ERR_STATUS_SDMA_GEN_MISMATCH_ERR_SMASK) |         \
	 SD(ENG_ERR_STATUS_SDMA_TOO_LONG_ERR_SMASK) |             \
	 SD(ENG_ERR_STATUS_SDMA_TAIL_OUT_OF_BOUNDS_ERR_SMASK) |   \
	 SD(ENG_ERR_STATUS_SDMA_FIRST_DESC_ERR_SMASK) |           \
	 SD(ENG_ERR_STATUS_SDMA_MEM_READ_ERR_SMASK) |             \
	 SD(ENG_ERR_STATUS_SDMA_HALT_ERR_SMASK) |                 \
	 SD(ENG_ERR_STATUS_SDMA_LENGTH_MISMATCH_ERR_SMASK) |      \
	 SD(ENG_ERR_STATUS_SDMA_PACKET_DESC_OVERFLOW_ERR_SMASK) | \
	 SD(ENG_ERR_STATUS_SDMA_HEADER_SELECT_ERR_SMASK) |        \
	 SD(ENG_ERR_STATUS_SDMA_HEADER_ADDRESS_ERR_SMASK) |       \
	 SD(ENG_ERR_STATUS_SDMA_HEADER_LENGTH_ERR_SMASK) |        \
	 SD(ENG_ERR_STATUS_SDMA_TIMEOUT_ERR_SMASK) |              \
	 SD(ENG_ERR_STATUS_SDMA_DESC_TABLE_UNC_ERR_SMASK) |       \
	 SD(ENG_ERR_STATUS_SDMA_ASSEMBLY_UNC_ERR_SMASK) |         \
	 SD(ENG_ERR_STATUS_SDMA_PACKET_TRACKING_UNC_ERR_SMASK) |  \
	 SD(ENG_ERR_STATUS_SDMA_HEADER_STORAGE_UNC_ERR_SMASK) |   \
	 SD(ENG_ERR_STATUS_SDMA_HEADER_REQUEST_FIFO_UNC_ERR_SMASK))

/* all SDMA engine errors that are correctable */
#define ALL_SDMA_ENG_COR_ERRS                                             \
	(SEND_DMA_ENG_ERR_STATUS_SDMA_HEADER_REQUEST_FIFO_COR_ERR_SMASK | \
	 SEND_DMA_ENG_ERR_STATUS_SDMA_HEADER_STORAGE_COR_ERR_SMASK |      \
	 SEND_DMA_ENG_ERR_STATUS_SDMA_PACKET_TRACKING_COR_ERR_SMASK |     \
	 SEND_DMA_ENG_ERR_STATUS_SDMA_ASSEMBLY_COR_ERR_SMASK |            \
	 SEND_DMA_ENG_ERR_STATUS_SDMA_DESC_TABLE_COR_ERR_SMASK)

/* sdma_sendctrl operations */
#define SDMA_SENDCTRL_OP_ENABLE BIT(0)
#define SDMA_SENDCTRL_OP_INTENABLE BIT(1)
#define SDMA_SENDCTRL_OP_HALT BIT(2)
#define SDMA_SENDCTRL_OP_CLEANUP BIT(3)

/* handle long defines */
#define SDMA_EGRESS_PACKET_OCCUPANCY_SMASK \
	SEND_EGRESS_SEND_DMA_STATUS_SDMA_EGRESS_PACKET_OCCUPANCY_SMASK
#define SDMA_EGRESS_PACKET_OCCUPANCY_SHIFT \
	SEND_EGRESS_SEND_DMA_STATUS_SDMA_EGRESS_PACKET_OCCUPANCY_SHIFT

static const char *const sdma_state_names[] = {
	[sdma_state_s00_hw_down] = "s00_HwDown",
	[sdma_state_s10_hw_start_up_halt_wait] = "s10_HwStartUpHaltWait",
	[sdma_state_s15_hw_start_up_clean_wait] = "s15_HwStartUpCleanWait",
	[sdma_state_s20_idle] = "s20_Idle",
	[sdma_state_s30_sw_clean_up_wait] = "s30_SwCleanUpWait",
	[sdma_state_s40_hw_clean_up_wait] = "s40_HwCleanUpWait",
	[sdma_state_s50_hw_halt_wait] = "s50_HwHaltWait",
	[sdma_state_s60_idle_halt_wait] = "s60_IdleHaltWait",
	[sdma_state_s80_hw_freeze] = "s80_HwFreeze",
	[sdma_state_s82_freeze_sw_clean] = "s82_FreezeSwClean",
	[sdma_state_s99_running] = "s99_Running",
};

#ifdef CONFIG_HFI2_SDMA_VERBOSITY
static const char *const sdma_event_names[] = {
	[sdma_event_e00_go_hw_down] = "e00_GoHwDown",
	[sdma_event_e10_go_hw_start] = "e10_GoHwStart",
	[sdma_event_e15_hw_halt_done] = "e15_HwHaltDone",
	[sdma_event_e25_hw_clean_up_done] = "e25_HwCleanUpDone",
	[sdma_event_e30_go_running] = "e30_GoRunning",
	[sdma_event_e40_sw_cleaned] = "e40_SwCleaned",
	[sdma_event_e50_hw_cleaned] = "e50_HwCleaned",
	[sdma_event_e60_hw_halted] = "e60_HwHalted",
	[sdma_event_e70_go_idle] = "e70_GoIdle",
	[sdma_event_e80_hw_freeze] = "e80_HwFreeze",
	[sdma_event_e81_hw_frozen] = "e81_HwFrozen",
	[sdma_event_e82_hw_unfreeze] = "e82_HwUnfreeze",
	[sdma_event_e85_link_down] = "e85_LinkDown",
	[sdma_event_e90_sw_halted] = "e90_SwHalted",
};
#endif

static const struct sdma_set_state_action sdma_action_table[] = {
	[sdma_state_s00_hw_down] = {
		.go_s99_running_tofalse = 1,
		.op_enable = 0,
		.op_intenable = 0,
		.op_halt = 0,
		.op_cleanup = 0,
	},
	[sdma_state_s10_hw_start_up_halt_wait] = {
		.op_enable = 0,
		.op_intenable = 0,
		.op_halt = 1,
		.op_cleanup = 0,
	},
	[sdma_state_s15_hw_start_up_clean_wait] = {
		.op_enable = 0,
		.op_intenable = 1,
		.op_halt = 0,
		.op_cleanup = 1,
	},
	[sdma_state_s20_idle] = {
		.op_enable = 0,
		.op_intenable = 1,
		.op_halt = 0,
		.op_cleanup = 0,
	},
	[sdma_state_s30_sw_clean_up_wait] = {
		.op_enable = 0,
		.op_intenable = 0,
		.op_halt = 0,
		.op_cleanup = 0,
	},
	[sdma_state_s40_hw_clean_up_wait] = {
		.op_enable = 0,
		.op_intenable = 0,
		.op_halt = 0,
		.op_cleanup = 1,
	},
	[sdma_state_s50_hw_halt_wait] = {
		.op_enable = 0,
		.op_intenable = 0,
		.op_halt = 0,
		.op_cleanup = 0,
	},
	[sdma_state_s60_idle_halt_wait] = {
		.go_s99_running_tofalse = 1,
		.op_enable = 0,
		.op_intenable = 0,
		.op_halt = 1,
		.op_cleanup = 0,
	},
	[sdma_state_s80_hw_freeze] = {
		.op_enable = 0,
		.op_intenable = 0,
		.op_halt = 0,
		.op_cleanup = 0,
	},
	[sdma_state_s82_freeze_sw_clean] = {
		.op_enable = 0,
		.op_intenable = 0,
		.op_halt = 0,
		.op_cleanup = 0,
	},
	[sdma_state_s99_running] = {
		.op_enable = 1,
		.op_intenable = 1,
		.op_halt = 0,
		.op_cleanup = 0,
		.go_s99_running_totrue = 1,
	},
};

#define SDMA_TAIL_UPDATE_THRESH 0x1F

/* declare all statics here rather than keep sorting */
static void sdma_complete(struct kref *);
static void sdma_finalput(struct sdma_state *);
static void sdma_get(struct sdma_state *);
static void sdma_hw_clean_up_worker(struct work_struct *);
static void sdma_put(struct sdma_state *);
static void sdma_set_state(struct sdma_engine *, enum sdma_states);
static void sdma_start_hw_clean_up(struct sdma_engine *);
static void sdma_sw_clean_up_worker(struct work_struct *);
static void sdma_sendctrl(struct sdma_engine *sde, unsigned int op);
static void init_sdma_regs(struct sdma_engine *, uint);
static void sdma_process_event(struct sdma_engine *sde, enum sdma_events event);
static void __sdma_process_event(struct sdma_engine *sde,
				 enum sdma_events event);
static void dump_sdma_state(struct sdma_engine *sde);
static void sdma_make_progress(struct sdma_engine *sde, u64 status);
static void sdma_desc_avail(struct sdma_engine *sde, uint avail);
static void sdma_flush_descq(struct sdma_engine *sde);
static void sdma_rht_free(void *ptr, void *arg);
static int prime_sdma_memories(struct hfi2_devdata *dd);

/**
 * sdma_state_name() - return state string from enum
 * @state: state
 */
static const char *sdma_state_name(enum sdma_states state)
{
	return sdma_state_names[state];
}

static void sdma_get(struct sdma_state *ss)
{
	kref_get(&ss->kref);
}

static void sdma_complete(struct kref *kref)
{
	struct sdma_state *ss = container_of(kref, struct sdma_state, kref);

	complete(&ss->comp);
}

static void sdma_put(struct sdma_state *ss)
{
	kref_put(&ss->kref, sdma_complete);
}

static void sdma_finalput(struct sdma_state *ss)
{
	sdma_put(ss);
	wait_for_completion(&ss->comp);
}

static inline void write_sde_csr(struct sdma_engine *sde, u32 offset0,
				 u64 value)
{
	write_sdma_csr(sde->dd, sde->this_idx, offset0, value);
}

static inline u64 read_sde_csr(struct sdma_engine *sde, u32 offset0)
{
	return read_sdma_csr(sde->dd, sde->this_idx, offset0);
}

static inline void write_sdecfg_csr(struct sdma_engine *sde, u32 offset,
				    u64 value)
{
	write_sdmacfg_csr(sde->dd, sde->this_idx, offset, value);
}

static inline u64 read_sdecfg_csr(struct sdma_engine *sde, u32 offset)
{
	return read_sdmacfg_csr(sde->dd, sde->this_idx, offset);
}

static inline void __iomem *get_sdma_csr_addr(const struct hfi2_devdata *dd,
					      int eng, u32 offset)
{
	return hfi2_get_csr_addr(dd,
				 offset + (dd->params->txe_sdma_stride * eng));
}

/*
 * sdma_wait_for_packet_egress() - wait for the Launch FIFO occupancy for
 * sdma engine 'sde' to drop to 0.
 */
static void sdma_wait_for_packet_egress(struct sdma_engine *sde, int pause)
{
	u64 off = 8 * sde->this_idx;
	struct hfi2_devdata *dd = sde->dd;
	struct hfi2_pportdata *ppd;
	int lcnt;
	int pidx;
	u64 reg_prev;
	u64 reg;

	for (pidx = 0; pidx < dd->num_pports; pidx++) {
		ppd = dd->pport + pidx;
		lcnt = 0;
		reg = 0;

		while (1) {
			reg_prev = reg;
			reg = read_eport_csr(
				dd, pidx,
				off + dd->params->send_egress_send_dma_status_reg);

			reg &= SDMA_EGRESS_PACKET_OCCUPANCY_SMASK;
			reg >>= SDMA_EGRESS_PACKET_OCCUPANCY_SHIFT;
			if (reg == 0)
				break;
			/* counter is reset if accupancy count changes */
			if (reg != reg_prev)
				lcnt = 0;
			if (lcnt++ > 500) {
				/* timed out - bounce the link */
				dd_dev_err(
					dd,
					"%s: engine %u timeout waiting for packets to egress, remaining count %u, bouncing link\n",
					__func__, sde->this_idx, (u32)reg);
				queue_work(ppd->link_wq,
					   &ppd->link_bounce_work);
				break;
			}
			udelay(1);
		}
	}
}

/*
 * hfi2_sdma_wait() - wait for packet egress to complete for all SDMA engines,
 * and pause for credit return.
 */
void hfi2_sdma_wait(struct hfi2_devdata *dd)
{
	struct hfi2_devrsrcs *dr = &dd->rsrcs;
	int i;

	for (i = dr->first_sdma_engine; i < dr->last_sdma_engine; i++) {
		struct sdma_engine *sde = &dd->per_sdma[i];

		sdma_wait_for_packet_egress(sde, 0);
	}
}

static inline void sdma_set_desc_cnt(struct sdma_engine *sde, unsigned int cnt)
{
	u64 reg;

	if (!(sde->dd->flags & HFI2_HAS_SDMA_TIMEOUT))
		return;
	reg = cnt;
	reg &= SD(DESC_CNT_CNT_MASK);
	reg <<= SD(DESC_CNT_CNT_SHIFT);
	write_sde_csr(sde, sde->dd->params->send_dma_desc_cnt_reg, reg);
}

static inline void complete_tx(struct sdma_engine *sde, struct sdma_txreq *tx,
			       int res)
{
	/* protect against complete modifying */
	struct iowait *wait = tx->wait;
	callback_t complete = tx->complete;

#ifdef CONFIG_HFI2_DEBUG_SDMA_ORDER
	trace_hfi2_sdma_out_sn(sde, tx->sn);
	if (WARN_ON_ONCE(sde->head_sn != tx->sn))
		dd_dev_err(sde->dd, "expected %llu got %llu\n", sde->head_sn,
			   tx->sn);
	sde->head_sn++;
#endif
	__hfi2_sdma_txclean(sde->dd, tx);
	if (complete)
		(*complete)(tx, res);
	if (iowait_sdma_dec(wait))
		iowait_drain_wakeup(wait);
}

/*
 * Complete all the sdma requests with a SDMA_TXREQ_S_ABORTED status
 *
 * Depending on timing there can be txreqs in two places:
 * - in the descq ring
 * - in the flush list
 *
 * To avoid ordering issues the descq ring needs to be flushed
 * first followed by the flush list.
 *
 * This routine is called from two places
 * - From a work queue item
 * - Directly from the state machine just before setting the
 *   state to running
 *
 * Must be called with head_lock held
 *
 */
static void sdma_flush(struct sdma_engine *sde)
{
	struct sdma_txreq *txp, *txp_next;
	LIST_HEAD(flushlist);
	unsigned long flags;
	uint seq;

	/* flush from head to tail */
	sdma_flush_descq(sde);
	spin_lock_irqsave(&sde->flushlist_lock, flags);
	/* copy flush list */
	list_splice_init(&sde->flushlist, &flushlist);
	spin_unlock_irqrestore(&sde->flushlist_lock, flags);
	/* flush from flush list */
	list_for_each_entry_safe(txp, txp_next, &flushlist, list)
		complete_tx(sde, txp, SDMA_TXREQ_S_ABORTED);
	/* wakeup QPs orphaned on the dmawait list */
	do {
		struct iowait *w, *nw;

		seq = read_seqbegin(&sde->waitlock);
		if (!list_empty(&sde->dmawait)) {
			write_seqlock(&sde->waitlock);
			list_for_each_entry_safe(w, nw, &sde->dmawait, list) {
				if (w->wakeup) {
					w->wakeup(w, SDMA_AVAIL_REASON);
					list_del_init(&w->list);
				}
			}
			write_sequnlock(&sde->waitlock);
		}
	} while (read_seqretry(&sde->waitlock, seq));
}

/*
 * Fields a work request for flushing the descq ring
 * and the flush list
 *
 * If the engine has been brought to running during
 * the scheduling delay, the flush is ignored, assuming
 * that the process of bringing the engine to running
 * would have done this flush prior to going to running.
 *
 */
static void sdma_field_flush(struct work_struct *work)
{
	unsigned long flags;
	struct sdma_engine *sde =
		container_of(work, struct sdma_engine, flush_worker);

	write_seqlock_irqsave(&sde->head_lock, flags);
	if (!__sdma_running(sde))
		sdma_flush(sde);
	write_sequnlock_irqrestore(&sde->head_lock, flags);
}

static void sdma_err_halt_wait(struct work_struct *work)
{
	struct sdma_engine *sde =
		container_of(work, struct sdma_engine, err_halt_worker);
	u64 statuscsr;
	unsigned long timeout;

	if (unlikely(sde->dd->icode == ICODE_FUNCTIONAL_SIMULATOR))
		timeout = jiffies + msecs_to_jiffies(SIM_SDMA_ERR_HALT_TIMEOUT);
	else
		timeout = jiffies + msecs_to_jiffies(SDMA_ERR_HALT_TIMEOUT);
	while (1) {
		statuscsr =
			read_sde_csr(sde, sde->dd->params->send_dma_status_reg);
		statuscsr &= SD(STATUS_ENG_HALTED_SMASK);
		if (statuscsr)
			break;
		if (time_after(jiffies, timeout)) {
			dd_dev_err(
				sde->dd,
				"SDMA engine %d - timeout waiting for engine to halt\n",
				sde->this_idx);
			/*
			 * Continue anyway.  This could happen if there was
			 * an uncorrectable error in the wrong spot.
			 */
			break;
		}
		usleep_range(80, 120);
	}

	sdma_process_event(sde, sdma_event_e15_hw_halt_done);
}

static void sdma_err_progress_check_schedule(struct sdma_engine *sde)
{
	struct hfi2_devrsrcs *dr = &sde->dd->rsrcs;

	if (!hfi2_is_bx(sde->dd) && HFI2_CAP_IS_KSET(SDMA_AHG)) {
		unsigned int index;
		struct hfi2_devdata *dd = sde->dd;

		for (index = dr->first_sdma_engine;
		     index < dr->last_sdma_engine; index++) {
			struct sdma_engine *curr_sdma = &dd->per_sdma[index];

			if (curr_sdma != sde)
				curr_sdma->progress_check_head =
					curr_sdma->descq_head;
		}
		dd_dev_err(sde->dd, "SDMA engine %d - check scheduled\n",
			   sde->this_idx);
		mod_timer(&sde->err_progress_check_timer, jiffies + 10);
	}
}

static void sdma_err_progress_check(struct timer_list *t)
{
	unsigned int index;
	struct sdma_engine *sde =
		timer_container_of(sde, t, err_progress_check_timer);
	struct hfi2_devrsrcs *dr = &sde->dd->rsrcs;

	dd_dev_err(sde->dd, "SDE progress check event\n");
	for (index = dr->first_sdma_engine; index < dr->last_sdma_engine;
	     index++) {
		struct sdma_engine *curr_sde = &sde->dd->per_sdma[index];
		unsigned long flags;

		/* check progress on each engine except the current one */
		if (curr_sde == sde)
			continue;
		/*
		 * We must lock interrupts when acquiring sde->lock,
		 * to avoid a deadlock if interrupt triggers and spins on
		 * the same lock on same CPU
		 */
		spin_lock_irqsave(&curr_sde->tail_lock, flags);
		write_seqlock(&curr_sde->head_lock);

		/* skip non-running queues */
		if (curr_sde->state.current_state != sdma_state_s99_running) {
			write_sequnlock(&curr_sde->head_lock);
			spin_unlock_irqrestore(&curr_sde->tail_lock, flags);
			continue;
		}

		if ((curr_sde->descq_head != curr_sde->descq_tail) &&
		    (curr_sde->descq_head == curr_sde->progress_check_head))
			__sdma_process_event(curr_sde,
					     sdma_event_e90_sw_halted);
		write_sequnlock(&curr_sde->head_lock);
		spin_unlock_irqrestore(&curr_sde->tail_lock, flags);
	}
	schedule_work(&sde->err_halt_worker);
}

static void sdma_hw_clean_up_worker(struct work_struct *work)
{
	struct sdma_engine *sde =
		container_of(work, struct sdma_engine, sdma_hw_clean_up_work);
	u64 statuscsr;
	u32 count = 0;

	while (1) {
#ifdef CONFIG_HFI2_SDMA_VERBOSITY
		dd_dev_err(sde->dd, "CONFIG SDMA(%u) %s:%d %s()\n",
			   sde->this_idx, slashstrip(__FILE__), __LINE__,
			   __func__);
#endif
		statuscsr =
			read_sde_csr(sde, sde->dd->params->send_dma_status_reg);
		statuscsr &= SD(STATUS_ENG_CLEANED_UP_SMASK);
		if (statuscsr)
			break;
		if (++count > 100) {
			dd_dev_err(
				sde->dd,
				"SDMA engine %d - timeout waiting for engine to clean\n",
				sde->this_idx);
			break;
		}
		udelay(10);
	}

	sdma_process_event(sde, sdma_event_e25_hw_clean_up_done);
}

static inline struct sdma_txreq *get_txhead(struct sdma_engine *sde)
{
	return sde->tx_ring[sde->tx_head & sde->sdma_mask];
}

/*
 * flush ring for recovery
 */
static void sdma_flush_descq(struct sdma_engine *sde)
{
	u16 head, tail;
	int progress = 0;
	struct sdma_txreq *txp = get_txhead(sde);

	/* The reason for some of the complexity of this code is that
	 * not all descriptors have corresponding txps.  So, we have to
	 * be able to skip over descs until we wander into the range of
	 * the next txp on the list.
	 */
	head = sde->descq_head & sde->sdma_mask;
	tail = sde->descq_tail & sde->sdma_mask;
	while (head != tail) {
		/* advance head, wrap if needed */
		head = ++sde->descq_head & sde->sdma_mask;
		/* if now past this txp's descs, do the callback */
		if (txp && txp->next_descq_idx == head) {
			/* remove from list */
			sde->tx_ring[sde->tx_head++ & sde->sdma_mask] = NULL;
			trace_hfi2_sdma_progress(sde, head, tail, txp);
			complete_tx(sde, txp, SDMA_TXREQ_S_ABORTED);
			txp = get_txhead(sde);
		}
		progress++;
	}
	if (progress)
		sdma_desc_avail(sde, sdma_descq_freecnt(sde));
}

static void sdma_sw_clean_up_worker(struct work_struct *work)
{
	struct sdma_engine *sde =
		container_of(work, struct sdma_engine, sdma_sw_clean_up_work);
	unsigned long flags;

	spin_lock_irqsave(&sde->tail_lock, flags);
	write_seqlock(&sde->head_lock);

	/*
	 * At this point, the following should always be true:
	 * - We are halted, so no more descriptors are getting retired.
	 * - We are not running, so no one is submitting new work.
	 * - Only we can send the e40_sw_cleaned, so we can't start
	 *   running again until we say so.  So, the active list and
	 *   descq are ours to play with.
	 */

	/*
	 * In the error clean up sequence, software clean must be called
	 * before the hardware clean so we can use the hardware head in
	 * the progress routine.  A hardware clean or SPC unfreeze will
	 * reset the hardware head.
	 *
	 * Process all retired requests. The progress routine will use the
	 * latest physical hardware head - we are not running so speed does
	 * not matter.
	 */
	sdma_make_progress(sde, 0);

	sdma_flush(sde);

	/*
	 * Reset our notion of head and tail.
	 * Note that the HW registers have been reset via an earlier
	 * clean up.
	 */
	sde->descq_tail = 0;
	sde->descq_head = 0;
	sde->desc_avail = sdma_descq_freecnt(sde);
	*sde->head_dma = 0;

	__sdma_process_event(sde, sdma_event_e40_sw_cleaned);

	write_sequnlock(&sde->head_lock);
	spin_unlock_irqrestore(&sde->tail_lock, flags);
}

static void sdma_sw_tear_down(struct sdma_engine *sde)
{
	struct sdma_state *ss = &sde->state;

	/* Releasing this reference means the state machine has stopped. */
	sdma_put(ss);

	/* stop waiting for all unfreeze events to complete */
	atomic_set(&sde->dd->sdma_unfreeze_count, -1);
	wake_up_interruptible(&sde->dd->sdma_unfreeze_wq);
}

static void sdma_start_hw_clean_up(struct sdma_engine *sde)
{
	queue_work(sde->dd->hfi2_wq, &sde->sdma_hw_clean_up_work);
}

static void sdma_set_state(struct sdma_engine *sde, enum sdma_states next_state)
{
	struct sdma_state *ss = &sde->state;
	const struct sdma_set_state_action *action = sdma_action_table;
	unsigned int op = 0;

	trace_hfi2_sdma_state(sde, sdma_state_names[ss->current_state],
			      sdma_state_names[next_state]);

	/* debugging bookkeeping */
	ss->previous_state = ss->current_state;
	ss->previous_op = ss->current_op;
	ss->current_state = next_state;

	if (ss->previous_state != sdma_state_s99_running &&
	    next_state == sdma_state_s99_running)
		sdma_flush(sde);

	if (action[next_state].op_enable)
		op |= SDMA_SENDCTRL_OP_ENABLE;

	if (action[next_state].op_intenable)
		op |= SDMA_SENDCTRL_OP_INTENABLE;

	if (action[next_state].op_halt)
		op |= SDMA_SENDCTRL_OP_HALT;

	if (action[next_state].op_cleanup)
		op |= SDMA_SENDCTRL_OP_CLEANUP;

	if (action[next_state].go_s99_running_tofalse)
		ss->go_s99_running = 0;

	if (action[next_state].go_s99_running_totrue)
		ss->go_s99_running = 1;

	ss->current_op = op;
	sdma_sendctrl(sde, ss->current_op);
}

/**
 * hfi2_sdma_get_descq_cnt() - called when device probed
 *
 * Return a validated descq count.
 *
 * This is currently only used in the verbs initialization to build the tx
 * list.
 *
 * This will probably be deleted in favor of a more scalable approach to
 * alloc tx's.
 *
 */
u16 hfi2_sdma_get_descq_cnt(void)
{
	u16 count = sdma_descq_cnt;

	if (!count)
		return SDMA_DESCQ_CNT;
	/* count must be a power of 2 greater than 64 and less than
	 * 32768.   Otherwise return default.
	 */
	if (!is_power_of_2(count))
		return SDMA_DESCQ_CNT;
	if (count < 64 || count > 32768)
		return SDMA_DESCQ_CNT;
	return count;
}

/**
 * hfi2_sdma_engine_get_vl() - return vl for a given sdma engine
 * @ppd: port structure
 * @sde: sdma engine
 *
 * This function returns the vl mapped to a given engine, or an error if
 * the mapping can't be found. The mapping fields are protected by RCU.
 */
int hfi2_sdma_engine_get_vl(struct hfi2_pportdata *ppd, struct sdma_engine *sde)
{
	struct sdma_vl_map *m;
	u8 vl;

	if (sde->this_idx >= TXE_NUM_SDMA_ENGINES)
		return -EINVAL;

	rcu_read_lock();
	m = rcu_dereference(ppd->sdma_map);
	if (unlikely(!m)) {
		rcu_read_unlock();
		return -EINVAL;
	}
	vl = m->engine_to_vl[sde->this_idx];
	rcu_read_unlock();

	return vl;
}

/**
 * hfi2_sdma_select_engine_vl() - select sdma engine
 * @ppd: port structure
 * @selector: a spreading factor
 * @vl: this vl
 *
 * This function returns an engine based on the selector and a vl.  The
 * mapping fields are protected by RCU.
 */
struct sdma_engine *hfi2_sdma_select_engine_vl(struct hfi2_pportdata *ppd,
					       u32 selector, u8 vl)
{
	struct hfi2_devdata *dd = ppd->dd;
	struct hfi2_devrsrcs *dr = &dd->rsrcs;
	struct sdma_vl_map *m;
	struct sdma_map_elem *e;
	struct sdma_engine *rval;

	/* NOTE This should only happen if SC->VL changed after the initial
	 *      checks on the QP/AH
	 *      Default will return engine 0 below
	 */
	if (vl >= hfi2_num_vls) {
		rval = NULL;
		goto done;
	}

	rcu_read_lock();
	m = rcu_dereference(ppd->sdma_map);
	if (unlikely(!m)) {
		rcu_read_unlock();
		return &dd->per_sdma[dr->first_sdma_engine];
	}
	e = m->map[vl & m->mask];
	rval = e->sde[selector & e->mask];
	rcu_read_unlock();

done:
	rval = !rval ? &dd->per_sdma[dr->first_sdma_engine] : rval;
	trace_hfi2_sdma_engine_select(dd, selector, vl, rval->this_idx);
	return rval;
}

/**
 * hfi2_sdma_select_engine_sc() - select sdma engine
 * @ppd: port structure
 * @selector: a spreading factor
 * @sc5: the 5 bit sc
 *
 * This function returns an engine based on the selector and an sc.
 */
struct sdma_engine *hfi2_sdma_select_engine_sc(struct hfi2_pportdata *ppd,
					       u32 selector, u8 sc5)
{
	u8 vl = sc_to_vlt(ppd, sc5);

	return hfi2_sdma_select_engine_vl(ppd, selector, vl);
}

struct sdma_rht_map_elem {
	u32 mask;
	u8 ctr;
	struct sdma_engine *sde[TXE_NUM_SDMA_ENGINES];
};

struct sdma_rht_node {
	unsigned long cpu_id;
	struct sdma_rht_map_elem
		*port_map[LARGEST_NUM_PORTS][HFI2_MAX_VLS_SUPPORTED];
	struct rhash_head node;
};

#define NR_CPUS_HINT 192

static const struct rhashtable_params sdma_rht_params = {
	.nelem_hint = NR_CPUS_HINT,
	.head_offset = offsetof(struct sdma_rht_node, node),
	.key_offset = offsetof(struct sdma_rht_node, cpu_id),
	.key_len = sizeof_field(struct sdma_rht_node, cpu_id),
	.max_size = NR_CPUS,
	.min_size = 8,
	.automatic_shrinking = true,
};

/*
 * hfi2_sdma_select_user_engine() - select sdma engine based on user setup
 * @ppd: port structure
 * @selector: a spreading factor
 * @vl: this vl
 *
 * This function returns an sdma engine for a user sdma request.
 * User defined sdma engine affinity setting is honored when applicable,
 * otherwise system default sdma engine mapping is used. To ensure correct
 * ordering, the mapping from <selector, vl> to sde must remain unchanged.
 */
struct sdma_engine *hfi2_sdma_select_user_engine(struct hfi2_pportdata *ppd,
						 u32 selector, u8 vl)
{
	struct hfi2_devdata *dd = ppd->dd;
	struct sdma_rht_node *rht_node;
	struct sdma_rht_map_elem *map;
	struct sdma_engine *sde = NULL;
	unsigned long cpu_id;

	/*
	 * To ensure that always the same sdma engine(s) will be
	 * selected make sure the process is pinned to this CPU only.
	 */
	if (current->nr_cpus_allowed != 1)
		goto out;

	rcu_read_lock();
	cpu_id = smp_processor_id();
	rht_node = rhashtable_lookup(dd->sdma_rht, &cpu_id, sdma_rht_params);

	if (!rht_node)
		goto unlock;
	map = rht_node->port_map[ppd->hw_pidx][vl];
	if (!map)
		goto unlock;
	sde = map->sde[selector & map->mask];
unlock:
	rcu_read_unlock();

	if (sde)
		return sde;

out:
	return hfi2_sdma_select_engine_vl(ppd, selector, vl);
}

static void sdma_populate_sde_map(struct sdma_rht_map_elem *map)
{
	int i;

	for (i = 0; i < roundup_pow_of_two(map->ctr ?: 1) - map->ctr; i++)
		map->sde[map->ctr + i] = map->sde[i];
}

static void sdma_cleanup_sde_map(struct sdma_rht_map_elem *map,
				 struct sdma_engine *sde)
{
	unsigned int i, pow;

	/* only need to check the first ctr entries for a match */
	for (i = 0; i < map->ctr; i++) {
		if (map->sde[i] == sde) {
			memmove(&map->sde[i], &map->sde[i + 1],
				(map->ctr - i - 1) * sizeof(map->sde[0]));
			map->ctr--;
			pow = roundup_pow_of_two(map->ctr ?: 1);
			map->mask = pow - 1;
			sdma_populate_sde_map(map);
			break;
		}
	}
}

/*
 * Prevents concurrent reads and writes of the sdma engine cpu_mask
 */
static DEFINE_MUTEX(process_to_sde_mutex);

ssize_t hfi2_sdma_set_cpu_to_sde_map(struct sdma_engine *sde, const char *buf,
				     size_t count)
{
	struct hfi2_devdata *dd = sde->dd;
	cpumask_var_t mask, new_mask;
	unsigned long cpu;
	int ret, vl, sz;
	int pidx;
	struct sdma_rht_node *rht_node;

	ret = zalloc_cpumask_var(&mask, GFP_KERNEL);
	if (!ret)
		return -ENOMEM;

	ret = zalloc_cpumask_var(&new_mask, GFP_KERNEL);
	if (!ret) {
		free_cpumask_var(mask);
		return -ENOMEM;
	}
	ret = cpulist_parse(buf, mask);
	if (ret)
		goto out_free;

	if (!cpumask_subset(mask, cpu_online_mask)) {
		dd_dev_warn(sde->dd, "Invalid CPU mask\n");
		ret = -EINVAL;
		goto out_free;
	}

	sz = sizeof(struct sdma_rht_map_elem);

	mutex_lock(&process_to_sde_mutex);

	for_each_cpu(cpu, mask) {
		struct sdma_rht_map_elem *elem;
		bool do_insert;

		/* Check if we have this already mapped */
		if (cpumask_test_cpu(cpu, &sde->cpu_mask)) {
			cpumask_set_cpu(cpu, new_mask);
			continue;
		}

		rht_node = rhashtable_lookup_fast(dd->sdma_rht, &cpu,
						  sdma_rht_params);

		do_insert = false;
		if (!rht_node) {
			rht_node = kzalloc_obj(*rht_node, GFP_KERNEL);
			if (!rht_node) {
				ret = -ENOMEM;
				goto out;
			}
			rht_node->cpu_id = cpu;
			/* insert later to allow free if there is an error */
			do_insert = true;
		}

		for (pidx = 0; pidx < dd->num_pports; pidx++) {
			vl = hfi2_sdma_engine_get_vl(&dd->pport[pidx], sde);
			if (unlikely(vl < 0 || vl >= HFI2_MAX_VLS_SUPPORTED)) {
				ret = -EINVAL;
				goto fail_new;
			}

			elem = rht_node->port_map[pidx][vl];
			if (!elem) {
				elem = kzalloc(sz, GFP_KERNEL);
				if (!elem) {
					ret = -ENOMEM;
					goto fail_new;
				}
				rht_node->port_map[pidx][vl] = elem;
			}

			if (WARN_ON_ONCE(elem->ctr >= TXE_NUM_SDMA_ENGINES)) {
				ret = -EINVAL;
				goto fail_new;
			}
			elem->sde[elem->ctr++] = sde;
			elem->mask = roundup_pow_of_two(elem->ctr) - 1;

			/* Populate the sde map table */
			sdma_populate_sde_map(elem);
		}

		if (do_insert) {
			ret = rhashtable_insert_fast(
				dd->sdma_rht, &rht_node->node, sdma_rht_params);
		}
		if (ret) {
fail_new:
			/* completely free node if not inserted yet */
			if (do_insert)
				sdma_rht_free(rht_node, dd);
			dd_dev_err(
				sde->dd,
				"Failed to set process to sde affinity for cpu %lu\n",
				cpu);
			goto out;
		}

		cpumask_set_cpu(cpu, new_mask);
	}

	/* Clean up old mappings */
	for_each_online_cpu(cpu) {
		struct sdma_rht_node *rht_node;
		bool empty = true;

		/* Don't cleanup sdes that are set in the new mask */
		if (cpumask_test_cpu(cpu, mask))
			continue;

		rht_node = rhashtable_lookup_fast(dd->sdma_rht, &cpu,
						  sdma_rht_params);
		if (!rht_node)
			continue;

		for (pidx = 0; pidx < dd->num_pports; pidx++) {
			int i;

			/* Remove mappings for old sde */
			for (i = 0; i < HFI2_MAX_VLS_SUPPORTED; i++)
				if (rht_node->port_map[pidx][i])
					sdma_cleanup_sde_map(
						rht_node->port_map[pidx][i],
						sde);

			/* check for populated entries */
			for (i = 0; i < HFI2_MAX_VLS_SUPPORTED; i++) {
				if (!rht_node->port_map[pidx][i])
					continue;

				if (rht_node->port_map[pidx][i]->ctr) {
					empty = false;
					break;
				}
			}
		}

		if (empty) {
			ret = rhashtable_remove_fast(
				dd->sdma_rht, &rht_node->node, sdma_rht_params);
			WARN_ON(ret);
			if (!ret) {
				synchronize_rcu();
				sdma_rht_free(rht_node, dd);
			}
		}
	}

	cpumask_copy(&sde->cpu_mask, new_mask);
out:
	mutex_unlock(&process_to_sde_mutex);
out_free:
	free_cpumask_var(mask);
	free_cpumask_var(new_mask);
	return ret ?: strnlen(buf, PAGE_SIZE);
}

ssize_t hfi2_sdma_get_cpu_to_sde_map(struct sdma_engine *sde, char *buf)
{
	mutex_lock(&process_to_sde_mutex);
	if (cpumask_empty(&sde->cpu_mask))
		snprintf(buf, PAGE_SIZE, "%s\n", "empty");
	else
		cpumap_print_to_pagebuf(true, buf, &sde->cpu_mask);
	mutex_unlock(&process_to_sde_mutex);
	return strnlen(buf, PAGE_SIZE);
}

static void sdma_rht_free(void *ptr, void *arg)
{
	struct sdma_rht_node *rht_node = ptr;
	struct hfi2_devdata *dd = arg;
	int pidx;
	int i;

	for (pidx = 0; pidx < dd->num_pports; pidx++) {
		for (i = 0; i < HFI2_MAX_VLS_SUPPORTED; i++)
			kfree(rht_node->port_map[pidx][i]);
	}
	kfree(rht_node);
}

/**
 * hfi2_sdma_seqfile_dump_cpu_list() - debugfs dump the cpu to sdma mappings
 * @s: seq file
 * @dd: hfi2_devdata
 * @cpuid: cpu id
 *
 * This routine dumps the process to sde mappings per cpu
 */
void hfi2_sdma_seqfile_dump_cpu_list(struct seq_file *s,
				     struct hfi2_devdata *dd,
				     unsigned long cpuid)
{
	struct sdma_rht_node *rht_node;
	int i, j;
	int pidx;

	rht_node =
		rhashtable_lookup_fast(dd->sdma_rht, &cpuid, sdma_rht_params);
	if (!rht_node)
		return;

	for (pidx = 0; pidx < dd->num_pports; pidx++) {
		if (pidx == 0)
			seq_printf(s, "cpu%3lu: ", cpuid);
		else
			seq_puts(s, "        ");
		seq_printf(s, "pidx %d: ", pidx);

		for (i = 0; i < HFI2_MAX_VLS_SUPPORTED; i++) {
			if (!rht_node->port_map[pidx][i] ||
			    !rht_node->port_map[pidx][i]->ctr)
				continue;

			seq_printf(s, " vl%d: [", i);

			for (j = 0; j < rht_node->port_map[pidx][i]->ctr; j++) {
				if (!rht_node->port_map[pidx][i]->sde[j])
					continue;

				if (j > 0)
					seq_puts(s, ",");

				seq_printf(s, " sdma%2d",
					   rht_node->port_map[pidx][i]
						   ->sde[j]
						   ->this_idx);
			}
			seq_puts(s, " ]");
		}

		seq_puts(s, "\n");
	}
}

/*
 * Free the indicated map struct
 */
static void sdma_map_free(struct sdma_vl_map *m)
{
	int i;

	for (i = 0; m && i < m->actual_vls; i++)
		kfree(m->map[i]);
	kfree(m);
}

/*
 * Handle RCU callback
 */
static void sdma_map_rcu_callback(struct rcu_head *list)
{
	struct sdma_vl_map *m = container_of(list, struct sdma_vl_map, list);

	sdma_map_free(m);
}

/**
 * hfi2_sdma_map_init - called when # vls change
 * @ppd: port structure
 * @hfi2_num_vls: number of vls
 * @vl_engines: per vl engine mapping (optional)
 *
 * This routine changes the mapping of VL to SDMA engine.
 *
 * vl_engines is used to specify a non-uniform vl/engine loading. NULL
 * implies auto computing the loading and giving each VLs a uniform
 * distribution of engines per VL.
 *
 * The auto algorithm computes the sde_per_vl and the number of extra
 * engines.  Any extra engines are added from the last VL on down.
 *
 * rcu locking is used here to control access to the mapping fields.
 *
 * If either the hfi2_num_vls or num_sdma are non-power of 2, the array sizes
 * in the struct sdma_vl_map and the struct sdma_map_elem are rounded
 * up to the next highest power of 2 and the first entry is reused
 * in a round robin fashion.
 *
 * If an error occurs the map change is not done and the mapping is
 * not changed.
 */
int hfi2_sdma_map_init(struct hfi2_pportdata *ppd, u8 hfi2_num_vls,
		       u8 *vl_engines)
{
	struct hfi2_devdata *dd = ppd->dd;
	struct hfi2_devrsrcs *dr = &dd->rsrcs;
	int i, j;
	int extra, sde_per_vl;
	int engine = dr->first_sdma_engine;
	int num_sdma = dr->last_sdma_engine - dr->first_sdma_engine;
	u8 lvl_engines[OPA_MAX_VLS];
	struct sdma_vl_map *oldmap, *newmap;

	if (!(dd->flags & HFI2_HAS_SEND_DMA))
		return 0;

	if (!vl_engines) {
		/* truncate divide */
		sde_per_vl = num_sdma / hfi2_num_vls;
		/* extras */
		extra = num_sdma % hfi2_num_vls;
		vl_engines = lvl_engines;
		/* add extras from last vl down */
		for (i = hfi2_num_vls - 1; i >= 0; i--, extra--)
			vl_engines[i] = sde_per_vl + (extra > 0 ? 1 : 0);
	}
	/* build new map */
	newmap = kzalloc(sizeof(struct sdma_vl_map) +
				 roundup_pow_of_two(hfi2_num_vls) *
					 sizeof(struct sdma_map_elem *),
			 GFP_KERNEL);
	if (!newmap)
		goto bail;
	newmap->actual_vls = hfi2_num_vls;
	newmap->vls = roundup_pow_of_two(hfi2_num_vls);
	newmap->mask = (1 << ilog2(newmap->vls)) - 1;
	/* initialize back-map */
	for (i = 0; i < TXE_NUM_SDMA_ENGINES; i++)
		newmap->engine_to_vl[i] = -1;
	for (i = 0; i < newmap->vls; i++) {
		/* save for wrap around */
		int first_engine = engine;

		if (i < newmap->actual_vls) {
			int sz = roundup_pow_of_two(vl_engines[i]);

			/* only allocate once */
			newmap->map[i] = kzalloc(
				sizeof(struct sdma_map_elem) +
					sz * sizeof(struct sdma_engine *),
				GFP_KERNEL);
			if (!newmap->map[i])
				goto bail;
			newmap->map[i]->mask = (1 << ilog2(sz)) - 1;
			/* assign engines */
			for (j = 0; j < sz; j++) {
				newmap->map[i]->sde[j] = &dd->per_sdma[engine];
				if (++engine >= first_engine + vl_engines[i])
					/* wrap back to first engine */
					engine = first_engine;
			}
			/* assign back-map */
			for (j = 0; j < vl_engines[i]; j++)
				newmap->engine_to_vl[first_engine + j] = i;
		} else {
			/* just re-use entry without allocating */
			newmap->map[i] = newmap->map[i % hfi2_num_vls];
		}
		engine = first_engine + vl_engines[i];
	}
	/* newmap in hand, save old map */
	spin_lock_irq(&dd->sde_map_lock);
	oldmap = rcu_dereference_protected(ppd->sdma_map,
					   lockdep_is_held(&dd->sde_map_lock));

	/* publish newmap */
	rcu_assign_pointer(ppd->sdma_map, newmap);

	spin_unlock_irq(&dd->sde_map_lock);
	/* success, free any old map after grace period */
	if (oldmap)
		call_rcu(&oldmap->list, sdma_map_rcu_callback);
	return 0;
bail:
	/* free any partial allocation */
	sdma_map_free(newmap);
	return -ENOMEM;
}

/**
 * hfi2_sdma_clean - Clean up allocated memory
 * @dd:          struct hfi2_devdata
 * @num_engines: num sdma engines
 *
 * This routine can be called regardless of the success of
 * hfi2_sdma_init()
 */
void hfi2_sdma_clean(struct hfi2_devdata *dd)
{
	struct hfi2_devrsrcs *dr = &dd->rsrcs;
	size_t i;
	struct sdma_engine *sde;
	int pidx;

	if (dd->sdma_pad_dma) {
		dma_free_coherent(&dd->pcidev->dev, SDMA_PAD,
				  (void *)dd->sdma_pad_dma, dd->sdma_pad_phys);
		dd->sdma_pad_dma = NULL;
		dd->sdma_pad_phys = 0;
	}
	if (dd->sdma_heads_dma) {
		dma_free_coherent(&dd->pcidev->dev, dd->sdma_heads_size,
				  (void *)dd->sdma_heads_dma,
				  dd->sdma_heads_phys);
		dd->sdma_heads_dma = NULL;
		dd->sdma_heads_phys = 0;
	}
	for (i = dr->first_sdma_engine;
	     dd->per_sdma && i < dr->last_sdma_engine; ++i) {
		sde = &dd->per_sdma[i];

		sde->head_dma = NULL;
		sde->head_phys = 0;

		if (sde->descq) {
			dma_free_coherent(&dd->pcidev->dev,
					  sde->descq_cnt * sizeof(u64[2]),
					  sde->descq, sde->descq_phys);
			sde->descq = NULL;
			sde->descq_phys = 0;
		}
		kvfree(sde->tx_ring);
		sde->tx_ring = NULL;
	}
	kfree(dd->per_sdma);
	dd->per_sdma = NULL;

	for (pidx = 0; pidx < dd->num_pports; pidx++) {
		struct hfi2_pportdata *ppd = dd->pport + pidx;

		if (rcu_access_pointer(ppd->sdma_map)) {
			spin_lock_irq(&dd->sde_map_lock);
			sdma_map_free(rcu_access_pointer(ppd->sdma_map));
			RCU_INIT_POINTER(ppd->sdma_map, NULL);
			spin_unlock_irq(&dd->sde_map_lock);
			synchronize_rcu();
		}
	}

	if (dd->sdma_rht) {
		rhashtable_free_and_destroy(dd->sdma_rht, sdma_rht_free, dd);
		kfree(dd->sdma_rht);
		dd->sdma_rht = NULL;
	}
}

static u32 sdma_per_engine_credits(struct hfi2_devdata *dd, u32 num_engines)
{
	u32 per_sdma_credits =
		chip_sdma_mem_size(dd) / (num_engines * SDMA_BLOCK_SIZE);
	u32 limit = jkr_sdma_credits_limit < 0 ?
			    JKR_DEFAULT_SDMA_CREDITS_LIMIT :
			    jkr_sdma_credits_limit;

	if (dd->params->chip_type != CHIP_JKR || !jkr_sdma_credits_limit)
		goto rounddown;

	if (limit > per_sdma_credits) {
		/*
		 * Only warn user if they actually set jkr_sdma_credits_limit,
		 * not if jkr_sdma_credits_limit > per_sdma_credits because of
		 * num_sdma.
		 */
		if (jkr_sdma_credits_limit > 0)
			dd_dev_info(
				dd,
				"Ignoring jkr_sdma_credits_limit (%u) > per_sdma_credits (%u)\n",
				limit, per_sdma_credits);
	} else if (limit < 2) {
		/* Min 2 to make sure JKR doesn't round down to 0 */
		dd_dev_info(dd, "Ignoring jkr_sdma_credits_limit %u < 2\n",
			    limit);
	} else {
		u32 was = per_sdma_credits;

		per_sdma_credits = limit & ~1;
		dd_dev_info(
			dd,
			"Setting per_sdma_credits to %u (was %u) from jkr_sdma_credits_limit module param\n",
			per_sdma_credits, was);
	}
rounddown:
	/* non-WFR hardware requires an even number of credits */
	if (dd->params->chip_type != CHIP_WFR && (per_sdma_credits & 1))
		per_sdma_credits &= ~1;

	return per_sdma_credits;
}

/* no-op SDMA descriptor */
static struct hw_sdma_desc sdma_pad;

/**
 * hfi2_sdma_init() - called when device probed
 * @dd: hfi2_devdata
 *
 * Initializes each sde and its csrs.
 * Interrupts are not required to be enabled.
 *
 * Returns:
 * 0 - success, -errno on failure
 */
int hfi2_sdma_init(struct hfi2_devdata *dd)
{
	struct hfi2_devrsrcs *dr = &dd->rsrcs;
	unsigned int this_idx;
	unsigned int start_bit;
	struct sdma_engine *sde;
	struct rhashtable *tmp_sdma_rht;
	u16 descq_cnt;
	u64 offset;
	struct hfi2_pportdata *ppd;
	u32 per_sdma_credits = 0; /* not used on VFs */
	u32 chip_engines;
	uint idle_cnt = sdma_idle_cnt;
	size_t num_engines = dd->num_sdma;
	int ret = -ENOMEM;
	int pidx;

	if (prime_sdma_memories(dd))
		return -EIO;

	if (num_engines == 0)
		return 0;

	if (!dd->is_vf) {
		dd_dev_info(dd, "SDMA hfi2_mod_num_sdma: %u\n",
			    hfi2_mod_num_sdma);
		dd_dev_info(dd, "SDMA chip_sdma_engines: %u\n",
			    chip_sdma_engines(dd));
		dd_dev_info(dd, "SDMA chip_sdma_mem_size: %u\n",
			    chip_sdma_mem_size(dd));

		per_sdma_credits = sdma_per_engine_credits(dd, num_engines);
	}

	dd->sdma_threshold = sdma_threshold;
	dd->pad_sdma_desc = pad_sdma_desc;
	dd->sdma_align = sdma_align;
	/* fill in defaults if parameter not specified */
	if (dd->params->chip_type == CHIP_JKR) {
		bool amd = boot_cpu_data.x86_vendor == X86_VENDOR_AMD;

		if (amd) {
			if (dd->sdma_threshold == -1)
				dd->sdma_threshold = 32;
			if (dd->pad_sdma_desc == -1)
				dd->pad_sdma_desc = 32;
			if (dd->sdma_align == -1)
				dd->sdma_align = ALIGN_256_HEAD_TAIL;
		} else {
			if (dd->sdma_threshold == -1)
				dd->sdma_threshold = 0;
			if (dd->pad_sdma_desc == -1)
				dd->pad_sdma_desc = 0;
			if (dd->sdma_align == -1)
				dd->sdma_align = ALIGN_NONE;
		}
	} else {
		if (dd->sdma_threshold == -1)
			dd->sdma_threshold = 0;
		if (dd->pad_sdma_desc == -1)
			dd->pad_sdma_desc = 0;
		if (dd->sdma_align == -1)
			dd->sdma_align = ALIGN_NONE;
	}
	/* sanity check parameters */
	switch (dd->pad_sdma_desc) {
	case 0:
	case 4:
	case 8:
	case 16:
	case 32:
		break;
	default:
		dd_dev_err(
			dd,
			"Invalid pad_sdma_desc parameter %d, setting to zero\n",
			dd->pad_sdma_desc);
		dd->pad_sdma_desc = 0;
		break;
	}
	switch (dd->sdma_align) {
	case ALIGN_NONE:
	case ALIGN_256_ALL:
	case ALIGN_256_HEAD_TAIL:
	case ALIGN_256_TAIL:
		break;
	default:
		dd_dev_err(dd,
			   "Invalid sdma_align parameter %d, setting to %d\n",
			   dd->sdma_align, ALIGN_256_HEAD_TAIL);
		dd->sdma_align = ALIGN_256_HEAD_TAIL;
		break;
	}
	dd_dev_info(dd, "SDMA threshold,pad,align: %d,%d,%d\n",
		    dd->sdma_threshold, dd->pad_sdma_desc, dd->sdma_align);

	/* set up freeze waitqueue */
	init_waitqueue_head(&dd->sdma_unfreeze_wq);
	atomic_set(&dd->sdma_unfreeze_count, 0);

	atomic_set(&dd->sdma_print_tag, 0);
	descq_cnt = hfi2_sdma_get_descq_cnt();
	dd_dev_info(dd, "SDMA engines %zu descq_cnt %u\n", num_engines,
		    descq_cnt);

	/* alloc memory for array of send engines */
	dd->per_sdma = kcalloc_node(num_engines, sizeof(*dd->per_sdma),
				    GFP_KERNEL, dd->node);
	if (!dd->per_sdma)
		return ret;

	idle_cnt = hfi2_ns_to_cclock(dd, idle_cnt);
	if (idle_cnt)
		dd->default_desc1 = SDMA_DESC1_HEAD_TO_HOST_FLAG;
	else
		dd->default_desc1 = SDMA_DESC1_INT_REQ_FLAG;

	if (!sdma_desct_intr)
		sdma_desct_intr = SDMA_DESC_INTR;

	/*
	 * The driver is coded to assume that all masks fit in a single
	 * 64-bit interrupt source vector entry.  Enforce that here.
	 */
	start_bit = dd->params->is_sdma_start % 64;
	if (start_bit + (3 * TXE_NUM_SDMA_ENGINES) > 64) {
		dd_dev_err(dd, "invalid SDMA interrupt masks\n");
		return -EINVAL;
	}

	/* Allocate memory for SendDMA descriptor FIFOs */
	for (this_idx = dr->first_sdma_engine; this_idx < dr->last_sdma_engine;
	     ++this_idx) {
		sde = &dd->per_sdma[this_idx];
		sde->dd = dd;
		sde->this_idx = this_idx;
		sde->descq_cnt = descq_cnt;
		sde->desc_avail = sdma_descq_freecnt(sde);
		sde->sdma_shift = ilog2(descq_cnt);
		sde->sdma_mask = (1 << sde->sdma_shift) - 1;

		/* Create a mask specifically for each interrupt source */
		sde->int_mask = (u64)1 << (0 * TXE_NUM_SDMA_ENGINES + this_idx +
					   start_bit);
		sde->progress_mask = (u64)1 << (1 * TXE_NUM_SDMA_ENGINES +
						this_idx + start_bit);
		sde->idle_mask = (u64)1 << (2 * TXE_NUM_SDMA_ENGINES +
					    this_idx + start_bit);
		/* Create a combined mask to cover all 3 interrupt sources */
		sde->imask = sde->int_mask | sde->progress_mask |
			     sde->idle_mask;

		spin_lock_init(&sde->tail_lock);
		seqlock_init(&sde->head_lock);
		spin_lock_init(&sde->senddmactrl_lock);
		spin_lock_init(&sde->flushlist_lock);
		seqlock_init(&sde->waitlock);
		/* insure there is always a zero bit */
		sde->ahg_bits = 0xfffffffe00000000ULL;

		sdma_set_state(sde, sdma_state_s00_hw_down);

		/* set up reference counting */
		kref_init(&sde->state.kref);
		init_completion(&sde->state.comp);

		INIT_LIST_HEAD(&sde->flushlist);
		INIT_LIST_HEAD(&sde->dmawait);

		sde->tail_csr = get_sdma_csr_addr(
			dd, this_idx, dd->params->send_dma_tail_reg);

		INIT_WORK(&sde->sdma_hw_clean_up_work, sdma_hw_clean_up_worker);
		INIT_WORK(&sde->sdma_sw_clean_up_work, sdma_sw_clean_up_worker);
		INIT_WORK(&sde->err_halt_worker, sdma_err_halt_wait);
		INIT_WORK(&sde->flush_worker, sdma_field_flush);

		sde->progress_check_head = 0;

		timer_setup(&sde->err_progress_check_timer,
			    sdma_err_progress_check, 0);

		sde->descq = dma_alloc_coherent(&dd->pcidev->dev,
						descq_cnt * sizeof(u64[2]),
						&sde->descq_phys, GFP_KERNEL);
		if (!sde->descq)
			goto bail;
		sde->tx_ring = kvzalloc_node(
			array_size(descq_cnt, sizeof(struct sdma_txreq *)),
			GFP_KERNEL, dd->node);
		if (!sde->tx_ring)
			goto bail;
	}
	if (!dd->is_vf) {
		/* Clear SendDmaCfgMemory on disabled engines */
		chip_engines = chip_sdma_engines(dd);
		for (this_idx = num_engines; this_idx < chip_engines;
		     ++this_idx)
			write_sdmacfg_csr(dd, this_idx,
					  dd->params->send_dma_cfg_memory_reg,
					  0);
	}

	dd->sdma_heads_size =
		L1_CACHE_BYTES * (dr->last_sdma_engine - dr->first_sdma_engine);
	/* Allocate memory for DMA of head registers to memory */
	dd->sdma_heads_dma =
		dma_alloc_coherent(&dd->pcidev->dev, dd->sdma_heads_size,
				   &dd->sdma_heads_phys, GFP_KERNEL);
	if (!dd->sdma_heads_dma) {
		dd_dev_err(dd, "failed to allocate SendDMA head memory\n");
		goto bail;
	}

	/* Allocate memory for pad */
	dd->sdma_pad_dma = dma_alloc_coherent(&dd->pcidev->dev, SDMA_PAD,
					      &dd->sdma_pad_phys, GFP_KERNEL);
	if (!dd->sdma_pad_dma) {
		dd_dev_err(dd, "failed to allocate SendDMA pad memory\n");
		goto bail;
	}

	/* setup credits for all SDMA engines, only on PF0 (before SiIdx is set) */
	if (!dd->is_vf) {
		for (this_idx = 0; this_idx < dd->num_sdma; ++this_idx) {
			/* dd->per_sdma[this_idx] are not initialized for all engines */
			write_sdmacfg_csr(
				dd, this_idx,
				dd->params->send_dma_cfg_memory_reg,
				((u64)per_sdma_credits
				 << SD(MEMORY_SDMA_MEMORY_CNT_SHIFT)) |
					((u64)(per_sdma_credits * this_idx)
					 << SD(MEMORY_SDMA_MEMORY_INDEX_SHIFT)));
		}
	}

	/* assign each engine to a different cacheline and init registers */
	offset = 0;
	for (this_idx = dr->first_sdma_engine; this_idx < dr->last_sdma_engine;
	     ++this_idx) {
		sde = &dd->per_sdma[this_idx];
		sde->head_dma = (void *)dd->sdma_heads_dma + offset;
		sde->head_phys = dd->sdma_heads_phys + offset;
		offset += L1_CACHE_BYTES;
		init_sdma_regs(sde, idle_cnt);
	}
	dd->flags |= HFI2_HAS_SEND_DMA;
	dd->flags |= idle_cnt ? HFI2_HAS_SDMA_TIMEOUT : 0;

	for (pidx = 0; pidx < dd->num_pports; pidx++) {
		ppd = dd->pport + pidx;
		ret = hfi2_sdma_map_init(ppd, ppd->vls_operational, NULL);
		if (ret < 0)
			goto bail;
	}

	tmp_sdma_rht = kzalloc(sizeof(*tmp_sdma_rht), GFP_KERNEL);
	if (!tmp_sdma_rht) {
		ret = -ENOMEM;
		goto bail;
	}

	ret = rhashtable_init(tmp_sdma_rht, &sdma_rht_params);
	if (ret < 0) {
		kfree(tmp_sdma_rht);
		goto bail;
	}

	dd->sdma_rht = tmp_sdma_rht;

	dd_dev_info(dd, "SDMA num_sdma: %u\n", dd->num_sdma);
	return 0;

bail:
	hfi2_sdma_clean(dd);
	return ret;
}

/**
 * hfi2_sdma_all_running() - called when the link goes up
 * @dd: hfi2_devdata
 *
 * This routine moves all engines to the running state.
 */
void hfi2_sdma_all_running(struct hfi2_devdata *dd)
{
	struct hfi2_devrsrcs *dr = &dd->rsrcs;
	struct sdma_engine *sde;
	unsigned int i;

	/* move all engines to running */
	for (i = dr->first_sdma_engine; i < dr->last_sdma_engine; ++i) {
		sde = &dd->per_sdma[i];
		sdma_process_event(sde, sdma_event_e30_go_running);
	}
}

/**
 * hfi2_sdma_start() - called to kick off state processing for all engines
 * @dd: hfi2_devdata
 *
 * This routine is for kicking off the state processing for all required
 * sdma engines.  Interrupts need to be working at this point.
 *
 */
void hfi2_sdma_start(struct hfi2_devdata *dd)
{
	struct hfi2_devrsrcs *dr = &dd->rsrcs;
	unsigned i;
	struct sdma_engine *sde;

	/* kick off the engines state processing */
	for (i = dr->first_sdma_engine; i < dr->last_sdma_engine; ++i) {
		sde = &dd->per_sdma[i];
		sdma_process_event(sde, sdma_event_e10_go_hw_start);
	}

	/* tell all engines to go running */
	hfi2_sdma_all_running(dd);
}

/**
 * hfi2_sdma_exit() - used when module is removed
 * @dd: hfi2_devdata
 */
void hfi2_sdma_exit(struct hfi2_devdata *dd)
{
	struct hfi2_devrsrcs *dr = &dd->rsrcs;
	unsigned this_idx;
	struct sdma_engine *sde;

	/*
	 * In the mean time, can only exit engines known to us.
	 */
	for (this_idx = dr->first_sdma_engine;
	     dd->per_sdma && this_idx < dr->last_sdma_engine; ++this_idx) {
		sde = &dd->per_sdma[this_idx];
		if (!list_empty(&sde->dmawait))
			dd_dev_err(dd, "sde %u: dmawait list not empty!\n",
				   sde->this_idx);
		sdma_process_event(sde, sdma_event_e00_go_hw_down);

		timer_delete_sync(&sde->err_progress_check_timer);

		cancel_work_sync(&sde->flush_worker);
		cancel_work_sync(&sde->err_halt_worker);
		cancel_work_sync(&sde->sdma_hw_clean_up_work);

		/*
		 * This waits for the state machine to exit so it is not
		 * necessary to kill the sdma_sw_clean_up_worker to make sure
		 * it is not running.
		 */
		sdma_finalput(&sde->state);
	}
}

/*
 * unmap the indicated descriptor
 */
static inline void sdma_unmap_desc(struct hfi2_devdata *dd,
				   struct sdma_desc *descp, u8 map_type)
{
	switch (map_type) {
	case SDMA_MAP_SINGLE:
		dma_unmap_single(&dd->pcidev->dev, sdma_mapping_addr(dd, descp),
				 sdma_mapping_len(dd, descp), DMA_TO_DEVICE);
		break;
	case SDMA_MAP_PAGE:
		dma_unmap_page(&dd->pcidev->dev, sdma_mapping_addr(dd, descp),
			       sdma_mapping_len(dd, descp), DMA_TO_DEVICE);
		break;
	}
}

/*
 * return the mode as indicated by the first
 * descriptor in the tx.
 */
static inline u8 ahg_mode(struct sdma_txreq *tx)
{
	return (tx->descp[0].qw[1] & SDMA_DESC1_HEADER_MODE_SMASK) >>
	       SDMA_DESC1_HEADER_MODE_SHIFT;
}

/**
 * __hfi2_sdma_txclean() - clean tx of mappings, descp *kmalloc's
 * @dd: hfi2_devdata for unmapping
 * @tx: tx request to clean
 *
 * This is used in the progress routine to clean the tx or
 * by the ULP to toss an in-process tx build.
 *
 * The code can be called multiple times without issue.
 *
 */
void __hfi2_sdma_txclean(struct hfi2_devdata *dd, struct sdma_txreq *tx)
{
	u16 i;

	if (tx->num_desc) {
		u8 skip = 0, mode = ahg_mode(tx);

		/* unmap first */
		sdma_unmap_desc(dd, &tx->descp[0], sdma_get_map_type(tx, 0));
		/* determine number of AHG descriptors to skip */
		if (mode > SDMA_AHG_APPLY_UPDATE1)
			skip = mode >> 1;
		for (i = 1 + skip; i < tx->num_desc; i++)
			sdma_unmap_desc(dd, &tx->descp[i],
					sdma_get_map_type(tx, i));
		tx->num_desc = 0;
	}
	kfree(tx->coalesce_buf);
	tx->coalesce_buf = NULL;
	/* kmalloc'ed descp */
	if (unlikely(tx->descp != tx->descs)) {
		kfree(tx->descp);
		tx->descp = tx->descs;
		tx->desc_limit = ARRAY_SIZE(tx->descs);
	}
}

static inline u16 sdma_gethead(struct sdma_engine *sde)
{
	struct hfi2_devdata *dd = sde->dd;
	int use_dmahead;
	u16 hwhead;

#ifdef CONFIG_HFI2_SDMA_VERBOSITY
	dd_dev_err(sde->dd, "CONFIG SDMA(%u) %s:%d %s()\n", sde->this_idx,
		   slashstrip(__FILE__), __LINE__, __func__);
#endif

retry:
	use_dmahead = HFI2_CAP_IS_KSET(USE_SDMA_HEAD) && __sdma_running(sde) &&
		      (dd->flags & HFI2_HAS_SDMA_TIMEOUT);
	hwhead = use_dmahead ?
			 (u16)le64_to_cpu(*sde->head_dma) :
			 (u16)read_sde_csr(sde, dd->params->send_dma_head_reg);

	if (unlikely(HFI2_CAP_IS_KSET(SDMA_HEAD_CHECK))) {
		u16 cnt;
		u16 swtail;
		u16 swhead;
		int sane;

		swhead = sde->descq_head & sde->sdma_mask;
		/* this code is really bad for cache line trading */
		swtail = READ_ONCE(sde->descq_tail) & sde->sdma_mask;
		cnt = sde->descq_cnt;

		if (swhead < swtail)
			/* not wrapped */
			sane = (hwhead >= swhead) & (hwhead <= swtail);
		else if (swhead > swtail)
			/* wrapped around */
			sane = ((hwhead >= swhead) && (hwhead < cnt)) ||
			       (hwhead <= swtail);
		else
			/* empty */
			sane = (hwhead == swhead);

		if (unlikely(!sane)) {
			dd_dev_err(
				dd,
				"SDMA(%u) bad head (%s) hwhd=%u swhd=%u swtl=%u cnt=%u\n",
				sde->this_idx, use_dmahead ? "dma" : "kreg",
				hwhead, swhead, swtail, cnt);
			if (use_dmahead) {
				/* try one more time, using csr */
				use_dmahead = 0;
				goto retry;
			}
			/* proceed as if no progress */
			hwhead = swhead;
		}
	}
	return hwhead;
}

/*
 * This is called when there are send DMA descriptors that might be
 * available.
 *
 * This is called with head_lock held.
 */
static void sdma_desc_avail(struct sdma_engine *sde, uint avail)
{
	struct iowait *wait, *nw, *twait;
	struct iowait *waits[SDMA_WAIT_BATCH_SIZE];
	uint i, n = 0, seq, tidx = 0;

#ifdef CONFIG_HFI2_SDMA_VERBOSITY
	dd_dev_err(sde->dd, "CONFIG SDMA(%u) %s:%d %s()\n", sde->this_idx,
		   slashstrip(__FILE__), __LINE__, __func__);
	dd_dev_err(sde->dd, "avail: %u\n", avail);
#endif

	do {
		seq = read_seqbegin(&sde->waitlock);
		if (!list_empty(&sde->dmawait)) {
			/* at least one item */
			write_seqlock(&sde->waitlock);
			/* Harvest waiters wanting DMA descriptors */
			list_for_each_entry_safe(wait, nw, &sde->dmawait,
						 list) {
				u32 num_desc;

				if (!wait->wakeup)
					continue;
				if (n == ARRAY_SIZE(waits))
					break;
				iowait_init_priority(wait);
				num_desc = iowait_get_all_desc(wait);
				if (num_desc > avail)
					break;
				avail -= num_desc;
				/* Find the top-priority wait memeber */
				if (n) {
					twait = waits[tidx];
					tidx = hfi2_iowait_priority_update_top(
						wait, twait, n, tidx);
				}
				list_del_init(&wait->list);
				waits[n++] = wait;
			}
			write_sequnlock(&sde->waitlock);
			break;
		}
	} while (read_seqretry(&sde->waitlock, seq));

	/* Schedule the top-priority entry first */
	if (n)
		waits[tidx]->wakeup(waits[tidx], SDMA_AVAIL_REASON);

	for (i = 0; i < n; i++)
		if (i != tidx)
			waits[i]->wakeup(waits[i], SDMA_AVAIL_REASON);
}

/* head_lock must be held */
static void sdma_make_progress(struct sdma_engine *sde, u64 status)
{
	struct sdma_txreq *txp = NULL;
	int progress = 0;
	u16 hwhead, swhead;
	int idle_check_done = 0;

	hwhead = sdma_gethead(sde);

	/* The reason for some of the complexity of this code is that
	 * not all descriptors have corresponding txps.  So, we have to
	 * be able to skip over descs until we wander into the range of
	 * the next txp on the list.
	 */

retry:
	txp = get_txhead(sde);
	swhead = sde->descq_head & sde->sdma_mask;
	trace_hfi2_sdma_progress(sde, hwhead, swhead, txp);
	while (swhead != hwhead) {
		/* advance head, wrap if needed */
		swhead = ++sde->descq_head & sde->sdma_mask;

		/* if now past this txp's descs, do the callback */
		if (txp && txp->next_descq_idx == swhead) {
			/* remove from list */
			sde->tx_ring[sde->tx_head++ & sde->sdma_mask] = NULL;
			complete_tx(sde, txp, SDMA_TXREQ_S_OK);
			/* see if there is another txp */
			txp = get_txhead(sde);
		}
		trace_hfi2_sdma_progress(sde, hwhead, swhead, txp);
		progress++;
	}

	/*
	 * The SDMA idle interrupt is not guaranteed to be ordered with respect
	 * to updates to the dma_head location in host memory. The head
	 * value read might not be fully up to date. If there are pending
	 * descriptors and the SDMA idle interrupt fired then read from the
	 * CSR SDMA head instead to get the latest value from the hardware.
	 * The hardware SDMA head should be read at most once in this invocation
	 * of sdma_make_progress(..) which is ensured by idle_check_done flag
	 */
	if ((status & sde->idle_mask) && !idle_check_done) {
		u16 swtail;

		swtail = READ_ONCE(sde->descq_tail) & sde->sdma_mask;
		if (swtail != hwhead) {
			hwhead = (u16)read_sde_csr(
				sde, sde->dd->params->send_dma_head_reg);
			idle_check_done = 1;
			goto retry;
		}
	}

	sde->last_status = status;
	if (progress)
		sdma_desc_avail(sde, sdma_descq_freecnt(sde));
}

bool hfi2_sdma_work_pending(struct sdma_engine *sde)
{
	u16 hwhead, swhead;

	hwhead = sdma_gethead(sde);
	swhead = sde->descq_head & sde->sdma_mask;
	return (swhead != hwhead);
}

/*
 * hfi2_sdma_engine_interrupt() - interrupt handler for engine
 * @sde: sdma engine
 * @status: sdma interrupt reason
 *
 * Status is a mask of the 3 possible interrupts for this engine.  It will
 * contain bits _only_ for this SDMA engine.  It will contain at least one
 * bit, it may contain more.
 */
void hfi2_sdma_engine_interrupt(struct sdma_engine *sde, u64 status)
{
	unsigned long flags;

	trace_hfi2_sdma_engine_interrupt(sde, status);
	write_seqlock_irqsave(&sde->head_lock, flags);
	sdma_set_desc_cnt(sde, sdma_desct_intr);
	if (status & sde->idle_mask)
		sde->idle_int_cnt++;
	else if (status & sde->progress_mask)
		sde->progress_int_cnt++;
	else if (status & sde->int_mask)
		sde->sdma_int_cnt++;
	sdma_make_progress(sde, status);
	write_sequnlock_irqrestore(&sde->head_lock, flags);
}

/**
 * hfi2_sdma_engine_error() - error handler for engine
 * @sde: sdma engine
 * @status: sdma interrupt reason
 */
void hfi2_sdma_engine_error(struct sdma_engine *sde, u64 status)
{
	unsigned long flags;

#ifdef CONFIG_HFI2_SDMA_VERBOSITY
	dd_dev_err(sde->dd, "CONFIG SDMA(%u) error status 0x%llx state %s\n",
		   sde->this_idx, (unsigned long long)status,
		   sdma_state_names[sde->state.current_state]);
#endif
	spin_lock_irqsave(&sde->tail_lock, flags);
	write_seqlock(&sde->head_lock);
	if (status & ALL_SDMA_ENG_HALT_ERRS)
		__sdma_process_event(sde, sdma_event_e60_hw_halted);
	/* only print if not (SDmaHaltErr or a correctable error) */
	if (status &
	    ~(SD(ENG_ERR_STATUS_SDMA_HALT_ERR_SMASK) | ALL_SDMA_ENG_COR_ERRS)) {
		dd_dev_err(sde->dd, "SDMA (%u) engine error: 0x%llx state %s\n",
			   sde->this_idx, (unsigned long long)status,
			   sdma_state_names[sde->state.current_state]);
		dump_sdma_state(sde);
	}
	write_sequnlock(&sde->head_lock);
	spin_unlock_irqrestore(&sde->tail_lock, flags);
}

static void sdma_sendctrl(struct sdma_engine *sde, unsigned int op)
{
	struct hfi2_devdata *dd = sde->dd;
	u64 set_senddmactrl = 0;
	u64 clr_senddmactrl = 0;
	unsigned long flags;

#ifdef CONFIG_HFI2_SDMA_VERBOSITY
	dd_dev_err(sde->dd, "CONFIG SDMA(%u) senddmactrl E=%d I=%d H=%d C=%d\n",
		   sde->this_idx, (op & SDMA_SENDCTRL_OP_ENABLE) ? 1 : 0,
		   (op & SDMA_SENDCTRL_OP_INTENABLE) ? 1 : 0,
		   (op & SDMA_SENDCTRL_OP_HALT) ? 1 : 0,
		   (op & SDMA_SENDCTRL_OP_CLEANUP) ? 1 : 0);
#endif

	if (op & SDMA_SENDCTRL_OP_ENABLE)
		set_senddmactrl |= SD(CTRL_SDMA_ENABLE_SMASK);
	else
		clr_senddmactrl |= SD(CTRL_SDMA_ENABLE_SMASK);

	if (op & SDMA_SENDCTRL_OP_INTENABLE)
		set_senddmactrl |= SD(CTRL_SDMA_INT_ENABLE_SMASK);
	else
		clr_senddmactrl |= SD(CTRL_SDMA_INT_ENABLE_SMASK);

	if (op & SDMA_SENDCTRL_OP_HALT)
		set_senddmactrl |= SD(CTRL_SDMA_HALT_SMASK);
	else
		clr_senddmactrl |= SD(CTRL_SDMA_HALT_SMASK);

	spin_lock_irqsave(&sde->senddmactrl_lock, flags);

	sde->p_senddmactrl |= set_senddmactrl;
	sde->p_senddmactrl &= ~clr_senddmactrl;
	/* conditionally set SDmaSingleDescriptor */
	if (sdma_single_descriptor)
		sde->p_senddmactrl |= (1uLL << 5);
	/* conditionally set SDmaThresholdEnable - JKR only */
	if (dd->sdma_threshold) {
		sde->p_senddmactrl |= (1uLL << 4);
		write_sde_csr(sde, dd->params->send_dma_priority_thld_reg,
			      dd->sdma_threshold);
	}

	if (op & SDMA_SENDCTRL_OP_CLEANUP)
		write_sde_csr(sde, dd->params->send_dma_ctrl_reg,
			      sde->p_senddmactrl | SD(CTRL_SDMA_CLEANUP_SMASK));
	else
		write_sde_csr(sde, dd->params->send_dma_ctrl_reg,
			      sde->p_senddmactrl);

	spin_unlock_irqrestore(&sde->senddmactrl_lock, flags);

#ifdef CONFIG_HFI2_SDMA_VERBOSITY
	sdma_dumpstate(sde);
#endif
}

static void sdma_setlengen(struct sdma_engine *sde)
{
#ifdef CONFIG_HFI2_SDMA_VERBOSITY
	dd_dev_err(sde->dd, "CONFIG SDMA(%u) %s:%d %s()\n", sde->this_idx,
		   slashstrip(__FILE__), __LINE__, __func__);
#endif

	/*
	 * Set SendDmaLenGen and clear-then-set the MSB of the generation
	 * count to enable generation checking and load the internal
	 * generation counter.
	 */
	write_sde_csr(sde, sde->dd->params->send_dma_len_gen_reg,
		      (sde->descq_cnt / 64) << SD(LEN_GEN_LENGTH_SHIFT));
	write_sde_csr(sde, sde->dd->params->send_dma_len_gen_reg,
		      ((sde->descq_cnt / 64) << SD(LEN_GEN_LENGTH_SHIFT)) |
			      (4ULL << SD(LEN_GEN_GENERATION_SHIFT)));
}

static inline void sdma_update_tail(struct sdma_engine *sde, u16 tail)
{
	/* Commit writes to memory and advance the tail on the chip */
	smp_wmb(); /* see get_txhead() */
	writeq(tail, sde->tail_csr);
}

/*
 * This is called when changing to state s10_hw_start_up_halt_wait as
 * a result of send buffer errors or send DMA descriptor errors.
 */
static void sdma_hw_start_up(struct sdma_engine *sde)
{
	u64 reg;

#ifdef CONFIG_HFI2_SDMA_VERBOSITY
	dd_dev_err(sde->dd, "CONFIG SDMA(%u) %s:%d %s()\n", sde->this_idx,
		   slashstrip(__FILE__), __LINE__, __func__);
#endif

	sdma_setlengen(sde);
	sdma_update_tail(sde, 0); /* Set SendDmaTail */
	*sde->head_dma = 0;

	reg = SD(ENG_ERR_CLEAR_SDMA_HEADER_REQUEST_FIFO_UNC_ERR_MASK)
	      << SD(ENG_ERR_CLEAR_SDMA_HEADER_REQUEST_FIFO_UNC_ERR_SHIFT);
	write_sde_csr(sde, sde->dd->params->send_dma_eng_err_clear_reg, reg);
}

/*
 * set_sdma_integrity
 *
 * Set the SEND_DMA_CHECK_ENABLE register for send DMA engine 'sde'.
 */
static void set_sdma_integrity(struct sdma_engine *sde)
{
	struct hfi2_devdata *dd = sde->dd;

	write_sde_csr(sde, SD(CHECK_ENABLE), hfi2_pkt_base_sdma_integrity(dd));
}

static void init_sdma_regs(struct sdma_engine *sde, uint idle_cnt)
{
	u64 opval, opmask;
	struct hfi2_devdata *dd = sde->dd;

#ifdef CONFIG_HFI2_SDMA_VERBOSITY
	dd_dev_err(dd, "CONFIG SDMA(%u) %s:%d %s()\n", sde->this_idx,
		   slashstrip(__FILE__), __LINE__, __func__);
#endif

	write_sde_csr(sde, dd->params->send_dma_base_addr_reg, sde->descq_phys);
	sdma_setlengen(sde);
	sdma_update_tail(sde, 0); /* Set SendDmaTail */
	write_sde_csr(sde, dd->params->send_dma_reload_cnt_reg, idle_cnt);
	write_sde_csr(sde, dd->params->send_dma_desc_cnt_reg, 0);
	write_sde_csr(sde, dd->params->send_dma_head_addr_reg, sde->head_phys);
	write_sde_csr(sde, dd->params->send_dma_eng_err_mask_reg, ~0ull);
	if (dd->params->chip_type == CHIP_WFR) {
		/* SEND_DMA_CHECK_* are WFR only */
		set_sdma_integrity(sde);
		opmask = OPCODE_CHECK_MASK_DISABLED;
		opval = OPCODE_CHECK_VAL_DISABLED;
		write_sde_csr(sde, SD(CHECK_OPCODE),
			      (opmask << SEND_CTXT_CHECK_OPCODE_MASK_SHIFT) |
				      (opval
				       << SEND_CTXT_CHECK_OPCODE_VALUE_SHIFT));
	}
}

#ifdef CONFIG_HFI2_SDMA_VERBOSITY

#define sdma_dumpstate_helper0(reg)                                     \
	do {                                                            \
		csr = hfi2_read_csr(sde->dd, reg);                      \
		dd_dev_err(sde->dd, "%41s     0x%016llx\n", #reg, csr); \
	} while (0)

#define sdma_dumpstate_helper(reg)                                  \
	do {                                                        \
		csr = read_sde_csr(sde, reg);                       \
		dd_dev_err(sde->dd, "%41s[%02u] 0x%016llx\n", #reg, \
			   sde->this_idx, csr);                     \
	} while (0)

#define sdma_dumpstate_helper1(reg)                                 \
	do {                                                        \
		csr = read_sdecfg_csr(sde, reg);                    \
		dd_dev_err(sde->dd, "%41s[%02u] 0x%016llx\n", #reg, \
			   sde->this_idx, csr);                     \
	} while (0)

/* interrupt status */
static void sdma_dumpstate_int(struct sdma_engine *sde, u32 is_base,
			       const char *what)
{
	struct hfi2_devdata *dd = sde->dd;
	u32 is_num = is_base + sde->this_idx;
	u32 reg_off = 8 * (is_num / BITS_PER_REGISTER);
	u64 reg_mask = BIT_ULL(is_num % BITS_PER_REGISTER);
	int status;
	int mask;
	int blocked;

	status =
		!!(hfi2_read_csr(dd, dd->params->cce_int_status_reg + reg_off) &
		   reg_mask);
	mask = !!(hfi2_read_csr(dd, dd->params->cce_int_mask_reg + reg_off) &
		  reg_mask);
	blocked = !!(
		hfi2_read_csr(dd, dd->params->cce_int_blocked_reg + reg_off) &
		reg_mask);

	dd_dev_err(dd, "%41s[%02u] status:%d mask:%d blocked:%d\n", what,
		   sde->this_idx, status, mask, blocked);
}

void sdma_dumpstate(struct sdma_engine *sde)
{
	struct hfi2_devdata *dd = sde->dd;
	u64 csr;

	sdma_dumpstate_helper(dd->params->send_dma_ctrl_reg);
	sdma_dumpstate_helper(dd->params->send_dma_status_reg);
	sdma_dumpstate_helper0(dd->params->send_dma_err_status_reg);
	sdma_dumpstate_helper0(dd->params->send_dma_err_mask_reg);
	sdma_dumpstate_helper(dd->params->send_dma_eng_err_status_reg);
	sdma_dumpstate_helper(dd->params->send_dma_eng_err_mask_reg);

	sdma_dumpstate_int(sde, dd->params->is_sdma_start, "SdmaInt");
	sdma_dumpstate_int(sde, dd->params->is_sdma_progress_start,
			   "SdmaProgressInt");
	sdma_dumpstate_int(sde, dd->params->is_sdma_idle_start, "SdmaIdleInt");

	sdma_dumpstate_helper(dd->params->send_dma_tail_reg);
	sdma_dumpstate_helper(dd->params->send_dma_head_reg);
	sdma_dumpstate_helper(dd->params->send_dma_priority_thld_reg);
	sdma_dumpstate_helper(dd->params->send_dma_idle_cnt_reg);
	sdma_dumpstate_helper(dd->params->send_dma_reload_cnt_reg);
	sdma_dumpstate_helper(dd->params->send_dma_desc_cnt_reg);
	sdma_dumpstate_helper(dd->params->send_dma_desc_fetched_cnt_reg);
	sdma_dumpstate_helper1(dd->params->send_dma_cfg_memory_reg);
	sdma_dumpstate_helper0(dd->params->send_dma_engines_reg);
	sdma_dumpstate_helper0(dd->params->send_dma_mem_size_reg);
	/* sdma_dumpstate_helper(SEND_EGRESS_SEND_DMA_STATUS);  */
	sdma_dumpstate_helper(dd->params->send_dma_base_addr_reg);
	sdma_dumpstate_helper(dd->params->send_dma_len_gen_reg);
	sdma_dumpstate_helper(dd->params->send_dma_head_addr_reg);
	if (dd->params->chip_type == CHIP_WFR) {
		/* SEND_DMA_CHECK_* are WFR only */
		sdma_dumpstate_helper(SD(CHECK_ENABLE));
		sdma_dumpstate_helper(SD(CHECK_VL));
		sdma_dumpstate_helper(SD(CHECK_JOB_KEY));
		sdma_dumpstate_helper(SD(CHECK_PARTITION_KEY));
		sdma_dumpstate_helper(SD(CHECK_SLID));
		sdma_dumpstate_helper(SD(CHECK_OPCODE));
	}
}
#endif

/*
 * Translate the SDMA descriptor (qw) into human readable text.
 *
 * Output is one or two buffers.  Both buffers will always be initialized into
 * valid strings.  The second buffer is empty if it is an empty string.  Both
 * buffers are expected to be of length QW_BUF_SIZE.
 */
#define QW_BUF_SIZE 128
static void sdma_qw_strings(struct hfi2_devdata *dd, u32 idx, u64 *qw,
			    char *buf0, char *buf1)
{
	u64 addr;
	u32 len;
	u32 gen;
	char flags[6];
	bool first = sdma_qw_get(dd, first_desc, qw);

	flags[0] = (qw[1] & SDMA_DESC1_INT_REQ_FLAG) ? 'I' : '-';
	flags[1] = (qw[1] & SDMA_DESC1_HEAD_TO_HOST_FLAG) ? 'H' : '-';
	flags[2] = first ? 'F' : '-';
	flags[3] = sdma_qw_get(dd, last_desc, qw) ? 'L' : '-';
	flags[4] = 0; /* terminate */
	addr = sdma_qw_get(dd, phy_addr, qw);
	gen = (qw[1] >> SDMA_DESC1_GENERATION_SHIFT) &
	      SDMA_DESC1_GENERATION_MASK;
	len = sdma_qw_get(dd, byte_count, qw);
	scnprintf(
		buf0, QW_BUF_SIZE,
		"desc[%u]: flags:%s addr:0x%016llx gen:%u len:%u d0:0x%016llx d1:0x%016llx",
		idx, flags, addr, gen, len, qw[0], qw[1]);
	if (first) {
		scnprintf(buf1, QW_BUF_SIZE, "aidx:%u amode:%u alen:%u",
			  (u32)((qw[1] & SDMA_DESC1_HEADER_INDEX_SMASK) >>
				SDMA_DESC1_HEADER_INDEX_SHIFT),
			  (u32)((qw[1] & SDMA_DESC1_HEADER_MODE_SMASK) >>
				SDMA_DESC1_HEADER_MODE_SHIFT),
			  (u32)((qw[1] & SDMA_DESC1_HEADER_DWS_SMASK) >>
				SDMA_DESC1_HEADER_DWS_SHIFT));
	} else {
		buf1[0] = 0;
	}
}

/* interrupt deferred SDMA descriptor dump information */
struct sdma_print_info {
	struct work_struct sdma_dump_work;
	struct sdma_engine *sde; /* only use for auxiliary info */
	struct hw_sdma_desc *descs;
	u32 tag;
	u16 head;
	u16 tail;
};

/* show descriptors using information from argument sdi, not sdi->sde */
static void show_tagged_sdma_descriptors(const struct sdma_print_info *sdi)
{
	char buf0[QW_BUF_SIZE];
	char buf1[QW_BUF_SIZE];
	struct sdma_engine *sde = sdi->sde;
	struct hfi2_devdata *dd = sde->dd;
	struct hw_sdma_desc *descqp;
	u64 desc[2];
	u16 head = sdi->head;

	/* print info for each entry in the descriptor queue */
	while (head != sdi->tail) {
		descqp = &sdi->descs[head];
		desc[0] = le64_to_cpu(descqp->qw[0]);
		desc[1] = le64_to_cpu(descqp->qw[1]);

		sdma_qw_strings(dd, head, desc, buf0, buf1);
		dd_dev_err(dd, "[desc %u] SDMA %s\n", sdi->tag, buf0);
		if (buf1[0])
			dd_dev_err(dd, "[desc %u]\t%s\n", sdi->tag, buf1);
		head = (head + 1) & sde->sdma_mask;
	}
}

static void sdma_dump_worker(struct work_struct *work)
{
	struct sdma_print_info *sdi =
		container_of(work, struct sdma_print_info, sdma_dump_work);

	show_tagged_sdma_descriptors(sdi);
	kfree(sdi->descs);
	kfree(sdi);
}

static void dump_sdma_state(struct sdma_engine *sde)
{
	char tag_info[64];
	struct hfi2_devdata *dd = sde->dd;
	struct sdma_print_info local_sdi;
	struct sdma_print_info *sdi;
	struct hw_sdma_desc *descs;
	int tag;
	u16 head, tail, used, avail;

	head = sde->descq_head & sde->sdma_mask;
	tail = sde->descq_tail & sde->sdma_mask;
	used = sdma_descq_inprocess(sde);
	avail = sdma_descq_freecnt(sde);

	if (used) {
		tag = atomic_fetch_inc(&dd->sdma_print_tag);
		snprintf(tag_info, sizeof(tag_info),
			 ", descriptor print prefix \"[desc %d]\"", tag);
	} else {
		tag = 0;
		tag_info[0] = 0;
	}

	dd_dev_err(
		dd,
		"SDMA (%u) descq_head %u, descq_tail %u, used %u, avail %u, FLE %d%s\n",
		sde->this_idx, head, tail, used, avail,
		!list_empty(&sde->flushlist), tag_info);
	if (used == 0)
		return;

	/* print descriptors - either immediately or delayed */
	if (in_interrupt()) {
		size_t size = sizeof(struct hw_sdma_desc) * sde->descq_cnt;

		sdi = kmalloc_obj(*sdi, GFP_ATOMIC);
		descs = kmalloc(size, GFP_ATOMIC);
		if (!sdi || !descs) {
			kfree(sdi);
			kfree(descs);
			return;
		}

		INIT_WORK(&sdi->sdma_dump_work, sdma_dump_worker);
		memcpy(descs, sde->descq, size);
	} else {
		sdi = &local_sdi;
		descs = sde->descq;
		memset(&sdi->sdma_dump_work, 0, sizeof(sdi->sdma_dump_work));
	}

	sdi->sde = sde;
	sdi->descs = descs;
	sdi->tag = tag;
	sdi->head = head;
	sdi->tail = tail;

	if (in_interrupt())
		queue_work(dd->hfi2_wq, &sdi->sdma_dump_work);
	else
		show_tagged_sdma_descriptors(sdi);
}

#define SDE_FMT \
	"SDE %u CPU %d STE %s C 0x%llx S 0x%016llx E 0x%llx T(HW) 0x%llx T(SW) 0x%x H(HW) 0x%llx H(SW) 0x%x H(D) 0x%llx DM 0x%llx GL 0x%llx R 0x%llx LIS 0x%llx AHGI 0x%llx TXT %u TXH %u DT %u DH %u FLNE %d DQF %u SLC 0x%llx\n"
/**
 * hfi2_sdma_seqfile_dump_sde() - debugfs dump of sde
 * @s: seq file
 * @sde: send dma engine to dump
 *
 * This routine dumps the sde to the indicated seq file.
 */
void hfi2_sdma_seqfile_dump_sde(struct seq_file *s, struct sdma_engine *sde)
{
	char buf0[QW_BUF_SIZE];
	char buf1[QW_BUF_SIZE];
	struct hfi2_devdata *dd = sde->dd;
	unsigned long long check_slid;
	u16 head, tail;
	struct hw_sdma_desc *descqp;
	u64 desc[2];

	head = sde->descq_head & sde->sdma_mask;
	tail = READ_ONCE(sde->descq_tail) & sde->sdma_mask;
	/* SEND_DMA_CHECK_SLID is only available on WFR */
	check_slid = dd->params->chip_type == CHIP_WFR ?
			     read_sde_csr(sde, SEND_DMA_CHECK_SLID) :
			     0ULL;
	seq_printf(
		s, SDE_FMT, sde->this_idx, sde->cpu,
		sdma_state_name(sde->state.current_state),
		(unsigned long long)read_sde_csr(sde,
						 dd->params->send_dma_ctrl_reg),
		(unsigned long long)read_sde_csr(
			sde, dd->params->send_dma_status_reg),
		(unsigned long long)read_sde_csr(
			sde, dd->params->send_dma_eng_err_status_reg),
		(unsigned long long)read_sde_csr(sde,
						 dd->params->send_dma_tail_reg),
		tail,
		(unsigned long long)read_sde_csr(sde,
						 dd->params->send_dma_head_reg),
		head, (unsigned long long)le64_to_cpu(*sde->head_dma),
		(unsigned long long)read_sdecfg_csr(
			sde, dd->params->send_dma_cfg_memory_reg),
		(unsigned long long)read_sde_csr(
			sde, dd->params->send_dma_len_gen_reg),
		(unsigned long long)read_sde_csr(
			sde, dd->params->send_dma_reload_cnt_reg),
		(unsigned long long)sde->last_status,
		(unsigned long long)sde->ahg_bits, sde->tx_tail, sde->tx_head,
		sde->descq_tail, sde->descq_head, !list_empty(&sde->flushlist),
		sde->descq_full_count, check_slid);

	/* print info for each entry in the descriptor queue */
	while (head != tail) {
		descqp = &sde->descq[head];
		desc[0] = le64_to_cpu(descqp->qw[0]);
		desc[1] = le64_to_cpu(descqp->qw[1]);

		sdma_qw_strings(dd, head, desc, buf0, buf1);
		seq_printf(s, "\t%s\n", buf0);
		if (buf1[0])
			seq_printf(s, "\t\t%s\n", buf1);

		head = (head + 1) & sde->sdma_mask;
	}
}

/*
 * Add the generation number into qw1 and return the updated value.
 * The incoming value of the field is expected to be zero.
 */
static inline u64 add_gen(struct sdma_engine *sde, u64 qw1)
{
	u64 generation = (sde->descq_tail >> sde->sdma_shift) &
			 SDMA_DESC1_GENERATION_MASK;

	return qw1 | (generation << SDMA_DESC1_GENERATION_SHIFT);
}

/*
 * This routine submits the indicated tx
 *
 * Space has already been guaranteed and
 * tail side of ring is locked.
 *
 * The hardware tail update is done
 * in the caller and that is facilitated
 * by returning the new tail.
 *
 * There is special case logic for ahg
 * to not add the generation number for
 * up to 2 descriptors that follow the
 * first descriptor.
 *
 */
static inline u16 _submit_tx(struct sdma_engine *sde, struct sdma_txreq *tx,
			     u8 pad)
{
	int i;
	u16 tail;
	struct sdma_desc *descp = tx->descp;
	u8 skip = 0, mode = ahg_mode(tx);

	tail = sde->descq_tail & sde->sdma_mask;
	sde->descq[tail].qw[0] = cpu_to_le64(descp->qw[0]);
	sde->descq[tail].qw[1] = cpu_to_le64(add_gen(sde, descp->qw[1]));
	trace_hfi2_sdma_descriptor(sde, descp->qw, tail, &sde->descq[tail]);
	tail = ++sde->descq_tail & sde->sdma_mask;
	descp++;
	if (mode > SDMA_AHG_APPLY_UPDATE1)
		skip = mode >> 1;
	for (i = 1; i < tx->num_desc; i++, descp++) {
		u64 qw[2];

		if (i == tx->num_desc - 1 && pad) {
			u16 j;

			qw[0] = sdma_pad.qw[0];
			for (j = 0; j < pad; j++) {
				qw[1] = add_gen(sde, sdma_pad.qw[1]);
				sde->descq[tail].qw[0] = cpu_to_le64(qw[0]);
				sde->descq[tail].qw[1] = cpu_to_le64(qw[1]);
				trace_hfi2_sdma_descriptor(sde, qw, tail,
							   &sde->descq[tail]);
				tail = ++sde->descq_tail & sde->sdma_mask;
			}
		}

		qw[0] = descp->qw[0];
		if (skip) {
			/* edits don't have generation */
			qw[1] = descp->qw[1];
			skip--;
		} else {
			/* replace generation with real one for non-edits */
			qw[1] = add_gen(sde, descp->qw[1]);
		}
		sde->descq[tail].qw[0] = cpu_to_le64(qw[0]);
		sde->descq[tail].qw[1] = cpu_to_le64(qw[1]);
		trace_hfi2_sdma_descriptor(sde, qw, tail, &sde->descq[tail]);
		tail = ++sde->descq_tail & sde->sdma_mask;
	}
	tx->next_descq_idx = tail;
#ifdef CONFIG_HFI2_DEBUG_SDMA_ORDER
	tx->sn = sde->tail_sn++;
	trace_hfi2_sdma_in_sn(sde, tx->sn);
	WARN_ON_ONCE(sde->tx_ring[sde->tx_tail & sde->sdma_mask]);
#endif
	sde->tx_ring[sde->tx_tail++ & sde->sdma_mask] = tx;
	sde->desc_avail -= tx->num_desc + pad;
	return tail;
}

static inline u16 submit_tx(struct sdma_engine *sde, struct sdma_txreq *tx)
{
	if (sde->dd->pad_sdma_desc)
		trace_hfi2_sdma_pad(sde->this_idx, 1, sde->dd->pad_sdma_desc,
				    tx->num_desc, tx->num_pad);
	return _submit_tx(sde, tx, tx->num_pad);
}

/*
 * Check for progress
 */
static int sdma_check_progress(struct sdma_engine *sde,
			       struct iowait_work *wait, struct sdma_txreq *tx,
			       bool pkts_sent)
{
	int ret;

	sde->desc_avail = sdma_descq_freecnt(sde);
	if (tx->num_desc + tx->num_pad <= sde->desc_avail)
		return -EAGAIN;
	/* pulse the head_lock */
	if (wait && iowait_ioww_to_iow(wait)->sleep) {
		unsigned int seq;

		seq = raw_seqcount_begin(
			(const seqcount_t *)&sde->head_lock.seqcount);
		ret = wait->iow->sleep(sde, wait, tx, seq, pkts_sent);
		if (ret == -EAGAIN)
			sde->desc_avail = sdma_descq_freecnt(sde);
	} else {
		ret = -EBUSY;
	}
	return ret;
}

/**
 * hfi2_sdma_send_txreq() - submit a tx req to ring
 * @sde: sdma engine to use
 * @wait: SE wait structure to use when full (may be NULL)
 * @tx: sdma_txreq to submit
 * @pkts_sent: has any packet been sent yet?
 *
 * The call submits the tx into the ring.  If a iowait structure is non-NULL
 * the packet will be queued to the list in wait.
 *
 * Return:
 * 0            - success (submittted to ring or silently dropped)
 * -EINVAL      - sdma_txreq incomplete
 * -EBUSY       - no space in ring (wait == NULL)
 * -EIOCBQUEUED - tx queued to iowait
 * -ECOMM       - bad sdma state, tx queued to flush list
 */
int hfi2_sdma_send_txreq(struct sdma_engine *sde, struct iowait_work *wait,
			 struct sdma_txreq *tx, bool pkts_sent)
{
	int ret = 0;
	u16 tail;
	unsigned long flags;

	/* user should have supplied entire packet */
	if (unlikely(tx->tlen))
		return -EINVAL;
	tx->wait = iowait_ioww_to_iow(wait);
	spin_lock_irqsave(&sde->tail_lock, flags);
retry:
	if (unlikely(!__sdma_running(sde)))
		goto unlock_noconn;
	if (unlikely(tx->num_desc + tx->num_pad > sde->desc_avail))
		goto nodesc;
	tail = submit_tx(sde, tx);
	if (wait)
		iowait_sdma_inc(iowait_ioww_to_iow(wait));
	sdma_update_tail(sde, tail);
unlock:
	spin_unlock_irqrestore(&sde->tail_lock, flags);
	return ret;
unlock_noconn:
	if (wait)
		iowait_sdma_inc(iowait_ioww_to_iow(wait));
	tx->next_descq_idx = 0;
#ifdef CONFIG_HFI2_DEBUG_SDMA_ORDER
	tx->sn = sde->tail_sn++;
	trace_hfi2_sdma_in_sn(sde, tx->sn);
#endif
	spin_lock(&sde->flushlist_lock);
	list_add_tail(&tx->list, &sde->flushlist);
	spin_unlock(&sde->flushlist_lock);
	iowait_inc_wait_count(wait, tx->num_desc);
	queue_work_on(sde->cpu, system_highpri_wq, &sde->flush_worker);
	ret = -ECOMM;
	goto unlock;
nodesc:
	ret = sdma_check_progress(sde, wait, tx, pkts_sent);
	if (ret == -EAGAIN) {
		ret = 0;
		goto retry;
	}
	sde->descq_full_count++;
	goto unlock;
}

/**
 * hfi2_sdma_send_txlist() - submit a list of tx req to ring
 * @sde: sdma engine to use
 * @wait: SE wait structure to use when full (may be NULL)
 * @tx_list: list of sdma_txreqs to submit
 * @count_out: pointer to a u16 which, after return will contain the total number of
 *             sdma_txreqs removed from the tx_list. This will include sdma_txreqs
 *             whose SDMA descriptors are submitted to the ring and the sdma_txreqs
 *             which are added to SDMA engine flush list if the SDMA engine state is
 *             not running.
 *
 * The call submits the list into the ring.
 *
 * If the iowait structure is non-NULL and not equal to the iowait list
 * the unprocessed part of the list  will be appended to the list in wait.
 *
 * In all cases, the tx_list will be updated so the head of the tx_list is
 * the list of descriptors that have yet to be transmitted.
 *
 * The intent of this call is to provide a more efficient
 * way of submitting multiple packets to SDMA while holding the tail
 * side locking.
 *
 * Return:
 * 0 - Success,
 * -EINVAL - sdma_txreq incomplete, -EBUSY - no space in ring (wait == NULL)
 * -EIOCBQUEUED - tx queued to iowait, -ECOMM bad sdma state
 */
int hfi2_sdma_send_txlist(struct sdma_engine *sde, struct iowait_work *wait,
			  struct list_head *tx_list, u16 *count_out)
{
	struct hfi2_devdata *dd = sde->dd;
	struct sdma_txreq *tx, *tx_next;
	int ret = 0;
	unsigned long flags;
	u16 tail = INVALID_TAIL;
	u16 desc_avail;
	u16 cur_descs;
	u8 pkts;
	u32 submit_count = 0, flush_count = 0, total_count;

	spin_lock_irqsave(&sde->tail_lock, flags);
retry:
	cur_descs = 0;
	pkts = 0;
	desc_avail = sde->desc_avail;
	if (dd->pad_sdma_desc)
		desc_avail = round_down(desc_avail, dd->pad_sdma_desc);

	list_for_each_entry_safe(tx, tx_next, tx_list, list) {
		u8 pad = 0;

		tx->wait = iowait_ioww_to_iow(wait);
		if (unlikely(!__sdma_running(sde)))
			goto unlock_noconn;
		if (unlikely(tx->num_desc > desc_avail))
			goto nodesc;
		if (unlikely(tx->tlen)) {
			ret = -EINVAL;
			goto update_tail;
		}
		list_del_init(&tx->list);
		cur_descs += tx->num_desc;
		desc_avail -= tx->num_desc;
		pkts++;

		/*
		 * Only add padding descriptors to last packet before tail
		 * update so that tail update is a dd->pad_sdma_desc multiple.
		 *
		 * Look ahead to see if the next packet will cause a tail
		 * update. If so, this packet is the last packet, so pad out
		 * its descriptors as necessary. The next packet will start its
		 * own update block.
		 */
		if (dd->pad_sdma_desc) {
			bool last = false;

			/* No more packets */
			if (list_empty(tx_list))
				last = true;
			/* Next packet will goto nodesc, then update_tail */
			else if (tx_next->num_desc > desc_avail)
				last = true;
			/* Next packet will goto update_tail */
			else if (unlikely(tx_next->tlen))
				last = true;
			else if (((submit_count + 1) &
				  SDMA_TAIL_UPDATE_THRESH) == 0)
				last = true;

			if (last) {
				pad = (u8)(round_up(cur_descs,
						    dd->pad_sdma_desc) -
					   cur_descs);
				trace_hfi2_sdma_pad(sde->this_idx, pkts,
						    dd->pad_sdma_desc,
						    cur_descs, pad);
				cur_descs += pad;
				desc_avail -= pad;
			}
		}

		tail = _submit_tx(sde, tx, pad);
		submit_count++;
		if (tail != INVALID_TAIL &&
		    (submit_count & SDMA_TAIL_UPDATE_THRESH) == 0) {
			sdma_update_tail(sde, tail);
			tail = INVALID_TAIL;
		}
	}
update_tail:
	total_count = submit_count + flush_count;
	if (wait) {
		iowait_sdma_add(iowait_ioww_to_iow(wait), total_count);
		iowait_starve_clear(submit_count > 0, iowait_ioww_to_iow(wait));
	}
	if (tail != INVALID_TAIL)
		sdma_update_tail(sde, tail);
	spin_unlock_irqrestore(&sde->tail_lock, flags);
	*count_out = total_count;
	return ret;
unlock_noconn:
	spin_lock(&sde->flushlist_lock);
	list_for_each_entry_safe(tx, tx_next, tx_list, list) {
		tx->wait = iowait_ioww_to_iow(wait);
		list_del_init(&tx->list);
		tx->next_descq_idx = 0;
#ifdef CONFIG_HFI2_DEBUG_SDMA_ORDER
		tx->sn = sde->tail_sn++;
		trace_hfi2_sdma_in_sn(sde, tx->sn);
#endif
		list_add_tail(&tx->list, &sde->flushlist);
		flush_count++;
		iowait_inc_wait_count(wait, tx->num_desc);
	}
	spin_unlock(&sde->flushlist_lock);
	queue_work_on(sde->cpu, system_highpri_wq, &sde->flush_worker);
	ret = -ECOMM;
	goto update_tail;
nodesc:
	ret = sdma_check_progress(sde, wait, tx, submit_count > 0);
	if (ret == -EAGAIN) {
		ret = 0;
		goto retry;
	}
	sde->descq_full_count++;
	goto update_tail;
}

static void sdma_process_event(struct sdma_engine *sde, enum sdma_events event)
{
	unsigned long flags;

	spin_lock_irqsave(&sde->tail_lock, flags);
	write_seqlock(&sde->head_lock);

	__sdma_process_event(sde, event);

	if (sde->state.current_state == sdma_state_s99_running)
		sdma_desc_avail(sde, sdma_descq_freecnt(sde));

	write_sequnlock(&sde->head_lock);
	spin_unlock_irqrestore(&sde->tail_lock, flags);
}

static void __sdma_process_event(struct sdma_engine *sde,
				 enum sdma_events event)
{
	struct sdma_state *ss = &sde->state;
	int need_progress = 0;

	/* CONFIG SDMA temporary */
#ifdef CONFIG_HFI2_SDMA_VERBOSITY
	dd_dev_err(sde->dd, "CONFIG SDMA(%u) [%s] %s\n", sde->this_idx,
		   sdma_state_names[ss->current_state],
		   sdma_event_names[event]);
#endif

	switch (ss->current_state) {
	case sdma_state_s00_hw_down:
		switch (event) {
		case sdma_event_e00_go_hw_down:
			break;
		case sdma_event_e30_go_running:
			/*
			 * If down, but running requested (usually result
			 * of link up, then we need to start up.
			 * This can happen when hw down is requested while
			 * bringing the link up with traffic active on
			 * 7220, e.g.
			 */
			ss->go_s99_running = 1;
			fallthrough; /* and start dma engine */
		case sdma_event_e10_go_hw_start:
			/* This reference means the state machine is started */
			sdma_get(&sde->state);
			sdma_set_state(sde,
				       sdma_state_s10_hw_start_up_halt_wait);
			break;
		case sdma_event_e15_hw_halt_done:
			break;
		case sdma_event_e25_hw_clean_up_done:
			break;
		case sdma_event_e40_sw_cleaned:
			sdma_sw_tear_down(sde);
			break;
		case sdma_event_e50_hw_cleaned:
			break;
		case sdma_event_e60_hw_halted:
			break;
		case sdma_event_e70_go_idle:
			break;
		case sdma_event_e80_hw_freeze:
			break;
		case sdma_event_e81_hw_frozen:
			break;
		case sdma_event_e82_hw_unfreeze:
			break;
		case sdma_event_e85_link_down:
			break;
		case sdma_event_e90_sw_halted:
			break;
		}
		break;

	case sdma_state_s10_hw_start_up_halt_wait:
		switch (event) {
		case sdma_event_e00_go_hw_down:
			sdma_set_state(sde, sdma_state_s00_hw_down);
			sdma_sw_tear_down(sde);
			break;
		case sdma_event_e10_go_hw_start:
			break;
		case sdma_event_e15_hw_halt_done:
			sdma_set_state(sde,
				       sdma_state_s15_hw_start_up_clean_wait);
			sdma_start_hw_clean_up(sde);
			break;
		case sdma_event_e25_hw_clean_up_done:
			break;
		case sdma_event_e30_go_running:
			ss->go_s99_running = 1;
			break;
		case sdma_event_e40_sw_cleaned:
			break;
		case sdma_event_e50_hw_cleaned:
			break;
		case sdma_event_e60_hw_halted:
			schedule_work(&sde->err_halt_worker);
			break;
		case sdma_event_e70_go_idle:
			ss->go_s99_running = 0;
			break;
		case sdma_event_e80_hw_freeze:
			break;
		case sdma_event_e81_hw_frozen:
			break;
		case sdma_event_e82_hw_unfreeze:
			break;
		case sdma_event_e85_link_down:
			break;
		case sdma_event_e90_sw_halted:
			break;
		}
		break;

	case sdma_state_s15_hw_start_up_clean_wait:
		switch (event) {
		case sdma_event_e00_go_hw_down:
			sdma_set_state(sde, sdma_state_s00_hw_down);
			sdma_sw_tear_down(sde);
			break;
		case sdma_event_e10_go_hw_start:
			break;
		case sdma_event_e15_hw_halt_done:
			break;
		case sdma_event_e25_hw_clean_up_done:
			sdma_hw_start_up(sde);
			sdma_set_state(sde, ss->go_s99_running ?
						    sdma_state_s99_running :
						    sdma_state_s20_idle);
			break;
		case sdma_event_e30_go_running:
			ss->go_s99_running = 1;
			break;
		case sdma_event_e40_sw_cleaned:
			break;
		case sdma_event_e50_hw_cleaned:
			break;
		case sdma_event_e60_hw_halted:
			break;
		case sdma_event_e70_go_idle:
			ss->go_s99_running = 0;
			break;
		case sdma_event_e80_hw_freeze:
			break;
		case sdma_event_e81_hw_frozen:
			break;
		case sdma_event_e82_hw_unfreeze:
			break;
		case sdma_event_e85_link_down:
			break;
		case sdma_event_e90_sw_halted:
			break;
		}
		break;

	case sdma_state_s20_idle:
		switch (event) {
		case sdma_event_e00_go_hw_down:
			sdma_set_state(sde, sdma_state_s00_hw_down);
			sdma_sw_tear_down(sde);
			break;
		case sdma_event_e10_go_hw_start:
			break;
		case sdma_event_e15_hw_halt_done:
			break;
		case sdma_event_e25_hw_clean_up_done:
			break;
		case sdma_event_e30_go_running:
			sdma_set_state(sde, sdma_state_s99_running);
			ss->go_s99_running = 1;
			break;
		case sdma_event_e40_sw_cleaned:
			break;
		case sdma_event_e50_hw_cleaned:
			break;
		case sdma_event_e60_hw_halted:
			sdma_set_state(sde, sdma_state_s50_hw_halt_wait);
			schedule_work(&sde->err_halt_worker);
			break;
		case sdma_event_e70_go_idle:
			break;
		case sdma_event_e85_link_down:
		case sdma_event_e80_hw_freeze:
			sdma_set_state(sde, sdma_state_s80_hw_freeze);
			atomic_dec(&sde->dd->sdma_unfreeze_count);
			wake_up_interruptible(&sde->dd->sdma_unfreeze_wq);
			break;
		case sdma_event_e81_hw_frozen:
			break;
		case sdma_event_e82_hw_unfreeze:
			break;
		case sdma_event_e90_sw_halted:
			break;
		}
		break;

	case sdma_state_s30_sw_clean_up_wait:
		switch (event) {
		case sdma_event_e00_go_hw_down:
			sdma_set_state(sde, sdma_state_s00_hw_down);
			break;
		case sdma_event_e10_go_hw_start:
			break;
		case sdma_event_e15_hw_halt_done:
			break;
		case sdma_event_e25_hw_clean_up_done:
			break;
		case sdma_event_e30_go_running:
			ss->go_s99_running = 1;
			break;
		case sdma_event_e40_sw_cleaned:
			sdma_set_state(sde, sdma_state_s40_hw_clean_up_wait);
			sdma_start_hw_clean_up(sde);
			break;
		case sdma_event_e50_hw_cleaned:
			break;
		case sdma_event_e60_hw_halted:
			break;
		case sdma_event_e70_go_idle:
			ss->go_s99_running = 0;
			break;
		case sdma_event_e80_hw_freeze:
			break;
		case sdma_event_e81_hw_frozen:
			break;
		case sdma_event_e82_hw_unfreeze:
			break;
		case sdma_event_e85_link_down:
			ss->go_s99_running = 0;
			break;
		case sdma_event_e90_sw_halted:
			break;
		}
		break;

	case sdma_state_s40_hw_clean_up_wait:
		switch (event) {
		case sdma_event_e00_go_hw_down:
			sdma_set_state(sde, sdma_state_s00_hw_down);
			queue_work(sde->dd->hfi2_wq,
				   &sde->sdma_sw_clean_up_work);
			break;
		case sdma_event_e10_go_hw_start:
			break;
		case sdma_event_e15_hw_halt_done:
			break;
		case sdma_event_e25_hw_clean_up_done:
			sdma_hw_start_up(sde);
			sdma_set_state(sde, ss->go_s99_running ?
						    sdma_state_s99_running :
						    sdma_state_s20_idle);
			break;
		case sdma_event_e30_go_running:
			ss->go_s99_running = 1;
			break;
		case sdma_event_e40_sw_cleaned:
			break;
		case sdma_event_e50_hw_cleaned:
			break;
		case sdma_event_e60_hw_halted:
			break;
		case sdma_event_e70_go_idle:
			ss->go_s99_running = 0;
			break;
		case sdma_event_e80_hw_freeze:
			break;
		case sdma_event_e81_hw_frozen:
			break;
		case sdma_event_e82_hw_unfreeze:
			break;
		case sdma_event_e85_link_down:
			ss->go_s99_running = 0;
			break;
		case sdma_event_e90_sw_halted:
			break;
		}
		break;

	case sdma_state_s50_hw_halt_wait:
		switch (event) {
		case sdma_event_e00_go_hw_down:
			sdma_set_state(sde, sdma_state_s00_hw_down);
			queue_work(sde->dd->hfi2_wq,
				   &sde->sdma_sw_clean_up_work);
			break;
		case sdma_event_e10_go_hw_start:
			break;
		case sdma_event_e15_hw_halt_done:
			sdma_set_state(sde, sdma_state_s30_sw_clean_up_wait);
			queue_work(sde->dd->hfi2_wq,
				   &sde->sdma_sw_clean_up_work);
			break;
		case sdma_event_e25_hw_clean_up_done:
			break;
		case sdma_event_e30_go_running:
			ss->go_s99_running = 1;
			break;
		case sdma_event_e40_sw_cleaned:
			break;
		case sdma_event_e50_hw_cleaned:
			break;
		case sdma_event_e60_hw_halted:
			schedule_work(&sde->err_halt_worker);
			break;
		case sdma_event_e70_go_idle:
			ss->go_s99_running = 0;
			break;
		case sdma_event_e80_hw_freeze:
			break;
		case sdma_event_e81_hw_frozen:
			break;
		case sdma_event_e82_hw_unfreeze:
			break;
		case sdma_event_e85_link_down:
			ss->go_s99_running = 0;
			break;
		case sdma_event_e90_sw_halted:
			break;
		}
		break;

	case sdma_state_s60_idle_halt_wait:
		switch (event) {
		case sdma_event_e00_go_hw_down:
			sdma_set_state(sde, sdma_state_s00_hw_down);
			queue_work(sde->dd->hfi2_wq,
				   &sde->sdma_sw_clean_up_work);
			break;
		case sdma_event_e10_go_hw_start:
			break;
		case sdma_event_e15_hw_halt_done:
			sdma_set_state(sde, sdma_state_s30_sw_clean_up_wait);
			queue_work(sde->dd->hfi2_wq,
				   &sde->sdma_sw_clean_up_work);
			break;
		case sdma_event_e25_hw_clean_up_done:
			break;
		case sdma_event_e30_go_running:
			ss->go_s99_running = 1;
			break;
		case sdma_event_e40_sw_cleaned:
			break;
		case sdma_event_e50_hw_cleaned:
			break;
		case sdma_event_e60_hw_halted:
			schedule_work(&sde->err_halt_worker);
			break;
		case sdma_event_e70_go_idle:
			ss->go_s99_running = 0;
			break;
		case sdma_event_e80_hw_freeze:
			break;
		case sdma_event_e81_hw_frozen:
			break;
		case sdma_event_e82_hw_unfreeze:
			break;
		case sdma_event_e85_link_down:
			break;
		case sdma_event_e90_sw_halted:
			break;
		}
		break;

	case sdma_state_s80_hw_freeze:
		switch (event) {
		case sdma_event_e00_go_hw_down:
			sdma_set_state(sde, sdma_state_s00_hw_down);
			queue_work(sde->dd->hfi2_wq,
				   &sde->sdma_sw_clean_up_work);
			break;
		case sdma_event_e10_go_hw_start:
			break;
		case sdma_event_e15_hw_halt_done:
			break;
		case sdma_event_e25_hw_clean_up_done:
			break;
		case sdma_event_e30_go_running:
			ss->go_s99_running = 1;
			break;
		case sdma_event_e40_sw_cleaned:
			break;
		case sdma_event_e50_hw_cleaned:
			break;
		case sdma_event_e60_hw_halted:
			break;
		case sdma_event_e70_go_idle:
			ss->go_s99_running = 0;
			break;
		case sdma_event_e80_hw_freeze:
			break;
		case sdma_event_e81_hw_frozen:
			sdma_set_state(sde, sdma_state_s82_freeze_sw_clean);
			queue_work(sde->dd->hfi2_wq,
				   &sde->sdma_sw_clean_up_work);
			break;
		case sdma_event_e82_hw_unfreeze:
			break;
		case sdma_event_e85_link_down:
			break;
		case sdma_event_e90_sw_halted:
			break;
		}
		break;

	case sdma_state_s82_freeze_sw_clean:
		switch (event) {
		case sdma_event_e00_go_hw_down:
			sdma_set_state(sde, sdma_state_s00_hw_down);
			queue_work(sde->dd->hfi2_wq,
				   &sde->sdma_sw_clean_up_work);
			break;
		case sdma_event_e10_go_hw_start:
			break;
		case sdma_event_e15_hw_halt_done:
			break;
		case sdma_event_e25_hw_clean_up_done:
			break;
		case sdma_event_e30_go_running:
			ss->go_s99_running = 1;
			break;
		case sdma_event_e40_sw_cleaned:
			/* notify caller this engine is done cleaning */
			atomic_dec(&sde->dd->sdma_unfreeze_count);
			wake_up_interruptible(&sde->dd->sdma_unfreeze_wq);
			break;
		case sdma_event_e50_hw_cleaned:
			break;
		case sdma_event_e60_hw_halted:
			break;
		case sdma_event_e70_go_idle:
			ss->go_s99_running = 0;
			break;
		case sdma_event_e80_hw_freeze:
			break;
		case sdma_event_e81_hw_frozen:
			break;
		case sdma_event_e82_hw_unfreeze:
			sdma_hw_start_up(sde);
			sdma_set_state(sde, ss->go_s99_running ?
						    sdma_state_s99_running :
						    sdma_state_s20_idle);
			break;
		case sdma_event_e85_link_down:
			break;
		case sdma_event_e90_sw_halted:
			break;
		}
		break;

	case sdma_state_s99_running:
		switch (event) {
		case sdma_event_e00_go_hw_down:
			sdma_set_state(sde, sdma_state_s00_hw_down);
			queue_work(sde->dd->hfi2_wq,
				   &sde->sdma_sw_clean_up_work);
			break;
		case sdma_event_e10_go_hw_start:
			break;
		case sdma_event_e15_hw_halt_done:
			break;
		case sdma_event_e25_hw_clean_up_done:
			break;
		case sdma_event_e30_go_running:
			break;
		case sdma_event_e40_sw_cleaned:
			break;
		case sdma_event_e50_hw_cleaned:
			break;
		case sdma_event_e60_hw_halted:
			need_progress = 1;
			sdma_err_progress_check_schedule(sde);
			fallthrough;
		case sdma_event_e90_sw_halted:
			/*
			 * SW initiated halt does not perform engines
			 * progress check
			 */
			sdma_set_state(sde, sdma_state_s50_hw_halt_wait);
			schedule_work(&sde->err_halt_worker);
			break;
		case sdma_event_e70_go_idle:
			sdma_set_state(sde, sdma_state_s60_idle_halt_wait);
			break;
		case sdma_event_e85_link_down:
			ss->go_s99_running = 0;
			fallthrough;
		case sdma_event_e80_hw_freeze:
			sdma_set_state(sde, sdma_state_s80_hw_freeze);
			atomic_dec(&sde->dd->sdma_unfreeze_count);
			wake_up_interruptible(&sde->dd->sdma_unfreeze_wq);
			break;
		case sdma_event_e81_hw_frozen:
			break;
		case sdma_event_e82_hw_unfreeze:
			break;
		}
		break;
	}

	ss->last_event = event;
	if (need_progress)
		sdma_make_progress(sde, 0);
}

/*
 * _hfi2_extend_sdma_tx_descs() - helper to extend txreq
 *
 * Called when the initial nominal allocation of descriptors in the sdma_txreq
 * is exhausted.
 *
 * The code will bump the allocation up to the max of MAX_DESC (64) descriptors.
 * There doesn't seem much point in an interim step.  The last descriptor or
 * two is reserved for coalesce buffer and possible pad in order to support
 * cases where input packet has >MAX_DESC iovecs.
 */
int _hfi2_extend_sdma_tx_descs(struct hfi2_devdata *dd, struct sdma_txreq *tx)
{
	int i;
	struct sdma_desc *descp;

	descp = kmalloc_array(MAX_DESC, sizeof(struct sdma_desc), GFP_ATOMIC);
	if (!descp) {
		__hfi2_sdma_txclean(dd, tx);
		return -ENOMEM;
	}
	tx->descp = descp;
	tx->desc_limit = MAX_DESC;

	/* copy ones already built */
	for (i = 0; i < tx->num_desc; i++)
		tx->descp[i] = tx->descs[i];
	return 0;
}

/*
 * hfi2_do_coalesce() - coalesce this tx buffer
 *
 * Called when the data in this buffer must be coalesced.
 *
 * If needed, create the coalesce buffer.  Copy the current data into the
 * coalesce buffer.  If this is the last data, dmamap the buffer and add
 * its descriptor.
 *
 * Return:
 * <0 - error
 *  0 - success
 */
int hfi2_do_coalesce(struct hfi2_devdata *dd, struct sdma_txreq *tx, int type,
		     void *kvaddr, struct page *page, unsigned long offset,
		     u16 len)
{
	int pad_len, rval;
	dma_addr_t addr;

	if (!tx->coalesce_buf) {
		/* allocate coalesce buffer with space for padding */
		tx->coalesce_buf = kmalloc(tx->tlen + sizeof(u32), GFP_ATOMIC);
		if (!tx->coalesce_buf) {
			rval = -ENOMEM;
			goto fail;
		}
		tx->coalesce_idx = 0;
	}

	if (type == SDMA_MAP_NONE) {
		rval = -EINVAL;
		goto fail;
	}

	if (type == SDMA_MAP_PAGE) {
		kvaddr = kmap_local_page(page);
		kvaddr += offset;
	} else if (WARN_ON(!kvaddr)) {
		rval = -EINVAL;
		goto fail;
	}

	memcpy(tx->coalesce_buf + tx->coalesce_idx, kvaddr, len);
	tx->coalesce_idx += len;
	if (type == SDMA_MAP_PAGE)
		kunmap_local(kvaddr);

	/* if there is more data, return */
	if (tx->tlen != tx->coalesce_idx)
		return 0;

	/* Whole packet is received; add any padding */
	pad_len = pad_length(tx);
	if (pad_len) {
		memset(tx->coalesce_buf + tx->coalesce_idx, 0, pad_len);
		/* padding is taken care of for coalescing case */
		tx->packet_len += pad_len;
		tx->tlen += pad_len;
	}

	/* dma map the coalesce buffer */
	addr = dma_map_single(&dd->pcidev->dev, tx->coalesce_buf, tx->tlen,
			      DMA_TO_DEVICE);

	if (unlikely(dma_mapping_error(&dd->pcidev->dev, addr))) {
		rval = -ENOSPC;
		goto fail;
	}

	return _sdma_txadd_daddr(dd, SDMA_MAP_SINGLE, tx, addr, tx->tlen);

fail:
	__hfi2_sdma_txclean(dd, tx);
	return rval;
}

/* Update sdes when the lmc changes */
void hfi2_sdma_update_lmc(struct hfi2_devdata *dd, u64 mask, u32 lid)
{
	struct hfi2_devrsrcs *dr = &dd->rsrcs;
	struct sdma_engine *sde;
	int i;
	u64 sreg;

	/* only WFR has SDMA CHECK registers - skip for all others */
	if (dd->params->chip_type != CHIP_WFR)
		return;

	sreg = ((mask & SD(CHECK_SLID_MASK_MASK))
		<< SD(CHECK_SLID_MASK_SHIFT)) |
	       (((lid & mask) & SD(CHECK_SLID_VALUE_MASK))
		<< SD(CHECK_SLID_VALUE_SHIFT));

	for (i = dr->first_sdma_engine; i < dr->last_sdma_engine; ++i) {
		hfi2_cdbg(LINKVERB, "SendDmaEngine[%d].SLID_CHECK = 0x%x", i,
			  (u32)sreg);
		sde = &dd->per_sdma[i];
		write_sde_csr(sde, SD(CHECK_SLID), sreg);
	}
}

/*
 * Add ahg to the sdma_txreq
 *
 * The logic will consume up to 3
 * descriptors at the beginning of
 * sdma_txreq.
 */
void _hfi2_sdma_txreq_ahgadd(struct sdma_txreq *tx, u8 num_ahg, u8 ahg_entry,
			     u32 *ahg, u8 ahg_hlen)
{
	u32 i, shift = 0, desc = 0;
	u8 mode;

	WARN_ON_ONCE(num_ahg > 9 || (ahg_hlen & 3) || ahg_hlen == 4);
	/* compute mode */
	if (num_ahg == 1)
		mode = SDMA_AHG_APPLY_UPDATE1;
	else if (num_ahg <= 5)
		mode = SDMA_AHG_APPLY_UPDATE2;
	else
		mode = SDMA_AHG_APPLY_UPDATE3;
	tx->num_desc++;
	/* initialize to consumed descriptors to zero */
	switch (mode) {
	case SDMA_AHG_APPLY_UPDATE3:
		tx->num_desc++;
		tx->descs[2].qw[0] = 0;
		tx->descs[2].qw[1] = 0;
		fallthrough;
	case SDMA_AHG_APPLY_UPDATE2:
		tx->num_desc++;
		tx->descs[1].qw[0] = 0;
		tx->descs[1].qw[1] = 0;
		break;
	}
	ahg_hlen >>= 2;
	tx->descs[0].qw[1] |= (((u64)ahg_entry & SDMA_DESC1_HEADER_INDEX_MASK)
			       << SDMA_DESC1_HEADER_INDEX_SHIFT) |
			      (((u64)ahg_hlen & SDMA_DESC1_HEADER_DWS_MASK)
			       << SDMA_DESC1_HEADER_DWS_SHIFT) |
			      (((u64)mode & SDMA_DESC1_HEADER_MODE_MASK)
			       << SDMA_DESC1_HEADER_MODE_SHIFT) |
			      (((u64)ahg[0] & SDMA_DESC1_HEADER_UPDATE1_MASK)
			       << SDMA_DESC1_HEADER_UPDATE1_SHIFT);
	for (i = 0; i < (num_ahg - 1); i++) {
		if (!shift && !(i & 2))
			desc++;
		tx->descs[desc].qw[!!(i & 2)] |= (((u64)ahg[i + 1]) << shift);
		shift = (shift + 32) & 63;
	}
}

/**
 * hfi2_sdma_ahg_alloc - allocate an AHG entry
 * @sde: engine to allocate from
 *
 * Return:
 * 0-31 when successful, -EOPNOTSUPP if AHG is not enabled,
 * -ENOSPC if an entry is not available
 */
int hfi2_sdma_ahg_alloc(struct sdma_engine *sde)
{
	int nr;
	int oldbit;

	if (!sde) {
		trace_hfi2_ahg_allocate(sde, -EINVAL);
		return -EINVAL;
	}
	while (1) {
		nr = ffz(READ_ONCE(sde->ahg_bits));
		if (nr > 31) {
			trace_hfi2_ahg_allocate(sde, -ENOSPC);
			return -ENOSPC;
		}
		oldbit = test_and_set_bit(nr, &sde->ahg_bits);
		if (!oldbit)
			break;
		cpu_relax();
	}
	trace_hfi2_ahg_allocate(sde, nr);
	return nr;
}

/**
 * hfi2_sdma_ahg_free - free an AHG entry
 * @sde: engine to return AHG entry
 * @ahg_index: index to free
 *
 * This routine frees the indicate AHG entry.
 */
void hfi2_sdma_ahg_free(struct sdma_engine *sde, int ahg_index)
{
	if (!sde)
		return;
	trace_hfi2_ahg_deallocate(sde, ahg_index);
	if (ahg_index < 0 || ahg_index > 31)
		return;
	clear_bit(ahg_index, &sde->ahg_bits);
}

/*
 * SPC freeze handling for SDMA engines.  Called when the driver knows
 * the SPC is going into a freeze but before the freeze is fully
 * settled.  Generally an error interrupt.
 *
 * This event will pull the engine out of running so no more entries can be
 * added to the engine's queue.
 */
void hfi2_sdma_freeze_notify(struct hfi2_devdata *dd, int link_down)
{
	struct hfi2_devrsrcs *dr = &dd->rsrcs;
	int i;
	enum sdma_events event = link_down ? sdma_event_e85_link_down :
					     sdma_event_e80_hw_freeze;

	/* set up the wait but do not wait here */
	atomic_set(&dd->sdma_unfreeze_count,
		   dr->last_sdma_engine - dr->first_sdma_engine);

	/* tell all engines to stop running and wait */
	for (i = dr->first_sdma_engine; i < dr->last_sdma_engine; ++i)
		sdma_process_event(&dd->per_sdma[i], event);

	/* hfi2_sdma_freeze() will wait for all engines to have stopped */
}

/*
 * SPC freeze handling for SDMA engines.  Called when the driver knows
 * the SPC is fully frozen.
 */
void hfi2_sdma_freeze(struct hfi2_devdata *dd)
{
	struct hfi2_devrsrcs *dr = &dd->rsrcs;
	int i;
	int ret;

	/*
	 * Make sure all engines have moved out of the running state before
	 * continuing.
	 */
	ret = wait_event_interruptible(dd->sdma_unfreeze_wq,
				       atomic_read(&dd->sdma_unfreeze_count) <=
					       0);
	/* interrupted or count is negative, then unloading - just exit */
	if (ret || atomic_read(&dd->sdma_unfreeze_count) < 0)
		return;

	/* set up the count for the next wait */
	atomic_set(&dd->sdma_unfreeze_count,
		   dr->last_sdma_engine - dr->first_sdma_engine);

	/* tell all engines that the SPC is frozen, they can start cleaning */
	for (i = dr->first_sdma_engine; i < dr->last_sdma_engine; ++i)
		sdma_process_event(&dd->per_sdma[i], sdma_event_e81_hw_frozen);

	/*
	 * Wait for everyone to finish software clean before exiting.  The
	 * software clean will read engine CSRs, so must be completed before
	 * the next step, which will clear the engine CSRs.
	 */
	(void)wait_event_interruptible(dd->sdma_unfreeze_wq,
				       atomic_read(&dd->sdma_unfreeze_count) <=
					       0);
	/* no need to check results - done no matter what */
}

/*
 * SPC freeze handling for the SDMA engines.  Called after the SPC is unfrozen.
 *
 * The SPC freeze acts like a SDMA halt and a hardware clean combined.  All
 * that is left is a software clean.  We could do it after the SPC is fully
 * frozen, but then we'd have to add another state to wait for the unfreeze.
 * Instead, just defer the software clean until the unfreeze step.
 */
void hfi2_sdma_unfreeze(struct hfi2_devdata *dd)
{
	struct hfi2_devrsrcs *dr = &dd->rsrcs;
	int i;

	/* tell all engines start freeze clean up */
	for (i = dr->first_sdma_engine; i < dr->last_sdma_engine; ++i)
		sdma_process_event(&dd->per_sdma[i],
				   sdma_event_e82_hw_unfreeze);
}

/**
 * _hfi2_sdma_engine_progress_schedule() - schedule progress on engine
 * @sde: sdma_engine to schedule progress
 */
void _hfi2_sdma_engine_progress_schedule(struct sdma_engine *sde)
{
	trace_hfi2_sdma_engine_progress(sde, sde->progress_mask);
	/* assume we have selected a good cpu */
	hfi2_write_csr(sde->dd,
		       sde->dd->params->cce_int_force_reg +
			       (8 * (sde->dd->params->is_sdma_start / 64)),
		       sde->progress_mask);
}

/*
 * Wait for the given bit to be set in the SendDmaStatus register.
 *
 * Return:
 *   0          success
 *   -ETIMEDOUT fail
 */
static int wait_for_engine_bit(struct hfi2_devdata *dd, u32 engine, u64 bit,
			       u32 mstimeout)
{
	unsigned long timeout;
	u64 status;

	timeout = jiffies + msecs_to_jiffies(mstimeout);
	while (1) {
		usleep_range(80, 120);
		status = read_sdma_csr(dd, engine,
				       dd->params->send_dma_status_reg);
		if (status & bit)
			return 0;
		if (time_after(jiffies, timeout))
			return -ETIMEDOUT;
	}
}

static void engine_disable(struct hfi2_devdata *dd, u32 engine)
{
	write_sdma_csr(dd, engine, dd->params->send_dma_ctrl_reg, 0);
}

/* expects engine is disabled */
static void engine_halt(struct hfi2_devdata *dd, u32 engine)
{
	u64 status;
	int ret;

	/* halt only if needed */
	status = read_sdma_csr(dd, engine, dd->params->send_dma_status_reg);
	if (status & SD(STATUS_ENG_HALTED_SMASK))
		return;

	write_sdma_csr(dd, engine, dd->params->send_dma_ctrl_reg,
		       SD(CTRL_SDMA_HALT_SMASK));
	ret = wait_for_engine_bit(dd, engine, SD(STATUS_ENG_HALTED_SMASK), 100);
	if (ret)
		dd_dev_err(dd, "%s: engine %d did not halt\n", __func__,
			   engine);
}

/* expects engine is disabled */
static void engine_cleanup(struct hfi2_devdata *dd, u32 engine)
{
	u64 status;
	int ret;

	/* cleanup only if needed */
	status = read_sdma_csr(dd, engine, dd->params->send_dma_status_reg);
	if (status & SD(STATUS_ENG_CLEANED_UP_SMASK))
		return;

	write_sdma_csr(dd, engine, dd->params->send_dma_ctrl_reg,
		       SD(CTRL_SDMA_CLEANUP_SMASK));
	ret = wait_for_engine_bit(dd, engine, SD(STATUS_ENG_CLEANED_UP_SMASK),
				  100);
	if (ret)
		dd_dev_err(dd, "%s: engine %d did not clean up\n", __func__,
			   engine);
}

static void engine_enable(struct hfi2_devdata *dd, u32 engine)
{
	write_sdma_csr(dd, engine, dd->params->send_dma_ctrl_reg,
		       SD(CTRL_SDMA_ENABLE_SMASK));
}

/*
 * Write to each JKR SDMA memory
 *
 * Expect:
 *   o Interrupts are masked.
 *   o SDMA engines are not set up for main driver use.
 *
 * Return:
 *   o 0      success
 *   o -errno failure
 */
static int prime_sdma_memories(struct hfi2_devdata *dd)
{
	const u32 engine = 0;
	/* data */
	const u32 num_desc = 64; /* in multiples of 64 */
	const u32 data_size = 8; /* in bytes */
	const u32 desc_size = sizeof(struct hw_sdma_desc) * num_desc;
	const u32 buf_size = desc_size + data_size;
	/* memories */
	const u32 num_memories = 16;
	const u32 num_banks = 4;
	const u32 bank_size = 784 * 8; /* bytes */
	const u32 mem_size = num_banks * bank_size;
	const u32 num_credits = bank_size / SDMA_BLOCK_SIZE;
	/* variables */
	unsigned long timeout;
	dma_addr_t buf_phys;
	dma_addr_t desc_phys;
	dma_addr_t data_phys;
	void *buf_addr;
	struct hw_sdma_desc *desc_addr;
	u64 status;
	u64 value;
	u64 qw0, qw1;
	u32 mem;
	u32 bank;
	u32 start;

	/* only for JKR */
	if (dd->params->chip_type != CHIP_JKR)
		return 0;
	if (!enable_jkr_sdma_mem_init) {
		dd_dev_info(dd, "SKIPPING Write JKR SDMA memories\n");
		return 0;
	}
	dd_dev_info(dd, "Write JKR SDMA memories\n");

	/*
	 * Set up memory
	 */
	/* allocate dma memory */
	buf_addr = dma_alloc_coherent(&dd->pcidev->dev, buf_size, &buf_phys,
				      GFP_KERNEL);
	if (!buf_addr)
		return -EIO;
	memset(buf_addr, 0, buf_size);

	/* assign DMA buffer: desc, then data */
	desc_addr = buf_addr;
	desc_phys = buf_phys;
	data_phys = buf_phys + desc_size;

	/* create bad packet: leave as zero, size is 8 */

	/*
	 * Set up engine
	 */
	/* mask all errors */
	write_sdma_csr(dd, engine, dd->params->send_dma_eng_err_mask_reg, 0);

	/* disable, halt, and clean in case anything is lingering */
	engine_disable(dd, engine);
	engine_halt(dd, engine);
	engine_cleanup(dd, engine);

	/* descriptor address */
	write_sdma_csr(dd, engine, dd->params->send_dma_base_addr_reg,
		       desc_phys);
	/* no idle countdown */
	write_sdma_csr(dd, engine, dd->params->send_dma_reload_cnt_reg, 0);
	/* no progress countdown */
	write_sdma_csr(dd, engine, dd->params->send_dma_desc_cnt_reg, 0);
	/* no head dma address */
	write_sdma_csr(dd, engine, dd->params->send_dma_head_addr_reg, 0);

	/*
	 * Loop over memories
	 *
	 * Expect engine is cleaned up at top of loop.
	 */
	for (mem = 0; mem < num_memories; mem++) {
		for (bank = 0; bank < num_banks; bank++) {
			/* set fetch destination */
			start = ((mem * mem_size) + (bank * bank_size)) /
				SDMA_BLOCK_SIZE;
			value = ((u64)num_credits
				 << SD(MEMORY_SDMA_MEMORY_CNT_SHIFT)) |
				((u64)start
				 << SD(MEMORY_SDMA_MEMORY_INDEX_SHIFT));
			write_sdmacfg_csr(dd, engine,
					  dd->params->send_dma_cfg_memory_reg,
					  value);
			/* set descriptor length, disable generation counter */
			write_sdma_csr(
				dd, engine, dd->params->send_dma_len_gen_reg,
				(num_desc / 64) << SD(LEN_GEN_LENGTH_SHIFT));

			/* zero descriptors */
			memset(desc_addr, 0, desc_size);

			/* enable engine */
			engine_enable(dd, engine);

			/*
			 * Do send
			 */
			/*
			 * step: create descriptor
			 *   no ahg, no generation, no interrupt request,
			 *   no host writes
			 */
			qw0 = data_phys;
			qw1 = JKR_SDMA_DESC1_FIRST_DESC_FLAG |
			      JKR_SDMA_DESC1_LAST_DESC_FLAG |
			      data_size << JKR_SDMA_DESC1_BYTE_COUNT_SHIFT;

			/*
			 * step: write descriptor
			 *   expect index 0 from engine cleanup
			 */
			desc_addr->qw[0] = cpu_to_le64(qw0);
			desc_addr->qw[1] = cpu_to_le64(qw1);

			/*
			 * step: start engine by updating tail to index 1
			 */
			smp_wmb(); /* commit previous writes to memory */
			write_sdma_csr(dd, engine,
				       dd->params->send_dma_tail_reg, 1);

			/*
			 * step: wait for error halt
			 */
			timeout = jiffies + msecs_to_jiffies(500);
			while (1) {
				usleep_range(80, 120);
				status = read_sdma_csr(
					dd, engine,
					dd->params->send_dma_status_reg);
				if (status & SD(STATUS_ENG_HALTED_SMASK))
					break;

				if (time_after(jiffies, timeout)) {
					dd_dev_warn(
						dd,
						"%s: [%2d,%d] timeout waiting for halt\n",
						__func__, mem, bank);
					break;
				}
			}

			/* disable, halt, and clean up */
			engine_disable(dd, engine);
			engine_halt(dd, engine);
			engine_cleanup(dd, engine);
		}
	}

	/*
	 * Clean up
	 */
	write_sdma_csr(dd, engine, dd->params->send_dma_base_addr_reg, 0);
	write_sdmacfg_csr(dd, engine, dd->params->send_dma_cfg_memory_reg, 0);
	write_sdma_csr(dd, engine, dd->params->send_dma_eng_err_clear_reg,
		       ~0ull);

	/* free dma memory */
	dma_free_coherent(&dd->pcidev->dev, buf_size, buf_addr, buf_phys);
	return 0;
}
