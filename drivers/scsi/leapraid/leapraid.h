/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2025 LeapIO Tech Inc.
 */

#ifndef LEAPRAID_H
#define LEAPRAID_H

// #define LEAPRAID_ADAPTER_COALESCING_TEST

#define REP_POST_HOST_IDX_REG_CNT (16)

#define LEAPRAID_DB_RESET		(0x00000000)
#define LEAPRAID_DB_READY		(0x10000000)
#define LEAPRAID_DB_OPERATIONAL	(0x20000000)
#define LEAPRAID_DB_FAULT		(0x40000000)
#define LEAPRAID_DB_MASK		(0xF0000000)

#define LEAPRAID_DB_OVER_TEMPERATURE	(0x2810)

#define LEAPRAID_DB_USED				(0x08000000)
#define LEAPRAID_DB_DATA_MASK			(0x0000FFFF)
#define LEAPRAID_DB_FUNC_SHIFT			(24)
#define LEAPRAID_DB_ADD_DWORDS_SHIFT	(16)

#define LEAPRAID_DIAG_WRITE_ENABLE      (0x00000080)
#define LEAPRAID_DIAG_RESET				(0x00000004)
#define LEAPRAID_DIAG_HOLD_ADAPTER_RESET	(0x00000002)

#define LEAPRAID_HOST2ADAPTER_DB_STATUS	(0x80000000)
#define LEAPRAID_ADAPTER2HOST_DB_STATUS	(0x00000001)

#define LEAPRAID_RPHI_MSIX_IDX_SHIFT	(24)

#define LEAPRAID_REQ_DESC_FLG_SCSI_IO		(0x00)
#define LEAPRAID_REQ_DESC_FLG_HPR			(0x06)
#define LEAPRAID_REQ_DESC_FLG_DFLT_TYPE		(0x08)

#define LEAPRAID_RPY_DESC_FLG_TYPE_MASK				(0x0F)
#define LEAPRAID_RPY_DESC_FLG_SCSI_IO_SUCCESS		(0x00)
#define LEAPRAID_RPY_DESC_FLG_ADDRESS_REPLY			(0x01)
#define LEAPRAID_RPY_DESC_FLG_UNUSED				(0x0F)

#define LEAPRAID_FUNC_SCSIIO_REQ				(0x00)
#define LEAPRAID_FUNC_SCSI_TMF					(0x01)
#define LEAPRAID_FUNC_ADAPTER_INIT				(0x02)
#define LEAPRAID_FUNC_GET_ADAPTER_FEATURES		(0x03)
#define LEAPRAID_FUNC_CONFIG_OP					(0x04)
#define LEAPRAID_FUNC_SCAN_DEV					(0x06)
#define LEAPRAID_FUNC_EVENT_NOTIFY				(0x07)
#define LEAPRAID_FUNC_FW_DOWNLOAD				(0x09)
#define LEAPRAID_FUNC_FW_UPLOAD					(0x12)
#define LEAPRAID_FUNC_RAID_ACTION				(0x15)
#define LEAPRAID_FUNC_RAID_SCSIIO_PASSTHROUGH	(0x16)
#define LEAPRAID_FUNC_SCSI_ENC_PROCESSOR		(0x18)
#define LEAPRAID_FUNC_SMP_PASSTHROUGH			(0x1A)
#define LEAPRAID_FUNC_SAS_IO_UNIT_CTRL			(0x1B)
#define LEAPRAID_FUNC_SATA_PASSTHROUGH			(0x1C)
#define LEAPRAID_FUNC_ADAPTER_UNIT_RESET		(0x40)
#define LEAPRAID_FUNC_HANDSHAKE					(0x42)
#define LEAPRAID_FUNC_LOGBUF_INIT				(0x57)

#define LEAPRAID_ADAPTER_STATUS_MASK	(0x7FFF)
#define LEAPRAID_ADAPTER_STATUS_SUCCESS	(0x0000)
#define LEAPRAID_ADAPTER_STATUS_BUSY	(0x0002)
#define LEAPRAID_ADAPTER_STATUS_INTERNAL_ERROR	(0x0004)
#define LEAPRAID_ADAPTER_STATUS_INSUFFICIENT_RESOURCES	(0x0006)
#define LEAPRAID_ADAPTER_STATUS_CONFIG_INVALID_ACTION	(0x0020)
#define LEAPRAID_ADAPTER_STATUS_CONFIG_INVALID_TYPE		(0x0021)
#define LEAPRAID_ADAPTER_STATUS_CONFIG_INVALID_PAGE		(0x0022)
#define LEAPRAID_ADAPTER_STATUS_CONFIG_INVALID_DATA		(0x0023)
#define LEAPRAID_ADAPTER_STATUS_CONFIG_NO_DEFAULTS		(0x0024)
#define LEAPRAID_ADAPTER_STATUS_CONFIG_CANT_COMMIT		(0x0025)
#define LEAPRAID_ADAPTER_STATUS_SCSI_RECOVERED_ERROR	(0x0040)
#define LEAPRAID_ADAPTER_STATUS_SCSI_DEVICE_NOT_THERE	(0x0043)
#define LEAPRAID_ADAPTER_STATUS_SCSI_DATA_OVERRUN		(0x0044)
#define LEAPRAID_ADAPTER_STATUS_SCSI_DATA_UNDERRUN		(0x0045)
#define LEAPRAID_ADAPTER_STATUS_SCSI_IO_DATA_ERROR		(0x0046)
#define LEAPRAID_ADAPTER_STATUS_SCSI_PROTOCOL_ERROR		(0x0047)
#define LEAPRAID_ADAPTER_STATUS_SCSI_TASK_TERMINATED	(0x0048)
#define LEAPRAID_ADAPTER_STATUS_SCSI_RESIDUAL_MISMATCH	(0x0049)
#define LEAPRAID_ADAPTER_STATUS_SCSI_TASK_MGMT_FAILED	(0x004A)
#define LEAPRAID_ADAPTER_STATUS_SCSI_ADAPTER_TERMINATED	(0x004B)
#define LEAPRAID_ADAPTER_STATUS_SCSI_EXT_TERMINATED		(0x004C)

#define LEAPRAID_SGE_FLG_LAST_ONE		(0x80)
#define LEAPRAID_SGE_FLG_EOB			(0x40)
#define LEAPRAID_SGE_FLG_EOL			(0x01)
#define LEAPRAID_SGE_FLG_SHIFT			(24)
#define LEAPRAID_SGE_FLG_SIMPLE_ONE		(0x10)
#define LEAPRAID_SGE_FLG_SYSTEM_ADDR	(0x00)
#define LEAPRAID_SGE_FLG_H2C			(0x04)
#define LEAPRAID_SGE_FLG_32				(0x00)
#define LEAPRAID_SGE_FLG_64				(0x02)

#define LEAPRAID_IEEE_SGE_FLG_EOL			(0x40)
#define LEAPRAID_IEEE_SGE_FLG_SIMPLE_ONE	(0x00)
#define LEAPRAID_IEEE_SGE_FLG_CHAIN_ONE		(0x80)
#define LEAPRAID_IEEE_SGE_FLG_SYSTEM_ADDR	(0x00)

#define LEAPRAID_CFG_PT_IO_UNIT			(0x00)
#define LEAPRAID_CFG_PT_ADAPTER			(0x01)
#define LEAPRAID_CFG_PT_BIOS			(0x02)
#define LEAPRAID_CFG_PT_RAID_VOLUME		(0x08)
#define LEAPRAID_CFG_PT_RAID_PHYSDISK	(0x0A)
#define LEAPRAID_CFG_PT_EXTENDED		(0x0F)
#define LEAPRAID_CFG_EXTPT_SAS_IO_UNIT	(0x10)
#define LEAPRAID_CFG_EXTPT_SAS_EXP		(0x11)
#define LEAPRAID_CFG_EXTPT_SAS_DEV		(0x12)
#define LEAPRAID_CFG_EXTPT_SAS_PHY		(0x13)
#define LEAPRAID_CFG_EXTPT_ENC			(0x15)
#define LEAPRAID_CFG_EXTPT_RAID_CONFIG	(0x16)

#define LEAPRAID_SAS_CFG_PGAD_GET_NEXT_LOOP		(0x00000000)
#define LEAPRAID_SAS_ENC_CFG_PGAD_HDL			(0x10000000)
#define LEAPRAID_SAS_DEV_CFG_PGAD_HDL			(0x20000000)
#define LEAPRAID_SAS_EXP_CFG_PGAD_HDL_PHY_NUM	(0x10000000)
#define LEAPRAID_SAS_EXP_CFD_PGAD_HDL			(0x20000000)
#define LEAPRAID_SAS_EXP_CFG_PGAD_PHYNUM_SHIFT	(16)
#define LEAPRAID_RAID_VOL_CFG_PGAD_HDL			(0x10000000)
#define LEAPRAID_SAS_PHY_CFG_PGAD_PHY_NUMBER	(0x00000000)
#define LEAPRAID_PHYSDISK_CFG_PGAD_PHYSDISKNUM	(0x10000000)

#define LEAPRAID_CFG_ACT_PAGE_HEADER		(0x00)
#define LEAPRAID_CFG_ACT_PAGE_READ_CUR		(0x01)
#define LEAPRAID_CFG_ACT_PAGE_WRITE_CUR		(0x02)


#define LEAPRAID_VOL_STATE_MISSING		(0x00)
#define LEAPRAID_VOL_STATE_FAILED		(0x01)
#define LEAPRAID_VOL_STATE_INITIALIZING	(0x02)
#define LEAPRAID_VOL_STATE_ONLINE		(0x03)
#define LEAPRAID_VOL_STATE_DEGRADED		(0x04)
#define LEAPRAID_VOL_STATE_OPTIMAL		(0x05)
#define LEAPRAID_VOL_TYPE_RAID0			(0x00)
#define LEAPRAID_VOL_TYPE_RAID1E		(0x01)
#define LEAPRAID_VOL_TYPE_RAID1			(0x02)
#define LEAPRAID_VOL_TYPE_RAID10		(0x05)
#define LEAPRAID_VOL_TYPE_UNKNOWN		(0xFF)

#define LEAPRAID_SAS_NEG_LINK_RATE_MASK_PHYSICAL		(0x0F)
#define LEAPRAID_SAS_NEG_LINK_RATE_UNKNOWN_LINK_RATE	(0x00)
#define LEAPRAID_SAS_NEG_LINK_RATE_PHY_DISABLED			(0x01)
#define LEAPRAID_SAS_NEG_LINK_RATE_NEGOTIATION_FAILED	(0x02)
#define LEAPRAID_SAS_NEG_LINK_RATE_SATA_OOB_COMPLETE	(0x03)
#define LEAPRAID_SAS_NEG_LINK_RATE_PORT_SELECTOR		(0x04)
#define LEAPRAID_SAS_NEG_LINK_RATE_SMP_RESETTING		(0x05)
#define LEAPRAID_SAS_NEG_LINK_RATE_1_5					(0x08)
#define LEAPRAID_SAS_NEG_LINK_RATE_3_0					(0x09)
#define LEAPRAID_SAS_NEG_LINK_RATE_6_0					(0x0A)
#define LEAPRAID_SAS_NEG_LINK_RATE_12_0					(0x0B)

#define LEAPRAID_SAS_PHYINFO_VPHY	(0x00001000)

#define LEAPRAID_SAS_PRATE_MIN_RATE_MASK	(0x0F)
#define LEAPRAID_SAS_HWRATE_MIN_RATE_MASK	(0x0F)

#define LEAPRAID_SAS_DEV_P0_FLG_FP_CAP			(0x2000)
#define LEAPRAID_SAS_DEV_P0_FLG_SATA_SMART		(0x0040)
#define LEAPRAID_SAS_DEV_P0_FLG_ENC_LEVEL_VALID	(0x0002)
#define LEAPRAID_SAS_DEV_P0_FLG_DEV_PRESENT		(0x0001)

#define LEAPRAID_RAIDCFG_P0_EFLG_MASK_ELEMENT_TYPE		(0x000F)
#define LEAPRAID_RAIDCFG_P0_EFLG_VOL_PHYS_DISK_ELEMENT	(0x0001)
#define LEAPRAID_RAIDCFG_P0_EFLG_HOT_SPARE_ELEMENT		(0x0002)
#define LEAPRAID_RAIDCFG_P0_EFLG_OCE_ELEMENT			(0x0003)

#define LEAPRAID_SCSIIO_MSGFLG_SYSTEM_SENSE_ADDR	(0x00)

#define LEAPRAID_SCSIIO_CTRL_ADDCDBLEN_SHIFT	(26)
#define LEAPRAID_SCSIIO_CTRL_NODATATRANSFER		(0x00000000)
#define LEAPRAID_SCSIIO_CTRL_WRITE				(0x01000000)
#define LEAPRAID_SCSIIO_CTRL_READ				(0x02000000)
#define LEAPRAID_SCSIIO_CTRL_BIDIRECTIONAL		(0x03000000)
#define LEAPRAID_SCSIIO_CTRL_SIMPLEQ			(0x00000000)
#define LEAPRAID_SCSIIO_CTRL_ORDEREDQ			(0x00000200)

#define LEAPRAID_SCSI_STATUS_BUSY					(0x08)
#define LEAPRAID_SCSI_STATUS_RESERVATION_CONFLICT	(0x18)
#define LEAPRAID_SCSI_STATUS_TASK_SET_FULL			(0x28)
#define LEAPRAID_SCSI_STATE_RESPONSE_INFO_VALID		(0x10)
#define LEAPRAID_SCSI_STATE_TERMINATED				(0x08)
#define LEAPRAID_SCSI_STATE_NO_SCSI_STATUS			(0x04)
#define LEAPRAID_SCSI_STATE_AUTOSENSE_FAILED		(0x02)
#define LEAPRAID_SCSI_STATE_AUTOSENSE_VALID			(0x01)

#define LEAPRAID_TM_TASKTYPE_ABORT_TASK			(0x01)
#define LEAPRAID_TM_TASKTYPE_ABRT_TASK_SET		(0x02)
#define LEAPRAID_TM_TASKTYPE_TARGET_RESET		(0x03)
#define LEAPRAID_TM_TASKTYPE_LOGICAL_UNIT_RESET	(0x05)
#define LEAPRAID_TM_TASKTYPE_CLEAR_TASK_SET	    (0x06)
#define LEAPRAID_TM_TASKTYPE_QUERY_TASK			(0x07)
#define LEAPRAID_TM_TASKTYPE_CLEAR_ACA			(0x08)
#define LEAPRAID_TM_TASKTYPE_QUERY_TASK_SET		(0x09)
#define LEAPRAID_TM_TASKTYPE_QUERY_ASYNC_EVENT	(0x0A)

#define LEAPRAID_TM_MSGFLAGS_LINK_RESET			(0x00)
#define LEAPRAID_TM_RSP_INVALID_FRAME			(0x02)
#define LEAPRAID_TM_RSP_TM_SUCCEEDED			(0x08)
#define LEAPRAID_TM_RSP_IO_QUEUED_ON_ADAPTER	(0x80)

#define LEAPRAID_SEP_REQ_ACT_WRITE_STATUS			(0x00)
#define LEAPRAID_SEP_REQ_FLG_DEVHDL_ADDRESS			(0x00)
#define LEAPRAID_SEP_REQ_FLG_ENCLOSURE_SLOT_ADDRESS	(0x01)
#define LEAPRAID_SEP_REQ_SLOTSTATUS_PREDICTED_FAULT	(0x00000040)

#define LEAPRAID_WHOINIT_HOST_DRIVER					(0x04)
#define LEAPRAID_ADAPTER_INIT_MSGFLG_RDPQ_ARRAY_MODE	(0x01)

#define LEAPRAID_ADAPTER_FEATURES_CAP_ATOMIC_REQ	(0x00080000)
#define LEAPRAID_ADAPTER_FEATURES_CAP_RDPQ_ARRAY_CAPABLE	(0x00040000)
#define LEAPRAID_ADAPTER_FEATURES_CAP_EVENT_REPLAY	(0x00002000)
#define LEAPRAID_ADAPTER_FEATURES_CAP_INTEGRATED_RAID	(0x00001000)

#define LEAPRAID_EVT_SAS_DISCOVERY				(0x0016)

#define LEAPRAID_EVT_SAS_TOPO_CHANGE_LIST		(0x001C)
#define LEAPRAID_EVT_SAS_ENCL_DEV_STATUS_CHANGE	(0x001D)
#define LEAPRAID_EVT_SAS_DEV_STATUS_CHANGE		(0x000F)


#define LEAPRAID_EVT_TURN_ON_PFA_LED			(0xFFFC)
#define LEAPRAID_EVT_REMOVE_DEAD_DEV			(0xFFFF)

#define LEAPRAID_EVT_SAS_DEV_STAT_RC_INTERNAL_DEV_RESET		(0x08)
#define LEAPRAID_EVT_SAS_DEV_STAT_RC_CMP_INTERNAL_DEV_RESET	(0x0E)


#define LEAPRAID_EVT_IR_CHANGE                  (0x0020)

#define LEAPRAID_EVT_IR_RC_VOLUME_ADD                       (0x01)
#define LEAPRAID_EVT_IR_RC_VOLUME_DELETE                    (0x02)
#define LEAPRAID_EVT_IR_RC_PD_HIDDEN_TO_ADD                 (0x03)
#define LEAPRAID_EVT_IR_RC_PD_UNHIDDEN_TO_DELETE            (0x04)
#define LEAPRAID_EVT_IR_RC_PD_CREATED_TO_HIDE               (0x05)
#define LEAPRAID_EVT_IR_RC_PD_DELETED_TO_EXPOSE             (0x06)

#define LEAPRAID_EVT_SAS_TOPO_ES_NO_EXPANDER			(0x00)
#define LEAPRAID_EVT_SAS_TOPO_ES_ADDED					(0x01)
#define LEAPRAID_EVT_SAS_TOPO_ES_NOT_RESPONDING			(0x02)
#define LEAPRAID_EVT_SAS_TOPO_ES_RESPONDING				(0x03)

#define LEAPRAID_EVT_SAS_TOPO_RC_MASK					(0x0F)
#define LEAPRAID_EVT_SAS_TOPO_RC_TARG_ADDED				(0x01)
#define LEAPRAID_EVT_SAS_TOPO_RC_TARG_NOT_RESPONDING	(0x02)
#define LEAPRAID_EVT_SAS_TOPO_RC_PHY_CHANGED			(0x03)

#define LEAPRAID_EVT_IR_VOLUME_RC_STATE_CHANGED		(0x03)
#define LEAPRAID_EVT_IR_PHYSDISK_RC_STATE_CHANGED	(0x03)
#define LEAPRAID_EVT_IR_CHANGE_FLG_FOREIGN_CONFIG	(0x00000001)

#define LEAPRAID_EVT_SAS_DISC_RC_STARTED		(0x01)
#define LEAPRAID_EVT_SAS_DISC_RC_COMPLETED		(0x02)

#define LEAPRAID_EVT_SAS_ENCL_RC_ADDED			(0x01)
#define LEAPRAID_EVT_SAS_ENCL_RC_NOT_RESPONDING	(0x02)

#define LEAPRAID_EVT_PRIMITIVE_ASYNC_EVT	(0x04)
#define LEAPRAID_CTRL_OP_REMOVE_DEV			(0x0D)

#define LEAPRAID_DEVTYP_SEP			(0x00004000)
#define LEAPRAID_DEVTYP_SSP_TGT		(0x00000400)
#define LEAPRAID_DEVTYP_STP_TGT		(0x00000200)
#define LEAPRAID_DEVTYP_SMP_TGT		(0x00000100)
#define LEAPRAID_DEVTYP_SATA_DEV	(0x00000080)
#define LEAPRAID_DEVTYP_SSP_INIT	(0x00000040)
#define LEAPRAID_DEVTYP_STP_INIT	(0x00000020)
#define LEAPRAID_DEVTYP_SMP_INIT	(0x00000010)
#define LEAPRAID_DEVTYP_SATA_HOST	(0x00000008)

#define LEAPRAID_DEVTYP_MASK_DEV_TYPE	(0x00000007)
#define LEAPRAID_DEVTYP_NO_DEV			(0x00000000)
#define LEAPRAID_DEVTYP_END_DEV			(0x00000001)
#define LEAPRAID_DEVTYP_EDGE_EXPANDER	(0x00000002)
#define LEAPRAID_DEVTYP_FANOUT_EXPANDER	(0x00000003)

#define LEAPRAID_SAS_OP_PHY_LINK_RESET	(0x06)
#define LEAPRAID_SAS_OP_PHY_HARD_RESET	(0x07)
#define LEAPRAID_SAS_OP_SET_PARAMETER	(0x0F)

#define LEAPRAID_RAID_ACT_SYSTEM_SHUTDOWN_INITIATED		(0x20)
#define LEAPRAID_RAID_ACT_PHYSDISK_HIDDEN				(0x24)


#define LEAPRAID_BOOTDEV_FORM_MASK		(0x0F)
#define LEAPRAID_BOOTDEV_FORM_NONE		(0x00)
#define LEAPRAID_BOOTDEV_FORM_SAS_WWID	(0x05)
#define LEAPRAID_BOOTDEV_FORM_ENC_SLOT	(0x06)
#define LEAPRAID_BOOTDEV_FORM_DEV_NAME	(0x07)


struct leapraid_reg_base {
	__le32 db;
	__le32 ws;
	__le32 host_diag;
	__le32 r1[9];
	__le32 host_int_status;
	__le32 host_int_mask;
	__le32 r2[4];
	__le32 rep_msg_host_idx;
	__le32 r3[29];
	__le32 r4[2];
	__le32 atomic_req_desc_post;
	__le32 adapter_log_buf_pos;
	__le32 host_log_buf_pos;
	__le32 r5[142];
	struct leapraid_rep_post_reg_idx {
		__le32 idx;
		__le32 r1;
		__le32 r2;
		__le32 r3;
	} rep_post_reg_idx[REP_POST_HOST_IDX_REG_CNT];
} __packed;

struct leapraid_atomic_req_desc {
	u8 flg;
	u8 msix_idx;
	__le16 taskid;
};

union leapraid_rep_desc_union {
	struct leapraid_rep_desc {
		u8 rep_flg;
		u8 msix_idx;
		__le16 taskid;
		u8 r1[4];
	} dflt_rep;
	struct leapraid_add_rep_desc {
		u8 rep_flg;
		u8 msix_idx;
		__le16 taskid;
		__le32 rep_frame_addr;
	} addr_rep;
	__le64 words;
	struct {
		u32 low;
		u32 high;
	} u;
}  __packed __aligned(4);

struct leapraid_req {
	__le16 func_dep1;
	u8 r1;
	u8 func;
	u8 r2[8];
};

struct leapraid_rep {
	u8 r1[2];
	u8 msg_len;
	u8 function;
	u8 r2[10];
	__le16 adapter_status;
	u8 r3[4];
};

struct leapraid_sge_simple32 {
	__le32 flg_and_len;
	__le32 addr;
};

struct leapraid_sge_simple64 {
	__le32 flg_and_len;
	__le64 addr;
} __packed __aligned(4);

struct leapraid_sge_simple_union {
	__le32 flg_and_len;
	union {
		__le32 addr32;
		__le64 addr64;
	} u;
} __packed __aligned(4);

struct leapraid_sge_chain_union {
	__le16 len;
	u8 next_chain_offset;
	u8 flg;
	union {
		__le32 addr32;
		__le64 addr64;
	} u;
} __packed __aligned(4);

struct leapraid_ieee_sge_simple32 {
	__le32 addr;
	__le32 flg_and_len;
};

struct leapraid_ieee_sge_simple64 {
	__le64 addr;
	__le32 len;
	u8 r1[3];
	u8 flg;
} __packed __aligned(4);

union leapraid_ieee_sge_simple_union {
	struct leapraid_ieee_sge_simple32 simple32;
	struct leapraid_ieee_sge_simple64 simple64;
};

union leapraid_ieee_sge_chain_union {
	struct leapraid_ieee_sge_simple32 chain32;
	struct leapraid_ieee_sge_simple64 chain64;
};

struct leapraid_chain64_ieee_sg {
	__le64 addr;
	__le32 len;
	u8 r1[2];
	u8 next_chain_offset;
	u8 flg;
} __packed __aligned(4);

union leapraid_ieee_sge_io_union {
	struct leapraid_ieee_sge_simple64 ieee_simple;
	struct leapraid_chain64_ieee_sg ieee_chain;
};

union leapraid_simple_sge_union {
	struct leapraid_sge_simple_union leapio_simple;
	union leapraid_ieee_sge_simple_union ieee_simple;
};

union leapraid_sge_io_union {
	struct leapraid_sge_simple_union leapio_simple;
	struct leapraid_sge_chain_union leapio_chain;
	union leapraid_ieee_sge_simple_union ieee_simple;
	union leapraid_ieee_sge_chain_union ieee_chain;
};

struct leapraid_cfg_pg_header {
	u8 r1;
	u8 page_len;
	u8 page_num;
	u8 page_type;
};

struct leapraid_cfg_ext_pg_header {
	u8 r1;
	u8 r2;
	u8 page_num;
	u8 page_type;
	__le16 ext_page_len;
	u8 ext_page_type;
	u8 r3;
};

struct leapraid_cfg_req {
	u8 action;
	u8 sgl_flag;
	u8 chain_offset;
	u8 func;
	__le16 ext_page_len;
	u8 ext_page_type;
	u8 msg_flag;
	u8 r2[12];
	struct leapraid_cfg_pg_header header;
	__le32 page_addr;
	union leapraid_sge_io_union page_buf_sge;
};

struct leapraid_cfg_rep {
	u8 action;
	u8 r1;
	u8 msg_len;
	u8 func;
	__le16 ext_page_len;
	u8 ext_page_type;
	u8 msg_flag;
	u8 r2[6];
	__le16 adapter_status;
	u8 r3[4];
	struct leapraid_cfg_pg_header header;
};

struct leapraid_boot_dev_format_sas_wwid {
	__le64 sas_addr;
	u8 lun[8];
	u8 r1[8];
} __packed __aligned(4);

struct leapraid_boot_dev_format_enc_slot {
	__le64 enc_lid;
	u8 r1[8];
	__le16 slot_num;
	u8 r2[6];
} __packed __aligned(4);

struct leapraid_boot_dev_format_dev_name {
	__le64 dev_name;
	u8 lun[8];
	u8 r1[8];
} __packed __aligned(4);

union leapraid_boot_dev_format {
	struct leapraid_boot_dev_format_sas_wwid sas_wwid;
	struct leapraid_boot_dev_format_enc_slot enc_slot;
	struct leapraid_boot_dev_format_dev_name dev_name;
};

struct leapraid_bios_page2 {
	struct leapraid_cfg_pg_header header;
	u8 r1[24];
	u8 requested_boot_dev_form;
	u8 r2[3];
	union leapraid_boot_dev_format requested_boot_dev;
	u8 requested_alt_boot_dev_form;
	u8 r3[3];
	union leapraid_boot_dev_format requested_alt_boot_dev;
	u8 current_boot_dev_form;
	u8 r4[3];
	union leapraid_boot_dev_format current_boot_dev;
};

struct leapraid_bios_page3 {
	struct leapraid_cfg_pg_header header;
	u8 r1[4];
	__le32 bios_version;
	u8 r2[84];
};


struct leapraid_raidvol0_phys_disk {
	u8 r1[2];
	u8 phys_disk_num;
	u8 r2;
};

struct leapraid_raidvol_p0 {
	struct leapraid_cfg_pg_header header;
	__le16 dev_hdl;
	u8 volume_state;
	u8 volume_type;
	u8 r1[28];
	u8 num_phys_disks;
	u8 r2[3];
	struct leapraid_raidvol0_phys_disk phys_disk[];
};

struct leapraid_raidvol_p1 {
	struct leapraid_cfg_pg_header header;
	__le16 dev_hdl;
	u8 r1[42];
	__le64 wwid;
	u8 r2[8];
} __packed __aligned(4);

struct leapraid_raidpd_p0 {
	struct leapraid_cfg_pg_header header;
	__le16 dev_hdl;
	u8 r1;
	u8 phys_disk_num;
	u8 r2[112];
};

struct leapraid_sas_io_unit0_phy_info {
	u8 port;
	u8 port_flg;
	u8 phy_flg;
	u8 neg_link_rate;
	__le32 controller_phy_dev_info;
	__le16 attached_dev_hdl;
	__le16 controller_dev_hdl;
	u8 r1[8];
};

struct leapraid_sas_io_unit_p0 {
	struct leapraid_cfg_ext_pg_header header;
	u8 r1[4];
	u8 phy_num;
	u8 r2[3];
	struct leapraid_sas_io_unit0_phy_info phy_info[];
};

struct leapraid_sas_io_unit1_phy_info {
	u8 r1[12];
};

struct leapraid_sas_io_unit_page1 {
	struct leapraid_cfg_ext_pg_header header;
	u8 r1[2];
	__le16 narrowport_max_queue_depth;
	u8 r2[2];
	__le16 wideport_max_queue_depth;
	u8 r3;
	u8 sata_max_queue_depth;
	u8 r4[2];
	struct leapraid_sas_io_unit1_phy_info phy_info[];
};

struct leapraid_exp_p0 {
	struct leapraid_cfg_ext_pg_header header;
	u8 physical_port;
	u8 r1;
	__le16 enc_hdl;
	__le64 sas_address;
	u8 r2[4];
	__le16 dev_hdl;
	__le16 parent_dev_hdl;
	u8 r3[4];
	u8 phy_num;
	u8 r4[27];
} __packed __aligned(4);

struct leapraid_exp_p1 {
	struct leapraid_cfg_ext_pg_header header;
	u8 r1[8];
	u8 p_link_rate;
	u8 hw_link_rate;
	__le16 attached_dev_hdl;
	u8 r2[11];
	u8 neg_link_rate;
	u8 r3[12];
};

struct leapraid_sas_dev_p0 {
	struct leapraid_cfg_ext_pg_header header;
	__le16 slot;
	__le16 enc_hdl;
	__le64 sas_address;
	__le16 parent_dev_hdl;
	u8 phy_num;
	u8 r1;
	__le16 dev_hdl;
	u8 r2[2];
	__le32 dev_info;
	__le16 flg;
	u8 physical_port;
	u8 max_port_connections;
	__le64 dev_name;
	u8 port_groups;
	u8 r3[2];
	u8 enc_level;
	u8 connector_name[4];
	u8 r4[4];
} __packed __aligned(4);

struct leapraid_sas_phy_p0 {
	struct leapraid_cfg_ext_pg_header header;
	u8 r1[4];
	__le16 attached_dev_hdl;
	u8 r2[6];
	u8 p_link_rate;
	u8 hw_link_rate;
	u8 r3[2];
	__le32 phy_info;
	u8 neg_link_rate;
	u8 r4[3];
};

struct leapraid_enc_p0 {
	struct leapraid_cfg_ext_pg_header header;
	u8 r1[4];
	__le64 enc_lid;
	u8 r2[2];
	__le16 enc_hdl;
	u8 r3[15];
} __packed __aligned(4);

struct leapraid_raid_cfg_p0_element {
	__le16 element_flg;
	__le16 vol_dev_hdl;
	u8 r1[2];
	__le16 phys_disk_dev_hdl;
};

struct leapraid_raid_cfg_p0 {
	struct leapraid_cfg_ext_pg_header header;
	u8 r1[3];
	u8 cfg_num;
	u8 r2[32];
	u8 elements_num;
	u8 r3[3];
	struct leapraid_raid_cfg_p0_element cfg_element[];
};

union leapraid_mpi_scsi_io_cdb_union {
	u8 cdb32[32];
	struct leapraid_sge_simple_union sge;
};

struct leapraid_mpi_scsiio_req {
	__le16 dev_hdl;
	u8 chain_offset;
	u8 func;
	u8 r1[3];
	u8 msg_flg;
	u8 r2[4];
	__le32 sense_buffer_low_add;
	u8 dma_flag;
	u8 r3;
	u8 sense_buffer_len;
	u8 r4;
	u8 sgl_offset0;
	u8 sgl_offset1;
	u8 sgl_offset2;
	u8 sgl_offset3;
	__le32 skip_count;
	__le32 data_len;
	__le32 bi_dir_data_len;
	__le16 io_flg;
	__le16 eedp_flag;
	__le16 eedp_block_size;
	u8 r5[2];
	__le32 secondary_ref_tag;
	__le16 secondary_app_tag;
	__le16 app_tag_trans_mask;
	u8 lun[8];
	__le32 ctrl;
	union leapraid_mpi_scsi_io_cdb_union cdb;
	union leapraid_sge_io_union sgl;
};

union leapraid_scsi_io_cdb_union {
	u8 cdb32[32];
	struct leapraid_ieee_sge_simple64 sge;
};

struct leapraid_scsiio_req {
	__le16 dev_hdl;
	u8 chain_offset;
	u8 func;
	u8 r1[3];
	u8 msg_flg;
	u8 r2[4];
	__le32 sense_buffer_low_add;
	u8 dma_flag;
	u8 r3;
	u8 sense_buffer_len;
	u8 r4;
	u8 sgl_offset0;
	u8 sgl_offset1;
	u8 sgl_offset2;
	u8 sgl_offset3;
	__le32 skip_count;
	__le32 data_len;
	__le32 bi_dir_data_len;
	__le16 io_flg;
	__le16 eedp_flag;
	__le16 eedp_block_size;
	u8 r5[2];
	__le32 secondary_ref_tag;
	__le16 secondary_app_tag;
	__le16 app_tag_trans_mask;
	u8 lun[8];
	__le32 ctrl;
	union leapraid_scsi_io_cdb_union cdb;
	union leapraid_ieee_sge_io_union sgl;
};

struct leapraid_scsiio_rep {
	__le16 dev_hdl;
	u8 msg_len;
	u8 func;
	u8 r1[3];
	u8 msg_flg;
	u8 r2[4];
	u8 scsi_status;
	u8 scsi_state;
	__le16 adapter_status;
	u8 r3[4];
	__le32 transfer_count;
	__le32 sense_count;
	__le32 resp_info;
	__le16 task_tag;
	__le16 scsi_status_qualifier;
	__le32 bi_dir_trans_count;
	__le32 eedp_error_offset;
	__le16 eedp_observed_app_tag;
	__le16 eedp_observed_guard;
	__le32 eedp_observed_ref_tag;
};

struct leapraid_scsi_tm_req {
	__le16 dev_hdl;
	u8 chain_offset;
	u8 func;
	u8 r1;
	u8 task_type;
	u8 r2;
	u8 msg_flg;
	u8 r3[4];
	u8 lun[8];
	u8 r4[28];
	__le16 task_mid;
	u8 r5[2];
};

struct leapraid_scsi_tm_rep {
	__le16 dev_hdl;
	u8 msg_len;
	u8 func;
	u8 resp_code;
	u8 task_type;
	u8 r1;
	u8 msg_flag;
	u8 r2[6];
	__le16 adapter_status;
	u8 r3[4];
	__le32 termination_count;
	__le32 response_info;
};

struct leapraid_sep_req {
	__le16 dev_hdl;
	u8 chain_offset;
	u8 func;
	u8 act;
	u8 flg;
	u8 r1;
	u8 msg_flag;
	u8 r2[4];
	__le32 slot_status;
	u8 r3[12];
	__le16 slot;
	__le16 enc_hdl;
};

struct leapraid_sep_rep {
	__le16 dev_hdl;
	u8 msg_len;
	u8 func;
	u8 act;
	u8 flg;
	u8 r1;
	u8 msg_flag;
	u8 r2[6];
	__le16 adapter_status;
	u8 r3[4];
	__le32 slot_status;
	u8 r4[4];
	__le16 slot;
	__le16 enc_hdl;
};

struct leapraid_adapter_init_req {
	u8 who_init;
	u8 r1;
	u8 chain_offset;
	u8 func;
	u8 r2[3];
	u8 msg_flg;
	u8 r3[4];
	__le16 msg_ver;
	__le16 header_ver;
	__le32 host_buf_addr;
	u8 r4[2];
	u8 host_buf_size;
	u8 host_msix_vectors;
	u8 r6[2];
	__le16 req_frame_size;
	__le16 rep_desc_qd;
	__le16 rep_msg_qd;
	__le32 sense_buffer_add_high;
	__le32 rep_msg_dma_high;
	__le64 task_desc_base_addr;
	__le64 rep_desc_q_arr_addr;
	__le64 rep_msg_addr_dma;
	__le64 time_stamp;
} __packed __aligned(4);

struct leapraid_rep_desc_q_arr {
	__le64 rep_desc_base_addr;
	__le64 r1;
} __packed __aligned(4);

struct leapraid_adapter_init_rep {
	u8 who_init;
	u8 r1;
	u8 msg_len;
	u8 func;
	u8 r2[3];
	u8 msg_flag;
	u8 r3[6];
	__le16 adapter_status;
	u8 r4[4];
};

struct leapraid_adapter_log_req {
	u8 action;
	u8 type;
	u8 chain_offset;
	u8 func;
	u8 r2[3];
	u8 msg_flag;
	u8 r3[4];
	union {
		u8		b[12];
		__le16	s[6];
		__le32	w[3];
	} mbox;
	struct leapraid_sge_simple64 sge;
} __packed __aligned(4);

struct leapraid_adapter_log_rep {
	u8 action;
	u8 type;
	u8 msg_len;
	u8 func;
	u8 r2[3];
	u8 msg_flag;
	u8 r3[6];
	__le16 adapter_status;
};

struct leapraid_adapter_features_req {
	u8 r1[2];
	u8 chain_offset;
	u8 func;
	u8 r2[3];
	u8 msg_flag;
	u8 r3[4];
};

struct leapraid_adapter_features_rep {
	u16 msg_ver;
	u8 msg_len;
	u8 func;
	u16 header_ver;
	u8 r1;
	u8 msg_flag;
	u8 r2[6];
	u16 adapter_status;
	u8 r3[4];
	u8 sata_max_qdepth;
	u8 who_init;
	u8 r4;
	u8 max_msix_vectors;
	__le16 req_slot;
	u8 r5[2];
	__le32 adapter_caps;
	__le32 fw_version;
	__le16 sas_wide_max_qdepth;
	__le16 sas_narrow_max_qdepth;
	u8 r6[10];
	__le16 hp_slot;
	u8 r7[3];
	u8 max_volumes;
	__le16 max_dev_hdl;
	u8 r8[2];
	__le16 min_dev_hdl;
	u8 r9[6];
};

struct leapraid_scan_dev_req {
	u8 r1[2];
	u8 chain_offset;
	u8 func;
	u8 r2[3];
	u8 msg_flag;
	u8 r3[4];
};

struct leapraid_scan_dev_rep {
	u8 r1[2];
	u8 msg_len;
	u8 func;
	u8 r2[3];
	u8 msg_flag;
	u8 r3[6];
	__le16 adapter_status;
	u8 r4[4];
};

struct leapraid_evt_notify_req {
	u8 r1[2];
	u8 chain_offset;
	u8 func;
	u8 r2[3];
	u8 msg_flag;
	u8 r3[12];
	__le32 evt_masks[4];
	u8 r4[8];
};

struct leapraid_evt_notify_rep {
	__le16 evt_data_len;
	u8 msg_len;
	u8 func;
	u8 r1[2];
	u8 r2;
	u8 msg_flag;
	u8 r3[6];
	__le16 adapter_status;
	u8 r4[4];
	__le16 evt;
	u8 r5[6];
	__le32 evt_data[];
};

struct leapraid_evt_data_sas_dev_status_change {
	__le16 task_tag;
	u8 reason_code;
	u8 physical_port;
	u8 r1[2];
	__le16 dev_hdl;
	u8 r2[4];
	__le64 sas_address;
	u8 lun[8];
} __packed __aligned(4);
struct leapraid_evt_data_ir_change {
	u8 r1;
	u8 reason_code;
	u8 r2[2];
	__le16 vol_dev_hdl;
	__le16 phys_disk_dev_hdl;
};

struct leapraid_evt_data_sas_disc {
	u8 r1;
	u8 reason_code;
	u8 physical_port;
	u8 r2[5];
};


struct leapraid_evt_sas_topo_phy_entry {
	__le16 attached_dev_hdl;
	u8 link_rate;
	u8 phy_status;
};

struct leapraid_evt_data_sas_topo_change_list {
	__le16 encl_hdl;
	__le16 exp_dev_hdl;
	u8 num_phys;
	u8 r2[3];
	u8 entry_num;
	u8 start_phy_num;
	u8 exp_status;
	u8 physical_port;
	struct leapraid_evt_sas_topo_phy_entry phy[];
};

struct leapraid_evt_data_sas_enc_dev_status_change {
	__le16 enc_hdl;
	u8 reason_code;
	u8 physical_port;
	__le64 encl_logical_id;
	__le16 num_slots;
	__le16 start_slot;
	__le32 phy_bits;
};

struct leapraid_io_unit_ctrl_req {
	u8 op;
	u8 r1;
	u8 chain_offset;
	u8 func;
	u16 dev_hdl;
	u8 adapter_para;
	u8 msg_flag;
	u8 r2[6];
	u8 phy_num;
	u8 r3[17];
	__le32 adapter_para_value;
	__le32 adapter_para_value2;
	u8 r4[4];
};

struct leapraid_io_unit_ctrl_rep {
	u8 op;
	u8 r1[2];
	u8 func;
	__le16 dev_hdl;
	u8 r2[14];
};

struct leapraid_raid_act_req {
	u8 act;
	u8 r1[2];
	u8 func;
	u8 r2[2];
	u8 phys_disk_num;
	u8 r3[13];
	struct leapraid_sge_simple_union action_data_sge;
};

struct leapraid_raid_act_rep {
	u8 act;
	u8 r1[2];
	u8 func;
	__le16 vol_dev_hdl;
	u8 r2[8];
	__le16 adapter_status;
	u8 r3[76];
};

struct leapraid_smp_passthrough_req {
	u8 passthrough_flg;
	u8 physical_port;
	u8 r1;
	u8 func;
	__le16 req_data_len;
	u8 r2[10];
	__le64 sas_address;
	u8 r3[8];
	union leapraid_simple_sge_union sgl;
} __packed __aligned(4);

struct leapraid_smp_passthrough_rep {
	u8 passthrough_flg;
	u8 physical_port;
	u8 r1;
	u8 func;
	__le16 resp_data_len;
	u8 r2[8];
	__le16 adapter_status;
	u8 r3[12];
};

struct leapraid_sas_io_unit_ctrl_req {
	u8 op;
	u8 r1[2];
	u8 func;
	__le16 dev_hdl;
	u8 r2[38];
};

struct leapraid_sas_io_unit_ctrl_rep {
	u8 op;
	u8 r1[2];
	u8 func;
	__le16 dev_hdl;
	u8 r2[8];
	__le16 adapter_status;
	u8 r3[4];
};

#endif /* LEAPRAID_H */
