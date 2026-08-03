// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Copyright(c) 2015-2018 Intel Corporation.
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */

#include <linux/delay.h>
#include "hfi2.h"
#include "affinity.h"
#include "qp.h"
#include "trace.h"
#include "vf2pf.h"

#define SC(name) SEND_CTXT_##name
/*
 * Send Context functions
 */
static void sc_wait_for_packet_egress(struct send_context *sc, int pause);
static int pio_init_wait_progress(struct hfi2_devdata *dd);

/*
 * Set the CM reset bit and wait for it to clear.  Use the provided
 * sendctrl register.  This routine has no locking.
 */
void __hfi2_cm_reset(struct hfi2_pportdata *ppd, u64 sendctrl)
{
	struct hfi2_devdata *dd = ppd->dd;
	int pidx = ppd->hw_pidx;

	write_eport_csr(dd, pidx, dd->params->send_ctrl_reg,
			sendctrl | SEND_CTRL_CM_RESET_SMASK);
	while (1) {
		udelay(1);
		sendctrl = read_eport_csr(dd, pidx, dd->params->send_ctrl_reg);
		if ((sendctrl & SEND_CTRL_CM_RESET_SMASK) == 0)
			break;
	}
}

/* global control of PIO send */
void hfi2_pio_send_control(struct hfi2_pportdata *ppd, int op)
{
	struct hfi2_devdata *dd = ppd->dd;
	u64 reg, mask;
	unsigned long flags;
	int write = 1; /* write sendctrl back */
	int flush = 0; /* re-read sendctrl to make sure it is flushed */
	int i;

	/* only WFR needs to write SendCtrl */
	if (dd->params->chip_type != CHIP_WFR)
		return;

	spin_lock_irqsave(&dd->sendctrl_lock, flags);

	reg = read_eport_csr(dd, ppd->hw_pidx, dd->params->send_ctrl_reg);
	switch (op) {
	case PSC_GLOBAL_ENABLE:
		reg |= SEND_CTRL_SEND_ENABLE_SMASK |
		       dd->params->send_ctrl_flush;
		fallthrough;
	case PSC_DATA_VL_ENABLE:
		mask = 0;
		for (i = 0; i < ARRAY_SIZE(ppd->vld); i++)
			if (!ppd->vld[i].mtu)
				mask |= BIT_ULL(i);
		/* Disallow sending on VLs not enabled */
		mask = (mask & SEND_CTRL_UNSUPPORTED_VL_MASK)
		       << SEND_CTRL_UNSUPPORTED_VL_SHIFT;
		reg = (reg & ~SEND_CTRL_UNSUPPORTED_VL_SMASK) | mask;
		break;
	case PSC_GLOBAL_DISABLE:
		reg &= ~SEND_CTRL_SEND_ENABLE_SMASK;
		break;
	case PSC_GLOBAL_VLARB_ENABLE:
		reg |= SEND_CTRL_VL_ARBITER_ENABLE_SMASK;
		break;
	case PSC_GLOBAL_VLARB_DISABLE:
		reg &= ~SEND_CTRL_VL_ARBITER_ENABLE_SMASK;
		break;
	case PSC_CM_RESET:
		__hfi2_cm_reset(ppd, reg);
		write = 0; /* CSR already written (and flushed) */
		break;
	case PSC_DATA_VL_DISABLE:
		reg |= SEND_CTRL_UNSUPPORTED_VL_SMASK;
		flush = 1;
		break;
	default:
		dd_dev_err(dd, "%s: invalid control %d\n", __func__, op);
		break;
	}

	if (write) {
		write_eport_csr(dd, ppd->hw_pidx, dd->params->send_ctrl_reg,
				reg);
		if (flush) {
			/* flush write */
			(void)read_eport_csr(dd, ppd->hw_pidx,
					     dd->params->send_ctrl_reg);
		}
	}

	spin_unlock_irqrestore(&dd->sendctrl_lock, flags);
}

/* number of send context memory pools */
#define NUM_SC_POOLS 2

/* Send Context Size (SCS) wildcards */
#define SCS_POOL_0 -1
#define SCS_POOL_1 -2

/* Send Context Count (SCC) wildcards */
#define SCC_PER_VL -1
#define SCC_PER_CPU -2
#define SCC_PER_KRCVQ -3
#define SCC_MIN_WC SCC_PER_KRCVQ

/* Send Context Size (SCS) constants */
#define SCS_ACK_CREDITS 32
#define SCS_VL15_CREDITS 102 /* 3 pkts of 2048B data + 128B header */

#define PIO_THRESHOLD_CEILING 4096

#define PIO_WAIT_BATCH_SIZE 5

/* default send context sizes */
static struct sc_config_sizes sc_config_sizes[SC_MAX] = {
	[SC_KERNEL] = { .size = SCS_POOL_0, /* even divide, pool 0 */
			.count = SCC_PER_VL }, /* one per NUMA */
	[SC_ACK] = { .size = SCS_ACK_CREDITS, .count = SCC_PER_KRCVQ },
	[SC_USER] = { .size = SCS_POOL_0, /* even divide, pool 0 */
		      .count = SCC_PER_CPU }, /* one per CPU */
	[SC_VL15] = { .size = SCS_VL15_CREDITS, .count = 1 },

};

/* send context memory pool configuration */
struct mem_pool_config {
	int centipercent; /* % of memory, in 100ths of 1% */
	int absolute_blocks; /* absolute block count */
};

/* default memory pool configuration: 100% in pool 0 */
static struct mem_pool_config sc_mem_pool_config[NUM_SC_POOLS] = {
	/* centi%, abs blocks */
	{ 10000, -1 }, /* pool 0 */
	{ 0, -1 }, /* pool 1 */
};

/* memory pool information, used when calculating final sizes */
struct mem_pool_info {
	/*
	 * 100th of 1% of memory to use, -1 if blocks
	 * already set
	 */
	int centipercent;
	int count; /* count of contexts in the pool */
	int blocks; /* block size of the pool */
	int size; /* context size, in blocks */
};

/*
 * Convert a pool wildcard to a valid pool index.  The wildcards
 * start at -1 and increase negatively.  Map them as:
 *	-1 => 0
 *	-2 => 1
 *	etc.
 *
 * Return -1 on non-wildcard input, otherwise convert to a pool number.
 */
static int wildcard_to_pool(int wc)
{
	if (wc >= 0)
		return -1; /* non-wildcard */
	return -wc - 1;
}

static const char *sc_type_names[SC_MAX] = {
	"kernel",
	"vl15",
	"ack",
	"user",
};

static const char *sc_type_name(int index)
{
	if (index < 0 || index >= SC_MAX)
		return "unknown";
	return sc_type_names[index];
}

/*
 * Read the send context memory pool configuration and send context
 * size configuration.  Replace any wildcards and come up with final
 * counts and sizes for the send context types.
 */
int hfi2_init_sc_pools_and_sizes(struct hfi2_devdata *dd)
{
	struct hfi2_devrsrcs *dr = &dd->rsrcs;
	struct mem_pool_info mem_pool_info[NUM_SC_POOLS] = { { 0 } };
	/* do not use first N blocks */
	int total_blocks = dr->c.last_pio_block - dr->c.first_pio_block;
	u32 usable_sc = dr->c.last_send_context - dr->c.first_send_context;
	int total_contexts = 0;
	int fixed_blocks;
	int pool_blocks;
	int used_blocks;
	int cp_total; /* centipercent total */
	int ab_total; /* absolute block total */
	int extra;
	int pidx;
	int i;

	/*
	 * When SDMA is enabled, kernel context pio packet size is capped by
	 * "hfi2_piothreshold". Reduce pio buffer allocation for kernel context by
	 * setting it to a fixed size. The allocation allows 3-deep buffering
	 * of the largest pio packets plus up to 128 bytes header, sufficient
	 * to maintain verbs performance.
	 *
	 * When SDMA is disabled, keep the default pooling allocation.
	 */
	if (HFI2_CAP_IS_KSET(SDMA)) {
		u16 max_pkt_size = (hfi2_piothreshold < PIO_THRESHOLD_CEILING) ?
					   hfi2_piothreshold :
					   PIO_THRESHOLD_CEILING;
		sc_config_sizes[SC_KERNEL].size =
			3 * (max_pkt_size + 128) / PIO_BLOCK_SIZE;
	}

	/*
	 * Step 0:
	 *	- copy the centipercents/absolute sizes from the pool config
	 *	- sanity check these values
	 *	- add up centipercents, then later check for full value
	 *	- add up absolute blocks, then later check for over-commit
	 */
	cp_total = 0;
	ab_total = 0;
	for (i = 0; i < NUM_SC_POOLS; i++) {
		int cp = sc_mem_pool_config[i].centipercent;
		int ab = sc_mem_pool_config[i].absolute_blocks;

		/*
		 * A negative value is "unused" or "invalid".  Both *can*
		 * be valid, but centipercent wins, so check that first
		 */
		if (cp >= 0) { /* centipercent valid */
			cp_total += cp;
		} else if (ab >= 0) { /* absolute blocks valid */
			ab_total += ab;
		} else { /* neither valid */
			dd_dev_err(
				dd,
				"Send context memory pool %d: both the block count and centipercent are invalid\n",
				i);
			return -EINVAL;
		}

		mem_pool_info[i].centipercent = cp;
		mem_pool_info[i].blocks = ab;
	}

	/* do not use both % and absolute blocks for different pools */
	if (cp_total != 0 && ab_total != 0) {
		dd_dev_err(
			dd,
			"All send context memory pools must be described as either centipercent or blocks, no mixing between pools\n");
		return -EINVAL;
	}

	/* if any percentages are present, they must add up to 100% x 100 */
	if (cp_total != 0 && cp_total != 10000) {
		dd_dev_err(
			dd,
			"Send context memory pool centipercent is %d, expecting 10000\n",
			cp_total);
		return -EINVAL;
	}

	/* the absolute pool total cannot be more than the mem total */
	if (ab_total > total_blocks) {
		dd_dev_err(
			dd,
			"Send context memory pool absolute block count %d is larger than the memory size %d\n",
			ab_total, total_blocks);
		return -EINVAL;
	}

	/*
	 * Step 2:
	 *	- copy from the context size config
	 *	- replace context type wildcard counts with real values
	 *	- add up non-memory pool block sizes
	 *	- add up memory pool user counts
	 */
	fixed_blocks = 0;
	for (i = 0; i < SC_MAX; i++) {
		int count = sc_config_sizes[i].count;
		int size = sc_config_sizes[i].size;
		int pool;
		int newcnt;

		/*
		 * Sanity check count: Either a positive value or
		 * one of the expected wildcards is valid.  The positive
		 * value is checked later when we compare against total
		 * memory available.
		 */
		if (count < SCC_MIN_WC) {
			dd_dev_err(
				dd,
				"%s send context invalid count wildcard %d\n",
				sc_type_name(i), count);
			return -EINVAL;
		}
		newcnt = 0;
		for (pidx = 0; pidx < dd->num_pports; ++pidx) {
			struct hfi2_portrsrcs *pr = &dr->ppr[pidx];

			if (!port_available_pidx(dd, pidx))
				continue;

			if (count == SCC_PER_KRCVQ)
				newcnt += pr->n_krcv_queues;
			else if (count == SCC_PER_VL)
				newcnt += INIT_SC_PER_VL * hfi2_num_vls;
			else if (count == SCC_PER_CPU)
				newcnt += pr->num_rcv_contexts -
					  pr->n_krcv_queues;
			else
				newcnt += count;
		}
		count = newcnt;

		/* only expect SC_USER to possibly overflow */
		if (total_contexts + count > usable_sc) {
			if (i != SC_USER) {
				dd_dev_err(dd, "%s send context overflow\n",
					   sc_type_name(i));
				return -EINVAL;
			}
			dd_dev_warn(
				dd,
				"%s send context count reduced by %d, %d -> %d\n",
				sc_type_name(i),
				count - (usable_sc - total_contexts), count,
				usable_sc - total_contexts);
			count = usable_sc - total_contexts;
		}

		total_contexts += count;

		/*
		 * Sanity check pool: The conversion will return a pool
		 * number or -1 if a fixed (non-negative) value.  The fixed
		 * value is checked later when we compare against
		 * total memory available.
		 */
		pool = wildcard_to_pool(size);
		if (pool == -1) { /* non-wildcard */
			fixed_blocks += size * count;
		} else if (pool < NUM_SC_POOLS) { /* valid wildcard */
			mem_pool_info[pool].count += count;
		} else { /* invalid wildcard */
			dd_dev_err(dd,
				   "%s send context invalid pool wildcard %d\n",
				   sc_type_name(i), size);
			return -EINVAL;
		}

		dd->sc_sizes[i].count = count;
		dd->sc_sizes[i].size = size;
	}
	if (fixed_blocks > total_blocks) {
		dd_dev_err(
			dd,
			"Send context fixed block count, %u, larger than total block count %u\n",
			fixed_blocks, total_blocks);
		return -EINVAL;
	}

	/* step 3: calculate the blocks in the pools, and pool context sizes */
	pool_blocks = total_blocks - fixed_blocks;
	if (ab_total > pool_blocks) {
		dd_dev_err(
			dd,
			"Send context fixed pool sizes, %u, larger than pool block count %u\n",
			ab_total, pool_blocks);
		return -EINVAL;
	}
	/* subtract off the fixed pool blocks */
	pool_blocks -= ab_total;

	for (i = 0; i < NUM_SC_POOLS; i++) {
		struct mem_pool_info *pi = &mem_pool_info[i];

		/* % beats absolute blocks */
		if (pi->centipercent >= 0)
			pi->blocks = (pool_blocks * pi->centipercent) / 10000;

		if (pi->blocks == 0 && pi->count != 0) {
			dd_dev_err(
				dd,
				"Send context memory pool %d has %u contexts, but no blocks\n",
				i, pi->count);
			return -EINVAL;
		}
		if (pi->count == 0) {
			/* warn about wasted blocks */
			if (pi->blocks != 0)
				dd_dev_err(
					dd,
					"Send context memory pool %d has %u blocks, but zero contexts\n",
					i, pi->blocks);
			pi->size = 0;
		} else {
			pi->size = pi->blocks / pi->count;
		}
	}

	/* step 4: fill in the context type sizes from the pool sizes */
	used_blocks = 0;
	for (i = 0; i < SC_MAX; i++) {
		if (dd->sc_sizes[i].size < 0) {
			unsigned int pool =
				wildcard_to_pool(dd->sc_sizes[i].size);

			WARN_ON_ONCE(pool >= NUM_SC_POOLS);
			dd->sc_sizes[i].size = mem_pool_info[pool].size;
		}
		/* make sure we are not larger than what is allowed by the HW */
#define PIO_MAX_BLOCKS 1024
		if (dd->sc_sizes[i].size > PIO_MAX_BLOCKS)
			dd->sc_sizes[i].size = PIO_MAX_BLOCKS;

		/* calculate our total usage */
		used_blocks += dd->sc_sizes[i].size * dd->sc_sizes[i].count;
	}
	extra = total_blocks - used_blocks;
	if (extra != 0)
		dd_dev_info(dd, "unused send context blocks: %d\n", extra);

	return total_contexts;
}

int hfi2_init_send_contexts(struct hfi2_devdata *dd)
{
	struct hfi2_devrsrcs *dr = &dd->rsrcs;
	u32 num_hw_sc = chip_send_contexts(dd);
	u16 base;
	int ret, i, j, context;

	ret = hfi2_init_credit_return(dd);
	if (ret)
		return ret;

	dd->hw_to_sw = kmalloc_array(num_hw_sc, sizeof(u16), GFP_KERNEL);
	dd->send_contexts = kcalloc(dd->num_send_contexts,
				    sizeof(struct send_context_info),
				    GFP_KERNEL);
	if (!dd->send_contexts || !dd->hw_to_sw) {
		kfree(dd->hw_to_sw);
		kfree(dd->send_contexts);
		hfi2_free_credit_return(dd);
		return -ENOMEM;
	}

	/* hardware context map starts with invalid send context indices */
	for (i = 0; i < num_hw_sc; i++)
		dd->hw_to_sw[i] = INVALID_SCI;

	/*
	 * All send contexts have their credit sizes.  Allocate credits
	 * for each context one after another from the global space.
	 */
	context = 0;
	base = dr->c.first_pio_block; /* do not use first N blocks */
	for (i = 0; i < SC_MAX; i++) {
		struct sc_config_sizes *scs = &dd->sc_sizes[i];

		for (j = 0; j < scs->count; j++) {
			struct send_context_info *sci =
				&dd->send_contexts[context];
			sci->type = i;
			sci->base = base;
			sci->credits = scs->size;

			context++;
			base += scs->size;
		}
	}

	return 0;
}

/*
 * Allocate a software index and hardware context of the given type.
 *
 * Must be called with dd->sc_lock held.
 */
static int sc_hw_alloc(struct hfi2_devdata *dd, int type, u32 *sw_index,
		       u32 *hw_context)
{
	struct hfi2_devrsrcs *dr = &dd->rsrcs;
	struct send_context_info *sci;
	u32 index;
	u32 context;

	for (index = 0, sci = &dd->send_contexts[0];
	     index < dd->num_send_contexts; index++, sci++) {
		if (sci->type == type && sci->allocated == 0) {
			sci->allocated = 1;
			/*
			 * Use a 1:1 mapping, but use back-to-front.  This
			 * avoids the reserved range 0..dr->c.first_send_context.
			 */
			context = dr->c.last_send_context - index - 1;
			dd->hw_to_sw[context] = index;
			*sw_index = index;
			*hw_context = context;
			return 0; /* success */
		}
	}
	dd_dev_err(dd, "Unable to locate a free type %d send context\n", type);
	return -ENOSPC;
}

/*
 * Free the send context given by its software index.
 *
 * Must be called with dd->sc_lock held.
 */
static void sc_hw_free(struct hfi2_devdata *dd, u32 sw_index, u32 hw_context)
{
	struct send_context_info *sci;

	sci = &dd->send_contexts[sw_index];
	if (!sci->allocated) {
		dd_dev_err(dd, "%s: sw_index %u not allocated? hw_context %u\n",
			   __func__, sw_index, hw_context);
	}
	sci->allocated = 0;
	dd->hw_to_sw[hw_context] = INVALID_SCI;
}

/* return the base context of a context in a group */
static inline u32 group_context(u32 context, u32 group)
{
	return (context >> group) << group;
}

/* return the size of a group */
static inline u32 group_size(u32 group)
{
	return 1 << group;
}

/*
 * Obtain the credit return addresses, kernel virtual and bus, for the
 * given sc.
 *
 * To understand this routine:
 * o va and dma are arrays of struct credit_return.  One for each physical
 *   send context, per NUMA.
 * o Each send context always looks in its relative location in a struct
 *   credit_return for its credit return.
 * o Each send context in a group must have its return address CSR programmed
 *   with the same value.  Use the address of the first send context in the
 *   group.
 */
static void cr_group_addresses(struct send_context *sc, dma_addr_t *dma)
{
	u32 hw_gc = group_context(sc->hw_context, sc->group);
	u32 index = sc->hw_context & 0x7;
	u32 gc = sc->dd->hw_to_sw[hw_gc];

	sc->hw_free = &sc->dd->cr_base[sc->node].va[gc].cr[index];
	*dma = (unsigned long)&(
		(struct credit_return *)sc->dd->cr_base[sc->node].dma)[gc];
}

/*
 * Work queue function triggered in error interrupt routine for
 * kernel contexts.
 */
static void sc_halted(struct work_struct *work)
{
	struct send_context *sc;

	sc = container_of(work, struct send_context, halt_work);
	hfi2_sc_restart(sc);
}

/*
 * Calculate PIO block threshold for this send context using the given MTU.
 * Trigger a return when one MTU plus optional header of credits remain.
 *
 * Parameter mtu is in bytes.
 * Parameter hdrqentsize is in DWORDs.
 *
 * Return value is what to write into the CSR: trigger return when
 * unreturned credits pass this count.
 */
u32 hfi2_sc_mtu_to_threshold(struct send_context *sc, u32 mtu, u32 hdrqentsize)
{
	u32 release_credits;
	u32 threshold;

	/* add in the header size, then divide by the PIO block size */
	mtu += hdrqentsize << 2;
	release_credits = DIV_ROUND_UP(mtu, PIO_BLOCK_SIZE);

	/* check against this context's credits */
	if (sc->credits <= release_credits)
		threshold = 1;
	else
		threshold = sc->credits - release_credits;

	return threshold;
}

/*
 * Calculate credit threshold in terms of percent of the allocated credits.
 * Trigger when unreturned credits equal or exceed the percentage of the whole.
 *
 * Return value is what to write into the CSR: trigger return when
 * unreturned credits pass this count.
 */
u32 hfi2_sc_percent_to_threshold(struct send_context *sc, u32 percent)
{
	return (sc->credits * percent) / 100;
}

/*
 * Set the credit return threshold.
 */
void hfi2_sc_set_cr_threshold(struct send_context *sc, u32 new_threshold)
{
	unsigned long flags;
	u32 old_threshold;
	int force_return = 0;

	spin_lock_irqsave(&sc->credit_ctrl_lock, flags);

	old_threshold = (sc->credit_ctrl >> SC(CREDIT_CTRL_THRESHOLD_SHIFT)) &
			SC(CREDIT_CTRL_THRESHOLD_MASK);

	if (new_threshold != old_threshold) {
		sc->credit_ctrl =
			(sc->credit_ctrl & ~SC(CREDIT_CTRL_THRESHOLD_SMASK)) |
			((new_threshold & SC(CREDIT_CTRL_THRESHOLD_MASK))
			 << SC(CREDIT_CTRL_THRESHOLD_SHIFT));
		write_sctxt_csr(sc->dd, sc->hw_context,
				sc->dd->params->send_ctxt_credit_ctrl_reg,
				sc->credit_ctrl);

		/* force a credit return on change to avoid a possible stall */
		force_return = 1;
	}

	spin_unlock_irqrestore(&sc->credit_ctrl_lock, flags);

	if (force_return)
		hfi2_sc_return_credits(sc);
}

#define CLEAR_STATIC_RATE_CONTROL_SMASK(r) \
	((r) &= ~SEND_CTXT_CHECK_ENABLE_DISALLOW_PBC_STATIC_RATE_CONTROL_SMASK)

#define SET_STATIC_RATE_CONTROL_SMASK(r) \
	((r) |= SEND_CTXT_CHECK_ENABLE_DISALLOW_PBC_STATIC_RATE_CONTROL_SMASK)

/*
 * set_pio_integrity
 *
 * Set the CHECK_ENABLE register for the send context 'sc'.
 */
void hfi2_wfr_set_pio_integrity(struct hfi2_devdata *dd, u32 pidx,
				u32 hw_context, int type, enum spi_cmds cmd)
{
	u64 val;
	int set;

	/* DEFAULT does not do a read-modify-write */
	if (cmd == SPI_DEFAULT) {
		val = 0;
	} else {
		val = read_epsc_csr(dd, pidx, hw_context,
				    dd->params->send_ctxt_check_enable_reg);
	}

	switch (cmd) {
	case SPI_DEFAULT:
		val = hfi2_pkt_default_send_ctxt_mask(&dd->pport[pidx], type);
		break;
	case SPI_INIT:
		set = type == SC_USER ? HFI2_CAP_IS_USET(STATIC_RATE_CTRL) :
					HFI2_CAP_IS_KSET(STATIC_RATE_CTRL);
		if (set)
			CLEAR_STATIC_RATE_CONTROL_SMASK(val);
		else
			SET_STATIC_RATE_CONTROL_SMASK(val);
		break;
	case SPI_SET_JKEY:
		val |= SEND_CTXT_CHECK_ENABLE_CHECK_JOB_KEY_SMASK;
		break;
	case SPI_CLEAR_JKEY:
		val &= ~SEND_CTXT_CHECK_ENABLE_CHECK_JOB_KEY_SMASK;
		break;
	case SPI_SET_PKEY:
		val |= SEND_CTXT_CHECK_ENABLE_CHECK_PARTITION_KEY_SMASK;
		val &= ~SEND_CTXT_CHECK_ENABLE_DISALLOW_KDETH_PACKETS_SMASK;
		break;
	case SPI_CLEAR_PKEY:
		val &= ~SEND_CTXT_CHECK_ENABLE_CHECK_PARTITION_KEY_SMASK;
		break;
	}
	write_epsc_csr(dd, pidx, hw_context,
		       dd->params->send_ctxt_check_enable_reg, val);
}

static u32 get_buffers_allocated(struct send_context *sc)
{
	int cpu;
	u32 ret = 0;

	for_each_possible_cpu(cpu)
		ret += *per_cpu_ptr(sc->buffers_allocated, cpu);
	return ret;
}

static void reset_buffers_allocated(struct send_context *sc)
{
	int cpu;

	for_each_possible_cpu(cpu)
		(*per_cpu_ptr(sc->buffers_allocated, cpu)) = 0;
}

/*
 * Allocate a NUMA relative send context structure of the given type along
 * with a HW context.
 */
struct send_context *hfi2_sc_alloc(struct hfi2_pportdata *ppd, int type,
				   uint hdrqentsize, int numa)
{
	struct hfi2_devdata *dd = ppd->dd;
	struct send_context_info *sci;
	struct send_context *sc = NULL;
	dma_addr_t dma;
	unsigned long flags;
	u64 reg;
	u32 thresh;
	u32 sw_index;
	u32 hw_context;
	int ret;

	/* do not allocate while frozen */
	if (dd->flags & HFI2_FROZEN)
		return NULL;

	sc = kzalloc_node(sizeof(*sc), GFP_KERNEL, numa);
	if (!sc)
		return NULL;

	sc->buffers_allocated = alloc_percpu(u32);
	if (!sc->buffers_allocated) {
		kfree(sc);
		dd_dev_err(
			dd,
			"Cannot allocate buffers_allocated per cpu counters\n");
		return NULL;
	}

	spin_lock_irqsave(&dd->sc_lock, flags);
	ret = sc_hw_alloc(dd, type, &sw_index, &hw_context);
	if (ret) {
		spin_unlock_irqrestore(&dd->sc_lock, flags);
		free_percpu(sc->buffers_allocated);
		kfree(sc);
		return NULL;
	}

	sci = &dd->send_contexts[sw_index];
	sci->sc = sc;

	sc->dd = dd;
	sc->ppd = ppd;
	sc->node = numa;
	sc->type = type;
	spin_lock_init(&sc->alloc_lock);
	spin_lock_init(&sc->release_lock);
	spin_lock_init(&sc->credit_ctrl_lock);
	seqlock_init(&sc->waitlock);
	INIT_LIST_HEAD(&sc->piowait);
	INIT_WORK(&sc->halt_work, sc_halted);
	init_waitqueue_head(&sc->halt_wait);

	/* grouping is always single context for now */
	/*
	 * changing this may require alignment of dr->c.first_send_context,
	 * or some other compensation/adjustment.
	 */
	sc->group = 0;

	sc->sw_index = sw_index;
	sc->hw_context = hw_context;
	cr_group_addresses(sc, &dma);
	sc->credits = sci->credits;
	sc->size = sc->credits * PIO_BLOCK_SIZE;

/* PIO Send Memory Address details */
#define PIO_ADDR_CONTEXT_MASK 0xfful
#define PIO_ADDR_CONTEXT_SHIFT 16
	sc->base_addr = dd->bar_maps[ctxt_bar_idx(hw_context)].piobase +
			((ctxt_bar_ctxt(hw_context) & PIO_ADDR_CONTEXT_MASK)
			 << PIO_ADDR_CONTEXT_SHIFT);

	/* set base and credits */
	reg = ((sci->credits & SC(CTRL_CTXT_DEPTH_MASK))
	       << SC(CTRL_CTXT_DEPTH_SHIFT)) |
	      ((sci->base & MASK_ULL(dd->params->pio_base_bits))
	       << dd->params->pio_base_shift);

	/* unmask all errors */
	write_sctxt_csr(dd, hw_context, dd->params->send_ctxt_err_mask_reg,
			(u64)-1);

	hfi2_priv_reg_op(dd, ppd->hw_pidx, hw_context, type, SC_CHK_ALLOC_OP,
			 reg);

	/* set up credit return */
	write_sctxt_csr(dd, hw_context,
			dd->params->send_ctxt_credit_return_addr_reg, dma);

	/*
	 * Calculate the initial credit return threshold.
	 *
	 * For Ack contexts, set a threshold for half the credits.
	 * For User contexts use the given percentage.  This has been
	 * sanitized on driver start-up.
	 * For Kernel contexts, use the default MTU plus a header
	 * or half the credits, whichever is smaller. This should
	 * work for both the 3-deep buffering allocation and the
	 * pooling allocation.
	 */
	if (type == SC_ACK) {
		thresh = hfi2_sc_percent_to_threshold(sc, 50);
	} else if (type == SC_USER) {
		thresh = hfi2_sc_percent_to_threshold(
			sc, hfi2_user_credit_return_threshold);
	} else { /* kernel */
		thresh = min(hfi2_sc_percent_to_threshold(sc, 50),
			     hfi2_sc_mtu_to_threshold(sc, hfi2_max_mtu,
						      hdrqentsize));
	}
	reg = thresh << SC(CREDIT_CTRL_THRESHOLD_SHIFT);

	/*
	 * JKR does not support early credit return logic, so early credit return
	 * capability will not be enabled.
	 */
	if (dd->params->chip_type != CHIP_JKR) {
		/* add in early return */
		if (type == SC_USER && HFI2_CAP_IS_USET(EARLY_CREDIT_RETURN))
			reg |= SC(CREDIT_CTRL_EARLY_RETURN_SMASK);
		else if (HFI2_CAP_IS_KSET(
				 EARLY_CREDIT_RETURN)) /* kernel, ack */
			reg |= SC(CREDIT_CTRL_EARLY_RETURN_SMASK);
	}

	/* set up write-through credit_ctrl */
	sc->credit_ctrl = reg;
	write_sctxt_csr(dd, hw_context, dd->params->send_ctxt_credit_ctrl_reg,
			reg);

	spin_unlock_irqrestore(&dd->sc_lock, flags);

	/*
	 * Allocate shadow ring to track outstanding PIO buffers _after_
	 * unlocking.  We don't know the size until the lock is held and
	 * we can't allocate while the lock is held.  No one is using
	 * the context yet, so allocate it now.
	 *
	 * User contexts do not get a shadow ring.
	 */
	if (type != SC_USER) {
		/*
		 * Size the shadow ring 1 larger than the number of credits
		 * so head == tail can mean empty.
		 */
		sc->sr_size = sci->credits + 1;
		sc->sr = kcalloc_node(sc->sr_size,
				      sizeof(union pio_shadow_ring), GFP_KERNEL,
				      numa);
		if (!sc->sr) {
			hfi2_sc_free(sc);
			return NULL;
		}
	}

	hfi2_cdbg(
		PIO,
		"Send context %u(%u) %s group %u credits %u credit_ctrl 0x%llx threshold %u",
		sw_index, hw_context, sc_type_name(type), sc->group,
		sc->credits, sc->credit_ctrl, thresh);

	return sc;
}

/* free a per-NUMA send context structure */
void hfi2_sc_free(struct send_context *sc)
{
	struct hfi2_devdata *dd;
	unsigned long flags;
	u32 sw_index;
	u32 hw_context;
	int pidx;

	if (!sc)
		return;

	sc->flags |= SCF_IN_FREE; /* ensure no restarts */
	dd = sc->dd;
	if (!list_empty(&sc->piowait))
		dd_dev_err(dd, "piowait list not empty!\n");
	pidx = sc->ppd->hw_pidx;
	sw_index = sc->sw_index;
	hw_context = sc->hw_context;
	hfi2_sc_disable(sc); /* make sure the HW is disabled */
	flush_work(&sc->halt_work);

	spin_lock_irqsave(&dd->sc_lock, flags);
	dd->send_contexts[sw_index].sc = NULL;

	/* clear/disable all registers set in hfi2_sc_alloc */
	hfi2_priv_reg_op(dd, pidx, hw_context, sc->type, SC_CHK_FREE_OP, 0);
	write_sctxt_csr(dd, hw_context, dd->params->send_ctxt_err_mask_reg, 0);
	write_sctxt_csr(dd, hw_context,
			dd->params->send_ctxt_credit_return_addr_reg, 0);
	write_sctxt_csr(dd, hw_context, dd->params->send_ctxt_credit_ctrl_reg,
			0);

	/* release the index and context for re-use */
	sc_hw_free(dd, sw_index, hw_context);
	spin_unlock_irqrestore(&dd->sc_lock, flags);

	kfree(sc->sr);
	free_percpu(sc->buffers_allocated);
	kfree(sc);
}

/* disable the context */
void hfi2_sc_disable(struct send_context *sc)
{
	struct pio_buf *pbuf;
	LIST_HEAD(wake_list);

	if (!sc)
		return;

	/* do all steps, even if already disabled */
	spin_lock_irq(&sc->alloc_lock);
	sc->flags &= ~SCF_ENABLED;
	spin_unlock_irq(&sc->alloc_lock);

	sc_wait_for_packet_egress(sc, 1);

	spin_lock_irq(&sc->alloc_lock);
	hfi2_priv_reg_op(sc->dd, 0, sc->hw_context, sc->type, SC_DISABLE_OP, 0);

	/*
	 * Flush any waiters.  Once the context is disabled,
	 * credit return interrupts are stopped (although there
	 * could be one in-process when the context is disabled).
	 * Wait one microsecond for any lingering interrupts, then
	 * proceed with the flush.
	 */
	udelay(1);
	spin_lock(&sc->release_lock);
	if (sc->sr) { /* this context has a shadow ring */
		while (sc->sr_tail != sc->sr_head) {
			pbuf = &sc->sr[sc->sr_tail].pbuf;
			if (pbuf->cb)
				(*pbuf->cb)(pbuf->arg, PRC_SC_DISABLE);
			sc->sr_tail++;
			if (sc->sr_tail >= sc->sr_size)
				sc->sr_tail = 0;
		}
	}
	spin_unlock(&sc->release_lock);

	write_seqlock(&sc->waitlock);
	list_splice_init(&sc->piowait, &wake_list);
	write_sequnlock(&sc->waitlock);
	while (!list_empty(&wake_list)) {
		struct iowait *wait;
		struct rvt_qp *qp;
		struct hfi2_qp_priv *priv;

		wait = list_first_entry(&wake_list, struct iowait, list);
		qp = iowait_to_qp(wait);
		priv = qp->priv;
		list_del_init(&priv->s_iowait.list);
		priv->s_iowait.lock = NULL;
		hfi2_qp_wakeup(qp, RVT_S_WAIT_PIO | HFI2_S_WAIT_PIO_DRAIN);
	}

	spin_unlock_irq(&sc->alloc_lock);
}

/* return SendEgressCtxtStatus.PacketOccupancy */
static u64 packet_occupancy(u64 reg)
{
	return (reg &
		SEND_EGRESS_CTXT_STATUS_CTXT_EGRESS_PACKET_OCCUPANCY_SMASK) >>
	       SEND_EGRESS_CTXT_STATUS_CTXT_EGRESS_PACKET_OCCUPANCY_SHIFT;
}

/* is egress halted on the context? */
static bool egress_halted(u64 reg)
{
	return !!(reg & SEND_EGRESS_CTXT_STATUS_CTXT_EGRESS_HALT_STATUS_SMASK);
}

/* is the send context halted? */
static bool is_sc_halted(struct hfi2_devdata *dd, u32 hw_context)
{
	return !!(read_sctxt_csr(dd, hw_context,
				 dd->params->send_ctxt_status_reg) &
		  SC(STATUS_CTXT_HALTED_SMASK));
}

/**
 * sc_wait_for_packet_egress - wait for packet
 * @sc: valid send context
 * @pause: wait for credit return
 *
 * Wait for packet egress, optionally pause for credit return
 *
 * Egress halt and Context halt are not necessarily the same thing, so
 * check for both.
 *
 * NOTE: The context halt bit may not be set immediately.  Because of this,
 * it is necessary to check the SW SFC_HALTED bit (set in the IRQ) and the HW
 * context bit to determine if the context is halted.
 */
static void sc_wait_for_packet_egress(struct send_context *sc, int pause)
{
	struct hfi2_devdata *dd = sc->dd;
	struct hfi2_pportdata *ppd = sc->ppd;
	u64 reg = 0;
	u64 reg_prev;
	u32 loop = 0;

	while (1) {
		reg_prev = reg;
		reg = hfi2_pf0_read_csr(dd, CSR_TYPE_EPSCARR,
					dd->params->send_egress_ctxt_status_reg,
					sc->hw_context, ppd->hw_pidx);
		/* done if any halt bits, SW or HW are set */
		if (sc->flags & (SCF_HALTED | SCF_LINK_DOWN) ||
		    is_sc_halted(dd, sc->hw_context) || egress_halted(reg))
			break;
		reg = packet_occupancy(reg);
		if (reg == 0)
			break;
		/* counter is reset if occupancy count changes */
		if (reg != reg_prev)
			loop = 0;
		if (loop > 50) {
			/* timed out - bounce the link */
			dd_dev_err(
				dd,
				"%s: context %u(%u) timeout waiting for packets to egress, remaining count %u, bouncing link\n",
				__func__, sc->sw_index, sc->hw_context,
				(u32)reg);
			hfi2_priv_reg_op(dd, ppd->hw_pidx, 0, 0, LINK_BOUNCE_OP,
					 0);
			break;
		}
		loop++;
		mdelay(1);
	}

	if (pause)
		/* Add additional delay to ensure chip returns all credits */
		pause_for_credit_return(dd);
}

void hfi2_sc_wait(struct hfi2_devdata *dd)
{
	int i;

	for (i = 0; i < dd->num_send_contexts; i++) {
		struct send_context *sc = dd->send_contexts[i].sc;

		if (!sc)
			continue;
		sc_wait_for_packet_egress(sc, 0);
	}
}

/*
 * Restart a context after it has been halted due to error.
 *
 * If the first step fails - wait for the halt to be asserted, return early.
 * Otherwise complain about timeouts but keep going.
 *
 * It is expected that allocations (enabled flag bit) have been shut off
 * already (only applies to kernel contexts).
 */
int hfi2_sc_restart(struct send_context *sc)
{
	struct hfi2_devdata *dd = sc->dd;
	u64 reg;
	u32 loop;
	int count;

	/* bounce off if not (halted or link down) or being free'd */
	if (!(sc->flags & (SCF_HALTED | SCF_LINK_DOWN)) ||
	    (sc->flags & SCF_IN_FREE))
		return -EINVAL;

	dd_dev_info(dd, "restarting send context %u(%u)\n", sc->sw_index,
		    sc->hw_context);

	/*
	 * Step 1: Wait for the context to actually halt.
	 *
	 * The error interrupt is asynchronous to actually setting halt
	 * on the context.
	 */
	if (sc->flags & SCF_HALTED) {
		loop = 0;
		while (1) {
			reg = read_sctxt_csr(dd, sc->hw_context,
					     dd->params->send_ctxt_status_reg);
			if (reg & SC(STATUS_CTXT_HALTED_SMASK))
				break;
			if (loop > 100) {
				dd_dev_err(
					dd,
					"%s: context %u(%u) not halting, skipping\n",
					__func__, sc->sw_index, sc->hw_context);
				return -ETIME;
			}
			loop++;
			udelay(1);
		}
	}

	/*
	 * Step 2: Ensure no users are still trying to write to PIO.
	 *
	 * For kernel contexts, we have already turned off buffer allocation.
	 * Now wait for the buffer count to go to zero.
	 *
	 * For user contexts, the user handling code has cut off write access
	 * to the context's PIO pages before calling this routine and will
	 * restore write access after this routine returns.
	 */
	if (sc->type != SC_USER) {
		/* kernel context */
		loop = 0;
		while (1) {
			count = get_buffers_allocated(sc);
			if (count == 0)
				break;
			if (loop > 100) {
				dd_dev_err(
					dd,
					"%s: context %u(%u) timeout waiting for PIO buffers to zero, remaining %d\n",
					__func__, sc->sw_index, sc->hw_context,
					count);
			}
			loop++;
			udelay(1);
		}
	}

	/*
	 * Step 3: Wait for all packets to egress.
	 * This is done while disabling the send context
	 *
	 * Step 4: Disable the context
	 *
	 * This is a superset of the halt.  After the disable, the
	 * errors can be cleared.
	 */
	hfi2_sc_disable(sc);

	/*
	 * Step 5: Enable the context
	 *
	 * This enable will clear the halted flag and per-send context
	 * error flags.
	 */
	return hfi2_sc_enable(sc);
}

/*
 * PIO freeze processing.  To be called after the TXE block is fully frozen.
 * Go through all frozen send contexts and disable them.  The contexts are
 * already stopped by the freeze.
 */
void hfi2_pio_freeze(struct hfi2_devdata *dd)
{
	struct send_context *sc;
	int i;

	for (i = 0; i < dd->num_send_contexts; i++) {
		sc = dd->send_contexts[i].sc;
		/*
		 * Don't disable unallocated, unfrozen, or user send contexts.
		 * User send contexts will be disabled when the process
		 * calls into the driver to reset its context.
		 */
		if (!sc || !(sc->flags & SCF_FROZEN) || sc->type == SC_USER)
			continue;

		/* only need to disable, the context is already stopped */
		hfi2_sc_disable(sc);
	}
}

/*
 * Unfreeze PIO for kernel send contexts.  The precondition for calling this
 * is that all PIO send contexts have been disabled and the SPC freeze has
 * been cleared.  Now perform the last step and re-enable each kernel context.
 * User (PSM) processing will occur when PSM calls into the kernel to
 * acknowledge the freeze.
 */
void hfi2_pio_kernel_unfreeze(struct hfi2_devdata *dd)
{
	struct send_context *sc;
	int i;

	for (i = 0; i < dd->num_send_contexts; i++) {
		sc = dd->send_contexts[i].sc;
		if (!sc || !(sc->flags & SCF_FROZEN) || sc->type == SC_USER)
			continue;
		if (sc->flags & SCF_LINK_DOWN)
			continue;

		hfi2_sc_enable(sc); /* will clear the sc frozen flag */
	}
}

/**
 * hfi2_pio_kernel_linkup() - Re-enable send contexts after linkup event
 * @ppd: port data
 *
 * When the link goes down, the freeze path is taken.  However, a link down
 * event is different from a freeze because if the send context is re-enabled
 * whowever is sending data will start sending data again, which will hang
 * any QP that is sending data.
 *
 * The freeze path now looks at the type of event that occurs and takes this
 * path for link down event.
 */
void hfi2_pio_kernel_linkup(struct hfi2_pportdata *ppd)
{
	struct hfi2_devdata *dd = ppd->dd;
	struct send_context *sc;
	int i;

	for (i = 0; i < dd->num_send_contexts; i++) {
		sc = dd->send_contexts[i].sc;
		if (!sc || !(sc->flags & SCF_LINK_DOWN) || sc->type == SC_USER)
			continue;
		/* this port only */
		if (sc->ppd != ppd)
			continue;

		hfi2_sc_enable(sc); /* will clear the sc link down flag */
	}
}

/*
 * Wait for the SendPioInitCtxt.PioInitInProgress bit to clear.
 * Returns:
 *	-ETIMEDOUT - if we wait too long
 *	-EIO	   - if there was an error
 */
static int pio_init_wait_progress(struct hfi2_devdata *dd)
{
	u64 reg;
	int max, count = 0;

	/* max is the longest possible HW init time / delay */
	max = 5;
	while (1) {
		reg = hfi2_read_csr(dd, dd->params->send_pio_init_ctxt_reg);
		if (!(reg & SEND_PIO_INIT_CTXT_PIO_INIT_IN_PROGRESS_SMASK))
			break;
		if (count >= max)
			return -ETIMEDOUT;
		udelay(5);
		count++;
	}

	return reg & SEND_PIO_INIT_CTXT_PIO_INIT_ERR_SMASK ? -EIO : 0;
}

static int __pio_reset(struct hfi2_devdata *dd, u64 reg)
{
	hfi2_write_csr(dd, dd->params->send_pio_init_ctxt_reg, reg);
	/*
	 * Wait until the engine is done.  Give the chip the required time
	 * so, hopefully, we read the register just once.
	 */
	udelay(2);
	return pio_init_wait_progress(dd);
}

/*
 * Reset all of the send contexts to their power-on state.  Used
 * only during manual init - no lock against hfi2_sc_enable needed.
 */
void hfi2_pio_reset_all(struct hfi2_devdata *dd)
{
	int ret;

	/* make sure the init engine is not busy */
	ret = pio_init_wait_progress(dd);
	/* ignore any timeout */
	if (ret == -EIO) {
		/* clear the error */
		hfi2_write_csr(dd, dd->params->send_pio_err_clear_reg,
			       SEND_PIO_ERR_CLEAR_PIO_INIT_SM_IN_ERR_SMASK);
	}

	/* reset init all */
	ret = __pio_reset(dd, SEND_PIO_INIT_CTXT_PIO_ALL_CTXT_INIT_SMASK);
	if (ret < 0) {
		dd_dev_err(
			dd,
			"PIO send context init %s while initializing all PIO blocks\n",
			ret == -ETIMEDOUT ? "is stuck" : "had an error");
	}
}

int hfi2_pio_reset_one(struct hfi2_devdata *dd, u16 ctxt)
{
	u64 reg;
	int ret;

	/*
	 * The HW PIO initialization engine can handle only one init
	 * request at a time. Serialize access to each device's engine.
	 */
	spin_lock(&dd->sc_init_lock);
	/*
	 * Since access to this code block is serialized and
	 * each access waits for the initialization to complete
	 * before releasing the lock, the PIO initialization engine
	 * should not be in use, so we don't have to wait for the
	 * InProgress bit to go down.
	 */
	reg = ((ctxt & SEND_PIO_INIT_CTXT_PIO_CTXT_NUM_MASK)
	       << SEND_PIO_INIT_CTXT_PIO_CTXT_NUM_SHIFT) |
	      SEND_PIO_INIT_CTXT_PIO_SINGLE_CTXT_INIT_SMASK;
	ret = __pio_reset(dd, reg);
	spin_unlock(&dd->sc_init_lock);
	if (ret) {
		dd_dev_err(
			dd,
			"sctxt(%u): Context not enabled due to init failure %d\n",
			ctxt, ret);
	}
	return ret;
}

/* enable the context */
int hfi2_sc_enable(struct send_context *sc)
{
	u64 reg;
	struct hfi2_devdata *dd;
	unsigned long flags;
	int ret = 0;

	if (!sc)
		return -EINVAL;
	dd = sc->dd;

	/*
	 * Obtain the allocator lock to guard against any allocation
	 * attempts (which should not happen prior to context being
	 * enabled). On the release/disable side we don't need to
	 * worry about locking since the releaser will not do anything
	 * if the context accounting values have not changed.
	 */
	spin_lock_irqsave(&sc->alloc_lock, flags);
	if (sc->flags & SCF_ENABLED)
		goto unlock; /* already enabled */

	/* IMPORTANT: only clear free and fill if transitioning 0 -> 1 */

	*sc->hw_free = 0;
	sc->free = 0;
	sc->alloc_free = 0;
	sc->fill = 0;
	sc->fill_wrap = 0;
	sc->sr_head = 0;
	sc->sr_tail = 0;
	sc->flags = 0;
	/* the alloc lock insures no fast path allocation */
	reset_buffers_allocated(sc);

	/*
	 * Clear all per-context errors.  Some of these will be set when
	 * we are re-enabling after a context halt.  Now that the context
	 * is disabled, the halt will not clear until after the PIO init
	 * engine runs below.
	 */
	reg = read_sctxt_csr(dd, sc->hw_context,
			     dd->params->send_ctxt_err_status_reg);
	if (reg)
		write_sctxt_csr(dd, sc->hw_context,
				dd->params->send_ctxt_err_clear_reg, reg);

	ret = hfi2_priv_reg_op(dd, 0, sc->hw_context, sc->type, SC_ENABLE_OP,
			       0);
	if (ret)
		goto unlock;

	sc->flags |= SCF_ENABLED;

unlock:
	spin_unlock_irqrestore(&sc->alloc_lock, flags);

	return ret;
}

/* force a credit return on the context */
void hfi2_sc_return_credits(struct send_context *sc)
{
	if (!sc)
		return;

	/* a 0->1 transition schedules a credit return */
	write_sctxt_csr(sc->dd, sc->hw_context,
			sc->dd->params->send_ctxt_credit_force_reg,
			SC(CREDIT_FORCE_FORCE_RETURN_SMASK));
	/*
	 * Ensure that the write is flushed and the credit return is
	 * scheduled. We care more about the 0 -> 1 transition.
	 */
	read_sctxt_csr(sc->dd, sc->hw_context,
		       sc->dd->params->send_ctxt_credit_force_reg);
	/* set back to 0 for next time */
	write_sctxt_csr(sc->dd, sc->hw_context,
			sc->dd->params->send_ctxt_credit_force_reg, 0);
}

/* allow all in-flight packets to drain on the context */
void hfi2_sc_flush(struct send_context *sc)
{
	if (!sc)
		return;

	sc_wait_for_packet_egress(sc, 1);
}

/*
 * Start the software reaction to a context halt or SPC freeze:
 *	- mark the context as halted or frozen
 *	- stop buffer allocations
 *
 * Called from the error interrupt.  Other work is deferred until
 * out of the interrupt.
 */
void hfi2_sc_stop(struct send_context *sc, int flag)
{
	unsigned long flags;

	/* stop buffer allocations */
	spin_lock_irqsave(&sc->alloc_lock, flags);
	/* mark the context */
	sc->flags |= flag;
	sc->flags &= ~SCF_ENABLED;
	spin_unlock_irqrestore(&sc->alloc_lock, flags);
	wake_up(&sc->halt_wait);
}

#define BLOCK_DWORDS (PIO_BLOCK_SIZE / sizeof(u32))
#define dwords_to_blocks(x) DIV_ROUND_UP(x, BLOCK_DWORDS)

/*
 * The send context buffer "allocator".
 *
 * @sc: the PIO send context we are allocating from
 * @len: length of whole packet - including PBC - in dwords
 * @cb: optional callback to call when the buffer is finished sending
 * @arg: argument for cb
 *
 * Return a pointer to a PIO buffer, NULL if not enough room, -ECOMM
 * when link is down.
 */
struct pio_buf *hfi2_sc_buffer_alloc(struct send_context *sc, u32 dw_len,
				     pio_release_cb cb, void *arg)
{
	struct pio_buf *pbuf = NULL;
	unsigned long flags;
	unsigned long avail;
	unsigned long blocks = dwords_to_blocks(dw_len);
	u32 fill_wrap;
	int trycount = 0;
	u32 head, next;

	spin_lock_irqsave(&sc->alloc_lock, flags);
	if (!(sc->flags & SCF_ENABLED)) {
		spin_unlock_irqrestore(&sc->alloc_lock, flags);
		return ERR_PTR(-ECOMM);
	}

retry:
	avail = (unsigned long)sc->credits - (sc->fill - sc->alloc_free);
	if (blocks > avail) {
		/* not enough room */
		if (unlikely(trycount)) { /* already tried to get more room */
			spin_unlock_irqrestore(&sc->alloc_lock, flags);
			goto done;
		}
		/* copy from receiver cache line and recalculate */
		sc->alloc_free = READ_ONCE(sc->free);
		avail = (unsigned long)sc->credits -
			(sc->fill - sc->alloc_free);
		if (blocks > avail) {
			/* still no room, actively update */
			hfi2_sc_release_update(sc);
			sc->alloc_free = READ_ONCE(sc->free);
			trycount++;
			goto retry;
		}
	}

	/* there is enough room */

	preempt_disable();
	this_cpu_inc(*sc->buffers_allocated);

	/* read this once */
	head = sc->sr_head;

	/* "allocate" the buffer */
	sc->fill += blocks;
	fill_wrap = sc->fill_wrap;
	sc->fill_wrap += blocks;
	if (sc->fill_wrap >= sc->credits)
		sc->fill_wrap = sc->fill_wrap - sc->credits;

	/*
	 * Fill the parts that the releaser looks at before moving the head.
	 * The only necessary piece is the sent_at field.  The credits
	 * we have just allocated cannot have been returned yet, so the
	 * cb and arg will not be looked at for a "while".  Put them
	 * on this side of the memory barrier anyway.
	 */
	pbuf = &sc->sr[head].pbuf;
	pbuf->sent_at = sc->fill;
	pbuf->cb = cb;
	pbuf->arg = arg;
	pbuf->sc = sc; /* could be filled in at sc->sr init time */
	/* make sure this is in memory before updating the head */

	/* calculate next head index, do not store */
	next = head + 1;
	if (next >= sc->sr_size)
		next = 0;
	/*
	 * update the head - must be last! - the releaser can look at fields
	 * in pbuf once we move the head
	 */
	smp_wmb();
	sc->sr_head = next;
	spin_unlock_irqrestore(&sc->alloc_lock, flags);

	/* finish filling in the buffer outside the lock */
	pbuf->start = sc->base_addr + fill_wrap * PIO_BLOCK_SIZE;
	pbuf->end = sc->base_addr + sc->size;
	pbuf->qw_written = 0;
	pbuf->carry_bytes = 0;
	pbuf->carry.val64 = 0;
done:
	return pbuf;
}

/*
 * There are at least two entities that can turn on credit return
 * interrupts and they can overlap.  Avoid problems by implementing
 * a count scheme that is enforced by a lock.  The lock is needed because
 * the count and CSR write must be paired.
 */

/*
 * Start credit return interrupts.  This is managed by a count.  If already
 * on, just increment the count.
 */
void hfi2_sc_add_credit_return_intr(struct send_context *sc)
{
	unsigned long flags;

	/* lock must surround both the count change and the CSR update */
	spin_lock_irqsave(&sc->credit_ctrl_lock, flags);
	if (sc->credit_intr_count == 0) {
		sc->credit_ctrl |= SC(CREDIT_CTRL_CREDIT_INTR_SMASK);
		write_sctxt_csr(sc->dd, sc->hw_context,
				sc->dd->params->send_ctxt_credit_ctrl_reg,
				sc->credit_ctrl);
	}
	sc->credit_intr_count++;
	spin_unlock_irqrestore(&sc->credit_ctrl_lock, flags);
}

/*
 * Stop credit return interrupts.  This is managed by a count.  Decrement the
 * count, if the last user, then turn the credit interrupts off.
 */
void hfi2_sc_del_credit_return_intr(struct send_context *sc)
{
	unsigned long flags;

	WARN_ON(sc->credit_intr_count == 0);

	/* lock must surround both the count change and the CSR update */
	spin_lock_irqsave(&sc->credit_ctrl_lock, flags);
	sc->credit_intr_count--;
	if (sc->credit_intr_count == 0) {
		sc->credit_ctrl &= ~SC(CREDIT_CTRL_CREDIT_INTR_SMASK);
		write_sctxt_csr(sc->dd, sc->hw_context,
				sc->dd->params->send_ctxt_credit_ctrl_reg,
				sc->credit_ctrl);
	}
	spin_unlock_irqrestore(&sc->credit_ctrl_lock, flags);
}

/*
 * The caller must be careful when calling this.  All needint calls
 * must be paired with !needint.
 */
void hfi2_sc_wantpiobuf_intr(struct send_context *sc, u32 needint)
{
	if (needint)
		hfi2_sc_add_credit_return_intr(sc);
	else
		hfi2_sc_del_credit_return_intr(sc);
	trace_hfi2_wantpiointr(sc, needint, sc->credit_ctrl);
	if (needint)
		hfi2_sc_return_credits(sc);
}

/**
 * sc_piobufavail - callback when a PIO buffer is available
 * @sc: the send context
 *
 * This is called from the interrupt handler when a PIO buffer is
 * available after hfi2_verbs_send() returned an error that no buffers were
 * available. Disable the interrupt if there are no more QPs waiting.
 */
static void sc_piobufavail(struct send_context *sc)
{
	struct hfi2_devdata *dd = sc->dd;
	struct list_head *list;
	struct rvt_qp *qps[PIO_WAIT_BATCH_SIZE];
	struct rvt_qp *qp;
	struct hfi2_qp_priv *priv;
	unsigned long flags;
	uint i, n = 0, top_idx = 0;

	if (dd->send_contexts[sc->sw_index].type != SC_KERNEL &&
	    dd->send_contexts[sc->sw_index].type != SC_VL15)
		return;
	list = &sc->piowait;
	/*
	 * Note: checking that the piowait list is empty and clearing
	 * the buffer available interrupt needs to be atomic or we
	 * could end up with QPs on the wait list with the interrupt
	 * disabled.
	 */
	write_seqlock_irqsave(&sc->waitlock, flags);
	while (!list_empty(list)) {
		struct iowait *wait;

		if (n == ARRAY_SIZE(qps))
			break;
		wait = list_first_entry(list, struct iowait, list);
		iowait_get_priority(wait);
		qp = iowait_to_qp(wait);
		priv = qp->priv;
		list_del_init(&priv->s_iowait.list);
		priv->s_iowait.lock = NULL;
		if (n) {
			priv = qps[top_idx]->priv;
			top_idx = hfi2_iowait_priority_update_top(
				wait, &priv->s_iowait, n, top_idx);
		}

		/* refcount held until actual wake up */
		qps[n++] = qp;
	}
	/*
	 * If there had been waiters and there are more
	 * insure that we redo the force to avoid a potential hang.
	 */
	if (n) {
		hfi2_sc_wantpiobuf_intr(sc, 0);
		if (!list_empty(list))
			hfi2_sc_wantpiobuf_intr(sc, 1);
	}
	write_sequnlock_irqrestore(&sc->waitlock, flags);

	/* Wake up the top-priority one first */
	if (n)
		hfi2_qp_wakeup(qps[top_idx],
			       RVT_S_WAIT_PIO | HFI2_S_WAIT_PIO_DRAIN);
	for (i = 0; i < n; i++)
		if (i != top_idx)
			hfi2_qp_wakeup(qps[i],
				       RVT_S_WAIT_PIO | HFI2_S_WAIT_PIO_DRAIN);
}

/* translate a send credit update to a bit code of reasons */
static inline int fill_code(u64 hw_free)
{
	int code = 0;

	if (hw_free & CR_STATUS_SMASK)
		code |= PRC_STATUS_ERR;
	if (hw_free & CR_CREDIT_RETURN_DUE_TO_PBC_SMASK)
		code |= PRC_PBC;
	if (hw_free & CR_CREDIT_RETURN_DUE_TO_THRESHOLD_SMASK)
		code |= PRC_THRESHOLD;
	if (hw_free & CR_CREDIT_RETURN_DUE_TO_ERR_SMASK)
		code |= PRC_FILL_ERR;
	if (hw_free & CR_CREDIT_RETURN_DUE_TO_FORCE_SMASK)
		code |= PRC_SC_DISABLE;
	return code;
}

/* use the jiffies compare to get the wrap right */
#define sent_before(a, b) time_before(a, b) /* a < b */

/*
 * The send context buffer "releaser".
 */
void hfi2_sc_release_update(struct send_context *sc)
{
	struct pio_buf *pbuf;
	u64 hw_free;
	u32 head, tail;
	unsigned long old_free;
	unsigned long free;
	unsigned long extra;
	unsigned long flags;
	int code;

	if (!sc)
		return;

	spin_lock_irqsave(&sc->release_lock, flags);
	/* update free */
	hw_free = le64_to_cpu(*sc->hw_free); /* volatile read */
	old_free = sc->free;
	extra = (((hw_free & CR_COUNTER_SMASK) >> CR_COUNTER_SHIFT) -
		 (old_free & CR_COUNTER_MASK)) &
		CR_COUNTER_MASK;
	free = old_free + extra;
	trace_hfi2_piofree(sc, extra);

	/* call sent buffer callbacks */
	code = -1; /* code not yet set */
	head = READ_ONCE(sc->sr_head); /* snapshot the head */
	tail = sc->sr_tail;
	while (head != tail) {
		pbuf = &sc->sr[tail].pbuf;

		if (sent_before(free, pbuf->sent_at)) {
			/* not sent yet */
			break;
		}
		if (pbuf->cb) {
			if (code < 0) /* fill in code on first user */
				code = fill_code(hw_free);
			(*pbuf->cb)(pbuf->arg, code);
		}

		tail++;
		if (tail >= sc->sr_size)
			tail = 0;
	}
	sc->sr_tail = tail;
	/* make sure tail is updated before free */
	smp_wmb();
	sc->free = free;
	spin_unlock_irqrestore(&sc->release_lock, flags);
	sc_piobufavail(sc);
}

/*
 * Send context group releaser.  Argument is the send context that caused
 * the interrupt.  Called from the send context interrupt handler.
 *
 * Call release on all contexts in the group.
 *
 * This routine takes the sc_lock without an irqsave because it is only
 * called from an interrupt handler.  Adjust if that changes.
 */
void hfi2_sc_group_release_update(struct hfi2_devdata *dd, u32 hw_context)
{
	struct send_context *sc;
	u32 sw_index;
	u32 gc, gc_end;

	spin_lock(&dd->sc_lock);
	sw_index = dd->hw_to_sw[hw_context];
	if (unlikely(sw_index >= dd->num_send_contexts)) {
		dd_dev_err(dd, "%s: invalid hw (%u) to sw (%u) mapping\n",
			   __func__, hw_context, sw_index);
		goto done;
	}
	sc = dd->send_contexts[sw_index].sc;
	if (unlikely(!sc))
		goto done;

	gc = group_context(hw_context, sc->group);
	gc_end = gc + group_size(sc->group);
	for (; gc < gc_end; gc++) {
		sw_index = dd->hw_to_sw[gc];
		if (unlikely(sw_index >= dd->num_send_contexts)) {
			dd_dev_err(dd,
				   "%s: invalid hw (%u) to sw (%u) mapping\n",
				   __func__, hw_context, sw_index);
			continue;
		}
		hfi2_sc_release_update(dd->send_contexts[sw_index].sc);
	}
done:
	spin_unlock(&dd->sc_lock);
}

/*
 * hfi2_pio_select_send_context_vl() - select send context
 * @dd: devdata
 * @selector: a spreading factor
 * @vl: this vl
 *
 * This function returns a send context based on the selector and a vl.
 * The mapping fields are protected by RCU
 */
struct send_context *hfi2_pio_select_send_context_vl(struct hfi2_pportdata *ppd,
						     u32 selector, u8 vl)
{
	struct pio_vl_map *m;
	struct pio_map_elem *e;
	struct send_context *rval;

	/*
	 * NOTE This should only happen if SC->VL changed after the initial
	 * checks on the QP/AH
	 * Default will return VL0's send context below
	 */
	if (unlikely(vl >= hfi2_num_vls)) {
		rval = NULL;
		goto done;
	}

	rcu_read_lock();
	m = rcu_dereference(ppd->pio_map);
	if (unlikely(!m)) {
		rcu_read_unlock();
		return ppd->vld[0].sc;
	}
	e = m->map[vl & m->mask];
	rval = e->ksc[selector & e->mask];
	rcu_read_unlock();

done:
	rval = !rval ? ppd->vld[0].sc : rval;
	return rval;
}

/*
 * hfi2_pio_select_send_context_sc() - select send context
 * @dd: devdata
 * @selector: a spreading factor
 * @sc5: the 5 bit sc
 *
 * This function returns an send context based on the selector and an sc
 */
struct send_context *hfi2_pio_select_send_context_sc(struct hfi2_pportdata *ppd,
						     u32 selector, u8 sc5)
{
	u8 vl = sc_to_vlt(ppd, sc5);

	return hfi2_pio_select_send_context_vl(ppd, selector, vl);
}

/*
 * Free the indicated map struct
 */
static void pio_map_free(struct pio_vl_map *m)
{
	int i;

	for (i = 0; m && i < m->actual_vls; i++)
		kfree(m->map[i]);
	kfree(m);
}

/*
 * Handle RCU callback
 */
static void pio_map_rcu_callback(struct rcu_head *list)
{
	struct pio_vl_map *m = container_of(list, struct pio_vl_map, list);

	pio_map_free(m);
}

/*
 * Set credit return threshold for the kernel send context
 */
static void set_threshold(struct hfi2_pportdata *ppd, int scontext, int i)
{
	struct send_context *sc = ppd->kernel_send_context[scontext];
	u32 thres;

	thres = min(hfi2_sc_percent_to_threshold(sc, 50),
		    hfi2_sc_mtu_to_threshold(sc, sc->ppd->vld[i].mtu,
					     kctxt_hdrqentsize(sc->ppd)));
	hfi2_sc_set_cr_threshold(sc, thres);
}

/*
 * hfi2_pio_map_init - called when #vls change
 * @dd: hfi2_devdata
 * @hfi2_num_vls: number of vls
 *
 * This routine changes the vl to send context mapping based on the number of
 * vls and available send contexts.
 *
 * The auto algorithm computes the sc_per_vl and the number of extra send
 * contexts. Any extra send contexts are added from the highest VL on down
 *
 * rcu locking is used to control access to the mapping fields.
 *
 * If either the hfi2_num_vls or vl_scontexts[vl] are non-power of 2, the array
 * sizes in the struct pio_vl_map and the struct pio_map_elem are rounded up
 * to the next highest power of 2 and the first entry is reused in a round
 * robin fashion.
 *
 * If an error occurs the mapping is not changed.
 */
int hfi2_pio_map_init(struct hfi2_pportdata *ppd, u8 hfi2_num_vls)
{
	struct hfi2_devdata *dd = ppd->dd;
	int i, j;
	int extra, sc_per_vl;
	int scontext = 1; /* first non-vl15 kernel_send_context */
	int num_kernel_send_contexts = 0;
	int vl_scontexts[OPA_MAX_VLS];
	struct pio_vl_map *oldmap, *newmap;

	/* assign a count of send contexts to each VL */
	/* count kernel send contexts for this port */
	for (i = 0; i < dd->num_send_contexts; i++) {
		if (dd->send_contexts[i].type != SC_KERNEL)
			continue;
		if (!dd->send_contexts[i].sc)
			continue;
		if (dd->send_contexts[i].sc->ppd != ppd)
			continue;
		num_kernel_send_contexts++;
	}
	/* truncate divide */
	sc_per_vl = num_kernel_send_contexts / hfi2_num_vls;
	/* extras */
	extra = num_kernel_send_contexts % hfi2_num_vls;
	/* add extras from last vl down */
	for (i = hfi2_num_vls - 1; i >= 0; i--, extra--)
		vl_scontexts[i] = sc_per_vl + (extra > 0 ? 1 : 0);

	/* build new map */
	newmap = kzalloc(struct_size(newmap, map,
				     roundup_pow_of_two(hfi2_num_vls)),
			 GFP_KERNEL);
	if (!newmap)
		goto bail;
	newmap->actual_vls = hfi2_num_vls;
	newmap->vls = roundup_pow_of_two(hfi2_num_vls);
	newmap->mask = (1 << ilog2(newmap->vls)) - 1;
	for (i = 0; i < newmap->vls; i++) {
		/* save for wrap around */
		int first_scontext = scontext;

		if (i < newmap->actual_vls) {
			int sz = roundup_pow_of_two(vl_scontexts[i]);

			/* only allocate once */
			newmap->map[i] =
				kzalloc(struct_size(newmap->map[i], ksc, sz),
					GFP_KERNEL);
			if (!newmap->map[i])
				goto bail;
			newmap->map[i]->mask = (1 << ilog2(sz)) - 1;
			/*
			 * assign send contexts and
			 * adjust credit return threshold
			 */
			for (j = 0; j < sz; j++) {
				if (ppd->kernel_send_context[scontext]) {
					newmap->map[i]->ksc[j] =
						ppd->kernel_send_context
							[scontext];
					set_threshold(ppd, scontext, i);
				}
				if (++scontext >=
				    first_scontext + vl_scontexts[i])
					/* wrap back to first send context */
					scontext = first_scontext;
			}
		} else {
			/* just re-use entry without allocating */
			newmap->map[i] = newmap->map[i % hfi2_num_vls];
		}
		scontext = first_scontext + vl_scontexts[i];
	}
	/* newmap in hand, save old map */
	spin_lock_irq(&dd->pio_map_lock);
	oldmap = rcu_dereference_protected(ppd->pio_map,
					   lockdep_is_held(&dd->pio_map_lock));

	/* publish newmap */
	rcu_assign_pointer(ppd->pio_map, newmap);

	spin_unlock_irq(&dd->pio_map_lock);
	/* success, free any old map after grace period */
	if (oldmap)
		call_rcu(&oldmap->list, pio_map_rcu_callback);
	return 0;
bail:
	/* free any partial allocation */
	pio_map_free(newmap);
	return -ENOMEM;
}

void hfi2_free_pio_map(struct hfi2_devdata *dd)
{
	struct hfi2_pportdata *ppd;
	int i;

	for (i = 0; i < dd->num_pports; i++) {
		ppd = dd->pport + i;
		/* Free PIO map if allocated */
		if (rcu_access_pointer(ppd->pio_map)) {
			spin_lock_irq(&dd->pio_map_lock);
			pio_map_free(rcu_access_pointer(ppd->pio_map));
			RCU_INIT_POINTER(ppd->pio_map, NULL);
			spin_unlock_irq(&dd->pio_map_lock);
			synchronize_rcu();
		}
		kfree(ppd->kernel_send_context);
		ppd->kernel_send_context = NULL;
	}
}

int hfi2_init_pervl_scs(struct hfi2_pportdata *ppd)
{
	struct hfi2_devdata *dd = ppd->dd;
	struct send_context *sc;
	int i;
	u64 mask;
	const u64 all_vl_mask = (u64)0x80ff; /* VLs 0-7, 15 */
	const u64 data_vls_mask = (u64)0x00ff; /* VLs 0-7 */
	u32 ctxt;
	u8 rcvhdrqentsize;

	/* do nothing for an unavailable port */
	if (!port_available_ppd(ppd))
		return 0;

	rcvhdrqentsize = kctxt_hdrqentsize(ppd);
	ppd->vld[15].sc = hfi2_sc_alloc(ppd, SC_VL15, rcvhdrqentsize, dd->node);
	if (!ppd->vld[15].sc)
		return -ENOMEM;

	hfi2_init_ctxt(ppd->vld[15].sc);
	ppd->vld[15].mtu = hfi2_enum_to_mtu(OPA_MTU_2048);

	ppd->kernel_send_context = kcalloc_node(dd->num_send_contexts,
						sizeof(struct send_context *),
						GFP_KERNEL, dd->node);
	if (!ppd->kernel_send_context)
		goto freesc15;

	ppd->kernel_send_context[0] = ppd->vld[15].sc;

	for (i = 0; i < hfi2_num_vls; i++) {
		sc = hfi2_sc_alloc(ppd, SC_KERNEL, rcvhdrqentsize, dd->node);
		if (!sc)
			goto nomem;
		hfi2_init_ctxt(sc);
		ppd->kernel_send_context[i + 1] = sc;
		ppd->vld[i].sc = sc;
		/* non VL15 start with the max MTU */
		ppd->vld[i].mtu = hfi2_max_mtu;
	}
	for (i = hfi2_num_vls; i < INIT_SC_PER_VL * hfi2_num_vls; i++) {
		sc = hfi2_sc_alloc(ppd, SC_KERNEL, rcvhdrqentsize, dd->node);
		if (!sc)
			goto nomem;
		hfi2_init_ctxt(sc);
		ppd->kernel_send_context[i + 1] = sc;
	}

	hfi2_sc_enable(ppd->vld[15].sc);
	ctxt = ppd->vld[15].sc->hw_context;
	mask = all_vl_mask & ~(1LL << 15);
	hfi2_priv_reg_op(dd, ppd->hw_pidx, ctxt, ppd->vld[15].sc->type,
			 SC_CHK_VL_MASK_OP, mask);
	dd_dev_info(dd, "pidx %d: Using send context %u(%u) for VL15\n",
		    ppd->hw_pidx, ppd->vld[15].sc->sw_index, ctxt);

	for (i = 0; i < hfi2_num_vls; i++) {
		hfi2_sc_enable(ppd->vld[i].sc);
		ctxt = ppd->vld[i].sc->hw_context;
		mask = all_vl_mask & ~(data_vls_mask);
		hfi2_priv_reg_op(dd, ppd->hw_pidx, ctxt, ppd->vld[i].sc->type,
				 SC_CHK_VL_MASK_OP, mask);
	}
	for (i = hfi2_num_vls; i < INIT_SC_PER_VL * hfi2_num_vls; i++) {
		hfi2_sc_enable(ppd->kernel_send_context[i + 1]);
		ctxt = ppd->kernel_send_context[i + 1]->hw_context;
		mask = all_vl_mask & ~(data_vls_mask);
		hfi2_priv_reg_op(dd, ppd->hw_pidx, ctxt,
				 ppd->kernel_send_context[i + 1]->type,
				 SC_CHK_VL_MASK_OP, mask);
	}

	if (hfi2_pio_map_init(ppd, hfi2_num_vls))
		goto nomem;
	return 0;

nomem:
	for (i = 0; i < hfi2_num_vls; i++) {
		hfi2_sc_free(ppd->vld[i].sc);
		ppd->vld[i].sc = NULL;
	}

	for (i = hfi2_num_vls; i < INIT_SC_PER_VL * hfi2_num_vls; i++)
		hfi2_sc_free(ppd->kernel_send_context[i + 1]);

	kfree(ppd->kernel_send_context);
	ppd->kernel_send_context = NULL;

freesc15:
	hfi2_sc_free(ppd->vld[15].sc);
	ppd->vld[15].sc = NULL;
	return -ENOMEM;
}

int hfi2_init_credit_return(struct hfi2_devdata *dd)
{
	struct hfi2_devrsrcs *dr = &dd->rsrcs;
	size_t bytes = (dr->c.last_send_context - dr->c.first_send_context) *
		       sizeof(struct credit_return);
	int ret;
	int i;

	dd->cr_base = kcalloc(num_possible_nodes(),
			      sizeof(struct credit_return_base), GFP_KERNEL);
	if (!dd->cr_base) {
		ret = -ENOMEM;
		goto done;
	}
	for_each_node_with_cpus(i) {
		set_dev_node(&dd->pcidev->dev, i);
		dd->cr_base[i].va = dma_alloc_coherent(&dd->pcidev->dev, bytes,
						       &dd->cr_base[i].dma,
						       GFP_KERNEL);
		if (!dd->cr_base[i].va) {
			set_dev_node(&dd->pcidev->dev, dd->node);
			dd_dev_err(
				dd,
				"Unable to allocate credit return DMA range for NUMA %d\n",
				i);
			ret = -ENOMEM;
			goto free_cr_base;
		}
	}
	set_dev_node(&dd->pcidev->dev, dd->node);

	ret = 0;
done:
	return ret;

free_cr_base:
	hfi2_free_credit_return(dd);
	goto done;
}

void hfi2_free_credit_return(struct hfi2_devdata *dd)
{
	struct hfi2_devrsrcs *dr = &dd->rsrcs;
	size_t bytes = (dr->c.last_send_context - dr->c.first_send_context) *
		       sizeof(struct credit_return);
	int i;

	if (!dd->cr_base)
		return;
	for (i = 0; i < num_possible_nodes(); i++) {
		if (dd->cr_base[i].va) {
			dma_free_coherent(&dd->pcidev->dev, bytes,
					  dd->cr_base[i].va,
					  dd->cr_base[i].dma);
		}
	}
	kfree(dd->cr_base);
	dd->cr_base = NULL;
}

void hfi2_seqfile_dump_sci(struct seq_file *s, u32 i,
			   struct send_context_info *sci)
{
	struct send_context *sc = sci->sc;
	u64 reg;

	seq_printf(s, "SCI %u: type %u base %u credits %u\n", i, sci->type,
		   sci->base, sci->credits);
	seq_printf(s, "  flags 0x%x sw_inx %u hw_ctxt %u grp %u\n", sc->flags,
		   sc->sw_index, sc->hw_context, sc->group);
	seq_printf(s, "  sr_size %u credits %u sr_head %u sr_tail %u\n",
		   sc->sr_size, sc->credits, sc->sr_head, sc->sr_tail);
	seq_printf(s, "  fill %lu free %lu fill_wrap %u alloc_free %lu\n",
		   sc->fill, sc->free, sc->fill_wrap, sc->alloc_free);
	seq_printf(s, "  credit_intr_count %u credit_ctrl 0x%llx\n",
		   sc->credit_intr_count, sc->credit_ctrl);
	reg = read_sctxt_csr(sc->dd, sc->hw_context,
			     sc->dd->params->send_ctxt_credit_status_reg);
	seq_printf(s, "  *hw_free %llu CurrentFree %llu LastReturned %llu\n",
		   (le64_to_cpu(*sc->hw_free) & CR_COUNTER_SMASK) >>
			   CR_COUNTER_SHIFT,
		   (reg >> SC(CREDIT_STATUS_CURRENT_FREE_COUNTER_SHIFT)) &
			   SC(CREDIT_STATUS_CURRENT_FREE_COUNTER_MASK),
		   reg & SC(CREDIT_STATUS_LAST_RETURNED_COUNTER_SMASK));
}
