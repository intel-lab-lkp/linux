// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * Copyright(c) 2018 - 2020 Intel Corporation.
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */

#include "hfi2.h"
#include "sdma.h"
#include "netdev.h"
#include "vf2pf.h"

/**
 * hfi2_msix_initialize() - Calculate, request and configure MSIx IRQs
 * @dd: valid hfi2 devdata
 *
 */
int hfi2_msix_initialize(struct hfi2_devdata *dd)
{
	struct hfi2_devrsrcs *dr = &dd->rsrcs;
	u32 total;
	int ret;
	int pidx;
	struct hfi2_msix_entry *entries;

	/*
	 * MSIx interrupt count:
	 *	one for the general, "slow path" interrupt
	 *	as needed for vf2pf
	 *	one per used SDMA engine
	 *	one per kernel receive context
	 *      ...any new IRQs should be added here.
	 */
	total = 1 + hfi2_vf2pf_num_irq(dd) +
		(dr->last_sdma_engine - dr->first_sdma_engine);
	for (pidx = 0; pidx < dd->num_pports; pidx++) {
		struct hfi2_portrsrcs *pr = &dr->ppr[pidx];

		total += pr->n_krcv_queues + pr->num_netdev_contexts;
	}

	if (total >= CCE_NUM_MSIX_VECTORS)
		return -EINVAL;

	ret = pci_alloc_irq_vectors(dd->pcidev, total, total, PCI_IRQ_MSIX);
	if (ret < 0) {
		dd_dev_err(dd, "pci_alloc_irq_vectors() failed: %d\n", ret);
		return ret;
	}

	entries =
		kcalloc(total, sizeof(*dd->msix_info.msix_entries), GFP_KERNEL);
	if (!entries) {
		pci_free_irq_vectors(dd->pcidev);
		return -ENOMEM;
	}

	dd->msix_info.msix_entries = entries;
	spin_lock_init(&dd->msix_info.msix_lock);
	bitmap_zero(dd->msix_info.in_use_msix, total);
	dd->msix_info.max_requested = total;
	dd_dev_info(dd, "%u MSI-X interrupts allocated\n", total);

	return 0;
}

/**
 * msix_request_irq() - Allocate a free MSIx IRQ
 * @dd: valid devdata
 * @arg: context information for the IRQ
 * @handler: IRQ handler
 * @thread: IRQ thread handler (could be NULL)
 * @type: affinty IRQ type
 * @name: IRQ name
 *
 * Allocated an MSIx vector if available, and then create the appropriate
 * meta data needed to keep track of the pci IRQ request.
 *
 * Return:
 *   < 0   Error
 *   >= 0  MSIx vector
 *
 */
static int msix_request_irq(struct hfi2_devdata *dd, void *arg,
			    irq_handler_t handler, irq_handler_t thread,
			    enum irq_type type, const char *name)
{
	unsigned long nr;
	int irq;
	int ret;
	struct hfi2_msix_entry *me;

	/* Allocate an MSIx vector */
	spin_lock(&dd->msix_info.msix_lock);
	nr = find_first_zero_bit(dd->msix_info.in_use_msix,
				 dd->msix_info.max_requested);
	if (nr < dd->msix_info.max_requested)
		__set_bit(nr, dd->msix_info.in_use_msix);
	spin_unlock(&dd->msix_info.msix_lock);

	if (nr == dd->msix_info.max_requested) {
		pr_err("%s: failed, nr %ld, max_requested %d, -ENOSPC\n",
		       __func__, nr, dd->msix_info.max_requested);
		return -ENOSPC;
	}

	if (type < IRQ_SDMA || type >= IRQ_OTHER)
		return -EINVAL;

	irq = pci_irq_vector(dd->pcidev, nr);
	ret = pci_request_irq(dd->pcidev, nr, handler, thread, arg, name);
	if (ret) {
		dd_dev_err(dd,
			   "%s: request for IRQ %d failed, MSIx %lx, err %d\n",
			   name, irq, nr, ret);
		spin_lock(&dd->msix_info.msix_lock);
		__clear_bit(nr, dd->msix_info.in_use_msix);
		spin_unlock(&dd->msix_info.msix_lock);
		return ret;
	}

	/*
	 * assign arg after pci_request_irq call, so it will be
	 * cleaned up
	 */
	me = &dd->msix_info.msix_entries[nr];
	me->irq = irq;
	me->arg = arg;
	me->type = type;

	return nr;
}

static int msix_request_rcd_irq_common(struct hfi2_ctxtdata *rcd,
				       irq_handler_t handler,
				       irq_handler_t thread, const char *name)
{
	u32 source;
	int nr;

	nr = msix_request_irq(rcd->dd, rcd, handler, thread, IRQ_RCVCTXT, name);
	if (nr < 0)
		return nr;

	/*
	 * Set the interrupt register and mask for this context's interrupt.
	 */
	source = rcd->dd->params->is_rcvavail_start + rcd->ctxt;
	rcd->ireg = source / 64;
	rcd->imask = ((u64)1) << (source % 64);
	rcd->msix_intr = nr;
	hfi2_remap_intr(rcd->dd, source, nr);

	return 0;
}

/**
 * hfi2_msix_request_rcd_irq() - Helper function for RCVAVAIL IRQs
 * @rcd: valid rcd context
 *
 */
int hfi2_msix_request_rcd_irq(struct hfi2_ctxtdata *rcd)
{
	char name[MAX_NAME_SIZE];

	snprintf(name, sizeof(name), DRIVER_NAME "_%d kctxt%d", rcd->dd->unit,
		 rcd->ctxt);

	return msix_request_rcd_irq_common(rcd, hfi2_receive_context_interrupt,
					   hfi2_receive_context_thread, name);
}

/**
 * hfi2_msix_netdev_request_rcd_irq  - Helper function for RCVAVAIL IRQs
 * for netdev context
 * @rcd: valid netdev contexti
 */
int hfi2_msix_netdev_request_rcd_irq(struct hfi2_ctxtdata *rcd)
{
	char name[MAX_NAME_SIZE];

	snprintf(name, sizeof(name), DRIVER_NAME "_%d nd kctxt%d",
		 rcd->dd->unit, rcd->ctxt);
	return msix_request_rcd_irq_common(rcd, hfi2_receive_context_interrupt_napi,
					   NULL, name);
}

/**
 * hfi2_msix_request_sdma_irq  - Helper for getting SDMA IRQ resources
 * @sde: valid sdma engine
 *
 */
int hfi2_msix_request_sdma_irq(struct sdma_engine *sde)
{
	int nr;
	char name[MAX_NAME_SIZE];

	snprintf(name, sizeof(name), DRIVER_NAME "_%d sdma%d", sde->dd->unit,
		 sde->this_idx);
	nr = msix_request_irq(sde->dd, sde, hfi2_sdma_interrupt, hfi2_sdma_interrupt_thr,
			      IRQ_SDMA, name);
	if (nr < 0)
		return nr;
	sde->msix_intr = nr;
	hfi2_remap_sdma_interrupts(sde->dd, sde->this_idx, nr);

	return 0;
}

/**
 * hfi2_msix_request_general_irq - Helper for getting general IRQ
 * resources
 * @dd: valid device data
 */
int hfi2_msix_request_general_irq(struct hfi2_devdata *dd)
{
	int nr;
	char name[MAX_NAME_SIZE];

	snprintf(name, sizeof(name), DRIVER_NAME "_%d", dd->unit);
	nr = msix_request_irq(dd, dd, hfi2_general_interrupt, NULL, IRQ_GENERAL,
			      name);
	if (nr < 0)
		return nr;

	/* general interrupt must be MSIx vector 0 */
	if (nr) {
		hfi2_msix_free_irq(dd, (u8)nr);
		dd_dev_err(dd, "Invalid index %d for GENERAL IRQ\n", nr);
		return -EINVAL;
	}

	return 0;
}

/**
 * enable_sdma_srcs - Helper to enable SDMA IRQ srcs
 * @dd: valid devdata structure
 * @i: index of SDMA engine
 */
static void enable_sdma_srcs(struct hfi2_devdata *dd, int i)
{
	hfi2_set_intr_bits(dd, dd->params->is_sdma_start + i,
		      dd->params->is_sdma_start + i, true);
	hfi2_set_intr_bits(dd, dd->params->is_sdma_progress_start + i,
		      dd->params->is_sdma_progress_start + i, true);
	hfi2_set_intr_bits(dd, dd->params->is_sdma_idle_start + i,
		      dd->params->is_sdma_idle_start + i, true);
	hfi2_set_intr_bits(dd, dd->params->is_sdmaeng_err_start + i,
		      dd->params->is_sdmaeng_err_start + i, true);
}

/**
 * hfi2_msix_request_irqs() - Allocate SDMA and receive IRQs
 * @dd: valid devdata structure
 *
 * Helper function to request MSIx IRQs for SDMA and receive.
 */
int hfi2_msix_request_irqs(struct hfi2_devdata *dd)
{
	struct hfi2_devrsrcs *dr = &dd->rsrcs;
	int i;
	int j;
	int ret = 0;

	for (i = dr->first_sdma_engine; i < dr->last_sdma_engine; i++) {
		struct sdma_engine *sde = &dd->per_sdma[i];

		ret = hfi2_msix_request_sdma_irq(sde);
		if (ret)
			return ret;
		enable_sdma_srcs(sde->dd, i);
	}

	for (i = 0; i < dd->num_pports; i++) {
		struct hfi2_portrsrcs *pr = &dr->ppr[i];

		for (j = 0; j < pr->n_krcv_queues; j++) {
			u16 ctxt = pr->rcv_context_base + j;
			struct hfi2_ctxtdata *rcd =
				hfi2_rcd_get_by_index(dd, ctxt);

			if (rcd)
				ret = hfi2_msix_request_rcd_irq(rcd);
			hfi2_rcd_put(rcd);
			if (ret)
				return ret;
		}
	}

	return 0;
}

/**
 * hfi2_msix_early_request_irqs() - Allocate needed early IRQs.
 * @dd: valid devdata structure
 *
 * Helper function to request an MSIx IRQs for anthing needed early in the
 * device initialize.  Presently, only the general interrupt handler.
 */
int hfi2_msix_early_request_irqs(struct hfi2_devdata *dd)
{
	int ret;

	ret = hfi2_msix_request_general_irq(dd);
	if (ret)
		return ret;
	/*
	 * Only VFs can/must init VF2PF IRQs this early.
	 * The PF must wait until CPORT f/w has reset all
	 * resources in hfi2_start_cport().
	 */
	if (dd->is_vf)
		ret = hfi2_vf2pf_init_irq(dd);

	return ret;
}

/**
 * hfi2_msix_free_irq() - Free the specified MSIx resources and IRQ
 * @dd: valid devdata
 * @msix_intr: MSIx vector to free.
 *
 */
void hfi2_msix_free_irq(struct hfi2_devdata *dd, u8 msix_intr)
{
	struct hfi2_msix_entry *me;

	if (msix_intr >= dd->msix_info.max_requested)
		return;

	me = &dd->msix_info.msix_entries[msix_intr];

	if (!me->arg)
		return;

	pci_free_irq(dd->pcidev, msix_intr, me->arg);

	me->arg = NULL;

	spin_lock(&dd->msix_info.msix_lock);
	__clear_bit(msix_intr, dd->msix_info.in_use_msix);
	spin_unlock(&dd->msix_info.msix_lock);
}

/**
 * hfi2_msix_clean_up_interrupts  - Free all MSIx IRQ resources
 * @dd: valid device data structure
 *
 * Free the MSIx and associated PCI resources, if they have been allocated.
 */
void hfi2_msix_clean_up_interrupts(struct hfi2_devdata *dd)
{
	int i;

	/* remove irqs - must happen before disabling/turning off */
	for (i = 0; i < dd->msix_info.max_requested; i++)
		hfi2_msix_free_irq(dd, i);

	/* clean structures */
	kfree(dd->msix_info.msix_entries);
	dd->msix_info.msix_entries = NULL;
	dd->msix_info.max_requested = 0;

	pci_free_irq_vectors(dd->pcidev);
}

/*
 * hfi2_msix_shut_down_interrupts - Free all or most IRQs
 * @dd: device data structure
 * @keep_gen: when true, keep general interrupt
 *
 * Free all IRQs with the possible exception of the general IRQ.  Retain all
 * structures.  This should eventually be followed by a call to
 * hfi2_msix_clean_up_interrupts().
 */
void hfi2_msix_shut_down_interrupts(struct hfi2_devdata *dd, bool keep_gen)
{
	struct hfi2_msix_entry *me;
	int i;

	/* remove irqs - must happen before disabling/turning off */
	for (i = 0; i < dd->msix_info.max_requested; i++) {
		me = &dd->msix_info.msix_entries[i];
		if (keep_gen && me->type == IRQ_GENERAL)
			continue;
		hfi2_msix_free_irq(dd, i);
	}
}

/**
 * hfi2_msix_netdev_synchronize_irq - netdev IRQ synchronize
 * @ppd: valid port data
 */
void hfi2_msix_netdev_synchronize_irq(struct hfi2_pportdata *ppd)
{
	int i;
	int ctxt_count = hfi2_netdev_ctxt_count(ppd);

	for (i = 0; i < ctxt_count; i++) {
		struct hfi2_ctxtdata *rcd = hfi2_netdev_get_ctxt(ppd, i);
		struct hfi2_msix_entry *me;

		me = &ppd->dd->msix_info.msix_entries[rcd->msix_intr];

		synchronize_irq(me->irq);
	}
}

int hfi2_msix_request_irq_remap(struct hfi2_devdata *dd, u16 ctxt,
			   enum irq_type type, int src, irq_handler_t handler,
			   irq_handler_t thread, void *arg, const char *name)
{
	int nr;

	nr = msix_request_irq(dd, arg, handler, thread, type, name);
	if (nr < 0)
		return nr;

	src = dd->params->is_rcvavail_start + ctxt;
	hfi2_remap_intr(dd, src, nr);
	return nr;
}
