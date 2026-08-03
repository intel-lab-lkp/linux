// SPDX-License-Identifier: GPL-2.0 or BSD-3-Clause
/*
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 *
 * SR-IOV related functions and variables.
 */

#include <linux/pci.h>

#include "hfi2.h"
#include "sriov.h"
#include "chip_jkr.h"
#include "chip_gen.h"
#include "vf2pf.h"

#define HFI_SRIOV_DEBUG
#define HFI_SRIOV_BRINGUP
#define HFI_SRIOV_MOD_PARAMS /* use module params to define SRIOV config */
#undef HFI_SRIOV_AUTO_CONF /* automatically enable SRIOV if max_num_vfs > 0 */

static bool sriov_auto; /* might default to 'true' in the future */

#ifdef HFI_SRIOV_BRINGUP
static bool vf_test;

static bool vf_claim;
#endif

#ifdef HFI_SRIOV_MOD_PARAMS
static uint si_idx;
static uint max_num_vfs;
static uint ctxt_per_vf; /* send/recv use same number */
static uint sdma_per_vf;
static uint rcv_per_vf;
static uint pio_per_vf;
#endif

#define HFI_MIN_PF0_CONTEXTS 32 /* includes max used by CPORT */
#define HFI_MIN_PF0_SDE 2

/*
 * Only called on PF0, but possibly on behalf of VF.
 *
 * 'dd' is the PF0, 'out' is the target based on 'si'.
 * 'out' is assumed to be uninitialized.
 */
int hfi2_sriov_get_config(struct hfi2_devdata *dd, struct hfi2_devrsrcs *out, int si)
{
	u32 num_ctxt, pf0_ctxts, vf2pf_ctxts;
	u32 num_sdma;
	u32 num_rcvary;
	u32 num_pio;
	int num_vfs = 0;
	int num;

#ifdef HFI_SRIOV_MOD_PARAMS
	num_vfs = max_num_vfs;
#endif

	if (!num_vfs)
		return -ENODEV;

	if (si > JKR_C_CCE_NUM_VFS || num_vfs > JKR_C_CCE_NUM_VFS)
		return -EINVAL;

#ifdef HFI_SRIOV_MOD_PARAMS
	/*
	 * Assumes #send contexts == #recv contexts, uses #recv
	 * as the limit since #send might be larger in the future.
	 */
	vf2pf_ctxts = hfi2_vf2pf_num_ctxts(dd);
	num_ctxt = chip_rcv_contexts(dd) - vf2pf_ctxts;
	num_sdma = chip_sdma_engines(dd);
	num_rcvary = chip_rcv_array_count(dd) - HFI_MIN_PF0_RCVARY(vf2pf_ctxts);
	num_pio = chip_pio_mem_size(dd) / PIO_BLOCK_SIZE -
		  HFI_MIN_PF0_PIO(vf2pf_ctxts);

	/*
	 * Automatically adjust excessive values and set defaults for '0'.
	 * This may not work well if adapters of different architecture are
	 * installed (and having SRIOV capability).
	 */
	if (!ctxt_per_vf)
		ctxt_per_vf = (num_ctxt - HFI_MIN_PF0_CONTEXTS) / max_num_vfs;
	if (ctxt_per_vf * max_num_vfs > num_ctxt - HFI_MIN_PF0_CONTEXTS) {
		ctxt_per_vf = (num_ctxt - HFI_MIN_PF0_CONTEXTS) / max_num_vfs;
		dd_dev_info(dd, "Reducing ctxt_per_vf to %d\n", ctxt_per_vf);
	}
	pf0_ctxts = num_ctxt - ctxt_per_vf * max_num_vfs;

	if (!sdma_per_vf)
		sdma_per_vf = (num_sdma - HFI_MIN_PF0_SDE) / max_num_vfs;
	if (!rcv_per_vf)
		rcv_per_vf = (num_rcvary - HFI_MIN_PF0_RCVARY(pf0_ctxts)) /
			     max_num_vfs;
	if (!pio_per_vf)
		pio_per_vf =
			(num_pio - HFI_MIN_PF0_PIO(pf0_ctxts)) / max_num_vfs;

	if (sdma_per_vf * max_num_vfs > num_sdma - HFI_MIN_PF0_SDE) {
		sdma_per_vf = (num_sdma - HFI_MIN_PF0_SDE) / max_num_vfs;
		dd_dev_info(dd, "Reducing sdma_per_vf to %d\n", sdma_per_vf);
	}
	if (rcv_per_vf * max_num_vfs >
	    num_rcvary - HFI_MIN_PF0_RCVARY(pf0_ctxts)) {
		rcv_per_vf = (num_rcvary - HFI_MIN_PF0_RCVARY(pf0_ctxts)) /
			     max_num_vfs;
		dd_dev_info(dd, "Reducing rcv_per_vf to %d\n", rcv_per_vf);
	}
	if (pio_per_vf * max_num_vfs > num_pio - HFI_MIN_PF0_PIO(pf0_ctxts)) {
		pio_per_vf =
			(num_pio - HFI_MIN_PF0_PIO(pf0_ctxts)) / max_num_vfs;
		dd_dev_info(dd, "Reducing pio_per_vf to %d\n", pio_per_vf);
	}

	out->num_vfs = max_num_vfs;
	out->si_idx = si;
	out->pfunit = dd->unit;
	/* si_idx == 0 implies PF0, which decides all VF resources */

	num = si ? si : max_num_vfs + 1; /* never 0 */
	out->c.first_send_context = si ? num_ctxt - num * ctxt_per_vf : 0;
	out->c.last_send_context = num_ctxt - (num - 1) * ctxt_per_vf;
	out->c.first_rcv_context = si ? num_ctxt - num * ctxt_per_vf : 0;
	out->c.last_rcv_context = num_ctxt - (num - 1) * ctxt_per_vf;
	out->first_sdma_engine = si ? num_sdma - num * sdma_per_vf : 0;
	out->last_sdma_engine = num_sdma - (num - 1) * sdma_per_vf;
	out->c.first_rcvarray_entry = si ? num_rcvary - num * rcv_per_vf : 0;
	out->c.last_rcvarray_entry = num_rcvary - (num - 1) * rcv_per_vf;
	out->c.first_pio_block = si ? num_pio - num * pio_per_vf : 0;
	out->c.last_pio_block = num_pio - (num - 1) * pio_per_vf;
#endif
	return 0;
}

/*
 * If SRIOV is allowed (max_num_vfs > 0) then divide up
 * resources according to heuristics or limits. The rest of the driver
 * init should use these parameters, if available (dd->rsrcs.num_vfs != 0).
 * Note that the driver can no longer assume a resource begins at "0".
 */
int hfi2_sriov_set_cfg(struct hfi2_devdata *dd)
{
	int ret;

	if (dd->params->chip_type == CHIP_WFR)
		return 0;
	/* prior call to hfi2_sriov_set_si() must have succeeded */
	if (dd->is_vf) {
		ret = hfi2_vf2pf_get_config(dd, &dd->rsrcs, dd->rsrcs.si_idx);
		return ret;
	}
	if (!hfi2_sriov_is_enabled())
		return 0;

	ret = hfi2_sriov_get_config(dd, &dd->rsrcs, 0);
	return ret;
}

int hfi2_sriov_set_si(struct hfi2_devdata *dd)
{
	int si = 0; /* assume PF0 to start */

	if (!dd->is_vf)
		goto out_set;

	si = si_idx;
	if (!si) {
		if (dd->is_vm)
			si = hfi2_vf2pf_probe_si(dd); /* hope for the best */
		else
			si = pci_iov_vf_id(dd->pcidev) + 1;
	}
	if (!si) {
		dd_dev_err(dd, "Cannot determine device SI\n");
		return -ENXIO;
	}
out_set:
	dd->rsrcs.si_idx = si;
	return 0;
}

/*
 * Sets the SiIdx CSRs of contrexts and SDMA engines for
 * VF1..VFn based on data passed in 'dr'.
 *
 * Only PF0 can access the SiIdx CSRS, so any other callers
 * just return success.
 *
 * Called on behalf of a VF when it makes first contact with PF0.
 */
int hfi2_sriov_assign_rsrcs(struct hfi2_devdata *dd, struct hfi2_devrsrcs *dr)
{
	int x;

	if (dd->is_vf)
		return hfi2_vf2pf_assign_rsrcs(dd, dr);

	/* Only valid on PF0, if SRIOV is allowed */
	if (!dd->rsrcs.num_vfs || dd->rsrcs.si_idx)
		return 0;

	/*
	 * In theory, resources allocated to PF0 never need to have their
	 * SiIdx written.
	 */
	if (!dr->si_idx)
		return 0;

	for (x = dr->c.first_send_context; x < dr->c.last_send_context; ++x)
		write_tctxt_csr(dd, x, JKR_SEND_CTXT_SI_IDX, dr->si_idx);
	for (x = dr->c.first_rcv_context; x < dr->c.last_rcv_context; ++x)
		write_rctxt_csr(dd, x, JKR_RCV_SI_IDX, dr->si_idx);
	for (x = dr->first_sdma_engine; x < dr->last_sdma_engine; ++x)
		write_sdmacfg_csr(dd, x, JKR_SEND_DMA_CFG_SI_IDX, dr->si_idx);
	return 0;
}

/*
 * Resets the SiIdx CSRs of contrexts and SDMA engines for
 * VF1..VFn back to PF0.
 *
 * Only PF0 can access the SiIdx CSRS, so any other callers
 * just return success.
 *
 * Called on behalf of a VF when it disconnects with PF0.
 */
void hfi2_sriov_free_rsrcs(struct hfi2_devdata *dd, struct hfi2_devrsrcs *dr)
{
	int x;

	if (dd->is_vf) {
		hfi2_vf2pf_free_rsrcs(dd, dr);
		return;
	}
	/* Only valid on PF0, if SRIOV is allowed */
	if (!dd->rsrcs.num_vfs || dd->rsrcs.si_idx)
		return;

	/*
	 * In theory, resources allocated to PF0 never need to have their
	 * SiIdx written.
	 */
	if (!dr->si_idx)
		return;

	for (x = dr->c.first_send_context; x < dr->c.last_send_context; ++x)
		write_tctxt_csr(dd, x, JKR_SEND_CTXT_SI_IDX, 0);
	for (x = dr->c.first_rcv_context; x < dr->c.last_rcv_context; ++x)
		write_rctxt_csr(dd, x, JKR_RCV_SI_IDX, 0);
	for (x = dr->first_sdma_engine; x < dr->last_sdma_engine; ++x)
		write_sdmacfg_csr(dd, x, JKR_SEND_DMA_CFG_SI_IDX, 0);
}

/*
 * Free all SRIOV configuration resources. Called during
 * driver unload.
 *
 * Resets SiIdx CSRs as well as freeing memory.
 */
void hfi2_sriov_free_cfg(struct hfi2_devdata *dd)
{
	/* On PF0, set all SiIdx back to 0 */
	hfi2_sriov_free_rsrcs(dd, &dd->rsrcs);
}

/* NOTE:
 * There are three types of PCI devices when using SRIOV and VMs:
 *	in host OS, there is the PF0 pci_dev (pdev->is_physfn != 0)
 *	in host OS, there is the VFx pci_dev (pdev->is_virtfn != 0)
 *		(this will never have a 'dd'?)
 *	in guest OS, there is the VFx pci_dev (PCI_FUNC(pdev->devfn) != 0?)
 *		(what do pdev->is_physfn and pdev->is_virtfn mean?
 *		since pdev->physfn cannot be valid - nor pdev->sriov?)
 *		(this needs to have a 'dd' but handled different)
 *
 *	PCI_FUNC(pdev->devfn) != 0 && !pdev->is_virtfn for guest VFs?
 *
 */

int hfi2_sriov_is_enabled(void)
{
#ifdef HFI_SRIOV_MOD_PARAMS
	return max_num_vfs > 0;
#else
	return 0;
#endif
}

/*
 * This initializes the host VF PCI device - not SRIOV from PF0.
 *
 * Normally, the VF devices will be pass-through to VMs, in which
 * case the host driver does not want to claim the device.
 * by calling the remove_one() function when assigning the device
 * to the guest? If so, we can go ahead and claim the device here,
 * however that will cause a complete setup (and tear-down) of a 'dd' for it.
 *
 * pdev is the VF. Only called from host driver.
 */
int hfi2_sriov_init(struct pci_dev *pdev)
{
	int ret;

#ifdef CONFIG_HFI_L8SIM
	ret = sim_sriov_fixup(pdev);
	if (ret)
		dev_warn(&pdev->dev, "SRIOV simpci fixup failed %d\n", ret);
		/* continue, even though it probably won't work */
#endif
#ifdef HFI_SRIOV_BRINGUP
	if (vf_test) {
		ret = pci_enable_device(pdev);
		if (ret) {
			dev_err(&pdev->dev, "SRIOV pci enable failed %d\n",
				ret);
			return ret;
		}
		pci_set_master(pdev);
	}
	ret = vf_claim ? 0 : -ENODEV;
#else
	ret = -ENODEV;
#endif
	/*
	 * If not claimed then remove_one()/hfi2_sriov_remove() will never be called.
	 */

	/* pci_num_vf(pdev->physfn) is not valid until later, so can't use it */
#ifdef HFI_SRIOV_DEBUG
	dev_info(&pdev->dev, "probing VF%d (%d)\n", pci_iov_vf_id(pdev) + 1,
		 ret);
	dev_warn(&pdev->dev,
		 "is_vm=%d is_vf=%d is_physfn=%d is_virtfn=%d physfn=%p\n",
#if defined(CONFIG_X86)
		 boot_cpu_has(X86_FEATURE_HYPERVISOR),
#else
		 -1,
#endif
		 !pdev->pm_cap, pdev->is_physfn, pdev->is_virtfn, pdev->physfn);
#endif

	return ret;
}

void hfi2_sriov_remove(struct pci_dev *pdev)
{
#ifdef HFI_SRIOV_DEBUG
	dev_info(&pdev->dev, "removing VF%d\n", pci_iov_vf_id(pdev) + 1);
#endif
}

/*
 * This disables SRIOV from PF0, if it was enabled.
 *
 * pdev is PF0. The driver is about to release this PF0.
 */
int hfi2_sriov_disable(struct pci_dev *pdev)
{
	pci_disable_sriov(pdev);
	return 0;
}

/*
 * Deconfigure SRIOV on PF0. The driver may continue to run on PF0.
 */
static int hfi2_sriov_deconfigure(struct hfi2_devdata *dd)
{
	pci_disable_sriov(dd->pcidev);
	hfi2_pf0_cleanup(dd);
	return 0;
}

int hfi2_sriov_configure(struct pci_dev *pdev, int nvf)
{
	struct hfi2_devdata *dd = pci_get_drvdata(pdev);
	int ret;

	if (!nvf)
		return hfi2_sriov_deconfigure(dd);

	if (nvf > max_num_vfs)
		return -EINVAL;

	/* prepare VF resources (contexts) for creation of VFs */
	ret = hfi2_vf2pf_prep(dd);
	if (ret)
		return ret;

	ret = pci_enable_sriov(pdev, nvf);
	if (ret < 0) {
		hfi2_sriov_deconfigure(dd);
		return ret;
	}
	return nvf;
}

/*
 * Enables SRIOV if max_num_vfs > 0.
 *
 * Called at the very end of PF0 initialization (init_one()).
 */
int hfi2_sriov_auto_conf(struct hfi2_devdata *dd)
{
	int ret = 0;

	if (!max_num_vfs)
		return ret;

	if (sriov_auto) {
		ret = hfi2_sriov_configure(dd->pcidev, max_num_vfs);
		if (ret)
			dd_dev_err(dd,
				   "hfi2_sriov_configure(%d) failed (%d).\n",
				   max_num_vfs, ret);
	}
	return ret;
}
