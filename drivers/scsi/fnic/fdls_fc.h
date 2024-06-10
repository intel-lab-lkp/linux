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

#define MIN(x, y) (x < y ? x : y)

#define FNIC_FCP_SP_RD_XRDY_DIS 0x00000002
#define FNIC_FCP_SP_TARGET      0x00000010
#define FNIC_FCP_SP_INITIATOR   0x00000020
#define FNIC_FCP_SP_CONF_CMPL   0x00000080
#define FNIC_FCP_SP_RETRY       0x00000100

#ifdef _BIG_ENDIAN

#define FNIC_FLOGI_OXID        (0x1001)
#define FNIC_PLOGI_FABRIC_OXID (0x1002)
#define FNIC_RPN_REQ_OXID      (0x1003)
#define FNIC_GPN_FT_OXID       (0x1004)
#define FNIC_SCR_REQ_OXID      (0x1005)
#define FNIC_RSCN_RESP_OXID    (0x1006)
#define FNIC_LOGO_REQ_OXID     (0x1007)
#define FNIC_LOGO_RESP_OXID    (0x1008)
#define FNIC_RFT_REQ_OXID      (0x100a)
#define FNIC_RFF_REQ_OXID      (0x100b)
#define FNIC_ECHO_RESP_OXID    (0x100c)
#define FNIC_ADISC_RESP_OXID   (0x100d)
#define FNIC_FDMI_PLOGI_OXID   (0x100e)
#define FNIC_FDMI_REG_HBA_OXID (0X100f)
#define FNIC_FDMI_RPA_OXID     (0X1010)
#define FNIC_ELS_REQ_FCTL      (0x290000)
#define FNIC_ELS_REP_FCTL      (0x990000)

#define FNIC_FC_PH_VER         (0x2020)
#define FNIC_FC_B2B_CREDIT     (0x000A)
#define FNIC_FC_B2B_RDF_SZ     (0x0800)

#define FNIC_REQ_ABTS_FCTL     (0x090000)

#define FNIC_FC_FEATURES       (0x8000)

#define FNIC_FC_CONCUR_SEQS    (0x00FF)

#define FNIC_FC_RO_INFO        (0x001F)
#define FNIC_E_D_TOV           (0x07D0)

#define FC_CT_RPN_CMD          (0x0212)
#define FC_CT_GPN_FT_CMD       (0x0172)
#define FC_CT_ACC              (0x8002)
#define FC_CT_REJ              (0x8001)
#define FC_CT_RFT_CMD          (0x1702)
#define FC_CT_RFF_CMD          (0x1F02)

#define FNIC_FCP_RSP_FCTL      (0x990000)

#else /* _LITTLE_ENDIAN */

#define FNIC_FLOGI_OXID        (0x0110)
#define FNIC_PLOGI_FABRIC_OXID (0x0210)
#define FNIC_RPN_REQ_OXID      (0x0310)
#define FNIC_GPN_FT_OXID       (0x0410)
#define FNIC_SCR_REQ_OXID      (0x0510)
#define FNIC_RSCN_RESP_OXID    (0x0610)
#define FNIC_TLOGO_REQ_OXID    (0x0710)
#define FNIC_FLOGO_REQ_OXID    (0x0711)
#define FNIC_LOGO_RESP_OXID    (0x0810)
#define FNIC_PLOGI_RESP_OXID   (0x0910)
#define FNIC_RFT_REQ_OXID      (0x0a10)
#define FNIC_RFF_REQ_OXID      (0x0b10)
#define FNIC_ECHO_RESP_OXID    (0x0c10)
#define FNIC_UNSUPPORTED_RESP_OXID   (0xffff)
#define FNIC_ADISC_RESP_OXID    (0x0d10)
#define FNIC_PLOGI_FDMI_OXID    (0x0e10)
#define FNIC_FDMI_REG_HBA_OXID  (0X0f10)
#define FNIC_FDMI_RPA_OXID      (0X1010)
#define FNIC_ELS_REQ_FCTL      (0x000029)
#define FNIC_ELS_REP_FCTL      (0x000099)

#define FNIC_FCP_RSP_FCTL      (0x000099)
#define FNIC_REQ_ABTS_FCTL     (0x000009)

#define FNIC_FC_PH_VER         (0x2020)
#define FNIC_FC_B2B_CREDIT     (0x0A00)
#define FNIC_FC_B2B_RDF_SZ     (0x0008)

#define FNIC_FC_FEATURES       (0x0080)

#define FNIC_FC_CONCUR_SEQS    (0xFF00)
#define FNIC_FC_RO_INFO        (0x1F00)
#define FNIC_E_D_TOV           (0xD0070000)

#define FC_CT_RPN_CMD          (0x1202)
#define FC_CT_GPN_FT_CMD       (0x7201)
#define FC_CT_ACC              (0x0280)
#define FC_CT_REJ              (0x0180)
#define FC_CT_RFT_CMD          (0x1702)
#define FC_CT_RFF_CMD          (0x1F02)

#endif

#define ETH_TYPE_FCOE			0x8906
#define ETH_TYPE_FIP			0x8914

#define FC_DIR_SERVER          0xFFFFFC
#define FC_FABRIC_CONTROLLER   0xFFFFFD
#define FC_DOMAIN_CONTR        0xFFFFFE

#define FNIC_FC_GPN_LAST_ENTRY (0x80)

#define FC_ELS_FLOGI_REQ        0x04
#define FC_LS_REJ               0x01
#define FC_LS_ACC               0x02
#define FC_ELS_PLOGI_REQ        0x03
#define FC_ELS_ECHO_REQ         0x10
#define FC_ELS_PRLI_REQ         0x20
#define FC_ELS_SCR              0x62
#define FC_ELS_RLS_REQ          0x0F
#define FC_ELS_RRQ_REQ          0x12
#define FC_ELS_LOGO             0x05
#define FC_ELS_RSCN             0x61
#define FNIC_BA_ACC_RCTL        0x84
#define FNIC_BA_RJT_RCTL        0x85
#define FC_ABTS_RCTL            0x81
#define FNIC_ELS_ADISC_REQ      0x52
#define FC_ELS_RJT_LOGICAL_BUSY 0x05
#define FC_ELS_RJT_BUSY         0x09
#define FC_ELS_RTV_REQ          0x0E

/* FNIC FDMI Register HBA Macros */
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

/* FNIC FDMI Register PA Macros */
#define FNIC_FDMI_TYPE_FC4_TYPES	0X100
#define FNIC_FDMI_TYPE_SUPPORTED_SPEEDS 0X200
#define FNIC_FDMI_TYPE_CURRENT_SPEED	0X300
#define FNIC_FDMI_TYPE_MAX_FRAME_SIZE	0X400
#define FNIC_FDMI_TYPE_OS_NAME		0X500
#define FNIC_FDMI_TYPE_HOST_NAME	0X600

#define FNIC_SET_S_ID(_fchdr, _sid)        memcpy(_fchdr->sid, _sid, 3)
#define FNIC_SET_NPORT_NAME(_req, _pName)  (_req.nport_name = htonll(_pName))
#define FNIC_SET_NODE_NAME(_req, _pName)   (_req.node_name = htonll(_pName))
#define FNIC_SET_RDF_SIZE(_req, _rdf_size)	\
	(_req.b2b_rdf_size = htons(_rdf_size))
#define FNIC_SET_R_A_TOV(_req, _r_a_tov)   (_req.r_a_tov = htonl(_r_a_tov))
#define FNIC_SET_E_D_TOV(_req, _e_d_tov)   (_req.e_d_tov = htonl(_e_d_tov))
#define FNIC_SET_D_ID(_fchdr, _did)        memcpy(_fchdr->did, _did, 3)
#define FNIC_SET_OX_ID(_fchdr, _oxid)      (_fchdr->ox_id = _oxid)
#define FNIC_SET_RX_ID(_fchdr, _rxid)      (_fchdr->rx_id = _rxid)

#define FNIC_SET_PORT_ID(__req, __portid) \
	memcpy(__req->port_id, __portid, 3)
#define FNIC_SET_RPN_PORT_ID(__req, __portid) \
	memcpy(__req->port_id, __portid, 3)
#define FNIC_SET_RPN_PORT_NAME(_req, _pName) \
	(_req->port_name = htonll(_pName))

#define FNIC_GET_S_ID(_fchdr)        (_fchdr->sid)
#define FNIC_GET_D_ID(_fchdr)        (_fchdr->did)
#define FNIC_GET_OX_ID(_fchdr)       (_fchdr->ox_id)

#define FNIC_GET_FC_CT_CMD(__fcct_hdr)  (__fcct_hdr->command)

#define FNIC_FCOE_SOF         (0x2E)
#define FNIC_FCOE_EOF         (0x42)

#define FNIC_GET_FC_TYPE(_fchdr)        (_fchdr->type)
#define FNIC_GET_FC_RCTL(_fchdr)        (_fchdr->r_ctl)

#define FNIC_FC_TYPE_ELS        (0x01)
#define FNIC_FC_R_ELS_REQ       (0x22)
#define FNIC_FC_R_ELS_RSP       (0x23)

#define FNIC_FCOE_MAX_FRAME_SZ  (2048)
#define FNIC_FCOE_MIN_FRAME_SZ  (280)
#define FNIC_FC_MAX_PAYLOAD_LEN (2048)
#define FNIC_MIN_DATA_FIELD_SIZE  (256)
#define FNIC_R_A_TOV_DEF        (10 * 1000) /* msec */
#define FNIC_E_D_TOV_DEF        (2 * 1000)  /* msec */

#define FNIC_FC_EDTOV_NSEC    (0x400)
#define FNIC_NSEC_TO_MSEC     (0x1000000)
#define FCP_PRLI_FUNC_TARGET	(0x0010)
#define FC_CT_RJT_LOGICAL_BUSY 0x5
#define FC_CT_RJT_BUSY         0x9

#define FNIC_FC_FRAME_UNSOLICITED(_fchdr)  (_fchdr->r_ctl == 0x22)
#define FNIC_FC_FRAME_SOLICITED_DATA(_fchdr)    (_fchdr->r_ctl == 0x21)
#define FNIC_FC_FRAME_SOLICITED_CTRL_REPLY(_fchdr)    (_fchdr->r_ctl == 0x23)
#define FNIC_FC_FRAME_FCTL_LAST_END_SEQ(_fchdr)    (_fchdr->f_ctl == 0x98)
#define FNIC_FC_FRAME_FCTL_LAST_END_SEQ_INT(_fchdr)    (_fchdr->f_ctl == 0x99)
#define FNIC_FC_FRAME_FCTL_FIRST_LAST_SEQINIT(_fchdr)  (_fchdr->f_ctl == 0x29)
#define FNIC_FC_FRAME_FC4_SCTL(_fchdr)    (_fchdr->r_ctl == 0x03)
#define FNIC_FC_FRAME_TYPE_BLS(_fchdr) (_fchdr->type == 0x00)
#define FNIC_FC_FRAME_TYPE_ELS(_fchdr) (_fchdr->type == 0x01)
#define FNIC_FC_FRAME_TYPE_FC_GS(_fchdr) (_fchdr->type == 0x20)
#define FNIC_FC_FRAME_CS_CTL(_fchdr) (_fchdr->cs_ctl == 0x00)

#define FNIC_FC_C3_RDF         (0xfff)
#define FNIC_FC_PLOGI_RSP_RDF(_plogi_rsp) \
	(MIN(_plogi_rsp->u.csp_plogi.b2b_rdf_size, \
	(_plogi_rsp->spc3[4] & FNIC_FC_C3_RDF)))
#define FNIC_FC_PLOGI_RSP_CONCUR_SEQ(_plogi_rsp) \
	(MIN(_plogi_rsp->u.csp_plogi.total_concur_seqs, \
	(uint8_t)(_plogi_rsp->spc3[10] & 0xff)))

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

/*	Big	endian	*/
struct	fc_hdr_s	{
	uint8_t		r_ctl;
	uint8_t		did[3];
	uint8_t		cs_ctl:8;
	uint8_t		sid[3];
	uint32_t	type:8;
	uint32_t	f_ctl:24;
	uint8_t		seq_id;
	uint8_t		df_ctl;
	uint16_t	seq_cnt;
	uint16_t	ox_id;
	uint16_t	rx_id;
	uint32_t	param;
} __packed;

struct	fc_csp_flogi_s	{
	uint16_t	fc_ph_ver;
	uint16_t	b2b_credits;
	uint16_t	features;
	uint16_t	b2b_rdf_size;
	uint32_t	r_a_tov;
	uint32_t	e_d_tov;
} __packed;

struct	fc_csp_plogi_s	{
	uint16_t	fc_ph_ver;
	uint16_t	b2b_credits;
	uint16_t	features;
	uint16_t	b2b_rdf_size;
	uint16_t	total_concur_seqs;
	uint16_t	ro_info;
	uint32_t	e_d_tov;
} __packed;

struct	fc_els_s	{
	struct	fc_hdr_s	fchdr;
	uint8_t		command;
	uint8_t		rsvd[3];
	union	{
	struct	fc_csp_flogi_s	csp_flogi;
	struct	fc_csp_plogi_s	csp_plogi;
	}	u;
	uint64_t	nport_name;
	uint64_t	node_name;
	uint8_t		spc1[16];
	uint8_t		spc2[16];
	uint8_t		spc3[16];
	uint8_t		spc4[16];
	uint8_t		vendor_ver_level[16];
}	 __packed;

struct	fc_els_acc_s	{
	struct	fc_hdr_s	fchdr;
	uint8_t		command;
	uint8_t		rsvd[3];
}	 __packed;

struct	fc_els_reject_s	{
	struct	fc_hdr_s	fchdr;
	uint32_t	command;
	uint8_t		reserved;
	uint8_t		reason_code;
	uint8_t		reason_expl;
	uint8_t		vendor_specific;
}	 __packed;

struct	fc_els_adisc_s	{
	struct	fc_hdr_s	fchdr;
	uint8_t		command;
	uint8_t		zeros[3];
	uint32_t	unused;
	uint64_t	nport_name;
	uint64_t	node_name;
	uint8_t		reserved;
	uint8_t		fcid[3];
} __packed;

struct	fc_els_rls_ls_acc_s	{
	struct	fc_hdr_s	fchdr;
	uint8_t		command;
	uint8_t		reserved[3];
	uint32_t	link_fail_count;	/*	link	failure	count	*/
	uint32_t	sync_loss_count;	/*	loss	of	synchronization	count	*/
	uint32_t	sig_loss_count;	/*	loss	of	signal	count	*/
	uint32_t	prim_err_count;	/*	primitive	sequence	error	count	*/
	uint32_t	inv_word_count;	/*	invalid	transmission	word	count	*/
	uint32_t	inv_crc_count;	/*	invalid	CRC	count	*/
} __packed;

struct	fc_els_adisc_ls_acc_s	{
	struct	fc_hdr_s	fchdr;
	uint8_t		command;
	uint8_t		zeros[3];
	uint32_t	unused;
	uint64_t	nport_name;
	uint64_t	node_name;
	uint8_t		reserved;
	uint8_t		fcid[3];
} __packed;

struct	fc_abts_ba_acc_s	{
	struct	fc_hdr_s	fchdr;
	uint8_t		seq_id_validity;
	uint8_t		seq_id;
	uint16_t	reserved;
	uint16_t	ox_id;
	uint16_t	rx_id;
	uint16_t	low_seq_cnt;
	uint16_t	high_seq_cnt;
} __packed;

struct	fc_abts_ba_rjt_s	{
	struct	fc_hdr_s	fchdr;
	uint8_t		vend_uniq;
	uint8_t		reason_explanation;
	uint8_t		reason_code;
	uint8_t		reserved;
} __packed;

struct	fc_prli_sp_s	{
	uint8_t		type;
	uint8_t		type_ext;
	uint16_t	flags;

	uint32_t	ox_proc_assoc;
	uint32_t	rx_proc_assoc;
	uint32_t	csp;
}	 __packed;

struct	fc_els_prli_s	{
	struct	fc_hdr_s	fchdr;
	uint8_t		command;
	uint8_t		page_len;
	uint16_t	payload_len;
	struct	fc_prli_sp_s	sp;
}	 __packed;

struct	fc_ct_hdr_s	{
	uint32_t	rev:	8;
	uint32_t	in_id:	24;
	uint8_t		fs_type;
	uint8_t		fs_subtype;
	uint8_t		options;
	uint8_t		rsvd;
	uint16_t	command;
	uint16_t	max_res_size;
	uint8_t		rsvd1;
	uint8_t		reason_code;
	uint8_t		reason_expl;
	uint8_t		vendor_specific;
}	 __packed;

struct	fc_rpn_id_s	{
	struct	fc_hdr_s	fchdr;
	struct	fc_ct_hdr_s	fc_ct_hdr;
	uint8_t		rsvd;
	uint8_t		port_id[3];
	uint64_t	port_name;
}	 __packed;

struct	fc_fdmi_rhba_s	{
	struct	fc_hdr_s	fchdr;
	struct	fc_ct_hdr_s	fc_ct_hdr;
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

struct	fc_fdmi_rpa_s	{
	struct	fc_hdr_s	fchdr;
	struct	fc_ct_hdr_s	fc_ct_hdr;
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
	uint8_t		host_name[12];
}	 __packed;

struct	fc_rft_id	{
	struct	fc_hdr_s fchdr;
	struct	fc_ct_hdr_s	fc_ct_hdr;
	uint8_t		rsvd;
	uint8_t		port_id[3];
	uint8_t		fc4_types[32];
} __packed;

struct	fc_rff_id	{
	struct	fc_hdr_s fchdr;
	struct	fc_ct_hdr_s	fc_ct_hdr;
	uint8_t		rsvd;
	uint8_t		port_id[3];
	uint8_t		rsvd1;
	uint8_t		rsvd2;
	uint8_t		tgt;
	uint8_t		fc4_type;
} __packed;

/*
 *	Variables:
 *	sid
 */
struct	fc_gpn_ft_s	{
	struct	fc_hdr_s	fchdr;
	struct	fc_ct_hdr_s	fc_ct_hdr;
	uint8_t		rsvd[3];
	uint8_t		fc4_type;
} __packed;

/* Accept CT_IU	for	GPN_FT	*/
struct	fc_gpn_ft_rsp_iu_s	{
	uint8_t		ctrl;
	uint8_t		fcid[3];
	uint32_t	rsvd;
	uint64_t	wwpn;
} __packed;

/*
 *	Variables:
 *	sid
 */
struct	fc_scr_s	{
	struct	fc_hdr_s	fchdr;
	uint8_t		command;
	uint8_t		rsvd[3];
	uint8_t		rsvd1[3];
	uint8_t		reg_func;
} __packed;

struct	fc_rscn_hdr_s	{
	struct	fc_hdr_s	fchdr;
	uint8_t		command;
	uint8_t		page_len;
	uint16_t	payload_len;
} __packed;


struct	fc_rscn_port_s	{
	uint8_t		addr_format:2;
	uint8_t		rscn_evt_q:4;
	uint8_t		reserved:2;
	uint8_t		port_id[3];
} __packed;

struct	fc_logo_req_s	{
	struct fc_hdr_s	fchdr;
	uint8_t		command;
	uint8_t		rsvd[3];
	uint8_t		rsvd1;
	uint8_t		fcid[3];
	uint64_t	wwpn;
} __packed;

#define	FNIC_FCOE_FCHDR_OFFSET	\
	(sizeof(struct	fnic_eth_hdr_s)	+	sizeof(struct	fnic_fcoe_hdr_s))

#endif	/*	_FDLS_FC_H	*/
