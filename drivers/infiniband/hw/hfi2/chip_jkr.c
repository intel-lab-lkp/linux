// SPDX-License-Identifier: GPL-2.0 or BSD-3-Clause
/*
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */

#include "hfi2.h"
#include "trace.h"
#include "chip_jkr.h"
#include "cport.h"
#include "sriov.h"
#include "vf2pf.h"

int hfi2_jkr_find_used_resources(struct hfi2_devdata *dd)
{
	struct hfi2_devrsrcs *dr = &dd->rsrcs;
	u64 val;
	bool found_first_unused;
	int i;

	/*
	 * This sets up boundaries only. PF0 must initialize all and
	 * assign to SI(s), as well as avoid CPORT ones.
	 */
	if (dr->num_vfs) {
		/* resources already setup by hfi2_sriov_set_cfg() */
		if (dd->is_vf)
			goto out;
	} else {
		dr->pfunit = dd->unit;
		dr->c.first_send_context = 0;
		dr->c.last_send_context = chip_send_contexts(dd);
		dr->c.first_rcv_context = 0;
		dr->c.last_rcv_context = chip_rcv_contexts(dd);
		dr->c.first_rcvarray_entry = 0;
		dr->c.last_rcvarray_entry = chip_rcv_array_count(dd);
		dr->c.first_pio_block = 0;
		dr->c.last_pio_block = chip_pio_mem_size(dd) / PIO_BLOCK_SIZE;
	}

	/*
	 * Find reserved resources.  Expectations:  All used resources are
	 * at the front of the resource.  If this is not the case, there
	 * will be wasted resources.
	 */

	/*
	 * Look for send reserved.
	 */
	found_first_unused = false;
	for (i = 0; i < dr->c.last_send_context; i++) {
		val = hfi2_read_ctxt_csr(dd, JKR_SEND_CTXT_SI_IDX, i, 8);
		if (val == 0) { /* 0 means pf0 */
			/* this context is for the driver */
			if (!found_first_unused) {
				found_first_unused = true;
				dr->c.first_send_context = i;
			}
		} else {
			u32 base; /* in blocks */
			u32 size; /* in blocks */

			/* this context is non-driver */
			if (found_first_unused) {
				/*
				 * Expect an initial set of non-driver
				 * contexts, then all driver after that.
				 */
				return -EINVAL;
			}
			/* read PIO send resources for this context */
			val = read_tctxt_csr(dd, i,
					     dd->params->send_ctxt_ctrl_reg);
			base = (val >> JKR_SEND_CTXT_CTRL_CTXT_BASE_SHIFT) &
			       MASK_ULL(dd->params->pio_base_bits);
			size = (val >> SEND_CTXT_CTRL_CTXT_DEPTH_SHIFT) &
			       SEND_CTXT_CTRL_CTXT_DEPTH_MASK;
			dd_dev_info(
				dd,
				"Non driver send ctxt %d: base 0x%x, size 0x%x\n",
				i, base, size);
			/*
			 * Expect the non-driver contexts to use the blocks in
			 * increasing groups.  Warn otherwise.  This is a simple
			 * attempt to warn if there may be wasted reserved
			 * blocks.  I.e. no holes.  Doing this right would
			 * involve much more complicated range lists that are
			 * not worth doing.
			 */
			if (dr->c.first_pio_block != base) {
				dd_dev_warn(
					dd,
					"%s: WARNING: unexpected PIO blocks used\n",
					__func__);
			}
			/* adjust top used */
			if (dr->c.first_pio_block < base + size)
				dr->c.first_pio_block = base + size;
		}
	}
	if (dr->c.first_send_context >= dr->c.last_send_context)
		return -ENOSPC;

	/*
	 * Look for receive reserved.
	 */
	found_first_unused = false;
	for (i = 0; i < dr->c.last_rcv_context; i++) {
		val = read_rctxt_csr(dd, i, JKR_RCV_SI_IDX);
		if (val == 0) { /* 0 means pf0 */
			/* this context is for the driver */
			if (!found_first_unused) {
				found_first_unused = true;
				dr->c.first_rcv_context = i;
			}
		} else {
			u32 egr_base;
			u32 egr_count;
			u32 tid_base;
			u32 tid_count;

			/* this context is non-driver */
			if (found_first_unused) {
				/*
				 * Expect an initial set of non-driver
				 * contexts, then all driver after that.
				 */
				return -EINVAL;
			}

			/* read resources for this context */
			/* RcvEgrCtrl RcvTidCtrl */
			val = read_rctxt_csr(dd, i,
					     dd->params->rcv_egr_ctrl_reg);
			egr_base =
				(val >> JKR_RCV_EGR_CTRL_EGR_BASE_INDEX_SHIFT) &
				JKR_RCV_EGR_CTRL_EGR_BASE_INDEX_MASK;
			egr_count = (val >> JKR_RCV_EGR_CTRL_EGR_CNT_SHIFT) &
				    JKR_RCV_EGR_CTRL_EGR_CNT_MASK;
			val = read_rctxt_csr(dd, i,
					     dd->params->rcv_tid_ctrl_reg);
			tid_base =
				(val >> JKR_RCV_TID_CTRL_TID_BASE_INDEX_SHIFT) &
				JKR_RCV_TID_CTRL_TID_BASE_INDEX_MASK;
			tid_count =
				(val >> JKR_RCV_TID_CTRL_TID_PAIR_CNT_SHIFT) &
				JKR_RCV_TID_CTRL_TID_PAIR_CNT_MASK;
			dd_dev_info(
				dd,
				"Non driver rcv ctxt %d: egr_base 0x%x, egr_count 0x%x, tid_base 0x%x, tid_count 0x%x\n",
				i, egr_base, egr_count, tid_base, tid_count);
			/* expect no TID resources used */
			if (tid_count != 0)
				return -EINVAL;

			/* convert from group to individual counts */
			egr_base *= RCV_INCREMENT;
			egr_count *= RCV_INCREMENT;

			/*
			 * Expect the non-driver contexts to use the entries in
			 * increasing groups.  Warn otherwise.  This is a simple
			 * attempt to warn if there may be wasted reserved
			 * blocks.  I.e. no holes.  Doing this right would
			 * involve much more complicated range lists that are
			 * not worth doing.
			 */
			if (dr->c.first_rcvarray_entry != egr_base) {
				dd_dev_warn(
					dd,
					"%s: WARNING: unexpected RcvArray entries used\n",
					__func__);
			}

			if (dr->c.first_rcvarray_entry < egr_base + egr_count)
				dr->c.first_rcvarray_entry =
					egr_base + egr_count;
		}
	}
	if (dr->c.first_rcv_context >= dr->c.last_rcv_context)
		return -ENOSPC;

	/*
	 * Look for RSM rules being used.
	 */
	for (i = 0; i < dd->params->rsm_rule_size; i++) {
		val = hfi2_read_csr(dd, JKR_RCV_RSM_CFG + (8 * i));
		if (val == 0)
			break;
	}
	if (i == dd->params->rsm_rule_size) {
		dd_dev_err(dd, "All %d RSM rules used\n",
			   dd->params->rsm_rule_size);
		return -EINVAL;
	}
	dd->first_rsm_rule = i;
	/* mark these as used */
	for (i = 0; i < dd->first_rsm_rule; i++)
		set_bit(i, dd->rsm_rule_bitmap);
	dd->rsm_rule_init = true;

out:
	dd_dev_info(
		dd,
		"Resource starts: send ctxt %d, pio block %d, rcv ctxt %d, RcvArray %d, rsm rule %d\n",
		dr->c.first_send_context, dr->c.first_pio_block,
		dr->c.first_rcv_context, dr->c.first_rcvarray_entry,
		dd->first_rsm_rule);

	return 0;
}

void hfi2_jkr_read_guid(struct hfi2_devdata *dd)
{
	/* This should get refactored into early_per_chip_init() for all */
}

int hfi2_jkr_early_per_chip_init(struct hfi2_devdata *dd)
{
	hfi2_tune_pcie_caps(dd);
	hfi2_init_early_variables(dd);

	return hfi2_sriov_assign_rsrcs(dd, &dd->rsrcs);
}

int hfi2_jkr_mid_per_chip_init(struct hfi2_devdata *dd)
{
	struct cport_who_payload *who = NULL;
	int resp_len = 0;
	int ret = 0;

	if (dd->is_vf)
		goto skip_guid; /* guid obtained earlier via hfi2_sriov_set_cfg */

	dd->base_guid = 0xabcd; /* on success, a valid value is set */
	ret = hfi2_cport_send_req(dd, CH_OP_WHO, 0, NULL, 0, (void **)&who,
				  &resp_len, hfi2_cport_adm_to * HZ);
	if (ret) {
		dd_dev_err(dd, "CPORT who failed %d\n", ret);
	} else if (resp_len == sizeof(*who)) {
		struct ib_device *ibdev = &dd->verbs_dev.rdi.ibdev;
		char v_str[IB_FW_VERSION_NAME_MAX] = {};

		dd->base_guid = who->node_guid;
		if (!hfi2_ib_hfi2_sys_image_guid)
			hfi2_ib_hfi2_sys_image_guid =
				cpu_to_be64(dd->base_guid);
		dd->cport_ver = who->vers;
		hfi2_cport_get_dev_fw_str(ibdev, v_str);
		dd_dev_info(dd, "CPORT firmware version %s\n", v_str);
	} else
		dd_dev_err(dd, "CPORT who invalid resp %d\n", resp_len);

	kfree(who);
skip_guid:
	/* additional mid-init here */
	return ret;
}

static void set_si_int_enable_range(struct hfi2_devdata *dd, u64 *csrs,
				    u32 start, u32 end)
{
	int i;
	u32 idx;
	u32 bit;

	for (i = start; i < end; ++i) {
		idx = i / 64;
		bit = i % 64;
		csrs[idx] |= (1ull << bit);
	}
}

static void write_si_int_enable(struct hfi2_devdata *dd, int si, u64 *csrs)
{
	int i;
	u32 base;

	base = JKR_CCE_SI_INT_ENABLES + JKR_C_CCE_SI_INT_ENABLES_STRIDE * si;
	for (i = 0; i < dd->params->num_int_csrs; ++i)
		hfi2_write_csr(dd, base + (i * 8), csrs[i]);
}

/* non-RXE, non-TXE, csr init */
void hfi2_jkr_init_other(struct hfi2_devdata *dd)
{
	struct hfi2_devrsrcs dr;
	int si, nsi;
	u64 csrs[LARGEST_NUM_INT_CSRS];
	u32 is_base;

	if (dd->is_vf)
		return; /* VFs can't access these CSRs */

	nsi = dd->rsrcs.num_vfs + 1; /* #VFs + PF0 */

	/* Enable interrupts for each SI according to allocated resources */
	for (si = 0; si < nsi; ++si) {
		if (si)
			hfi2_sriov_get_config(dd, &dr, si);
		else
			dr = dd->rsrcs;
		memset(csrs, 0, sizeof(csrs));
		if (!si) {
			set_si_int_enable_range(dd, csrs,
						JKR_IS_GENERAL_ERR_START,
						JKR_ASIC_ERR_INT + 1);
			set_si_int_enable_range(
				dd, csrs, JKR_MCTXT_CPORT_TO_PCIE_INT,
				JKR_MCTXT_CPORT_TO_PCIE_INT + 1);
		}
		is_base = dd->params->is_sdmaeng_err_start;
		set_si_int_enable_range(dd, csrs,
					is_base + dr.first_sdma_engine,
					is_base + dr.last_sdma_engine);
		is_base = JKR_IS_SENDCTXT_ERR_START;
		set_si_int_enable_range(dd, csrs,
					is_base + dr.c.first_send_context,
					is_base + dr.c.last_send_context);
		is_base = dd->params->is_sdma_start;
		set_si_int_enable_range(dd, csrs,
					is_base + dr.first_sdma_engine,
					is_base + dr.last_sdma_engine);
		is_base = dd->params->is_sdma_progress_start;
		set_si_int_enable_range(dd, csrs,
					is_base + dr.first_sdma_engine,
					is_base + dr.last_sdma_engine);
		is_base = dd->params->is_sdma_idle_start;
		set_si_int_enable_range(dd, csrs,
					is_base + dr.first_sdma_engine,
					is_base + dr.last_sdma_engine);
		is_base = dd->params->is_rcvavail_start;
		set_si_int_enable_range(dd, csrs,
					is_base + dr.c.first_rcv_context,
					is_base + dr.c.last_rcv_context);
		is_base = dd->params->is_rcvurgent_start;
		set_si_int_enable_range(dd, csrs,
					is_base + dr.c.first_rcv_context,
					is_base + dr.c.last_rcv_context);
		is_base = JKR_IS_SENDCREDIT_START;
		set_si_int_enable_range(dd, csrs,
					is_base + dr.c.first_send_context,
					is_base + dr.c.last_send_context);
		is_base = JKR_IS_PBC_START;
		set_si_int_enable_range(dd, csrs,
					is_base + dr.c.first_send_context,
					is_base + dr.c.last_send_context);
		is_base = JKR_IS_PIO_ERR_START;
		set_si_int_enable_range(dd, csrs, is_base + si,
					is_base + si + 1);
		is_base = JKR_IS_SDMA_ERR_SI_START;
		set_si_int_enable_range(dd, csrs, is_base + si,
					is_base + si + 1);
		is_base = JKR_IS_CSR_ERR_START;
		set_si_int_enable_range(dd, csrs, is_base + si,
					is_base + si + 1);
		hfi2_vf2pf_set_si_enables(dd, si, csrs,
					  set_si_int_enable_range);
		write_si_int_enable(dd, si, csrs);
	}
}

/* all "misc" interrupt source names */
static const char *const jkr_misc_names[] = {
	"CceErrInt", /* 0 */
	"CceSpcFreezeInt", /* 1 */
	"AsicErrInt", /* 2 */
	"cfg_vpd_int", /* 3 */
	"MctxtCportToPcieInt", /* 4 */
	"MctxtPcieToCportInt", /* 5 */
	"CportVdmRxInt", /* 6 */
	"CportVdmTxInt", /* 7 */
	"FlrInt0", /* 8 */
	"FlrInt1", /* 9 */
	"FlrInt2", /* 10 */
	"FlrInt3", /* 11 */
	"FlrInt4", /* 12 */
	"FlrInt5", /* 13 */
	"FlrInt6", /* 14 */
	"FlrInt7", /* 15 */
};

/* all "various" interrupt source names */
static const char *const jkr_various_names[] = {
	"GpioAssertInt",
	"PcoreResetInt",
	"app_ltssm_enable_int",
	"TCritInt",
};

/* generic routine for returning names from a table */
static void gen_name(char *buf, size_t bsize, unsigned int source,
		     const char *const *names, size_t nsize, const char *detail)
{
	if (source < nsize)
		strscpy(buf, names[source], bsize);
	else
		snprintf(buf, bsize, "%s%u (invalid)", detail, source);
}

static char *jkr_is_misc_name(char *buf, size_t bsize, unsigned int source)
{
	gen_name(buf, bsize, source, jkr_misc_names, ARRAY_SIZE(jkr_misc_names),
		 "MiscInt");
	return buf;
}

static char *jkr_is_various_name(char *buf, size_t bsize, unsigned int source)
{
	gen_name(buf, bsize, source, jkr_various_names,
		 ARRAY_SIZE(jkr_various_names), "VariousInt");
	return buf;
}

static char *jkr_is_port_name(char *buf, size_t bsize, unsigned int source)
{
	/* ports have 8 interrupts each */
	snprintf(buf, bsize, "Port%uInt%u", source / 8, source % 8);
	return buf;
}

static char *jkr_is_pcb_name(char *buf, size_t bsize, unsigned int source)
{
	snprintf(buf, bsize, "PbcInt%u", source);
	return buf;
}

static char *jkr_is_pio_err_name(char *buf, size_t bsize, unsigned int source)
{
	snprintf(buf, bsize, "PioErrInt%u", source);
	return buf;
}

static char *jkr_is_sdma_err_si_name(char *buf, size_t bsize,
				     unsigned int source)
{
	snprintf(buf, bsize, "SdmaErrSiInt%u", source);
	return buf;
}

static char *jkr_is_csr_err_name(char *buf, size_t bsize, unsigned int source)
{
	snprintf(buf, bsize, "CsrErrInt%u", source);
	return buf;
}

static char *jkr_is_reserved_name(char *buf, size_t bsize, unsigned int source)
{
	snprintf(buf, bsize, "Reserved%u", source + JKR_IS_RESERVED_START);
	return buf;
}

static void jkr_handle_cce_err(struct hfi2_devdata *dd, u32 unused, u64 reg)
{
	dd_dev_warn(dd, "%s: unhandled 0x%016llx\n", __func__, reg);
}

static void jkr_handle_spc_freeze(struct hfi2_devdata *dd, u32 unused, u64 reg)
{
	dd_dev_warn(dd, "%s: unhandled 0x%016llx\n", __func__, reg);
}

static void jkr_handle_asic_err(struct hfi2_devdata *dd, u32 unused, u64 reg)
{
	dd_dev_warn(dd, "%s: unhandled 0x%016llx\n", __func__, reg);
}

static void jkr_handle_csr_err(struct hfi2_devdata *dd, u32 unused, u64 reg)
{
	dd_dev_warn(dd, "%s: unhandled 0x%016llx\n", __func__, reg);
}

/* misc errs that need a clear down are also the first 3 */
static const struct err_reg_info jkr_misc_errs[] = {
	EE_N(JKR_CCE_ERR, jkr_handle_cce_err, "CceErr"),
	EE_N(JKR_CCE_SPC_FREEZE_INT, jkr_handle_spc_freeze, "CceSpcFreeze"),
	EE_N(JKR_ASIC_ERR, jkr_handle_asic_err, "AsicErr"),
};

static const struct err_reg_info jkr_sdma_eng_err =
	EE_S(JKR_SEND_DMA_ENG_ERR, hfi2_handle_sdma_eng_err, "SDmaEngErr");

static const struct err_reg_info jkr_send_pio_err =
	EE_N(JKR_SEND_PIO_ERR, hfi2_handle_pio_err, "SendPioErr");

static const struct err_reg_info jkr_send_dma_err =
	EE_N(JKR_SEND_DMA_ERR, hfi2_handle_sdma_err, "SendDmaErr");

static const struct err_reg_info jkr_csr_err =
	EE_N(JKR_CSR_ERR, jkr_handle_csr_err, "CsrErr");

static const struct err_reg_info jkr_send_egress_err =
	EE_E(JKR_SEND_EGRESS_ERR, hfi2_handle_egress_err, "SendEgressErr");

static const struct err_reg_info jkr_rcv_err =
	EE_I(JKR_RCV_ERR, hfi2_handle_rxe_err, "RcvErr");

static void jkr_is_misc_int(struct hfi2_devdata *dd, unsigned int source)
{
	char name[64];

	/* jkr_misc_errs[] has all interrupts that need a clear down */
	if (source < ARRAY_SIZE(jkr_misc_errs)) {
		hfi2_interrupt_clear_down(dd, 0, &jkr_misc_errs[source]);
		return;
	}

	if (source == JKR_MCTXT_CPORT_TO_PCIE_INT - JKR_IS_GENERAL_ERR_START) {
		hfi2_is_cport_int(dd, source);
		return;
	}

	dd_dev_err(dd, "unhandled misc interrupt %s\n",
		   jkr_is_misc_name(name, sizeof(name), source));
}

static void jkr_is_sdma_eng_err_int(struct hfi2_devdata *dd,
				    unsigned int source)
{
	hfi2_interrupt_clear_down(dd, source, &jkr_sdma_eng_err);
}

static void jkr_is_various_int(struct hfi2_devdata *dd, unsigned int source)
{
	char name[64];

	if (source == 3) { /* "TCritInt" */
		hfi2_handle_temp_err(dd);
		return;
	}

	/* not expecting any other various interrupts */
	dd_dev_err(dd, "unhandled various interrupt %s\n",
		   jkr_is_various_name(name, sizeof(name), source));
}

static void jkr_is_port_int(struct hfi2_devdata *dd, unsigned int source)
{
	char name[64];
	u32 pidx = source / 8; /* port interrupts are in groups of 8 */
	u32 which = source % 8;

	if (which == 4) { /* send egress errors */
		hfi2_interrupt_clear_down(dd, pidx, &jkr_send_egress_err);
		return;
	}
	if (which == 5) { /* receive errors */
		hfi2_interrupt_clear_down(dd, pidx, &jkr_rcv_err);
		return;
	}

	dd_dev_err(dd, "unhandled port interrupt %s\n",
		   jkr_is_port_name(name, sizeof(name), source));
}

static void jkr_is_pcb_int(struct hfi2_devdata *dd, unsigned int source)
{
	char name[64];

	/*
	 * This is a per-send context interrupt.  It is called if the PbcIntr
	 * bit is set on a context's PIO PBC and the packet has completely
	 * cleared the send buffer.
	 *
	 * Presently, the PbcIntr bit is never set.
	 */
	dd_dev_err(dd, "unhandled pcb interrupt %s\n",
		   jkr_is_pcb_name(name, sizeof(name), source));
}

static void jkr_is_pio_err_int(struct hfi2_devdata *dd, unsigned int source)
{
	/* this is a per-SI interrupt */
	hfi2_interrupt_clear_down(dd, 0, &jkr_send_pio_err);
}

static void jkr_is_sdma_err_si_int(struct hfi2_devdata *dd, unsigned int source)
{
	/* this is a per-SI interrupt */
	hfi2_interrupt_clear_down(dd, 0, &jkr_send_dma_err);
}

static void jkr_is_csr_err_int(struct hfi2_devdata *dd, unsigned int source)
{
	/* this is a per-SI interrupt */
	hfi2_interrupt_clear_down(dd, 0, &jkr_csr_err);
}

static void jkr_is_reserved_int(struct hfi2_devdata *dd, unsigned int source)
{
	char name[64];

	dd_dev_err(dd, "unhandled reserved interrupt %s\n",
		   jkr_is_reserved_name(name, sizeof(name), source));
}

const struct hfi2_is_table hfi2_jkr_is_table[] = {
	/*
	 * start			end
	 *		name func			interrupt func
	 */
	{ JKR_IS_GENERAL_ERR_START, JKR_IS_GENERAL_ERR_END, jkr_is_misc_name,
	  jkr_is_misc_int },
	{ JKR_IS_SDMAENG_ERR_START, JKR_IS_SDMAENG_ERR_END,
	  hfi2_is_sdma_eng_err_name, jkr_is_sdma_eng_err_int },
	{ JKR_IS_SENDCTXT_ERR_START, JKR_IS_SENDCTXT_ERR_END,
	  hfi2_is_sendctxt_err_name, hfi2_is_sendctxt_err_int },
	{ JKR_IS_SDMA_START, JKR_IS_SDMA_IDLE_END, hfi2_is_sdma_eng_name,
	  hfi2_is_sdma_eng_int },
	{ JKR_IS_VARIOUS_START, JKR_IS_VARIOUS_END, jkr_is_various_name,
	  jkr_is_various_int },
	{ JKR_IS_PORT_START, JKR_IS_PORT_END, jkr_is_port_name,
	  jkr_is_port_int },
	{ JKR_IS_RCVAVAIL_START, JKR_IS_RCVAVAIL_END, hfi2_is_rcv_avail_name,
	  hfi2_is_rcv_avail_int },
	{ JKR_IS_RCVURGENT_START, JKR_IS_RCVURGENT_END, hfi2_is_rcv_urgent_name,
	  hfi2_is_rcv_urgent_int },
	{ JKR_IS_SENDCREDIT_START, JKR_IS_SENDCREDIT_END,
	  hfi2_is_send_credit_name, hfi2_is_send_credit_int },
	{ JKR_IS_PBC_START, JKR_IS_PBC_END, jkr_is_pcb_name, jkr_is_pcb_int },
	{ JKR_IS_PIO_ERR_START, JKR_IS_PIO_ERR_END, jkr_is_pio_err_name,
	  jkr_is_pio_err_int },
	{ JKR_IS_SDMA_ERR_SI_START, JKR_IS_SDMA_ERR_SI_END,
	  jkr_is_sdma_err_si_name, jkr_is_sdma_err_si_int },
	{ JKR_IS_CSR_ERR_START, JKR_IS_CSR_ERR_END, jkr_is_csr_err_name,
	  jkr_is_csr_err_int },
	{ JKR_IS_RESERVED_START, JKR_IS_RESERVED_END, jkr_is_reserved_name,
	  jkr_is_reserved_int },
	{ 0, 0, 0, 0 } /* terminator */
};

/*
 * General interrupt sources to enable.  This is all sources but SDMA
 * (SdmaEngErr, Sdma, SdmaProgress, SdmaIdle), and Receive (RcvAvail,
 * RcvUrgent). MctxtCportToPcieInt is enabled separately.
 */
const struct gi_enable_entry hfi2_jkr_gi_enable_table[] = {
	{ JKR_IS_GENERAL_ERR_START, JKR_ASIC_ERR_INT },
	{ JKR_IS_SENDCTXT_ERR_START, JKR_IS_SENDCTXT_ERR_END },
	{ JKR_IS_VARIOUS_START, JKR_IS_VARIOUS_END },
	{ JKR_IS_PORT_START, JKR_IS_PORT_END },
	{ JKR_IS_SENDCREDIT_START, JKR_IS_SENDCREDIT_END },
	{ JKR_IS_PBC_START, JKR_IS_PBC_END },
	{ JKR_IS_PIO_ERR_START, JKR_IS_PIO_ERR_END },
	{ JKR_IS_SDMA_ERR_SI_START, JKR_IS_SDMA_ERR_SI_END },
	{ JKR_IS_CSR_ERR_START, JKR_IS_CSR_ERR_END },
	{ 1, 0 } /* terminator */
};

void hfi2_jkr_set_port_tid_config(struct hfi2_devdata *dd, int pidx, u16 ctxt,
				  u32 eager_base, u16 alloced,
				  u32 expected_base, u32 expected_count)
{
	u64 reg;

	if (dd->is_vf) {
		hfi2_vf2pf_tid_config(dd, pidx, ctxt, eager_base, alloced,
				      expected_base, expected_count);
		return;
	}
	/* set eager count and base index */
	reg = ((u64)(alloced >> RCV_SHIFT) << RCV_EGR_CTRL_EGR_CNT_SHIFT) |
	      ((eager_base >> RCV_SHIFT) << RCV_EGR_CTRL_EGR_BASE_INDEX_SHIFT);
	write_rctxt_csr(dd, ctxt, dd->params->rcv_egr_ctrl_reg, reg);

	/*
	 * Set TID (expected) count and base index.
	 * rcd->expected_count is set to individual RcvArray entries,
	 * not pairs, and the CSR takes a pair-count in groups of
	 * four, so divide by 8.
	 */
	reg = ((u64)(expected_count >> RCV_SHIFT)
	       << RCV_TID_CTRL_TID_PAIR_CNT_SHIFT) |
	      ((expected_base >> RCV_SHIFT)
	       << RCV_TID_CTRL_TID_BASE_INDEX_SHIFT);
	write_rctxt_csr(dd, ctxt, dd->params->rcv_tid_ctrl_reg, reg);

	/*
	 * Value must match value written into RcvTidCtrl.TidPairCnt.  See
	 * hfi2_rcvctrl() write to rcv_tid_ctrl_reg.
	 */
	reg = (u64)(expected_count >> RCV_SHIFT);
	write_iprc_csr(dd, pidx, ctxt, JKR_RCV_TID_PAIR_COUNT, reg);
	if (dd->is_sriov && pidx < dd->num_pports)
		write_iprc_csr(dd, loopback_pidx_dd(dd, pidx), ctxt,
			       JKR_RCV_TID_PAIR_COUNT, reg);
}

static inline u32 rcvarray_offset(u32 ctxt, u32 index, u32 type)
{
	return (type == PT_EAGER ? 0 :
				   BIT(JKR_RCV_ARRAY_EGR_TID_SELECT_SHIFT)) |
	       (ctxt_bar_ctxt(ctxt) << JKR_RCV_ARRAY_RCV_CTXT_IDX_SHIFT) |
	       (index << JKR_RCV_ARRAY_CSR_INDEX_SHIFT);
}

static inline u8 __iomem *rcvarray_addr(struct hfi2_devdata *dd, u32 ctxt,
					u32 index, u32 type)
{
	return dd->bar_maps[ctxt_bar_idx(ctxt)].rcvarray_wc +
	       rcvarray_offset(ctxt, index, type);
}

/*
 * Update a TID entry of a given receive context.
 *
 * @rcd	  Receive context being updated.
 * @index When type is PT_EAGER or PT_EXPECTED, index is the index into the
 *	  receive array _relative_ to how the context is set up.
 * @pa	  Physical DMA address.  If invalidating, this should be zero.
 * @order Order of map.  If invalidating, this should be zero.
 * @flush Forced flush.  Otherwise, will flush on eager or on 32-byte boundary.
 */
void hfi2_jkr_put_tid(struct hfi2_ctxtdata *rcd, u32 index, u32 type,
		      unsigned long pa, u16 order, bool flush)
{
	struct hfi2_devdata *dd = rcd->dd;
	u64 reg;
	u8 __iomem *addr;

	if (!(dd->flags & HFI2_PRESENT))
		return;

	trace_hfi2_put_tid(dd, index, type, pa, order);
	addr = rcvarray_addr(dd, rcd->ctxt, index, type);

#define RT_ADDR_SHIFT 12 /* 4KB kernel address boundary */
	/* eager and expected have the same layout */
	reg = RCV_ARRAY_RT_WRITE_ENABLE_SMASK |
	      ((u64)order << JKR_RCV_ARRAY_EGR_RT_BUF_SIZE_SHIFT) |
	      (pa >> RT_ADDR_SHIFT);
	trace_hfi2_write_rcvarray(addr, reg);
	writeq(reg, addr);

	if (type == PT_EAGER || flush || (index & 3) == 3)
		flush_wc();
}

/*
 * Write an "no-op" RcvArray entry.
 *
 * Called by the TID registration code to write to unused/unneeded RcvArray
 * entries to fill out a write-combining buffer line.  The HFI will ignore this
 * write to the RcvArray entry.
 */
void hfi2_jkr_rcv_array_wc_fill(struct hfi2_ctxtdata *rcd, u32 index, u32 type)
{
	u8 __iomem *addr = rcvarray_addr(rcd->dd, rcd->ctxt, index, type);

	writeq(0, addr);
	if ((index & 3) == 3)
		flush_wc();
}

void hfi2_jkr_ena_rcv_ctxt(struct hfi2_devdata *dd, u8 pidx, u16 ctxt,
			   bool enable)
{
	u64 bits = JKR_RCV_PKT_CTRL_RCV_PORT_ENABLE_SMASK |
		   JKR_RCV_PKT_CTRL_CONTEXT_ENABLED_SMASK;
	u64 reg;

	reg = read_iprc_csr(dd, pidx, ctxt, JKR_RCV_PKT_CTRL);
	/* always clear the L2TypeEnable field */
	reg &= ~JKR_RCV_PKT_CTRL_L2_TYPE_ENABLE_MASK_SMASK;
	if (enable) {
		/* allow 16B and 9B L2 */
		reg |= bits |
		       (0xcull << JKR_RCV_PKT_CTRL_L2_TYPE_ENABLE_MASK_SHIFT);
	} else {
		reg &= ~bits;
	}
	write_iprc_csr(dd, pidx, ctxt, JKR_RCV_PKT_CTRL, reg);
}

void hfi2_jkr_upd_rcv_hdr_size(struct hfi2_devdata *dd, u8 pidx, u16 ctxt,
			       u32 size)
{
	u64 reg;

	reg = read_iprc_csr(dd, pidx, ctxt, JKR_RCV_PKT_CTRL);
	reg &= ~JKR_RCV_PKT_CTRL_HDR_SIZE_SMASK;
	reg |= (u64)size << JKR_RCV_PKT_CTRL_HDR_SIZE_SHIFT;
	write_iprc_csr(dd, pidx, ctxt, JKR_RCV_PKT_CTRL, reg);
}

/* chip specific rcv context enable, disable */
void hfi2_jkr_enable_rcv_context(struct hfi2_pportdata *ppd, u16 ctxt,
				 u64 *kctxt_ctrl, bool enable)
{
	struct hfi2_devdata *dd = ppd->dd;

	hfi2_priv_reg_op(dd, ppd->hw_pidx, ctxt, 0, RC_ENABLE_OP, enable);

	/* adjustments to KctxtCtrl */
	if (enable)
		*kctxt_ctrl |=
			JKR_RCV_KCTXT_CTRL_RECEIVE_CUT_THROUGH_DISABLE_SMASK;
}

void hfi2_jkr_update_rcv_hdr_size(struct hfi2_pportdata *ppd, u16 ctxt,
				  u32 size)
{
	struct hfi2_devdata *dd = ppd->dd;

	hfi2_priv_reg_op(dd, ppd->hw_pidx, ctxt, 0, RC_HEADER_OP, size);
}

void hfi2_jkr_set_rheq_addr(struct hfi2_devdata *dd, u16 ctxt, u64 dma_addr)
{
	write_kctxt_csr(dd, ctxt, JKR_RCV_ERR_ADDR, dma_addr);
}

bool hfi2_jkr_check_synth_status(struct hfi2_devdata *dd)
{
	return false;
}

void hfi2_jkr_update_synth_status(struct hfi2_devdata *dd)
{
}

#define FLAG_ENTRY1(flag, str) \
	{                      \
		flag, str, 0   \
	}
const struct flag_table hfi2_jkr_egress_err_info_flags[] = {
	FLAG_ENTRY1(BIT_ULL(62), "PbcTestErr"),
	FLAG_ENTRY1(BIT_ULL(61), "RawIPv6Err"),
	FLAG_ENTRY1(BIT_ULL(60), "RawErr"),
	FLAG_ENTRY1(BIT_ULL(59), "AgeCspecErr9B"),
	FLAG_ENTRY1(BIT_ULL(58), "AgeCspecErr16B"),
	FLAG_ENTRY1(BIT_ULL(57), "GRHErr9B"),
	FLAG_ENTRY1(BIT_ULL(56), "GRHErr16B"),
	FLAG_ENTRY1(BIT_ULL(55), "SdmaMemSpaceErr9B"),
	FLAG_ENTRY1(BIT_ULL(54), "SdmaMemSpaceErr16B"),
	FLAG_ENTRY1(BIT_ULL(53), "SdmaMemSpaceErr10B"),
	FLAG_ENTRY1(BIT_ULL(52), "SdmaMemSpaceErr8B"),
	FLAG_ENTRY1(BIT_ULL(51), "DisallowedPortErr9B"),
	FLAG_ENTRY1(BIT_ULL(50), "DisallowedPortErr16B"),
	FLAG_ENTRY1(BIT_ULL(49), "DisallowedPortErr10B"),
	FLAG_ENTRY1(BIT_ULL(48), "DisallowedPortErr8B"),
	FLAG_ENTRY1(BIT_ULL(47), "BadPktLenErr9B"),
	FLAG_ENTRY1(BIT_ULL(46), "BadPktLenErr16B"),
	FLAG_ENTRY1(BIT_ULL(45), "BadPktLenErr10B"),
	FLAG_ENTRY1(BIT_ULL(44), "BadPktLenErr8B"),
	FLAG_ENTRY1(BIT_ULL(43), "NonKDETHPacketErr9B"),
	FLAG_ENTRY1(BIT_ULL(42), "NonKDETHPacketErr16B"),
	FLAG_ENTRY1(BIT_ULL(41), "NonKDETHPacketErr10B"),
	FLAG_ENTRY1(BIT_ULL(40), "NonKDETHPacketErr8B"),
	FLAG_ENTRY1(BIT_ULL(39), "KDETHPacketErr9B"),
	FLAG_ENTRY1(BIT_ULL(38), "KDETHPacketErr16B"),
	FLAG_ENTRY1(BIT_ULL(37), "KDETHPacketErr10B"),
	FLAG_ENTRY1(BIT_ULL(36), "KDETHPacketErr8B"),
	FLAG_ENTRY1(BIT_ULL(35), "TooLongPacketErr9B"),
	FLAG_ENTRY1(BIT_ULL(34), "TooLongPacketErr16B"),
	FLAG_ENTRY1(BIT_ULL(33), "TooLongPacketErr10B"),
	FLAG_ENTRY1(BIT_ULL(32), "TooLongPacketErr8B"),
	FLAG_ENTRY1(BIT_ULL(31), "TooSmallPacketErr9B"),
	FLAG_ENTRY1(BIT_ULL(30), "TooSmallPacketErr16B"),
	FLAG_ENTRY1(BIT_ULL(29), "TooSmallPacketErr10B"),
	FLAG_ENTRY1(BIT_ULL(28), "TooSmallPacketErr8B"),
	FLAG_ENTRY1(BIT_ULL(27), "VLMappingErr9B"),
	FLAG_ENTRY1(BIT_ULL(26), "VLMappingErr16B"),
	FLAG_ENTRY1(BIT_ULL(25), "VLMappingErr10B"),
	FLAG_ENTRY1(BIT_ULL(24), "VLMappingErr8B"),
	FLAG_ENTRY1(BIT_ULL(23), "OpcodeErr9B"),
	FLAG_ENTRY1(BIT_ULL(22), "OpcodeErr16B"),
	FLAG_ENTRY1(BIT_ULL(21), "OpcodeErr10B"),
	FLAG_ENTRY1(BIT_ULL(20), "OpcodeErr8B"),
	FLAG_ENTRY1(BIT_ULL(19), "SLIDErr9B"),
	FLAG_ENTRY1(BIT_ULL(18), "SLIDErr16B"),
	FLAG_ENTRY1(BIT_ULL(17), "SLIDErr10B"),
	FLAG_ENTRY1(BIT_ULL(16), "SLIDErr8B"),
	FLAG_ENTRY1(BIT_ULL(15), "PartitionKeyErr9B"),
	FLAG_ENTRY1(BIT_ULL(14), "PartitionKeyErr16B"),
	FLAG_ENTRY1(BIT_ULL(13), "PartitionKeyErr10B"),
	FLAG_ENTRY1(BIT_ULL(12), "PartitionKeyErr8B"),
	FLAG_ENTRY1(BIT_ULL(11), "JobKeyErr9B"),
	FLAG_ENTRY1(BIT_ULL(10), "JobKeyErr16B"),
	FLAG_ENTRY1(BIT_ULL(9), "JobKeyErr10B"),
	FLAG_ENTRY1(BIT_ULL(8), "JobKeyErr8B"),
	FLAG_ENTRY1(BIT_ULL(7), "VLErr9B"),
	FLAG_ENTRY1(BIT_ULL(6), "VLErr16B"),
	FLAG_ENTRY1(BIT_ULL(5), "VLErr10B"),
	FLAG_ENTRY1(BIT_ULL(4), "VLErr8B"),
	FLAG_ENTRY1(BIT_ULL(3), "L2TypeErr9B"),
	FLAG_ENTRY1(BIT_ULL(2), "L2TypeErr16B"),
	FLAG_ENTRY1(BIT_ULL(1), "L2TypeErr10B"),
	FLAG_ENTRY1(BIT_ULL(0), "L2TypeErr8B"),
};

const struct flag_data hfi2_jkr_egress_err_info_data = {
	.table = hfi2_jkr_egress_err_info_flags,
	.size = ARRAY_SIZE(hfi2_jkr_egress_err_info_flags),
};

/* send context base integrity checks */
#define SC_BASE_CHECKS                                                   \
	(0 /* 9B */                                                      \
	 | JKR_SEND_CTXT_CHECK_ENABLE_DISALLOW9BTOO_LONG_PACKET_SMASK |  \
	 JKR_SEND_CTXT_CHECK_ENABLE_DISALLOW9BTOO_SMALL_PACKETS_SMASK |  \
	 JKR_SEND_CTXT_CHECK_ENABLE_DISALLOW9BBAD_PKT_LEN_SMASK |        \
	 JKR_SEND_CTXT_CHECK_ENABLE_DISALLOW9BRAW_IPV6_SMASK |           \
	 JKR_SEND_CTXT_CHECK_ENABLE_DISALLOW9BRAW_SMASK |                \
	 JKR_SEND_CTXT_CHECK_ENABLE_DISALLOW9BPBC_TEST_SMASK |           \
	 JKR_SEND_CTXT_CHECK_ENABLE_CHECK9BVL_MAPPING_SMASK |            \
	 JKR_SEND_CTXT_CHECK_ENABLE_CHECK9BOPCODE_SMASK |                \
	 JKR_SEND_CTXT_CHECK_ENABLE_CHECK9BSLID_SMASK |                  \
	 JKR_SEND_CTXT_CHECK_ENABLE_CHECK9BJOB_KEY_SMASK |               \
	 JKR_SEND_CTXT_CHECK_ENABLE_CHECK9BVL_SMASK /* 16B */            \
	 | JKR_SEND_CTXT_CHECK_ENABLE_DISALLOW16BTOO_LONG_PACKET_SMASK | \
	 JKR_SEND_CTXT_CHECK_ENABLE_DISALLOW16BTOO_SMALL_PACKETS_SMASK | \
	 JKR_SEND_CTXT_CHECK_ENABLE_DISALLOW16BBAD_PKT_LEN_SMASK |       \
	 JKR_SEND_CTXT_CHECK_ENABLE_DISALLOW16BRAW_IPV6_SMASK |          \
	 JKR_SEND_CTXT_CHECK_ENABLE_DISALLOW16BRAW_SMASK |               \
	 JKR_SEND_CTXT_CHECK_ENABLE_DISALLOW16BPBC_TEST_SMASK |          \
	 JKR_SEND_CTXT_CHECK_ENABLE_CHECK16BVL_MAPPING_SMASK |           \
	 JKR_SEND_CTXT_CHECK_ENABLE_CHECK16BSLID_SMASK |                 \
	 JKR_SEND_CTXT_CHECK_ENABLE_CHECK16BJOB_KEY_SMASK |              \
	 JKR_SEND_CTXT_CHECK_ENABLE_CHECK16BVL_SMASK)

/* send context user integrity checks */
#define SC_USER_CHECKS                                                     \
	(0 /* 9B */                                                        \
	 | JKR_SEND_CTXT_CHECK_ENABLE_DISALLOW9BNON_KDETH_PACKETS_SMASK |  \
	 JKR_SEND_CTXT_CHECK_ENABLE_DISALLOW9BGRH_SMASK /* 16B */          \
	 | JKR_SEND_CTXT_CHECK_ENABLE_DISALLOW16BNON_KDETH_PACKETS_SMASK | \
	 JKR_SEND_CTXT_CHECK_ENABLE_DISALLOW16BGRH_SMASK)

/* send context kernel integrity checks */
#define SC_KERNEL_CHECKS                                                      \
	(0 /* 9B */                                                           \
	 | JKR_SEND_CTXT_CHECK_ENABLE_DISALLOW9BKDETH_PACKETS_SMASK /* 16B */ \
	 | JKR_SEND_CTXT_CHECK_ENABLE_DISALLOW16BKDETH_PACKETS_SMASK)

static void jkr_set_pio_integ(struct hfi2_devdata *dd, u32 pidx, u32 hw_context,
			      int type, enum spi_cmds cmd)
{
	u64 val;

	/* DEFAULT does not do a read-modify-write */
	if (cmd == SPI_DEFAULT) {
		/* allow 9B and 16B packets, no checking */
		val = JKR_SEND_CTXT_CHECK_ENABLE_L2_TYPE9BALLOWED_SMASK |
		      JKR_SEND_CTXT_CHECK_ENABLE_L2_TYPE16BALLOWED_SMASK;
	} else {
		val = read_epsc_csr(dd, pidx, hw_context,
				    dd->params->send_ctxt_check_enable_reg);
	}

	switch (cmd) {
	case SPI_DEFAULT:
		/* No integrity checks if HFI2_CAP_NO_INTEGRITY is set */
		if (HFI2_CAP_IS_KSET(NO_INTEGRITY))
			break;
		val |= SC_BASE_CHECKS;
		if (type == SC_USER)
			val |= SC_USER_CHECKS;
		else if (type != SC_KERNEL)
			val |= SC_KERNEL_CHECKS;
		break;
	case SPI_INIT:
		/* no checks to set/clear */
		break;
	case SPI_SET_JKEY:
		val |= JKR_SEND_CTXT_CHECK_ENABLE_CHECK9BJOB_KEY_SMASK |
		       JKR_SEND_CTXT_CHECK_ENABLE_CHECK16BJOB_KEY_SMASK;
		break;
	case SPI_CLEAR_JKEY:
		val &= ~(JKR_SEND_CTXT_CHECK_ENABLE_CHECK9BJOB_KEY_SMASK |
			 JKR_SEND_CTXT_CHECK_ENABLE_CHECK16BJOB_KEY_SMASK);
		break;
	case SPI_SET_PKEY:
		val |= JKR_SEND_CTXT_CHECK_ENABLE_CHECK9BPARTITION_KEY_SMASK |
		       JKR_SEND_CTXT_CHECK_ENABLE_CHECK16BPARTITION_KEY_SMASK;

		val &= ~(
			JKR_SEND_CTXT_CHECK_ENABLE_DISALLOW9BKDETH_PACKETS_SMASK |
			JKR_SEND_CTXT_CHECK_ENABLE_DISALLOW16BKDETH_PACKETS_SMASK);
		break;
	case SPI_CLEAR_PKEY:
		val &= ~(
			JKR_SEND_CTXT_CHECK_ENABLE_CHECK9BPARTITION_KEY_SMASK |
			JKR_SEND_CTXT_CHECK_ENABLE_CHECK16BPARTITION_KEY_SMASK);
		break;
	}
	write_epsc_csr(dd, pidx, hw_context,
		       dd->params->send_ctxt_check_enable_reg, val);
}

void hfi2_jkr_set_pio_integrity(struct hfi2_devdata *dd, u32 pidx,
				u32 hw_context, int type, enum spi_cmds cmd)
{
	jkr_set_pio_integ(dd, pidx, hw_context, type, cmd);
	if (dd->is_sriov)
		jkr_set_pio_integ(dd, loopback_pidx_dd(dd, pidx), hw_context,
				  type, cmd);
}

void hfi2_jkr_read_link_quality(struct hfi2_pportdata *ppd, u8 *link_quality)
{
	*link_quality = 5; /* best */
}

void hfi2_jkr_handle_link_bounce(struct work_struct *work)
{
	struct hfi2_pportdata *ppd =
		container_of(work, struct hfi2_pportdata, link_bounce_work);

	if (ppd->host_link_state & HLS_UP) {
		hfi2_set_link_state(ppd, HLS_DN_OFFLINE);
		hfi2_start_link(ppd);
	} else {
		ppd_dev_info(ppd, "%s: link not up (%s), nothing to do\n",
			     __func__,
			     hfi2_link_state_name(ppd->host_link_state));
	}
}
