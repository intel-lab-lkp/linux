/* SPDX-License-Identifier: GPL-2.0 or BSD-3-Clause */
/*
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 *
 * SRIOV support
 */

#ifndef _VF2PF_H
#define _VF2PF_H

#include "hfi2.h"

#define VF2PF_SI_ALL	(-1)

int hfi2_vf2pf_early_init(struct hfi2_devdata *dd);
int hfi2_vf2pf_init(struct hfi2_devdata *dd);
int hfi2_vf2pf_prep(struct hfi2_devdata *dd);
void hfi2_vf2pf_deinit(struct hfi2_devdata *dd);
int hfi2_vf2pf_init_irq(struct hfi2_devdata *dd);
void hfi2_vf2pf_deinit_irq(struct hfi2_devdata *dd);
int hfi2_vf2pf_num_ctxts(struct hfi2_devdata *dd);
int hfi2_vf2pf_num_irq(struct hfi2_devdata *dd);
int hfi2_vf2pf_probe_si(struct hfi2_devdata *dd);
void hfi2_vf2pf_init_sysfs(struct hfi2_devdata *dd, struct device *class_dev);
int hfi2_vf2pf_sysfs_emit_at(struct hfi2_devdata *dd, char *buf, int at);
void hfi2_vf2pf_set_si_enables(struct hfi2_devdata *dd, int si, u64 *csrs,
			  void (*si_enables)(struct hfi2_devdata *dd,
					     u64 *csrs, u32 start, u32 end));

/*
 * Add vf2pf_* methods here, to request actions from PF0
 */
int hfi2_vf2pf_get_config(struct hfi2_devdata *dd, struct hfi2_devrsrcs *out, int si);
int hfi2_vf2pf_assign_rsrcs(struct hfi2_devdata *dd, struct hfi2_devrsrcs *vfr);
int hfi2_vf2pf_free_rsrcs(struct hfi2_devdata *dd, struct hfi2_devrsrcs *vfr);
int hfi2_vf2pf_priv_reg_op(struct hfi2_devdata *dd, int pidx, u32 ctxt, int type,
		      enum preg_op op, u64 arg);
u64 hfi2_pf0_read_csr(struct hfi2_devdata *dd, enum csr_type type, u32 off,
		 u16 ctxt, u8 pidx_eng);
u64 hfi2_pf0_rctxt_ctrl_op(struct hfi2_devdata *dd, u16 ctxt, unsigned int op);
void hfi2_vf2pf_tid_config(struct hfi2_devdata *dd, int pidx, u16 ctxt,
		      u32 eager_base, u16 alloced,
		      u32 expected_base, u32 expected_count);
int hfi2_vf2pf_init_rxe_rsm(struct hfi2_devdata *dd);
u16 hfi2_vf2pf_get_qp_map(struct hfi2_devdata *dd, int pidx, u16 idx);
int hfi2_pf2vf_push_portinfo(struct hfi2_pportdata *ppd, struct opa_smp *smp,
			struct opa_port_info *pi, int si_mask);
int hfi2_pf2vf_push_sc2vlt(struct hfi2_pportdata *ppd, int si_mask);
int hfi2_vf2pf_send_only_mad(struct hfi2_devdata *dd, u8 sb, const void *mad, int len);
int hfi2_vf2pf_send_recv_mad(struct hfi2_devdata *dd, u8 sb, const void *mad, int len,
			void *omad, size_t *omad_len, long to);
void hfi2_vf2pf_ready(struct hfi2_devdata *dd);

#endif /* _VF2PF_H */
