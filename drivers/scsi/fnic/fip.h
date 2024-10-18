/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2008 Cisco Systems, Inc.  All rights reserved.
 * Copyright 2007 Nuova Systems, Inc.  All rights reserved.
 */
#ifndef _FIP_H_
#define _FIP_H_

#include "fdls_fc.h"
#include "fnic_fdls.h"

#define FCOE_ALL_FCFS_MAC {0x01, 0x10, 0x18, 0x01, 0x00, 0x02}
#define FIP_ETH_TYPE 0x8914

#define FIP_ETH_TYPE_LE 0x1489
#define FCOE_MAX_SIZE_LE 0x2E08

#define WWNN_LEN 8

#define FCOE_CTLR_FIPVLAN_TOV (3*1000)
#define FCOE_CTLR_FCS_TOV     (3*1000)
#define FCOE_CTLR_VN_KA_TOV    (90*1000)
#define FCOE_CTLR_MAX_SOL      (5*1000)

#define FIP_SUBCODE_REQ  1
#define FIP_SUBCODE_RESP 2

#define FIP_FLAG_S 0x2
#define FIP_FLAG_A 0x4

/*
 * VLAN entry.
 */
struct fcoe_vlan {
	struct list_head list;
	uint16_t vid;		/* vlan ID */
	uint16_t sol_count;	/* no. of sols sent */
	uint16_t state;		/* state */
};

enum fdls_vlan_state_e {
	FIP_VLAN_AVAIL,
	FIP_VLAN_SENT
};

enum fdls_fip_state_e {
	FDLS_FIP_INIT,
	FDLS_FIP_VLAN_DISCOVERY_STARTED,
	FDLS_FIP_FCF_DISCOVERY_STARTED,
	FDLS_FIP_FLOGI_STARTED,
	FDLS_FIP_FLOGI_COMPLETE,
};

enum fip_protocol_code_e {
	FIP_DISCOVERY = 1,
	FIP_FLOGI,
	FIP_KA_CVL,
	FIP_VLAN_DISC
};

struct eth_hdr_s {
	uint8_t dmac[6];
	uint8_t smac[6];
	uint16_t eth_type;
};

struct fip_header_s {
	uint32_t ver:16;

	uint32_t protocol:16;
	uint32_t subcode:16;

	uint32_t desc_len:16;
	uint32_t flags:16;
} __packed;

enum fip_desc_type_e {
	FIP_TYPE_PRIORITY = 1,
	FIP_TYPE_MAC,
	FIP_TYPE_FCMAP,
	FIP_TYPE_NAME_ID,
	FIP_TYPE_FABRIC,
	FIP_TYPE_MAX_FCOE,
	FIP_TYPE_FLOGI,
	FIP_TYPE_FDISC,
	FIP_TYPE_LOGO,
	FIP_TYPE_ELP,
	FIP_TYPE_VX_PORT,
	FIP_TYPE_FKA_ADV,
	FIP_TYPE_VENDOR_ID,
	FIP_TYPE_VLAN
};

struct fip_mac_desc_s {
	uint8_t type;
	uint8_t len;
	uint8_t mac[6];
} __packed;

struct fip_vlan_desc_s {
	uint8_t type;
	uint8_t len;
	uint16_t vlan;
} __packed;

struct fip_vlan_req_s {
	struct eth_hdr_s eth;
	struct fip_header_s fip;
	struct fip_mac_desc_s mac_desc;
} __packed;

 /*
  * Variables:
  * eth.smac, mac_desc.mac
  */
struct fip_vlan_req_s fip_vlan_req_tmpl = {
	.eth = {.dmac = FCOE_ALL_FCFS_MAC,
		.eth_type = FIP_ETH_TYPE_LE},
	.fip = {.ver = 0x10,
		.protocol = FIP_VLAN_DISC << 8,
		.subcode = FIP_SUBCODE_REQ << 8,
		.desc_len = 2 << 8},
	.mac_desc = {.type = FIP_TYPE_MAC, .len = 2}
};

struct fip_vlan_notif_s {
	struct fip_header_s fip;
	struct fip_vlan_desc_s vlans_desc[];
} __packed;

struct fip_vn_port_desc_s {
	uint8_t type;
	uint8_t len;
	uint8_t vn_port_mac[6];
	uint8_t rsvd[1];
	uint8_t vn_port_id[3];
	uint64_t vn_port_name;
} __packed;

struct fip_vn_port_ka_s {
	struct eth_hdr_s eth;
	struct fip_header_s fip;
	struct fip_mac_desc_s mac_desc;
	struct fip_vn_port_desc_s vn_port_desc;
} __packed;

/*
 * Variables:
 * fcf_mac, eth.smac, mac_desc.enode_mac
 * vn_port_desc:mac, id, port_name
 */
struct fip_vn_port_ka_s fip_vn_port_ka_tmpl = {
	.eth = {
		.eth_type = FIP_ETH_TYPE_LE},
	.fip = {
		.ver = 0x10,
		.protocol = FIP_KA_CVL << 8,
		.subcode = FIP_SUBCODE_REQ << 8,
		.desc_len = 7 << 8},
	.mac_desc = {.type = FIP_TYPE_MAC, .len = 2},
	.vn_port_desc = {.type = FIP_TYPE_VX_PORT, .len = 5}
};

struct fip_enode_ka_s {
	struct eth_hdr_s eth;
	struct fip_header_s fip;
	struct fip_mac_desc_s mac_desc;
} __packed;

/*
 * Variables:
 * fcf_mac, eth.smac, mac_desc.enode_mac
 */
struct fip_enode_ka_s fip_enode_ka_tmpl = {
	.eth = {
		.eth_type = FIP_ETH_TYPE_LE},
	.fip = {
		.ver = 0x10,
		.protocol = FIP_KA_CVL << 8,
		.subcode = FIP_SUBCODE_REQ << 8,
		.desc_len = 2 << 8},
	.mac_desc = {.type = FIP_TYPE_MAC, .len = 2}
};

struct fip_name_desc_s {
	uint8_t type;
	uint8_t len;
	uint8_t rsvd[2];
	uint64_t name;
} __packed;

struct fip_cvl_s {
	struct fip_header_s fip;
	struct fip_mac_desc_s fcf_mac_desc;
	struct fip_name_desc_s name_desc;
	struct fip_vn_port_desc_s vn_ports_desc[];
} __packed;

struct fip_flogi_desc_s {
	uint8_t type;
	uint8_t len;
	uint16_t rsvd;
	struct fc_std_flogi flogi;
} __packed;

struct fip_flogi_rsp_desc_s {
	uint8_t type;
	uint8_t len;
	uint16_t rsvd;
	struct fc_std_flogi flogi;
} __packed;

struct fip_flogi_s {
	struct eth_hdr_s eth;
	struct fip_header_s fip;
	struct fip_flogi_desc_s flogi_desc;
	struct fip_mac_desc_s mac_desc;
} __packed;

struct fip_flogi_rsp_s {
	struct fip_header_s fip;
	struct fip_flogi_rsp_desc_s rsp_desc;
	struct fip_mac_desc_s mac_desc;
} __packed;

/*
 * Variables:
 * fcf_mac, eth.smac, mac_desc.enode_mac
 */
struct fip_flogi_s fip_flogi_tmpl = {
	.eth = {
		.eth_type = FIP_ETH_TYPE_LE},
	.fip = {
		.ver = 0x10,
		.protocol = FIP_FLOGI << 8,
		.subcode = FIP_SUBCODE_REQ << 8,
		.desc_len = 38 << 8,
		.flags = 0x80},
	.flogi_desc = {
		       .type = FIP_TYPE_FLOGI, .len = 36,
		       .flogi = {
				 .fchdr = {
					   .fh_r_ctl = FC_RCTL_ELS_REQ,
					   .fh_d_id = {0xFF, 0xFF, 0xFE},
					   .fh_type = FC_TYPE_ELS,
					   .fh_f_ctl = {FNIC_ELS_REQ_FCTL, 0, 0},
					   .fh_rx_id = 0xFFFF},
				 .els = {
					 .fl_cmd = ELS_FLOGI,
					 .fl_csp = {
						    .sp_hi_ver =
						    FNIC_FC_PH_VER_HI,
						    .sp_lo_ver =
						    FNIC_FC_PH_VER_LO,
						    .sp_bb_cred =
						    cpu_to_be16
						    (FNIC_FC_B2B_CREDIT),
						    .sp_bb_data =
						    cpu_to_be16
						    (FNIC_FC_B2B_RDF_SZ)},
					 .fl_cssp[2].cp_class =
					 cpu_to_be16(FC_CPC_VALID | FC_CPC_SEQ)
					},
				}
		},
	.mac_desc = {.type = FIP_TYPE_MAC, .len = 2}
};

struct fip_fcoe_desc_s {
	uint8_t type;
	uint8_t len;
	uint16_t max_fcoe_size;
} __packed;

struct fip_discovery_s {
	struct eth_hdr_s eth;
	struct fip_header_s fip;
	struct fip_mac_desc_s mac_desc;
	struct fip_name_desc_s name_desc;
	struct fip_fcoe_desc_s fcoe_desc;
} __packed;

/*
 * Variables:
 * eth.smac, mac_desc.enode_mac, node_name
 */
struct fip_discovery_s fip_discovery_tmpl = {
	.eth = {.dmac = FCOE_ALL_FCFS_MAC,
		.eth_type = FIP_ETH_TYPE_LE},
	.fip = {
		.ver = 0x10, .protocol = FIP_DISCOVERY << 8,
		.subcode = FIP_SUBCODE_REQ << 8, .desc_len = 6 << 8,
		.flags = 0x80},
	.mac_desc = {.type = FIP_TYPE_MAC, .len = 2},
	.name_desc = {.type = FIP_TYPE_NAME_ID, .len = 3},
	.fcoe_desc = {
		      .type = FIP_TYPE_MAX_FCOE, .len = 1,
		      .max_fcoe_size = FCOE_MAX_SIZE_LE}
};

struct fip_prio_desc_s {
	uint8_t type;
	uint8_t len;
	uint8_t rsvd;
	uint8_t priority;
} __packed;

struct fip_fabric_desc_s {
	uint8_t type;
	uint8_t len;
	uint16_t vf_id;
	uint8_t rsvd;
	uint8_t fc_map[3];
	uint64_t fabric_name;
} __packed;

struct fip_fka_adv_desc_s {
	uint8_t type;
	uint8_t len;
	uint8_t rsvd;
	uint8_t rsvd_D;
	uint32_t fka_adv;
} __packed;

struct fip_disc_adv_s {
	struct fip_header_s fip;
	struct fip_prio_desc_s prio_desc;
	struct fip_mac_desc_s mac_desc;
	struct fip_name_desc_s name_desc;
	struct fip_fabric_desc_s fabric_desc;
	struct fip_fka_adv_desc_s fka_adv_desc;
} __packed;

void fnic_fcoe_process_vlan_resp(struct fnic *fnic, struct fip_header_s *fiph);
void fnic_fcoe_fip_discovery_resp(struct fnic *fnic, struct fip_header_s *fiph);
void fnic_fcoe_process_flogi_resp(struct fnic *fnic, struct fip_header_s *fiph);
void fnic_work_on_fip_timer(struct work_struct *work);
void fnic_work_on_fcs_ka_timer(struct work_struct *work);
void fnic_fcoe_send_vlan_req(struct fnic *fnic);
void fnic_fcoe_start_fcf_discovery(struct fnic *fnic);
void fnic_fcoe_start_flogi(struct fnic *fnic);
void fnic_fcoe_process_cvl(struct fnic *fnic, struct fip_header_s *fiph);
void fnic_vlan_discovery_timeout(struct fnic *fnic);

#endif	/* _FIP_H_ */
