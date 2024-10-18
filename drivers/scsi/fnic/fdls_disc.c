// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2008 Cisco Systems, Inc.  All rights reserved.
 * Copyright 2007 Nuova Systems, Inc.  All rights reserved.
 */

#include <linux/workqueue.h>
#include "fnic.h"
#include "fdls_fc.h"
#include "fnic_fdls.h"
#include <scsi/fc/fc_fcp.h>
#include <linux/utsname.h>

/* Frame initialization */
/*
 * Variables:
 * sid
 */
struct fc_std_flogi fnic_std_flogi_req = {
	.fchdr = {.fh_r_ctl = FC_RCTL_ELS_REQ, .fh_d_id = {0xFF, 0xFF, 0xFE},
	      .fh_type = FC_TYPE_ELS, .fh_f_ctl = {FNIC_ELS_REQ_FCTL, 0, 0},
	      .fh_rx_id = 0xFFFF},
	.els.fl_cmd = ELS_FLOGI,
	.els.fl_csp = {.sp_hi_ver = FNIC_FC_PH_VER_HI,
		   .sp_lo_ver = FNIC_FC_PH_VER_LO,
		   .sp_bb_cred = cpu_to_be16(FNIC_FC_B2B_CREDIT),
		   .sp_bb_data = cpu_to_be16(FNIC_FC_B2B_RDF_SZ)},
	.els.fl_cssp[2].cp_class = cpu_to_be16(FC_CPC_VALID | FC_CPC_SEQ)
};

/*
 * Variables:
 * sid, did(nport logins), ox_id(nport logins), nport_name, node_name
 */
struct fc_std_flogi fnic_std_plogi_req = {
	.fchdr = {.fh_r_ctl = FC_RCTL_ELS_REQ, .fh_d_id = {0xFF, 0xFF, 0xFC},
	      .fh_type = FC_TYPE_ELS, .fh_f_ctl = {FNIC_ELS_REQ_FCTL, 0, 0},
	      .fh_rx_id = 0xFFFF},
	.els = {
	    .fl_cmd = ELS_PLOGI,
	    .fl_csp = {.sp_hi_ver = FNIC_FC_PH_VER_HI,
		       .sp_lo_ver = FNIC_FC_PH_VER_LO,
		       .sp_bb_cred = cpu_to_be16(FNIC_FC_B2B_CREDIT),
		       .sp_features = cpu_to_be16(FC_SP_FT_CIRO),
		       .sp_bb_data = cpu_to_be16(FNIC_FC_B2B_RDF_SZ),
		       .sp_tot_seq = cpu_to_be16(FNIC_FC_CONCUR_SEQS),
		       .sp_rel_off = cpu_to_be16(FNIC_FC_RO_INFO),
		       .sp_e_d_tov = cpu_to_be32(FNIC_E_D_TOV)},
	    .fl_cssp[2].cp_class = cpu_to_be16(FC_CPC_VALID | FC_CPC_SEQ),
	    .fl_cssp[2].cp_rdfs = cpu_to_be16(0x800),
	    .fl_cssp[2].cp_con_seq = cpu_to_be16(0xFF),
	    .fl_cssp[2].cp_open_seq = 1}
};

/*
 * Variables:
 * sid, port_id, port_name
 */
struct fc_std_rpn_id fnic_std_rpn_id_req = {
	.fchdr = {.fh_r_ctl = FC_RCTL_DD_UNSOL_CTL,
	      .fh_d_id = {0xFF, 0xFF, 0xFC}, .fh_type = FC_TYPE_CT,
	      .fh_f_ctl = {FNIC_ELS_REQ_FCTL, 0, 0},
	      .fh_rx_id = 0xFFFF},
	.fc_std_ct_hdr = {.ct_rev = FC_CT_REV, .ct_fs_type = FC_FST_DIR,
		      .ct_fs_subtype = FC_NS_SUBTYPE,
		      .ct_cmd = cpu_to_be16(FC_NS_RPN_ID)}
};

/*
 * Variables:
 * fh_s_id, port_id, port_name
 */
struct fc_std_rft_id fnic_std_rft_id_req = {
	.fchdr = {.fh_r_ctl = FC_RCTL_DD_UNSOL_CTL,
	      .fh_d_id = {0xFF, 0xFF, 0xFC}, .fh_type = FC_TYPE_CT,
	      .fh_f_ctl = {FNIC_ELS_REQ_FCTL, 0, 0},
	      .fh_rx_id = 0xFFFF},
	.fc_std_ct_hdr = {.ct_rev = FC_CT_REV, .ct_fs_type = FC_FST_DIR,
		      .ct_fs_subtype = FC_NS_SUBTYPE,
		      .ct_cmd = cpu_to_be16(FC_NS_RFT_ID)}
};

/*
 * Variables:
 * fh_s_id, port_id, port_name
 */
struct fc_std_rff_id fnic_std_rff_id_req = {
	.fchdr = {.fh_r_ctl = FC_RCTL_DD_UNSOL_CTL,
	      .fh_d_id = {0xFF, 0xFF, 0xFC}, .fh_type = FC_TYPE_CT,
	      .fh_f_ctl = {FNIC_ELS_REQ_FCTL, 0, 0},
	      .fh_rx_id = 0xFFFF},
	.fc_std_ct_hdr = {.ct_rev = FC_CT_REV, .ct_fs_type = FC_FST_DIR,
		      .ct_fs_subtype = FC_NS_SUBTYPE,
		      .ct_cmd = cpu_to_be16(FC_NS_RFF_ID)},
	.rff_id.fr_feat = 0x2,
	.rff_id.fr_type = FC_TYPE_FCP
};

/*
 * Variables:
 * sid
 */
struct fc_std_gpn_ft fnic_std_gpn_ft_req = {
	.fchdr = {.fh_r_ctl = FC_RCTL_DD_UNSOL_CTL,
	      .fh_d_id = {0xFF, 0xFF, 0xFC}, .fh_type = FC_TYPE_CT,
	      .fh_f_ctl = {FNIC_ELS_REQ_FCTL, 0, 0},
	      .fh_rx_id = 0xFFFF},
	.fc_std_ct_hdr = {.ct_rev = FC_CT_REV, .ct_fs_type = FC_FST_DIR,
		      .ct_fs_subtype = FC_NS_SUBTYPE,
		      .ct_cmd = cpu_to_be16(FC_NS_GPN_FT)},
	.gpn_ft.fn_fc4_type = 0x08
};

/*
 * Variables:
 * sid
 */
struct fc_std_scr fnic_std_scr_req = {
	.fchdr = {.fh_r_ctl = FC_RCTL_ELS_REQ,
	      .fh_d_id = {0xFF, 0xFF, 0xFD}, .fh_type = FC_TYPE_ELS,
	      .fh_f_ctl = {FNIC_ELS_REQ_FCTL, 0, 0},
	      .fh_rx_id = 0xFFFF},
	.scr = {.scr_cmd = ELS_SCR,
	    .scr_reg_func = ELS_SCRF_FULL}
};

/*
 * Variables:
 * did, ox_id, rx_id, fcid, wwpn
 */
struct fc_std_logo fnic_std_logo_req = {
	.fchdr = {.fh_r_ctl = FC_RCTL_ELS_REQ, .fh_type = FC_TYPE_ELS,
	      .fh_f_ctl = {FNIC_ELS_REQ_FCTL, 0, 0}},
	.els.fl_cmd = ELS_LOGO,
};

struct fc_frame_header fc_std_fabric_abts = {
	.fh_r_ctl = FC_RCTL_BA_ABTS,	/* ABTS */
	.fh_d_id = {0xFF, 0xFF, 0xFF}, .fh_s_id = {0x00, 0x00, 0x00},
	.fh_cs_ctl = 0x00, .fh_type = FC_TYPE_BLS,
	.fh_f_ctl = {FNIC_REQ_ABTS_FCTL, 0, 0}, .fh_seq_id = 0x00,
	.fh_df_ctl = 0x00, .fh_seq_cnt = 0x0000, .fh_rx_id = 0xFFFF,
	.fh_parm_offset = 0x00000000,	/* bit:0 = 0 Abort a exchange */
};

#define RETRIES_EXHAUSTED(iport)      \
	(iport->fabric.retry_counter == FABRIC_LOGO_MAX_RETRY)

/*
 * For fabric requests and fdmi, once OXIDs are allocated from the pool
 * (and a range) they are encoded with expected rsp type as
 * they come in convenience with identifying the received frames
 * and debugging with switches (OXID pool base will be chosen
 * such a way bits 6-11 are always 0) oxid(16 bits) -
 *   bits 0-5: idx into the pool(bitmap)
 *   bits 6-11: expected response types
 */
#define FDLS_OXID_ENCODE(oxid, rsp_type) ((oxid) | (rsp_type << 6))
#define FDLS_OXID_RSP_TYPE_UNMASKED(oxid) ((oxid) & ~0x0FC0)
#define FDLS_OXID_TO_RSP_TYPE(oxid) (((oxid) >> 6) & 0x3F)
/* meta->size has to be power-of-2 */
#define FDLS_OXID_TO_IDX(meta, oxid) ((oxid) & (meta->sz - 1))

/* Private Functions */
static void fdls_process_flogi_rsp(struct fnic_iport_s *iport,
				   struct fc_frame_header *fchdr,
				   void *rx_frame);
static void fnic_fdls_start_plogi(struct fnic_iport_s *iport);
static void fnic_fdls_start_flogi(struct fnic_iport_s *iport);
static void fdls_start_fabric_timer(struct fnic_iport_s *iport,
			int timeout);
static void
fdls_init_fabric_oxid_pool(struct fnic_fabric_oxid_pool_s *oxid_pool,
			   int oxid_base, int sz);

void fdls_init_oxid_pool(struct fnic_iport_s *iport)
{
	fdls_init_fabric_oxid_pool(&iport->fabric_oxid_pool,
			FDLS_FABRIC_OXID_POOL_BASE,
			FDLS_FABRIC_OXID_POOL_SZ);

	fdls_init_fabric_oxid_pool(&iport->fdmi_oxid_pool,
			FDLS_FDMI_OXID_POOL_BASE,
			FDLS_FDMI_OXID_POOL_SZ);
}

uint16_t fdls_alloc_oxid(struct fnic_iport_s *iport,
			 struct fnic_oxid_pool_meta_s *meta,
			 unsigned long *bitmap)
{
	struct fnic *fnic = iport->fnic;
	struct reclaim_entry_s *reclaim_entry, *next;
	int idx;
	uint16_t oxid;
	unsigned long expiry_ts;

	/* first walk through the oxid slots and reclaim any expired */
	list_for_each_entry_safe(reclaim_entry, next,
		&(meta->reclaim_list), links) {
		expiry_ts = reclaim_entry->timestamp +
		msecs_to_jiffies(2 * iport->r_a_tov);
		if (time_after(jiffies, expiry_ts)) {
			list_del(&reclaim_entry->links);
			fdls_free_oxid(iport, meta, bitmap, reclaim_entry->oxid);
			kfree(reclaim_entry);
		}
	}

	/* Allocate next available oxid from bitmap */
	idx = find_next_zero_bit(bitmap, meta->sz, meta->next_idx);

	if (idx == meta->sz) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			"Alloc oxid: all oxid slots are busy iport state:%d\n",
			iport->state);
		return 0xFFFF;
	}

	/* adding the oxid index to pool base will give the oxid */
	oxid = idx + meta->oxid_base;
	set_bit(idx, bitmap);
	meta->next_idx = (idx + 1) % meta->sz;	/* cycle through the bitmap */

	return oxid;
}

void fdls_free_oxid(struct fnic_iport_s *iport,
		    struct fnic_oxid_pool_meta_s *meta,
		    unsigned long *bitmap, uint16_t oxid)
{
	struct fnic *fnic = iport->fnic;
	int idx;

	idx = FDLS_OXID_TO_IDX(meta, oxid);

	if (!test_bit(idx, bitmap)) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
		     "Free oxid: already freed, iport state:%d\n",
		     iport->state);
	}
	clear_bit(idx, bitmap);
}

static void
fdls_init_fabric_oxid_pool(struct fnic_fabric_oxid_pool_s *oxid_pool,
			   int oxid_base, int sz)
{
	memset(oxid_pool, 0, sizeof(struct fnic_fabric_oxid_pool_s));
	oxid_pool->meta.oxid_base = oxid_base;
	oxid_pool->meta.sz = sz;
	INIT_LIST_HEAD(&oxid_pool->meta.reclaim_list);
}

/*
 * fdls_alloc_fabric_oxid - Allocate fabric oxid from a pool
 * @iport: Handle to fnic iport
 * @oxid_pool: Pool to allocate from
 * @exp_rsp_type: Response type that we expect
 *
 * Wrapper for fabric and fdmi oxid allocations.
 * Called with fnic_lock held.
 * Encode the allocated OXID with expected rsp type for easier
 * identification of the rx response and convenient debugging with switches
 */
uint16_t fdls_alloc_fabric_oxid(struct fnic_iport_s *iport,
				struct fnic_fabric_oxid_pool_s *oxid_pool,
				int exp_rsp_type)
{
	uint16_t oxid;

	oxid = fdls_alloc_oxid(iport, &oxid_pool->meta, oxid_pool->bitmap);

	oxid = FDLS_OXID_ENCODE(oxid, exp_rsp_type);

	/* Save the corresponding active oxid for abort, if needed.
	 * Since fabric requests are serialized, they need only one.
	 * fdmi_reg_hba and fdmi_hpa can go in parallel. Save them
	 * separately.
	 */
	switch (exp_rsp_type) {
	default:
		oxid_pool->active_oxid_fabric_req = oxid;
	break;
	}

	return oxid;
}

inline void fdls_free_fabric_oxid(struct fnic_iport_s *iport,
				  struct fnic_fabric_oxid_pool_s
				  *oxid_pool, uint16_t oxid)
{
	fdls_free_oxid(iport, &oxid_pool->meta, oxid_pool->bitmap, oxid);
}

static void fdls_schedule_oxid_free(struct fnic_oxid_pool_meta_s *meta,
				    uint16_t oxid)
{
	struct reclaim_entry_s *reclaim_entry;

	reclaim_entry = (struct reclaim_entry_s *)
	kzalloc(sizeof(struct reclaim_entry_s), GFP_KERNEL);
	reclaim_entry->oxid = oxid;
	reclaim_entry->timestamp = jiffies;

	list_add_tail(&reclaim_entry->links, &meta->reclaim_list);
}

static inline void fdls_schedule_fabric_oxid_free(struct fnic_iport_s
						  *iport)
{
	fdls_schedule_oxid_free(&iport->fabric_oxid_pool.meta,
			    iport->fabric_oxid_pool.active_oxid_fabric_req);
}

int fnic_fdls_expected_rsp(struct fnic_iport_s *iport, uint16_t oxid)
{
	struct fnic *fnic = iport->fnic;

	/* Received oxid should match with one of the following */
	if ((oxid != iport->fabric_oxid_pool.active_oxid_fabric_req) &&
		(oxid != iport->fdmi_oxid_pool.active_oxid_fdmi_plogi) &&
		(oxid != iport->fdmi_oxid_pool.active_oxid_fdmi_rhba) &&
		(oxid != iport->fdmi_oxid_pool.active_oxid_fdmi_rpa)) {
		FNIC_FCS_DBG(KERN_ERR, fnic->lport->host, fnic->fnic_num,
			"Received oxid(0x%x) not matching in oxid pool (0x%x) state:%d",
			oxid, iport->fabric_oxid_pool.active_oxid_fabric_req,
			iport->fabric.state);
		return 0xFFFF;
	}

	return FDLS_OXID_TO_RSP_TYPE(oxid);
}

static int fdls_is_oxid_in_fabric_range(uint16_t oxid)
{
	uint16_t oxid_unmasked = FDLS_OXID_RSP_TYPE_UNMASKED(oxid);

	return ((oxid_unmasked >= FDLS_FABRIC_OXID_POOL_BASE) &&
			(oxid_unmasked <= FDLS_FABRIC_OXID_POOL_END));
}

inline void fnic_del_fabric_timer_sync(struct fnic *fnic)
{
	fnic->iport.fabric.del_timer_inprogress = 1;
	spin_unlock_irqrestore(&fnic->fnic_lock, fnic->lock_flags);
	del_timer_sync(&fnic->iport.fabric.retry_timer);
	spin_lock_irqsave(&fnic->fnic_lock, fnic->lock_flags);
	fnic->iport.fabric.del_timer_inprogress = 0;
}

inline void fnic_del_tport_timer_sync(struct fnic *fnic,
						struct fnic_tport_s *tport)
{
	tport->del_timer_inprogress = 1;
	spin_unlock_irqrestore(&fnic->fnic_lock, fnic->lock_flags);
	del_timer_sync(&tport->retry_timer);
	spin_lock_irqsave(&fnic->fnic_lock, fnic->lock_flags);
	tport->del_timer_inprogress = 0;
}

static void
fdls_start_fabric_timer(struct fnic_iport_s *iport, int timeout)
{
	u64 fabric_tov;
	struct fnic *fnic = iport->fnic;

	if (iport->fabric.timer_pending) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "iport fcid: 0x%x: Canceling fabric disc timer\n",
					 iport->fcid);
		fnic_del_fabric_timer_sync(fnic);
		iport->fabric.timer_pending = 0;
	}

	if (!(iport->fabric.flags & FNIC_FDLS_FABRIC_ABORT_ISSUED))
		iport->fabric.retry_counter++;

	fabric_tov = jiffies + msecs_to_jiffies(timeout);
	mod_timer(&iport->fabric.retry_timer, round_jiffies(fabric_tov));
	iport->fabric.timer_pending = 1;
	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "fabric timer is %d ", timeout);
}

static void fdls_send_fabric_abts(struct fnic_iport_s *iport)
{
	uint8_t fcid[3];
	struct fnic *fnic = iport->fnic;
	struct fc_frame_header fabric_abort = fc_std_fabric_abts;
	struct fc_frame_header *fabric_abts = &fabric_abort;
	uint16_t oxid;

	switch (iport->fabric.state) {
	case FDLS_STATE_FABRIC_LOGO:
	fabric_abts->fh_d_id[2] = 0xFE;
		break;

	case FDLS_STATE_FABRIC_FLOGI:
	fabric_abts->fh_d_id[2] = 0xFE;
		break;

	case FDLS_STATE_FABRIC_PLOGI:
		hton24(fcid, iport->fcid);
	FNIC_STD_SET_S_ID(fabric_abts, fcid);
	fabric_abts->fh_d_id[2] = 0xFC;
		break;

	case FDLS_STATE_RPN_ID:
		hton24(fcid, iport->fcid);
	FNIC_STD_SET_S_ID(fabric_abts, fcid);
	fabric_abts->fh_d_id[2] = 0xFC;
		break;

	case FDLS_STATE_SCR:
		hton24(fcid, iport->fcid);
	FNIC_STD_SET_S_ID(fabric_abts, fcid);
	fabric_abts->fh_d_id[2] = 0xFD;
		break;

	case FDLS_STATE_REGISTER_FC4_TYPES:
		hton24(fcid, iport->fcid);
	FNIC_STD_SET_S_ID(fabric_abts, fcid);
	fabric_abts->fh_d_id[2] = 0xFC;
		break;

	case FDLS_STATE_REGISTER_FC4_FEATURES:
		hton24(fcid, iport->fcid);
	FNIC_STD_SET_S_ID(fabric_abts, fcid);
	fabric_abts->fh_d_id[2] = 0xFC;
		break;

	case FDLS_STATE_GPN_FT:
		hton24(fcid, iport->fcid);
	FNIC_STD_SET_S_ID(fabric_abts, fcid);
	fabric_abts->fh_d_id[2] = 0xFC;
		break;
	default:
		return;
	}

	oxid = iport->fabric_oxid_pool.active_oxid_fabric_req;
	FNIC_STD_SET_OX_ID(fabric_abts, htons(oxid));

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
		 "FDLS sending fabric abts. iport->fabric.state: %d, oxid:%x",
		 iport->fabric.state, oxid);

	iport->fabric.flags |= FNIC_FDLS_FABRIC_ABORT_ISSUED;

	fnic_send_fcoe_frame(iport, fabric_abts,
			 sizeof(struct fc_frame_header));
	/* Even if fnic_send_fcoe_frame() fails we want to retry after timeout */
	fdls_start_fabric_timer(iport, 2 * iport->e_d_tov);
	iport->fabric.timer_pending = 1;
}

static void fdls_send_fabric_flogi(struct fnic_iport_s *iport)
{
	struct fc_std_flogi flogi;
	struct fnic *fnic = iport->fnic;
	uint16_t oxid;

	memcpy(&flogi, &fnic_std_flogi_req, sizeof(struct fc_std_flogi));
	FNIC_LOGI_SET_NPORT_NAME(&flogi.els, iport->wwpn);
	FNIC_LOGI_SET_NODE_NAME(&flogi.els, iport->wwnn);
	FNIC_LOGI_SET_RDF_SIZE(&flogi.els, iport->max_payload_size);
	FNIC_LOGI_SET_R_A_TOV(&flogi.els, iport->r_a_tov);
	FNIC_LOGI_SET_E_D_TOV(&flogi.els, iport->e_d_tov);

	oxid = fdls_alloc_fabric_oxid(iport, &iport->fabric_oxid_pool,
				  FNIC_FABRIC_FLOGI_RSP);
	if (oxid == 0xFFFF) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
		     "Failed to allocate OXID to send flogi %p", iport);
		return;
	}
	FNIC_STD_SET_OX_ID(&flogi.fchdr, htons(oxid));

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
		 "0x%x: FDLS send fabric flogi with oxid:%x", iport->fcid,
		 oxid);

	fnic_send_fcoe_frame(iport, &flogi, sizeof(struct fc_std_flogi));
	/* Even if fnic_send_fcoe_frame() fails we want to retry after timeout */
	fdls_start_fabric_timer(iport, 2 * iport->e_d_tov);
}

static void fdls_send_fabric_plogi(struct fnic_iport_s *iport)
{
	struct fc_std_flogi plogi;
	struct fc_frame_header *fchdr = &plogi.fchdr;
	uint8_t fcid[3];
	struct fnic *fnic = iport->fnic;
	uint16_t oxid;

	memcpy(&plogi, &fnic_std_plogi_req, sizeof(struct fc_std_flogi));

	hton24(fcid, iport->fcid);

	FNIC_STD_SET_S_ID(fchdr, fcid);
	FNIC_LOGI_SET_NPORT_NAME(&plogi.els, iport->wwpn);
	FNIC_LOGI_SET_NODE_NAME(&plogi.els, iport->wwnn);
	FNIC_LOGI_SET_RDF_SIZE(&plogi.els, iport->max_payload_size);

	oxid = fdls_alloc_fabric_oxid(iport, &iport->fabric_oxid_pool,
				  FNIC_FABRIC_PLOGI_RSP);
	if (oxid == 0xFFFF) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
		     "Failed to allocate OXID to send fabric plogi %p",
		     iport);
		return;
	}
	FNIC_STD_SET_OX_ID(fchdr, htons(oxid));

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
		 "0x%x: FDLS send fabric PLOGI with oxid:%x", iport->fcid,
		 oxid);

	fnic_send_fcoe_frame(iport, &plogi, sizeof(struct fc_std_flogi));
	/* Even if fnic_send_fcoe_frame() fails we want to retry after timeout */
	fdls_start_fabric_timer(iport, 2 * iport->e_d_tov);
}

static void fdls_send_rpn_id(struct fnic_iport_s *iport)
{
	struct fc_std_rpn_id rpn_id;
	uint8_t fcid[3];
	struct fnic *fnic = iport->fnic;
	uint16_t oxid;

	memcpy(&rpn_id, &fnic_std_rpn_id_req, sizeof(struct fc_std_rpn_id));

	hton24(fcid, iport->fcid);

	FNIC_STD_SET_S_ID((&rpn_id.fchdr), fcid);
	FNIC_STD_SET_PORT_ID((&rpn_id.rpn_id), fcid);
	FNIC_STD_SET_PORT_NAME((&rpn_id.rpn_id), iport->wwpn);

	oxid = fdls_alloc_fabric_oxid(iport, &iport->fabric_oxid_pool,
				  FNIC_FABRIC_RPN_RSP);
	if (oxid == 0xFFFF) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
		     "Failed to allocate OXID to send rpn id %p", iport);
		return;
	}
	FNIC_STD_SET_OX_ID(&rpn_id.fchdr, htons(oxid));

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
		 "0x%x: FDLS send RPN ID with oxid:%x", iport->fcid, oxid);

	fnic_send_fcoe_frame(iport, &rpn_id, sizeof(struct fc_std_rpn_id));
	/* Even if fnic_send_fcoe_frame() fails we want to retry after timeout */
	fdls_start_fabric_timer(iport, 2 * iport->e_d_tov);
}

static void fdls_send_scr(struct fnic_iport_s *iport)
{
	struct fc_std_scr scr;
	uint8_t fcid[3];
	struct fnic *fnic = iport->fnic;
	uint16_t oxid;

	memcpy(&scr, &fnic_std_scr_req, sizeof(struct fc_std_scr));

	hton24(fcid, iport->fcid);
	FNIC_STD_SET_S_ID((&scr.fchdr), fcid);

	oxid = fdls_alloc_fabric_oxid(iport, &iport->fabric_oxid_pool,
				  FNIC_FABRIC_SCR_RSP);
	if (oxid == 0xFFFF) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
		     "Failed to allocate OXID to send scr %p", iport);
		return;
	}
	FNIC_STD_SET_OX_ID(&scr.fchdr, htons(oxid));

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
		 "0x%x: FDLS send SCR with oxid:%x", iport->fcid, oxid);


	fnic_send_fcoe_frame(iport, &scr, sizeof(struct fc_std_scr));
	/* Even if fnic_send_fcoe_frame() fails we want to retry after timeout */
	fdls_start_fabric_timer(iport, 2 * iport->e_d_tov);
}

static void fdls_send_gpn_ft(struct fnic_iport_s *iport, int fdls_state)
{
	struct fc_std_gpn_ft gpn_ft;
	uint8_t fcid[3];
	struct fnic *fnic = iport->fnic;
	uint16_t oxid;

	memcpy(&gpn_ft, &fnic_std_gpn_ft_req, sizeof(struct fc_std_gpn_ft));

	hton24(fcid, iport->fcid);
	FNIC_STD_SET_S_ID((&gpn_ft.fchdr), fcid);

	oxid = fdls_alloc_fabric_oxid(iport, &iport->fabric_oxid_pool,
				  FNIC_FABRIC_GPN_FT_RSP);
	if (oxid == 0xFFFF) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
		     "Failed to allocate OXID to send GPN FT %p", iport);
		return;
	}
	FNIC_STD_SET_OX_ID(&gpn_ft.fchdr, htons(oxid));

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
		 "0x%x: FDLS send GPN FT with oxid:%x", iport->fcid, oxid);

	fnic_send_fcoe_frame(iport, &gpn_ft, sizeof(struct fc_std_gpn_ft));
	/* Even if fnic_send_fcoe_frame() fails we want to retry after timeout */
	fdls_start_fabric_timer(iport, 2 * iport->e_d_tov);
	fdls_set_state((&iport->fabric), fdls_state);
}

static void fdls_send_register_fc4_types(struct fnic_iport_s *iport)
{
	struct fc_std_rft_id rft_id;
	uint8_t fcid[3];
	struct fnic *fnic = iport->fnic;
	uint16_t oxid;

	memset(&rft_id, 0, sizeof(struct fc_std_rft_id));
	memcpy(&rft_id, &fnic_std_rft_id_req, sizeof(struct fc_std_rft_id));
	hton24(fcid, iport->fcid);

	FNIC_STD_SET_S_ID((&rft_id.fchdr), fcid);
	FNIC_STD_SET_PORT_ID((&rft_id.rft_id), fcid);

	oxid = fdls_alloc_fabric_oxid(iport, &iport->fabric_oxid_pool,
				  FNIC_FABRIC_RFT_RSP);
	if (oxid == 0xFFFF) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
		     "Failed to allocate OXID to send RFT %p", iport);
		return;
	}
	FNIC_STD_SET_OX_ID(&rft_id.fchdr, htons(oxid));

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
		 "0x%x: FDLS sending FC4 Typeswith oxid:%x", iport->fcid,
		 oxid);

	if (IS_FNIC_FCP_INITIATOR(fnic))
		rft_id.rft_id.fr_fts.ff_type_map[0] =
	    cpu_to_be32(1 << FC_TYPE_FCP);

	rft_id.rft_id.fr_fts.ff_type_map[1] =
	cpu_to_be32(1 << (FC_TYPE_CT % FC_NS_BPW));

	fnic_send_fcoe_frame(iport, &rft_id, sizeof(struct fc_std_rft_id));
	fdls_start_fabric_timer(iport, 2 * iport->e_d_tov);
}

static void fdls_send_register_fc4_features(struct fnic_iport_s *iport)
{
	struct fc_std_rff_id rff_id;
	uint8_t fcid[3];
	struct fnic *fnic = iport->fnic;
	uint16_t oxid;

	memcpy(&rff_id, &fnic_std_rff_id_req, sizeof(struct fc_std_rff_id));

	hton24(fcid, iport->fcid);

	FNIC_STD_SET_S_ID((&rff_id.fchdr), fcid);
	FNIC_STD_SET_PORT_ID((&rff_id.rff_id), fcid);

	oxid = fdls_alloc_fabric_oxid(iport, &iport->fabric_oxid_pool,
				  FNIC_FABRIC_RFF_RSP);
	if (oxid == 0xFFFF) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
		     "Failed to allocate OXID to send RFF %p", iport);
		return;
	}
	FNIC_STD_SET_OX_ID(&rff_id.fchdr, htons(oxid));

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
		 "0x%x: FDLS sending FC4 features with %x", iport->fcid,
		 oxid);

	if (IS_FNIC_FCP_INITIATOR(fnic)) {
		rff_id.rff_id.fr_type = FC_TYPE_FCP;
	} else {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "0x%x: Unknown type", iport->fcid);
	}

	fnic_send_fcoe_frame(iport, &rff_id, sizeof(struct fc_std_rff_id));
	fdls_start_fabric_timer(iport, 2 * iport->e_d_tov);
}

/**
 * fdls_send_fabric_logo - Send flogo to the fcf
 * @iport: Handle to fnic iport
 *
 * This function does not change or check the fabric state.
 * It the caller's responsibility to set the appropriate iport fabric
 * state when this is called. Normally it is FDLS_STATE_FABRIC_LOGO.
 * Currently this assumes to be called with fnic lock held.
 */
void fdls_send_fabric_logo(struct fnic_iport_s *iport)
{
	struct fc_std_logo logo;
	uint8_t s_id[3];
	uint8_t d_id[3] = { 0xFF, 0xFF, 0xFE };
	struct fnic *fnic = iport->fnic;
	uint16_t oxid;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Sending logo to fabric from iport->fcid: 0x%x",
				 iport->fcid);
	memcpy(&logo, &fnic_std_logo_req, sizeof(struct fc_std_logo));

	hton24(s_id, iport->fcid);

	FNIC_STD_SET_S_ID((&logo.fchdr), s_id);
	FNIC_STD_SET_D_ID((&logo.fchdr), d_id);

	oxid = fdls_alloc_fabric_oxid(iport, &iport->fabric_oxid_pool,
				  FNIC_FABRIC_LOGO_RSP);
	if (oxid == 0xFFFF) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
		     "Failed to allocate OXID to send fabric logo %p",
		     iport);
		return;
	}
	FNIC_STD_SET_OX_ID((&logo.fchdr), htons(oxid));

	memcpy(&logo.els.fl_n_port_id, s_id, 3);
	FNIC_STD_SET_NPORT_NAME(&logo.els.fl_n_port_wwn,
			    le64_to_cpu(iport->wwpn));

	fdls_start_fabric_timer(iport, 2 * iport->e_d_tov);

	iport->fabric.flags &= ~FNIC_FDLS_FABRIC_ABORT_ISSUED;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
		 "Sending logo to fabric from fcid %x with oxid %x",
		 iport->fcid, oxid);

	fnic_send_fcoe_frame(iport, &logo, sizeof(struct fc_std_logo));
}

void fdls_fabric_timer_callback(struct timer_list *t)
{
	struct fnic_fdls_fabric_s *fabric = from_timer(fabric, t, retry_timer);
	struct fnic_iport_s *iport =
		container_of(fabric, struct fnic_iport_s, fabric);
	struct fnic *fnic = iport->fnic;
	unsigned long flags;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
		 "tp: %d fab state: %d fab retry counter: %d max_flogi_retries: %d",
		 iport->fabric.timer_pending, iport->fabric.state,
		 iport->fabric.retry_counter, iport->max_flogi_retries);

	spin_lock_irqsave(&fnic->fnic_lock, flags);

	if (!iport->fabric.timer_pending) {
		spin_unlock_irqrestore(&fnic->fnic_lock, flags);
		return;
	}

	if (iport->fabric.del_timer_inprogress) {
		iport->fabric.del_timer_inprogress = 0;
		spin_unlock_irqrestore(&fnic->fnic_lock, flags);
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "fabric_del_timer inprogress(%d). Skip timer cb",
					 iport->fabric.del_timer_inprogress);
		return;
	}

	iport->fabric.timer_pending = 0;

	/* The fabric state indicates which frames have time out, and we retry */
	switch (iport->fabric.state) {
	case FDLS_STATE_FABRIC_FLOGI:
		/* Flogi received a LS_RJT with busy we retry from here */
		if ((iport->fabric.flags & FNIC_FDLS_RETRY_FRAME)
			&& (iport->fabric.retry_counter < iport->max_flogi_retries)) {
			iport->fabric.flags &= ~FNIC_FDLS_RETRY_FRAME;
			fdls_send_fabric_flogi(iport);
		} else if (!(iport->fabric.flags & FNIC_FDLS_FABRIC_ABORT_ISSUED)) {
			/* Flogi has time out 2*ed_tov send abts */
			fdls_send_fabric_abts(iport);
		} else {
			/* ABTS has timed out
			 * Mark the OXID to be freed after 2 * r_a_tov and retry the req
			 */
			fdls_schedule_fabric_oxid_free(iport);
			if (iport->fabric.retry_counter < iport->max_flogi_retries) {
				iport->fabric.flags &= ~FNIC_FDLS_FABRIC_ABORT_ISSUED;
				fdls_send_fabric_flogi(iport);
			} else
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
							 "Exceeded max FLOGI retries");
		}
		break;
	case FDLS_STATE_FABRIC_PLOGI:
		/* Plogi received a LS_RJT with busy we retry from here */
		if ((iport->fabric.flags & FNIC_FDLS_RETRY_FRAME)
			&& (iport->fabric.retry_counter < iport->max_plogi_retries)) {
			iport->fabric.flags &= ~FNIC_FDLS_RETRY_FRAME;
			fdls_send_fabric_plogi(iport);
		} else if (!(iport->fabric.flags & FNIC_FDLS_FABRIC_ABORT_ISSUED)) {
		/* Plogi has timed out 2*ed_tov send abts */
			fdls_send_fabric_abts(iport);
		} else {
			/* ABTS has timed out
			 * Mark the OXID to be freed after 2 * r_a_tov and retry the req
			 */
			fdls_schedule_fabric_oxid_free(iport);
			if (iport->fabric.retry_counter < iport->max_plogi_retries) {
				iport->fabric.flags &= ~FNIC_FDLS_FABRIC_ABORT_ISSUED;
				fdls_send_fabric_plogi(iport);
			} else
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
							 "Exceeded max PLOGI retries");
		}
		break;
	case FDLS_STATE_RPN_ID:
		/* Rpn_id received a LS_RJT with busy we retry from here */
		if ((iport->fabric.flags & FNIC_FDLS_RETRY_FRAME)
			&& (iport->fabric.retry_counter < FDLS_RETRY_COUNT)) {
			iport->fabric.flags &= ~FNIC_FDLS_RETRY_FRAME;
			fdls_send_rpn_id(iport);
		} else if (!(iport->fabric.flags & FNIC_FDLS_FABRIC_ABORT_ISSUED))
			/* RPN has timed out. Send abts */
			fdls_send_fabric_abts(iport);
		else {
			/* ABTS has timed out */
			fdls_schedule_fabric_oxid_free(iport);
			fnic_fdls_start_plogi(iport);	/* go back to fabric Plogi */
		}
		break;
	case FDLS_STATE_SCR:
		/* scr received a LS_RJT with busy we retry from here */
		if ((iport->fabric.flags & FNIC_FDLS_RETRY_FRAME)
			&& (iport->fabric.retry_counter < FDLS_RETRY_COUNT)) {
			iport->fabric.flags &= ~FNIC_FDLS_RETRY_FRAME;
			fdls_send_scr(iport);
		} else if (!(iport->fabric.flags & FNIC_FDLS_FABRIC_ABORT_ISSUED))
		    /* scr has timed out. Send abts */
			fdls_send_fabric_abts(iport);
		else {
			/* ABTS has timed out */
			fdls_schedule_fabric_oxid_free(iport);
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "ABTS timed out. Starting PLOGI: %p", iport);
			fnic_fdls_start_plogi(iport);
		}
		break;
	case FDLS_STATE_REGISTER_FC4_TYPES:
		/* scr received a LS_RJT with busy we retry from here */
		if ((iport->fabric.flags & FNIC_FDLS_RETRY_FRAME)
			&& (iport->fabric.retry_counter < FDLS_RETRY_COUNT)) {
			iport->fabric.flags &= ~FNIC_FDLS_RETRY_FRAME;
			fdls_send_register_fc4_types(iport);
		} else if (!(iport->fabric.flags & FNIC_FDLS_FABRIC_ABORT_ISSUED)) {
			/* RFT_ID timed out send abts */
			fdls_send_fabric_abts(iport);
		} else {
			/* ABTS has timed out */
			fdls_schedule_fabric_oxid_free(iport);
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "ABTS timed out. Starting PLOGI: %p", iport);
			fnic_fdls_start_plogi(iport);	/* go back to fabric Plogi */
		}
		break;
	case FDLS_STATE_REGISTER_FC4_FEATURES:
		/* scr received a LS_RJT with busy we retry from here */
		if ((iport->fabric.flags & FNIC_FDLS_RETRY_FRAME)
			&& (iport->fabric.retry_counter < FDLS_RETRY_COUNT)) {
			iport->fabric.flags &= ~FNIC_FDLS_RETRY_FRAME;
			fdls_send_register_fc4_features(iport);
		} else if (!(iport->fabric.flags & FNIC_FDLS_FABRIC_ABORT_ISSUED))
			/* SCR has timed out. Send abts */
			fdls_send_fabric_abts(iport);
		else {
			/* ABTS has timed out */
			fdls_schedule_fabric_oxid_free(iport);
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "ABTS timed out. Starting PLOGI %p", iport);
			fnic_fdls_start_plogi(iport);	/* go back to fabric Plogi */
		}
		break;
	case FDLS_STATE_RSCN_GPN_FT:
	case FDLS_STATE_SEND_GPNFT:
	case FDLS_STATE_GPN_FT:
		/* GPN_FT received a LS_RJT with busy we retry from here */
		if ((iport->fabric.flags & FNIC_FDLS_RETRY_FRAME)
			&& (iport->fabric.retry_counter < FDLS_RETRY_COUNT)) {
			iport->fabric.flags &= ~FNIC_FDLS_RETRY_FRAME;
			fdls_send_gpn_ft(iport, iport->fabric.state);
		} else if (!(iport->fabric.flags & FNIC_FDLS_FABRIC_ABORT_ISSUED)) {
			/* gpn_ft has timed out. Send abts */
			fdls_send_fabric_abts(iport);
		} else {
			/* ABTS has timed out */
			fdls_schedule_fabric_oxid_free(iport);
			if (iport->fabric.retry_counter < FDLS_RETRY_COUNT) {
				fdls_send_gpn_ft(iport, iport->fabric.state);
			} else {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "ABTS timeout for fabric GPN_FT. Check name server: %p",
					 iport);
			}
		}
		break;
	default:
		fnic_fdls_start_flogi(iport);	/* Placeholder call */
		break;
	}
	spin_unlock_irqrestore(&fnic->fnic_lock, flags);
}

static void fnic_fdls_start_flogi(struct fnic_iport_s *iport)
{
	iport->fabric.retry_counter = 0;
	fdls_send_fabric_flogi(iport);
	fdls_set_state((&iport->fabric), FDLS_STATE_FABRIC_FLOGI);
	iport->fabric.flags = 0;
}

static void fnic_fdls_start_plogi(struct fnic_iport_s *iport)
{
	iport->fabric.retry_counter = 0;
	fdls_send_fabric_plogi(iport);
	fdls_set_state((&iport->fabric), FDLS_STATE_FABRIC_PLOGI);
	iport->fabric.flags &= ~FNIC_FDLS_FABRIC_ABORT_ISSUED;
}

static void
fdls_process_rff_id_rsp(struct fnic_iport_s *iport,
			struct fc_frame_header *fchdr)
{
	struct fnic *fnic = iport->fnic;
	struct fnic_fdls_fabric_s *fdls = &iport->fabric;
	struct fc_std_rff_id *rff_rsp = (struct fc_std_rff_id *) fchdr;
	uint16_t rsp;
	uint8_t reason_code;

	if (fdls_get_state(fdls) != FDLS_STATE_REGISTER_FC4_FEATURES) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "RFF_ID resp recvd in state(%d). Dropping.",
					 fdls_get_state(fdls));
		return;
	}

	rsp = FNIC_STD_GET_FC_CT_CMD((&rff_rsp->fc_std_ct_hdr));
	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "0x%x: FDLS process RFF ID response: 0x%04x", iport->fcid,
				 (uint32_t) rsp);

	fdls_free_fabric_oxid(iport, &iport->fdmi_oxid_pool,
			  ntohs(FNIC_STD_GET_OX_ID(fchdr)));

	switch (rsp) {
	case FC_FS_ACC:
		if (iport->fabric.timer_pending) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "Canceling fabric disc timer %p\n", iport);
			fnic_del_fabric_timer_sync(fnic);
		}
		iport->fabric.timer_pending = 0;
		fdls->retry_counter = 0;
		fdls_set_state((&iport->fabric), FDLS_STATE_SCR);
		fdls_send_scr(iport);
		break;
	case FC_FS_RJT:
		reason_code = rff_rsp->fc_std_ct_hdr.ct_reason;
		if (((reason_code == FC_FS_RJT_BSY)
			|| (reason_code == FC_FS_RJT_UNABL))
			&& (fdls->retry_counter < FDLS_RETRY_COUNT)) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "RFF_ID ret ELS_LS_RJT BUSY. Retry from timer routine %p",
					 iport);

			/* Retry again from the timer routine */
			fdls->flags |= FNIC_FDLS_RETRY_FRAME;
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "RFF_ID returned ELS_LS_RJT. Halting discovery %p",
			 iport);
			if (iport->fabric.timer_pending) {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
							 "Canceling fabric disc timer %p\n", iport);
				fnic_del_fabric_timer_sync(fnic);
			}
			fdls->timer_pending = 0;
			fdls->retry_counter = 0;
		}
		break;
	default:
		break;
	}
}

static void
fdls_process_rft_id_rsp(struct fnic_iport_s *iport,
			struct fc_frame_header *fchdr)
{
	struct fnic_fdls_fabric_s *fdls = &iport->fabric;
	struct fc_std_rft_id *rft_rsp = (struct fc_std_rft_id *) fchdr;
	uint16_t rsp;
	uint8_t reason_code;
	struct fnic *fnic = iport->fnic;

	if (fdls_get_state(fdls) != FDLS_STATE_REGISTER_FC4_TYPES) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "RFT_ID resp recvd in state(%d). Dropping.",
					 fdls_get_state(fdls));
		return;
	}

	rsp = FNIC_STD_GET_FC_CT_CMD((&rft_rsp->fc_std_ct_hdr));
	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "0x%x: FDLS process RFT ID response: 0x%04x", iport->fcid,
				 (uint32_t) rsp);

	fdls_free_fabric_oxid(iport, &iport->fabric_oxid_pool,
			  ntohs(FNIC_STD_GET_OX_ID(fchdr)));

	switch (rsp) {
	case FC_FS_ACC:
		if (iport->fabric.timer_pending) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "Canceling fabric disc timer %p\n", iport);
			fnic_del_fabric_timer_sync(fnic);
		}
		iport->fabric.timer_pending = 0;
		fdls->retry_counter = 0;
		fdls_send_register_fc4_features(iport);
		fdls_set_state((&iport->fabric), FDLS_STATE_REGISTER_FC4_FEATURES);
		break;
	case FC_FS_RJT:
		reason_code = rft_rsp->fc_std_ct_hdr.ct_reason;
		if (((reason_code == FC_FS_RJT_BSY)
			|| (reason_code == FC_FS_RJT_UNABL))
			&& (fdls->retry_counter < FDLS_RETRY_COUNT)) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "0x%x: RFT_ID ret ELS_LS_RJT BUSY. Retry from timer routine",
				 iport->fcid);

			/* Retry again from the timer routine */
			fdls->flags |= FNIC_FDLS_RETRY_FRAME;
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "0x%x: RFT_ID REJ. Halting discovery reason %d expl %d",
				 iport->fcid, reason_code,
			 rft_rsp->fc_std_ct_hdr.ct_explan);
			if (iport->fabric.timer_pending) {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
							 "Canceling fabric disc timer %p\n", iport);
				fnic_del_fabric_timer_sync(fnic);
			}
			fdls->timer_pending = 0;
			fdls->retry_counter = 0;
		}
		break;
	default:
		break;
	}
}

static void
fdls_process_rpn_id_rsp(struct fnic_iport_s *iport,
			struct fc_frame_header *fchdr)
{
	struct fnic_fdls_fabric_s *fdls = &iport->fabric;
	struct fc_std_rpn_id *rpn_rsp = (struct fc_std_rpn_id *) fchdr;
	uint16_t rsp;
	uint8_t reason_code;
	struct fnic *fnic = iport->fnic;

	if (fdls_get_state(fdls) != FDLS_STATE_RPN_ID) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "RPN_ID resp recvd in state(%d). Dropping.",
					 fdls_get_state(fdls));
		return;
	}

	rsp = FNIC_STD_GET_FC_CT_CMD((&rpn_rsp->fc_std_ct_hdr));
	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "0x%x: FDLS process RPN ID response: 0x%04x", iport->fcid,
				 (uint32_t) rsp);

	fdls_free_fabric_oxid(iport, &iport->fabric_oxid_pool,
			  ntohs(FNIC_STD_GET_OX_ID(fchdr)));

	switch (rsp) {
	case FC_FS_ACC:
		if (iport->fabric.timer_pending) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "Canceling fabric disc timer %p\n", iport);
			fnic_del_fabric_timer_sync(fnic);
		}
		iport->fabric.timer_pending = 0;
		fdls->retry_counter = 0;
		fdls_send_register_fc4_types(iport);
		fdls_set_state((&iport->fabric), FDLS_STATE_REGISTER_FC4_TYPES);
		break;
	case FC_FS_RJT:
		reason_code = rpn_rsp->fc_std_ct_hdr.ct_reason;
		if (((reason_code == FC_FS_RJT_BSY)
			|| (reason_code == FC_FS_RJT_UNABL))
			&& (fdls->retry_counter < FDLS_RETRY_COUNT)) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "RPN_ID returned REJ BUSY. Retry from timer routine %p",
					 iport);

			/* Retry again from the timer routine */
			fdls->flags |= FNIC_FDLS_RETRY_FRAME;
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "RPN_ID ELS_LS_RJT. Halting discovery %p", iport);
			if (iport->fabric.timer_pending) {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
							 "Canceling fabric disc timer %p\n", iport);
				fnic_del_fabric_timer_sync(fnic);
			}
			fdls->timer_pending = 0;
			fdls->retry_counter = 0;
		}
		break;
	default:
		break;
	}
}

static void
fdls_process_scr_rsp(struct fnic_iport_s *iport,
		     struct fc_frame_header *fchdr)
{
	struct fnic_fdls_fabric_s *fdls = &iport->fabric;
	struct fc_std_scr *scr_rsp = (struct fc_std_scr *) fchdr;
	struct fc_std_els_rsp *els_rjt = (struct fc_std_els_rsp *) fchdr;
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "FDLS process SCR response: 0x%04x",
		 (uint32_t) scr_rsp->scr.scr_cmd);

	if (fdls_get_state(fdls) != FDLS_STATE_SCR) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "SCR resp recvd in state(%d). Dropping.",
					 fdls_get_state(fdls));
		return;
	}

	fdls_free_fabric_oxid(iport, &iport->fabric_oxid_pool,
			  ntohs(FNIC_STD_GET_OX_ID(fchdr)));

	switch (scr_rsp->scr.scr_cmd) {
	case ELS_LS_ACC:
		if (iport->fabric.timer_pending) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "Canceling fabric disc timer %p\n", iport);
			fnic_del_fabric_timer_sync(fnic);
		}
		iport->fabric.timer_pending = 0;
		iport->fabric.retry_counter = 0;
		fdls_send_gpn_ft(iport, FDLS_STATE_GPN_FT);
		break;

	case ELS_LS_RJT:
		if (((els_rjt->u.rej.er_reason == ELS_RJT_BUSY)
	     || (els_rjt->u.rej.er_reason == ELS_RJT_UNAB))
			&& (fdls->retry_counter < FDLS_RETRY_COUNT)) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "SCR ELS_LS_RJT BUSY. Retry from timer routine %p",
						 iport);
			/* Retry again from the timer routine */
			fdls->flags |= FNIC_FDLS_RETRY_FRAME;
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "SCR returned ELS_LS_RJT. Halting discovery %p",
						 iport);
			if (iport->fabric.timer_pending) {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
							 "Canceling fabric disc timer %p\n", iport);
				fnic_del_fabric_timer_sync(fnic);
			}
			fdls->timer_pending = 0;
			fdls->retry_counter = 0;
		}
		break;

	default:
		break;
	}
}

static void
fdls_process_gpn_ft_rsp(struct fnic_iport_s *iport,
			struct fc_frame_header *fchdr, int len)
{
	struct fnic_fdls_fabric_s *fdls = &iport->fabric;
	struct fc_std_gpn_ft *gpn_ft_rsp = (struct fc_std_gpn_ft *) fchdr;
	uint16_t rsp;
	uint8_t reason_code;
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "FDLS process GPN_FT response: iport state: %d len: %d",
				 iport->state, len);

	/*
	 * GPNFT response :-
	 *  FDLS_STATE_GPN_FT      : GPNFT send after SCR state
	 *  during fabric discovery(FNIC_IPORT_STATE_FABRIC_DISC)
	 *  FDLS_STATE_RSCN_GPN_FT : GPNFT send in response to RSCN
	 *  FDLS_STATE_SEND_GPNFT  : GPNFT send after deleting a Target,
	 *  e.g. after receiving Target LOGO
	 *  FDLS_STATE_TGT_DISCOVERY :Target discovery is currently in progress
	 *  from previous GPNFT response,a new GPNFT response has come.
	 */
	if (!(((iport->state == FNIC_IPORT_STATE_FABRIC_DISC)
		   && (fdls_get_state(fdls) == FDLS_STATE_GPN_FT))
		  || ((iport->state == FNIC_IPORT_STATE_READY)
			  && ((fdls_get_state(fdls) == FDLS_STATE_RSCN_GPN_FT)
				  || (fdls_get_state(fdls) == FDLS_STATE_SEND_GPNFT)
				  || (fdls_get_state(fdls) == FDLS_STATE_TGT_DISCOVERY))))) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "GPNFT resp recvd in fab state(%d) iport_state(%d). Dropping.",
			 fdls_get_state(fdls), iport->state);
		return;
	}

	fdls_free_fabric_oxid(iport, &iport->fabric_oxid_pool,
			  ntohs(FNIC_STD_GET_OX_ID(fchdr)));

	iport->state = FNIC_IPORT_STATE_READY;
	rsp = FNIC_STD_GET_FC_CT_CMD((&gpn_ft_rsp->fc_std_ct_hdr));

	switch (rsp) {

	case FC_FS_ACC:
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "0x%x: GPNFT_RSP accept", iport->fcid);
		break;

	case FC_FS_RJT:
		reason_code = gpn_ft_rsp->fc_std_ct_hdr.ct_reason;
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "0x%x: GPNFT_RSP Reject reason: %d", iport->fcid, reason_code);
		break;

	default:
		break;
	}
}

/**
 * fdls_process_fabric_logo_rsp - Handle an flogo response from the fcf
 * @iport: Handle to fnic iport
 * @fchdr: Incoming frame
 */
static void
fdls_process_fabric_logo_rsp(struct fnic_iport_s *iport,
			     struct fc_frame_header *fchdr)
{
	struct fc_std_flogi *flogo_rsp = (struct fc_std_flogi *) fchdr;
	struct fnic *fnic = iport->fnic;

	fdls_free_fabric_oxid(iport, &iport->fabric_oxid_pool,
			  ntohs(FNIC_STD_GET_OX_ID(fchdr)));

	switch (flogo_rsp->els.fl_cmd) {
	case ELS_LS_ACC:
		if (iport->fabric.state != FDLS_STATE_FABRIC_LOGO) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Flogo response. Fabric not in LOGO state. Dropping! %p",
				 iport);
			return;
		}

		iport->fabric.state = FDLS_STATE_FLOGO_DONE;
		iport->state = FNIC_IPORT_STATE_LINK_WAIT;

		if (iport->fabric.timer_pending) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "iport 0x%p Canceling fabric disc timer\n",
						 iport);
			fnic_del_fabric_timer_sync(fnic);
		}
		iport->fabric.timer_pending = 0;
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Flogo response from Fabric for did: 0x%x",
		     ntoh24(fchdr->fh_d_id));
		return;

	case ELS_LS_RJT:
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "Flogo response from Fabric for did: 0x%x returned ELS_LS_RJT",
		     ntoh24(fchdr->fh_d_id));
		return;

	default:
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "FLOGO response not accepted or rejected: 0x%x",
		     flogo_rsp->els.fl_cmd);
	}
}

static void
fdls_process_flogi_rsp(struct fnic_iport_s *iport,
		       struct fc_frame_header *fchdr, void *rx_frame)
{
	struct fnic_fdls_fabric_s *fabric = &iport->fabric;
	struct fc_std_flogi *flogi_rsp = (struct fc_std_flogi *) fchdr;
	uint8_t *fcid;
	int rdf_size;
	uint8_t fcmac[6] = { 0x0E, 0XFC, 0x00, 0x00, 0x00, 0x00 };
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "0x%x: FDLS processing FLOGI response", iport->fcid);

	if (fdls_get_state(fabric) != FDLS_STATE_FABRIC_FLOGI) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "FLOGI response received in state (%d). Dropping frame",
					 fdls_get_state(fabric));
		return;
	}

	fdls_free_fabric_oxid(iport, &iport->fabric_oxid_pool,
			  ntohs(FNIC_STD_GET_OX_ID(fchdr)));

	switch (flogi_rsp->els.fl_cmd) {
	case ELS_LS_ACC:
		if (iport->fabric.timer_pending) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "iport fcid: 0x%x Canceling fabric disc timer\n",
						 iport->fcid);
			fnic_del_fabric_timer_sync(fnic);
		}

		iport->fabric.timer_pending = 0;
		iport->fabric.retry_counter = 0;
		fcid = FNIC_STD_GET_D_ID(fchdr);
		iport->fcid = ntoh24(fcid);
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "0x%x: FLOGI response accepted", iport->fcid);

		/* Learn the Service Params */
		rdf_size = ntohs(FNIC_LOGI_RDF_SIZE(&flogi_rsp->els));
		if ((rdf_size >= FNIC_MIN_DATA_FIELD_SIZE)
			&& (rdf_size < FNIC_FC_MAX_PAYLOAD_LEN))
			iport->max_payload_size = MIN(rdf_size,
								  iport->max_payload_size);

		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "max_payload_size from fabric: %d set: %d", rdf_size,
					 iport->max_payload_size);

		iport->r_a_tov = ntohl(FNIC_LOGI_R_A_TOV(&flogi_rsp->els));
		iport->e_d_tov = ntohl(FNIC_LOGI_E_D_TOV(&flogi_rsp->els));

		if (FNIC_LOGI_FEATURES(&flogi_rsp->els) & FNIC_FC_EDTOV_NSEC)
			iport->e_d_tov = iport->e_d_tov / FNIC_NSEC_TO_MSEC;

		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "From fabric: R_A_TOV: %d E_D_TOV: %d",
					 iport->r_a_tov, iport->e_d_tov);

		if (IS_FNIC_FCP_INITIATOR(fnic)) {
			fc_host_fabric_name(iport->fnic->lport->host) =
			get_unaligned_be64(&FNIC_LOGI_NODE_NAME(&flogi_rsp->els));
			fc_host_port_id(iport->fnic->lport->host) = iport->fcid;
		}

		fnic_fdls_learn_fcoe_macs(iport, rx_frame, fcid);

		memcpy(&fcmac[3], fcid, 3);
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "Adding vNIC device MAC addr: %02x:%02x:%02x:%02x:%02x:%02x",
			 fcmac[0], fcmac[1], fcmac[2], fcmac[3], fcmac[4],
			 fcmac[5]);
		vnic_dev_add_addr(iport->fnic->vdev, fcmac);

		if (fdls_get_state(fabric) == FDLS_STATE_FABRIC_FLOGI) {
			fnic_fdls_start_plogi(iport);
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "FLOGI response received. Starting PLOGI");
		} else {
			/* From FDLS_STATE_FABRIC_FLOGI state fabric can only go to
			 * FDLS_STATE_LINKDOWN
			 * state, hence we don't have to worry about undoing:
			 * the fnic_fdls_register_portid and vnic_dev_add_addr
			 */
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "FLOGI response received in state (%d). Dropping frame",
				 fdls_get_state(fabric));
		}
		break;

	case ELS_LS_RJT:
		if (fabric->retry_counter < iport->max_flogi_retries) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "FLOGI returned ELS_LS_RJT BUSY. Retry from timer routine %p",
				 iport);

			/* Retry Flogi again from the timer routine. */
			fabric->flags |= FNIC_FDLS_RETRY_FRAME;

		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "FLOGI returned ELS_LS_RJT. Halting discovery %p",
			 iport);
			if (iport->fabric.timer_pending) {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
							 "iport 0x%p Canceling fabric disc timer\n",
							 iport);
				fnic_del_fabric_timer_sync(fnic);
			}
			fabric->timer_pending = 0;
			fabric->retry_counter = 0;
		}
		break;

	default:
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "FLOGI response not accepted: 0x%x",
		     flogi_rsp->els.fl_cmd);
		break;
	}
}

static void
fdls_process_fabric_plogi_rsp(struct fnic_iport_s *iport,
			      struct fc_frame_header *fchdr)
{
	struct fc_std_flogi *plogi_rsp = (struct fc_std_flogi *) fchdr;
	struct fc_std_els_rsp *els_rjt = (struct fc_std_els_rsp *) fchdr;
	struct fnic *fnic = iport->fnic;

	if (fdls_get_state((&iport->fabric)) != FDLS_STATE_FABRIC_PLOGI) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "Fabric PLOGI response received in state (%d). Dropping frame",
			 fdls_get_state(&iport->fabric));
		return;
	}

	fdls_free_fabric_oxid(iport, &iport->fabric_oxid_pool,
			  ntohs(FNIC_STD_GET_OX_ID(fchdr)));

	switch (plogi_rsp->els.fl_cmd) {
	case ELS_LS_ACC:
		if (iport->fabric.timer_pending) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "iport fcid: 0x%x fabric PLOGI response: Accepted\n",
				 iport->fcid);
			fnic_del_fabric_timer_sync(fnic);
		}
		iport->fabric.timer_pending = 0;
		iport->fabric.retry_counter = 0;
		fdls_set_state(&iport->fabric, FDLS_STATE_RPN_ID);
		fdls_send_rpn_id(iport);
		break;
	case ELS_LS_RJT:
		if (((els_rjt->u.rej.er_reason == ELS_RJT_BUSY)
	     || (els_rjt->u.rej.er_reason == ELS_RJT_UNAB))
			&& (iport->fabric.retry_counter < iport->max_plogi_retries)) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "0x%x: Fabric PLOGI ELS_LS_RJT BUSY. Retry from timer routine",
				 iport->fcid);
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "0x%x: Fabric PLOGI ELS_LS_RJT. Halting discovery",
				 iport->fcid);
			if (iport->fabric.timer_pending) {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
							 "iport fcid: 0x%x Canceling fabric disc timer\n",
							 iport->fcid);
				fnic_del_fabric_timer_sync(fnic);
			}
			iport->fabric.timer_pending = 0;
			iport->fabric.retry_counter = 0;
			return;
		}
		break;
	default:
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "PLOGI response not accepted: 0x%x",
		     plogi_rsp->els.fl_cmd);
		break;
	}
}

static void
fdls_process_fabric_abts_rsp(struct fnic_iport_s *iport,
			     struct fc_frame_header *fchdr)
{
	uint32_t s_id;
	struct fc_std_abts_ba_acc *ba_acc =
	(struct fc_std_abts_ba_acc *) fchdr;
	struct fc_std_abts_ba_rjt *ba_rjt;
	uint32_t fabric_state = iport->fabric.state;
	struct fnic *fnic = iport->fnic;
	int expected_rsp;
	uint16_t oxid;

	s_id = ntoh24(fchdr->fh_s_id);
	ba_rjt = (struct fc_std_abts_ba_rjt *) fchdr;

	if (!((s_id == FC_DIR_SERVER) || (s_id == FC_DOMAIN_CONTR)
		  || (s_id == FC_FABRIC_CONTROLLER))) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "Received abts rsp with invalid SID: 0x%x. Dropping frame",
			 s_id);
		return;
	}

	if (iport->fabric.timer_pending) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Canceling fabric disc timer %p\n", iport);
		fnic_del_fabric_timer_sync(fnic);
	}
	iport->fabric.timer_pending = 0;
	iport->fabric.flags &= ~FNIC_FDLS_FABRIC_ABORT_ISSUED;

	if (fchdr->fh_r_ctl == FNIC_BA_ACC_RCTL) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "Received abts rsp BA_ACC for fabric_state: %d OX_ID: 0x%x",
		     fabric_state, be16_to_cpu(ba_acc->acc.ba_ox_id));
	} else if (fchdr->fh_r_ctl == FNIC_BA_RJT_RCTL) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "BA_RJT fs: %d OX_ID: 0x%x rc: 0x%x rce: 0x%x",
		     fabric_state, be16_to_cpu(ba_rjt->fchdr.fh_ox_id),
		     ba_rjt->rjt.br_reason, ba_rjt->rjt.br_explan);
	}

	oxid = ntohs(FNIC_STD_GET_OX_ID(fchdr));
	expected_rsp = FDLS_OXID_TO_RSP_TYPE(oxid);
	fdls_free_fabric_oxid(iport, &iport->fabric_oxid_pool, oxid);

	/* currently error handling/retry logic is same for ABTS BA_ACC & BA_RJT */
	switch (fabric_state) {
	case FDLS_STATE_FABRIC_FLOGI:
		if (expected_rsp == FNIC_FABRIC_FLOGI_RSP) {
			if (iport->fabric.retry_counter < iport->max_flogi_retries)
				fdls_send_fabric_flogi(iport);
			else
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "Exceeded max FLOGI retries");
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Unknown abts rsp OX_ID: 0x%x FABRIC_FLOGI state. Drop frame",
			 fchdr->fh_ox_id);
		}
		break;
	case FDLS_STATE_FABRIC_LOGO:
		if (expected_rsp == FNIC_FABRIC_LOGO_RSP) {
			if (!RETRIES_EXHAUSTED(iport))
				fdls_send_fabric_logo(iport);
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Unknown abts rsp OX_ID: 0x%x FABRIC_FLOGI state. Drop frame",
			 fchdr->fh_ox_id);
		}
		break;
	case FDLS_STATE_FABRIC_PLOGI:
		if (expected_rsp == FNIC_FABRIC_PLOGI_RSP) {
			if (iport->fabric.retry_counter < iport->max_plogi_retries)
				fdls_send_fabric_plogi(iport);
			else
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Exceeded max PLOGI retries");
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Unknown abts rsp OX_ID: 0x%x FABRIC_PLOGI state. Drop frame",
			 fchdr->fh_ox_id);
		}
		break;

	case FDLS_STATE_RPN_ID:
		if (expected_rsp == FNIC_FABRIC_RPN_RSP) {
			if (iport->fabric.retry_counter < FDLS_RETRY_COUNT) {
				fdls_send_rpn_id(iport);
			} else {
				/* go back to fabric Plogi */
				fnic_fdls_start_plogi(iport);
			}
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Unknown abts rsp OX_ID: 0x%x RPN_ID state. Drop frame",
			 fchdr->fh_ox_id);
		}
		break;

	case FDLS_STATE_SCR:
		if (expected_rsp == FNIC_FABRIC_SCR_RSP) {
			if (iport->fabric.retry_counter <= FDLS_RETRY_COUNT)
				fdls_send_scr(iport);
			else {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "abts rsp fab SCR after two tries. Start fabric PLOGI %p",
					 iport);
				fnic_fdls_start_plogi(iport);	/* go back to fabric Plogi */
			}
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Unknown abts rsp OX_ID: 0x%x SCR state. Drop frame",
			 fchdr->fh_ox_id);
		}
		break;
	case FDLS_STATE_REGISTER_FC4_TYPES:
		if (expected_rsp == FNIC_FABRIC_RFT_RSP) {
			if (iport->fabric.retry_counter <= FDLS_RETRY_COUNT) {
				fdls_send_register_fc4_types(iport);
			} else {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "abts rsp fab RFT_ID two tries. Start fabric PLOGI %p",
					 iport);
				fnic_fdls_start_plogi(iport);	/* go back to fabric Plogi */
			}
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Unknown abts rsp OX_ID: 0x%x RFT state. Drop frame",
			 fchdr->fh_ox_id);
		}
		break;
	case FDLS_STATE_REGISTER_FC4_FEATURES:
		if (expected_rsp == FNIC_FABRIC_RFF_RSP) {
			if (iport->fabric.retry_counter <= FDLS_RETRY_COUNT)
				fdls_send_register_fc4_features(iport);
			else {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "abts rsp fab SCR after two tries. Start fabric PLOGI %p",
					 iport);
				fnic_fdls_start_plogi(iport);	/* go back to fabric Plogi */
			}
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Unknown abts rsp OX_ID: 0x%x RFF state. Drop frame",
			 fchdr->fh_ox_id);
		}
		break;

	case FDLS_STATE_GPN_FT:
		if (expected_rsp == FNIC_FABRIC_GPN_FT_RSP) {
			if (iport->fabric.retry_counter <= FDLS_RETRY_COUNT) {
				fdls_send_gpn_ft(iport, fabric_state);
			} else {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "abts rsp fab GPN_FT after two tries %p",
					 iport);
			}
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Unknown abts rsp OX_ID: 0x%x GPN_FT state. Drop frame",
			 fchdr->fh_ox_id);
		}
		break;

	default:
		return;
	}
}

/*
 * Performs a validation for all FCOE frames and return the frame type
 */
int
fnic_fdls_validate_and_get_frame_type(struct fnic_iport_s *iport,
									  void *rx_frame, int len,
									  int fchdr_offset)
{
	struct fc_frame_header *fchdr;
	uint8_t type;
	uint8_t *fc_payload;
	uint16_t oxid;
	uint32_t s_id;
	uint32_t d_id;
	struct fnic *fnic = iport->fnic;
	struct fnic_fdls_fabric_s *fabric = &iport->fabric;
	int rsp_type;

	fchdr =
	(struct fc_frame_header *) ((uint8_t *) rx_frame + fchdr_offset);
	oxid = FNIC_STD_GET_OX_ID(fchdr);
	fc_payload = (uint8_t *) fchdr + sizeof(struct fc_frame_header);
	type = *fc_payload;
	s_id = ntoh24(fchdr->fh_s_id);
	d_id = ntoh24(fchdr->fh_d_id);

	/* some common validation */
	if (iport->fcid)
		if (fdls_get_state(fabric) > FDLS_STATE_FABRIC_FLOGI) {
			if ((iport->fcid != d_id) || (!FNIC_FC_FRAME_CS_CTL(fchdr))) {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
							 "invalid frame received. Dropping frame");
				return -1;
			}
		}

	/*  ABTS response */
	if ((fchdr->fh_r_ctl == FNIC_BA_ACC_RCTL)
	|| (fchdr->fh_r_ctl == FNIC_BA_RJT_RCTL)) {
		if (!(FNIC_FC_FRAME_TYPE_BLS(fchdr))) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "Received ABTS invalid frame. Dropping frame");
			return -1;

		}
		return FNIC_BLS_ABTS_RSP;
	}
	if ((fchdr->fh_r_ctl == FC_ABTS_RCTL)
	&& (FNIC_FC_FRAME_TYPE_BLS(fchdr))) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Receiving Abort Request from s_id: 0x%x", s_id);
		return FNIC_BLS_ABTS_REQ;
	}

	/* unsolicited requests frames */
	if (FNIC_FC_FRAME_UNSOLICITED(fchdr)) {
		switch (type) {
		case ELS_LOGO:
			if ((!FNIC_FC_FRAME_FCTL_FIRST_LAST_SEQINIT(fchdr))
				|| (!FNIC_FC_FRAME_UNSOLICITED(fchdr))
				|| (!FNIC_FC_FRAME_TYPE_ELS(fchdr))) {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
							 "Received LOGO invalid frame. Dropping frame");
				return -1;
			}
			return FNIC_ELS_LOGO_REQ;
		case ELS_RSCN:
			if ((!FNIC_FC_FRAME_FCTL_FIRST_LAST_SEQINIT(fchdr))
				|| (!FNIC_FC_FRAME_TYPE_ELS(fchdr))
				|| (!FNIC_FC_FRAME_UNSOLICITED(fchdr))) {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "Received RSCN invalid FCTL. Dropping frame");
				return -1;
			}
			if (s_id != FC_FABRIC_CONTROLLER)
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				     "Received RSCN from target FCTL: 0x%x type: 0x%x s_id: 0x%x.",
				     fchdr->fh_f_ctl[0], fchdr->fh_type, s_id);
			return FNIC_ELS_RSCN_REQ;
		case ELS_PLOGI:
			return FNIC_ELS_PLOGI_REQ;
		case ELS_ECHO:
			return FNIC_ELS_ECHO_REQ;
		case ELS_ADISC:
			return FNIC_ELS_ADISC;
		case ELS_RLS:
			return FNIC_ELS_RLS;
		case ELS_RRQ:
			return FNIC_ELS_RRQ;
		default:
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Unsupported frame (type:0x%02x) from fcid: 0x%x",
				 type, s_id);
			return FNIC_ELS_UNSUPPORTED_REQ;
		}
	}

	/*response from fabric */
	rsp_type = fnic_fdls_expected_rsp(iport, ntohs(oxid));

	switch (rsp_type) {

	case FNIC_FABRIC_FLOGI_RSP:
		if (type == ELS_LS_ACC) {
			if ((s_id != FC_DOMAIN_CONTR)
				|| (!FNIC_FC_FRAME_TYPE_ELS(fchdr))) {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Received unknown frame. Dropping frame");
				return -1;
			}
		}
	break;

	case FNIC_FABRIC_PLOGI_RSP:
		if (type == ELS_LS_ACC) {
			if ((s_id != FC_DIR_SERVER)
				|| (!FNIC_FC_FRAME_TYPE_ELS(fchdr))) {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Received unknown frame. Dropping frame");
				return -1;
			}
		}
	break;

	case FNIC_FABRIC_SCR_RSP:
		if (type == ELS_LS_ACC) {
			if ((s_id != FC_FABRIC_CONTROLLER)
				|| (!FNIC_FC_FRAME_TYPE_ELS(fchdr))) {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Received unknown frame. Dropping frame");
				return -1;
			}
		}
	break;

	case FNIC_FABRIC_RPN_RSP:
		if ((s_id != FC_DIR_SERVER) || (!FNIC_FC_FRAME_TYPE_FC_GS(fchdr))) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Received unknown frame. Dropping frame");
			return -1;
		}
	break;

	case FNIC_FABRIC_RFT_RSP:
		if ((s_id != FC_DIR_SERVER) || (!FNIC_FC_FRAME_TYPE_FC_GS(fchdr))) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Received unknown frame. Dropping frame");
			return -1;
		}
	break;

	case FNIC_FABRIC_RFF_RSP:
		if ((s_id != FC_DIR_SERVER) || (!FNIC_FC_FRAME_TYPE_FC_GS(fchdr))) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Received unknown frame. Dropping frame");
			return -1;
		}
	break;

	case FNIC_FABRIC_GPN_FT_RSP:
		if ((s_id != FC_DIR_SERVER) || (!FNIC_FC_FRAME_TYPE_FC_GS(fchdr))) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Received unknown frame. Dropping frame");
			return -1;
		}
	break;

	case FNIC_FABRIC_LOGO_RSP:
	break;
	default:
		/* Drop the Rx frame and log/stats it */
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Solicited response: unknown OXID: 0x%x", oxid);
		return -1;
	}

	return rsp_type;
}

void fnic_fdls_recv_frame(struct fnic_iport_s *iport, void *rx_frame,
						  int len, int fchdr_offset)
{
	uint16_t oxid;
	struct fc_frame_header *fchdr;
	uint32_t s_id = 0;
	uint32_t d_id = 0;
	struct fnic *fnic = iport->fnic;
	int frame_type;

	fchdr =
	(struct fc_frame_header *) ((uint8_t *) rx_frame + fchdr_offset);
	s_id = ntoh24(fchdr->fh_s_id);
	d_id = ntoh24(fchdr->fh_d_id);

	frame_type =
		fnic_fdls_validate_and_get_frame_type(iport, rx_frame, len,
					  fchdr_offset);

	/*if we are in flogo drop everything else */
	if (iport->fabric.state == FDLS_STATE_FABRIC_LOGO &&
		frame_type != FNIC_FABRIC_LOGO_RSP)
		return;

	switch (frame_type) {
	case FNIC_FABRIC_FLOGI_RSP:
		fdls_process_flogi_rsp(iport, fchdr, rx_frame);
		break;
	case FNIC_FABRIC_PLOGI_RSP:
		fdls_process_fabric_plogi_rsp(iport, fchdr);
		break;
	case FNIC_FABRIC_RPN_RSP:
		fdls_process_rpn_id_rsp(iport, fchdr);
		break;
	case FNIC_FABRIC_RFT_RSP:
		fdls_process_rft_id_rsp(iport, fchdr);
		break;
	case FNIC_FABRIC_RFF_RSP:
		fdls_process_rff_id_rsp(iport, fchdr);
		break;
	case FNIC_FABRIC_SCR_RSP:
		fdls_process_scr_rsp(iport, fchdr);
		break;
	case FNIC_FABRIC_GPN_FT_RSP:
		fdls_process_gpn_ft_rsp(iport, fchdr, len);
		break;
	case FNIC_FABRIC_LOGO_RSP:
		fdls_process_fabric_logo_rsp(iport, fchdr);
		break;

	case FNIC_BLS_ABTS_RSP:
		oxid = ntohs(FNIC_STD_GET_OX_ID(fchdr));
		if (fdls_is_oxid_in_fabric_range(oxid) &&
			(iport->fabric.flags & FNIC_FDLS_FABRIC_ABORT_ISSUED)) {
			fdls_process_fabric_abts_rsp(iport, fchdr);
		}
		break;
	default:
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "s_id: 0x%x d_did: 0x%x", s_id, d_id);
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "Received unknown FCoE frame of len: %d. Dropping frame", len);
		break;
	}
}
