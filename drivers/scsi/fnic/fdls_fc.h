/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2008 Cisco Systems, Inc.  All rights reserved.
 * Copyright 2007 Nuova Systems, Inc.  All rights reserved.
 */

#ifndef _FDLS_FC_H_
#define _FDLS_FC_H_

/* This file contains the declarations for FC fabric services
 * and target discovery
 *
 * Request and Response for
 * 1. FLOGI
 * 2. PLOGI to Fabric Controller
 * 3. GPN_ID, GPN_FT
 * 4. RSCN
 * 5. PLOGI to Target
 * 6. PRLI to Target
 */

#include <scsi/scsi.h>
#include <scsi/fc/fc_els.h>
#include <uapi/scsi/fc/fc_fs.h>
#include <uapi/scsi/fc/fc_ns.h>
#include <uapi/scsi/fc/fc_gs.h>
#include <scsi/fc/fc_ms.h>

#ifndef MIN
#define MIN(x, y) (x < y ? x : y)
#endif				/* MIN */

#define FNIC_FCP_SP_RD_XRDY_DIS 0x00000002
#define FNIC_FCP_SP_TARGET      0x00000010
#define FNIC_FCP_SP_INITIATOR   0x00000020
#define FNIC_FCP_SP_CONF_CMPL   0x00000080
#define FNIC_FCP_SP_RETRY       0x00000100

#define FNIC_E_D_TOV           (0x7d0)
#define FNIC_FC_CONCUR_SEQS    (0xFF)
#define FNIC_FC_RO_INFO        (0x1F)

/* Little Endian */
#define FNIC_UNSUPPORTED_RESP_OXID   (0xffff)
#define FNIC_UNASSIGNED_RXID	(0xffff)
#define FNIC_ELS_REQ_FCTL      (0x000029)
#define FNIC_ELS_REP_FCTL      (0x000099)

#define FNIC_FCP_RSP_FCTL      (0x000099)
#define FNIC_REQ_ABTS_FCTL     (0x000009)

#define FNIC_FC_PH_VER_HI      (0x20)
#define FNIC_FC_PH_VER_LO      (0x20)
#define FNIC_FC_PH_VER         (0x2020)
#define FNIC_FC_B2B_CREDIT     (0x0A)
#define FNIC_FC_B2B_RDF_SZ     (0x0800)

#define FNIC_FC_FEATURES       (0x0080)

#define ETH_TYPE_FCOE			0x8906
#define ETH_TYPE_FIP			0x8914

#define FC_DIR_SERVER          0xFFFFFC
#define FC_FABRIC_CONTROLLER   0xFFFFFD
#define FC_DOMAIN_CONTR        0xFFFFFE

#define FNIC_FC_GPN_LAST_ENTRY (0x80)

#define FNIC_BA_ACC_RCTL        0x84
#define FNIC_BA_RJT_RCTL        0x85
#define FC_ABTS_RCTL            0x81

/* FNIC FDMI Register HBA Macros */
#define FNIC_FDMI_NUM_PORTS 0x1000000
#define FNIC_FDMI_NUM_HBA_ATTRS 0x9000000
#define FNIC_FDMI_TYPE_NODE_NAME	0X100
#define FNIC_FDMI_TYPE_MANUFACTURER	0X200
#define FNIC_FDMI_MANUFACTURER		"Cisco Systems"
#define FNIC_FDMI_TYPE_SERIAL_NUMBER	0X300
#define FNIC_FDMI_TYPE_MODEL		0X400
#define FNIC_FDMI_TYPE_MODEL_DES	0X500
#define FNIC_FDMI_MODEL_DESCRIPTION	"Cisco Virtual Interface Card"
#define FNIC_FDMI_TYPE_HARDWARE_VERSION	0X600
#define FNIC_FDMI_TYPE_DRIVER_VERSION	0X700
#define FNIC_FDMI_TYPE_ROM_VERSION	0X800
#define FNIC_FDMI_TYPE_FIRMWARE_VERSION	0X900
#define FNIC_FDMI_NN_LEN 0xc00
#define FNIC_FDMI_MANU_LEN 0x1800
#define FNIC_FDMI_SERIAL_LEN 0x1400
#define FNIC_FDMI_MODEL_LEN 0x1000
#define FNIC_FDMI_MODEL_DES_LEN 0x3c00
#define FNIC_FDMI_HW_VER_LEN 0x1400
#define FNIC_FDMI_DR_VER_LEN 0x2000
#define FNIC_FDMI_ROM_VER_LEN 0xc00
#define FNIC_FDMI_FW_VER_LEN 0x1400

/* FNIC FDMI Register PA Macros */
#define FNIC_FDMI_TYPE_FC4_TYPES	0X100
#define FNIC_FDMI_TYPE_SUPPORTED_SPEEDS 0X200
#define FNIC_FDMI_TYPE_CURRENT_SPEED	0X300
#define FNIC_FDMI_TYPE_MAX_FRAME_SIZE	0X400
#define FNIC_FDMI_TYPE_OS_NAME		0X500
#define FNIC_FDMI_TYPE_HOST_NAME	0X600
#define FNIC_FDMI_NUM_PORT_ATTRS 0x6000000
#define FNIC_FDMI_FC4_LEN 0x2400
#define FNIC_FDMI_SUPP_SPEED_LEN 0x800
#define FNIC_FDMI_CUR_SPEED_LEN 0x800
#define FNIC_FDMI_MFS_LEN 0x800
#define FNIC_FDMI_MFS 0x0080000
#define FNIC_FDMI_OS_NAME_LEN 0x1400
#define FNIC_FDMI_HN_LEN 0x1C00

#define FNIC_LOGI_RDF_SIZE(_logi) ((_logi)->fl_csp.sp_bb_data)
#define FNIC_LOGI_R_A_TOV(_logi) ((_logi)->fl_csp.sp_r_a_tov)
#define FNIC_LOGI_E_D_TOV(_logi) ((_logi)->fl_csp.sp_e_d_tov)
#define FNIC_LOGI_FEATURES(_logi) ((_logi)->fl_csp.sp_features)
#define FNIC_LOGI_PORT_NAME(_logi) ((_logi)->fl_wwpn)
#define FNIC_LOGI_NODE_NAME(_logi) ((_logi)->fl_wwnn)

#define FNIC_LOGI_SET_NPORT_NAME(_logi, _pName) \
	(FNIC_LOGI_PORT_NAME(_logi) = get_unaligned_be64(&_pName))
#define FNIC_LOGI_SET_NODE_NAME(_logi, _pName) \
	(FNIC_LOGI_NODE_NAME(_logi) = get_unaligned_be64(&_pName))
#define FNIC_LOGI_SET_RDF_SIZE(_logi, _rdf_size) \
	(FNIC_LOGI_RDF_SIZE(_logi) = cpu_to_be16(_rdf_size))
#define FNIC_LOGI_SET_E_D_TOV(_logi, _e_d_tov) \
	(FNIC_LOGI_E_D_TOV(_logi) = htonl(_e_d_tov))
#define FNIC_LOGI_SET_R_A_TOV(_logi, _r_a_tov) \
	(FNIC_LOGI_R_A_TOV(_logi) = htonl(_r_a_tov))

#define FNIC_STD_SET_S_ID(_fchdr, _sid)        memcpy((_fchdr)->fh_s_id, _sid, 3)
#define FNIC_STD_SET_D_ID(_fchdr, _did)        memcpy((_fchdr)->fh_d_id, _did, 3)
#define FNIC_STD_SET_OX_ID(_fchdr, _oxid)      ((_fchdr)->fh_ox_id = _oxid)
#define FNIC_STD_SET_RX_ID(_fchdr, _rxid)      ((_fchdr)->fh_rx_id = _rxid)

#define FNIC_STD_SET_R_CTL(_fchdr, _rctl)	((_fchdr)->fh_r_ctl = _rctl)
#define FNIC_STD_SET_TYPE(_fchdr, _type)	((_fchdr)->fh_type = _type)
#define FNIC_STD_SET_F_CTL(_fchdr, _fctl) \
	put_unaligned_be24(_fctl, (_fchdr)->fh_f_ctl)

#define FNIC_STD_SET_NPORT_NAME(_ptr, _wwpn)	put_unaligned_be64(_wwpn, _ptr)
#define FNIC_STD_SET_NODE_NAME(_ptr, _wwnn)	put_unaligned_be64(_wwnn, _ptr)
#define FNIC_STD_SET_PORT_ID(__req, __portid) \
	memcpy(__req->fr_fid.fp_fid, __portid, 3)
#define FNIC_STD_SET_PORT_NAME(_req, _pName) \
	(put_unaligned_be64(_pName, &_req->fr_wwn))

#define FNIC_STD_GET_OX_ID(_fchdr)		((_fchdr)->fh_ox_id)
#define FNIC_STD_GET_RX_ID(_fchdr)		((_fchdr)->fh_rx_id)
#define FNIC_STD_GET_S_ID(_fchdr)		((_fchdr)->fh_s_id)
#define FNIC_STD_GET_D_ID(_fchdr)		((_fchdr)->fh_d_id)
#define FNIC_STD_GET_TYPE(_fchdr)		((_fchdr)->fh_type)
#define FNIC_STD_GET_F_CTL(_fchdr)		((_fchdr)->fh_f_ctl)
#define FNIC_STD_GET_R_CTL(_fchdr)		((_fchdr)->fh_r_ctl)

#define FNIC_STD_GET_FC_CT_CMD(__fcct_hdr)  (be16_to_cpu(__fcct_hdr->ct_cmd))

#define FNIC_FCOE_SOF         (0x2E)
#define FNIC_FCOE_EOF         (0x42)

#define FNIC_FCOE_MAX_FRAME_SZ  (2048)
#define FNIC_FCOE_MIN_FRAME_SZ  (280)
#define FNIC_FC_MAX_PAYLOAD_LEN (2048)
#define FNIC_MIN_DATA_FIELD_SIZE  (256)
#define FNIC_R_A_TOV_DEF        (10 * 1000) /* msec */
#define FNIC_E_D_TOV_DEF        (2 * 1000)  /* msec */

#define FNIC_FC_EDTOV_NSEC    (0x400)
#define FNIC_NSEC_TO_MSEC     (0x1000000)
#define FCP_PRLI_FUNC_TARGET	(0x0010)

#define FNIC_FC_R_CTL_SOLICITED_DATA			(0x21)
#define FNIC_FC_F_CTL_LAST_END_SEQ				(0x98)
#define FNIC_FC_F_CTL_LAST_END_SEQ_INT			(0x99)
#define FNIC_FC_F_CTL_FIRST_LAST_SEQINIT		(0x29)
#define FNIC_FC_R_CTL_FC4_SCTL					(0x03)
#define FNIC_FC_CS_CTL							(0x00)

#define FNIC_FC_FRAME_UNSOLICITED(_fchdr)				\
		(_fchdr->fh_r_ctl == FC_RCTL_ELS_REQ)
#define FNIC_FC_FRAME_SOLICITED_DATA(_fchdr)			\
		(_fchdr->fh_r_ctl == FNIC_FC_R_CTL_SOLICITED_DATA)
#define FNIC_FC_FRAME_SOLICITED_CTRL_REPLY(_fchdr)		\
		(_fchdr->fh_r_ctl == FC_RCTL_ELS_REP)
#define FNIC_FC_FRAME_FCTL_LAST_END_SEQ(_fchdr)			\
		(_fchdr->fh_f_ctl[0] == FNIC_FC_F_CTL_LAST_END_SEQ)
#define FNIC_FC_FRAME_FCTL_LAST_END_SEQ_INT(_fchdr)		\
		(_fchdr->fh_f_ctl[0] == FNIC_FC_F_CTL_LAST_END_SEQ_INT)
#define FNIC_FC_FRAME_FCTL_FIRST_LAST_SEQINIT(_fchdr)	\
		(_fchdr->fh_f_ctl[0] == FNIC_FC_F_CTL_FIRST_LAST_SEQINIT)
#define FNIC_FC_FRAME_FC4_SCTL(_fchdr)					\
		(_fchdr->fh_r_ctl == FNIC_FC_R_CTL_FC4_SCTL)
#define FNIC_FC_FRAME_TYPE_BLS(_fchdr) (_fchdr->fh_type == FC_TYPE_BLS)
#define FNIC_FC_FRAME_TYPE_ELS(_fchdr) (_fchdr->fh_type == FC_TYPE_ELS)
#define FNIC_FC_FRAME_TYPE_FC_GS(_fchdr) (_fchdr->fh_type == FC_TYPE_CT)
#define FNIC_FC_FRAME_CS_CTL(_fchdr) (_fchdr->fh_cs_ctl == FNIC_FC_CS_CTL)

#define FNIC_FC_C3_RDF         (0xfff)
#define FNIC_FC_PLOGI_RSP_RDF(_plogi_rsp) \
	(MIN(_plogi_rsp->u.csp_plogi.b2b_rdf_size, \
	(_plogi_rsp->spc3[4] & FNIC_FC_C3_RDF)))
#define FNIC_FC_PLOGI_RSP_CONCUR_SEQ(_plogi_rsp) \
	(MIN(_plogi_rsp->els.fl_csp.sp_tot_seq, \
	 (be16_to_cpu(_plogi_rsp->els.fl_cssp[2].cp_con_seq) & 0xff)))

/* Frame header */
struct fnic_eth_hdr_s {
	uint8_t		dst_mac[6];
	uint8_t		src_mac[6];
	uint16_t	ether_type;
}  __packed;

struct	fnic_fcoe_hdr_s	{
	uint8_t		ver;
	uint8_t		rsvd[12];
	uint8_t		sof;
} __packed;

/* FLOGI/PLOGI struct */
struct fc_std_flogi {
	struct fc_frame_header fchdr;
	struct fc_els_flogi els;
} __packed;

#define FC_ELS_RSP_ACC_SIZE (sizeof(struct fc_frame_header) + \
		sizeof(struct fc_els_ls_acc))
#define FC_ELS_RSP_REJ_SIZE (sizeof(struct fc_frame_header) + \
		sizeof(struct fc_els_ls_rjt))

struct fc_std_els_rsp {
	struct fc_frame_header fchdr;
	union	{
	u8 rsp_cmd;
	struct fc_els_ls_acc acc;
	struct fc_els_ls_rjt rej;
	}	u;
} __packed;

struct fc_std_els_adisc {
	struct fc_frame_header fchdr;
	struct fc_els_adisc els;
} __packed;

struct fc_std_rls_acc {
	struct fc_frame_header fchdr;
	struct fc_els_rls_resp els;
} __packed;

struct fc_std_abts_ba_acc {
	struct fc_frame_header fchdr;
	struct fc_ba_acc acc;
} __packed;

struct fc_std_abts_ba_rjt {
	struct fc_frame_header fchdr;
	struct fc_ba_rjt rjt;
} __packed;

struct fc_std_els_prli {
	struct fc_frame_header fchdr;
	struct fc_els_prli els_prli;
	struct fc_els_spp sp;
}	 __packed;

struct fc_std_rpn_id {
	struct fc_frame_header fchdr;
	struct fc_ct_hdr fc_std_ct_hdr;
	struct fc_ns_rn_id rpn_id;
} __packed;

struct fc_std_fdmi_rhba {
	struct fc_frame_header fchdr;
	struct fc_ct_hdr fc_std_ct_hdr;
	uint64_t	hba_identifier;
	uint32_t	num_ports;
	uint64_t	port_name;
	uint32_t	num_hba_attributes;
	uint16_t	type_nn;
	uint16_t	length_nn;
	uint64_t	node_name;
	uint16_t	type_manu;
	uint16_t	length_manu;
	uint8_t		manufacturer[20];
	uint16_t	type_serial;
	uint16_t	length_serial;
	uint8_t		serial_num[16];
	uint16_t	type_model;
	uint16_t	length_model;
	uint8_t		model[12];
	uint16_t	type_model_des;
	uint16_t	length_model_des;
	uint8_t		model_description[56];
	uint16_t	type_hw_ver;
	uint16_t	length_hw_ver;
	uint8_t		hardware_ver[16];
	uint16_t	type_dr_ver;
	uint16_t	length_dr_ver;
	uint8_t		driver_ver[28];
	uint16_t	type_rom_ver;
	uint16_t	length_rom_ver;
	uint8_t		rom_ver[8];
	uint16_t	type_fw_ver;
	uint16_t	length_fw_ver;
	uint8_t		firmware_ver[16];
} __packed;

struct fc_std_fdmi_rpa {
	struct fc_frame_header fchdr;
	struct fc_ct_hdr fc_std_ct_hdr;
	uint64_t	port_name;
	uint32_t	num_port_attributes;
	uint16_t	type_fc4;
	uint16_t	length_fc4;
	uint8_t		fc4_type[32];
	uint16_t	type_supp_speed;
	uint16_t	length_supp_speed;
	uint32_t	supported_speed;
	uint16_t	type_cur_speed;
	uint16_t	length_cur_speed;
	uint32_t	current_speed;
	uint16_t	type_max_frame_size;
	uint16_t	length_max_frame_size;
	uint32_t	max_frame_size;
	uint16_t	type_os_name;
	uint16_t	length_os_name;
	uint8_t		os_name[16];
	uint16_t	type_host_name;
	uint16_t	length_host_name;
	uint8_t host_name[24];
}	 __packed;

struct fc_std_rft_id {
	struct fc_frame_header fchdr;
	struct fc_ct_hdr fc_std_ct_hdr;
	struct fc_ns_rft_id rft_id;
} __packed;

struct fc_std_rff_id {
	struct fc_frame_header fchdr;
	struct fc_ct_hdr fc_std_ct_hdr;
	struct fc_ns_rff_id rff_id;
} __packed;

struct fc_std_gpn_ft {
	struct fc_frame_header fchdr;
	struct fc_ct_hdr fc_std_ct_hdr;
	struct fc_ns_gid_ft gpn_ft;
} __packed;

/* Accept CT_IU	for	GPN_FT	*/
struct fc_gpn_ft_rsp_iu {
	uint8_t		ctrl;
	uint8_t		fcid[3];
	uint32_t	rsvd;
	uint64_t	wwpn;
} __packed;

struct fc_std_rls {
	struct fc_frame_header fchdr;
	struct fc_els_rls els;
} __packed;

struct fc_std_scr {
	struct fc_frame_header fchdr;
	struct fc_els_scr scr;
} __packed;

struct fc_std_rscn {
	struct fc_frame_header fchdr;
	struct fc_els_rscn els;
} __packed;

struct fc_std_logo {
	struct fc_frame_header fchdr;
	struct fc_els_logo els;
} __packed;

#define	FNIC_FCOE_FCHDR_OFFSET	\
	(sizeof(struct	fnic_eth_hdr_s)	+	sizeof(struct	fnic_fcoe_hdr_s))

#endif	/*	_FDLS_FC_H	*/
