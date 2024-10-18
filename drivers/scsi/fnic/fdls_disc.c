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
#include <scsi/scsi_transport_fc.h>
#include <linux/utsname.h>

#define FC_FC4_TYPE_SCSI 0x08
#define PORT_SPEED_BIT_8 8
#define PORT_SPEED_BIT_9 9
#define PORT_SPEED_BIT_14 14
#define PORT_SPEED_BIT_15 15

static void fdls_send_rpn_id(struct fnic_iport_s *iport);
static void fdls_fdmi_register_hba(struct fnic_iport_s *iport);
static void fdls_fdmi_register_pa(struct fnic_iport_s *iport);
#define FDLS_FDMI_PLOGI_PENDING 0x1
#define FDLS_FDMI_REG_HBA_PENDING 0x2
#define FDLS_FDMI_RPA_PENDING 0x4
#define FDLS_FDMI_ABORT_PENDING 0x8
#define FDLS_FDMI_MAX_RETRY 3

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
 * did, sid, oxid
 */
struct fc_std_els_prli fnic_std_prli_req = {
	.fchdr = {.fh_r_ctl = FC_RCTL_ELS_REQ, .fh_type = FC_TYPE_ELS,
		  .fh_f_ctl = {FNIC_ELS_REQ_FCTL, 0, 0}, .fh_rx_id = 0xFFFF},
	.els_prli = {.prli_cmd = ELS_PRLI,
		     .prli_spp_len = 16,
		     .prli_len = cpu_to_be16(0x14)},
	.sp = {.spp_type = 0x08, .spp_flags = 0x0020,
	       .spp_params = cpu_to_be32(0xA2)}
};

/*
 * Variables:
 * sid, port_id, port_name
 */
struct fc_std_fdmi_rhba fnic_std_fdmi_rhba = {
	.fchdr = {.fh_r_ctl = FC_RCTL_DD_UNSOL_CTL,
			.fh_d_id = {0xFF, 0XFF, 0XFA},
		  .fh_type = FC_TYPE_CT, .fh_f_ctl = {FNIC_ELS_REQ_FCTL, 0, 0},
		  .fh_rx_id = 0xFFFF},
	.fc_std_ct_hdr = {.ct_rev = FC_CT_REV, .ct_fs_type = FC_FST_MGMT,
			  .ct_fs_subtype = FC_FDMI_SUBTYPE,
			  .ct_cmd = cpu_to_be16(FC_FDMI_RHBA)},
	.num_ports = FNIC_FDMI_NUM_PORTS,
	.num_hba_attributes = FNIC_FDMI_NUM_HBA_ATTRS,
	.type_nn = FNIC_FDMI_TYPE_NODE_NAME,
	.length_nn = FNIC_FDMI_NN_LEN,
	.type_manu = FNIC_FDMI_TYPE_MANUFACTURER,
	.length_manu = FNIC_FDMI_MANU_LEN,
	.manufacturer = FNIC_FDMI_MANUFACTURER,
	.type_serial = FNIC_FDMI_TYPE_SERIAL_NUMBER,
	.length_serial = FNIC_FDMI_SERIAL_LEN,
	.type_model = FNIC_FDMI_TYPE_MODEL,
	.length_model = FNIC_FDMI_MODEL_LEN,
	.type_model_des = FNIC_FDMI_TYPE_MODEL_DES,
	.length_model_des = FNIC_FDMI_MODEL_DES_LEN,
	.model_description = FNIC_FDMI_MODEL_DESCRIPTION,
	.type_hw_ver = FNIC_FDMI_TYPE_HARDWARE_VERSION,
	.length_hw_ver = FNIC_FDMI_HW_VER_LEN,
	.type_dr_ver = FNIC_FDMI_TYPE_DRIVER_VERSION,
	.length_dr_ver = FNIC_FDMI_DR_VER_LEN,
	.type_rom_ver = FNIC_FDMI_TYPE_ROM_VERSION,
	.length_rom_ver = FNIC_FDMI_ROM_VER_LEN,
	.type_fw_ver = FNIC_FDMI_TYPE_FIRMWARE_VERSION,
	.length_fw_ver = FNIC_FDMI_FW_VER_LEN,
};

/*
 * Variables
 *sid, port_id, port_name
 */
struct fc_std_fdmi_rpa fnic_std_fdmi_rpa = {
	.fchdr = {.fh_r_ctl = FC_RCTL_DD_UNSOL_CTL,
			.fh_d_id = {0xFF, 0xFF, 0xFA},
		  .fh_type = FC_TYPE_CT, .fh_f_ctl = {FNIC_ELS_REQ_FCTL, 0, 0},
		  .fh_rx_id = 0xFFFF},
	.fc_std_ct_hdr = {.ct_rev = FC_CT_REV, .ct_fs_type = FC_FST_MGMT,
			  .ct_fs_subtype = FC_FDMI_SUBTYPE,
			  .ct_cmd = cpu_to_be16(FC_FDMI_RPA)},
	.num_port_attributes = FNIC_FDMI_NUM_PORT_ATTRS,
	.type_fc4 = FNIC_FDMI_TYPE_FC4_TYPES,
	.length_fc4 = FNIC_FDMI_FC4_LEN,
	.type_supp_speed = FNIC_FDMI_TYPE_SUPPORTED_SPEEDS,
	.length_supp_speed = FNIC_FDMI_SUPP_SPEED_LEN,
	.type_cur_speed = FNIC_FDMI_TYPE_CURRENT_SPEED,
	.length_cur_speed = FNIC_FDMI_CUR_SPEED_LEN,
	.type_max_frame_size = FNIC_FDMI_TYPE_MAX_FRAME_SIZE,
	.length_max_frame_size = FNIC_FDMI_MFS_LEN,
	.max_frame_size = FNIC_FDMI_MFS,
	.type_os_name = FNIC_FDMI_TYPE_OS_NAME,
	.length_os_name = FNIC_FDMI_OS_NAME_LEN,
	.type_host_name = FNIC_FDMI_TYPE_HOST_NAME,
	.length_host_name = FNIC_FDMI_HN_LEN,
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
 * did, ox_id, rx_id
 */
struct fc_std_els_rsp fnic_std_els_acc = {
	.fchdr = {.fh_r_ctl = FC_RCTL_ELS_REP, .fh_d_id = {0xFF, 0xFF, 0xFD},
		  .fh_type = FC_TYPE_ELS, .fh_f_ctl = {FNIC_ELS_REP_FCTL, 0, 0}},
	.u.acc.la_cmd = ELS_LS_ACC,
};

struct fc_std_els_rsp fnic_std_els_rjt = {
	.fchdr = {.fh_r_ctl = FC_RCTL_ELS_REP, .fh_type = FC_TYPE_ELS,
		  .fh_f_ctl = {FNIC_ELS_REP_FCTL, 0, 0}},
	.u.rej.er_cmd = ELS_LS_RJT,
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

struct fc_frame_header fc_std_tport_abts = {
	.fh_r_ctl = FC_RCTL_BA_ABTS,	/* ABTS */
	.fh_cs_ctl = 0x00, .fh_type = FC_TYPE_BLS,
	.fh_f_ctl = {FNIC_REQ_ABTS_FCTL, 0, 0}, .fh_seq_id = 0x00,
	.fh_df_ctl = 0x00, .fh_seq_cnt = 0x0000, .fh_rx_id = 0xFFFF,
	.fh_parm_offset = 0x00000000,	/* bit:0 = 0 Abort a exchange */
};

static struct fc_std_abts_ba_acc fnic_std_ba_acc = {
	.fchdr = {.fh_r_ctl = FC_RCTL_BA_ACC,
						.fh_f_ctl = {FNIC_FCP_RSP_FCTL, 0, 0}},
	.acc = {.ba_low_seq_cnt = 0, .ba_high_seq_cnt = 0xFFFF}
};

#define RETRIES_EXHAUSTED(iport)      \
	(iport->fabric.retry_counter == FABRIC_LOGO_MAX_RETRY)

#define FNIC_TPORT_MAX_NEXUS_RESTART (8)

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
static struct fnic_tport_s *fdls_create_tport(struct fnic_iport_s *iport,
									  uint32_t fcid,
									  uint64_t wwpn);
static void fdls_target_restart_nexus(struct fnic_tport_s *tport);
static void fdls_start_tport_timer(struct fnic_iport_s *iport,
					struct fnic_tport_s *tport, int timeout);
static void fdls_tport_timer_callback(struct timer_list *t);
static void fdls_send_fdmi_plogi(struct fnic_iport_s *iport);
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

	fdls_init_tgt_oxid_pool(iport);
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
	case FNIC_FDMI_PLOGI_RSP:
		oxid_pool->active_oxid_fdmi_plogi = oxid;
		break;
	case FNIC_FDMI_REG_HBA_RSP:
		oxid_pool->active_oxid_fdmi_rhba = oxid;
		break;
	case FNIC_FDMI_RPA_RSP:
		oxid_pool->active_oxid_fdmi_rpa = oxid;
		break;
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

static inline void fdls_schedule_fdmi_oxid_free(struct fnic_iport_s *iport)
{
	if (iport->fabric.fdmi_pending & FDLS_FDMI_PLOGI_PENDING)
		fdls_schedule_oxid_free(&iport->fdmi_oxid_pool.meta,
					iport->fdmi_oxid_pool.active_oxid_fdmi_plogi);

	if (iport->fabric.fdmi_pending & FDLS_FDMI_REG_HBA_PENDING)
		fdls_schedule_oxid_free(&iport->fdmi_oxid_pool.meta,
					iport->fdmi_oxid_pool.active_oxid_fdmi_rhba);

	if (iport->fabric.fdmi_pending & FDLS_FDMI_RPA_PENDING)
		fdls_schedule_oxid_free(&iport->fdmi_oxid_pool.meta,
					iport->fdmi_oxid_pool.active_oxid_fdmi_rpa);
}

static inline void fdls_schedule_tgt_oxid_free(struct fnic_iport_s *iport,
					       struct fnic_tgt_oxid_pool_s
					       *oxid_pool, uint16_t oxid)
{
	fdls_schedule_oxid_free(&oxid_pool->meta, oxid);
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

static int fdls_is_oxid_in_fdmi_range(uint16_t oxid)
{
	uint16_t oxid_unmasked = FDLS_OXID_RSP_TYPE_UNMASKED(oxid);

	return ((oxid_unmasked >= FDLS_FDMI_OXID_POOL_BASE) &&
		(oxid_unmasked <= FDLS_FDMI_OXID_POOL_END));
}

void fdls_init_tgt_oxid_pool(struct fnic_iport_s *iport)
{
	memset(&iport->plogi_oxid_pool, 0, sizeof(iport->plogi_oxid_pool));
	iport->plogi_oxid_pool.meta.oxid_base = FDLS_PLOGI_OXID_BASE;
	iport->plogi_oxid_pool.meta.sz = FDLS_TGT_OXID_BLOCK_SZ;
	INIT_LIST_HEAD(&iport->plogi_oxid_pool.meta.reclaim_list);

	memset(&iport->prli_oxid_pool, 0, sizeof(iport->prli_oxid_pool));
	iport->prli_oxid_pool.meta.oxid_base = FDLS_PRLI_OXID_BASE;
	iport->prli_oxid_pool.meta.sz = FDLS_TGT_OXID_BLOCK_SZ;
	INIT_LIST_HEAD(&iport->prli_oxid_pool.meta.reclaim_list);

	memset(&iport->adisc_oxid_pool, 0, sizeof(iport->adisc_oxid_pool));
	iport->adisc_oxid_pool.meta.oxid_base = FDLS_ADISC_OXID_BASE;
	iport->adisc_oxid_pool.meta.sz = FDLS_TGT_OXID_BLOCK_SZ;
	INIT_LIST_HEAD(&iport->adisc_oxid_pool.meta.reclaim_list);
}

inline uint16_t fdls_alloc_tgt_oxid(struct fnic_iport_s *iport,
				    struct fnic_tgt_oxid_pool_s *oxid_pool)
{
	uint16_t oxid;

	oxid = fdls_alloc_oxid(iport, &oxid_pool->meta, oxid_pool->bitmap);
	return oxid;
}

inline void fdls_free_tgt_oxid(struct fnic_iport_s *iport,
			       struct fnic_tgt_oxid_pool_s *oxid_pool,
			       uint16_t oxid)
{
	fdls_free_oxid(iport, &oxid_pool->meta, oxid_pool->bitmap, oxid);
}

static struct fnic_tgt_oxid_pool_s *fdls_get_tgt_oxid_pool(struct fnic_tport_s
							   *tport)
{
	struct fnic_iport_s *iport = (struct fnic_iport_s *)tport->iport;
	struct fnic_tgt_oxid_pool_s *oxid_pool = NULL;

	switch (tport->state) {
	case FDLS_TGT_STATE_PLOGI:
		oxid_pool = &iport->plogi_oxid_pool;
		break;
	case FDLS_TGT_STATE_PRLI:
		oxid_pool = &iport->prli_oxid_pool;
		break;
	case FDLS_TGT_STATE_ADISC:
		oxid_pool = &iport->adisc_oxid_pool;
		break;
	default:
		break;
	}
	return oxid_pool;
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

static void
fdls_start_tport_timer(struct fnic_iport_s *iport,
					   struct fnic_tport_s *tport, int timeout)
{
	u64 fabric_tov;
	struct fnic *fnic = iport->fnic;

	if (tport->timer_pending) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "tport fcid 0x%x: Canceling disc timer\n",
					 tport->fcid);
		fnic_del_tport_timer_sync(fnic, tport);
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
		    struct fc_frame_header *rscn_fchdr)
{
	struct fc_std_els_rsp els_acc;
	uint16_t oxid;
	uint8_t fcid[3];

	memcpy(&els_acc, &fnic_std_els_acc, FC_ELS_RSP_ACC_SIZE);

	hton24(fcid, iport->fcid);
	FNIC_STD_SET_S_ID((&els_acc.fchdr), fcid);
	FNIC_STD_SET_D_ID((&els_acc.fchdr), rscn_fchdr->fh_s_id);

	oxid = FNIC_STD_GET_OX_ID(rscn_fchdr);
	FNIC_STD_SET_OX_ID((&els_acc.fchdr), oxid);

	FNIC_STD_SET_RX_ID((&els_acc.fchdr), FNIC_UNASSIGNED_RXID);

	fnic_send_fcoe_frame(iport, &els_acc, FC_ELS_RSP_ACC_SIZE);
}

static void
fdls_send_logo_resp(struct fnic_iport_s *iport,
		    struct fc_frame_header *req_fchdr)
{
	struct fc_std_els_rsp logo_resp;
	uint16_t oxid;
	uint8_t fcid[3];

	memcpy(&logo_resp, &fnic_std_els_acc, sizeof(struct fc_std_els_rsp));

	hton24(fcid, iport->fcid);
	FNIC_STD_SET_S_ID((&logo_resp.fchdr), fcid);
	FNIC_STD_SET_D_ID((&logo_resp.fchdr), req_fchdr->fh_s_id);

	oxid = FNIC_STD_GET_OX_ID(req_fchdr);
	FNIC_STD_SET_OX_ID((&logo_resp.fchdr), oxid);

	FNIC_STD_SET_RX_ID((&logo_resp.fchdr), FNIC_UNASSIGNED_RXID);

	fnic_send_fcoe_frame(iport, &logo_resp, FC_ELS_RSP_ACC_SIZE);
}

void
fdls_send_tport_abts(struct fnic_iport_s *iport,
					 struct fnic_tport_s *tport)
{
	uint8_t s_id[3];
	uint8_t d_id[3];
	struct fnic *fnic = iport->fnic;
	struct fc_frame_header tport_abort = fc_std_tport_abts;
	struct fc_frame_header *tport_abts = &tport_abort;

	hton24(s_id, iport->fcid);
	hton24(d_id, tport->fcid);
	FNIC_STD_SET_S_ID(tport_abts, s_id);
	FNIC_STD_SET_D_ID(tport_abts, d_id);
	tport->flags |= FNIC_FDLS_TGT_ABORT_ISSUED;

	tport_abts->fh_ox_id = tport->oxid_used;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "FDLS sending tport abts: tport->state: %d ",
				 tport->state);

	fnic_send_fcoe_frame(iport, tport_abts, sizeof(struct fc_frame_header));
	/* Even if fnic_send_fcoe_frame() fails we want to retry after timeout */
	fdls_start_tport_timer(iport, tport, 2 * iport->e_d_tov);
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

static void fdls_send_fdmi_abts(struct fnic_iport_s *iport)
{
	uint8_t fcid[3];
	struct fc_frame_header fabric_abort = fc_std_fabric_abts;
	struct fc_frame_header *fabric_abts = &fabric_abort;
	struct fnic_fabric_oxid_pool_s *oxid_pool = &iport->fdmi_oxid_pool;
	int fdmi_tov;
	uint16_t oxid;

	hton24(fcid, 0XFFFFFA);

	if (iport->fabric.fdmi_pending & FDLS_FDMI_PLOGI_PENDING) {
		oxid = htons(oxid_pool->active_oxid_fdmi_plogi);
		FNIC_STD_SET_OX_ID(fabric_abts, oxid);
		fnic_send_fcoe_frame(iport, fabric_abts,
				     sizeof(struct fc_frame_header));
	} else {
		if (iport->fabric.fdmi_pending & FDLS_FDMI_REG_HBA_PENDING) {
			oxid = htons(oxid_pool->active_oxid_fdmi_rhba);
			FNIC_STD_SET_OX_ID(fabric_abts, oxid);
			fnic_send_fcoe_frame(iport, fabric_abts,
					     sizeof(struct fc_frame_header));
		}
		if (iport->fabric.fdmi_pending & FDLS_FDMI_RPA_PENDING) {
			oxid = htons(oxid_pool->active_oxid_fdmi_rpa);
			FNIC_STD_SET_OX_ID(fabric_abts, oxid);
			fnic_send_fcoe_frame(iport, fabric_abts,
					     sizeof(struct fc_frame_header));
		}
	}

	fdmi_tov = jiffies + msecs_to_jiffies(2 * iport->e_d_tov);
	mod_timer(&iport->fabric.fdmi_timer, round_jiffies(fdmi_tov));
	iport->fabric.fdmi_pending |= FDLS_FDMI_ABORT_PENDING;
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

static void fdls_send_fdmi_plogi(struct fnic_iport_s *iport)
{
	struct fc_std_flogi plogi;
	struct fc_frame_header *fchdr = &plogi.fchdr;
	uint8_t fcid[3];
	struct fnic *fnic = iport->fnic;
	u64 fdmi_tov;
	uint16_t oxid;

	memcpy(&plogi, &fnic_std_plogi_req, sizeof(plogi));

	oxid = fdls_alloc_fabric_oxid(iport, &iport->fdmi_oxid_pool,
				      FNIC_FDMI_PLOGI_RSP);
	if (oxid == 0xFFFF) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			     "Failed to allocate OXID to send fdmi plogi %p",
			     iport);
		return;
	}

	hton24(fcid, iport->fcid);

	FNIC_STD_SET_S_ID(fchdr, fcid);
	hton24(fcid, 0XFFFFFA);
	FNIC_STD_SET_D_ID(fchdr, fcid);
	FNIC_STD_SET_OX_ID(fchdr, htons(oxid));
	FNIC_LOGI_SET_NPORT_NAME(&plogi.els, iport->wwpn);
	FNIC_LOGI_SET_NODE_NAME(&plogi.els, iport->wwnn);
	FNIC_LOGI_SET_RDF_SIZE(&plogi.els, iport->max_payload_size);

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
		     "fcid: 0x%x: FDLS send FDMI PLOGI with oxid:%x",
		     iport->fcid, oxid);

	fnic_send_fcoe_frame(iport, &plogi, sizeof(struct fc_std_flogi));

	fdmi_tov = jiffies + msecs_to_jiffies(2 * iport->e_d_tov);
	mod_timer(&iport->fabric.fdmi_timer, round_jiffies(fdmi_tov));
	iport->fabric.fdmi_pending = FDLS_FDMI_PLOGI_PENDING;
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

static void
fdls_send_tgt_adisc(struct fnic_iport_s *iport, struct fnic_tport_s *tport)
{
	struct fc_std_els_adisc adisc;
	uint8_t s_id[3];
	uint8_t d_id[3];
	uint16_t oxid;
	struct fnic *fnic = iport->fnic;

	memset(&adisc, 0, sizeof(struct fc_std_els_adisc));
	FNIC_STD_SET_R_CTL(&adisc.fchdr, 0x22);
	FNIC_STD_SET_TYPE(&adisc.fchdr, 0x01);
	FNIC_STD_SET_F_CTL(&adisc.fchdr, FNIC_ELS_REQ_FCTL << 16);
	FNIC_STD_SET_RX_ID(&adisc.fchdr, cpu_to_be16(0xFFFF));

	hton24(s_id, iport->fcid);
	hton24(d_id, tport->fcid);
	FNIC_STD_SET_S_ID(&adisc.fchdr, s_id);
	FNIC_STD_SET_D_ID(&adisc.fchdr, d_id);

	oxid = htons(fdls_alloc_tgt_oxid(iport, &iport->adisc_oxid_pool));
	if (oxid == 0xFFFF) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Failed to allocate OXID to send ADISC %p", iport);
		return;
	}

	tport->oxid_used = oxid;
	tport->flags &= ~FNIC_FDLS_TGT_ABORT_ISSUED;

	FNIC_STD_SET_OX_ID((&adisc.fchdr), oxid);
	FNIC_STD_SET_NPORT_NAME(&adisc.els.adisc_wwpn,
				le64_to_cpu(iport->wwpn));
	FNIC_STD_SET_NODE_NAME(&adisc.els.adisc_wwnn, le64_to_cpu(iport->wwnn));

	memcpy(adisc.els.adisc_port_id, s_id, 3);
	adisc.els.adisc_cmd = ELS_ADISC;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "sending ADISC to tgt fcid: 0x%x", tport->fcid);


	fnic_send_fcoe_frame(iport, &adisc, sizeof(struct fc_std_els_adisc));
	/* Even if fnic_send_fcoe_frame() fails we want to retry after timeout */
	fdls_start_tport_timer(iport, tport, 2 * iport->e_d_tov);
}

bool fdls_delete_tport(struct fnic_iport_s *iport, struct fnic_tport_s *tport)
{
	struct fnic_tport_event_s *tport_del_evt;
	struct fnic *fnic = iport->fnic;

	if ((tport->state == FDLS_TGT_STATE_OFFLINING)
	    || (tport->state == FDLS_TGT_STATE_OFFLINE)) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			     "tport fcid 0x%x: tport state is offlining/offline\n",
			     tport->fcid);
		return false;
	}

	fdls_set_tport_state(tport, FDLS_TGT_STATE_OFFLINING);
	/*
	 * By setting this flag, the tport will not be seen in a look-up
	 * in an RSCN. Even if we move to multithreaded model, this tport
	 * will be destroyed and a new RSCN will have to create a new one
	 */
	tport->flags |= FNIC_FDLS_TPORT_TERMINATING;

	if (tport->timer_pending) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "tport fcid 0x%x: Canceling disc timer\n",
					 tport->fcid);
		fnic_del_tport_timer_sync(fnic, tport);
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
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Failed to allocate memory for tport fcid: 0x%0x\n",
					 tport->fcid);
				return false;
			}
			tport_del_evt->event = TGT_EV_RPORT_DEL;
			tport_del_evt->arg1 = (void *) tport;
			list_add_tail(&tport_del_evt->links, &fnic->tport_event_list);
			queue_work(fnic_event_queue, &fnic->tport_work);
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "tport 0x%x not reg with scsi_transport. Freeing locally",
				 tport->fcid);
			list_del(&tport->links);
			kfree(tport);
		}
	}
	return true;
}

static void
fdls_send_tgt_plogi(struct fnic_iport_s *iport, struct fnic_tport_s *tport)
{
	struct fc_std_flogi plogi;
	uint8_t s_id[3];
	uint8_t d_id[3];
	uint16_t oxid;
	struct fnic *fnic = iport->fnic;
	uint32_t timeout;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Send tgt PLOGI to fcid: 0x%x", tport->fcid);

	memcpy(&plogi, &fnic_std_plogi_req, sizeof(struct fc_std_flogi));

	hton24(s_id, iport->fcid);
	hton24(d_id, tport->fcid);

	FNIC_STD_SET_S_ID(&plogi.fchdr, s_id);
	FNIC_STD_SET_D_ID(&plogi.fchdr, d_id);
	FNIC_LOGI_SET_RDF_SIZE(&plogi.els, iport->max_payload_size);

	oxid = htons(fdls_alloc_tgt_oxid(iport, &iport->plogi_oxid_pool));
	if (oxid == 0xFFFF) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "0x%x: Failed to allocate oxid to send PLOGI to fcid: 0x%x",
				 iport->fcid, tport->fcid);
		return;
	}

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "send tgt PLOGI: tgt fcid: 0x%x oxid: 0x%x", tport->fcid,
				 ntohs(oxid));
	tport->oxid_used = oxid;
	tport->flags &= ~FNIC_FDLS_TGT_ABORT_ISSUED;

	FNIC_STD_SET_OX_ID((&plogi.fchdr), oxid);
	FNIC_LOGI_SET_NPORT_NAME(&plogi.els, iport->wwpn);
	FNIC_LOGI_SET_NODE_NAME(&plogi.els, iport->wwnn);

	timeout = max(2 * iport->e_d_tov, iport->plogi_timeout);


	fnic_send_fcoe_frame(iport, &plogi, sizeof(struct fc_std_flogi));
	/* Even if fnic_send_fcoe_frame() fails we want to retry after timeout */
	fdls_start_tport_timer(iport, tport, timeout);
}

static uint16_t
fnic_fc_plogi_rsp_rdf(struct fnic_iport_s *iport,
		      struct fc_std_flogi *plogi_rsp)
{
	uint16_t b2b_rdf_size =
	    be16_to_cpu(FNIC_LOGI_RDF_SIZE(&plogi_rsp->els));
	uint16_t spc3_rdf_size =
	    be16_to_cpu(plogi_rsp->els.fl_cssp[2].cp_rdfs) & FNIC_FC_C3_RDF;
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "MFS: b2b_rdf_size: 0x%x spc3_rdf_size: 0x%x",
			 b2b_rdf_size, spc3_rdf_size);

	return MIN(b2b_rdf_size, spc3_rdf_size);
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

static void
fdls_send_tgt_prli(struct fnic_iport_s *iport, struct fnic_tport_s *tport)
{
	struct fc_std_els_prli prli;
	uint8_t s_id[3];
	uint8_t d_id[3];
	uint16_t oxid;
	struct fnic *fnic = iport->fnic;
	uint32_t timeout;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "FDLS sending PRLI to tgt: 0x%x", tport->fcid);

	oxid = htons(fdls_alloc_tgt_oxid(iport, &iport->prli_oxid_pool));
	if (oxid == 0xFFFF) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Failed to allocate OXID to send PRLI %p", iport);
		return;
	}
	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "FDLS sending PRLI to tgt: 0x%x OXID: 0x%x", tport->fcid,
				 ntohs(oxid));

	tport->oxid_used = oxid;
	tport->flags &= ~FNIC_FDLS_TGT_ABORT_ISSUED;
	memcpy(&prli, &fnic_std_prli_req, sizeof(struct fc_std_els_prli));

	hton24(s_id, iport->fcid);
	hton24(d_id, tport->fcid);

	FNIC_STD_SET_S_ID((&prli.fchdr), s_id);
	FNIC_STD_SET_D_ID((&prli.fchdr), d_id);
	FNIC_STD_SET_OX_ID((&prli.fchdr), oxid);

	timeout = max(2 * iport->e_d_tov, iport->plogi_timeout);

	fnic_send_fcoe_frame(iport, &prli, sizeof(struct fc_std_els_prli));
	/* Even if fnic_send_fcoe_frame() fails we want to retry after timeout */
	fdls_start_tport_timer(iport, tport, timeout);
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

/**
 * fdls_tgt_logout - Send plogo to the remote port
 * @iport: Handle to fnic iport
 * @tport: Handle to remote port
 *
 * This function does not change or check the fabric/tport state.
 * It the caller's responsibility to set the appropriate tport/fabric
 * state when this is called. Normally that is fdls_tgt_state_plogo.
 * This could be used to send plogo to nameserver process
 * also not just target processes
 */
void fdls_tgt_logout(struct fnic_iport_s *iport, struct fnic_tport_s *tport)
{
	struct fc_std_logo logo;
	uint8_t s_id[3];
	uint8_t d_id[3];
	struct fnic *fnic = iport->fnic;
	uint16_t oxid;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Sending logo to tport fcid: 0x%x", tport->fcid);
	memcpy(&logo, &fnic_std_logo_req, sizeof(struct fc_std_logo));

	hton24(s_id, iport->fcid);
	hton24(d_id, tport->fcid);

	FNIC_STD_SET_S_ID((&logo.fchdr), s_id);
	FNIC_STD_SET_D_ID((&logo.fchdr), d_id);

	oxid = htons(fdls_alloc_tgt_oxid(iport, &iport->plogi_oxid_pool));
	FNIC_STD_SET_OX_ID((&logo.fchdr), oxid);

	memcpy(&logo.els.fl_n_port_id, s_id, 3);
	FNIC_STD_SET_NPORT_NAME(&logo.els.fl_n_port_wwn,
				le64_to_cpu(iport->wwpn));


	fnic_send_fcoe_frame(iport, &logo, sizeof(struct fc_std_logo));
}

static void fdls_tgt_discovery_start(struct fnic_iport_s *iport)
{
	struct fnic_tport_s *tport, *next;
	u32 old_link_down_cnt = iport->fnic->link_down_cnt;
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
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
	bool retval = true;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "tport fcid: 0x%x state: %d restart_count: %d",
				 tport->fcid, tport->state, tport->nexus_restart_count);

	fcid = tport->fcid;
	wwpn = tport->wwpn;
	nexus_restart_count = tport->nexus_restart_count;

	retval = fdls_delete_tport(iport, tport);
	if (retval != true) {
		FNIC_FCS_DBG(KERN_ERR, fnic->lport->host, fnic->fnic_num,
			     "Error deleting tport: 0x%x", fcid);
		return;
	}

	if (nexus_restart_count >= FNIC_TPORT_MAX_NEXUS_RESTART) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			     "Exceeded nexus restart retries tport: 0x%x",
			     fcid);
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
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
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

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "FDLS create tport: fcid: 0x%x wwpn: 0x%llx", fcid, wwpn);

	tport = kzalloc(sizeof(struct fnic_tport_s), GFP_ATOMIC);
	if (!tport) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
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

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Need to setup tport timer callback");

	timer_setup(&tport->retry_timer, fdls_tport_timer_callback, 0);

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
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
	struct fc_std_fdmi_rhba fdmi_rhba;
	uint8_t fcid[3];
	uint16_t len;
	int err;
	struct fnic *fnic = iport->fnic;
	struct vnic_devcmd_fw_info *fw_info = NULL;
	uint16_t oxid;

	memcpy(&fdmi_rhba, &fnic_std_fdmi_rhba,
	       sizeof(struct fc_std_fdmi_rhba));

	hton24(fcid, iport->fcid);
	FNIC_STD_SET_S_ID((&fdmi_rhba.fchdr), fcid);

	oxid = fdls_alloc_fabric_oxid(iport, &iport->fdmi_oxid_pool,
				      FNIC_FDMI_REG_HBA_RSP);
	if (oxid == 0xFFFF) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			     "Failed to allocate OXID to send fdmi reg hba %p",
			     iport);
		return;
	}
	FNIC_STD_SET_OX_ID(&fdmi_rhba.fchdr, htons(oxid));

	fdmi_rhba.hba_identifier = get_unaligned_be64(&iport->wwpn);
	fdmi_rhba.port_name = get_unaligned_be64(&iport->wwpn);
	fdmi_rhba.node_name = get_unaligned_be64(&iport->wwnn);

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
		memcpy(&fdmi_rhba.model, fnic->subsys_desc,
		       fnic->subsys_desc_len);
		fdmi_rhba.model[fnic->subsys_desc_len] = 0x00;
	}

	snprintf(fdmi_rhba.driver_ver, sizeof(fdmi_rhba.driver_ver) - 1, "%s",
			 DRV_VERSION);
	snprintf(fdmi_rhba.rom_ver, sizeof(fdmi_rhba.rom_ver) - 1, "%s", "N/A");

	fnic_send_fcoe_frame(iport, &fdmi_rhba,
			     sizeof(struct fc_std_fdmi_rhba));
	iport->fabric.fdmi_pending |= FDLS_FDMI_REG_HBA_PENDING;
}

static void fdls_fdmi_register_pa(struct fnic_iport_s *iport)
{
	struct fc_std_fdmi_rpa fdmi_rpa;

	uint8_t fcid[3];
	struct fnic *fnic = iport->fnic;
	u32 port_speed_bm;
	u32 port_speed = vnic_dev_port_speed(fnic->vdev);
	uint16_t oxid;

	memcpy(&fdmi_rpa, &fnic_std_fdmi_rpa, sizeof(struct fc_std_fdmi_rpa));
	hton24(fcid, iport->fcid);
	FNIC_STD_SET_S_ID((&fdmi_rpa.fchdr), fcid);

	oxid = fdls_alloc_fabric_oxid(iport, &iport->fdmi_oxid_pool,
				      FNIC_FDMI_RPA_RSP);
	if (oxid == 0xFFFF) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			     "Failed to allocate OXID to send fdmi rpa %p",
			     iport);
		return;
	}
	FNIC_STD_SET_OX_ID(&fdmi_rpa.fchdr, htons(oxid));

	fdmi_rpa.port_name = get_unaligned_be64(&iport->wwpn);

	/* MDS does not support GIGE speed.
	 * Bit shift standard definitions from scsi_transport_fc.h to
	 * match FC spec.
	 */
	switch (port_speed) {
	case DCEM_PORTSPEED_10G:
	case DCEM_PORTSPEED_20G:
		/* There is no bit for 20G */
		port_speed_bm = FC_PORTSPEED_10GBIT << PORT_SPEED_BIT_14;
		break;
	case DCEM_PORTSPEED_25G:
		port_speed_bm = FC_PORTSPEED_25GBIT << PORT_SPEED_BIT_8;
		break;
	case DCEM_PORTSPEED_40G:
	case DCEM_PORTSPEED_4x10G:
		port_speed_bm = FC_PORTSPEED_40GBIT << PORT_SPEED_BIT_9;
		break;
	case DCEM_PORTSPEED_100G:
		port_speed_bm = FC_PORTSPEED_100GBIT << PORT_SPEED_BIT_8;
		break;
	default:
		port_speed_bm = FC_PORTSPEED_1GBIT << PORT_SPEED_BIT_15;
		break;
	}
	fdmi_rpa.supported_speed = htonl(port_speed_bm);
	fdmi_rpa.current_speed = htonl(port_speed_bm);
	fdmi_rpa.fc4_type[2] = 1;
	snprintf(fdmi_rpa.os_name, sizeof(fdmi_rpa.os_name) - 1, "host%d",
		 fnic->lport->host->host_no);
	sprintf(fc_host_system_hostname(fnic->lport->host), "%s", utsname()->nodename);
	snprintf(fdmi_rpa.host_name, sizeof(fdmi_rpa.host_name) - 1, "%s",
		 fc_host_system_hostname(fnic->lport->host));

	fnic_send_fcoe_frame(iport, &fdmi_rpa, sizeof(struct fc_std_fdmi_rpa));
	iport->fabric.fdmi_pending |= FDLS_FDMI_RPA_PENDING;
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
	unsigned long flags;

	spin_lock_irqsave(&fnic->fnic_lock, flags);

	if (!iport->fabric.fdmi_pending) {
		/* timer expired after fdmi responses received. */
		spin_unlock_irqrestore(&fnic->fnic_lock, flags);
		return;
	}

	/* if not abort pending, send an abort */
	if (!(iport->fabric.fdmi_pending & FDLS_FDMI_ABORT_PENDING)) {
		fdls_send_fdmi_abts(iport);
		spin_unlock_irqrestore(&fnic->fnic_lock, flags);
		return;
	}

	/* Abort timed out */
	fdls_schedule_fdmi_oxid_free(iport);

	iport->fabric.fdmi_pending = 0;
	/* If max retries not exhaused, start over from fdmi plogi */
	if (iport->fabric.fdmi_retry < FDLS_FDMI_MAX_RETRY) {
		iport->fabric.fdmi_retry++;
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "retry fdmi timer %d", iport->fabric.fdmi_retry);
		fdls_send_fdmi_plogi(iport);
	}
	spin_unlock_irqrestore(&fnic->fnic_lock, flags);
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
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
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
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "tport_del_timer inprogress. Skip timer cb tport fcid: 0x%x\n",
			 tport->fcid);
		return;
	}

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
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
			tport->flags &= ~FNIC_FDLS_RETRY_FRAME;
			fdls_send_tgt_plogi(iport, tport);
		} else if (!(tport->flags & FNIC_FDLS_TGT_ABORT_ISSUED)) {
			/* Plogi frame has timed out, send abts */
			fdls_send_tport_abts(iport, tport);
		} else if (tport->retry_counter < iport->max_plogi_retries) {
			/*
			 * ABTS has timed out
			 */
			fdls_schedule_tgt_oxid_free(iport,
						    &iport->plogi_oxid_pool,
						    oxid);
			fdls_send_tgt_plogi(iport, tport);
		} else {
			/* exceeded plogi retry count */
			fdls_schedule_tgt_oxid_free(iport,
						    &iport->plogi_oxid_pool,
						    oxid);
			fdls_send_delete_tport_msg(tport);
		}
		break;
	case FDLS_TGT_STATE_PRLI:
		/* PRLI received a LS_RJT with busy , hence we retry from here */
		if ((tport->flags & FNIC_FDLS_RETRY_FRAME)
			&& (tport->retry_counter < FDLS_RETRY_COUNT)) {
			tport->flags &= ~FNIC_FDLS_RETRY_FRAME;
			fdls_send_tgt_prli(iport, tport);
		} else if (!(tport->flags & FNIC_FDLS_TGT_ABORT_ISSUED)) {
			/* PRLI has time out, send abts */
			fdls_send_tport_abts(iport, tport);
		} else {
			/* ABTS has timed out for prli, we go back to PLOGI */
			fdls_schedule_tgt_oxid_free(iport,
						    &iport->prli_oxid_pool,
						    oxid);
			fdls_send_tgt_plogi(iport, tport);
			fdls_set_tport_state(tport, FDLS_TGT_STATE_PLOGI);
		}
		break;
	case FDLS_TGT_STATE_ADISC:
		/* ADISC timed out send an ABTS */
		if (!(tport->flags & FNIC_FDLS_TGT_ABORT_ISSUED)) {
			fdls_send_tport_abts(iport, tport);
		} else if ((tport->flags & FNIC_FDLS_TGT_ABORT_ISSUED)
				   && (tport->retry_counter < FDLS_RETRY_COUNT)) {
			/*
			 * ABTS has timed out
			 */
			fdls_schedule_tgt_oxid_free(iport,
						    &iport->adisc_oxid_pool,
						    oxid);
			fdls_send_tgt_adisc(iport, tport);
		} else {
			/* exceeded retry count */
			fdls_schedule_tgt_oxid_free(iport,
						    &iport->adisc_oxid_pool,
						    oxid);
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "ADISC not responding. Deleting target port: 0x%x",
					 tport->fcid);
			fdls_send_delete_tport_msg(tport);
		}
		break;
	default:
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
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
			   struct fc_frame_header *fchdr)
{
	uint32_t tgt_fcid;
	struct fnic_tport_s *tport;
	uint8_t *fcid;
	uint64_t frame_wwnn;
	uint64_t frame_wwpn;
	uint16_t oxid;
	struct fc_std_els_adisc *adisc_rsp = (struct fc_std_els_adisc *)fchdr;
	struct fc_std_els_rsp *els_rjt = (struct fc_std_els_rsp *)fchdr;
	struct fnic *fnic = iport->fnic;

	fcid = FNIC_STD_GET_S_ID(fchdr);
	tgt_fcid = ntoh24(fcid);
	tport = fnic_find_tport_by_fcid(iport, tgt_fcid);

	if (!tport) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Tgt ADISC response tport not found: 0x%x", tgt_fcid);
		return;
	}
	if ((iport->state != FNIC_IPORT_STATE_READY)
		|| (tport->state != FDLS_TGT_STATE_ADISC)
		|| (tport->flags & FNIC_FDLS_TGT_ABORT_ISSUED)) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Dropping this ADISC response");
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "iport state: %d tport state: %d Is abort issued on PRLI? %d",
			 iport->state, tport->state,
			 (tport->flags & FNIC_FDLS_TGT_ABORT_ISSUED));
		return;
	}
	if (ntohs(fchdr->fh_ox_id) != ntohs(tport->oxid_used)) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "Dropping frame from target: 0x%x",
			 tgt_fcid);
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "Reason: Stale ADISC/Aborted ADISC/OOO frame delivery");
		return;
	}

	oxid = ntohs(FNIC_STD_GET_OX_ID(fchdr));
	fdls_free_tgt_oxid(iport, &iport->adisc_oxid_pool, oxid);

	switch (adisc_rsp->els.adisc_cmd) {
	case ELS_LS_ACC:
		if (tport->timer_pending) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "tport 0x%p Canceling fabric disc timer\n",
						 tport);
			fnic_del_tport_timer_sync(fnic, tport);
		}
		tport->timer_pending = 0;
		tport->retry_counter = 0;
		frame_wwnn = get_unaligned_be64(&adisc_rsp->els.adisc_wwnn);
		frame_wwpn = get_unaligned_be64(&adisc_rsp->els.adisc_wwpn);
		if ((frame_wwnn == tport->wwnn) && (frame_wwpn == tport->wwpn)) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "ADISC accepted from target: 0x%x. Target logged in",
				 tgt_fcid);
			fdls_set_tport_state(tport, FDLS_TGT_STATE_READY);
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "Error mismatch frame: ADISC");
		}
		break;

	case ELS_LS_RJT:
		if (((els_rjt->u.rej.er_reason == ELS_RJT_BUSY)
		     || (els_rjt->u.rej.er_reason == ELS_RJT_UNAB))
			&& (tport->retry_counter < FDLS_RETRY_COUNT)) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "ADISC ret ELS_LS_RJT BUSY. Retry from timer routine: 0x%x",
				 tgt_fcid);

			/* Retry ADISC again from the timer routine. */
			tport->flags |= FNIC_FDLS_RETRY_FRAME;
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "ADISC returned ELS_LS_RJT from target: 0x%x",
						 tgt_fcid);
			fdls_delete_tport(iport, tport);
		}
		break;
	}
}


static void
fdls_process_tgt_plogi_rsp(struct fnic_iport_s *iport,
			   struct fc_frame_header *fchdr)
{
	uint32_t tgt_fcid;
	struct fnic_tport_s *tport;
	uint8_t *fcid;
	uint16_t oxid;
	struct fc_std_flogi *plogi_rsp = (struct fc_std_flogi *)fchdr;
	struct fc_std_els_rsp *els_rjt = (struct fc_std_els_rsp *)fchdr;
	int max_payload_size;
	struct fnic *fnic = iport->fnic;

	fcid = FNIC_STD_GET_S_ID(fchdr);
	tgt_fcid = ntoh24(fcid);

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "FDLS processing target PLOGI response: tgt_fcid: 0x%x",
				 tgt_fcid);

	tport = fnic_find_tport_by_fcid(iport, tgt_fcid);
	if (!tport) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "tport not found: 0x%x", tgt_fcid);
		return;
	}
	if ((iport->state != FNIC_IPORT_STATE_READY)
		|| (tport->flags & FNIC_FDLS_TGT_ABORT_ISSUED)) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Dropping frame! iport state: %d tport state: %d",
					 iport->state, tport->state);
		return;
	}

	if (tport->state != FDLS_TGT_STATE_PLOGI) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			     "PLOGI rsp recvd in wrong state. Drop the frame and restart nexus");
		fdls_target_restart_nexus(tport);
		return;
	}

	if (fchdr->fh_ox_id != tport->oxid_used) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "PLOGI response from target: 0x%x. Dropping frame",
			 tgt_fcid);
		return;
	}

	oxid = ntohs(FNIC_STD_GET_OX_ID(fchdr));
	fdls_free_tgt_oxid(iport, &iport->plogi_oxid_pool, oxid);

	switch (plogi_rsp->els.fl_cmd) {
	case ELS_LS_ACC:
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "PLOGI accepted by target: 0x%x", tgt_fcid);
		break;

	case ELS_LS_RJT:
		if (((els_rjt->u.rej.er_reason == ELS_RJT_BUSY)
		     || (els_rjt->u.rej.er_reason == ELS_RJT_UNAB))
			&& (tport->retry_counter < iport->max_plogi_retries)) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "PLOGI ret ELS_LS_RJT BUSY. Retry from timer routine: 0x%x",
				 tgt_fcid);
			/* Retry plogi again from the timer routine. */
			tport->flags |= FNIC_FDLS_RETRY_FRAME;
			return;
		}
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "PLOGI returned ELS_LS_RJT from target: 0x%x",
					 tgt_fcid);
		fdls_delete_tport(iport, tport);
		return;

	default:
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "PLOGI not accepted from target fcid: 0x%x",
					 tgt_fcid);
		return;
	}

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Found the PLOGI target: 0x%x and state: %d",
				 (unsigned int) tgt_fcid, tport->state);

	if (tport->timer_pending) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "tport fcid 0x%x: Canceling disc timer\n",
					 tport->fcid);
		fnic_del_tport_timer_sync(fnic, tport);
	}

	tport->timer_pending = 0;
	tport->wwpn = get_unaligned_be64(&FNIC_LOGI_PORT_NAME(&plogi_rsp->els));
	tport->wwnn = get_unaligned_be64(&FNIC_LOGI_NODE_NAME(&plogi_rsp->els));

	/* Learn the Service Params */

	/* Max frame size - choose the lowest */
	max_payload_size = fnic_fc_plogi_rsp_rdf(iport, plogi_rsp);
	tport->max_payload_size =
		MIN(max_payload_size, iport->max_payload_size);

	if (tport->max_payload_size < FNIC_MIN_DATA_FIELD_SIZE) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "MFS: tport max frame size below spec bounds: %d",
			 tport->max_payload_size);
		tport->max_payload_size = FNIC_MIN_DATA_FIELD_SIZE;
	}

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
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
			  struct fc_frame_header *fchdr)
{
	uint32_t tgt_fcid;
	struct fnic_tport_s *tport;
	uint8_t *fcid;
	uint16_t oxid;
	struct fc_std_els_prli *prli_rsp = (struct fc_std_els_prli *)fchdr;
	struct fc_std_els_rsp *els_rjt = (struct fc_std_els_rsp *)fchdr;
	struct fnic_tport_event_s *tport_add_evt;
	struct fnic *fnic = iport->fnic;
	bool mismatched_tgt = false;

	fcid = FNIC_STD_GET_S_ID(fchdr);
	tgt_fcid = ntoh24(fcid);

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "FDLS process tgt PRLI response: 0x%x", tgt_fcid);

	tport = fnic_find_tport_by_fcid(iport, tgt_fcid);
	if (!tport) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "tport not found: 0x%x", tgt_fcid);
		/* Handle or just drop? */
		return;
	}

	if ((iport->state != FNIC_IPORT_STATE_READY)
		|| (tport->flags & FNIC_FDLS_TGT_ABORT_ISSUED)) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "Dropping frame! iport st: %d tport st: %d tport fcid: 0x%x",
			 iport->state, tport->state, tport->fcid);
		return;
	}

	if (tport->state != FDLS_TGT_STATE_PRLI) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			     "PRLI rsp recvd in wrong state. Drop frame. Restarting nexus");
		fdls_target_restart_nexus(tport);
		return;
	}

	if (fchdr->fh_ox_id != tport->oxid_used) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "Dropping PRLI response from target: 0x%x ",
			 tgt_fcid);
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "Reason: Stale PRLI response/Aborted PDISC/OOO frame delivery");
		return;
	}

	oxid = ntohs(FNIC_STD_GET_OX_ID(fchdr));
	fdls_free_tgt_oxid(iport, &iport->prli_oxid_pool, oxid);

	switch (prli_rsp->els_prli.prli_cmd) {
	case ELS_LS_ACC:
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "PRLI accepted from target: 0x%x", tgt_fcid);

		if (prli_rsp->sp.spp_type != FC_FC4_TYPE_SCSI) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
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
	case ELS_LS_RJT:
		if (((els_rjt->u.rej.er_reason == ELS_RJT_BUSY)
		     || (els_rjt->u.rej.er_reason == ELS_RJT_UNAB))
			&& (tport->retry_counter < FDLS_RETRY_COUNT)) {

			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "PRLI ret ELS_LS_RJT BUSY. Retry from timer routine: 0x%x",
				 tgt_fcid);

			/*Retry Plogi again from the timer routine. */
			tport->flags |= FNIC_FDLS_RETRY_FRAME;
			return;
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "PRLI returned ELS_LS_RJT from target: 0x%x",
						 tgt_fcid);

			fdls_tgt_logout(iport, tport);
			fdls_delete_tport(iport, tport);
			return;
		}
		break;

	default:
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "PRLI not accepted from target: 0x%x", tgt_fcid);
		return;
		break;
	}

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Found the PRLI target: 0x%x and state: %d",
				 (unsigned int) tgt_fcid, tport->state);

	if (tport->timer_pending) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "tport fcid 0x%x: Canceling disc timer\n",
					 tport->fcid);
		fnic_del_tport_timer_sync(fnic, tport);
	}
	tport->timer_pending = 0;

	/* Learn Service Params */
	tport->fcp_csp = be32_to_cpu(prli_rsp->sp.spp_params);
	tport->retry_counter = 0;

	if (prli_rsp->sp.spp_params & FCP_SPPF_RETRY)
		tport->tgt_flags |= FNIC_FC_RP_FLAGS_RETRY;

	/* Check if the device plays Target Mode Function */
	if (!(tport->fcp_csp & FCP_PRLI_FUNC_TARGET)) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
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
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "tport event memory allocation failure: 0x%0x\n",
				 tport->fcid);
		return;
	}
	tport_add_evt->event = TGT_EV_RPORT_ADD;
	tport_add_evt->arg1 = (void *) tport;
	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "iport fcid: 0x%x add tport event fcid: 0x%x\n",
			 tport->fcid, iport->fcid);
	list_add_tail(&tport_add_evt->links, &fnic->tport_event_list);
	queue_work(fnic_event_queue, &fnic->tport_work);
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
					     "Canceling fabric disc timer %p\n",
					     iport);
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
fdls_process_gpn_ft_tgt_list(struct fnic_iport_s *iport,
			     struct fc_frame_header *fchdr, int len)
{
	struct fc_gpn_ft_rsp_iu *gpn_ft_tgt;
	struct fnic_tport_s *tport, *next;
	uint32_t fcid;
	uint64_t wwpn;
	int rem_len = len;
	u32 old_link_down_cnt = iport->fnic->link_down_cnt;
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "0x%x: FDLS process GPN_FT tgt list", iport->fcid);

	gpn_ft_tgt =
	    (struct fc_gpn_ft_rsp_iu *)((uint8_t *) fchdr +
					sizeof(struct fc_frame_header)
					+ sizeof(struct fc_ct_hdr));
	len -= sizeof(struct fc_frame_header) + sizeof(struct fc_ct_hdr);

	while (rem_len > 0) {

		fcid = ntoh24(gpn_ft_tgt->fcid);
		wwpn = ntohll(gpn_ft_tgt->wwpn);

		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "tport: 0x%x: ctrl:0x%x", fcid, gpn_ft_tgt->ctrl);

		if (fcid == iport->fcid) {
			if (gpn_ft_tgt->ctrl & FNIC_FC_GPN_LAST_ENTRY)
				break;
			gpn_ft_tgt++;
			rem_len -= sizeof(struct fc_gpn_ft_rsp_iu);
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
		rem_len -= sizeof(struct fc_gpn_ft_rsp_iu);
	}
	if (rem_len <= 0) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "GPN_FT response: malformed/corrupt frame rxlen: %d remlen: %d",
			 len, rem_len);
	}

	/*remove those ports which was not listed in GPN_FT */
	if (fdls_get_state((&iport->fabric)) == FDLS_STATE_RSCN_GPN_FT) {
		list_for_each_entry_safe(tport, next, &iport->tport_list, links) {

			if (!(tport->flags & FNIC_FDLS_TPORT_IN_GPN_FT_LIST)) {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
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
fdls_process_gpn_ft_rsp(struct fnic_iport_s *iport,
			struct fc_frame_header *fchdr, int len)
{
	struct fnic_fdls_fabric_s *fdls = &iport->fabric;
	struct fc_std_gpn_ft *gpn_ft_rsp = (struct fc_std_gpn_ft *) fchdr;
	uint16_t rsp;
	uint8_t reason_code;
	int count = 0;
	struct fnic_tport_s *tport, *next;
	u32 old_link_down_cnt = iport->fnic->link_down_cnt;
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
		if (iport->fabric.timer_pending) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "0x%x: Canceling fabric disc timer\n",
						 iport->fcid);
			fnic_del_fabric_timer_sync(fnic);
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
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Halting target discovery: fab st: %d iport st: %d ",
				 fdls_get_state(fdls), iport->state);
			break;
		}
		fdls_tgt_discovery_start(iport);
		break;

	case FC_FS_RJT:
		reason_code = gpn_ft_rsp->fc_std_ct_hdr.ct_reason;
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "0x%x: GPNFT_RSP Reject reason: %d", iport->fcid, reason_code);

		if (((reason_code == FC_FS_RJT_BSY)
		     || (reason_code == FC_FS_RJT_UNABL))
			&& (fdls->retry_counter < FDLS_RETRY_COUNT)) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "0x%x: GPNFT_RSP ret REJ/BSY. Retry from timer routine",
				 iport->fcid);
			/* Retry again from the timer routine */
			fdls->flags |= FNIC_FDLS_RETRY_FRAME;
		} else {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "0x%x: GPNFT_RSP reject", iport->fcid);
			if (iport->fabric.timer_pending) {
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
							 "0x%x: Canceling fabric disc timer\n",
							 iport->fcid);
				fnic_del_fabric_timer_sync(fnic);
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
				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
							 "GPN_FT_REJECT: Remove port: 0x%x",
							 tport->fcid);
				fdls_delete_tport(iport, tport);
				if ((old_link_down_cnt != iport->fnic->link_down_cnt)
					|| (iport->state != FNIC_IPORT_STATE_READY)) {
					return;
				}
				count++;
			}
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "GPN_FT_REJECT: Removed (0x%x) ports", count);
		}
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

		if (fnic_fdls_register_portid(iport, iport->fcid, rx_frame) != 0) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "0x%x: FLOGI registration failed", iport->fcid);
			break;
		}

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

static void fdls_process_fdmi_plogi_rsp(struct fnic_iport_s *iport,
					struct fc_frame_header *fchdr)
{
	struct fc_std_flogi *plogi_rsp = (struct fc_std_flogi *)fchdr;
	struct fc_std_els_rsp *els_rjt = (struct fc_std_els_rsp *)fchdr;
	struct fnic *fnic = iport->fnic;
	u64 fdmi_tov;

	iport->fabric.fdmi_pending &= ~FDLS_FDMI_PLOGI_PENDING;
	fdls_free_fabric_oxid(iport, &iport->fabric_oxid_pool,
			      ntohs(FNIC_STD_GET_OX_ID(fchdr)));

	if (ntoh24(fchdr->fh_s_id) == 0XFFFFFA) {
		del_timer_sync(&iport->fabric.fdmi_timer);
		iport->fabric.fdmi_pending = 0;
		switch (plogi_rsp->els.fl_cmd) {
		case ELS_LS_ACC:
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "FDLS process fdmi PLOGI response status: ELS_LS_ACC\n");
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Sending fdmi registration for port 0x%x\n",
				 iport->fcid);

			fdls_fdmi_register_hba(iport);
			fdls_fdmi_register_pa(iport);
			fdmi_tov = jiffies + msecs_to_jiffies(5000);
			mod_timer(&iport->fabric.fdmi_timer,
				  round_jiffies(fdmi_tov));
			break;
		case ELS_LS_RJT:
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Fabric FDMI PLOGI returned ELS_LS_RJT reason: 0x%x",
				     els_rjt->u.rej.er_reason);

			if (((els_rjt->u.rej.er_reason == ELS_RJT_BUSY)
			     || (els_rjt->u.rej.er_reason == ELS_RJT_UNAB))
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
				      struct fc_frame_header *fchdr,
				      int rsp_type)
{
	struct fnic *fnic = iport->fnic;

	if (!iport->fabric.fdmi_pending) {
		FNIC_FCS_DBG(KERN_ERR, fnic->lport->host, fnic->fnic_num,
			     "Received FDMI ack while not waiting:%x\n",
			     ntohs(FNIC_STD_GET_OX_ID(fchdr)));
		return;
	}

	if (rsp_type == FNIC_FDMI_REG_HBA_RSP)
		iport->fabric.fdmi_pending &= ~FDLS_FDMI_REG_HBA_PENDING;
	else
		iport->fabric.fdmi_pending &= ~FDLS_FDMI_RPA_PENDING;

	fdls_free_fabric_oxid(iport, &iport->fdmi_oxid_pool,
			      ntohs(FNIC_STD_GET_OX_ID(fchdr)));

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
		"iport fcid: 0x%x: Received FDMI registration ack\n",
		 iport->fcid);

	if (!iport->fabric.fdmi_pending) {
		del_timer_sync(&iport->fabric.fdmi_timer);
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "iport fcid: 0x%x: Canceling FDMI timer\n",
					 iport->fcid);
	}
}

static void fdls_process_fdmi_abts_rsp(struct fnic_iport_s *iport,
				       struct fc_frame_header *fchdr)
{
	uint32_t s_id;
	struct fnic *fnic = iport->fnic;

	s_id = ntoh24(FNIC_STD_GET_S_ID(fchdr));

	if (!(s_id != 0xFFFFFA)) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			     "Received abts rsp with invalid SID: 0x%x. Dropping frame",
			     s_id);
		return;
	}

	del_timer_sync(&iport->fabric.fdmi_timer);
	iport->fabric.fdmi_pending &= ~FDLS_FDMI_ABORT_PENDING;

	fdls_free_fabric_oxid(iport, &iport->fdmi_oxid_pool,
			      ntohs(FNIC_STD_GET_OX_ID(fchdr)));
	fdls_send_fdmi_plogi(iport);
}

static void
fdls_process_fabric_abts_rsp(struct fnic_iport_s *iport,
			     struct fc_frame_header *fchdr)
{
	uint32_t s_id;
	struct fc_std_abts_ba_acc *ba_acc = (struct fc_std_abts_ba_acc *)fchdr;
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

static void
fdls_process_abts_req(struct fnic_iport_s *iport, struct fc_frame_header *fchdr)
{
	struct fc_std_abts_ba_acc ba_acc;
	uint32_t nport_id;
	uint16_t oxid;
	struct fnic_tport_s *tport;
	struct fnic *fnic = iport->fnic;
	struct fnic_tgt_oxid_pool_s *oxid_pool;

	nport_id = ntoh24(fchdr->fh_s_id);
	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Received abort from SID %8x", nport_id);

	tport = fnic_find_tport_by_fcid(iport, nport_id);
	if (tport) {
		oxid = FNIC_STD_GET_OX_ID(fchdr);
		if (tport->oxid_used == oxid) {
			tport->flags |= FNIC_FDLS_TGT_ABORT_ISSUED;
			oxid_pool = fdls_get_tgt_oxid_pool(tport);
			fdls_free_tgt_oxid(iport, oxid_pool, ntohs(oxid));
		}
	}

	memcpy(&ba_acc, &fnic_std_ba_acc, sizeof(struct fc_std_abts_ba_acc));
	FNIC_STD_SET_S_ID((&ba_acc.fchdr), fchdr->fh_d_id);
	FNIC_STD_SET_D_ID((&ba_acc.fchdr), fchdr->fh_s_id);

	ba_acc.fchdr.fh_rx_id = fchdr->fh_rx_id;
	ba_acc.acc.ba_rx_id = ba_acc.fchdr.fh_rx_id;
	ba_acc.fchdr.fh_ox_id = fchdr->fh_ox_id;
	ba_acc.acc.ba_ox_id = ba_acc.fchdr.fh_ox_id;

	fnic_send_fcoe_frame(iport, &ba_acc, sizeof(struct fc_std_abts_ba_acc));
}

static void
fdls_process_unsupported_els_req(struct fnic_iport_s *iport,
				 struct fc_frame_header *fchdr)
{
	struct fc_std_els_rsp ls_rsp;
	uint16_t oxid;
	uint32_t d_id = ntoh24(fchdr->fh_d_id);
	struct fnic *fnic = iport->fnic;

	memcpy(&ls_rsp, &fnic_std_els_rjt, FC_ELS_RSP_REJ_SIZE);

	if (iport->fcid != d_id) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "Dropping unsupported ELS with illegal frame bits 0x%x\n",
			 d_id);
		return;
	}

	if ((iport->state != FNIC_IPORT_STATE_READY)
		&& (iport->state != FNIC_IPORT_STATE_FABRIC_DISC)) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "Dropping unsupported ELS request in iport state: %d",
			 iport->state);
		return;
	}

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Process unsupported ELS request from SID: 0x%x",
		     ntoh24(fchdr->fh_s_id));
	/* We don't support this ELS request, send a reject */
	ls_rsp.u.rej.er_reason = 0x0B;
	ls_rsp.u.rej.er_explan = 0x0;
	ls_rsp.u.rej.er_vendor = 0x0;

	FNIC_STD_SET_S_ID((&ls_rsp.fchdr), fchdr->fh_d_id);
	FNIC_STD_SET_D_ID((&ls_rsp.fchdr), fchdr->fh_s_id);
	oxid = FNIC_STD_GET_OX_ID(fchdr);
	FNIC_STD_SET_OX_ID((&ls_rsp.fchdr), oxid);

	FNIC_STD_SET_RX_ID((&ls_rsp.fchdr), FNIC_UNSUPPORTED_RESP_OXID);

	fnic_send_fcoe_frame(iport, &ls_rsp, FC_ELS_RSP_REJ_SIZE);
}

static void
fdls_process_rls_req(struct fnic_iport_s *iport, struct fc_frame_header *fchdr)
{
	struct fc_std_rls_acc rls_acc_rsp;
	uint16_t oxid;
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Process RLS request %d", iport->fnic->fnic_num);

	if ((iport->state != FNIC_IPORT_STATE_READY)
		&& (iport->state != FNIC_IPORT_STATE_FABRIC_DISC)) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "Received RLS req in iport state: %d. Dropping the frame.",
			 iport->state);
		return;
	}

	memset(&rls_acc_rsp, 0, sizeof(struct fc_std_rls_acc));

	FNIC_STD_SET_S_ID((&rls_acc_rsp.fchdr), fchdr->fh_d_id);
	FNIC_STD_SET_D_ID((&rls_acc_rsp.fchdr), fchdr->fh_s_id);
	oxid = FNIC_STD_GET_OX_ID(fchdr);
	FNIC_STD_SET_OX_ID((&rls_acc_rsp.fchdr), oxid);
	FNIC_STD_SET_RX_ID((&rls_acc_rsp.fchdr), 0xffff);
	FNIC_STD_SET_F_CTL(&rls_acc_rsp.fchdr, FNIC_ELS_REP_FCTL << 16);
	FNIC_STD_SET_R_CTL(&rls_acc_rsp.fchdr, FC_RCTL_ELS_REP);
	FNIC_STD_SET_TYPE(&rls_acc_rsp.fchdr, FC_TYPE_ELS);
	rls_acc_rsp.els.rls_cmd = ELS_LS_ACC;
	rls_acc_rsp.els.rls_lesb.lesb_link_fail =
	    htonl(iport->fnic->link_down_cnt);

	fnic_send_fcoe_frame(iport, &rls_acc_rsp,
			     sizeof(struct fc_std_rls_acc));
}

static void
fdls_process_els_req(struct fnic_iport_s *iport, struct fc_frame_header *fchdr,
					 uint32_t len)
{
	struct fc_std_els_rsp *els_acc;
	uint16_t oxid;
	uint8_t fcid[3];
	uint8_t *fc_payload;
	uint8_t *dst_frame;
	uint8_t type;
	struct fnic *fnic = iport->fnic;

	fc_payload = (uint8_t *) fchdr + sizeof(struct fc_frame_header);
	type = *fc_payload;

	if ((iport->state != FNIC_IPORT_STATE_READY)
		&& (iport->state != FNIC_IPORT_STATE_FABRIC_DISC)) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Dropping ELS frame type :%x in iport state: %d",
				 type, iport->state);
		return;
	}
	switch (type) {
	case ELS_ECHO:
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "sending LS_ACC for ECHO request %d\n",
					 iport->fnic->fnic_num);
		break;

	case ELS_RRQ:
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "sending LS_ACC for RRQ request %d\n",
					 iport->fnic->fnic_num);
		break;

	default:
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "sending LS_ACC for %x ELS frame\n", type);
		break;
	}
	dst_frame = kzalloc(len, GFP_ATOMIC);
	if (!dst_frame) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Failed to allocate ELS response for %x", type);
		return;
	}
	if (type == ELS_ECHO) {
		/* Brocade sends a longer payload, copy all frame back */
		memcpy(dst_frame, fchdr, len);
	}

	els_acc = (struct fc_std_els_rsp *)dst_frame;
	memcpy(els_acc, &fnic_std_els_acc, FC_ELS_RSP_ACC_SIZE);

	hton24(fcid, iport->fcid);
	FNIC_STD_SET_S_ID((&els_acc->fchdr), fcid);
	FNIC_STD_SET_D_ID((&els_acc->fchdr), fchdr->fh_s_id);

	oxid = FNIC_STD_GET_OX_ID(fchdr);
	FNIC_STD_SET_OX_ID((&els_acc->fchdr), oxid);
	FNIC_STD_SET_RX_ID((&els_acc->fchdr), 0xffff);

	if (type == ELS_ECHO)
		fnic_send_fcoe_frame(iport, els_acc, len);
	else
		fnic_send_fcoe_frame(iport, els_acc, FC_ELS_RSP_ACC_SIZE);

	kfree(dst_frame);
}

static void
fdls_process_tgt_abts_rsp(struct fnic_iport_s *iport,
			  struct fc_frame_header *fchdr)
{
	uint32_t s_id;
	struct fnic_tport_s *tport;
	uint32_t tport_state;
	struct fc_std_abts_ba_acc *ba_acc;
	struct fc_std_abts_ba_rjt *ba_rjt;
	uint16_t oxid;
	struct fnic *fnic = iport->fnic;

	s_id = ntoh24(fchdr->fh_s_id);
	ba_acc = (struct fc_std_abts_ba_acc *)fchdr;
	ba_rjt = (struct fc_std_abts_ba_rjt *)fchdr;

	tport = fnic_find_tport_by_fcid(iport, s_id);
	if (!tport) {
		FNIC_FCS_DBG(KERN_ERR, fnic->lport->host, fnic->fnic_num,
					 "Received tgt abts rsp with invalid SID: 0x%x", s_id);
		return;
	}

	if (tport->timer_pending) {
		FNIC_FCS_DBG(KERN_ERR, fnic->lport->host, fnic->fnic_num,
					 "tport 0x%p Canceling fabric disc timer\n", tport);
		fnic_del_tport_timer_sync(fnic, tport);
	}
	if (iport->state != FNIC_IPORT_STATE_READY) {
		FNIC_FCS_DBG(KERN_ERR, fnic->lport->host, fnic->fnic_num,
					 "Received tgt abts rsp in iport state(%d). Dropping.",
					 iport->state);
		return;
	}
	tport->timer_pending = 0;
	tport->flags &= ~FNIC_FDLS_TGT_ABORT_ISSUED;
	tport_state = tport->state;
	oxid = ntohs(fchdr->fh_ox_id);

	/*This abort rsp is for ADISC */
	if ((oxid >= FDLS_ADISC_OXID_BASE) && (oxid < FDLS_TGT_OXID_POOL_END)) {
		if (fchdr->fh_r_ctl == FNIC_BA_ACC_RCTL) {
			FNIC_FCS_DBG(KERN_ERR, fnic->lport->host, fnic->fnic_num,
				     "OX_ID: 0x%x tgt_fcid: 0x%x rcvd tgt adisc abts resp BA_ACC",
				     be16_to_cpu(ba_acc->acc.ba_ox_id),
				     tport->fcid);
		} else if (fchdr->fh_r_ctl == FNIC_BA_RJT_RCTL) {
			FNIC_FCS_DBG(KERN_ERR, fnic->lport->host, fnic->fnic_num,
				 "ADISC BA_RJT rcvd tport_fcid: 0x%x tport_state: %d ",
				 tport->fcid, tport_state);
			FNIC_FCS_DBG(KERN_ERR, fnic->lport->host, fnic->fnic_num,
				 "reason code: 0x%x reason code explanation:0x%x ",
				     ba_rjt->rjt.br_reason,
				     ba_rjt->rjt.br_explan);
		}
		if ((tport->retry_counter < FDLS_RETRY_COUNT)
		    && (fchdr->fh_r_ctl == FNIC_BA_ACC_RCTL)) {
			fdls_free_tgt_oxid(iport, &iport->adisc_oxid_pool,
					   oxid);
			fdls_send_tgt_adisc(iport, tport);
			return;
		}

		fdls_free_tgt_oxid(iport, &iport->adisc_oxid_pool, oxid);
		FNIC_FCS_DBG(KERN_ERR, fnic->lport->host, fnic->fnic_num,
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
		if (fchdr->fh_r_ctl == FNIC_BA_ACC_RCTL) {
			FNIC_FCS_DBG(KERN_ERR, fnic->lport->host, fnic->fnic_num,
				 "Received tgt PLOGI abts response BA_ACC tgt_fcid: 0x%x",
				 tport->fcid);
		} else if (fchdr->fh_r_ctl == FNIC_BA_RJT_RCTL) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "PLOGI BA_RJT received for tport_fcid: 0x%x OX_ID: 0x%x",
				     tport->fcid, fchdr->fh_ox_id);
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "reason code: 0x%x reason code explanation: 0x%x",
				     ba_rjt->rjt.br_reason,
				     ba_rjt->rjt.br_explan);
		}
		if ((tport->retry_counter < iport->max_plogi_retries)
		    && (fchdr->fh_r_ctl == FNIC_BA_ACC_RCTL)) {
			fdls_free_tgt_oxid(iport, &iport->plogi_oxid_pool,
					   oxid);
			fdls_send_tgt_plogi(iport, tport);
			return;
		}

		fdls_free_tgt_oxid(iport, &iport->plogi_oxid_pool, oxid);
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
		if (fchdr->fh_r_ctl == FNIC_BA_ACC_RCTL) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "0x%x: Received tgt PRLI abts response BA_ACC",
				 tport->fcid);
		} else if (fchdr->fh_r_ctl == FNIC_BA_RJT_RCTL) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "PRLI BA_RJT received for tport_fcid: 0x%x OX_ID: 0x%x ",
				     tport->fcid, fchdr->fh_ox_id);
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "reason code: 0x%x reason code explanation: 0x%x",
				     ba_rjt->rjt.br_reason,
				     ba_rjt->rjt.br_explan);
		}
		if ((tport->retry_counter < FDLS_RETRY_COUNT)
		    && (fchdr->fh_r_ctl == FNIC_BA_ACC_RCTL)) {
			fdls_free_tgt_oxid(iport, &iport->prli_oxid_pool, oxid);
			fdls_send_tgt_prli(iport, tport);
			return;
		}
		fdls_free_tgt_oxid(iport, &iport->prli_oxid_pool, oxid);
		fdls_send_tgt_plogi(iport, tport);	/* go back to plogi */
		fdls_set_tport_state(tport, FDLS_TGT_STATE_PLOGI);
		return;
	}

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Received ABTS response for unknown frame %p", iport);
}

static void
fdls_process_plogi_req(struct fnic_iport_s *iport,
		       struct fc_frame_header *fchdr)
{
	struct fc_std_els_rsp plogi_rsp;
	uint16_t oxid;
	uint32_t d_id = ntoh24(fchdr->fh_d_id);
	struct fnic *fnic = iport->fnic;

	memcpy(&plogi_rsp, &fnic_std_els_rjt, sizeof(struct fc_std_els_rsp));

	if (iport->fcid != d_id) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "Received PLOGI with illegal frame bits. Dropping frame %p",
			 iport);
		return;
	}

	if (iport->state != FNIC_IPORT_STATE_READY) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "Received PLOGI request in iport state: %d Dropping frame",
			 iport->state);
		return;
	}
	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Process PLOGI request from SID: 0x%x",
		     ntoh24(fchdr->fh_s_id));

	/* We don't support PLOGI request, send a reject */
	plogi_rsp.u.rej.er_reason = 0x0B;
	plogi_rsp.u.rej.er_explan = 0x0;
	plogi_rsp.u.rej.er_vendor = 0x0;

	FNIC_STD_SET_S_ID((&plogi_rsp.fchdr), fchdr->fh_d_id);
	FNIC_STD_SET_D_ID((&plogi_rsp.fchdr), fchdr->fh_s_id);

	oxid = FNIC_STD_GET_OX_ID(fchdr);
	FNIC_STD_SET_OX_ID((&plogi_rsp.fchdr), oxid);

	FNIC_STD_SET_RX_ID((&plogi_rsp.fchdr), FNIC_UNASSIGNED_RXID);

	fnic_send_fcoe_frame(iport, &plogi_rsp, FC_ELS_RSP_REJ_SIZE);
}

static void
fdls_process_logo_req(struct fnic_iport_s *iport, struct fc_frame_header *fchdr)
{
	struct fc_std_logo *logo = (struct fc_std_logo *)fchdr;
	uint32_t nport_id;
	uint64_t nport_name;
	struct fnic_tport_s *tport;
	struct fnic *fnic = iport->fnic;
	uint16_t oxid;

	nport_id = ntoh24(logo->els.fl_n_port_id);
	nport_name = logo->els.fl_n_port_wwn;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Process LOGO request from fcid: 0x%x", nport_id);

	if (iport->state != FNIC_IPORT_STATE_READY) {
		FNIC_FCS_DBG(KERN_ERR, fnic->lport->host, fnic->fnic_num,
			 "Dropping LOGO req from 0x%x in iport state: %d",
			 nport_id, iport->state);
		return;
	}

	tport = fnic_find_tport_by_fcid(iport, nport_id);

	if (!tport) {
		/* We are not logged in with the nport, log and drop... */
		FNIC_FCS_DBG(KERN_ERR, fnic->lport->host, fnic->fnic_num,
			 "Received LOGO from an nport not logged in: 0x%x(0x%llx)",
			 nport_id, nport_name);
		return;
	}
	if (tport->fcid != nport_id) {
		FNIC_FCS_DBG(KERN_ERR, fnic->lport->host, fnic->fnic_num,
		 "Received LOGO with invalid target port fcid: 0x%x(0x%llx)",
		 nport_id, nport_name);
		return;
	}
	if (tport->timer_pending) {
		FNIC_FCS_DBG(KERN_ERR, fnic->lport->host, fnic->fnic_num,
					 "tport fcid 0x%x: Canceling disc timer\n",
					 tport->fcid);
		fnic_del_tport_timer_sync(fnic, tport);
		tport->timer_pending = 0;
	}

	/* got a logo in response to adisc to a target which has logged out */
	if (tport->state == FDLS_TGT_STATE_ADISC) {
		tport->retry_counter = 0;
		oxid = ntohs(tport->oxid_used);
		fdls_free_tgt_oxid(iport, &iport->adisc_oxid_pool, oxid);
		fdls_delete_tport(iport, tport);
		fdls_send_logo_resp(iport, &logo->fchdr);
		if ((iport->state == FNIC_IPORT_STATE_READY)
			&& (fdls_get_state(&iport->fabric) != FDLS_STATE_SEND_GPNFT)
			&& (fdls_get_state(&iport->fabric) != FDLS_STATE_RSCN_GPN_FT)) {
			FNIC_FCS_DBG(KERN_ERR, fnic->lport->host, fnic->fnic_num,
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
			FNIC_FCS_DBG(KERN_ERR, fnic->lport->host, fnic->fnic_num,
						 "Sending GPNFT in response to LOGO from Target:0x%x",
						 nport_id);
			fdls_send_gpn_ft(iport, FDLS_STATE_SEND_GPNFT);
		}
	}
}

static void
fdls_process_rscn(struct fnic_iport_s *iport, struct fc_frame_header *fchdr)
{
	struct fc_std_rscn *rscn;
	struct fc_els_rscn_page *rscn_port = NULL;
	int num_ports;
	struct fnic_tport_s *tport, *next;
	uint32_t nport_id;
	uint8_t fcid[3];
	int newports = 0;
	struct fnic_fdls_fabric_s *fdls = &iport->fabric;
	struct fnic *fnic = iport->fnic;
	uint16_t rscn_payload_len;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "FDLS process RSCN %p", iport);

	if (iport->state != FNIC_IPORT_STATE_READY) {
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "FDLS RSCN received in state(%d). Dropping",
					 fdls_get_state(fdls));
		return;
	}

	rscn = (struct fc_std_rscn *)fchdr;
	rscn_payload_len = be16_to_cpu(rscn->els.rscn_plen);

	/* frame validation */
	if ((rscn_payload_len % 4 != 0) || (rscn_payload_len < 8)
	    || (rscn_payload_len > 1024)
	    || (rscn->els.rscn_page_len != 4)) {
		num_ports = 0;
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "RSCN payload_len: 0x%x page_len: 0x%x",
				     rscn_payload_len, rscn->els.rscn_page_len);
		/* if this happens then we need to send ADISC to all the tports. */
		list_for_each_entry_safe(tport, next, &iport->tport_list, links) {
			if (tport->state == FDLS_TGT_STATE_READY)
				tport->flags |= FNIC_FDLS_TPORT_SEND_ADISC;
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "RSCN for port id: 0x%x", tport->fcid);
		}
	} else {
		num_ports = (rscn_payload_len - 4) / rscn->els.rscn_page_len;
		rscn_port = (struct fc_els_rscn_page *)(rscn + 1);
	}
	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "RSCN received for num_ports: %d payload_len: %d page_len: %d ",
		     num_ports, rscn_payload_len, rscn->els.rscn_page_len);

	/*
	 * RSCN have at least one Port_ID page , but may not have any port_id
	 * in it. If no port_id is specified in the Port_ID page , we send
	 * ADISC to all the tports
	 */

	while (num_ports) {

		memcpy(fcid, rscn_port->rscn_fid, 3);

		nport_id = ntoh24(fcid);
		rscn_port++;
		num_ports--;
		/* if this happens then we need to send ADISC to all the tports. */
		if (nport_id == 0) {
			list_for_each_entry_safe(tport, next, &iport->tport_list,
									 links) {
				if (tport->state == FDLS_TGT_STATE_READY)
					tport->flags |= FNIC_FDLS_TPORT_SEND_ADISC;

				FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
							 "RSCN for port id: 0x%x", tport->fcid);
			}
			break;
		}
		tport = fnic_find_tport_by_fcid(iport, nport_id);

		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "RSCN port id list: 0x%x", nport_id);

		if (!tport) {
			newports++;
			continue;
		}
		if (tport->state == FDLS_TGT_STATE_READY)
			tport->flags |= FNIC_FDLS_TPORT_SEND_ADISC;
	}

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
		 "FDLS process RSCN sending GPN_FT: newports: %d", newports);
	fdls_send_gpn_ft(iport, FDLS_STATE_RSCN_GPN_FT);
	fdls_send_rscn_resp(iport, fchdr);
}

void fnic_fdls_disc_start(struct fnic_iport_s *iport)
{
	struct fnic *fnic = iport->fnic;

	if (IS_FNIC_FCP_INITIATOR(fnic)) {
		fc_host_fabric_name(iport->fnic->lport->host) = 0;
		fc_host_post_event(iport->fnic->lport->host, fc_get_event_number(),
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
fdls_process_adisc_req(struct fnic_iport_s *iport,
		       struct fc_frame_header *fchdr)
{
	struct fc_std_els_adisc adisc_acc;
	struct fc_std_els_adisc *adisc_req = (struct fc_std_els_adisc *)fchdr;
	uint64_t frame_wwnn;
	uint64_t frame_wwpn;
	uint32_t tgt_fcid;
	struct fnic_tport_s *tport;
	uint8_t *fcid;
	struct fc_std_els_rsp rjts_rsp;
	uint16_t oxid;
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "Process ADISC request %d", iport->fnic->fnic_num);

	fcid = FNIC_STD_GET_S_ID(fchdr);
	tgt_fcid = ntoh24(fcid);
	tport = fnic_find_tport_by_fcid(iport, tgt_fcid);
	if (!tport) {
		FNIC_FCS_DBG(KERN_ERR, fnic->lport->host, fnic->fnic_num,
					 "tport for fcid: 0x%x not found. Dropping ADISC req.",
					 tgt_fcid);
		return;
	}
	if (iport->state != FNIC_IPORT_STATE_READY) {
		FNIC_FCS_DBG(KERN_ERR, fnic->lport->host, fnic->fnic_num,
			 "Dropping ADISC req from fcid: 0x%x in iport state: %d",
			 tgt_fcid, iport->state);
		return;
	}

	frame_wwnn = ntohll(adisc_req->els.adisc_wwnn);
	frame_wwpn = ntohll(adisc_req->els.adisc_wwpn);

	if ((frame_wwnn != tport->wwnn) || (frame_wwpn != tport->wwpn)) {
		/* send reject */
		FNIC_FCS_DBG(KERN_ERR, fnic->lport->host, fnic->fnic_num,
			 "ADISC req from fcid: 0x%x mismatch wwpn: 0x%llx wwnn: 0x%llx",
			 tgt_fcid, frame_wwpn, frame_wwnn);
		FNIC_FCS_DBG(KERN_ERR, fnic->lport->host, fnic->fnic_num,
			 "local tport wwpn: 0x%llx wwnn: 0x%llx. Sending RJT",
			 tport->wwpn, tport->wwnn);

		memcpy(&rjts_rsp, &fnic_std_els_rjt,
		       sizeof(struct fc_std_els_rsp));

		rjts_rsp.u.rej.er_reason = 0x03;	/*  logical error */
		rjts_rsp.u.rej.er_explan = 0x1E;	/*  N_port login required */
		rjts_rsp.u.rej.er_vendor = 0x0;
		FNIC_STD_SET_S_ID((&rjts_rsp.fchdr), fchdr->fh_d_id);
		FNIC_STD_SET_D_ID((&rjts_rsp.fchdr), fchdr->fh_s_id);
		oxid = FNIC_STD_GET_OX_ID(fchdr);
		FNIC_STD_SET_OX_ID((&rjts_rsp.fchdr), oxid);
		FNIC_STD_SET_RX_ID((&rjts_rsp.fchdr), FNIC_UNASSIGNED_RXID);

		fnic_send_fcoe_frame(iport, &rjts_rsp, FC_ELS_RSP_REJ_SIZE);
		return;
	}
	memset(&adisc_acc.fchdr, 0, sizeof(struct fc_frame_header));
	FNIC_STD_SET_S_ID(&adisc_acc.fchdr, fchdr->fh_d_id);
	FNIC_STD_SET_D_ID(&adisc_acc.fchdr, fchdr->fh_s_id);
	FNIC_STD_SET_F_CTL(&adisc_acc.fchdr, FNIC_ELS_REP_FCTL << 16);
	FNIC_STD_SET_R_CTL(&adisc_acc.fchdr, FC_RCTL_ELS_REP);
	FNIC_STD_SET_TYPE(&adisc_acc.fchdr, FC_TYPE_ELS);
	oxid = FNIC_STD_GET_OX_ID(fchdr);
	FNIC_STD_SET_OX_ID(&adisc_acc.fchdr, oxid);
	FNIC_STD_SET_RX_ID(&adisc_acc.fchdr, FNIC_UNASSIGNED_RXID);
	adisc_acc.els.adisc_cmd = ELS_LS_ACC;

	FNIC_STD_SET_NPORT_NAME(&adisc_acc.els.adisc_wwpn,
				le64_to_cpu(iport->wwpn));
	FNIC_STD_SET_NODE_NAME(&adisc_acc.els.adisc_wwnn,
			       le64_to_cpu(iport->wwnn));
	memcpy(adisc_acc.els.adisc_port_id, fchdr->fh_d_id, 3);

	fnic_send_fcoe_frame(iport, &adisc_acc,
			     sizeof(struct fc_std_els_adisc));
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

	/* ELS response from a target */
	if ((ntohs(oxid) >= FDLS_PLOGI_OXID_BASE)
		&& (ntohs(oxid) < FDLS_PRLI_OXID_BASE)) {
		if (!FNIC_FC_FRAME_TYPE_ELS(fchdr)) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				"Dropping Unknown frame in PLOGI exchange range type: 0x%x.",
				     fchdr->fh_type);
			return -1;
		}
		return FNIC_TPORT_PLOGI_RSP;
	}
	if ((ntohs(oxid) >= FDLS_PRLI_OXID_BASE)
		&& (ntohs(oxid) < FDLS_ADISC_OXID_BASE)) {
		if (!FNIC_FC_FRAME_TYPE_ELS(fchdr)) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				"Dropping Unknown frame in PRLI exchange range type: 0x%x.",
				     fchdr->fh_type);
			return -1;
		}
		return FNIC_TPORT_PRLI_RSP;
	}

	if ((ntohs(oxid) >= FDLS_ADISC_OXID_BASE)
		&& (ntohs(oxid) < FDLS_TGT_OXID_POOL_END)) {
		if (!FNIC_FC_FRAME_TYPE_ELS(fchdr)) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				"Dropping Unknown frame in ADISC exchange range type: 0x%x.",
				     fchdr->fh_type);
			return -1;
		}
		return FNIC_TPORT_ADISC_RSP;
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
	case FNIC_FDMI_PLOGI_RSP:
	case FNIC_FDMI_REG_HBA_RSP:
	case FNIC_FDMI_RPA_RSP:
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
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
					 "Logo response from tgt: 0x%x",
			     ntoh24(fchdr->fh_s_id));
		break;
	case FNIC_FABRIC_LOGO_RSP:
		fdls_process_fabric_logo_rsp(iport, fchdr);
		break;

	case FNIC_BLS_ABTS_RSP:
		oxid = ntohs(FNIC_STD_GET_OX_ID(fchdr));
		if (fdls_is_oxid_in_fabric_range(oxid) &&
			(iport->fabric.flags & FNIC_FDLS_FABRIC_ABORT_ISSUED)) {
			fdls_process_fabric_abts_rsp(iport, fchdr);
		} else if (fdls_is_oxid_in_fdmi_range(oxid) &&
			   iport->fabric.fdmi_pending) {
			fdls_process_fdmi_abts_rsp(iport, fchdr);
		} else {
			fdls_process_tgt_abts_rsp(iport, fchdr);
		}
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
	case FNIC_FDMI_REG_HBA_RSP:
	case FNIC_FDMI_RPA_RSP:
		fdls_process_fdmi_reg_ack(iport, fchdr, frame_type);
		break;
	default:
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "s_id: 0x%x d_did: 0x%x", s_id, d_id);
		FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
			 "Received unknown FCoE frame of len: %d. Dropping frame", len);
		break;
	}
}

void fnic_fdls_disc_init(struct fnic_iport_s *iport)
{
	fdls_init_oxid_pool(iport);
	fdls_set_state((&iport->fabric), FDLS_STATE_INIT);
}

void fnic_fdls_link_down(struct fnic_iport_s *iport)
{
	struct fnic_tport_s *tport, *next;
	struct fnic *fnic = iport->fnic;

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "0x%x: FDLS processing link down", iport->fcid);

	fdls_set_state((&iport->fabric), FDLS_STATE_LINKDOWN);
	iport->fabric.flags = 0;

	if (IS_FNIC_FCP_INITIATOR(fnic)) {
		spin_unlock_irqrestore(&fnic->fnic_lock, fnic->lock_flags);
		fnic_scsi_fcpio_reset(iport->fnic);
		spin_lock_irqsave(&fnic->fnic_lock, fnic->lock_flags);
		fdls_init_oxid_pool(iport);

		list_for_each_entry_safe(tport, next, &iport->tport_list, links) {
			FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
						 "removing rport: 0x%x", tport->fcid);
			fdls_delete_tport(iport, tport);
		}
	}

	if ((fnic_fdmi_support == 1) && (iport->fabric.fdmi_pending > 0)) {
		del_timer_sync(&iport->fabric.fdmi_timer);
		iport->fabric.fdmi_pending = 0;
	}

	FNIC_FCS_DBG(KERN_INFO, fnic->lport->host, fnic->fnic_num,
				 "0x%x: FDLS finish processing link down", iport->fcid);
}
