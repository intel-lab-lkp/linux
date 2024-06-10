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

#define FC_FC4_TYPE_SCSI 0x08

static void fdls_send_rpn_id(struct fnic_iport_s *iport);
static void fdls_fdmi_register_hba(struct fnic_iport_s *iport);
static void fdls_fdmi_register_pa(struct fnic_iport_s *iport);

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
 * did, sid, oxid
 */
struct fc_els_prli_s fnic_prli_req = {
	.fchdr = {.r_ctl = 0x22, .type = 0x01,
			  .f_ctl = FNIC_ELS_REQ_FCTL, .rx_id = 0xFFFF},
	.command = FC_ELS_PRLI_REQ,
	.page_len = 16,
	.payload_len = 0x1400,
	.sp = {.type = 0x08, .flags = 0x0020, .csp = 0xA2000000}
};

/*
 * Variables:
 * sid, port_id, port_name
 */
struct fc_fdmi_rhba_s fnic_fdmi_rhba = {
	.fchdr = {.r_ctl = 0x02, .did = {0xFF, 0XFF, 0XFA}, .type = 0x20,
			  .f_ctl = FNIC_ELS_REQ_FCTL, .ox_id = FNIC_FDMI_REG_HBA_OXID,
			  .rx_id = 0xFFFF},
	.fc_ct_hdr = {.rev = 0x01, .fs_type = 0xFA, .fs_subtype = 0x10,
				  .command = 0x0002},
	.num_ports = 0x1000000,
	.num_hba_attributes = 0x9000000,
	.type_nn = FNIC_FDMI_TYPE_NODE_NAME,
	.length_nn = 0xc00,
	.type_manu = FNIC_FDMI_TYPE_MANUFACTURER,
	.length_manu = 0x1800,
	.manufacturer = FNIC_FDMI_MANUFACTURER,
	.type_serial = FNIC_FDMI_TYPE_SERIAL_NUMBER,
	.length_serial = 0x1400,
	.type_model = FNIC_FDMI_TYPE_MODEL,
	.length_model = 0x1000,
	.type_model_des = FNIC_FDMI_TYPE_MODEL_DES,
	.length_model_des = 0x3c00,
	.model_description = FNIC_FDMI_MODEL_DESCRIPTION,
	.type_hw_ver = FNIC_FDMI_TYPE_HARDWARE_VERSION,
	.length_hw_ver = 0x1400,
	.type_dr_ver = FNIC_FDMI_TYPE_DRIVER_VERSION,
	.length_dr_ver = 0x2000,
	.type_rom_ver = FNIC_FDMI_TYPE_ROM_VERSION,
	.length_rom_ver = 0xc00,
	.type_fw_ver = FNIC_FDMI_TYPE_FIRMWARE_VERSION,
	.length_fw_ver = 0x1400,
};

/*
 * Variables
 *sid, port_id, port_name
 */
struct fc_fdmi_rpa_s fnic_fdmi_rpa = {
	.fchdr = {.r_ctl = 0x02, .did = {0xFF, 0xFF, 0xFA}, .type = 0x20,
			  .f_ctl = FNIC_ELS_REQ_FCTL, .ox_id = FNIC_FDMI_RPA_OXID,
			  .rx_id = 0xFFFF},
	.fc_ct_hdr = {.rev = 0x01, .fs_type = 0xFA, .fs_subtype = 0x10,
				  .command = 0x1102},
	.num_port_attributes = 0x6000000,
	.type_fc4 = FNIC_FDMI_TYPE_FC4_TYPES,
	.length_fc4 = 0x2400,
	.type_supp_speed = FNIC_FDMI_TYPE_SUPPORTED_SPEEDS,
	.length_supp_speed = 0x800,
	.type_cur_speed = FNIC_FDMI_TYPE_CURRENT_SPEED,
	.length_cur_speed = 0x800,
	.type_max_frame_size = FNIC_FDMI_TYPE_MAX_FRAME_SIZE,
	.length_max_frame_size = 0x800,
	.max_frame_size = 0x0080000,
	.type_os_name = FNIC_FDMI_TYPE_OS_NAME,
	.length_os_name = 0x1400,
	.type_host_name = FNIC_FDMI_TYPE_HOST_NAME,
	.length_host_name = 0x1000,
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
 * did, ox_id, rx_id
 */
struct fc_els_acc_s fnic_els_acc = {
	.fchdr = {.r_ctl = 0x23, .did = {0xFF, 0xFF, 0xFD}, .type = 0x01,
			  .f_ctl = FNIC_ELS_REP_FCTL},
	.command = FC_LS_ACC,
};

struct fc_els_reject_s fnic_els_rjt = {
	.fchdr = {.r_ctl = 0x23, .type = 0x01, .f_ctl = FNIC_ELS_REP_FCTL},
	.command = FC_LS_REJ,
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

static struct fc_abts_ba_acc_s fnic_ba_acc = {
	.fchdr = {.r_ctl = 0x84,
			  .f_ctl = FNIC_FCP_RSP_FCTL},
	.low_seq_cnt = 0,
	.high_seq_cnt = 0xFFFF,
};

#define RETRIES_EXHAUSTED(iport)      \
	(iport->fabric.retry_counter == FABRIC_LOGO_MAX_RETRY)

#define FNIC_TPORT_MAX_NEXUS_RESTART (8)

/* Private Functions */
static void fdls_process_flogi_rsp(struct fnic_iport_s *iport,
		   struct fc_hdr_s *fchdr, void *rx_frame);
static void fnic_fdls_start_plogi(struct fnic_iport_s *iport);
static struct fnic_tport_s *fdls_create_tport(struct fnic_iport_s *iport,
									  uint32_t fcid,
									  uint64_t wwpn);
static void fdls_target_restart_nexus(struct fnic_tport_s *tport);
static void fdls_start_tport_timer(struct fnic_iport_s *iport,
					struct fnic_tport_s *tport, int timeout);
static void fdls_send_fdmi_plogi(struct fnic_iport_s *iport);
static void fdls_start_fabric_timer(struct fnic_iport_s *iport,
			int timeout);
static void fdls_tport_timer_callback(struct timer_list *t);

void fdls_init_tgt_oxid_pool(struct fnic_iport_s *iport)
{
	memset(iport->tgt_oxid_pool, 0, FDLS_TGT_OXID_POOL_SZ);
}

static uint16_t fdls_alloc_tgt_oxid(struct fnic_iport_s *iport,
									uint16_t base)
{
	int i;
	int start, end;

	start = base - FDLS_PLOGI_OXID_BASE;
	end = start + FDLS_TGT_OXID_BLOCK_SZ;

	for (i = start; i < end; i++) {
		if (iport->tgt_oxid_pool[i] == 0) {
			iport->tgt_oxid_pool[i] = 1;
			return (i + FDLS_PLOGI_OXID_BASE);
		}
	}
	return 0xFFFF;
}

static void fdls_free_tgt_oxid(struct fnic_iport_s *iport, uint16_t oxid)
{
	struct fnic *fnic = iport->fnic;

	if (iport->tgt_oxid_pool[oxid - FDLS_PLOGI_OXID_BASE] != 1) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "Freeing unused OXID: 0x%x", oxid);
	}
	iport->tgt_oxid_pool[oxid - FDLS_PLOGI_OXID_BASE] = 0;
}

static void
fdls_start_fabric_timer(struct fnic_iport_s *iport, int timeout)
{
	u64 fabric_tov;
	struct fnic *fnic = iport->fnic;

	if (iport->fabric.timer_pending) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
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
	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "fabric timer is %d ", timeout);
}

static void
fdls_start_tport_timer(struct fnic_iport_s *iport,
					   struct fnic_tport_s *tport, int timeout)
{
	u64 fabric_tov;
	struct fnic *fnic = iport->fnic;

	if (tport->timer_pending) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "tport fcid 0x%x: Canceling disc timer\n",
					 tport->fcid);
		fnic_del_tport_timer_sync();
		tport->timer_pending = 0;
	}

	if (!(tport->flags & FNIC_FDLS_TGT_ABORT_ISSUED))
		tport->retry_counter++;

	fabric_tov = jiffies + msecs_to_jiffies(timeout);
	mod_timer(&tport->retry_timer, round_jiffies(fabric_tov));
	tport->timer_pending = 1;
}

static void
fdls_send_rscn_resp(struct fnic_iport_s *iport,
					struct fc_hdr_s *rscn_fchdr)
{
	struct fc_els_acc_s els_acc;
	uint16_t oxid;
	uint8_t fcid[3];

	memcpy(&els_acc, &fnic_els_acc, sizeof(struct fc_els_acc_s));

	hton24(fcid, iport->fcid);
	FNIC_SET_S_ID((&els_acc.fchdr), fcid);
	FNIC_SET_D_ID((&els_acc.fchdr), rscn_fchdr->sid);

	oxid = FNIC_GET_OX_ID(rscn_fchdr);
	FNIC_SET_OX_ID((&els_acc.fchdr), oxid);

	FNIC_SET_RX_ID((&els_acc.fchdr), FNIC_RSCN_RESP_OXID);

	fnic_send_fcoe_frame(iport, &els_acc, sizeof(struct fc_els_acc_s));
}

static void
fdls_send_logo_resp(struct fnic_iport_s *iport, struct fc_hdr_s *req_fchdr)
{
	struct fc_els_acc_s logo_resp;
	uint16_t oxid;
	uint8_t fcid[3];

	memcpy(&logo_resp, &fnic_els_acc, sizeof(struct fc_els_acc_s));

	hton24(fcid, iport->fcid);
	FNIC_SET_S_ID((&logo_resp.fchdr), fcid);
	FNIC_SET_D_ID((&logo_resp.fchdr), req_fchdr->sid);

	oxid = FNIC_GET_OX_ID(req_fchdr);
	FNIC_SET_OX_ID((&logo_resp.fchdr), oxid);

	FNIC_SET_RX_ID((&logo_resp.fchdr), FNIC_LOGO_RESP_OXID);
	fnic_send_fcoe_frame(iport, &logo_resp, sizeof(struct fc_els_acc_s));
}

void
fdls_send_tport_abts(struct fnic_iport_s *iport,
					 struct fnic_tport_s *tport)
{
	uint8_t s_id[3];
	uint8_t d_id[3];
	struct fnic *fnic = iport->fnic;

	struct fc_hdr_s fc_abts_s = {
		.r_ctl = 0x81,			/* ABTS */
		.cs_ctl = 0x00, .type = 0x00, .f_ctl = FNIC_REQ_ABTS_FCTL, .seq_id =
			0x00, .df_ctl = 0x00, .seq_cnt = 0x0000, .rx_id = 0xFFFF,
		.param = 0x00000000,	/* bit:0 = 0 Abort a exchange */
	};

	struct fc_hdr_s *pfc_abts = &fc_abts_s;

	hton24(s_id, iport->fcid);
	hton24(d_id, tport->fcid);
	FNIC_SET_S_ID(pfc_abts, s_id);
	FNIC_SET_D_ID(pfc_abts, d_id);
	tport->flags |= FNIC_FDLS_TGT_ABORT_ISSUED;

	fc_abts_s.ox_id = tport->oxid_used;

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "FDLS sending tport abts: tport->state: %d ",
				 tport->state);

	fnic_send_fcoe_frame(iport, &fc_abts_s, sizeof(struct fc_hdr_s));
	/* Even if fnic_send_fcoe_frame() fails we want to retry after timeout */
	fdls_start_tport_timer(iport, tport, 2 * iport->r_a_tov);
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
	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
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

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "0x%x: FDLS send fabric FLOGI", iport->fcid);

	memcpy(&flogi, &fnic_flogi_req, sizeof(struct fc_els_s));
	FNIC_SET_NPORT_NAME(flogi, iport->wwpn);
	FNIC_SET_NODE_NAME(flogi, iport->wwnn);
	FNIC_SET_RDF_SIZE(flogi.u.csp_flogi, iport->max_payload_size);
	FNIC_SET_R_A_TOV(flogi.u.csp_flogi, iport->r_a_tov);
	FNIC_SET_E_D_TOV(flogi.u.csp_flogi, iport->e_d_tov);

	fnic_send_fcoe_frame(iport, &flogi, sizeof(struct fc_els_s));
	/* Even if fnic_send_fcoe_frame() fails we want to retry after timeout */
	atomic64_inc(&iport->iport_stats.fabric_flogi_sent);
	fdls_start_fabric_timer(iport, 2 * iport->e_d_tov);
}


static void fdls_send_fabric_plogi(struct fnic_iport_s *iport)
{
	struct fc_els_s plogi;
	struct fc_hdr_s *fchdr = &plogi.fchdr;
	uint8_t fcid[3];
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "0x%x: FDLS send fabric PLOGI", iport->fcid);

	memcpy(&plogi, &fnic_plogi_req, sizeof(struct fc_els_s));

	hton24(fcid, iport->fcid);

	FNIC_SET_S_ID(fchdr, fcid);
	FNIC_SET_NPORT_NAME(plogi, iport->wwpn);
	FNIC_SET_NODE_NAME(plogi, iport->wwnn);
	FNIC_SET_RDF_SIZE(plogi.u.csp_plogi, iport->max_payload_size);

	atomic64_inc(&iport->iport_stats.fabric_plogi_sent);
	fnic_send_fcoe_frame(iport, &plogi, sizeof(struct fc_els_s));
	/* Even if fnic_send_fcoe_frame() fails we want to retry after timeout */
	fdls_start_fabric_timer(iport, 2 * iport->e_d_tov);
}

static void fdls_send_fdmi_plogi(struct fnic_iport_s *iport)
{
	struct fc_els_s plogi;
	struct fc_hdr_s *fchdr = &plogi.fchdr;
	uint8_t fcid[3];
	struct fnic *fnic = iport->fnic;
	u64 fdmi_tov;

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "fcid: 0x%x: FDLS send FDMI PLOGI", iport->fcid);

	memcpy(&plogi, &fnic_plogi_req, sizeof(plogi));

	hton24(fcid, iport->fcid);

	FNIC_SET_S_ID(fchdr, fcid);
	hton24(fcid, 0XFFFFFA);
	FNIC_SET_D_ID(fchdr, fcid);
	FNIC_SET_OX_ID(fchdr, FNIC_PLOGI_FDMI_OXID);
	FNIC_SET_NPORT_NAME(plogi, iport->wwpn);
	FNIC_SET_NODE_NAME(plogi, iport->wwnn);
	FNIC_SET_RDF_SIZE(plogi.u.csp_plogi, iport->max_payload_size);

	fnic_send_fcoe_frame(iport, &plogi, sizeof(struct fc_els_s));

	fdmi_tov = jiffies + msecs_to_jiffies(5000);
	mod_timer(&iport->fabric.fdmi_timer, round_jiffies(fdmi_tov));
	iport->fabric.fdmi_pending = 1;
}

static void fdls_send_rpn_id(struct fnic_iport_s *iport)
{
	struct fc_rpn_id_s rpn_id;
	uint8_t fcid[3];
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
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

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "0x%x: FDLS send SCR", iport->fcid);

	memcpy(&scr_req, &fnic_scr_req, sizeof(struct fc_scr_s));

	hton24(fcid, iport->fcid);
	FNIC_SET_S_ID((&scr_req.fchdr), fcid);

	atomic64_inc(&iport->iport_stats.fabric_scr_sent);
	fnic_send_fcoe_frame(iport, &scr_req, sizeof(struct fc_scr_s));
	/* Even if fnic_send_fcoe_frame() fails we want to retry after timeout */
	fdls_start_fabric_timer(iport, 2 * iport->e_d_tov);
}

static void fdls_send_gpn_ft(struct fnic_iport_s *iport, int fdls_state)
{
	struct fc_gpn_ft_s gpn_ft;
	uint8_t fcid[3];
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "0x%x: FDLS send GPN FT", iport->fcid);

	memcpy(&gpn_ft, &fnic_gpn_ft_req, sizeof(struct fc_gpn_ft_s));

	hton24(fcid, iport->fcid);
	FNIC_SET_S_ID((&gpn_ft.fchdr), fcid);
	fnic_send_fcoe_frame(iport, &gpn_ft, sizeof(struct fc_gpn_ft_s));
	/* Even if fnic_send_fcoe_frame() fails we want to retry after timeout */
	fdls_start_fabric_timer(iport, 2 * iport->e_d_tov);
	fdls_set_state((&iport->fabric), fdls_state);
}

static void
fdls_send_tgt_adisc(struct fnic_iport_s *iport, struct fnic_tport_s *tport)
{
	struct fc_els_adisc_s adisc;
	uint8_t s_id[3];
	uint8_t d_id[3];
	uint16_t oxid;
	struct fnic *fnic = iport->fnic;

	memset(&adisc, 0, sizeof(struct fc_els_adisc_s));
	adisc.fchdr.r_ctl = 0x22;
	adisc.fchdr.type = 0x01;
	adisc.fchdr.f_ctl = FNIC_ELS_REQ_FCTL;
	adisc.fchdr.rx_id = 0xFFFF;

	hton24(s_id, iport->fcid);
	hton24(d_id, tport->fcid);
	FNIC_SET_S_ID((&adisc.fchdr), s_id);
	FNIC_SET_D_ID((&adisc.fchdr), d_id);

	oxid = htons(fdls_alloc_tgt_oxid(iport, FDLS_ADISC_OXID_BASE));
	if (oxid == 0xFFFF) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "Failed to allocate OXID to send ADISC %p", iport);
		return;
	}

	tport->oxid_used = oxid;
	tport->flags &= ~FNIC_FDLS_TGT_ABORT_ISSUED;

	FNIC_SET_OX_ID((&adisc.fchdr), oxid);
	FNIC_SET_NPORT_NAME(adisc, iport->wwpn);
	FNIC_SET_NODE_NAME(adisc, iport->wwnn);

	memcpy(adisc.fcid, s_id, 3);
	adisc.command = FNIC_ELS_ADISC_REQ;

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "sending ADISC to tgt fcid: 0x%x", tport->fcid);

	atomic64_inc(&iport->iport_stats.tport_adisc_sent);
	fnic_send_fcoe_frame(iport, &adisc, sizeof(struct fc_els_adisc_s));
	/* Even if fnic_send_fcoe_frame() fails we want to retry after timeout */
	fdls_start_tport_timer(iport, tport, 2 * iport->e_d_tov);
}

void fdls_delete_tport(struct fnic_iport_s *iport,
					   struct fnic_tport_s *tport)
{
	struct fnic_tport_event_s *tport_del_evt;
	struct fnic *fnic = iport->fnic;

	if ((tport->state == FDLS_TGT_STATE_OFFLINING)
		|| (tport->state == FDLS_TGT_STATE_OFFLINE))
		return;

	fdls_set_tport_state(tport, FDLS_TGT_STATE_OFFLINING);
	/*
	 * By setting this flag, the tport will not be seen in a look-up
	 * in an RSCN. Even if we move to multithreaded model, this tport
	 * will be destroyed and a new RSCN will have to create a new one
	 */
	tport->flags |= FNIC_FDLS_TPORT_TERMINATING;

	if (tport->timer_pending) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "tport fcid 0x%x: Canceling disc timer\n",
					 tport->fcid);
		fnic_del_tport_timer_sync();
		tport->timer_pending = 0;
	}

	if (IS_FNIC_FCP_INITIATOR(fnic)) {
		spin_unlock_irqrestore(&fnic->fnic_lock, fnic->lock_flags);
		fnic_rport_exch_reset(iport->fnic, tport->fcid);
		spin_lock_irqsave(&fnic->fnic_lock, fnic->lock_flags);

		if (tport->flags & FNIC_FDLS_SCSI_REGISTERED) {
			tport_del_evt =
				kzalloc(sizeof(struct fnic_tport_event_s), GFP_ATOMIC);
			if (!tport_del_evt) {
				FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "Failed to allocate memory for tport fcid: 0x%0x\n",
					 tport->fcid);
				return;
			}
			tport_del_evt->event = TGT_EV_RPORT_DEL;
			tport_del_evt->arg1 = (void *) tport;
			list_add_tail(&tport_del_evt->links, &fnic->tport_event_list);
			queue_work(fnic_event_queue, &fnic->tport_work);
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "tport 0x%x not reg with scsi_transport. Freeing locally",
				 tport->fcid);
			list_del(&tport->links);
			kfree(tport);
		}
	}
}

static void
fdls_send_tgt_plogi(struct fnic_iport_s *iport, struct fnic_tport_s *tport)
{
	struct fc_els_s plogi;
	uint8_t s_id[3];
	uint8_t d_id[3];
	uint16_t oxid;
	struct fnic *fnic = iport->fnic;
	uint32_t timeout;

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "Send tgt PLOGI to fcid: 0x%x", tport->fcid);

	memcpy(&plogi, &fnic_plogi_req, sizeof(struct fc_els_s));

	hton24(s_id, iport->fcid);
	hton24(d_id, tport->fcid);

	FNIC_SET_S_ID((&plogi.fchdr), s_id);
	FNIC_SET_D_ID((&plogi.fchdr), d_id);
	FNIC_SET_RDF_SIZE(plogi.u.csp_plogi, iport->max_payload_size);

	oxid = htons(fdls_alloc_tgt_oxid(iport, FDLS_PLOGI_OXID_BASE));
	if (oxid == 0xFFFF) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "0x%x: Failed to allocate oxid to send PLOGI to fcid: 0x%x",
				 iport->fcid, tport->fcid);
		return;
	}

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "send tgt PLOGI: tgt fcid: 0x%x oxid: 0x%x", tport->fcid,
				 ntohs(oxid));
	tport->oxid_used = oxid;
	tport->flags &= ~FNIC_FDLS_TGT_ABORT_ISSUED;

	FNIC_SET_OX_ID((&plogi.fchdr), oxid);
	FNIC_SET_NPORT_NAME(plogi, iport->wwpn);
	FNIC_SET_NODE_NAME(plogi, iport->wwnn);

	timeout = max(2 * iport->e_d_tov, iport->plogi_timeout);

	atomic64_inc(&iport->iport_stats.tport_plogi_sent);
	fnic_send_fcoe_frame(iport, &plogi, sizeof(struct fc_els_s));
	/* Even if fnic_send_fcoe_frame() fails we want to retry after timeout */
	fdls_start_tport_timer(iport, tport, timeout);
}

static uint16_t
fnic_fc_plogi_rsp_rdf(struct fnic_iport_s *iport,
					  struct fc_els_s *plogi_rsp)
{
	uint16_t b2b_rdf_size = ntohs(plogi_rsp->u.csp_plogi.b2b_rdf_size);
	uint16_t spc3_rdf_size =
		((uint16_t) (plogi_rsp->spc3[6] << 8 | plogi_rsp->spc3[7]) &
		 FNIC_FC_C3_RDF);
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "MFS: b2b_rdf_size: 0x%x spc3_rdf_size: 0x%x",
			 b2b_rdf_size, spc3_rdf_size);

	return MIN(b2b_rdf_size, spc3_rdf_size);
}

static void fdls_send_register_fc4_types(struct fnic_iport_s *iport)
{
	struct fc_rft_id rft_id;
	uint8_t fcid[3];
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
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

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "0x%x: FDLS sending FC4 features", iport->fcid);
	memcpy(&rff_id, &fnic_rff_id_req, sizeof(struct fc_rff_id));

	hton24(fcid, iport->fcid);

	FNIC_SET_S_ID((&rff_id.fchdr), fcid);
	FNIC_SET_PORT_ID((&rff_id), fcid);

	if (IS_FNIC_FCP_INITIATOR(fnic)) {
		rff_id.fc4_type = 0x08;
	} else {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "0x%x: Unknown type", iport->fcid);
	}

	fnic_send_fcoe_frame(iport, &rff_id, sizeof(struct fc_rff_id));
	fdls_start_fabric_timer(iport, 2 * iport->e_d_tov);
}

static void
fdls_send_tgt_prli(struct fnic_iport_s *iport, struct fnic_tport_s *tport)
{
	struct fc_els_prli_s prli;
	uint8_t s_id[3];
	uint8_t d_id[3];
	uint16_t oxid;
	struct fnic *fnic = iport->fnic;
	uint32_t timeout;

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "FDLS sending PRLI to tgt: 0x%x", tport->fcid);

	oxid = htons(fdls_alloc_tgt_oxid(iport, FDLS_PRLI_OXID_BASE));
	if (oxid == 0xFFFF) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "Failed to allocate OXID to send PRLI %p", iport);
		return;
	}
	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "FDLS sending PRLI to tgt: 0x%x OXID: 0x%x", tport->fcid,
				 ntohs(oxid));

	tport->oxid_used = oxid;
	tport->flags &= ~FNIC_FDLS_TGT_ABORT_ISSUED;
	memcpy(&prli, &fnic_prli_req, sizeof(struct fc_els_prli_s));

	hton24(s_id, iport->fcid);
	hton24(d_id, tport->fcid);

	FNIC_SET_S_ID((&prli.fchdr), s_id);
	FNIC_SET_D_ID((&prli.fchdr), d_id);
	FNIC_SET_OX_ID((&prli.fchdr), oxid);

	timeout = max(2 * iport->e_d_tov, iport->plogi_timeout);

	atomic64_inc(&iport->iport_stats.tport_prli_sent);
	fnic_send_fcoe_frame(iport, &prli, sizeof(struct fc_els_prli_s));
	/* Even if fnic_send_fcoe_frame() fails we want to retry after timeout */
	fdls_start_tport_timer(iport, tport, timeout);
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

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
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

/***********************************************************************
 * fdls_tgt_logout
 *
 * \brief Send plogo to the remote port
 *
 * \param[in]  iport   Handle to fnic iport. remote port
 *
 * \retval void
 *
 * \note This function does not change or check the fabric/tport state.
 *       It the caller responsibility to set the appropriate tport/fabric
 *       state when this is called. Normall fdls_tgt_state_plogo.
 *       fdls_set_tport_state(tport, fdls_tgt_state_plogo)
 *
 *\note This could be used to send plogo to nameserver process
 *       also not just target processes
 *
 ***********************************************************************/
void fdls_tgt_logout(struct fnic_iport_s *iport,
					 struct fnic_tport_s *tport)
{
	struct fc_logo_req_s logo;
	uint8_t s_id[3];
	uint8_t d_id[3];
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "Sending logo to tport fcid: 0x%x", tport->fcid);
	memcpy(&logo, &fnic_logo_req, sizeof(struct fc_logo_req_s));

	hton24(s_id, iport->fcid);
	hton24(d_id, tport->fcid);

	FNIC_SET_S_ID((&logo.fchdr), s_id);
	FNIC_SET_D_ID((&logo.fchdr), d_id);
	FNIC_SET_OX_ID((&logo.fchdr), FNIC_TLOGO_REQ_OXID);

	memcpy(&logo.fcid, s_id, 3);
	logo.wwpn = htonll(iport->wwpn);

	atomic64_inc(&iport->iport_stats.tport_logo_sent);
	fnic_send_fcoe_frame(iport, &logo, sizeof(struct fc_logo_req_s));
}

static void fdls_tgt_discovery_start(struct fnic_iport_s *iport)
{
	struct fnic_tport_s *tport, *next;
	u32 old_link_down_cnt = iport->fnic->link_down_cnt;
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "0x%x: Starting FDLS target discovery", iport->fcid);

	list_for_each_entry_safe(tport, next, &iport->tport_list, links) {
		if ((old_link_down_cnt != iport->fnic->link_down_cnt)
			|| (iport->state != FNIC_IPORT_STATE_READY)) {
			break;
		}
		/* if we marked the tport as deleted due to GPN_FT
		 * We should not send ADISC anymore
		 */
		if ((tport->state == FDLS_TGT_STATE_OFFLINING) ||
			(tport->state == FDLS_TGT_STATE_OFFLINE))
			continue;

		/* For tports which have received RSCN */
		if (tport->flags & FNIC_FDLS_TPORT_SEND_ADISC) {
			tport->retry_counter = 0;
			fdls_set_tport_state(tport, FDLS_TGT_STATE_ADISC);
			tport->flags &= ~FNIC_FDLS_TPORT_SEND_ADISC;
			fdls_send_tgt_adisc(iport, tport);
			continue;
		}
		if (fdls_get_tport_state(tport) != FDLS_TGT_STATE_INIT) {
			/* Not a new port, skip  */
			continue;
		}
		tport->retry_counter = 0;
		fdls_set_tport_state(tport, FDLS_TGT_STATE_PLOGI);
		fdls_send_tgt_plogi(iport, tport);
	}
	fdls_set_state((&iport->fabric), FDLS_STATE_TGT_DISCOVERY);
}

/*
 * Function to restart the IT nexus if we received any out of
 * sequence PLOGI/PRLI  response from the target.
 * The memory for the new tport structure is allocated
 * inside fdls_create_tport and added to the iport's tport list.
 * This will get freed later during tport_offline/linkdown
 * or module unload. The new_tport pointer will go out of scope
 * safely since the memory it is
 * pointing to it will be freed later
 */
static void fdls_target_restart_nexus(struct fnic_tport_s *tport)
{
	struct fnic_iport_s *iport = tport->iport;
	struct fnic_tport_s *new_tport = NULL;
	uint32_t fcid;
	uint64_t wwpn;
	int nexus_restart_count;
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "tport fcid: 0x%x state: %d restart_count: %d",
				 tport->fcid, tport->state, tport->nexus_restart_count);

	fcid = tport->fcid;
	wwpn = tport->wwpn;
	nexus_restart_count = tport->nexus_restart_count;

	fdls_delete_tport(iport, tport);

	if (nexus_restart_count >= FNIC_TPORT_MAX_NEXUS_RESTART) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "Exceeded nexus restart retries tport: 0x%x", fcid);
		return;
	}

	/*
	 * Allocate memory for the new tport and add it to
	 * iport's tport list.
	 * This memory will be freed during tport_offline/linkdown
	 * or module unload. The pointer new_tport is safe to go
	 * out of scope when this function returns, since the memory
	 * it is pointing to is guaranteed to be freed later
	 * as mentioned above.
	 */
	new_tport = fdls_create_tport(iport, fcid, wwpn);
	if (!new_tport) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "Error creating new tport: 0x%x", fcid);
		return;
	}

	new_tport->nexus_restart_count = nexus_restart_count + 1;
	fdls_send_tgt_plogi(iport, new_tport);
	fdls_set_tport_state(new_tport, FDLS_TGT_STATE_PLOGI);
}

struct fnic_tport_s *fnic_find_tport_by_fcid(struct fnic_iport_s *iport,
									 uint32_t fcid)
{
	struct fnic_tport_s *tport, *next;

	list_for_each_entry_safe(tport, next, &(iport->tport_list), links) {
		if ((tport->fcid == fcid)
			&& !(tport->flags & FNIC_FDLS_TPORT_TERMINATING))
			return tport;
	}
	return NULL;
}

static struct fnic_tport_s *fdls_create_tport(struct fnic_iport_s *iport,
								  uint32_t fcid, uint64_t wwpn)
{
	struct fnic_tport_s *tport;
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "FDLS create tport: fcid: 0x%x wwpn: 0x%llx", fcid, wwpn);

	tport = kzalloc(sizeof(struct fnic_tport_s), GFP_ATOMIC);
	if (!tport) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "Memory allocation failure while creating tport: 0x%x\n",
			 fcid);
		return NULL;
	}

	tport->max_payload_size = FNIC_FCOE_MAX_FRAME_SZ;
	tport->r_a_tov = FNIC_R_A_TOV_DEF;
	tport->e_d_tov = FNIC_E_D_TOV_DEF;
	tport->fcid = fcid;
	tport->wwpn = wwpn;
	tport->iport = iport;

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "Need to setup tport timer callback");

	timer_setup(&tport->retry_timer, fdls_tport_timer_callback, 0);

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "Added tport 0x%x", tport->fcid);
	fdls_set_tport_state(tport, FDLS_TGT_STATE_INIT);
	list_add_tail(&tport->links, &iport->tport_list);
	atomic_set(&tport->in_flight, 0);
	return tport;
}

struct fnic_tport_s *fnic_find_tport_by_wwpn(struct fnic_iport_s *iport,
									 uint64_t wwpn)
{
	struct fnic_tport_s *tport, *next;

	list_for_each_entry_safe(tport, next, &(iport->tport_list), links) {
		if ((tport->wwpn == wwpn)
			&& !(tport->flags & FNIC_FDLS_TPORT_TERMINATING))
			return tport;
	}
	return NULL;
}

static void fdls_fdmi_register_hba(struct fnic_iport_s *iport)
{
	struct fc_fdmi_rhba_s fdmi_rhba;
	uint8_t fcid[3];
	uint16_t len;
	int err;
	struct fnic *fnic = iport->fnic;
	struct vnic_devcmd_fw_info *fw_info = NULL;

	memcpy(&fdmi_rhba, &fnic_fdmi_rhba, sizeof(struct fc_fdmi_rhba_s));

	hton24(fcid, iport->fcid);
	FNIC_SET_S_ID((&fdmi_rhba.fchdr), fcid);
	fdmi_rhba.hba_identifier = htonll(iport->wwpn);
	fdmi_rhba.port_name = htonll(iport->wwpn);
	fdmi_rhba.node_name = htonll(iport->wwnn);

	err = vnic_dev_fw_info(fnic->vdev, &fw_info);
	if (!err) {
		snprintf(fdmi_rhba.serial_num, sizeof(fdmi_rhba.serial_num) - 1,
				 "%s", fw_info->hw_serial_number);
		snprintf(fdmi_rhba.hardware_ver,
				 sizeof(fdmi_rhba.hardware_ver) - 1, "%s",
				 fw_info->hw_version);
		strscpy(fdmi_rhba.firmware_ver, fw_info->fw_version,
				sizeof(fdmi_rhba.firmware_ver) - 1);

		len = ARRAY_SIZE(fdmi_rhba.model);
		if (fnic->subsys_desc_len >= len)
			fnic->subsys_desc_len = len - 1;
		memcpy(&fdmi_rhba.model, fnic->subsys_desc, fnic->subsys_desc_len);
		fdmi_rhba.model[fnic->subsys_desc_len] = 0x00;
	}

	snprintf(fdmi_rhba.driver_ver, sizeof(fdmi_rhba.driver_ver) - 1, "%s",
			 DRV_VERSION);
	snprintf(fdmi_rhba.rom_ver, sizeof(fdmi_rhba.rom_ver) - 1, "%s",
			 "N/A");
	fnic_send_fcoe_frame(iport, &fdmi_rhba, sizeof(struct fc_fdmi_rhba_s));
}

static void fdls_fdmi_register_pa(struct fnic_iport_s *iport)
{
	struct fc_fdmi_rpa_s fdmi_rpa;

	uint8_t fcid[3];
	struct fnic *fnic = iport->fnic;
	u32 port_speed_bm;
	u32 port_speed = vnic_dev_port_speed(fnic->vdev);

	memcpy(&fdmi_rpa, &fnic_fdmi_rpa, sizeof(struct fc_fdmi_rpa_s));
	hton24(fcid, iport->fcid);
	FNIC_SET_S_ID((&fdmi_rpa.fchdr), fcid);
	fdmi_rpa.port_name = htonll(iport->wwpn);

	/* MDS does not support GIGE speed */
	switch (port_speed) {
	case DCEM_PORTSPEED_10G:
	case DCEM_PORTSPEED_20G:
		/* There is no bit for 20G */
		port_speed_bm = 0x010000;
		break;
	case DCEM_PORTSPEED_25G:
		port_speed_bm = 0x080000;
		break;
	case DCEM_PORTSPEED_40G:
	case DCEM_PORTSPEED_4x10G:
		port_speed_bm = 0x020000;
		break;
	case DCEM_PORTSPEED_100G:
		port_speed_bm = 0x040000;
		break;
	default:
		port_speed_bm = 0x8000;
		break;
	}
	fdmi_rpa.supported_speed = htonl(port_speed_bm);
	fdmi_rpa.current_speed = htonl(port_speed_bm);
	fdmi_rpa.fc4_type[2] = 1;
	snprintf(fdmi_rpa.os_name, sizeof(fdmi_rpa.os_name) - 1, "host%d",
			 fnic->host->host_no);
	snprintf(fdmi_rpa.host_name, sizeof(fdmi_rpa.host_name) - 1, "%s",
			 utsname()->nodename);
	fnic_send_fcoe_frame(iport, &fdmi_rpa, sizeof(struct fc_fdmi_rpa_s));
}

void fdls_fabric_timer_callback(struct timer_list *t)
{
	struct fnic_fdls_fabric_s *fabric = from_timer(fabric, t, retry_timer);
	struct fnic_iport_s *iport =
		container_of(fabric, struct fnic_iport_s, fabric);
	struct fnic *fnic = iport->fnic;
	unsigned long flags;

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
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
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
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
				FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
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
				FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
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
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
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
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
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
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
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
				FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
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

void fdls_fdmi_timer_callback(struct timer_list *t)
{
	struct fnic_fdls_fabric_s *fabric = from_timer(fabric, t, fdmi_timer);
	struct fnic_iport_s *iport =
		container_of(fabric, struct fnic_iport_s, fabric);
	struct fnic *fnic = iport->fnic;

	if (iport->fabric.fdmi_retry < 7) {
		iport->fabric.fdmi_retry++;
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "retry fdmi timer %d", iport->fabric.fdmi_retry);
		fdls_send_fdmi_plogi(iport);
	} else {
		iport->fabric.fdmi_pending = 0;
	}
}

static void fdls_send_delete_tport_msg(struct fnic_tport_s *tport)
{
	struct fnic_iport_s *iport = (struct fnic_iport_s *) tport->iport;
	struct fnic *fnic = iport->fnic;
	struct fnic_tport_event_s *tport_del_evt;

	if (!IS_FNIC_FCP_INITIATOR(fnic))
		return;

	tport_del_evt = kzalloc(sizeof(struct fnic_tport_event_s), GFP_ATOMIC);
	if (!tport_del_evt) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "Failed to allocate memory for tport event fcid: 0x%x",
			 tport->fcid);
		return;
	}
	tport_del_evt->event = TGT_EV_TPORT_DELETE;
	tport_del_evt->arg1 = (void *) tport;
	list_add_tail(&tport_del_evt->links, &fnic->tport_event_list);
	queue_work(fnic_event_queue, &fnic->tport_work);
}

static void fdls_tport_timer_callback(struct timer_list *t)
{
	struct fnic_tport_s *tport = from_timer(tport, t, retry_timer);
	struct fnic_iport_s *iport = (struct fnic_iport_s *) tport->iport;
	struct fnic *fnic = iport->fnic;
	uint16_t oxid;
	unsigned long flags;

	spin_lock_irqsave(&fnic->fnic_lock, flags);
	if (!tport->timer_pending) {
		spin_unlock_irqrestore(&fnic->fnic_lock, flags);
		return;
	}

	if (iport->state != FNIC_IPORT_STATE_READY) {
		spin_unlock_irqrestore(&fnic->fnic_lock, flags);
		return;
	}

	if (tport->del_timer_inprogress) {
		tport->del_timer_inprogress = 0;
		spin_unlock_irqrestore(&fnic->fnic_lock, flags);
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "tport_del_timer inprogress. Skip timer cb tport fcid: 0x%x\n",
			 tport->fcid);
		return;
	}

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
		 "tport fcid: 0x%x timer pending: %d state: %d retry counter: %d",
		 tport->fcid, tport->timer_pending, tport->state,
		 tport->retry_counter);

	tport->timer_pending = 0;
	oxid = ntohs(tport->oxid_used);

	/* We retry plogi/prli/adisc frames depending on the tport state */
	switch (tport->state) {
	case FDLS_TGT_STATE_PLOGI:
		/* PLOGI frame received a LS_RJT with busy, we retry from here */
		if ((tport->flags & FNIC_FDLS_RETRY_FRAME)
			&& (tport->retry_counter < iport->max_plogi_retries)) {
			fdls_free_tgt_oxid(iport, oxid);
			tport->flags &= ~FNIC_FDLS_RETRY_FRAME;
			fdls_send_tgt_plogi(iport, tport);
			spin_unlock_irqrestore(&fnic->fnic_lock, flags);
			return;
		}
		/* Plogi frame has timed out 2*ed_tov send abts */
		if (!(tport->flags & FNIC_FDLS_TGT_ABORT_ISSUED)) {
			fdls_send_tport_abts(iport, tport);
		} else if (tport->retry_counter < iport->max_plogi_retries) {
			/*
			 * ABTS has timed out have waited (2*ra_tov)
			 * can retry safely
			 * even if with same exchange id
			 */
			fdls_free_tgt_oxid(iport, oxid);
			fdls_send_tgt_plogi(iport, tport);
		} else {
			/* exceeded plogi retry count */
			fdls_free_tgt_oxid(iport, oxid);
			fdls_send_delete_tport_msg(tport);
		}
		break;
	case FDLS_TGT_STATE_PRLI:
		/* PRLI received a LS_RJT with busy , hence we retry from here */
		if ((tport->flags & FNIC_FDLS_RETRY_FRAME)
			&& (tport->retry_counter < FDLS_RETRY_COUNT)) {
			fdls_free_tgt_oxid(iport, oxid);
			tport->flags &= ~FNIC_FDLS_RETRY_FRAME;
			fdls_send_tgt_prli(iport, tport);
			spin_unlock_irqrestore(&fnic->fnic_lock, flags);
			return;
		}
		/* PRLI has time out 2*ed_tov send abts */
		if (!(tport->flags & FNIC_FDLS_TGT_ABORT_ISSUED)) {
			fdls_send_tport_abts(iport, tport);
		} else {
			/* ABTS has timed out for prli, we go back to PLOGI */
			fdls_free_tgt_oxid(iport, oxid);
			fdls_send_tgt_plogi(iport, tport);
			fdls_set_tport_state(tport, FDLS_TGT_STATE_PLOGI);
		}
		break;
	case FDLS_TGT_STATE_ADISC:
		/* ADISC timed out send a ABTS */
		if (!(tport->flags & FNIC_FDLS_TGT_ABORT_ISSUED)) {
			fdls_send_tport_abts(iport, tport);
			spin_unlock_irqrestore(&fnic->fnic_lock, flags);
			return;
		} else if ((tport->flags & FNIC_FDLS_TGT_ABORT_ISSUED)
				   && (tport->retry_counter < FDLS_RETRY_COUNT)) {
			/*
			 * ABTS has timed out have waited (2*ra_tov) can
			 * retry safely even if with same exchange id
			 */
			fdls_free_tgt_oxid(iport, oxid);
			fdls_send_tgt_adisc(iport, tport);

			spin_unlock_irqrestore(&fnic->fnic_lock, flags);
			return;
		}
		/* exceeded retry count */
		fdls_free_tgt_oxid(iport, oxid);
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "ADISC not responding. Deleting target port: 0x%x",
				 tport->fcid);
		fdls_send_delete_tport_msg(tport);
		spin_unlock_irqrestore(&fnic->fnic_lock, flags);
		return;
	default:
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "Unknown tport state: 0x%x", tport->state);
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

	if ((fnic_fdmi_support == 1) && (!(iport->flags & FNIC_FDMI_ACTIVE))) {
		/* we can do FDMI at the same time */
		iport->fabric.fdmi_retry = 0;
		timer_setup(&iport->fabric.fdmi_timer, fdls_fdmi_timer_callback,
					0);
		fdls_send_fdmi_plogi(iport);
		iport->flags |= FNIC_FDMI_ACTIVE;
	}
}

static void
fdls_process_tgt_adisc_rsp(struct fnic_iport_s *iport,
						   struct fc_hdr_s *fchdr)
{
	uint32_t tgt_fcid;
	struct fnic_tport_s *tport;
	uint8_t *fcid;
	uint64_t frame_wwnn;
	uint64_t frame_wwpn;
	uint16_t oxid;
	struct fc_els_adisc_ls_acc_s *adisc_rsp =
		(struct fc_els_adisc_ls_acc_s *) fchdr;
	struct fc_els_reject_s *els_rjt = (struct fc_els_reject_s *) fchdr;
	struct fnic *fnic = iport->fnic;

	fcid = FNIC_GET_S_ID(fchdr);
	tgt_fcid = ntoh24(fcid);
	tport = fnic_find_tport_by_fcid(iport, tgt_fcid);

	if (!tport) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "Tgt ADISC response tport not found: 0x%x", tgt_fcid);
		return;
	}
	if ((iport->state != FNIC_IPORT_STATE_READY)
		|| (tport->state != FDLS_TGT_STATE_ADISC)
		|| (tport->flags & FNIC_FDLS_TGT_ABORT_ISSUED)) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "Dropping this ADISC response");
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "iport state: %d tport state: %d Is abort issued on PRLI? %d",
			 iport->state, tport->state,
			 (tport->flags & FNIC_FDLS_TGT_ABORT_ISSUED));
		return;
	}
	if (ntohs(fchdr->ox_id) != ntohs(tport->oxid_used)) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "Dropping frame from target: 0x%x",
			 tgt_fcid);
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "Reason: Stale ADISC/Aborted ADISC/OOO frame delivery");
		return;
	}

	switch (adisc_rsp->command) {
	case FC_LS_ACC:
		atomic64_inc(&iport->iport_stats.tport_adisc_ls_accepts);
		if (tport->timer_pending) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
						 "tport 0x%p Canceling fabric disc timer\n",
						 tport);
			fnic_del_tport_timer_sync();
		}
		tport->timer_pending = 0;
		tport->retry_counter = 0;
		oxid = ntohs(fchdr->ox_id);
		fdls_free_tgt_oxid(iport, oxid);
		frame_wwnn = htonll(adisc_rsp->node_name);
		frame_wwpn = htonll(adisc_rsp->nport_name);
		if ((frame_wwnn == tport->wwnn) && (frame_wwpn == tport->wwpn)) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "ADISC accepted from target: 0x%x. Target logged in",
				 tgt_fcid);
			fdls_set_tport_state(tport, FDLS_TGT_STATE_READY);
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
						 "Error mismatch frame: ADISC");
		}
		break;

	case FC_LS_REJ:
		atomic64_inc(&iport->iport_stats.tport_adisc_ls_rejects);
		if (((els_rjt->reason_code == FC_ELS_RJT_LOGICAL_BUSY)
			 || (els_rjt->reason_code == FC_ELS_RJT_BUSY))
			&& (tport->retry_counter < FDLS_RETRY_COUNT)) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "ADISC ret FC_LS_REJ BUSY. Retry from timer routine: 0x%x",
				 tgt_fcid);

			/* Retry ADISC again from the timer routine. */
			tport->flags |= FNIC_FDLS_RETRY_FRAME;
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
						 "ADISC returned FC_LS_REJ from target: 0x%x",
						 tgt_fcid);
			oxid = ntohs(fchdr->ox_id);
			fdls_free_tgt_oxid(iport, oxid);
			fdls_delete_tport(iport, tport);
		}
		break;
	}
}


static void
fdls_process_tgt_plogi_rsp(struct fnic_iport_s *iport,
						   struct fc_hdr_s *fchdr)
{
	uint32_t tgt_fcid;
	struct fnic_tport_s *tport;
	uint8_t *fcid;
	uint16_t oxid;
	struct fc_els_s *plogi_rsp = (struct fc_els_s *) fchdr;
	struct fc_els_reject_s *els_rjt = (struct fc_els_reject_s *) fchdr;
	int max_payload_size;
	struct fnic *fnic = iport->fnic;

	fcid = FNIC_GET_S_ID(fchdr);
	tgt_fcid = ntoh24(fcid);

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "FDLS processing target PLOGI response: tgt_fcid: 0x%x",
				 tgt_fcid);

	tport = fnic_find_tport_by_fcid(iport, tgt_fcid);
	if (!tport) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "tport not found: 0x%x", tgt_fcid);
		return;
	}
	if ((iport->state != FNIC_IPORT_STATE_READY)
		|| (tport->flags & FNIC_FDLS_TGT_ABORT_ISSUED)) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "Dropping frame! iport state: %d tport state: %d",
					 iport->state, tport->state);
		return;
	}

	if (tport->state != FDLS_TGT_STATE_PLOGI) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "PLOGI rsp recvd in wrong state. Restarting nexus");
		oxid = ntohs(FNIC_GET_OX_ID(fchdr));
		fdls_free_tgt_oxid(iport, oxid);
		fdls_target_restart_nexus(tport);
		return;
	}

	if (ntohs(fchdr->ox_id) != ntohs(tport->oxid_used)) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "PLOGI response from target: 0x%x. Dropping frame",
			 tgt_fcid);
		return;
	}

	switch (plogi_rsp->command) {
	case FC_LS_ACC:
		atomic64_inc(&iport->iport_stats.tport_plogi_ls_accepts);
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "PLOGI accepted by target: 0x%x", tgt_fcid);
		oxid = ntohs(FNIC_GET_OX_ID(fchdr));
		fdls_free_tgt_oxid(iport, oxid);
		break;

	case FC_LS_REJ:
		atomic64_inc(&iport->iport_stats.tport_plogi_ls_rejects);
		if (((els_rjt->reason_code == FC_ELS_RJT_LOGICAL_BUSY)
			 || (els_rjt->reason_code == FC_ELS_RJT_BUSY))
			&& (tport->retry_counter < iport->max_plogi_retries)) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "PLOGI ret FC_LS_REJ BUSY. Retry from timer routine: 0x%x",
				 tgt_fcid);
			/* Retry plogi again from the timer routine. */
			tport->flags |= FNIC_FDLS_RETRY_FRAME;
			return;
		}
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "PLOGI returned FC_LS_REJ from target: 0x%x",
					 tgt_fcid);
		oxid = ntohs(fchdr->ox_id);
		fdls_free_tgt_oxid(iport, oxid);
		fdls_delete_tport(iport, tport);
		return;

	default:
		atomic64_inc(&iport->iport_stats.tport_plogi_misc_rejects);
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "PLOGI not accepted from target fcid: 0x%x",
					 tgt_fcid);
		return;
	}

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "Found the PLOGI target: 0x%x and state: %d",
				 (unsigned int) tgt_fcid, tport->state);

	if (tport->timer_pending) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "tport fcid 0x%x: Canceling disc timer\n",
					 tport->fcid);
		fnic_del_tport_timer_sync();
	}

	tport->timer_pending = 0;
	tport->wwpn = htonll(plogi_rsp->nport_name);
	tport->wwnn = htonll(plogi_rsp->node_name);

	/* Learn the Service Params */

	/* Max frame size - choose the lowest */
	max_payload_size = fnic_fc_plogi_rsp_rdf(iport, plogi_rsp);
	tport->max_payload_size =
		MIN(max_payload_size, iport->max_payload_size);

	if (tport->max_payload_size < FNIC_MIN_DATA_FIELD_SIZE) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "MFS: tport max frame size below spec bounds: %d",
			 tport->max_payload_size);
		tport->max_payload_size = FNIC_MIN_DATA_FIELD_SIZE;
	}

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
		 "MAX frame size: %d iport max_payload_size: %d tport mfs: %d",
		 max_payload_size, iport->max_payload_size,
		 tport->max_payload_size);

	tport->max_concur_seqs = FNIC_FC_PLOGI_RSP_CONCUR_SEQ(plogi_rsp);

	tport->retry_counter = 0;
	fdls_set_tport_state(tport, FDLS_TGT_STATE_PRLI);
	fdls_send_tgt_prli(iport, tport);
}

static void
fdls_process_tgt_prli_rsp(struct fnic_iport_s *iport,
						  struct fc_hdr_s *fchdr)
{
	uint32_t tgt_fcid;
	struct fnic_tport_s *tport;
	uint8_t *fcid;
	uint16_t oxid;
	struct fc_els_prli_s *prli_rsp = (struct fc_els_prli_s *) fchdr;
	struct fc_els_reject_s *els_rjt = (struct fc_els_reject_s *) fchdr;
	struct fnic_tport_event_s *tport_add_evt;
	struct fnic *fnic = iport->fnic;
	bool mismatched_tgt = false;

	fcid = FNIC_GET_S_ID(fchdr);
	tgt_fcid = ntoh24(fcid);

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "FDLS process tgt PRLI response: 0x%x", tgt_fcid);

	tport = fnic_find_tport_by_fcid(iport, tgt_fcid);
	if (!tport) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "tport not found: 0x%x", tgt_fcid);
		/* Handle or just drop? */
		return;
	}

	if ((iport->state != FNIC_IPORT_STATE_READY)
		|| (tport->flags & FNIC_FDLS_TGT_ABORT_ISSUED)) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "Dropping frame! iport st: %d tport st: %d tport fcid: 0x%x",
			 iport->state, tport->state, tport->fcid);
		return;
	}

	if (tport->state != FDLS_TGT_STATE_PRLI) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "PRLI rsp recvd in wrong state. Restarting nexus");
		oxid = ntohs(FNIC_GET_OX_ID(fchdr));
		fdls_free_tgt_oxid(iport, oxid);
		fdls_target_restart_nexus(tport);
		return;
	}

	if (ntohs(fchdr->ox_id) != ntohs(tport->oxid_used)) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "Dropping PRLI response from target: 0x%x ",
			 tgt_fcid);
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "Reason: Stale PRLI response/Aborted PDISC/OOO frame delivery");
		return;
	}

	switch (prli_rsp->command) {
	case FC_LS_ACC:
		atomic64_inc(&iport->iport_stats.tport_prli_ls_accepts);
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "PRLI accepted from target: 0x%x", tgt_fcid);
		oxid = ntohs(FNIC_GET_OX_ID(fchdr));
		fdls_free_tgt_oxid(iport, oxid);

		if (prli_rsp->sp.type != FC_FC4_TYPE_SCSI) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "mismatched target zoned with FC SCSI initiator: 0x%x",
				 tgt_fcid);
			mismatched_tgt = true;
		}
		if (mismatched_tgt) {
			fdls_tgt_logout(iport, tport);
			fdls_delete_tport(iport, tport);
			return;
		}
		break;

	case FC_LS_REJ:
		atomic64_inc(&iport->iport_stats.tport_prli_ls_rejects);
		if (((els_rjt->reason_code == FC_ELS_RJT_LOGICAL_BUSY)
			 || (els_rjt->reason_code == FC_ELS_RJT_BUSY))
			&& (tport->retry_counter < FDLS_RETRY_COUNT)) {

			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "PRLI ret FC_LS_REJ BUSY. Retry from timer routine: 0x%x",
				 tgt_fcid);

			/*Retry Plogi again from the timer routine. */
			tport->flags |= FNIC_FDLS_RETRY_FRAME;
			return;
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
						 "PRLI returned FC_LS_REJ from target: 0x%x",
						 tgt_fcid);

			oxid = ntohs(fchdr->ox_id);
			fdls_free_tgt_oxid(iport, oxid);
			fdls_tgt_logout(iport, tport);
			fdls_delete_tport(iport, tport);
			return;
		}
		break;

	default:
		atomic64_inc(&iport->iport_stats.tport_prli_misc_rejects);
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "PRLI not accepted from target: 0x%x", tgt_fcid);
		return;
		break;
	}

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "Found the PRLI target: 0x%x and state: %d",
				 (unsigned int) tgt_fcid, tport->state);

	if (tport->timer_pending) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "tport fcid 0x%x: Canceling disc timer\n",
					 tport->fcid);
		fnic_del_tport_timer_sync();
	}
	tport->timer_pending = 0;

	oxid = ntohs(FNIC_GET_OX_ID(fchdr));
	fdls_free_tgt_oxid(iport, oxid);

	/* Learn Service Params */
	tport->fcp_csp = ntohl(prli_rsp->sp.csp);
	tport->retry_counter = 0;

	if (prli_rsp->sp.csp & FCP_SPPF_RETRY)
		tport->tgt_flags |= FNIC_FC_RP_FLAGS_RETRY;

	/* Check if the device plays Target Mode Function */
	if (!(tport->fcp_csp & FCP_PRLI_FUNC_TARGET)) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "Remote port(0x%x): no target support. Deleting it\n",
			 tgt_fcid);
		fdls_tgt_logout(iport, tport);
		fdls_delete_tport(iport, tport);
		return;
	}

	fdls_set_tport_state(tport, FDLS_TGT_STATE_READY);

	/* Inform the driver about new target added */
	tport_add_evt = kzalloc(sizeof(struct fnic_tport_event_s), GFP_ATOMIC);
	if (!tport_add_evt) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "tport event memory allocation failure: 0x%0x\n",
				 tport->fcid);
		return;
	}
	tport_add_evt->event = TGT_EV_RPORT_ADD;
	tport_add_evt->arg1 = (void *) tport;
	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "iport fcid: 0x%x add tport event fcid: 0x%x\n",
			 tport->fcid, iport->fcid);
	list_add_tail(&tport_add_evt->links, &fnic->tport_event_list);
	queue_work(fnic_event_queue, &fnic->tport_work);
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
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "RFF_ID resp recvd in state(%d). Dropping.",
					 fdls_get_state(fdls));
		return;
	}

	rsp = FNIC_GET_FC_CT_CMD((&rff_rsp->fc_ct_hdr));
	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "0x%x: FDLS process RFF ID response: 0x%04x", iport->fcid,
				 (uint32_t) rsp);

	switch (rsp) {
	case FC_CT_ACC:
		if (iport->fabric.timer_pending) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
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
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "RFF_ID ret FC_LS_REJ BUSY. Retry from timer routine %p",
					 iport);

			/* Retry again from the timer routine */
			fdls->flags |= FNIC_FDLS_RETRY_FRAME;
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "RFF_ID returned FC_LS_REJ. Halting discovery %p", iport);
			if (iport->fabric.timer_pending) {
				FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
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
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "RFT_ID resp recvd in state(%d). Dropping.",
					 fdls_get_state(fdls));
		return;
	}

	rsp = FNIC_GET_FC_CT_CMD((&rft_rsp->fc_ct_hdr));
	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "0x%x: FDLS process RFT ID response: 0x%04x", iport->fcid,
				 (uint32_t) rsp);

	switch (rsp) {
	case FC_CT_ACC:
		if (iport->fabric.timer_pending) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
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
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "0x%x: RFT_ID ret FC_LS_REJ BUSY. Retry from timer routine",
				 iport->fcid);

			/* Retry again from the timer routine */
			fdls->flags |= FNIC_FDLS_RETRY_FRAME;
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "0x%x: RFT_ID REJ. Halting discovery reason %d expl %d",
				 iport->fcid, reason_code,
				 rft_rsp->fc_ct_hdr.reason_expl);
			if (iport->fabric.timer_pending) {
				FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
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
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "RPN_ID resp recvd in state(%d). Dropping.",
					 fdls_get_state(fdls));
		return;
	}

	rsp = FNIC_GET_FC_CT_CMD((&rpn_rsp->fc_ct_hdr));
	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "0x%x: FDLS process RPN ID response: 0x%04x", iport->fcid,
				 (uint32_t) rsp);

	switch (rsp) {
	case FC_CT_ACC:
		if (iport->fabric.timer_pending) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
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
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "RPN_ID returned REJ BUSY. Retry from timer routine %p",
					 iport);

			/* Retry again from the timer routine */
			fdls->flags |= FNIC_FDLS_RETRY_FRAME;
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
						 "RPN_ID FC_LS_REJ. Halting discovery %p", iport);
			if (iport->fabric.timer_pending) {
				FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
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

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "FDLS process SCR response: 0x%04x",
				 (uint32_t) scr_rsp->command);

	if (fdls_get_state(fdls) != FDLS_STATE_SCR) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "SCR resp recvd in state(%d). Dropping.",
					 fdls_get_state(fdls));
		return;
	}

	switch (scr_rsp->command) {
	case FC_LS_ACC:
		atomic64_inc(&iport->iport_stats.fabric_scr_ls_accepts);
		if (iport->fabric.timer_pending) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
						 "Canceling fabric disc timer %p\n", iport);
			fnic_del_fabric_timer_sync();
		}
		iport->fabric.timer_pending = 0;
		iport->fabric.retry_counter = 0;
		fdls_send_gpn_ft(iport, FDLS_STATE_GPN_FT);
		break;

	case FC_LS_REJ:
		atomic64_inc(&iport->iport_stats.fabric_scr_ls_rejects);
		if (((els_rjt->reason_code == FC_ELS_RJT_LOGICAL_BUSY)
			 || (els_rjt->reason_code == FC_ELS_RJT_BUSY))
			&& (fdls->retry_counter < FDLS_RETRY_COUNT)) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
						 "SCR FC_LS_REJ BUSY. Retry from timer routine %p",
						 iport);
			/* Retry again from the timer routine */
			fdls->flags |= FNIC_FDLS_RETRY_FRAME;
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
						 "SCR returned FC_LS_REJ. Halting discovery %p",
						 iport);
			if (iport->fabric.timer_pending) {
				FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
							 "Canceling fabric disc timer %p\n", iport);
				fnic_del_fabric_timer_sync();
			}
			fdls->timer_pending = 0;
			fdls->retry_counter = 0;
		}
		break;

	default:
		atomic64_inc(&iport->iport_stats.fabric_scr_misc_rejects);
		break;
	}
}

static void
fdls_process_gpn_ft_tgt_list(struct fnic_iport_s *iport,
							 struct fc_hdr_s *fchdr, int len)
{
	struct fc_gpn_ft_rsp_iu_s *gpn_ft_tgt;
	struct fc_gpn_ft_rsp_iu_s *gpn_ft_tgt_rem;
	struct fnic_tport_s *tport, *next;
	uint32_t fcid;
	uint64_t wwpn;
	int rem_len = len;
	u32 old_link_down_cnt = iport->fnic->link_down_cnt;
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "0x%x: FDLS process GPN_FT tgt list", iport->fcid);

	gpn_ft_tgt =
		(struct fc_gpn_ft_rsp_iu_s *) ((uint8_t *) fchdr +
								   sizeof(struct fc_hdr_s)
								   + sizeof(struct fc_ct_hdr_s));
	gpn_ft_tgt_rem = gpn_ft_tgt;
	len -= sizeof(struct fc_hdr_s) + sizeof(struct fc_ct_hdr_s);

	while (rem_len > 0) {

		fcid = ntoh24(gpn_ft_tgt->fcid);
		wwpn = ntohll(gpn_ft_tgt->wwpn);

		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "tport: 0x%x: ctrl:0x%x", fcid, gpn_ft_tgt->ctrl);

		if (fcid == iport->fcid) {
			if (gpn_ft_tgt->ctrl & FNIC_FC_GPN_LAST_ENTRY)
				break;
			gpn_ft_tgt++;
			rem_len -= sizeof(struct fc_gpn_ft_rsp_iu_s);
			continue;
		}

		tport = fnic_find_tport_by_wwpn(iport, wwpn);
		if (!tport) {
			/*
			 * New port registered with the switch or first time query
			 */
			tport = fdls_create_tport(iport, fcid, wwpn);
			if (!tport)
				return;
		}
		/*
		 * check if this was an existing tport with same fcid
		 * but whose wwpn has changed now ,then remove it and
		 * create a new one
		 */
		if (tport->fcid != fcid) {
			fdls_delete_tport(iport, tport);
			tport = fdls_create_tport(iport, fcid, wwpn);
			if (!tport)
				return;
		}

		/*
		 * If this GPN_FT rsp is after RSCN then mark the tports which
		 * matches with the new GPN_FT list, if some tport is not
		 * found in GPN_FT we went to delete that tport later.
		 */
		if (fdls_get_state((&iport->fabric)) == FDLS_STATE_RSCN_GPN_FT)
			tport->flags |= FNIC_FDLS_TPORT_IN_GPN_FT_LIST;

		if (gpn_ft_tgt->ctrl & FNIC_FC_GPN_LAST_ENTRY)
			break;

		gpn_ft_tgt++;
		rem_len -= sizeof(struct fc_gpn_ft_rsp_iu_s);
	}
	if (rem_len <= 0) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "GPN_FT response: malformed/corrupt frame rxlen: %d remlen: %d",
			 len, rem_len);
	}

	/*remove those ports which was not listed in GPN_FT */
	if (fdls_get_state((&iport->fabric)) == FDLS_STATE_RSCN_GPN_FT) {
		list_for_each_entry_safe(tport, next, &iport->tport_list, links) {

			if (!(tport->flags & FNIC_FDLS_TPORT_IN_GPN_FT_LIST)) {
				FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "Remove port: 0x%x not found in GPN_FT list",
					 tport->fcid);
				fdls_delete_tport(iport, tport);
			} else {
				tport->flags &= ~FNIC_FDLS_TPORT_IN_GPN_FT_LIST;
			}
			if ((old_link_down_cnt != iport->fnic->link_down_cnt)
				|| (iport->state != FNIC_IPORT_STATE_READY)) {
				return;
			}
		}
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
	int count = 0;
	struct fnic_tport_s *tport, *next;
	u32 old_link_down_cnt = iport->fnic->link_down_cnt;
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
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
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "GPNFT resp recvd in fab state(%d) iport_state(%d). Dropping.",
			 fdls_get_state(fdls), iport->state);
		return;
	}

	iport->state = FNIC_IPORT_STATE_READY;
	rsp = FNIC_GET_FC_CT_CMD((&gpn_ft_rsp->fc_ct_hdr));

	switch (rsp) {

	case FC_CT_ACC:
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "0x%x: GPNFT_RSP accept", iport->fcid);
		if (iport->fabric.timer_pending) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
						 "0x%x: Canceling fabric disc timer\n",
						 iport->fcid);
			fnic_del_fabric_timer_sync();
		}
		iport->fabric.timer_pending = 0;
		iport->fabric.retry_counter = 0;
		fdls_process_gpn_ft_tgt_list(iport, fchdr, len);

		/*
		 * iport state can change only if link down event happened
		 * We don't need to undo fdls_process_gpn_ft_tgt_list,
		 * that will be taken care in next link up event
		 */
		if (iport->state != FNIC_IPORT_STATE_READY) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "Halting target discovery: fab st: %d iport st: %d ",
				 fdls_get_state(fdls), iport->state);
			break;
		}
		fdls_tgt_discovery_start(iport);
		break;

	case FC_CT_REJ:
		reason_code = gpn_ft_rsp->fc_ct_hdr.reason_code;
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "0x%x: GPNFT_RSP Reject", iport->fcid);

		if (((reason_code == FC_CT_RJT_LOGICAL_BUSY)
			 || (reason_code == FC_CT_RJT_BUSY))
			&& (fdls->retry_counter < FDLS_RETRY_COUNT)) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "0x%x: GPNFT_RSP ret REJ/BSY. Retry from timer routine",
				 iport->fcid);
			/* Retry again from the timer routine */
			fdls->flags |= FNIC_FDLS_RETRY_FRAME;
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
						 "0x%x: GPNFT_RSP reject", iport->fcid);
			if (iport->fabric.timer_pending) {
				FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
							 "0x%x: Canceling fabric disc timer\n",
							 iport->fcid);
				fnic_del_fabric_timer_sync();
			}
			iport->fabric.timer_pending = 0;
			iport->fabric.retry_counter = 0;
			/*
			 * If GPN_FT ls_rjt then we should delete
			 * all existing tports
			 */
			count = 0;
			list_for_each_entry_safe(tport, next, &iport->tport_list,
									 links) {
				FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
							 "GPN_FT_REJECT: Remove port: 0x%x",
							 tport->fcid);
				fdls_delete_tport(iport, tport);
				if ((old_link_down_cnt != iport->fnic->link_down_cnt)
					|| (iport->state != FNIC_IPORT_STATE_READY)) {
					return;
				}
				count++;
			}
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
						 "GPN_FT_REJECT: Removed (0x%x) ports", count);
		}
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
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "Flogo response. Fabric not in LOGO state. Dropping! %p",
				 iport);
			return;
		}

		iport->fabric.state = FDLS_STATE_FLOGO_DONE;
		iport->state = FNIC_IPORT_STATE_LINK_WAIT;

		if (iport->fabric.timer_pending) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
						 "iport 0x%p Canceling fabric disc timer\n",
						 iport);
			fnic_del_fabric_timer_sync();
		}
		iport->fabric.timer_pending = 0;
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "Flogo response from Fabric for did: 0x%x",
					 ntoh24(fchdr->did));
		return;

	case FC_LS_REJ:
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "Flogo response from Fabric for did: 0x%x returned FC_LS_REJ",
			 ntoh24(fchdr->did));
		return;

	default:
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
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

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "0x%x: FDLS processing FLOGI response", iport->fcid);

	if (fdls_get_state(fabric) != FDLS_STATE_FABRIC_FLOGI) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "FLOGI response received in state (%d). Dropping frame",
					 fdls_get_state(fabric));
		return;
	}

	switch (flogi_rsp->command) {
	case FC_LS_ACC:
		atomic64_inc(&iport->iport_stats.fabric_flogi_ls_accepts);
		if (iport->fabric.timer_pending) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
						 "iport fcid: 0x%x Canceling fabric disc timer\n",
						 iport->fcid);
			fnic_del_fabric_timer_sync();
		}

		iport->fabric.timer_pending = 0;
		iport->fabric.retry_counter = 0;
		fcid = FNIC_GET_D_ID(fchdr);
		iport->fcid = ntoh24(fcid);
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "0x%x: FLOGI response accepted", iport->fcid);

		/* Learn the Service Params */
		rdf_size = ntohs(flogi_rsp->u.csp_flogi.b2b_rdf_size);
		if ((rdf_size >= FNIC_MIN_DATA_FIELD_SIZE)
			&& (rdf_size < FNIC_FC_MAX_PAYLOAD_LEN))
			iport->max_payload_size = MIN(rdf_size,
								  iport->max_payload_size);

		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "max_payload_size from fabric: %d set: %d", rdf_size,
					 iport->max_payload_size);

		iport->r_a_tov = ntohl(flogi_rsp->u.csp_flogi.r_a_tov);
		iport->e_d_tov = ntohl(flogi_rsp->u.csp_flogi.e_d_tov);

		if (flogi_rsp->u.csp_flogi.features & FNIC_FC_EDTOV_NSEC)
			iport->e_d_tov = iport->e_d_tov / FNIC_NSEC_TO_MSEC;

		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "From fabric: R_A_TOV: %d E_D_TOV: %d",
					 iport->r_a_tov, iport->e_d_tov);

		if (IS_FNIC_FCP_INITIATOR(fnic)) {
			fc_host_fabric_name(iport->fnic->host) =
				get_unaligned_be64(&flogi_rsp->node_name);
			fc_host_port_id(iport->fnic->host) = iport->fcid;
		}

		fnic_fdls_learn_fcoe_macs(iport, rx_frame, fcid);

		if (fnic_fdls_register_portid(iport, iport->fcid, rx_frame) != 0) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
						 "0x%x: FLOGI registration failed", iport->fcid);
			break;
		}

		memcpy(&fcmac[3], fcid, 3);
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "Adding vNIC device MAC addr: %02x:%02x:%02x:%02x:%02x:%02x",
			 fcmac[0], fcmac[1], fcmac[2], fcmac[3], fcmac[4],
			 fcmac[5]);
		vnic_dev_add_addr(iport->fnic->vdev, fcmac);

		if (fdls_get_state(fabric) == FDLS_STATE_FABRIC_FLOGI) {
			fnic_fdls_start_plogi(iport);
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
						 "FLOGI response received. Starting PLOGI");
		} else {
			/* From FDLS_STATE_FABRIC_FLOGI state fabric can only go to
			 * FDLS_STATE_LINKDOWN
			 * state, hence we don't have to worry about undoing:
			 * the fnic_fdls_register_portid and vnic_dev_add_addr
			 */
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "FLOGI response received in state (%d). Dropping frame",
				 fdls_get_state(fabric));
		}
		break;

	case FC_LS_REJ:
		atomic64_inc(&iport->iport_stats.fabric_flogi_ls_rejects);
		els_rjt = (struct fc_els_reject_s *) fchdr;
		if (fabric->retry_counter < iport->max_flogi_retries) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "FLOGI returned FC_LS_REJ BUSY. Retry from timer routine %p",
				 iport);

			/* Retry Flogi again from the timer routine. */
			fabric->flags |= FNIC_FDLS_RETRY_FRAME;

		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "FLOGI returned FC_LS_REJ. Halting discovery %p", iport);
			if (iport->fabric.timer_pending) {
				FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
							 "iport 0x%p Canceling fabric disc timer\n",
							 iport);
				fnic_del_fabric_timer_sync();
			}
			fabric->timer_pending = 0;
			fabric->retry_counter = 0;
		}
		break;

	default:
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "FLOGI response not accepted: 0x%x",
					 flogi_rsp->command);
		atomic64_inc(&iport->iport_stats.fabric_flogi_misc_rejects);
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
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "Fabric PLOGI response received in state (%d). Dropping frame",
			 fdls_get_state(&iport->fabric));
		return;
	}

	switch (plogi_rsp->command) {
	case FC_LS_ACC:
		atomic64_inc(&iport->iport_stats.fabric_plogi_ls_accepts);
		if (iport->fabric.timer_pending) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
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
		atomic64_inc(&iport->iport_stats.fabric_plogi_ls_rejects);
		if (((els_rjt->reason_code == FC_ELS_RJT_LOGICAL_BUSY)
			 || (els_rjt->reason_code == FC_ELS_RJT_BUSY))
			&& (iport->fabric.retry_counter < iport->max_plogi_retries)) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "0x%x: Fabric PLOGI FC_LS_REJ BUSY. Retry from timer routine",
				 iport->fcid);
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "0x%x: Fabric PLOGI FC_LS_REJ. Halting discovery",
				 iport->fcid);
			if (iport->fabric.timer_pending) {
				FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
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
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "PLOGI response not accepted: 0x%x",
					 plogi_rsp->command);
		atomic64_inc(&iport->iport_stats.fabric_plogi_misc_rejects);
		break;
	}
}

static void fdls_process_fdmi_plogi_rsp(struct fnic_iport_s *iport,
								struct fc_hdr_s *fchdr)
{
	struct fc_els_s *plogi_rsp = (struct fc_els_s *) fchdr;
	struct fc_els_reject_s *els_rjt = (struct fc_els_reject_s *) fchdr;
	struct fnic *fnic = iport->fnic;
	u64 fdmi_tov;

	if (ntoh24(fchdr->sid) == 0XFFFFFA) {
		del_timer_sync(&iport->fabric.fdmi_timer);
		iport->fabric.fdmi_pending = 0;
		switch (plogi_rsp->command) {
		case FC_LS_ACC:
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "FDLS process fdmi PLOGI response status: FC_LS_ACC\n");
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "Sending fdmi registration for port 0x%x\n",
				 iport->fcid);

			fdls_fdmi_register_hba(iport);
			fdls_fdmi_register_pa(iport);
			fdmi_tov = jiffies + msecs_to_jiffies(5000);
			mod_timer(&iport->fabric.fdmi_timer, round_jiffies(fdmi_tov));
			iport->fabric.fdmi_pending = 2;
			break;
		case FC_LS_REJ:
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "Fabric FDMI PLOGI returned FC_LS_REJ reason: 0x%x",
				 els_rjt->reason_code);

			if (((els_rjt->reason_code == FC_ELS_RJT_LOGICAL_BUSY)
				 || (els_rjt->reason_code == FC_ELS_RJT_BUSY))
				&& (iport->fabric.fdmi_retry < 7)) {
				iport->fabric.fdmi_retry++;
				fdls_send_fdmi_plogi(iport);
			}
			break;
		default:
			break;
		}
	}
}

static void fdls_process_fdmi_reg_ack(struct fnic_iport_s *iport,
									  struct fc_hdr_s *fchdr)
{
	struct fnic *fnic = iport->fnic;

	if (iport->fabric.fdmi_pending > 0) {
		iport->fabric.fdmi_pending--;
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "iport fcid: 0x%x: Received FDMI registration ack\n",
					 iport->fcid);

		if (iport->fabric.fdmi_pending == 0) {
			del_timer_sync(&iport->fabric.fdmi_timer);
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
						 "iport fcid: 0x%x: Canceling FDMI timer\n",
						 iport->fcid);
		}
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
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "Received abts rsp with invalid SID: 0x%x. Dropping frame",
			 s_id);
		return;
	}

	if (iport->fabric.timer_pending) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "Canceling fabric disc timer %p\n", iport);
		fnic_del_fabric_timer_sync();
	}
	iport->fabric.timer_pending = 0;
	iport->fabric.flags &= ~FNIC_FDLS_FABRIC_ABORT_ISSUED;

	if (fchdr->r_ctl == FNIC_BA_ACC_RCTL) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "Received abts rsp BA_ACC for fabric_state: %d OX_ID: 0x%x",
			 fabric_state, ba_acc->ox_id);
	} else if (fchdr->r_ctl == FNIC_BA_RJT_RCTL) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
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
				FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
						 "Exceeded max FLOGI retries");
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "Unknown abts rsp OX_ID: 0x%x FABRIC_FLOGI state. Drop frame",
				 fchdr->ox_id);
		}
		break;
	case FDLS_STATE_FABRIC_LOGO:
		if (fchdr->ox_id == FNIC_FLOGO_REQ_OXID) {
			if (!RETRIES_EXHAUSTED(iport))
				fdls_send_fabric_logo(iport);
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "Unknown abts rsp OX_ID: 0x%x FABRIC_FLOGI state. Drop frame",
				 fchdr->ox_id);
		}
		break;
	case FDLS_STATE_FABRIC_PLOGI:
		if (fchdr->ox_id == FNIC_PLOGI_FABRIC_OXID) {
			if (iport->fabric.retry_counter < iport->max_plogi_retries)
				fdls_send_fabric_plogi(iport);
			else
				FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "Exceeded max PLOGI retries");
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
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
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "Unknown abts rsp OX_ID: 0x%x RPN_ID state. Drop frame",
				 fchdr->ox_id);
		}
		break;

	case FDLS_STATE_SCR:
		if (fchdr->ox_id == FNIC_SCR_REQ_OXID) {
			if (iport->fabric.retry_counter <= FDLS_RETRY_COUNT)
				fdls_send_scr(iport);
			else {
				FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "abts rsp fab SCR after two tries. Start fabric PLOGI %p",
					 iport);
				fnic_fdls_start_plogi(iport);	/* go back to fabric Plogi */
			}
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "Unknown abts rsp OX_ID: 0x%x SCR state. Drop frame",
				 fchdr->ox_id);
		}
		break;
	case FDLS_STATE_REGISTER_FC4_TYPES:
		if (fchdr->ox_id == FNIC_RFT_REQ_OXID) {
			if (iport->fabric.retry_counter <= FDLS_RETRY_COUNT) {
				fdls_send_register_fc4_types(iport);
			} else {
				FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "abts rsp fab RFT_ID two tries. Start fabric PLOGI %p",
					 iport);
				fnic_fdls_start_plogi(iport);	/* go back to fabric Plogi */
			}
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "Unknown abts rsp OX_ID: 0x%x RFT state. Drop frame",
				 fchdr->ox_id);
		}
		break;
	case FDLS_STATE_REGISTER_FC4_FEATURES:
		if (fchdr->ox_id == FNIC_RFF_REQ_OXID) {
			if (iport->fabric.retry_counter <= FDLS_RETRY_COUNT)
				fdls_send_register_fc4_features(iport);
			else {
				FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "abts rsp fab SCR after two tries. Start fabric PLOGI %p",
					 iport);
				fnic_fdls_start_plogi(iport);	/* go back to fabric Plogi */
			}
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "Unknown abts rsp OX_ID: 0x%x RFF state. Drop frame",
				 fchdr->ox_id);
		}
		break;

	case FDLS_STATE_GPN_FT:
		if (fchdr->ox_id == FNIC_GPN_FT_OXID) {
			if (iport->fabric.retry_counter <= FDLS_RETRY_COUNT) {
				fdls_send_gpn_ft(iport, fabric_state);
			} else {
				FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "abts rsp fab GPN_FT after two tries %p",
					 iport);
			}
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "Unknown abts rsp OX_ID: 0x%x GPN_FT state. Drop frame",
				 fchdr->ox_id);
		}
		break;

	default:
		return;
	}
}

static void
fdls_process_abts_req(struct fnic_iport_s *iport, struct fc_hdr_s *fchdr)
{
	struct fc_abts_ba_acc_s ba_acc;
	uint32_t nport_id;
	uint16_t oxid;
	struct fnic_tport_s *tport;
	struct fnic *fnic = iport->fnic;

	nport_id = ntoh24(fchdr->sid);
	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "Received abort from SID %8x", nport_id);

	tport = fnic_find_tport_by_fcid(iport, nport_id);
	if (tport) {
		oxid = FNIC_GET_OX_ID(fchdr);
		if (tport->oxid_used == oxid) {
			tport->flags |= FNIC_FDLS_TGT_ABORT_ISSUED;
			fdls_free_tgt_oxid(iport, ntohs(oxid));
		}
	}

	memcpy(&ba_acc, &fnic_ba_acc, sizeof(struct fc_abts_ba_acc_s));
	FNIC_SET_S_ID((&ba_acc.fchdr), fchdr->did);
	FNIC_SET_D_ID((&ba_acc.fchdr), fchdr->sid);

	ba_acc.fchdr.rx_id = fchdr->rx_id;
	ba_acc.rx_id = ba_acc.fchdr.rx_id;
	ba_acc.fchdr.ox_id = fchdr->ox_id;
	ba_acc.ox_id = ba_acc.fchdr.ox_id;

	fnic_send_fcoe_frame(iport, &ba_acc, sizeof(struct fc_abts_ba_acc_s));
}

static void
fdls_process_unsupported_els_req(struct fnic_iport_s *iport,
								 struct fc_hdr_s *fchdr)
{
	struct fc_els_reject_s ls_rsp;
	uint16_t oxid;
	uint32_t d_id = ntoh24(fchdr->did);
	struct fnic *fnic = iport->fnic;

	memcpy(&ls_rsp, &fnic_els_rjt, sizeof(struct fc_els_reject_s));

	if (iport->fcid != d_id) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "Dropping unsupported ELS with illegal frame bits 0x%x\n",
			 d_id);
		atomic64_inc(&iport->iport_stats.unsupported_frames_dropped);
		return;
	}

	if ((iport->state != FNIC_IPORT_STATE_READY)
		&& (iport->state != FNIC_IPORT_STATE_FABRIC_DISC)) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "Dropping unsupported ELS request in iport state: %d",
			 iport->state);
		atomic64_inc(&iport->iport_stats.unsupported_frames_dropped);
		return;
	}

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "Process unsupported ELS request from SID: 0x%x",
				 ntoh24(fchdr->sid));
	/* We don't support this ELS request, send a reject */
	ls_rsp.reason_code = 0x0B;
	ls_rsp.reason_expl = 0x0;
	ls_rsp.vendor_specific = 0x0;

	FNIC_SET_S_ID((&ls_rsp.fchdr), fchdr->did);
	FNIC_SET_D_ID((&ls_rsp.fchdr), fchdr->sid);
	oxid = FNIC_GET_OX_ID(fchdr);
	FNIC_SET_OX_ID((&ls_rsp.fchdr), oxid);

	FNIC_SET_RX_ID((&ls_rsp.fchdr), FNIC_UNSUPPORTED_RESP_OXID);
	fnic_send_fcoe_frame(iport, &ls_rsp, sizeof(struct fc_els_reject_s));
}

static void
fdls_process_rls_req(struct fnic_iport_s *iport, struct fc_hdr_s *fchdr)
{
	struct fc_els_rls_ls_acc_s rls_acc_rsp;
	uint16_t oxid;
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "Process RLS request %d", iport->fnic->fnic_num);

	if ((iport->state != FNIC_IPORT_STATE_READY)
		&& (iport->state != FNIC_IPORT_STATE_FABRIC_DISC)) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "Received RLS req in iport state: %d. Dropping the frame.",
			 iport->state);
		return;
	}

	memset(&rls_acc_rsp, 0, sizeof(struct fc_els_rls_ls_acc_s));

	FNIC_SET_S_ID((&rls_acc_rsp.fchdr), fchdr->did);
	FNIC_SET_D_ID((&rls_acc_rsp.fchdr), fchdr->sid);
	oxid = FNIC_GET_OX_ID(fchdr);
	FNIC_SET_OX_ID((&rls_acc_rsp.fchdr), oxid);
	FNIC_SET_RX_ID((&rls_acc_rsp.fchdr), 0xffff);
	rls_acc_rsp.fchdr.f_ctl = FNIC_ELS_REP_FCTL;
	rls_acc_rsp.fchdr.r_ctl = 0x23;
	rls_acc_rsp.fchdr.type = 0x01;
	rls_acc_rsp.command = FC_LS_ACC;
	rls_acc_rsp.link_fail_count = htonl(iport->fnic->link_down_cnt);

	fnic_send_fcoe_frame(iport, &rls_acc_rsp,
						 sizeof(struct fc_els_rls_ls_acc_s));
}

static void
fdls_process_els_req(struct fnic_iport_s *iport, struct fc_hdr_s *fchdr,
					 uint32_t len)
{
	struct fc_els_acc_s *els_acc;
	uint16_t oxid;
	uint8_t fcid[3];
	uint8_t *fc_payload;
	uint8_t *dst_frame;
	uint8_t type;
	struct fnic *fnic = iport->fnic;

	fc_payload = (uint8_t *) fchdr + sizeof(struct fc_hdr_s);
	type = *fc_payload;

	if ((iport->state != FNIC_IPORT_STATE_READY)
		&& (iport->state != FNIC_IPORT_STATE_FABRIC_DISC)) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "Dropping ELS frame type :%x in iport state: %d",
				 type, iport->state);
		return;
	}
	switch (type) {
	case FC_ELS_ECHO_REQ:
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "sending LS_ACC for ECHO request %d\n",
					 iport->fnic->fnic_num);
		break;

	case FC_ELS_RRQ_REQ:
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "sending LS_ACC for RRQ request %d\n",
					 iport->fnic->fnic_num);
		break;

	default:
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "sending LS_ACC for %x ELS frame\n", type);
		break;
	}
	dst_frame = kzalloc(len, GFP_ATOMIC);
	if (!dst_frame) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "Failed to allocate ELS response for %x", type);
		return;
	}
	if (type == FC_ELS_ECHO_REQ) {
		/* Brocade sends a longer payload, copy all frame back */
		memcpy(dst_frame, fchdr, len);
	}

	els_acc = (struct fc_els_acc_s *) dst_frame;
	memcpy(els_acc, &fnic_els_acc, sizeof(struct fc_els_acc_s));

	hton24(fcid, iport->fcid);
	FNIC_SET_S_ID((&els_acc->fchdr), fcid);
	FNIC_SET_D_ID((&els_acc->fchdr), fchdr->sid);

	oxid = FNIC_GET_OX_ID(fchdr);
	FNIC_SET_OX_ID((&els_acc->fchdr), oxid);
	FNIC_SET_RX_ID((&els_acc->fchdr), 0xffff);

	if (type == FC_ELS_ECHO_REQ)
		fnic_send_fcoe_frame(iport, els_acc, len);
	else
		fnic_send_fcoe_frame(iport, els_acc, sizeof(struct fc_els_acc_s));

	kfree(dst_frame);
}

static void
fdls_process_tgt_abts_rsp(struct fnic_iport_s *iport,
						  struct fc_hdr_s *fchdr)
{
	uint32_t s_id;
	struct fnic_tport_s *tport;
	uint32_t tport_state;
	struct fc_abts_ba_acc_s *ba_acc;
	struct fc_abts_ba_rjt_s *ba_rjt;
	uint16_t oxid;
	struct fnic *fnic = iport->fnic;

	s_id = ntoh24(fchdr->sid);
	ba_acc = (struct fc_abts_ba_acc_s *) fchdr;
	ba_rjt = (struct fc_abts_ba_rjt_s *) fchdr;

	tport = fnic_find_tport_by_fcid(iport, s_id);
	if (!tport) {
		FNIC_FCS_DBG(KERN_ERR, fnic->host, fnic->fnic_num,
					 "Received tgt abts rsp with invalid SID: 0x%x", s_id);
		return;
	}

	if (tport->timer_pending) {
		FNIC_FCS_DBG(KERN_ERR, fnic->host, fnic->fnic_num,
					 "tport 0x%p Canceling fabric disc timer\n", tport);
		fnic_del_tport_timer_sync();
	}
	if (iport->state != FNIC_IPORT_STATE_READY) {
		FNIC_FCS_DBG(KERN_ERR, fnic->host, fnic->fnic_num,
					 "Received tgt abts rsp in iport state(%d). Dropping.",
					 iport->state);
		return;
	}
	tport->timer_pending = 0;
	tport->flags &= ~FNIC_FDLS_TGT_ABORT_ISSUED;
	tport_state = tport->state;
	oxid = ntohs(fchdr->ox_id);

	/*This abort rsp is for ADISC */
	if ((oxid >= FDLS_ADISC_OXID_BASE) && (oxid < FDLS_TGT_OXID_POOL_END)) {
		if (fchdr->r_ctl == FNIC_BA_ACC_RCTL) {
			FNIC_FCS_DBG(KERN_ERR, fnic->host, fnic->fnic_num,
				 "OX_ID: 0x%x tgt_fcid: 0x%x rcvd tgt adisc abts resp BA_ACC",
				 ba_acc->ox_id, tport->fcid);
		} else if (fchdr->r_ctl == FNIC_BA_RJT_RCTL) {
			FNIC_FCS_DBG(KERN_ERR, fnic->host, fnic->fnic_num,
				 "ADISC BA_RJT rcvd tport_fcid: 0x%x tport_state: %d ",
				 tport->fcid, tport_state);
			FNIC_FCS_DBG(KERN_ERR, fnic->host, fnic->fnic_num,
				 "reason code: 0x%x reason code explanation:0x%x ",
				 ba_rjt->reason_code, ba_rjt->reason_explanation);
		}
		if ((tport->retry_counter < FDLS_RETRY_COUNT)
			&& (fchdr->r_ctl == FNIC_BA_ACC_RCTL)) {
			fdls_free_tgt_oxid(iport, oxid);
			fdls_send_tgt_adisc(iport, tport);
			return;
		}
		fdls_free_tgt_oxid(iport, oxid);
		FNIC_FCS_DBG(KERN_ERR, fnic->host, fnic->fnic_num,
					 "ADISC not responding. Deleting target port: 0x%x",
					 tport->fcid);
		fdls_delete_tport(iport, tport);
		if ((iport->state == FNIC_IPORT_STATE_READY)
			&& (iport->fabric.state != FDLS_STATE_SEND_GPNFT)
			&& (iport->fabric.state != FDLS_STATE_RSCN_GPN_FT)) {
			fdls_send_gpn_ft(iport, FDLS_STATE_SEND_GPNFT);
		}
		/*Restart a discovery of targets */
		return;
	}

	/*This abort rsp is for PLOGI */
	if ((oxid >= FDLS_PLOGI_OXID_BASE) && (oxid < FDLS_PRLI_OXID_BASE)) {
		if (fchdr->r_ctl == FNIC_BA_ACC_RCTL) {
			FNIC_FCS_DBG(KERN_ERR, fnic->host, fnic->fnic_num,
				 "Received tgt PLOGI abts response BA_ACC tgt_fcid: 0x%x",
				 tport->fcid);
		} else if (fchdr->r_ctl == FNIC_BA_RJT_RCTL) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "PLOGI BA_RJT received for tport_fcid: 0x%x OX_ID: 0x%x",
				 tport->fcid, fchdr->ox_id);
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "reason code: 0x%x reason code explanation: 0x%x",
				  ba_rjt->reason_code, ba_rjt->reason_explanation);
		}
		if ((tport->retry_counter < iport->max_plogi_retries)
			&& (fchdr->r_ctl == FNIC_BA_ACC_RCTL)) {
			fdls_free_tgt_oxid(iport, oxid);
			fdls_send_tgt_plogi(iport, tport);
			return;
		}
		fdls_free_tgt_oxid(iport, oxid);
		fdls_delete_tport(iport, tport);
		/*Restart a discovery of targets */
		if ((iport->state == FNIC_IPORT_STATE_READY)
			&& (iport->fabric.state != FDLS_STATE_SEND_GPNFT)
			&& (iport->fabric.state != FDLS_STATE_RSCN_GPN_FT)) {
			fdls_send_gpn_ft(iport, FDLS_STATE_SEND_GPNFT);
		}
		return;
	}

	/*This abort rsp is for PRLI */
	if ((oxid >= FDLS_PRLI_OXID_BASE) && (oxid < FDLS_ADISC_OXID_BASE)) {
		if (fchdr->r_ctl == FNIC_BA_ACC_RCTL) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "0x%x: Received tgt PRLI abts response BA_ACC",
				 tport->fcid);
		} else if (fchdr->r_ctl == FNIC_BA_RJT_RCTL) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "PRLI BA_RJT received for tport_fcid: 0x%x OX_ID: 0x%x ",
				 tport->fcid, fchdr->ox_id);
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "reason code: 0x%x reason code explanation: 0x%x",
				 ba_rjt->reason_code,
				 ba_rjt->reason_explanation);
		}
		if ((tport->retry_counter < FDLS_RETRY_COUNT)
			&& (fchdr->r_ctl == FNIC_BA_ACC_RCTL)) {
			fdls_free_tgt_oxid(iport, oxid);
			fdls_send_tgt_prli(iport, tport);
			return;
		}
		fdls_free_tgt_oxid(iport, oxid);
		fdls_send_tgt_plogi(iport, tport);	/* go back to plogi */
		fdls_set_tport_state(tport, FDLS_TGT_STATE_PLOGI);
		return;
	}

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "Received ABTS response for unknown frame %p", iport);
}

static void
fdls_process_plogi_req(struct fnic_iport_s *iport, struct fc_hdr_s *fchdr)
{
	struct fc_els_reject_s plogi_rsp;
	uint16_t oxid;
	uint32_t d_id = ntoh24(fchdr->did);
	struct fnic *fnic = iport->fnic;

	memcpy(&plogi_rsp, &fnic_els_rjt, sizeof(struct fc_els_reject_s));

	if (iport->fcid != d_id) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "Received PLOGI with illegal frame bits. Dropping frame %p",
			 iport);
		return;
	}

	if (iport->state != FNIC_IPORT_STATE_READY) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "Received PLOGI request in iport state: %d Dropping frame",
			 iport->state);
		return;
	}
	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "Process PLOGI request from SID: 0x%x",
				 ntoh24(fchdr->sid));

	/* We don't support PLOGI request, send a reject */
	plogi_rsp.reason_code = 0x0B;
	plogi_rsp.reason_expl = 0x0;
	plogi_rsp.vendor_specific = 0x0;

	FNIC_SET_S_ID((&plogi_rsp.fchdr), fchdr->did);
	FNIC_SET_D_ID((&plogi_rsp.fchdr), fchdr->sid);

	oxid = FNIC_GET_OX_ID(fchdr);
	FNIC_SET_OX_ID((&plogi_rsp.fchdr), oxid);

	FNIC_SET_RX_ID((&plogi_rsp.fchdr), FNIC_PLOGI_RESP_OXID);
	fnic_send_fcoe_frame(iport, &plogi_rsp,
						 sizeof(struct fc_els_reject_s));
}

static void
fdls_process_logo_req(struct fnic_iport_s *iport, struct fc_hdr_s *fchdr)
{
	struct fc_logo_req_s *logo = (struct fc_logo_req_s *) fchdr;
	uint32_t nport_id;
	uint64_t nport_name;
	struct fnic_tport_s *tport;
	struct fnic *fnic = iport->fnic;
	uint16_t oxid;

	nport_id = ntoh24(logo->fcid);
	nport_name = logo->wwpn;

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "Process LOGO request from fcid: 0x%x", nport_id);

	if (iport->state != FNIC_IPORT_STATE_READY) {
		FNIC_FCS_DBG(KERN_ERR, fnic->host, fnic->fnic_num,
			 "Dropping LOGO req from 0x%x in iport state: %d",
			 nport_id, iport->state);
		return;
	}

	tport = fnic_find_tport_by_fcid(iport, nport_id);

	if (!tport) {
		/* We are not logged in with the nport, log and drop... */
		FNIC_FCS_DBG(KERN_ERR, fnic->host, fnic->fnic_num,
					 "Received LOGO from an nport not logged in: 0x%x",
					 nport_id);
		return;
	}
	if (tport->fcid != nport_id) {
		FNIC_FCS_DBG(KERN_ERR, fnic->host, fnic->fnic_num,
					 "Received LOGO with invalid target port fcid: 0x%x",
					 nport_id);
		return;
	}
	if (tport->timer_pending) {
		FNIC_FCS_DBG(KERN_ERR, fnic->host, fnic->fnic_num,
					 "tport fcid 0x%x: Canceling disc timer\n",
					 tport->fcid);
		fnic_del_tport_timer_sync();
		tport->timer_pending = 0;
	}

	/* got a logo in response to adisc to a target which has logged out */
	if (tport->state == FDLS_TGT_STATE_ADISC) {
		tport->retry_counter = 0;
		oxid = ntohs(tport->oxid_used);
		fdls_free_tgt_oxid(iport, oxid);
		fdls_delete_tport(iport, tport);
		fdls_send_logo_resp(iport, &logo->fchdr);
		if ((iport->state == FNIC_IPORT_STATE_READY)
			&& (fdls_get_state(&iport->fabric) != FDLS_STATE_SEND_GPNFT)
			&& (fdls_get_state(&iport->fabric) != FDLS_STATE_RSCN_GPN_FT)) {
			FNIC_FCS_DBG(KERN_ERR, fnic->host, fnic->fnic_num,
						 "Sending GPNFT in response to LOGO from Target:0x%x",
						 nport_id);
			fdls_send_gpn_ft(iport, FDLS_STATE_SEND_GPNFT);
			return;
		}
	} else {
		fdls_delete_tport(iport, tport);
	}
	if (iport->state == FNIC_IPORT_STATE_READY) {
		fdls_send_logo_resp(iport, &logo->fchdr);
		if ((fdls_get_state(&iport->fabric) != FDLS_STATE_SEND_GPNFT) &&
			(fdls_get_state(&iport->fabric) != FDLS_STATE_RSCN_GPN_FT)) {
			FNIC_FCS_DBG(KERN_ERR, fnic->host, fnic->fnic_num,
						 "Sending GPNFT in response to LOGO from Target:0x%x",
						 nport_id);
			fdls_send_gpn_ft(iport, FDLS_STATE_SEND_GPNFT);
		}
	}
}

static void
fdls_process_rscn(struct fnic_iport_s *iport, struct fc_hdr_s *fchdr)
{
	struct fc_rscn_hdr_s *rscn;
	struct fc_rscn_port_s *rscn_port = NULL;
	int num_ports;
	struct fnic_tport_s *tport, *next;
	uint32_t nport_id;
	uint8_t fcid[3];
	int newports = 0;
	struct fnic_fdls_fabric_s *fdls = &iport->fabric;
	struct fnic *fnic = iport->fnic;

	atomic64_inc(&iport->iport_stats.num_rscns);

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "FDLS process RSCN %p", iport);

	if (iport->state != FNIC_IPORT_STATE_READY) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "FDLS RSCN received in state(%d). Dropping",
					 fdls_get_state(fdls));
		return;
	}

	rscn = (struct fc_rscn_hdr_s *) fchdr;
	/* frame validation */
	if ((rscn->payload_len % 4 != 0) || (rscn->payload_len < 8)
		|| (rscn->payload_len > 1024) || (rscn->page_len != 4)) {
		num_ports = 0;
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "RSCN payload_len: 0x%x page_len: 0x%x",
					 rscn->payload_len, rscn->page_len);
		/* if this happens then we need to send ADISC to all the tports. */
		list_for_each_entry_safe(tport, next, &iport->tport_list, links) {
			if (tport->state == FDLS_TGT_STATE_READY)
				tport->flags |= FNIC_FDLS_TPORT_SEND_ADISC;
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
						 "RSCN for port id: 0x%x", tport->fcid);
		}
	} else {
		num_ports = (rscn->payload_len - 4) / rscn->page_len;
		rscn_port = (struct fc_rscn_port_s *) (rscn + 1);
	}
	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "RSCN received for num_ports: %d payload_len: %d page_len: %d ",
			 num_ports, rscn->payload_len, rscn->page_len);

	/*
	 * RSCN have at least one Port_ID page , but may not have any port_id
	 * in it. If no port_id is specified in the Port_ID page , we send
	 * ADISC to all the tports
	 */

	while (num_ports) {

		memcpy(fcid, rscn_port->port_id, 3);

		nport_id = ntoh24(fcid);
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "RSCN event: 0x%x for 0x%x", rscn_port->rscn_evt_q,
					 nport_id);
		rscn_port++;
		num_ports--;
		/* if this happens then we need to send ADISC to all the tports. */
		if (nport_id == 0) {
			list_for_each_entry_safe(tport, next, &iport->tport_list,
									 links) {
				if (tport->state == FDLS_TGT_STATE_READY)
					tport->flags |= FNIC_FDLS_TPORT_SEND_ADISC;

				FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
							 "RSCN for port id: 0x%x", tport->fcid);
			}
			break;
		}
		tport = fnic_find_tport_by_fcid(iport, nport_id);

		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "RSCN port id list: 0x%x", nport_id);

		if (!tport) {
			newports++;
			continue;
		}
		if (tport->state == FDLS_TGT_STATE_READY)
			tport->flags |= FNIC_FDLS_TPORT_SEND_ADISC;
	}

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "FDLS process RSCN sending GPN_FT %p", iport);
	fdls_send_gpn_ft(iport, FDLS_STATE_RSCN_GPN_FT);
	fdls_send_rscn_resp(iport, fchdr);
}

void fnic_fdls_disc_start(struct fnic_iport_s *iport)
{
	struct fnic *fnic = iport->fnic;

	if (IS_FNIC_FCP_INITIATOR(fnic)) {
		fc_host_fabric_name(iport->fnic->host) = 0;
		fc_host_post_event(iport->fnic->host, fc_get_event_number(),
						   FCH_EVT_LIPRESET, 0);
	}

	if (!iport->usefip) {
		if (iport->flags & FNIC_FIRST_LINK_UP) {
			spin_unlock_irqrestore(&fnic->fnic_lock, fnic->lock_flags);
			fnic_scsi_fcpio_reset(iport->fnic);
			spin_lock_irqsave(&fnic->fnic_lock, fnic->lock_flags);

			iport->flags &= ~FNIC_FIRST_LINK_UP;
		}
		fnic_fdls_start_flogi(iport);
	} else
		fnic_fdls_start_plogi(iport);
}

static void
fdls_process_adisc_req(struct fnic_iport_s *iport, struct fc_hdr_s *fchdr)
{
	struct fc_els_adisc_ls_acc_s adisc_acc;
	struct fc_els_adisc_s *adisc_req = (struct fc_els_adisc_s *) fchdr;
	uint64_t frame_wwnn;
	uint64_t frame_wwpn;
	uint32_t tgt_fcid;
	struct fnic_tport_s *tport;
	uint8_t *fcid;
	struct fc_els_reject_s rjts_rsp;
	uint16_t oxid;
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "Process ADISC request %d", iport->fnic->fnic_num);

	fcid = FNIC_GET_S_ID(fchdr);
	tgt_fcid = ntoh24(fcid);
	tport = fnic_find_tport_by_fcid(iport, tgt_fcid);
	if (!tport) {
		FNIC_FCS_DBG(KERN_ERR, fnic->host, fnic->fnic_num,
					 "tport for fcid: 0x%x not found. Dropping ADISC req.",
					 tgt_fcid);
		return;
	}
	if (iport->state != FNIC_IPORT_STATE_READY) {
		FNIC_FCS_DBG(KERN_ERR, fnic->host, fnic->fnic_num,
			 "Dropping ADISC req from fcid: 0x%x in iport state: %d",
			 tgt_fcid, iport->state);
		return;
	}

	frame_wwnn = ntohll(adisc_req->node_name);
	frame_wwpn = ntohll(adisc_req->nport_name);

	if ((frame_wwnn != tport->wwnn) || (frame_wwpn != tport->wwpn)) {
		/* send reject */
		FNIC_FCS_DBG(KERN_ERR, fnic->host, fnic->fnic_num,
			 "ADISC req from fcid: 0x%x mismatch wwpn: 0x%llx wwnn: 0x%llx",
			 tgt_fcid, frame_wwpn, frame_wwnn);
		FNIC_FCS_DBG(KERN_ERR, fnic->host, fnic->fnic_num,
			 "local tport wwpn: 0x%llx wwnn: 0x%llx. Sending RJT",
			 tport->wwpn, tport->wwnn);

		memcpy(&rjts_rsp, &fnic_els_rjt, sizeof(struct fc_els_reject_s));

		rjts_rsp.reason_code = 0x03;	/*  logical error */
		rjts_rsp.reason_expl = 0x1E;	/*  N_port login required */
		rjts_rsp.vendor_specific = 0x0;
		FNIC_SET_S_ID((&rjts_rsp.fchdr), fchdr->did);
		FNIC_SET_D_ID((&rjts_rsp.fchdr), fchdr->sid);
		oxid = FNIC_GET_OX_ID(fchdr);
		FNIC_SET_OX_ID((&rjts_rsp.fchdr), oxid);
		FNIC_SET_RX_ID((&rjts_rsp.fchdr), FNIC_ADISC_RESP_OXID);
		fnic_send_fcoe_frame(iport, &rjts_rsp,
							 sizeof(struct fc_els_reject_s));
		return;
	}
	memset(&adisc_acc.fchdr, 0, sizeof(struct fc_hdr_s));
	FNIC_SET_S_ID((&adisc_acc.fchdr), fchdr->did);
	FNIC_SET_D_ID((&adisc_acc.fchdr), fchdr->sid);
	adisc_acc.fchdr.f_ctl = FNIC_ELS_REP_FCTL;
	adisc_acc.fchdr.r_ctl = 0x23;
	adisc_acc.fchdr.type = 0x01;
	oxid = FNIC_GET_OX_ID(fchdr);
	FNIC_SET_OX_ID((&adisc_acc.fchdr), oxid);
	FNIC_SET_RX_ID((&adisc_acc.fchdr), FNIC_ADISC_RESP_OXID);
	adisc_acc.command = FC_LS_ACC;

	FNIC_SET_NPORT_NAME(adisc_acc, iport->wwpn);
	FNIC_SET_NODE_NAME(adisc_acc, iport->wwnn);
	memcpy(adisc_acc.fcid, fchdr->did, 3);
	fnic_send_fcoe_frame(iport, &adisc_acc,
						 sizeof(struct fc_els_adisc_ls_acc_s));
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
				FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
							 "invalid frame received. Dropping frame");
				return -1;
			}
		}

	/*  ABTS response */
	if ((fchdr->r_ctl == FNIC_BA_ACC_RCTL)
		|| (fchdr->r_ctl == FNIC_BA_RJT_RCTL)) {
		if (!(FNIC_FC_FRAME_TYPE_BLS(fchdr))) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
						 "Received ABTS invalid frame. Dropping frame");
			return -1;

		}
		return FNIC_BLS_ABTS_RSP;
	}
	if ((fchdr->r_ctl == FC_ABTS_RCTL) && (FNIC_FC_FRAME_TYPE_BLS(fchdr))) {
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
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
				FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
							 "Received LOGO invalid frame. Dropping frame");
				return -1;
			}
			return FNIC_ELS_LOGO_REQ;
		case FC_ELS_RSCN:
			if ((!FNIC_FC_FRAME_FCTL_FIRST_LAST_SEQINIT(fchdr))
				|| (!FNIC_FC_FRAME_TYPE_ELS(fchdr))
				|| (!FNIC_FC_FRAME_UNSOLICITED(fchdr))) {
				FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
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
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "Unsupported frame (type:0x%02x) from fcid: 0x%x",
				 type, s_id);
			return FNIC_ELS_UNSUPPORTED_REQ;
		}
	}

	/* ELS response from a target */
	if ((ntohs(oxid) >= FDLS_PLOGI_OXID_BASE)
		&& (ntohs(oxid) < FDLS_PRLI_OXID_BASE)) {
		if (!FNIC_FC_FRAME_TYPE_ELS(fchdr)) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				"Dropping Unknown frame in PLOGI exchange range type: 0x%x.",
				fchdr->type);
			return -1;
		}
		return FNIC_TPORT_PLOGI_RSP;
	}
	if ((ntohs(oxid) >= FDLS_PRLI_OXID_BASE)
		&& (ntohs(oxid) < FDLS_ADISC_OXID_BASE)) {
		if (!FNIC_FC_FRAME_TYPE_ELS(fchdr)) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				"Dropping Unknown frame in PRLI exchange range type: 0x%x.",
				fchdr->type);
			return -1;
		}
		return FNIC_TPORT_PRLI_RSP;
	}

	if ((ntohs(oxid) >= FDLS_ADISC_OXID_BASE)
		&& (ntohs(oxid) < FDLS_TGT_OXID_POOL_END)) {
		if (!FNIC_FC_FRAME_TYPE_ELS(fchdr)) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				"Dropping Unknown frame in ADISC exchange range type: 0x%x.",
				fchdr->type);
			return -1;
		}
		return FNIC_TPORT_ADISC_RSP;
	}
	if (ntohs(oxid) == FNIC_TLOGO_REQ_OXID) {
		return FNIC_TPORT_LOGO_RSP;
	}

	/*response from fabric */
	switch (oxid) {

	case FNIC_FLOGO_REQ_OXID:
		return FNIC_FABRIC_LOGO_RSP;

	case FNIC_FLOGI_OXID:
		if (type == FC_LS_ACC) {
			if ((s_id != FC_DOMAIN_CONTR)
				|| (!FNIC_FC_FRAME_TYPE_ELS(fchdr))) {
				FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "Received unknown frame. Dropping frame");
				return -1;
			}
		}
		return FNIC_FABRIC_FLOGI_RSP;

	case FNIC_PLOGI_FABRIC_OXID:
		if (type == FC_LS_ACC) {
			if ((s_id != FC_DIR_SERVER)
				|| (!FNIC_FC_FRAME_TYPE_ELS(fchdr))) {
				FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "Received unknown frame. Dropping frame");
				return -1;
			}
		}
		return FNIC_FABRIC_PLOGI_RSP;

	case FNIC_PLOGI_FDMI_OXID:
		return FNIC_FDMI_PLOGI_RSP;
	case FNIC_FDMI_REG_HBA_OXID:
	case FNIC_FDMI_RPA_OXID:
		return FNIC_FDMI_RSP;

	case FNIC_SCR_REQ_OXID:
		if (type == FC_LS_ACC) {
			if ((s_id != FC_FABRIC_CONTROLLER)
				|| (!FNIC_FC_FRAME_TYPE_ELS(fchdr))) {
				FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "Received unknown frame. Dropping frame");
				return -1;
			}
		}
		return FNIC_FABRIC_SCR_RSP;

	case FNIC_RPN_REQ_OXID:
		if ((s_id != FC_DIR_SERVER) || (!FNIC_FC_FRAME_TYPE_FC_GS(fchdr))) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "Received unknown frame. Dropping frame");
			return -1;
		}
		return FNIC_FABRIC_RPN_RSP;
	case FNIC_RFT_REQ_OXID:
		if ((s_id != FC_DIR_SERVER) || (!FNIC_FC_FRAME_TYPE_FC_GS(fchdr))) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "Received unknown frame. Dropping frame");
			return -1;
		}
		return FNIC_FABRIC_RFT_RSP;
	case FNIC_RFF_REQ_OXID:
		if ((s_id != FC_DIR_SERVER) || (!FNIC_FC_FRAME_TYPE_FC_GS(fchdr))) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "Received unknown frame. Dropping frame");
			return -1;
		}
		return FNIC_FABRIC_RFF_RSP;

	case FNIC_GPN_FT_OXID:
		if ((s_id != FC_DIR_SERVER) || (!FNIC_FC_FRAME_TYPE_FC_GS(fchdr))) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "Received unknown frame. Dropping frame");
			return -1;
		}
		return FNIC_FABRIC_GPN_FT_RSP;

	default:
		/* Drop the Rx frame and log/stats it */
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
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
	case FNIC_FDMI_PLOGI_RSP:
		fdls_process_fdmi_plogi_rsp(iport, fchdr);
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
	case FNIC_TPORT_PLOGI_RSP:
		fdls_process_tgt_plogi_rsp(iport, fchdr);
		break;
	case FNIC_TPORT_PRLI_RSP:
		fdls_process_tgt_prli_rsp(iport, fchdr);
		break;
	case FNIC_TPORT_ADISC_RSP:
		fdls_process_tgt_adisc_rsp(iport, fchdr);
		break;
	case FNIC_TPORT_LOGO_RSP:
		/* Logo response from tgt which we have deleted */
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
					 "Logo response from tgt: 0x%x",
					 ntoh24(fchdr->sid));
		break;
	case FNIC_FABRIC_LOGO_RSP:
		fdls_process_fabric_logo_rsp(iport, fchdr);
		break;

	case FNIC_BLS_ABTS_RSP:
		oxid = FNIC_GET_OX_ID(fchdr);
		if ((iport->fabric.flags & FNIC_FDLS_FABRIC_ABORT_ISSUED)
			&& (oxid >= FNIC_FLOGI_OXID && oxid <= FNIC_RFF_REQ_OXID)) {
			fdls_process_fabric_abts_rsp(iport, fchdr);
		} else
			fdls_process_tgt_abts_rsp(iport, fchdr);
		break;
	case FNIC_BLS_ABTS_REQ:
		fdls_process_abts_req(iport, fchdr);
		break;
	case FNIC_ELS_UNSUPPORTED_REQ:
		fdls_process_unsupported_els_req(iport, fchdr);
		break;
	case FNIC_ELS_PLOGI_REQ:
		fdls_process_plogi_req(iport, fchdr);
		break;
	case FNIC_ELS_RSCN_REQ:
		fdls_process_rscn(iport, fchdr);
		break;
	case FNIC_ELS_LOGO_REQ:
		fdls_process_logo_req(iport, fchdr);
		break;
	case FNIC_ELS_RRQ:
	case FNIC_ELS_ECHO_REQ:
		fdls_process_els_req(iport, fchdr, len);
		break;
	case FNIC_ELS_ADISC:
		fdls_process_adisc_req(iport, fchdr);
		break;
	case FNIC_ELS_RLS:
		fdls_process_rls_req(iport, fchdr);
		break;
	case FNIC_FDMI_RSP:
		fdls_process_fdmi_reg_ack(iport, fchdr);
		break;
	default:
		FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
			 "Received unknown FCoE frame of len: %d. Dropping frame", len);
		break;
	}
}


void fnic_fdls_disc_init(struct fnic_iport_s *iport)
{
	fdls_init_tgt_oxid_pool(iport);
	fdls_set_state((&iport->fabric), FDLS_STATE_INIT);
}

void fnic_fdls_link_down(struct fnic_iport_s *iport)
{
	struct fnic_tport_s *tport, *next;
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "0x%x: FDLS processing link down", iport->fcid);

	fdls_set_state((&iport->fabric), FDLS_STATE_LINKDOWN);
	iport->fabric.flags = 0;

	if (IS_FNIC_FCP_INITIATOR(fnic)) {
		spin_unlock_irqrestore(&fnic->fnic_lock, fnic->lock_flags);
		fnic_scsi_fcpio_reset(iport->fnic);
		spin_lock_irqsave(&fnic->fnic_lock, fnic->lock_flags);
		fdls_init_tgt_oxid_pool(iport);

		list_for_each_entry_safe(tport, next, &iport->tport_list, links) {
			FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
						 "removing rport: 0x%x", tport->fcid);
			fdls_delete_tport(iport, tport);
		}
	}

	if ((fnic_fdmi_support == 1) && (iport->fabric.fdmi_pending > 0)) {
		del_timer_sync(&iport->fabric.fdmi_timer);
		iport->fabric.fdmi_pending = 0;
	}

	FNIC_FCS_DBG(KERN_INFO, fnic->host, fnic->fnic_num,
				 "0x%x: FDLS finish processing link down", iport->fcid);
}

