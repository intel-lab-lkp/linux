// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) 2023, Intel Corporation. */

#include "ice.h"
#include "ice_lib.h"
#include "ice_irq.h"

static int
ice_init_virt_irq_tracker(struct ice_pf *pf, u32 base, u32 num_entries)
{
	pf->virt_irq_tracker.bm = bitmap_zalloc(num_entries, GFP_KERNEL);
	if (!pf->virt_irq_tracker.bm)
		return -ENOMEM;

	pf->virt_irq_tracker.num_entries = num_entries;
	pf->virt_irq_tracker.base = base;

	return 0;
}

static void ice_deinit_virt_irq_tracker(struct ice_pf *pf)
{
	bitmap_free(pf->virt_irq_tracker.bm);
}

#define ICE_RDMA_AEQ_MSIX 1
static int ice_get_default_msix_amount(struct ice_pf *pf)
{
	return ICE_MIN_LAN_OICR_MSIX + netif_get_num_default_rss_queues() +
	       (test_bit(ICE_FLAG_FD_ENA, pf->flags) ? ICE_FDIR_MSIX : 0) +
	       (ice_is_rdma_ena(pf) ? netif_get_num_default_rss_queues() +
				      ICE_RDMA_AEQ_MSIX : 0);
}

/**
 * ice_clear_interrupt_scheme - Undo things done by ice_init_interrupt_scheme
 * @pf: board private structure
 */
void ice_clear_interrupt_scheme(struct ice_pf *pf)
{
	libie_irq_deinit(&pf->irq);
	ice_deinit_virt_irq_tracker(pf);
}

/**
 * ice_init_interrupt_scheme - Determine proper interrupt scheme
 * @pf: board private structure to initialize
 */
int ice_init_interrupt_scheme(struct ice_pf *pf)
{
	int total_vectors = pf->hw.func_caps.common_cap.num_msix_vectors;
	int err;

	/* load default PF MSI-X range */
	if (!pf->msix.min)
		pf->msix.min = ICE_MIN_MSIX;

	if (!pf->msix.max)
		pf->msix.max = min(total_vectors,
				   ice_get_default_msix_amount(pf));

	pf->msix.total = total_vectors;
	pf->msix.rest = total_vectors - pf->msix.max;

	err = libie_irq_init(&pf->irq, pf->pdev, pf->msix.min, pf->msix.max);
	if (err)
		return err;

	err = ice_init_virt_irq_tracker(pf, pf->msix.max, pf->msix.rest);
	if (err)
		libie_irq_deinit(&pf->irq);

	return err;
}

/**
 * ice_virt_get_irqs - get irqs for SR-IOV usacase
 * @pf: pointer to PF structure
 * @needed: number of irqs to get
 *
 * This returns the first MSI-X vector index in PF space that is used by this
 * VF. This index is used when accessing PF relative registers such as
 * GLINT_VECT2FUNC and GLINT_DYN_CTL.
 * This will always be the OICR index in the AVF driver so any functionality
 * using vf->first_vector_idx for queue configuration_id: id of VF which will
 * use this irqs
 */
int ice_virt_get_irqs(struct ice_pf *pf, u32 needed)
{
	int res = bitmap_find_next_zero_area(pf->virt_irq_tracker.bm,
					     pf->virt_irq_tracker.num_entries,
					     0, needed, 0);

	if (res >= pf->virt_irq_tracker.num_entries)
		return -ENOENT;

	bitmap_set(pf->virt_irq_tracker.bm, res, needed);

	/* conversion from number in bitmap to global irq index */
	return res + pf->virt_irq_tracker.base;
}

/**
 * ice_virt_free_irqs - free irqs used by the VF
 * @pf: pointer to PF structure
 * @index: first index to be free
 * @irqs: number of irqs to free
 */
void ice_virt_free_irqs(struct ice_pf *pf, u32 index, u32 irqs)
{
	bitmap_clear(pf->virt_irq_tracker.bm, index - pf->virt_irq_tracker.base,
		     irqs);
}
