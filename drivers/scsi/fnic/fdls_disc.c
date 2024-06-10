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

static void fdls_send_rpn_id(struct fnic_iport_s *iport);

/* Frame initialization */
/*
 * Variables:
 * sid
 */
struct fc_els_s fnic_flogi_req = {
	.fchdr = {.r_ctl = 0x22, .did = {0xFF,  0xFF,  0xFE},
			  .type = 0x01, .f_ctl = FNIC_ELS_REQ_FCTL,
			  .ox_id = FNIC_FLOGI_OXID, .rx_id = 0xFFFF},
	.command = FC_ELS_FLOGI_REQ,
	.u.csp_flogi = {.fc_ph_ver = FNIC_FC_PH_VER,
					.b2b_credits = FNIC_FC_B2B_CREDIT,
					.b2b_rdf_size = FNIC_FC_B2B_RDF_SZ},
	.spc3 = {0x88, 0x00}
};

/*
 * Variables:
 * sid, did(nport logins), ox_id(nport logins), nport_name, node_name
 */
struct fc_els_s fnic_plogi_req = {
	.fchdr = {.r_ctl = 0x22, .did = {0xFF, 0xFF, 0xFC}, .type = 0x01,
			  .f_ctl = FNIC_ELS_REQ_FCTL, .ox_id = FNIC_PLOGI_FABRIC_OXID,
			  .rx_id = 0xFFFF},
	.command = FC_ELS_PLOGI_REQ,
	.u.csp_plogi = {.fc_ph_ver = FNIC_FC_PH_VER,
					.b2b_credits = FNIC_FC_B2B_CREDIT, .features = 0x0080,
					.b2b_rdf_size = FNIC_FC_B2B_RDF_SZ,
					.total_concur_seqs = FNIC_FC_CONCUR_SEQS,
					.ro_info = FNIC_FC_RO_INFO, .e_d_tov = FNIC_E_D_TOV},
	.spc3 = {0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0xFF,
			 0x00, 0x00, 0x00, 0x01, 0x00, 0x00}
};

/*
 * Variables:
 * sid, port_id, port_name
 */
struct fc_rpn_id_s fnic_rpn_id_req = {
	.fchdr = {.r_ctl = 0x02, .did = {0xFF, 0xFF, 0xFC}, .type = 0x20,
			  .f_ctl = FNIC_ELS_REQ_FCTL, .ox_id = FNIC_RPN_REQ_OXID,
			  .rx_id = 0xFFFF},
	.fc_ct_hdr = {.rev = 0x01, .fs_type = 0xFC, .fs_subtype = 0x02,
				  .command = FC_CT_RPN_CMD}
};

/*
 * Variables:
 * fh_s_id, port_id, port_name
 */
struct fc_rft_id fnic_rft_id_req = {
	.fchdr = {.r_ctl = 0x02, .did = {0xFF, 0xFF, 0xFC}, .type = 0x20,
			  .f_ctl = FNIC_ELS_REQ_FCTL, .ox_id = FNIC_RFT_REQ_OXID,
			  .rx_id = 0xFFFF},
	.fc_ct_hdr = {.rev = 0x01, .fs_type = 0xFC, .fs_subtype = 0x02,
				  .command = FC_CT_RFT_CMD}
};

/*
 * Variables:
 * fh_s_id, port_id, port_name
 */
struct fc_rff_id fnic_rff_id_req = {
	.fchdr = {.r_ctl = 0x02, .did = {0xFF, 0xFF, 0xFC}, .type = 0x20,
			  .f_ctl = FNIC_ELS_REQ_FCTL, .ox_id = FNIC_RFF_REQ_OXID,
			  .rx_id = 0xFFFF},
	.fc_ct_hdr = {.rev = 0x01, .fs_type = 0xFC, .fs_subtype = 0x02,
				  .command = FC_CT_RFF_CMD},
	.tgt = 0x2,
	.fc4_type = 0x28
};

/*
 * Variables:
 * sid
 */
struct fc_gpn_ft_s fnic_gpn_ft_req = {
	.fchdr = {.r_ctl = 0x02, .did = {0xFF, 0xFF, 0xFC}, .type = 0x20,
			  .f_ctl = FNIC_ELS_REQ_FCTL, .ox_id = FNIC_GPN_FT_OXID,
			  .rx_id = 0xFFFF},
	.fc_ct_hdr = {.rev = 0x01, .fs_type = 0xFC, .fs_subtype = 0x02,
				  .command = FC_CT_GPN_FT_CMD},
	.fc4_type = 0x08
};

/*
 * Variables:
 * sid
 */
struct fc_scr_s fnic_scr_req = {
	.fchdr = {.r_ctl = 0x22, .did = {0xFF, 0xFF, 0xFD}, .type = 0x01,
			  .f_ctl = FNIC_ELS_REQ_FCTL, .ox_id = FNIC_SCR_REQ_OXID,
			  .rx_id = 0xFFFF},
	.command = FC_ELS_SCR,
	.reg_func = 0x03
};

/*
 * Variables:
 * did, ox_id, rx_id, fcid, wwpn
 */
struct fc_logo_req_s fnic_logo_req = {
	.fchdr = {.r_ctl = 0x22, .type = 0x01,
			  .f_ctl = FNIC_ELS_REQ_FCTL},
	.command = FC_ELS_LOGO,
};

#define RETRIES_EXHAUSTED(iport)      \
	(iport->fabric.retry_counter == FABRIC_LOGO_MAX_RETRY)

static void fdls_process_flogi_rsp(struct fnic_iport_s *iport,
		   struct fc_hdr_s *fchdr, void *rx_frame);
static void fnic_fdls_start_plogi(struct fnic_iport_s *iport);
static void fdls_start_fabric_timer(struct fnic_iport_s *iport,
			int timeout);

static void
fdls_start_fabric_timer(struct fnic_iport_s *iport, int timeout)
{
	u64 fabric_tov;
	struct fnic *fnic = iport->fnic;

	if (iport->fabric.timer_pending) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "iport fcid: 0x%x: Canceling fabric disc timer\n",
					 iport->fcid);
		fnic_del_fabric_timer_sync();
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
	struct fc_hdr_s fc_abts_s = {
		.r_ctl = 0x81,			/* ABTS */
		.did = {0xFF, 0xFF, 0xFF}, .sid = {0x00, 0x00, 0x00}, .cs_ctl =
			0x00, .type = 0x00, .f_ctl = FNIC_REQ_ABTS_FCTL, .seq_id =
			0x00, .df_ctl = 0x00, .seq_cnt = 0x0000, .rx_id = 0xFFFF,
		.param = 0x00000000,	/* bit:0 = 0 Abort a exchange */
	};

	struct fc_hdr_s *pfc_abts = &fc_abts_s;

	switch (iport->fabric.state) {
	case FDLS_STATE_FABRIC_LOGO:
		fc_abts_s.ox_id = FNIC_FLOGO_REQ_OXID;
		fc_abts_s.did[2] = 0xFE;
		break;
	case FDLS_STATE_FABRIC_FLOGI:
		fc_abts_s.ox_id = FNIC_FLOGI_OXID;
		fc_abts_s.did[2] = 0xFE;
		break;

	case FDLS_STATE_FABRIC_PLOGI:
		hton24(fcid, iport->fcid);
		FNIC_SET_S_ID(pfc_abts, fcid);
		fc_abts_s.ox_id = FNIC_PLOGI_FABRIC_OXID;
		fc_abts_s.did[2] = 0xFC;
		break;

	case FDLS_STATE_RPN_ID:
		hton24(fcid, iport->fcid);
		FNIC_SET_S_ID(pfc_abts, fcid);
		fc_abts_s.ox_id = FNIC_RPN_REQ_OXID;
		fc_abts_s.did[2] = 0xFC;
		break;

	case FDLS_STATE_SCR:
		hton24(fcid, iport->fcid);
		FNIC_SET_S_ID(pfc_abts, fcid);
		fc_abts_s.ox_id = FNIC_SCR_REQ_OXID;
		fc_abts_s.did[2] = 0xFD;
		break;

	case FDLS_STATE_REGISTER_FC4_TYPES:
		hton24(fcid, iport->fcid);
		FNIC_SET_S_ID(pfc_abts, fcid);
		fc_abts_s.ox_id = FNIC_RFT_REQ_OXID;
		fc_abts_s.did[2] = 0xFC;
		break;

	case FDLS_STATE_REGISTER_FC4_FEATURES:
		hton24(fcid, iport->fcid);
		FNIC_SET_S_ID(pfc_abts, fcid);
		fc_abts_s.ox_id = FNIC_RFF_REQ_OXID;
		fc_abts_s.did[2] = 0xFC;
		break;

	case FDLS_STATE_GPN_FT:
		hton24(fcid, iport->fcid);
		FNIC_SET_S_ID(pfc_abts, fcid);
		fc_abts_s.ox_id = FNIC_GPN_FT_OXID;
		fc_abts_s.did[2] = 0xFC;
		break;
	default:
		return;
	}
	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "FDLS sending fabric abts. iport->fabric.state: %d",
				 iport->fabric.state);

	iport->fabric.flags |= FNIC_FDLS_FABRIC_ABORT_ISSUED;
	fnic_send_fcoe_frame(iport, &fc_abts_s, sizeof(struct fc_hdr_s));
	/* Even if fnic_send_fcoe_frame() fails we want to retry after timeout */

	fdls_start_fabric_timer(iport, 2 * iport->r_a_tov);
	iport->fabric.timer_pending = 1;
}

static void fdls_send_fabric_flogi(struct fnic_iport_s *iport)
{
	struct fc_els_s flogi;
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "0x%x: FDLS send fabric FLOGI", iport->fcid);

	memcpy(&flogi, &fnic_flogi_req, sizeof(struct fc_els_s));
	FNIC_SET_NPORT_NAME(flogi, iport->wwpn);
	FNIC_SET_NODE_NAME(flogi, iport->wwnn);
	FNIC_SET_RDF_SIZE(flogi.u.csp_flogi, iport->max_payload_size);
	FNIC_SET_R_A_TOV(flogi.u.csp_flogi, iport->r_a_tov);
	FNIC_SET_E_D_TOV(flogi.u.csp_flogi, iport->e_d_tov);

	fnic_send_fcoe_frame(iport, &flogi, sizeof(struct fc_els_s));
	/* Even if fnic_send_fcoe_frame() fails we want to retry after timeout */
	fdls_start_fabric_timer(iport, 2 * iport->e_d_tov);
}


static void fdls_send_fabric_plogi(struct fnic_iport_s *iport)
{
	struct fc_els_s plogi;
	struct fc_hdr_s *fchdr = &plogi.fchdr;
	uint8_t fcid[3];
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "0x%x: FDLS send fabric PLOGI", iport->fcid);

	memcpy(&plogi, &fnic_plogi_req, sizeof(struct fc_els_s));

	hton24(fcid, iport->fcid);

	FNIC_SET_S_ID(fchdr, fcid);
	FNIC_SET_NPORT_NAME(plogi, iport->wwpn);
	FNIC_SET_NODE_NAME(plogi, iport->wwnn);
	FNIC_SET_RDF_SIZE(plogi.u.csp_plogi, iport->max_payload_size);

	fnic_send_fcoe_frame(iport, &plogi, sizeof(struct fc_els_s));
	/* Even if fnic_send_fcoe_frame() fails we want to retry after timeout */
	fdls_start_fabric_timer(iport, 2 * iport->e_d_tov);
}

static void fdls_send_rpn_id(struct fnic_iport_s *iport)
{
	struct fc_rpn_id_s rpn_id;
	uint8_t fcid[3];
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "0x%x: FDLS send RPN ID", iport->fcid);

	memcpy(&rpn_id, &fnic_rpn_id_req, sizeof(struct fc_rpn_id_s));

	hton24(fcid, iport->fcid);

	FNIC_SET_S_ID((&rpn_id.fchdr), fcid);
	FNIC_SET_RPN_PORT_ID((&rpn_id), fcid);
	FNIC_SET_RPN_PORT_NAME((&rpn_id), iport->wwpn);

	fnic_send_fcoe_frame(iport, &rpn_id, sizeof(struct fc_rpn_id_s));
	/* Even if fnic_send_fcoe_frame() fails we want to retry after timeout */
	fdls_start_fabric_timer(iport, 2 * iport->e_d_tov);
}

static void fdls_send_scr(struct fnic_iport_s *iport)
{
	struct fc_scr_s scr_req;
	uint8_t fcid[3];
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "0x%x: FDLS send SCR", iport->fcid);

	memcpy(&scr_req, &fnic_scr_req, sizeof(struct fc_scr_s));

	hton24(fcid, iport->fcid);
	FNIC_SET_S_ID((&scr_req.fchdr), fcid);

	fnic_send_fcoe_frame(iport, &scr_req, sizeof(struct fc_scr_s));
	/* Even if fnic_send_fcoe_frame() fails we want to retry after timeout */
	fdls_start_fabric_timer(iport, 2 * iport->e_d_tov);
}

static void fdls_send_gpn_ft(struct fnic_iport_s *iport, int fdls_state)
{
	struct fc_gpn_ft_s gpn_ft;
	uint8_t fcid[3];
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "0x%x: FDLS send GPN FT", iport->fcid);

	memcpy(&gpn_ft, &fnic_gpn_ft_req, sizeof(struct fc_gpn_ft_s));

	hton24(fcid, iport->fcid);
	FNIC_SET_S_ID((&gpn_ft.fchdr), fcid);
	fnic_send_fcoe_frame(iport, &gpn_ft, sizeof(struct fc_gpn_ft_s));
	/* Even if fnic_send_fcoe_frame() fails we want to retry after timeout */
	fdls_start_fabric_timer(iport, 2 * iport->e_d_tov);
	fdls_set_state((&iport->fabric), fdls_state);
}

static void fdls_send_register_fc4_types(struct fnic_iport_s *iport)
{
	struct fc_rft_id rft_id;
	uint8_t fcid[3];
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "0x%x: FDLS sending FC4 Types", iport->fcid);

	memset(&rft_id, 0, sizeof(struct fc_rft_id));
	memcpy(&rft_id, &fnic_rft_id_req, sizeof(struct fc_rft_id));
	hton24(fcid, iport->fcid);

	FNIC_SET_S_ID((&rft_id.fchdr), fcid);
	FNIC_SET_PORT_ID((&rft_id), fcid);
	if (IS_FNIC_FCP_INITIATOR(fnic))
		rft_id.fc4_types[2] = 1;

	rft_id.fc4_types[7] = 1;
	fnic_send_fcoe_frame(iport, &rft_id, sizeof(struct fc_rft_id));
	fdls_start_fabric_timer(iport, 2 * iport->e_d_tov);
}

static void fdls_send_register_fc4_features(struct fnic_iport_s *iport)
{
	struct fc_rff_id rff_id;
	uint8_t fcid[3];
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "0x%x: FDLS sending FC4 features", iport->fcid);
	memcpy(&rff_id, &fnic_rff_id_req, sizeof(struct fc_rff_id));

	hton24(fcid, iport->fcid);

	FNIC_SET_S_ID((&rff_id.fchdr), fcid);
	FNIC_SET_PORT_ID((&rff_id), fcid);

	if (IS_FNIC_FCP_INITIATOR(fnic)) {
		rff_id.fc4_type = 0x08;
	} else {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "0x%x: Unknown type", iport->fcid);
	}

	fnic_send_fcoe_frame(iport, &rff_id, sizeof(struct fc_rff_id));
	fdls_start_fabric_timer(iport, 2 * iport->e_d_tov);
}

/***********************************************************************
 * fdls_send_fabric_logo
 *
 * \brief Send flogo to the fcf
 *
 * \param[in]  iport   Handle to fnic iport.
 *
 * \param[in]  start_timer  1 if we want to start a perodic timer else 0
 *
 * \retval void
 *
 * \locks Currently this assumes to be called with fnic lock held
 *
 * \note This function does not change or check the fabric state.
 *       It the caller responsibility to set the appropriate iport fabric
 *       state when this is called. Normall its FDLS_STATE_FABRIC_LOGO.
 *       fdls_set_state((&iport->fabric), FDLS_STATE_FABRIC_LOGO)
 * \note Locking can be changed and made bit granuler in future
 *
 ***********************************************************************/
void fdls_send_fabric_logo(struct fnic_iport_s *iport)
{
	struct fc_logo_req_s logo;
	uint8_t s_id[3];
	uint8_t d_id[3] = { 0xFF, 0xFF, 0xFE };
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Sending logo to fabric from iport->fcid: 0x%x",
				 iport->fcid);
	memcpy(&logo, &fnic_logo_req, sizeof(struct fc_logo_req_s));

	hton24(s_id, iport->fcid);

	FNIC_SET_S_ID((&logo.fchdr), s_id);
	FNIC_SET_D_ID((&logo.fchdr), d_id);
	FNIC_SET_OX_ID((&logo.fchdr), FNIC_FLOGO_REQ_OXID);

	memcpy(&logo.fcid, s_id, 3);
	logo.wwpn = htonll(iport->wwpn);

	fdls_start_fabric_timer(iport, 2 * iport->e_d_tov);

	iport->fabric.flags &= ~FNIC_FDLS_FABRIC_ABORT_ISSUED;
	fnic_send_fcoe_frame(iport, &logo, sizeof(struct fc_logo_req_s));
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
			spin_unlock_irqrestore(&fnic->fnic_lock, flags);
			return;
		}
		/* Flogi has time out 2*ed_tov send abts */
		if (!(iport->fabric.flags & FNIC_FDLS_FABRIC_ABORT_ISSUED)) {
			fdls_send_fabric_abts(iport);
		} else {
			/* Flogi ABTS has timed out and we have waited
			 * (2 * ra_tov), we can retry safely with same
			 * exchange id
			 */
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
			spin_unlock_irqrestore(&fnic->fnic_lock, flags);
			return;
		}
		/* Plogi has timed out 2*ed_tov send abts */
		if (!(iport->fabric.flags & FNIC_FDLS_FABRIC_ABORT_ISSUED)) {
			fdls_send_fabric_abts(iport);
		} else {
			/* plogi ABTS has timed out and we have waited
			 * (2 * ra_tov) can retry safely with same
			 * exchange id
			 */
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
			spin_unlock_irqrestore(&fnic->fnic_lock, flags);
			return;
		}
		/* RPN have timed out send abts */
		if (!(iport->fabric.flags & FNIC_FDLS_FABRIC_ABORT_ISSUED))
			fdls_send_fabric_abts(iport);
		else
			/* ABTS has timed out (2*ra_tov) */
			fnic_fdls_start_plogi(iport);	/* go back to fabric Plogi */
		break;
	case FDLS_STATE_SCR:
		/* scr received a LS_RJT with busy we retry from here */
		if ((iport->fabric.flags & FNIC_FDLS_RETRY_FRAME)
			&& (iport->fabric.retry_counter < FDLS_RETRY_COUNT)) {
			iport->fabric.flags &= ~FNIC_FDLS_RETRY_FRAME;
			fdls_send_scr(iport);
			spin_unlock_irqrestore(&fnic->fnic_lock, flags);
			return;
		}
		/* scr have timed out send abts */
		if (!(iport->fabric.flags & FNIC_FDLS_FABRIC_ABORT_ISSUED))
			fdls_send_fabric_abts(iport);
		else {
			/* ABTS has timed out (2*ra_tov), we give up */
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
			spin_unlock_irqrestore(&fnic->fnic_lock, flags);
			return;
		}
		/* RFT_ID timed out send abts */
		if (!(iport->fabric.flags & FNIC_FDLS_FABRIC_ABORT_ISSUED)) {
			fdls_send_fabric_abts(iport);
		} else {
			/* ABTS has timed out (2*ra_tov), we give up */
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
			spin_unlock_irqrestore(&fnic->fnic_lock, flags);
			return;
		}
		/* scr have timed out send abts */
		if (!(iport->fabric.flags & FNIC_FDLS_FABRIC_ABORT_ISSUED))
			fdls_send_fabric_abts(iport);
		else {
			/* ABTS has timed out (2*ra_tov), we give up */
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
			spin_unlock_irqrestore(&fnic->fnic_lock, flags);
			return;
		}
		/* gpn_gt have timed out send abts */
		if (!(iport->fabric.flags & FNIC_FDLS_FABRIC_ABORT_ISSUED)) {
			fdls_send_fabric_abts(iport);
		} else {
			/*
			 * ABTS has timed out have waited (2*ra_tov) can
			 * retry safely with same exchange id
			 */
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
fdls_process_rff_id_rsp(struct fnic_iport_s *iport, struct fc_hdr_s *fchdr)
{
	struct fnic *fnic = iport->fnic;
	struct fnic_fdls_fabric_s *fdls = &iport->fabric;
	struct fc_rff_id *rff_rsp = (struct fc_rff_id *) fchdr;
	uint16_t rsp;
	uint8_t reason_code;

	if (fdls_get_state(fdls) != FDLS_STATE_REGISTER_FC4_FEATURES) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "RFF_ID resp recvd in state(%d). Dropping.",
					 fdls_get_state(fdls));
		return;
	}

	rsp = FNIC_GET_FC_CT_CMD((&rff_rsp->fc_ct_hdr));
	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "0x%x: FDLS process RFF ID response: 0x%04x", iport->fcid,
				 (uint32_t) rsp);

	switch (rsp) {
	case FC_CT_ACC:
		if (iport->fabric.timer_pending) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "Canceling fabric disc timer %p\n", iport);
			fnic_del_fabric_timer_sync();
		}
		iport->fabric.timer_pending = 0;
		fdls->retry_counter = 0;
		fdls_set_state((&iport->fabric), FDLS_STATE_SCR);
		fdls_send_scr(iport);
		break;
	case FC_CT_REJ:
		reason_code = rff_rsp->fc_ct_hdr.reason_code;
		if (((reason_code == FC_CT_RJT_LOGICAL_BUSY)
			 || (reason_code == FC_CT_RJT_BUSY))
			&& (fdls->retry_counter < FDLS_RETRY_COUNT)) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "RFF_ID ret FC_LS_REJ BUSY. Retry from timer routine %p",
					 iport);

			/* Retry again from the timer routine */
			fdls->flags |= FNIC_FDLS_RETRY_FRAME;
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "RFF_ID returned FC_LS_REJ. Halting discovery %p", iport);
			if (iport->fabric.timer_pending) {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
							 "Canceling fabric disc timer %p\n", iport);
				fnic_del_fabric_timer_sync();
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
fdls_process_rft_id_rsp(struct fnic_iport_s *iport, struct fc_hdr_s *fchdr)
{
	struct fnic_fdls_fabric_s *fdls = &iport->fabric;
	struct fc_rft_id *rft_rsp = (struct fc_rft_id *) fchdr;
	uint16_t rsp;
	uint8_t reason_code;
	struct fnic *fnic = iport->fnic;

	if (fdls_get_state(fdls) != FDLS_STATE_REGISTER_FC4_TYPES) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "RFT_ID resp recvd in state(%d). Dropping.",
					 fdls_get_state(fdls));
		return;
	}

	rsp = FNIC_GET_FC_CT_CMD((&rft_rsp->fc_ct_hdr));
	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "0x%x: FDLS process RFT ID response: 0x%04x", iport->fcid,
				 (uint32_t) rsp);

	switch (rsp) {
	case FC_CT_ACC:
		if (iport->fabric.timer_pending) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "Canceling fabric disc timer %p\n", iport);
			fnic_del_fabric_timer_sync();
		}
		iport->fabric.timer_pending = 0;
		fdls->retry_counter = 0;
		fdls_send_register_fc4_features(iport);
		fdls_set_state((&iport->fabric), FDLS_STATE_REGISTER_FC4_FEATURES);
		break;
	case FC_CT_REJ:
		reason_code = rft_rsp->fc_ct_hdr.reason_code;
		if (((reason_code == FC_CT_RJT_LOGICAL_BUSY)
			 || (reason_code == FC_CT_RJT_BUSY))
			&& (fdls->retry_counter < FDLS_RETRY_COUNT)) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "0x%x: RFT_ID ret FC_LS_REJ BUSY. Retry from timer routine",
				 iport->fcid);

			/* Retry again from the timer routine */
			fdls->flags |= FNIC_FDLS_RETRY_FRAME;
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "0x%x: RFT_ID REJ. Halting discovery reason %d expl %d",
				 iport->fcid, reason_code,
				 rft_rsp->fc_ct_hdr.reason_expl);
			if (iport->fabric.timer_pending) {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
							 "Canceling fabric disc timer %p\n", iport);
				fnic_del_fabric_timer_sync();
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
fdls_process_rpn_id_rsp(struct fnic_iport_s *iport, struct fc_hdr_s *fchdr)
{
	struct fnic_fdls_fabric_s *fdls = &iport->fabric;
	struct fc_rpn_id_s *rpn_rsp = (struct fc_rpn_id_s *) fchdr;
	uint16_t rsp;
	uint8_t reason_code;
	struct fnic *fnic = iport->fnic;

	if (fdls_get_state(fdls) != FDLS_STATE_RPN_ID) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "RPN_ID resp recvd in state(%d). Dropping.",
					 fdls_get_state(fdls));
		return;
	}

	rsp = FNIC_GET_FC_CT_CMD((&rpn_rsp->fc_ct_hdr));
	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "0x%x: FDLS process RPN ID response: 0x%04x", iport->fcid,
				 (uint32_t) rsp);

	switch (rsp) {
	case FC_CT_ACC:
		if (iport->fabric.timer_pending) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "Canceling fabric disc timer %p\n", iport);
			fnic_del_fabric_timer_sync();
		}
		iport->fabric.timer_pending = 0;
		fdls->retry_counter = 0;
		fdls_send_register_fc4_types(iport);
		fdls_set_state((&iport->fabric), FDLS_STATE_REGISTER_FC4_TYPES);
		break;
	case FC_CT_REJ:
		reason_code = rpn_rsp->fc_ct_hdr.reason_code;
		if (((reason_code == FC_CT_RJT_LOGICAL_BUSY)
			 || (reason_code == FC_CT_RJT_BUSY))
			&& (fdls->retry_counter < FDLS_RETRY_COUNT)) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "RPN_ID returned REJ BUSY. Retry from timer routine %p",
					 iport);

			/* Retry again from the timer routine */
			fdls->flags |= FNIC_FDLS_RETRY_FRAME;
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "RPN_ID FC_LS_REJ. Halting discovery %p", iport);
			if (iport->fabric.timer_pending) {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
							 "Canceling fabric disc timer %p\n", iport);
				fnic_del_fabric_timer_sync();
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
fdls_process_scr_rsp(struct fnic_iport_s *iport, struct fc_hdr_s *fchdr)
{
	struct fnic_fdls_fabric_s *fdls = &iport->fabric;
	struct fc_scr_s *scr_rsp = (struct fc_scr_s *) fchdr;
	struct fc_els_reject_s *els_rjt = (struct fc_els_reject_s *) fchdr;
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "FDLS process SCR response: 0x%04x",
				 (uint32_t) scr_rsp->command);

	if (fdls_get_state(fdls) != FDLS_STATE_SCR) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "SCR resp recvd in state(%d). Dropping.",
					 fdls_get_state(fdls));
		return;
	}

	switch (scr_rsp->command) {
	case FC_LS_ACC:
		if (iport->fabric.timer_pending) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "Canceling fabric disc timer %p\n", iport);
			fnic_del_fabric_timer_sync();
		}
		iport->fabric.timer_pending = 0;
		iport->fabric.retry_counter = 0;
		fdls_send_gpn_ft(iport, FDLS_STATE_GPN_FT);
		break;

	case FC_LS_REJ:
		if (((els_rjt->reason_code == FC_ELS_RJT_LOGICAL_BUSY)
			 || (els_rjt->reason_code == FC_ELS_RJT_BUSY))
			&& (fdls->retry_counter < FDLS_RETRY_COUNT)) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "SCR FC_LS_REJ BUSY. Retry from timer routine %p",
						 iport);
			/* Retry again from the timer routine */
			fdls->flags |= FNIC_FDLS_RETRY_FRAME;
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "SCR returned FC_LS_REJ. Halting discovery %p",
						 iport);
			if (iport->fabric.timer_pending) {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
							 "Canceling fabric disc timer %p\n", iport);
				fnic_del_fabric_timer_sync();
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
fdls_process_gpn_ft_rsp(struct fnic_iport_s *iport, struct fc_hdr_s *fchdr,
						int len)
{
	struct fnic_fdls_fabric_s *fdls = &iport->fabric;
	struct fc_gpn_ft_s *gpn_ft_rsp = (struct fc_gpn_ft_s *) fchdr;
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

	iport->state = FNIC_IPORT_STATE_READY;
	rsp = FNIC_GET_FC_CT_CMD((&gpn_ft_rsp->fc_ct_hdr));

	switch (rsp) {

	case FC_CT_ACC:
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "0x%x: GPNFT_RSP accept", iport->fcid);
		break;

	case FC_CT_REJ:
		reason_code = gpn_ft_rsp->fc_ct_hdr.reason_code;
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "0x%x: GPNFT_RSP Reject", iport->fcid);
		break;

	default:
		break;
	}
}

/***********************************************************************
 * fdls_process_fabric_logo_rsp
 *
 * \brief Handles a flogo response from the fcf
 *
 * \param[in]  iport   Handle to fnic iport.
 *
 * \param[in]  fchdr   Incoming frame
 *
 * \retval void
 *
 *
 ***********************************************************************/
static void
fdls_process_fabric_logo_rsp(struct fnic_iport_s *iport,
							 struct fc_hdr_s *fchdr)
{
	struct fc_els_s *flogo_rsp = (struct fc_els_s *) fchdr;
	struct fnic *fnic = iport->fnic;

	switch (flogo_rsp->command) {
	case FC_LS_ACC:
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
						 "lport 0x%p Canceling fabric disc timer\n",
						 iport);
			fnic_del_fabric_timer_sync();
		}
		iport->fabric.timer_pending = 0;
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Flogo response from Fabric for did: 0x%x",
					 ntoh24(fchdr->did));
		return;

	case FC_LS_REJ:
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "Flogo response from Fabric for did: 0x%x returned FC_LS_REJ",
			 ntoh24(fchdr->did));
		return;

	default:
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "FLOGO response not accepted or rejected: 0x%x",
					 flogo_rsp->command);
	}
}

static void
fdls_process_flogi_rsp(struct fnic_iport_s *iport, struct fc_hdr_s *fchdr,
					   void *rx_frame)
{
	struct fnic_fdls_fabric_s *fabric = &iport->fabric;
	struct fc_els_s *flogi_rsp = (struct fc_els_s *) fchdr;
	uint8_t *fcid;
	int rdf_size;
	struct fc_els_reject_s *els_rjt;
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

	switch (flogi_rsp->command) {
	case FC_LS_ACC:
		if (iport->fabric.timer_pending) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "iport fcid: 0x%x Canceling fabric disc timer\n",
						 iport->fcid);
			fnic_del_fabric_timer_sync();
		}

		iport->fabric.timer_pending = 0;
		iport->fabric.retry_counter = 0;
		fcid = FNIC_GET_D_ID(fchdr);
		iport->fcid = ntoh24(fcid);
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "0x%x: FLOGI response accepted", iport->fcid);

		/* Learn the Service Params */
		rdf_size = ntohs(flogi_rsp->u.csp_flogi.b2b_rdf_size);
		if ((rdf_size >= FNIC_MIN_DATA_FIELD_SIZE)
			&& (rdf_size < FNIC_FC_MAX_PAYLOAD_LEN))
			iport->max_payload_size = MIN(rdf_size,
								  iport->max_payload_size);

		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "max_payload_size from fabric: %d set: %d", rdf_size,
					 iport->max_payload_size);

		iport->r_a_tov = ntohl(flogi_rsp->u.csp_flogi.r_a_tov);
		iport->e_d_tov = ntohl(flogi_rsp->u.csp_flogi.e_d_tov);

		if (flogi_rsp->u.csp_flogi.features & FNIC_FC_EDTOV_NSEC)
			iport->e_d_tov = iport->e_d_tov / FNIC_NSEC_TO_MSEC;

		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "From fabric: R_A_TOV: %d E_D_TOV: %d",
					 iport->r_a_tov, iport->e_d_tov);

		if (IS_FNIC_FCP_INITIATOR(fnic)) {
			fc_host_fabric_name(iport->fnic->lport->host) =
				get_unaligned_be64(&flogi_rsp->node_name);
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

	case FC_LS_REJ:
		els_rjt = (struct fc_els_reject_s *) fchdr;
		if (fabric->retry_counter < iport->max_flogi_retries) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "FLOGI returned FC_LS_REJ BUSY. Retry from timer routine %p",
				 iport);

			/* Retry Flogi again from the timer routine. */
			fabric->flags |= FNIC_FDLS_RETRY_FRAME;

		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "FLOGI returned FC_LS_REJ. Halting discovery %p", iport);
			if (iport->fabric.timer_pending) {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
							 "iport 0x%p Canceling fabric disc timer\n",
							 iport);
				fnic_del_fabric_timer_sync();
			}
			fabric->timer_pending = 0;
			fabric->retry_counter = 0;
		}
		break;

	default:
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "FLOGI response not accepted: 0x%x",
					 flogi_rsp->command);
		break;
	}
}

static void
fdls_process_fabric_plogi_rsp(struct fnic_iport_s *iport,
							  struct fc_hdr_s *fchdr)
{
	struct fc_els_s *plogi_rsp = (struct fc_els_s *) fchdr;
	struct fc_els_reject_s *els_rjt = (struct fc_els_reject_s *) fchdr;
	struct fnic *fnic = iport->fnic;

	if (fdls_get_state((&iport->fabric)) != FDLS_STATE_FABRIC_PLOGI) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "Fabric PLOGI response received in state (%d). Dropping frame",
			 fdls_get_state(&iport->fabric));
		return;
	}

	switch (plogi_rsp->command) {
	case FC_LS_ACC:
		if (iport->fabric.timer_pending) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "iport fcid: 0x%x fabric PLOGI response: Accepted\n",
				 iport->fcid);
			fnic_del_fabric_timer_sync();
		}
		iport->fabric.timer_pending = 0;
		iport->fabric.retry_counter = 0;
		fdls_set_state(&iport->fabric, FDLS_STATE_RPN_ID);
		fdls_send_rpn_id(iport);
		break;
	case FC_LS_REJ:
		if (((els_rjt->reason_code == FC_ELS_RJT_LOGICAL_BUSY)
			 || (els_rjt->reason_code == FC_ELS_RJT_BUSY))
			&& (iport->fabric.retry_counter < iport->max_plogi_retries)) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "0x%x: Fabric PLOGI FC_LS_REJ BUSY. Retry from timer routine",
				 iport->fcid);
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "0x%x: Fabric PLOGI FC_LS_REJ. Halting discovery",
				 iport->fcid);
			if (iport->fabric.timer_pending) {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
							 "iport fcid: 0x%x Canceling fabric disc timer\n",
							 iport->fcid);
				fnic_del_fabric_timer_sync();
			}
			iport->fabric.timer_pending = 0;
			iport->fabric.retry_counter = 0;
			return;
		}
		break;
	default:
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "PLOGI response not accepted: 0x%x",
					 plogi_rsp->command);
		break;
	}
}

static void
fdls_process_fabric_abts_rsp(struct fnic_iport_s *iport,
							 struct fc_hdr_s *fchdr)
{
	uint32_t s_id;
	struct fc_abts_ba_acc_s *ba_acc = (struct fc_abts_ba_acc_s *) fchdr;
	struct fc_abts_ba_rjt_s *ba_rjt;
	uint32_t fabric_state = iport->fabric.state;
	struct fnic *fnic = iport->fnic;

	s_id = ntoh24(fchdr->sid);
	ba_rjt = (struct fc_abts_ba_rjt_s *) fchdr;

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
		fnic_del_fabric_timer_sync();
	}
	iport->fabric.timer_pending = 0;
	iport->fabric.flags &= ~FNIC_FDLS_FABRIC_ABORT_ISSUED;

	if (fchdr->r_ctl == FNIC_BA_ACC_RCTL) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "Received abts rsp BA_ACC for fabric_state: %d OX_ID: 0x%x",
			 fabric_state, ba_acc->ox_id);
	} else if (fchdr->r_ctl == FNIC_BA_RJT_RCTL) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "BA_RJT fs: %d OX_ID: 0x%x rc: 0x%x rce: 0x%x",
			 fabric_state, ba_rjt->fchdr.ox_id,
			 ba_rjt->reason_code, ba_rjt->reason_explanation);
	}

	/* currently error handling/retry logic is same for ABTS BA_ACC & BA_RJT */
	switch (fabric_state) {
	case FDLS_STATE_FABRIC_FLOGI:
		if (fchdr->ox_id == FNIC_FLOGI_OXID) {
			if (iport->fabric.retry_counter < iport->max_flogi_retries)
				fdls_send_fabric_flogi(iport);
			else
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "Exceeded max FLOGI retries");
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Unknown abts rsp OX_ID: 0x%x FABRIC_FLOGI state. Drop frame",
				 fchdr->ox_id);
		}
		break;
	case FDLS_STATE_FABRIC_LOGO:
		if (fchdr->ox_id == FNIC_FLOGO_REQ_OXID) {
			if (!RETRIES_EXHAUSTED(iport))
				fdls_send_fabric_logo(iport);
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Unknown abts rsp OX_ID: 0x%x FABRIC_FLOGI state. Drop frame",
				 fchdr->ox_id);
		}
		break;
	case FDLS_STATE_FABRIC_PLOGI:
		if (fchdr->ox_id == FNIC_PLOGI_FABRIC_OXID) {
			if (iport->fabric.retry_counter < iport->max_plogi_retries)
				fdls_send_fabric_plogi(iport);
			else
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Exceeded max PLOGI retries");
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Unknown abts rsp OX_ID: 0x%x FABRIC_PLOGI state. Drop frame",
				 fchdr->ox_id);
		}
		break;

	case FDLS_STATE_RPN_ID:
		if (fchdr->ox_id == FNIC_RPN_REQ_OXID) {
			if (iport->fabric.retry_counter < FDLS_RETRY_COUNT) {
				fdls_send_rpn_id(iport);
			} else {
				/* go back to fabric Plogi */
				fnic_fdls_start_plogi(iport);
			}
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Unknown abts rsp OX_ID: 0x%x RPN_ID state. Drop frame",
				 fchdr->ox_id);
		}
		break;

	case FDLS_STATE_SCR:
		if (fchdr->ox_id == FNIC_SCR_REQ_OXID) {
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
				 fchdr->ox_id);
		}
		break;
	case FDLS_STATE_REGISTER_FC4_TYPES:
		if (fchdr->ox_id == FNIC_RFT_REQ_OXID) {
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
				 fchdr->ox_id);
		}
		break;
	case FDLS_STATE_REGISTER_FC4_FEATURES:
		if (fchdr->ox_id == FNIC_RFF_REQ_OXID) {
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
				 fchdr->ox_id);
		}
		break;

	case FDLS_STATE_GPN_FT:
		if (fchdr->ox_id == FNIC_GPN_FT_OXID) {
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
				 fchdr->ox_id);
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
	struct fc_hdr_s *fchdr;
	uint8_t type;
	uint8_t *fc_payload;
	uint16_t oxid;
	uint32_t s_id;
	uint32_t d_id;
	struct fnic *fnic = iport->fnic;
	struct fnic_fdls_fabric_s *fabric = &iport->fabric;

	fchdr = (struct fc_hdr_s *) ((uint8_t *) rx_frame + fchdr_offset);
	oxid = FNIC_GET_OX_ID(fchdr);
	fc_payload = (uint8_t *) fchdr + sizeof(struct fc_hdr_s);
	type = *fc_payload;
	s_id = ntoh24(fchdr->sid);
	d_id = ntoh24(fchdr->did);

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
	if ((fchdr->r_ctl == FNIC_BA_ACC_RCTL)
		|| (fchdr->r_ctl == FNIC_BA_RJT_RCTL)) {
		if (!(FNIC_FC_FRAME_TYPE_BLS(fchdr))) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "Received ABTS invalid frame. Dropping frame");
			return -1;

		}
		return FNIC_BLS_ABTS_RSP;
	}
	if ((fchdr->r_ctl == FC_ABTS_RCTL) && (FNIC_FC_FRAME_TYPE_BLS(fchdr))) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Receiving Abort Request from s_id: 0x%x", s_id);
		return FNIC_BLS_ABTS_REQ;
	}

	/* unsolicited requests frames */
	if (FNIC_FC_FRAME_UNSOLICITED(fchdr)) {
		switch (type) {
		case FC_ELS_LOGO:
			if ((!FNIC_FC_FRAME_FCTL_FIRST_LAST_SEQINIT(fchdr))
				|| (!FNIC_FC_FRAME_UNSOLICITED(fchdr))
				|| (!FNIC_FC_FRAME_TYPE_ELS(fchdr))) {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
							 "Received LOGO invalid frame. Dropping frame");
				return -1;
			}
			return FNIC_ELS_LOGO_REQ;
		case FC_ELS_RSCN:
			if ((!FNIC_FC_FRAME_FCTL_FIRST_LAST_SEQINIT(fchdr))
				|| (!FNIC_FC_FRAME_TYPE_ELS(fchdr))
				|| (!FNIC_FC_FRAME_UNSOLICITED(fchdr))) {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "Received RSCN invalid FCTL. Dropping frame");
				return -1;
			}
			if (s_id != FC_FABRIC_CONTROLLER)
				return FNIC_ELS_RSCN_REQ;
			break;
		case FC_ELS_PLOGI_REQ:
			return FNIC_ELS_PLOGI_REQ;
		case FC_ELS_ECHO_REQ:
			return FNIC_ELS_ECHO_REQ;
		case FNIC_ELS_ADISC_REQ:
			return FNIC_ELS_ADISC;
		case FC_ELS_RLS_REQ:
			return FNIC_ELS_RLS;
		case FC_ELS_RRQ_REQ:
			return FNIC_ELS_RRQ;
		default:
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Unsupported frame (type:0x%02x) from fcid: 0x%x",
				 type, s_id);
			return FNIC_ELS_UNSUPPORTED_REQ;
		}
	}

	/*response from fabric */
	switch (oxid) {

	case FNIC_FLOGO_REQ_OXID:
		return FNIC_FABRIC_LOGO_RSP;

	case FNIC_FLOGI_OXID:
		if (type == FC_LS_ACC) {
			if ((s_id != FC_DOMAIN_CONTR)
				|| (!FNIC_FC_FRAME_TYPE_ELS(fchdr))) {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Received unknown frame. Dropping frame");
				return -1;
			}
		}
		return FNIC_FABRIC_FLOGI_RSP;

	case FNIC_PLOGI_FABRIC_OXID:
		if (type == FC_LS_ACC) {
			if ((s_id != FC_DIR_SERVER)
				|| (!FNIC_FC_FRAME_TYPE_ELS(fchdr))) {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Received unknown frame. Dropping frame");
				return -1;
			}
		}
		return FNIC_FABRIC_PLOGI_RSP;

	case FNIC_SCR_REQ_OXID:
		if (type == FC_LS_ACC) {
			if ((s_id != FC_FABRIC_CONTROLLER)
				|| (!FNIC_FC_FRAME_TYPE_ELS(fchdr))) {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Received unknown frame. Dropping frame");
				return -1;
			}
		}
		return FNIC_FABRIC_SCR_RSP;

	case FNIC_RPN_REQ_OXID:
		if ((s_id != FC_DIR_SERVER) || (!FNIC_FC_FRAME_TYPE_FC_GS(fchdr))) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Received unknown frame. Dropping frame");
			return -1;
		}
		return FNIC_FABRIC_RPN_RSP;
	case FNIC_RFT_REQ_OXID:
		if ((s_id != FC_DIR_SERVER) || (!FNIC_FC_FRAME_TYPE_FC_GS(fchdr))) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Received unknown frame. Dropping frame");
			return -1;
		}
		return FNIC_FABRIC_RFT_RSP;
	case FNIC_RFF_REQ_OXID:
		if ((s_id != FC_DIR_SERVER) || (!FNIC_FC_FRAME_TYPE_FC_GS(fchdr))) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Received unknown frame. Dropping frame");
			return -1;
		}
		return FNIC_FABRIC_RFF_RSP;

	case FNIC_GPN_FT_OXID:
		if ((s_id != FC_DIR_SERVER) || (!FNIC_FC_FRAME_TYPE_FC_GS(fchdr))) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Received unknown frame. Dropping frame");
			return -1;
		}
		return FNIC_FABRIC_GPN_FT_RSP;

	default:
		/* Drop the Rx frame and log/stats it */
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Solicited response: unknown OXID: 0x%x", oxid);
		return -1;
	}
	return -1;
}

void fnic_fdls_recv_frame(struct fnic_iport_s *iport, void *rx_frame,
						  int len, int fchdr_offset)
{
	uint16_t oxid;
	struct fc_hdr_s *fchdr;
	uint32_t s_id = 0;
	uint32_t d_id = 0;
	struct fnic *fnic = iport->fnic;
	int frame_type;

	fchdr = (struct fc_hdr_s *) ((uint8_t *) rx_frame + fchdr_offset);
	s_id = ntoh24(fchdr->sid);
	d_id = ntoh24(fchdr->did);

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
		oxid = FNIC_GET_OX_ID(fchdr);
		if ((iport->fabric.flags & FNIC_FDLS_FABRIC_ABORT_ISSUED)
			&& (oxid >= FNIC_FLOGI_OXID && oxid <= FNIC_RFF_REQ_OXID)) {
			fdls_process_fabric_abts_rsp(iport, fchdr);
		}
		break;
	default:
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "Received unknown FCoE frame of len: %d. Dropping frame", len);
		break;
	}
}
