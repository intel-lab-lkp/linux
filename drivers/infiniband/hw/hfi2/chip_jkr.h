/* SPDX-License-Identifier: GPL-2.0 or BSD-3-Clause */
/*
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */

#ifndef _CHIP_JKR_H
#define _CHIP_JKR_H

#include "chip_registers_jkr.h"

/* items not defined in the generated register file */
#define JKR_PIO_SEND (JKR_TXE + JKR_C_TXE_PIO_SEND_OFFSET)
#define JKR_RCV_ARRAY JKR_C_RXE_RCV_ARRAY_EGR_BASE

/* RcvArray access shifts */
#define JKR_RCV_ARRAY_EGR_TID_SELECT_SHIFT 25
#define JKR_RCV_ARRAY_RCV_CTXT_IDX_SHIFT 17
#define JKR_RCV_ARRAY_CSR_INDEX_SHIFT 3

/*
 * The JKR BAR space is not split up by the RcvArray.  To maintain
 * compatibility with WFR, arbitrarily split the BAR space at some
 * page-aligned spot. Use JKR_RXE - the start of the RXE block.
 */
#define JKR_BAR0_SIZE (128 * 1024 * 1024)	/* 128 MB */
#define JKR_KREG1_SIZE JKR_RXE
#define JKR_KREG2_OFFSET JKR_RXE
#define JKR_KREG2_SIZE (JKR_PIO_SEND - JKR_RXE)

#define JKR_RCV_ARRAY_SIZE (64 * 1024 * 1024)	/* 64 MB */

/* cclock tick time, in picoseconds per tick: 1/speed * 10^12  */
#define JKR_ASIC_CCLOCK_PS 602	/* 1660.15625 MHz */

/* max number of eager entries per context */
#define JKR_MAX_EAGER_ENTRIES 16384

/* Number of PKey entries in the JKR HW */
#define JKR_MAX_PKEY_VALUES 1024
/* number of SendCtxtCtrl.CtxtBase bits in the JKR HW */
#define JKR_PIO_BASE_BITS 16

/* register strides not derived from the spec */
#define JKR_C_RXE_IPRC_STRIDE 8
#define JKR_C_TXE_TCTXT_STRIDE 8
#define JKR_C_TXE_SDMACFG_STRIDE 8
#define JKR_C_TXE_EPSC_STRIDE 8

void hfi2_jkr_init_other(struct hfi2_devdata *dd);

/* Specific IRQ sources */
#define JKR_ASIC_ERR_INT		2
#define JKR_MCTXT_CPORT_TO_PCIE_INT	4	/* part of JKR_IS_GENERAL_ERR_START/END */

/* interrupt source starts */
#define JKR_IS_GENERAL_ERR_START	   0
#define JKR_IS_SDMAENG_ERR_START	  16
#define JKR_IS_SENDCTXT_ERR_START	  32
#define JKR_IS_SDMA_START		 272
#define JKR_IS_SDMA_PROGRESS_START	 288
#define JKR_IS_SDMA_IDLE_START		 304
#define JKR_IS_VARIOUS_START		 320
#define JKR_IS_PORT_START		 324
#define JKR_IS_RCVAVAIL_START		 356
#define JKR_IS_RCVURGENT_START		 596
#define JKR_IS_SENDCREDIT_START		 836
#define JKR_IS_PBC_START		1076
#define JKR_IS_PIO_ERR_START		1316
#define JKR_IS_SDMA_ERR_SI_START	1325
#define JKR_IS_CSR_ERR_START		1334
#define JKR_IS_RESERVED_START		1343
#define JKR_IS_TABLE_NUM		1344

/* derived interrupt source values */
#define JKR_IS_GENERAL_ERR_END		(JKR_IS_SDMAENG_ERR_START - 1)
#define JKR_IS_SDMAENG_ERR_END		(JKR_IS_SENDCTXT_ERR_START - 1)
#define JKR_IS_SENDCTXT_ERR_END		(JKR_IS_SDMA_START - 1)
#define JKR_IS_SDMA_END			(JKR_IS_SDMA_PROGRESS_START - 1)
#define JKR_IS_SDMA_PROGRESS_END	(JKR_IS_SDMA_IDLE_START - 1)
#define JKR_IS_SDMA_IDLE_END		(JKR_IS_VARIOUS_START - 1)
#define JKR_IS_VARIOUS_END		(JKR_IS_PORT_START - 1)
#define JKR_IS_PORT_END			(JKR_IS_RCVAVAIL_START - 1)
#define JKR_IS_RCVAVAIL_END		(JKR_IS_RCVURGENT_START - 1)
#define JKR_IS_RCVURGENT_END		(JKR_IS_SENDCREDIT_START - 1)
#define JKR_IS_SENDCREDIT_END		(JKR_IS_PBC_START - 1)
#define JKR_IS_PBC_END			(JKR_IS_PIO_ERR_START - 1)
#define JKR_IS_PIO_ERR_END		(JKR_IS_SDMA_ERR_SI_START - 1)
#define JKR_IS_SDMA_ERR_SI_END		(JKR_IS_CSR_ERR_START - 1)
#define JKR_IS_CSR_ERR_END		(JKR_IS_RESERVED_START - 1)
#define JKR_IS_RESERVED_END		(JKR_IS_TABLE_NUM - 1)

/* last interrupt */
#define JKR_IS_LAST_SOURCE JKR_IS_RESERVED_END

extern const struct hfi2_is_table hfi2_jkr_is_table[];
extern const struct gi_enable_entry hfi2_jkr_gi_enable_table[];
extern const struct flag_data hfi2_jkr_egress_err_info_data;

/* SendEgressErrInfo bits that correspond to a PortXmitDiscard counter */
#define JKR_PORT_DISCARD_EGRESS_ERRS \
	(JKR_SEND_EGRESS_ERR_INFO_TOO_LONG_PACKET_ERR9B_SMASK \
	| JKR_SEND_EGRESS_ERR_INFO_VL_MAPPING_ERR9B_SMASK \
	| JKR_SEND_EGRESS_ERR_INFO_VL_ERR9B_SMASK)

int hfi2_jkr_find_used_resources(struct hfi2_devdata *dd);
void hfi2_jkr_read_guid(struct hfi2_devdata *dd);
int hfi2_jkr_early_per_chip_init(struct hfi2_devdata *dd);
int hfi2_jkr_mid_per_chip_init(struct hfi2_devdata *dd);
void hfi2_jkr_put_tid(struct hfi2_ctxtdata *rcd, u32 index,
		 u32 type, unsigned long pa, u16 order, bool flush);
void hfi2_jkr_rcv_array_wc_fill(struct hfi2_ctxtdata *rcd, u32 index, u32 type);
void hfi2_jkr_set_port_tid_config(struct hfi2_devdata *dd, int pidx, u16 ctxt,
			     u32 eager_base, u16 alloced,
			     u32 expected_base, u32 expected_count);
void hfi2_jkr_update_rcv_hdr_size(struct hfi2_pportdata *ppd, u16 ctxt, u32 size);
bool hfi2_jkr_check_synth_status(struct hfi2_devdata *dd);
void hfi2_jkr_update_synth_status(struct hfi2_devdata *dd);
void hfi2_jkr_set_pio_integrity(struct hfi2_devdata *dd, u32 pidx, u32 ctxt, int type,
			   enum spi_cmds cmd);
void hfi2_jkr_read_link_quality(struct hfi2_pportdata *ppd, u8 *link_quality);
void hfi2_jkr_set_rheq_addr(struct hfi2_devdata *dd, u16 ctxt, u64 dma_addr);
void hfi2_jkr_handle_link_bounce(struct work_struct *work);
void hfi2_jkr_enable_rcv_context(struct hfi2_pportdata *ppd, u16 ctxt,
			    u64 *kctxt_ctrl, bool enable);
void hfi2_jkr_ena_rcv_ctxt(struct hfi2_devdata *dd, u8 pidx, u16 ctxt, bool enable);
void hfi2_jkr_upd_rcv_hdr_size(struct hfi2_devdata *dd, u8 pidx, u16 ctxt, u32 size);

#endif /* _CHIP_JKR_H */
