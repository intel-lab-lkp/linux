/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/*
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 * Copyright(c) 2015 - 2020 Intel Corporation.
 */

#ifndef _CHIP_H
#define _CHIP_H
/*
 * This file contains all of the defines that is specific to the HFI chip
 */

/* sizes */
#define BITS_PER_REGISTER (BITS_PER_BYTE * sizeof(u64))
#define NUM_INTERRUPT_SOURCES 768
#define RXE_NUM_CONTEXTS 160
#define RXE_NUM_TID_FLOWS 32
#define RXE_NUM_DATA_VL 8
#define TXE_NUM_SDMA_ENGINES 16
#define NUM_CONTEXTS_PER_SET 8
#define VL_ARB_HIGH_PRIO_TABLE_SIZE 16
#define VL_ARB_LOW_PRIO_TABLE_SIZE 16
#define VL_ARB_TABLE_SIZE 16
#define TXE_NUM_32_BIT_COUNTER 7
#define TXE_NUM_64_BIT_COUNTER 30
#define TXE_NUM_DATA_VL 8
#define TXE_PIO_SIZE (32 * 0x100000) /* 32 MB */
#define RCV_ARRAY_SIZE (64 * 1024 * 8) /* 64K entries of 8 bytes = 512 KB */
#define PIO_BLOCK_SIZE 64 /* bytes */
#define SDMA_BLOCK_SIZE 64 /* bytes */
#define RCV_BUF_BLOCK_SIZE 64 /* bytes */
#define PIO_CMASK 0x7ff /* counter mask for free and fill counters */
#define WFR_MAX_EAGER_ENTRIES 2048 /* max receive eager entries */
#define MAX_TID_PAIR_ENTRIES 1024 /* max receive expected pairs */
/*
 * Virtual? Allocation Unit, defined as AU = 8*2^vAU, 64 bytes, AU is fixed
 * at 64 bytes for all generation one devices
 */
#define CM_VAU 3
/* HFI link credit count, AKA receive buffer depth (RBUF_DEPTH) */
#define CM_GLOBAL_CREDITS 0x880
/* Number of PKey entries in the WFR HW */
#define WFR_MAX_PKEY_VALUES 16
/* number of SendCtxtCtrl.CtxtBase bits in the WFR HW */
#define WFR_PIO_BASE_BITS 14

#include "chip_registers.h"

#define TXE_PIO_SEND (TXE + TXE_PIO_SEND_OFFSET)

/* register strides */
#define WFR_RXE_IPRC_STRIDE 0x100
#define WFR_RXE_RCTXT_STRIDE 0x100
#define WFR_RXE_KCTXT_STRIDE 0x100
#define WFR_RXE_UCTXT_STRIDE 0x1000
#define WFR_TXE_SCTXT_STRIDE 0x100
#define WFR_TXE_TCTXT_STRIDE 0x100
#define WFR_TXE_SDMA_STRIDE 0x100
#define WFR_TXE_SDMACFG_STRIDE 0x100
#define WFR_TXE_EPSC_STRIDE 0x100

/* PBC flags */
#define PBC_INTR BIT_ULL(31)
#define PBC_9B_SC4_SHIFT (30) /* aka PBC_DC_INFO */
#define PBC_9B_SC4 BIT_ULL(PBC_9B_SC4_SHIFT)
#define PBC_TEST_EBP BIT_ULL(29)
#define PBC_PACKET_BYPASS BIT_ULL(28) /* WFR only */
#define PBC_CREDIT_RETURN BIT_ULL(25)
#define PBC_INSERT_BYPASS_ICRC BIT_ULL(24)
#define PBC_TEST_BAD_ICRC BIT_ULL(23)
#define PBC_FECN BIT_ULL(22)

/* return PBC flag for bit sc[4] */
static inline u64 pbc_sc4_flag(u16 sc5)
{
	return (u64)ib_is_sc5(sc5) << PBC_9B_SC4_SHIFT;
}

/* PBC L2 types */
#define PBC_L2_16B 2 /* 16B header */
#define PBC_L2_9B 3 /* 9B header */

/* PbcInsertHcrc field settings */
#define PBC_IHCRC_LKDETH 0x0 /* insert @ local KDETH offset */
#define PBC_IHCRC_GKDETH 0x1 /* insert @ global KDETH offset */
#define PBC_IHCRC_NONE 0x2 /* no HCRC inserted */

/* WFR PBC fields */
#define PBC_STATIC_RATE_CONTROL_COUNT_SHIFT 32
#define PBC_STATIC_RATE_CONTROL_COUNT_MASK 0xffffull
#define PBC_STATIC_RATE_CONTROL_COUNT_SMASK \
	(PBC_STATIC_RATE_CONTROL_COUNT_MASK \
	 << PBC_STATIC_RATE_CONTROL_COUNT_SHIFT)

/* JKR and beyond PBC fields */
#define PBC_SEND_CTXT_SHIFT 56
#define PBC_DLID_SHIFT 32
#define PBC_DLID_MASK 0xffffff
#define PBC_L2_TYPE_SHIFT 20
#define PBC_PORT_IDX_SHIFT 16

/* common PBC fields */
#define PBC_INSERT_HCRC_SHIFT 26
#define PBC_INSERT_HCRC_MASK 0x3ull
#define PBC_INSERT_HCRC_SMASK (PBC_INSERT_HCRC_MASK << PBC_INSERT_HCRC_SHIFT)

#define PBC_VL_SHIFT 12
#define PBC_VL_MASK 0xfull
#define PBC_VL_SMASK (PBC_VL_MASK << PBC_VL_SHIFT)

#define PBC_LENGTH_DWS_SHIFT 0
#define PBC_LENGTH_DWS_MASK 0xfffull
#define PBC_LENGTH_DWS_SMASK (PBC_LENGTH_DWS_MASK << PBC_LENGTH_DWS_SHIFT)

/* Credit Return Fields */
#define CR_COUNTER_SHIFT 0
#define CR_COUNTER_MASK 0x7ffull
#define CR_COUNTER_SMASK (CR_COUNTER_MASK << CR_COUNTER_SHIFT)

#define CR_STATUS_SHIFT 11
#define CR_STATUS_MASK 0x1ull
#define CR_STATUS_SMASK (CR_STATUS_MASK << CR_STATUS_SHIFT)

#define CR_CREDIT_RETURN_DUE_TO_PBC_SHIFT 12
#define CR_CREDIT_RETURN_DUE_TO_PBC_MASK 0x1ull
#define CR_CREDIT_RETURN_DUE_TO_PBC_SMASK \
	(CR_CREDIT_RETURN_DUE_TO_PBC_MASK << CR_CREDIT_RETURN_DUE_TO_PBC_SHIFT)

#define CR_CREDIT_RETURN_DUE_TO_THRESHOLD_SHIFT 13
#define CR_CREDIT_RETURN_DUE_TO_THRESHOLD_MASK 0x1ull
#define CR_CREDIT_RETURN_DUE_TO_THRESHOLD_SMASK \
	(CR_CREDIT_RETURN_DUE_TO_THRESHOLD_MASK \
	 << CR_CREDIT_RETURN_DUE_TO_THRESHOLD_SHIFT)

#define CR_CREDIT_RETURN_DUE_TO_ERR_SHIFT 14
#define CR_CREDIT_RETURN_DUE_TO_ERR_MASK 0x1ull
#define CR_CREDIT_RETURN_DUE_TO_ERR_SMASK \
	(CR_CREDIT_RETURN_DUE_TO_ERR_MASK << CR_CREDIT_RETURN_DUE_TO_ERR_SHIFT)

#define CR_CREDIT_RETURN_DUE_TO_FORCE_SHIFT 15
#define CR_CREDIT_RETURN_DUE_TO_FORCE_MASK 0x1ull
#define CR_CREDIT_RETURN_DUE_TO_FORCE_SMASK \
	(CR_CREDIT_RETURN_DUE_TO_FORCE_MASK \
	 << CR_CREDIT_RETURN_DUE_TO_FORCE_SHIFT)

/* Specific IRQ sources */
#define CCE_ERR_INT 0
#define RXE_ERR_INT 1
#define MISC_ERR_INT 2
#define PIO_ERR_INT 4
#define SDMA_ERR_INT 5
#define EGRESS_ERR_INT 6
#define TXE_ERR_INT 7
#define PBC_INT 240
#define GPIO_ASSERT_INT 241
#define QSFP1_INT 242
#define QSFP2_INT 243
#define TCRIT_INT 244

/* interrupt source ranges */
#define IS_GENERAL_ERR_START 0
#define IS_SDMAENG_ERR_START 16
#define IS_SENDCTXT_ERR_START 32
#define IS_SDMA_START 192
#define IS_SDMA_PROGRESS_START 208
#define IS_SDMA_IDLE_START 224
#define IS_VARIOUS_START 240
#define IS_DC_START 248
#define IS_RCVAVAIL_START 256
#define IS_RCVURGENT_START 416
#define IS_SENDCREDIT_START 576
#define IS_RESERVED_START 736
#define IS_LAST_SOURCE 767

/* derived interrupt source values */
#define IS_GENERAL_ERR_END 15
#define IS_SDMAENG_ERR_END 31
#define IS_SENDCTXT_ERR_END 191
#define IS_SDMA_END 207
#define IS_SDMA_PROGRESS_END 223
#define IS_SDMA_IDLE_END 239
#define IS_VARIOUS_END 247
#define IS_DC_END 255
#define IS_RCVAVAIL_END 415
#define IS_RCVURGENT_END 575
#define IS_SENDCREDIT_END 735
#define IS_RESERVED_END IS_LAST_SOURCE

/* DCC_CFG_PORT_CONFIG logical link states */
#define LSTATE_DOWN 0x1
#define LSTATE_INIT 0x2
#define LSTATE_ARMED 0x3
#define LSTATE_ACTIVE 0x4

/* DCC_CFG_RESET reset states */
#define LCB_RX_FPE_TX_FPE_INTO_RESET                            \
	(DCC_CFG_RESET_RESET_LCB | DCC_CFG_RESET_RESET_TX_FPE | \
	 DCC_CFG_RESET_RESET_RX_FPE | DCC_CFG_RESET_ENABLE_CCLK_BCC)
/* 0x17 */

#define LCB_RX_FPE_TX_FPE_OUT_OF_RESET DCC_CFG_RESET_ENABLE_CCLK_BCC /* 0x10 */

/* DC8051_STS_CUR_STATE port values (physical link states) */
#define PLS_DISABLED 0x30
#define PLS_OFFLINE 0x90
#define PLS_OFFLINE_QUIET 0x90
#define PLS_OFFLINE_PLANNED_DOWN_INFORM 0x91
#define PLS_OFFLINE_READY_TO_QUIET_LT 0x92
#define PLS_OFFLINE_REPORT_FAILURE 0x93
#define PLS_OFFLINE_READY_TO_QUIET_BCC 0x94
#define PLS_OFFLINE_QUIET_DURATION 0x95
#define PLS_POLLING 0x20
#define PLS_POLLING_QUIET 0x20
#define PLS_POLLING_ACTIVE 0x21
#define PLS_CONFIGPHY 0x40
#define PLS_CONFIGPHY_DEBOUCE 0x40
#define PLS_CONFIGPHY_ESTCOMM 0x41
#define PLS_CONFIGPHY_ESTCOMM_TXRX_HUNT 0x42
#define PLS_CONFIGPHY_ESTCOMM_LOCAL_COMPLETE 0x43
#define PLS_CONFIGPHY_OPTEQ 0x44
#define PLS_CONFIGPHY_OPTEQ_OPTIMIZING 0x44
#define PLS_CONFIGPHY_OPTEQ_LOCAL_COMPLETE 0x45
#define PLS_CONFIGPHY_VERIFYCAP 0x46
#define PLS_CONFIGPHY_VERIFYCAP_EXCHANGE 0x46
#define PLS_CONFIGPHY_VERIFYCAP_LOCAL_COMPLETE 0x47
#define PLS_CONFIGLT 0x48
#define PLS_CONFIGLT_CONFIGURE 0x48
#define PLS_CONFIGLT_LINK_TRANSFER_ACTIVE 0x49
#define PLS_LINKUP 0x50
#define PLS_PHYTEST 0xB0
#define PLS_INTERNAL_SERDES_LOOPBACK 0xe1
#define PLS_QUICK_LINKUP 0xe2

/* DC_DC8051_CFG_HOST_CMD_0.REQ_TYPE - 8051 host commands */
#define HCMD_LOAD_CONFIG_DATA 0x01
#define HCMD_READ_CONFIG_DATA 0x02
#define HCMD_CHANGE_PHY_STATE 0x03
#define HCMD_SEND_LCB_IDLE_MSG 0x04
#define HCMD_MISC 0x05
#define HCMD_READ_LCB_IDLE_MSG 0x06
#define HCMD_READ_LCB_CSR 0x07
#define HCMD_WRITE_LCB_CSR 0x08
#define HCMD_INTERFACE_TEST 0xff

/* DC_DC8051_CFG_HOST_CMD_1.RETURN_CODE - 8051 host command return */
#define HCMD_SUCCESS 2

/* DC_DC8051_DBG_ERR_INFO_SET_BY_8051.ERROR - error flags */
#define SPICO_ROM_FAILED BIT(0)
#define UNKNOWN_FRAME BIT(1)
#define TARGET_BER_NOT_MET BIT(2)
#define FAILED_SERDES_INTERNAL_LOOPBACK BIT(3)
#define FAILED_SERDES_INIT BIT(4)
#define FAILED_LNI_POLLING BIT(5)
#define FAILED_LNI_DEBOUNCE BIT(6)
#define FAILED_LNI_ESTBCOMM BIT(7)
#define FAILED_LNI_OPTEQ BIT(8)
#define FAILED_LNI_VERIFY_CAP1 BIT(9)
#define FAILED_LNI_VERIFY_CAP2 BIT(10)
#define FAILED_LNI_CONFIGLT BIT(11)
#define HOST_HANDSHAKE_TIMEOUT BIT(12)
#define EXTERNAL_DEVICE_REQ_TIMEOUT BIT(13)

#define FAILED_LNI                                                            \
	(FAILED_LNI_POLLING | FAILED_LNI_DEBOUNCE | FAILED_LNI_ESTBCOMM |     \
	 FAILED_LNI_OPTEQ | FAILED_LNI_VERIFY_CAP1 | FAILED_LNI_VERIFY_CAP2 | \
	 FAILED_LNI_CONFIGLT | HOST_HANDSHAKE_TIMEOUT |                       \
	 EXTERNAL_DEVICE_REQ_TIMEOUT)

/* DC_DC8051_DBG_ERR_INFO_SET_BY_8051.HOST_MSG - host message flags */
#define HOST_REQ_DONE BIT(0)
#define BC_PWR_MGM_MSG BIT(1)
#define BC_SMA_MSG BIT(2)
#define BC_BCC_UNKNOWN_MSG BIT(3)
#define BC_IDLE_UNKNOWN_MSG BIT(4)
#define EXT_DEVICE_CFG_REQ BIT(5)
#define VERIFY_CAP_FRAME BIT(6)
#define LINKUP_ACHIEVED BIT(7)
#define LINK_GOING_DOWN BIT(8)
#define LINK_WIDTH_DOWNGRADED BIT(9)

/* DC_DC8051_CFG_EXT_DEV_1.REQ_TYPE - 8051 host requests */
#define HREQ_LOAD_CONFIG 0x01
#define HREQ_SAVE_CONFIG 0x02
#define HREQ_READ_CONFIG 0x03
#define HREQ_SET_TX_EQ_ABS 0x04
#define HREQ_SET_TX_EQ_REL 0x05
#define HREQ_ENABLE 0x06
#define HREQ_LCB_RESET 0x07
#define HREQ_CONFIG_DONE 0xfe
#define HREQ_INTERFACE_TEST 0xff

/* DC_DC8051_CFG_EXT_DEV_0.RETURN_CODE - 8051 host request return codes */
#define HREQ_INVALID 0x01
#define HREQ_SUCCESS 0x02
#define HREQ_NOT_SUPPORTED 0x03
#define HREQ_FEATURE_NOT_SUPPORTED 0x04 /* request specific feature */
#define HREQ_REQUEST_REJECTED 0xfe
#define HREQ_EXECUTION_ONGOING 0xff

/* MISC host command functions */
#define HCMD_MISC_REQUEST_LCB_ACCESS 0x1
#define HCMD_MISC_GRANT_LCB_ACCESS 0x2

/* idle flit message types */
#define IDLE_PHYSICAL_LINK_MGMT 0x1
#define IDLE_CRU 0x2
#define IDLE_SMA 0x3
#define IDLE_POWER_MGMT 0x4

/* idle flit message send fields (both send and read) */
#define IDLE_PAYLOAD_MASK 0xffffffffffull /* 40 bits */
#define IDLE_PAYLOAD_SHIFT 8
#define IDLE_MSG_TYPE_MASK 0xf
#define IDLE_MSG_TYPE_SHIFT 0

/* idle flit message read fields */
#define READ_IDLE_MSG_TYPE_MASK 0xf
#define READ_IDLE_MSG_TYPE_SHIFT 0

/* SMA idle flit payload commands */
#define SMA_IDLE_ARM 1
#define SMA_IDLE_ACTIVE 2

/* DC_DC8051_CFG_MODE.GENERAL bits */
#define DISABLE_SELF_GUID_CHECK 0x2

/* Bad L2 frame error code */
#define BAD_L2_ERR 0x6

/*
 * Eager buffer minimum and maximum sizes supported by the hardware.
 * All power-of-two sizes in between are supported as well.
 * MAX_EAGER_BUFFER_TOTAL is the maximum size of memory
 * allocatable for Eager buffer to a single context. All others
 * are limits for the RcvArray entries.
 */
#define MIN_EAGER_BUFFER (4 * 1024)
#define MAX_EAGER_BUFFER (256 * 1024)
#define MAX_EAGER_BUFFER_TOTAL (64 * (1 << 20)) /* max per ctxt 64MB */
#define MAX_EXPECTED_BUFFER (2048 * 1024)
#define HFI2_MIN_HDRQ_EGRBUF_CNT 32
#define HFI2_MAX_HDRQ_EGRBUF_CNT 16352

/*
 * Receive expected base and count and eager base and count increment -
 * the CSR fields hold multiples of this value.
 */
#define RCV_SHIFT 3
#define RCV_INCREMENT BIT(RCV_SHIFT)

/*
 * Receive header queue entry increment - the CSR holds multiples of
 * this value.
 */
#define HDRQ_SIZE_SHIFT 5
#define HDRQ_INCREMENT BIT(HDRQ_SIZE_SHIFT)

/*
 * Freeze handling flags
 */
#define FREEZE_ABORT 0x01 /* do not do recovery */
#define FREEZE_SELF 0x02 /* initiate the freeze */
#define FREEZE_LINK_DOWN 0x04 /* link is down */

/*
 * Chip implementation codes.
 */
#define ICODE_RTL_SILICON 0x00
#define ICODE_RTL_VCS_SIMULATION 0x01
#define ICODE_FPGA_EMULATION 0x02
#define ICODE_FUNCTIONAL_SIMULATOR 0x03

/*
 * 8051 data memory size.
 */
#define DC8051_DATA_MEM_SIZE 0x1000

/*
 * 8051 firmware registers
 */
#define NUM_GENERAL_FIELDS 0x17
#define NUM_LANE_FIELDS 0x8

/* 8051 general register Field IDs */
#define LINK_OPTIMIZATION_SETTINGS 0x00
#define LINK_TUNING_PARAMETERS 0x02
#define DC_HOST_COMM_SETTINGS 0x03
#define TX_SETTINGS 0x06
#define VERIFY_CAP_LOCAL_PHY 0x07
#define VERIFY_CAP_LOCAL_FABRIC 0x08
#define VERIFY_CAP_LOCAL_LINK_MODE 0x09
#define LOCAL_DEVICE_ID 0x0a
#define RESERVED_REGISTERS 0x0b
#define LOCAL_LNI_INFO 0x0c
#define REMOTE_LNI_INFO 0x0d
#define MISC_STATUS 0x0e
#define VERIFY_CAP_REMOTE_PHY 0x0f
#define VERIFY_CAP_REMOTE_FABRIC 0x10
#define VERIFY_CAP_REMOTE_LINK_WIDTH 0x11
#define LAST_LOCAL_STATE_COMPLETE 0x12
#define LAST_REMOTE_STATE_COMPLETE 0x13
#define LINK_QUALITY_INFO 0x14
#define REMOTE_DEVICE_ID 0x15
#define LINK_DOWN_REASON 0x16 /* first byte of offset 0x16 */
#define VERSION_PATCH 0x16 /* last byte of offset 0x16 */

/* 8051 lane specific register field IDs */
#define TX_EQ_SETTINGS 0x00
#define CHANNEL_LOSS_SETTINGS 0x05

/* Lane ID for general configuration registers */
#define GENERAL_CONFIG 4

/* LINK_TUNING_PARAMETERS fields */
#define TUNING_METHOD_SHIFT 24

/* LINK_OPTIMIZATION_SETTINGS fields */
#define ENABLE_EXT_DEV_CONFIG_SHIFT 24

/* LOAD_DATA 8051 command shifts and fields */
#define LOAD_DATA_FIELD_ID_SHIFT 40
#define LOAD_DATA_FIELD_ID_MASK 0xfull
#define LOAD_DATA_LANE_ID_SHIFT 32
#define LOAD_DATA_LANE_ID_MASK 0xfull
#define LOAD_DATA_DATA_SHIFT 0x0
#define LOAD_DATA_DATA_MASK 0xffffffffull

/* READ_DATA 8051 command shifts and fields */
#define READ_DATA_FIELD_ID_SHIFT 40
#define READ_DATA_FIELD_ID_MASK 0xffull
#define READ_DATA_LANE_ID_SHIFT 32
#define READ_DATA_LANE_ID_MASK 0xffull
#define READ_DATA_DATA_SHIFT 0x0
#define READ_DATA_DATA_MASK 0xffffffffull

/* TX settings fields */
#define ENABLE_LANE_TX_SHIFT 0
#define ENABLE_LANE_TX_MASK 0xff
#define TX_POLARITY_INVERSION_SHIFT 8
#define TX_POLARITY_INVERSION_MASK 0xff
#define RX_POLARITY_INVERSION_SHIFT 16
#define RX_POLARITY_INVERSION_MASK 0xff
#define MAX_RATE_SHIFT 24
#define MAX_RATE_MASK 0xff

/* verify capability PHY fields */
#define CONTINIOUS_REMOTE_UPDATE_SUPPORT_SHIFT 0x4
#define CONTINIOUS_REMOTE_UPDATE_SUPPORT_MASK 0x1
#define POWER_MANAGEMENT_SHIFT 0x0
#define POWER_MANAGEMENT_MASK 0xf

/* 8051 lane register Field IDs */
#define SPICO_FW_VERSION 0x7 /* SPICO firmware version */

/* SPICO firmware version fields */
#define SPICO_ROM_VERSION_SHIFT 0
#define SPICO_ROM_VERSION_MASK 0xffff
#define SPICO_ROM_PROD_ID_SHIFT 16
#define SPICO_ROM_PROD_ID_MASK 0xffff

/* verify capability fabric fields */
#define VAU_SHIFT 0
#define VAU_MASK 0x0007
#define Z_SHIFT 3
#define Z_MASK 0x0001
#define VCU_SHIFT 4
#define VCU_MASK 0x0007
#define VL15BUF_SHIFT 8
#define VL15BUF_MASK 0x0fff
#define CRC_SIZES_SHIFT 20
#define CRC_SIZES_MASK 0x7

/* verify capability local link width fields */
#define LINK_WIDTH_SHIFT 0 /* also for remote link width */
#define LINK_WIDTH_MASK 0xffff /* also for remote link width */
#define LOCAL_FLAG_BITS_SHIFT 16
#define LOCAL_FLAG_BITS_MASK 0xff
#define MISC_CONFIG_BITS_SHIFT 24
#define MISC_CONFIG_BITS_MASK 0xff

/* verify capability remote link width fields */
#define REMOTE_TX_RATE_SHIFT 16
#define REMOTE_TX_RATE_MASK 0xff

/* LOCAL_DEVICE_ID fields */
#define LOCAL_DEVICE_REV_SHIFT 0
#define LOCAL_DEVICE_REV_MASK 0xff
#define LOCAL_DEVICE_ID_SHIFT 8
#define LOCAL_DEVICE_ID_MASK 0xffff

/* REMOTE_DEVICE_ID fields */
#define REMOTE_DEVICE_REV_SHIFT 0
#define REMOTE_DEVICE_REV_MASK 0xff
#define REMOTE_DEVICE_ID_SHIFT 8
#define REMOTE_DEVICE_ID_MASK 0xffff

/* local LNI link width fields */
#define ENABLE_LANE_RX_SHIFT 16
#define ENABLE_LANE_RX_MASK 0xff

/* mask, shift for reading 'mgmt_enabled' value from REMOTE_LNI_INFO field */
#define MGMT_ALLOWED_SHIFT 23
#define MGMT_ALLOWED_MASK 0x1

/* mask, shift for 'link_quality' within LINK_QUALITY_INFO field */
#define LINK_QUALITY_SHIFT 24
#define LINK_QUALITY_MASK 0x7

/*
 * mask, shift for reading 'planned_down_remote_reason_code'
 * from LINK_QUALITY_INFO field
 */
#define DOWN_REMOTE_REASON_SHIFT 16
#define DOWN_REMOTE_REASON_MASK 0xff

#define HOST_INTERFACE_VERSION 1
#define HOST_INTERFACE_VERSION_SHIFT 16
#define HOST_INTERFACE_VERSION_MASK 0xff

/* verify capability PHY power management bits */
#define PWRM_BER_CONTROL 0x1
#define PWRM_BANDWIDTH_CONTROL 0x2

/* 8051 link down reasons */
#define LDR_LINK_TRANSFER_ACTIVE_LOW 0xa
#define LDR_RECEIVED_LINKDOWN_IDLE_MSG 0xb
#define LDR_RECEIVED_HOST_OFFLINE_REQ 0xc

/* verify capability fabric CRC size bits */
enum {
	CAP_CRC_14B = (1 << 0), /* 14b CRC */
	CAP_CRC_48B = (1 << 1), /* 48b CRC */
	CAP_CRC_12B_16B_PER_LANE = (1 << 2) /* 12b-16b per lane CRC */
};

#define SUPPORTED_CRCS (CAP_CRC_14B | CAP_CRC_48B)

/* misc status version fields */
#define STS_FM_VERSION_MINOR_SHIFT 16
#define STS_FM_VERSION_MINOR_MASK 0xff
#define STS_FM_VERSION_MAJOR_SHIFT 24
#define STS_FM_VERSION_MAJOR_MASK 0xff
#define STS_FM_VERSION_PATCH_SHIFT 24
#define STS_FM_VERSION_PATCH_MASK 0xff

/* LCB_CFG_CRC_MODE TX_VAL and RX_VAL CRC mode values */
#define LCB_CRC_16B 0x0 /* 16b CRC */
#define LCB_CRC_14B 0x1 /* 14b CRC */
#define LCB_CRC_48B 0x2 /* 48b CRC */
#define LCB_CRC_12B_16B_PER_LANE 0x3 /* 12b-16b per lane CRC */

/*
 * the following enum is (almost) a copy/paste of the definition
 * in the OPA spec, section 20.2.2.6.8 (PortInfo)
 */
enum {
	PORT_LTP_CRC_MODE_NONE = 0,
	PORT_LTP_CRC_MODE_14 = 1, /* 14-bit LTP CRC mode (optional) */
	PORT_LTP_CRC_MODE_16 = 2, /* 16-bit LTP CRC mode */
	PORT_LTP_CRC_MODE_48 = 4,
	/* 48-bit overlapping LTP CRC mode (optional) */
	PORT_LTP_CRC_MODE_PER_LANE = 8
	/* 12 to 16 bit per lane LTP CRC mode (optional) */
};

/* timeouts */
#define LINK_RESTART_DELAY 1000 /* link restart delay, in ms */
#define TIMEOUT_8051_START 5000 /* 8051 start timeout, in ms */
#define DC8051_COMMAND_TIMEOUT 1000 /* DC8051 command timeout, in ms */
#define FREEZE_STATUS_TIMEOUT 20 /* wait for freeze indicators, in ms */
#define VL_STATUS_CLEAR_TIMEOUT 5000 /* per-VL status clear, in ms */
#define CCE_STATUS_TIMEOUT 10 /* time to clear CCE Status, in ms */

/* cclock tick time, in picoseconds per tick: 1/speed * 10^12  */
#define ASIC_CCLOCK_PS 1242 /* 805 MHz */

/*
 * Mask of enabled MISC errors.  Do not enable the two RSA engine errors -
 * see firmware.c:run_rsa() for details.
 */
#define DRIVER_MISC_MASK                                   \
	(~(MISC_ERR_STATUS_MISC_FW_AUTH_FAILED_ERR_SMASK | \
	   MISC_ERR_STATUS_MISC_KEY_MISMATCH_ERR_SMASK))

/* valid values for the hfi2_loopback module parameter */
#define LOOPBACK_NONE 0 /* no hfi2_loopback - default */
#define LOOPBACK_SERDES 1
#define LOOPBACK_LCB 2
#define LOOPBACK_CABLE 3 /* external cable */

/* set up bits in MISC_CONFIG_BITS */
#define LOOPBACK_SERDES_CONFIG_BIT_MASK_SHIFT 0
#define EXT_CFG_LCB_RESET_SUPPORTED_SHIFT 3

/* read and write hardware registers */
u64 hfi2_read_csr(const struct hfi2_devdata *dd, u32 offset);
void hfi2_write_csr(const struct hfi2_devdata *dd, u32 offset, u64 value);
u64 hfi2_read_ctxt_csr(const struct hfi2_devdata *dd, u32 offset, u32 ctxt,
		       u32 stride);
void hfi2_write_ctxt_csr(const struct hfi2_devdata *dd, u32 offset, u32 ctxt,
			 u32 stride, u64 value);

int hfi2_read_lcb_csr(struct hfi2_pportdata *ppd, u32 offset, u64 *data);
int hfi2_write_lcb_csr(struct hfi2_pportdata *ppd, u32 offset, u64 data);

void __iomem *hfi2_get_csr_addr(const struct hfi2_devdata *dd, u32 offset);

bool hfi2_wfr_check_synth_status(struct hfi2_devdata *dd);
void hfi2_wfr_update_synth_status(struct hfi2_devdata *dd);

u8 hfi2_encode_rcv_header_entry_size(u8 size);
int hfi2_validate_rcvhdrcnt(struct hfi2_devdata *dd, uint thecnt);
void hfi2_set_hdrq_regs(struct hfi2_pportdata *ppd, u16 ctxt, u8 entsize,
			u16 hdrcnt, u8 kdeth_rcv_hdr);
void hfi2_wfr_update_rcv_hdr_size(struct hfi2_pportdata *ppd, u16 ctxt,
				  u32 size);

u64 hfi2_wfr_create_pbc(struct hfi2_pportdata *ppd, bool hfi2_loopback,
			u64 flags, int srate_mbs, u32 vl, u32 dw_len, u32 l2,
			u32 dlid, u32 sctxt);

/* firmware.c */
#define SBUS_MASTER_BROADCAST 0xfd
#define NUM_PCIE_SERDES 16 /* number of PCIe serdes on the SBus */
extern const u8 hfi2_pcie_serdes_broadcast[];
extern const u8 hfi2_pcie_pcs_addrs[2][NUM_PCIE_SERDES];

/* SBus commands */
#define RESET_SBUS_RECEIVER 0x20
#define WRITE_SBUS_RECEIVER 0x21
#define READ_SBUS_RECEIVER 0x22
void hfi2_sbus_request(struct hfi2_devdata *dd, u8 receiver_addr, u8 data_addr,
		       u8 command, u32 data_in);
int hfi2_sbus_request_slow(struct hfi2_devdata *dd, u8 receiver_addr,
			   u8 data_addr, u8 command, u32 data_in);
void hfi2_set_sbus_fast_mode(struct hfi2_devdata *dd);
void hfi2_clear_sbus_fast_mode(struct hfi2_devdata *dd);
int hfi2_firmware_init(struct hfi2_devdata *dd);
int hfi2_load_pcie_firmware(struct hfi2_devdata *dd);
int hfi2_load_firmware(struct hfi2_devdata *dd);
void hfi2_dispose_firmware(void);
int hfi2_acquire_hw_mutex(struct hfi2_devdata *dd);
void hfi2_release_hw_mutex(struct hfi2_devdata *dd);

/*
 * Bitmask of dynamic access for ASIC block chip resources.  Each HFI has its
 * own range of bits for the resource so it can clear its own bits on
 * starting and exiting.  If either HFI has the resource bit set, the
 * resource is in use.  The separate bit ranges are:
 *	HFI0 bits  7:0
 *	HFI2 bits 15:8
 */
#define CR_SBUS 0x01 /* SBUS, THERM, and PCIE registers */
#define CR_EPROM 0x02 /* EEP, GPIO registers */
#define CR_I2C1 0x04 /* QSFP1_OE register */
#define CR_I2C2 0x08 /* QSFP2_OE register */
#define CR_DYN_SHIFT 8 /* dynamic flag shift */
#define CR_DYN_MASK ((1ull << CR_DYN_SHIFT) - 1)

/*
 * Bitmask of static ASIC states these are outside of the dynamic ASIC
 * block chip resources above.  These are to be set once and never cleared.
 * Must be holding the SBus dynamic flag when setting.
 */
#define CR_THERM_INIT 0x010000

int hfi2_acquire_chip_resource(struct hfi2_devdata *dd, u32 resource,
			       u32 mswait);
void hfi2_release_chip_resource(struct hfi2_devdata *dd, u32 resource);
bool hfi2_check_chip_resource(struct hfi2_devdata *dd, u32 resource,
			      const char *func);
void hfi2_init_chip_resources(struct hfi2_devdata *dd);
void hfi2_finish_chip_resources(struct hfi2_devdata *dd);

/* ms wait time for access to an SBus resoure */
#define SBUS_TIMEOUT 4000 /* long enough for a FW download and SBR */

/* ms wait time for a qsfp (i2c) chain to become available */
#define QSFP_WAIT 20000 /* long enough for FW update to the F4 uc */

void hfi2_fabric_serdes_reset(struct hfi2_devdata *dd);
int hfi2_read_8051_data(struct hfi2_devdata *dd, u32 addr, u32 len,
			u64 *result);

/* wfr specific */
int hfi2_wfr_find_used_resources(struct hfi2_devdata *dd);
int hfi2_wfr_early_per_chip_init(struct hfi2_devdata *dd);
int hfi2_wfr_mid_per_chip_init(struct hfi2_devdata *dd);
int hfi2_wfr_late_per_chip_init(struct hfi2_devdata *dd);
void hfi2_wfr_enable_rcv_context(struct hfi2_pportdata *ppd, u16 ctxt,
				 u64 *kctxt_ctrl, bool enable);

/* chip.c */
void hfi2_read_misc_status(struct hfi2_devdata *dd, u8 *ver_major,
			   u8 *ver_minor, u8 *ver_patch);
int hfi2_write_host_interface_version(struct hfi2_devdata *dd, u8 version);
void hfi2_read_guid(struct hfi2_devdata *dd);
int hfi2_wait_fm_ready(struct hfi2_devdata *dd, u32 mstimeout);
void hfi2_set_link_down_reason(struct hfi2_pportdata *ppd, u8 lcl_reason,
			       u8 neigh_reason, u8 rem_reason);
int hfi2_set_link_state(struct hfi2_pportdata *ppd, u32 state);
void hfi2_init_kdeth_qp(struct hfi2_devdata *dd);
int hfi2_port_ltp_to_cap(int port_ltp);
void hfi2_handle_verify_cap(struct work_struct *work);
void hfi2_handle_freeze(struct work_struct *work);
void hfi2_handle_link_up(struct work_struct *work);
void hfi2_handle_link_down(struct work_struct *work);
void hfi2_handle_link_downgrade(struct work_struct *work);
void hfi2_wfr_handle_link_bounce(struct work_struct *work);
void hfi2_handle_start_link(struct work_struct *work);
void hfi2_handle_sma_message(struct work_struct *work);
int hfi2_reset_qsfp(struct hfi2_pportdata *ppd);
void hfi2_qsfp_event(struct work_struct *work);
void hfi2_start_freeze_handling(struct hfi2_devdata *dd, int flags);
void hfi2_start_linkdown_handling(struct hfi2_pportdata *ppd);
int hfi2_send_idle_sma(struct hfi2_devdata *dd, u64 message);
int hfi2_load_8051_config(struct hfi2_devdata *dd, u8 target, u8 addr,
			  u32 data);
int hfi2_read_8051_config(struct hfi2_devdata *dd, u8 target, u8 addr,
			  u32 *data);
int hfi2_start_link(struct hfi2_pportdata *ppd);
int hfi2_bringup_serdes(struct hfi2_pportdata *ppd);
bool hfi2_apply_link_downgrade_policy(struct hfi2_pportdata *ppd,
				      bool refresh_widths);
void hfi2_update_usrhead(struct hfi2_ctxtdata *rcd, u32 hd, u32 updegr,
			 u32 egrhd, u32 intr_adjust, u32 npkts);
void hfi2_update_usrhead_ctxt(struct hfi2_devdata *dd, u16 ctxt, u32 hd,
			      u32 intr_cnt, u32 updegr, u32 egrhd);
int hfi2_stop_drain_data_vls(struct hfi2_pportdata *ppd);
int hfi2_open_fill_data_vls(struct hfi2_pportdata *ppd);
u32 hfi2_ns_to_cclock(struct hfi2_devdata *dd, u32 ns);
u32 hfi2_cclock_to_ns(struct hfi2_devdata *dd, u32 cclock);
void hfi2_get_linkup_link_widths(struct hfi2_pportdata *ppd);
void hfi2_clear_linkup_counters(struct hfi2_pportdata *ppd);
u32 hfi2_hdrqempty(struct hfi2_ctxtdata *rcd);
int hfi2_is_ax(struct hfi2_devdata *dd);
int hfi2_is_bx(struct hfi2_devdata *dd);
bool hfi2_is_urg_masked(struct hfi2_ctxtdata *rcd);
u32 hfi2_read_physical_state(struct hfi2_devdata *dd);
u32 hfi2_chip_to_opa_pstate(struct hfi2_devdata *dd, u32 chip_pstate);
const char *hfi2_opa_pstate_name(u32 pstate);
u32 hfi2_driver_pstate(struct hfi2_pportdata *ppd);
u32 hfi2_driver_lstate(struct hfi2_pportdata *ppd);

int hfi2_acquire_lcb_access(struct hfi2_devdata *dd, int sleep_ok);
int hfi2_release_lcb_access(struct hfi2_devdata *dd, int sleep_ok);
#define LCB_START DC_LCB_CSRS
#define LCB_END DC_8051_CSRS /* next block is 8051 */
extern uint hfi2_num_vls;

extern uint disable_integrity;
u64 hfi2_read_dev_cntr(struct hfi2_devdata *dd, int index, int vl);
u64 hfi2_write_dev_cntr(struct hfi2_devdata *dd, int index, int vl, u64 data);
u64 hfi2_read_port_cntr(struct hfi2_pportdata *ppd, int index, int vl);
u64 hfi2_write_port_cntr(struct hfi2_pportdata *ppd, int index, int vl,
			 u64 data);
u32 hfi2_read_logical_state(struct hfi2_devdata *dd);
void hfi2_force_recv_intr(struct hfi2_ctxtdata *rcd);
void hfi2_force_intr(struct hfi2_devdata *dd, u16 nr);

/* Per VL indexes */
enum {
	C_VL_0 = 0,
	C_VL_1,
	C_VL_2,
	C_VL_3,
	C_VL_4,
	C_VL_5,
	C_VL_6,
	C_VL_7,
	C_VL_15,
	C_VL_COUNT
};

static inline int vl_from_idx(int idx)
{
	return (idx == C_VL_15 ? 15 : idx);
}

static inline int idx_from_vl(int vl)
{
	return (vl == 15 ? C_VL_15 : vl);
}

/* shared device counter indexes */
enum {
	C_CCE_PCI_CR_ST,
	C_CCE_SDMA_INT,
	C_CCE_MISC_INT,
	C_CCE_RCV_AV_INT,
	C_CCE_RCV_URG_INT,
	C_CCE_SEND_CR_INT,
	C_SW_CPU_INTR,
	C_SW_CPU_RCV_LIM,
	C_SW_CTX0_SEQ_DROP,
	C_SW_VTX_WAIT,
	C_SW_PIO_WAIT,
	C_SW_PIO_DRAIN,
	C_SW_KMEM_WAIT,
	C_SW_TID_WAIT,
	C_SW_SEND_SCHED,
	C_SDMA_DESC_FETCHED_CNT,
	C_SDMA_INT_CNT,
	C_SDMA_ERR_CNT,
	C_SDMA_IDLE_INT_CNT,
	C_SDMA_PROGRESS_INT_CNT,
	/* MISC_ERR_STATUS */
	C_MISC_PLL_LOCK_FAIL_ERR,
	C_MISC_MBIST_FAIL_ERR,
	C_MISC_INVALID_EEP_CMD_ERR,
	C_MISC_EFUSE_DONE_PARITY_ERR,
	C_MISC_EFUSE_WRITE_ERR,
	C_MISC_EFUSE_READ_BAD_ADDR_ERR,
	C_MISC_EFUSE_CSR_PARITY_ERR,
	C_MISC_FW_AUTH_FAILED_ERR,
	C_MISC_KEY_MISMATCH_ERR,
	C_MISC_SBUS_WRITE_FAILED_ERR,
	C_MISC_CSR_WRITE_BAD_ADDR_ERR,
	C_MISC_CSR_READ_BAD_ADDR_ERR,
	C_MISC_CSR_PARITY_ERR,
	/* CceErrStatus */
	/*
	 * A special counter that is the aggregate count
	 * of all the cce_err_status errors.  The remainder
	 * are actual bits in the CceErrStatus register.
	 */
	C_CCE_ERR_STATUS_AGGREGATED_CNT,
	C_CCE_MSIX_CSR_PARITY_ERR,
	C_CCE_INT_MAP_UNC_ERR,
	C_CCE_INT_MAP_COR_ERR,
	C_CCE_MSIX_TABLE_UNC_ERR,
	C_CCE_MSIX_TABLE_COR_ERR,
	C_CCE_RXDMA_CONV_FIFO_PARITY_ERR,
	C_CCE_RCPL_ASYNC_FIFO_PARITY_ERR,
	C_CCE_SEG_WRITE_BAD_ADDR_ERR,
	C_CCE_SEG_READ_BAD_ADDR_ERR,
	C_LA_TRIGGERED,
	C_CCE_TRGT_CPL_TIMEOUT_ERR,
	C_PCIC_RECEIVE_PARITY_ERR,
	C_PCIC_TRANSMIT_BACK_PARITY_ERR,
	C_PCIC_TRANSMIT_FRONT_PARITY_ERR,
	C_PCIC_CPL_DAT_Q_UNC_ERR,
	C_PCIC_CPL_HD_Q_UNC_ERR,
	C_PCIC_POST_DAT_Q_UNC_ERR,
	C_PCIC_POST_HD_Q_UNC_ERR,
	C_PCIC_RETRY_SOT_MEM_UNC_ERR,
	C_PCIC_RETRY_MEM_UNC_ERR,
	C_PCIC_N_POST_DAT_Q_PARITY_ERR,
	C_PCIC_N_POST_H_Q_PARITY_ERR,
	C_PCIC_CPL_DAT_Q_COR_ERR,
	C_PCIC_CPL_HD_Q_COR_ERR,
	C_PCIC_POST_DAT_Q_COR_ERR,
	C_PCIC_POST_HD_Q_COR_ERR,
	C_PCIC_RETRY_SOT_MEM_COR_ERR,
	C_PCIC_RETRY_MEM_COR_ERR,
	C_CCE_CLI1_ASYNC_FIFO_DBG_PARITY_ERR,
	C_CCE_CLI1_ASYNC_FIFO_RXDMA_PARITY_ERR,
	C_CCE_CLI1_ASYNC_FIFO_SDMA_HD_PARITY_ERR,
	C_CCE_CLI1_ASYNC_FIFO_PIO_CRDT_PARITY_ERR,
	C_CCE_CLI2_ASYNC_FIFO_PARITY_ERR,
	C_CCE_CSR_CFG_BUS_PARITY_ERR,
	C_CCE_CLI0_ASYNC_FIFO_PARTIY_ERR,
	C_CCE_RSPD_DATA_PARITY_ERR,
	C_CCE_TRGT_ACCESS_ERR,
	C_CCE_TRGT_ASYNC_FIFO_PARITY_ERR,
	C_CCE_CSR_WRITE_BAD_ADDR_ERR,
	C_CCE_CSR_READ_BAD_ADDR_ERR,
	C_CCE_CSR_PARITY_ERR,
	/* RcvErrStatus */
	C_RX_CSR_PARITY_ERR,
	C_RX_CSR_WRITE_BAD_ADDR_ERR,
	C_RX_CSR_READ_BAD_ADDR_ERR,
	C_RX_DMA_CSR_UNC_ERR,
	C_RX_DMA_DQ_FSM_ENCODING_ERR,
	C_RX_DMA_EQ_FSM_ENCODING_ERR,
	C_RX_DMA_CSR_PARITY_ERR,
	C_RX_RBUF_DATA_COR_ERR,
	C_RX_RBUF_DATA_UNC_ERR,
	C_RX_DMA_DATA_FIFO_RD_COR_ERR,
	C_RX_DMA_DATA_FIFO_RD_UNC_ERR,
	C_RX_DMA_HDR_FIFO_RD_COR_ERR,
	C_RX_DMA_HDR_FIFO_RD_UNC_ERR,
	C_RX_RBUF_DESC_PART2_COR_ERR,
	C_RX_RBUF_DESC_PART2_UNC_ERR,
	C_RX_RBUF_DESC_PART1_COR_ERR,
	C_RX_RBUF_DESC_PART1_UNC_ERR,
	C_RX_HQ_INTR_FSM_ERR,
	C_RX_HQ_INTR_CSR_PARITY_ERR,
	C_RX_LOOKUP_CSR_PARITY_ERR,
	C_RX_LOOKUP_RCV_ARRAY_COR_ERR,
	C_RX_LOOKUP_RCV_ARRAY_UNC_ERR,
	C_RX_LOOKUP_DES_PART2_PARITY_ERR,
	C_RX_LOOKUP_DES_PART1_UNC_COR_ERR,
	C_RX_LOOKUP_DES_PART1_UNC_ERR,
	C_RX_RBUF_NEXT_FREE_BUF_COR_ERR,
	C_RX_RBUF_NEXT_FREE_BUF_UNC_ERR,
	C_RX_RBUF_FL_INIT_WR_ADDR_PARITY_ERR,
	C_RX_RBUF_FL_INITDONE_PARITY_ERR,
	C_RX_RBUF_FL_WRITE_ADDR_PARITY_ERR,
	C_RX_RBUF_FL_RD_ADDR_PARITY_ERR,
	C_RX_RBUF_EMPTY_ERR,
	C_RX_RBUF_FULL_ERR,
	C_RX_RBUF_BAD_LOOKUP_ERR,
	C_RX_RBUF_CTX_ID_PARITY_ERR,
	C_RX_RBUF_CSR_QEOPDW_PARITY_ERR,
	C_RX_RBUF_CSR_Q_NUM_OF_PKT_PARITY_ERR,
	C_RX_RBUF_CSR_Q_T1_PTR_PARITY_ERR,
	C_RX_RBUF_CSR_Q_HD_PTR_PARITY_ERR,
	C_RX_RBUF_CSR_Q_VLD_BIT_PARITY_ERR,
	C_RX_RBUF_CSR_Q_NEXT_BUF_PARITY_ERR,
	C_RX_RBUF_CSR_Q_ENT_CNT_PARITY_ERR,
	C_RX_RBUF_CSR_Q_HEAD_BUF_NUM_PARITY_ERR,
	C_RX_RBUF_BLOCK_LIST_READ_COR_ERR,
	C_RX_RBUF_BLOCK_LIST_READ_UNC_ERR,
	C_RX_RBUF_LOOKUP_DES_COR_ERR,
	C_RX_RBUF_LOOKUP_DES_UNC_ERR,
	C_RX_RBUF_LOOKUP_DES_REG_UNC_COR_ERR,
	C_RX_RBUF_LOOKUP_DES_REG_UNC_ERR,
	C_RX_RBUF_FREE_LIST_COR_ERR,
	C_RX_RBUF_FREE_LIST_UNC_ERR,
	C_RX_RCV_FSM_ENCODING_ERR,
	C_RX_DMA_FLAG_COR_ERR,
	C_RX_DMA_FLAG_UNC_ERR,
	C_RX_DC_SOP_EOP_PARITY_ERR,
	C_RX_RCV_CSR_PARITY_ERR,
	C_RX_RCV_QP_MAP_TABLE_COR_ERR,
	C_RX_RCV_QP_MAP_TABLE_UNC_ERR,
	C_RX_RCV_DATA_COR_ERR,
	C_RX_RCV_DATA_UNC_ERR,
	C_RX_RCV_HDR_COR_ERR,
	C_RX_RCV_HDR_UNC_ERR,
	C_RX_DC_INTF_PARITY_ERR,
	C_RX_DMA_CSR_COR_ERR,
	/* SendPioErrStatus */
	C_PIO_PEC_SOP_HEAD_PARITY_ERR,
	C_PIO_PCC_SOP_HEAD_PARITY_ERR,
	C_PIO_LAST_RETURNED_CNT_PARITY_ERR,
	C_PIO_CURRENT_FREE_CNT_PARITY_ERR,
	C_PIO_RSVD_31_ERR,
	C_PIO_RSVD_30_ERR,
	C_PIO_PPMC_SOP_LEN_ERR,
	C_PIO_PPMC_BQC_MEM_PARITY_ERR,
	C_PIO_VL_FIFO_PARITY_ERR,
	C_PIO_VLF_SOP_PARITY_ERR,
	C_PIO_VLF_V1_LEN_PARITY_ERR,
	C_PIO_BLOCK_QW_COUNT_PARITY_ERR,
	C_PIO_WRITE_QW_VALID_PARITY_ERR,
	C_PIO_STATE_MACHINE_ERR,
	C_PIO_WRITE_DATA_PARITY_ERR,
	C_PIO_HOST_ADDR_MEM_COR_ERR,
	C_PIO_HOST_ADDR_MEM_UNC_ERR,
	C_PIO_PKT_EVICT_SM_OR_ARM_SM_ERR,
	C_PIO_INIT_SM_IN_ERR,
	C_PIO_PPMC_PBL_FIFO_ERR,
	C_PIO_CREDIT_RET_FIFO_PARITY_ERR,
	C_PIO_V1_LEN_MEM_BANK1_COR_ERR,
	C_PIO_V1_LEN_MEM_BANK0_COR_ERR,
	C_PIO_V1_LEN_MEM_BANK1_UNC_ERR,
	C_PIO_V1_LEN_MEM_BANK0_UNC_ERR,
	C_PIO_SM_PKT_RESET_PARITY_ERR,
	C_PIO_PKT_EVICT_FIFO_PARITY_ERR,
	C_PIO_SBRDCTRL_CRREL_FIFO_PARITY_ERR,
	C_PIO_SBRDCTL_CRREL_PARITY_ERR,
	C_PIO_PEC_FIFO_PARITY_ERR,
	C_PIO_PCC_FIFO_PARITY_ERR,
	C_PIO_SB_MEM_FIFO1_ERR,
	C_PIO_SB_MEM_FIFO0_ERR,
	C_PIO_CSR_PARITY_ERR,
	C_PIO_WRITE_ADDR_PARITY_ERR,
	C_PIO_WRITE_BAD_CTXT_ERR,
	/* SendDmaErrStatus */
	C_SDMA_PCIE_REQ_TRACKING_COR_ERR,
	C_SDMA_PCIE_REQ_TRACKING_UNC_ERR,
	C_SDMA_CSR_PARITY_ERR,
	C_SDMA_RPY_TAG_ERR,
	/* SendEgressErrStatus */
	C_TX_READ_PIO_MEMORY_CSR_UNC_ERR,
	C_TX_READ_SDMA_MEMORY_CSR_UNC_ERR,
	C_TX_EGRESS_FIFO_COR_ERR,
	C_TX_READ_PIO_MEMORY_COR_ERR,
	C_TX_READ_SDMA_MEMORY_COR_ERR,
	C_TX_SB_HDR_COR_ERR,
	C_TX_CREDIT_OVERRUN_ERR,
	C_TX_LAUNCH_FIFO8_COR_ERR,
	C_TX_LAUNCH_FIFO7_COR_ERR,
	C_TX_LAUNCH_FIFO6_COR_ERR,
	C_TX_LAUNCH_FIFO5_COR_ERR,
	C_TX_LAUNCH_FIFO4_COR_ERR,
	C_TX_LAUNCH_FIFO3_COR_ERR,
	C_TX_LAUNCH_FIFO2_COR_ERR,
	C_TX_LAUNCH_FIFO1_COR_ERR,
	C_TX_LAUNCH_FIFO0_COR_ERR,
	C_TX_CREDIT_RETURN_VL_ERR,
	C_TX_HCRC_INSERTION_ERR,
	C_TX_EGRESS_FIFI_UNC_ERR,
	C_TX_READ_PIO_MEMORY_UNC_ERR,
	C_TX_READ_SDMA_MEMORY_UNC_ERR,
	C_TX_SB_HDR_UNC_ERR,
	C_TX_CREDIT_RETURN_PARITY_ERR,
	C_TX_LAUNCH_FIFO8_UNC_OR_PARITY_ERR,
	C_TX_LAUNCH_FIFO7_UNC_OR_PARITY_ERR,
	C_TX_LAUNCH_FIFO6_UNC_OR_PARITY_ERR,
	C_TX_LAUNCH_FIFO5_UNC_OR_PARITY_ERR,
	C_TX_LAUNCH_FIFO4_UNC_OR_PARITY_ERR,
	C_TX_LAUNCH_FIFO3_UNC_OR_PARITY_ERR,
	C_TX_LAUNCH_FIFO2_UNC_OR_PARITY_ERR,
	C_TX_LAUNCH_FIFO1_UNC_OR_PARITY_ERR,
	C_TX_LAUNCH_FIFO0_UNC_OR_PARITY_ERR,
	C_TX_SDMA15_DISALLOWED_PACKET_ERR,
	C_TX_SDMA14_DISALLOWED_PACKET_ERR,
	C_TX_SDMA13_DISALLOWED_PACKET_ERR,
	C_TX_SDMA12_DISALLOWED_PACKET_ERR,
	C_TX_SDMA11_DISALLOWED_PACKET_ERR,
	C_TX_SDMA10_DISALLOWED_PACKET_ERR,
	C_TX_SDMA9_DISALLOWED_PACKET_ERR,
	C_TX_SDMA8_DISALLOWED_PACKET_ERR,
	C_TX_SDMA7_DISALLOWED_PACKET_ERR,
	C_TX_SDMA6_DISALLOWED_PACKET_ERR,
	C_TX_SDMA5_DISALLOWED_PACKET_ERR,
	C_TX_SDMA4_DISALLOWED_PACKET_ERR,
	C_TX_SDMA3_DISALLOWED_PACKET_ERR,
	C_TX_SDMA2_DISALLOWED_PACKET_ERR,
	C_TX_SDMA1_DISALLOWED_PACKET_ERR,
	C_TX_SDMA0_DISALLOWED_PACKET_ERR,
	C_TX_CONFIG_PARITY_ERR,
	C_TX_SBRD_CTL_CSR_PARITY_ERR,
	C_TX_LAUNCH_CSR_PARITY_ERR,
	C_TX_ILLEGAL_CL_ERR,
	C_TX_SBRD_CTL_STATE_MACHINE_PARITY_ERR,
	C_TX_RESERVED_10,
	C_TX_RESERVED_9,
	C_TX_SDMA_LAUNCH_INTF_PARITY_ERR,
	C_TX_PIO_LAUNCH_INTF_PARITY_ERR,
	C_TX_RESERVED_6,
	C_TX_INCORRECT_LINK_STATE_ERR,
	C_TX_LINK_DOWN_ERR,
	C_TX_EGRESS_FIFO_UNDERRUN_OR_PARITY_ERR,
	C_TX_RESERVED_2,
	C_TX_PKT_INTEGRITY_MEM_UNC_ERR,
	C_TX_PKT_INTEGRITY_MEM_COR_ERR,
	/* SendErrStatus */
	C_SEND_CSR_WRITE_BAD_ADDR_ERR,
	C_SEND_CSR_READ_BAD_ADD_ERR,
	C_SEND_CSR_PARITY_ERR,
	/* SendCtxtErrStatus */
	C_PIO_WRITE_OUT_OF_BOUNDS_ERR,
	C_PIO_WRITE_OVERFLOW_ERR,
	C_PIO_WRITE_CROSSES_BOUNDARY_ERR,
	C_PIO_DISALLOWED_PACKET_ERR,
	C_PIO_INCONSISTENT_SOP_ERR,
	/*SendDmaEngErrStatus */
	C_SDMA_HEADER_REQUEST_FIFO_COR_ERR,
	C_SDMA_HEADER_STORAGE_COR_ERR,
	C_SDMA_PACKET_TRACKING_COR_ERR,
	C_SDMA_ASSEMBLY_COR_ERR,
	C_SDMA_DESC_TABLE_COR_ERR,
	C_SDMA_HEADER_REQUEST_FIFO_UNC_ERR,
	C_SDMA_HEADER_STORAGE_UNC_ERR,
	C_SDMA_PACKET_TRACKING_UNC_ERR,
	C_SDMA_ASSEMBLY_UNC_ERR,
	C_SDMA_DESC_TABLE_UNC_ERR,
	C_SDMA_TIMEOUT_ERR,
	C_SDMA_HEADER_LENGTH_ERR,
	C_SDMA_HEADER_ADDRESS_ERR,
	C_SDMA_HEADER_SELECT_ERR,
	C_SMDA_RESERVED_9,
	C_SDMA_PACKET_DESC_OVERFLOW_ERR,
	C_SDMA_LENGTH_MISMATCH_ERR,
	C_SDMA_HALT_ERR,
	C_SDMA_MEM_READ_ERR,
	C_SDMA_FIRST_DESC_ERR,
	C_SDMA_TAIL_OUT_OF_BOUNDS_ERR,
	C_SDMA_TOO_LONG_ERR,
	C_SDMA_GEN_MISMATCH_ERR,
	C_SDMA_WRONG_DW_ERR,
	SHARED_DEV_CNTR_LAST /* keep last */
};

/* chip specific counter start points - keep a separate range per chip */
#define WFR_DEV_CNTR_FIRST 0x200
#define JKR_DEV_CNTR_FIRST 0x400
#define WFR_PORT_CNTR_FIRST 0x200
#define JKR_PORT_CNTR_FIRST 0x400

/* WFR device counter indexes */
enum {
	C_DC_UNC_ERR = WFR_DEV_CNTR_FIRST,
	C_DC_RCV_ERR,
	C_DC_FM_CFG_ERR,
	C_DC_RMT_PHY_ERR,
	C_DC_DROPPED_PKT,
	C_DC_MC_XMIT_PKTS,
	C_DC_MC_RCV_PKTS,
	C_DC_XMIT_CERR,
	C_DC_RCV_CERR,
	C_DC_RCV_FCC,
	C_DC_XMIT_FCC,
	C_DC_XMIT_FLITS,
	C_DC_RCV_FLITS,
	C_DC_XMIT_PKTS,
	C_DC_RCV_PKTS,
	C_DC_RX_FLIT_VL,
	C_DC_RX_PKT_VL,
	C_DC_RCV_FCN,
	C_DC_RCV_FCN_VL,
	C_DC_RCV_BCN,
	C_DC_RCV_BCN_VL,
	C_DC_RCV_BBL,
	C_DC_RCV_BBL_VL,
	C_DC_MARK_FECN,
	C_DC_MARK_FECN_VL,
	C_DC_TOTAL_CRC,
	C_DC_CRC_LN0,
	C_DC_CRC_LN1,
	C_DC_CRC_LN2,
	C_DC_CRC_LN3,
	C_DC_CRC_MULT_LN,
	C_DC_TX_REPLAY,
	C_DC_RX_REPLAY,
	C_DC_SEQ_CRC_CNT,
	C_DC_ESC0_ONLY_CNT,
	C_DC_ESC0_PLUS1_CNT,
	C_DC_ESC0_PLUS2_CNT,
	C_DC_REINIT_FROM_PEER_CNT,
	C_DC_SBE_CNT,
	C_DC_MISC_FLG_CNT,
	C_DC_PRF_GOOD_LTP_CNT,
	C_DC_PRF_ACCEPTED_LTP_CNT,
	C_DC_PRF_RX_FLIT_CNT,
	C_DC_PRF_TX_FLIT_CNT,
	C_DC_PRF_CLK_CNTR,
	C_DC_PG_DBG_FLIT_CRDTS_CNT,
	C_DC_PG_STS_PAUSE_COMPLETE_CNT,
	C_DC_PG_STS_TX_SBE_CNT,
	C_DC_PG_STS_TX_MBE_CNT,
	C_CCE_PCI_TR_ST,
	C_CCE_PIO_WR_ST,
	C_CCE_ERR_INT,
	WFR_DEV_CNTR_LAST /* keep last */
};

#define WFR_NUM_DEV_CNTRS (WFR_DEV_CNTR_LAST - WFR_DEV_CNTR_FIRST)

/* JKR device counter indexes */
enum {
	C_CCE_RW_ST_BY_R = JKR_DEV_CNTR_FIRST,
	C_CCE_OTHER_INT,
	C_CCE_PBC_INT,
	C_CCE_PIO_ERR_INT,
	C_CCE_SDMA_ERR_INT,
	C_CCE_CSR_ERR_INT,
	JKR_DEV_CNTR_LAST /* keep last */
};

#define JKR_NUM_DEV_CNTRS (JKR_DEV_CNTR_LAST - JKR_DEV_CNTR_FIRST)

/* Per port counter indexes */
enum {
	C_TX_UNSUP_VL = 0,
	C_TX_INVAL_LEN,
	C_TX_MM_LEN_ERR,
	C_TX_UNDERRUN,
	C_TX_FLOW_STALL,
	C_TX_DROPPED,
	C_TX_HDR_ERR,
	C_TX_PKT,
	C_TX_WORDS,
	C_TX_WAIT,
	C_TX_FLIT_VL,
	C_TX_PKT_VL,
	C_TX_WAIT_VL,
	C_RCV_OVF,
	C_RX_LEN_ERR,
	C_RX_SHORT_ERR,
	C_RX_ICRC_ERR,
	C_RX_EBP,
	C_RX_PKEY_MISMATCH,
	C_RX_PKT,
	C_RX_WORDS,
	C_SW_LINK_DOWN,
	C_SW_LINK_UP,
	C_SW_UNKNOWN_FRAME,
	C_SW_XMIT_DSCD,
	C_SW_XMIT_DSCD_VL,
	C_SW_XMIT_CSTR_ERR,
	C_SW_RCV_CSTR_ERR,
	C_SW_IBP_LOOP_PKTS,
	C_SW_IBP_RC_RESENDS,
	C_SW_IBP_RNR_NAKS,
	C_SW_IBP_OTHER_NAKS,
	C_SW_IBP_RC_TIMEOUTS,
	C_SW_IBP_PKT_DROPS,
	C_SW_IBP_DMA_WAIT,
	C_SW_IBP_RC_SEQNAK,
	C_SW_IBP_RC_DUPREQ,
	C_SW_IBP_RDMA_SEQ,
	C_SW_IBP_UNALIGNED,
	C_SW_IBP_SEQ_NAK,
	C_SW_IBP_RC_CRWAITS,
	C_SW_CPU_RC_ACKS,
	C_SW_CPU_RC_QACKS,
	C_SW_CPU_RC_DELAYED_COMP,
	C_RCV_HDR_OVF,
	SHARED_PORT_CNTR_LAST /* Must be kept last */
};

/* WFR port counter indexes */
enum {
	C_WFR_RX_DROPPED_PKT = WFR_PORT_CNTR_FIRST,
	C_WFR_RX_DROPPED_BYPASS_PKT,
	C_WFR_RX_TID_FULL,
	C_WFR_RX_TID_INVALID,
	C_WFR_RX_TID_FLGMS,
	C_WFR_RX_CTX_EGRS,
	C_WFR_RCV_TID_FLSMS,
	WFR_PORT_CNTR_LAST, /* keep last */
};

#define WFR_NUM_PORT_CNTRS (WFR_PORT_CNTR_LAST - WFR_PORT_CNTR_FIRST)

/* JKR port counter indexes */
enum {
	C_JKR_RX_L2_TYPE_DISABLED = JKR_PORT_CNTR_FIRST,
	C_JKR_RX_DROPPED_PKT_16B,
	C_JKR_RX_DROPPED_PKT_9B,
	C_JKR_RX_TID_FULL,
	C_JKR_RX_TID_INVALID,
	C_JKR_RX_TID_FLGMS,
	C_JKR_RX_CTX_EGRS,
	C_JKR_RCV_TID_FLSMS,
	JKR_PORT_CNTR_LAST, /* keep last */
};

#define JKR_NUM_PORT_CNTRS (JKR_PORT_CNTR_LAST - JKR_PORT_CNTR_FIRST)

/* SendEgressErrInfo bits that correspond to a PortXmitDiscard counter */
#define WFR_PORT_DISCARD_EGRESS_ERRS                         \
	(SEND_EGRESS_ERR_INFO_TOO_LONG_IB_PACKET_ERR_SMASK | \
	 SEND_EGRESS_ERR_INFO_VL_MAPPING_ERR_SMASK |         \
	 SEND_EGRESS_ERR_INFO_VL_ERR_SMASK)

#define RT_ADDR_SHIFT 12 /* 4KB kernel address boundary */

/* PIO Send Memory Address details */
#define PIO_ADDR_CONTEXT_MASK 0xfful
#define PIO_ADDR_CONTEXT_SHIFT 16
#define SOP_DISTANCE (TXE_PIO_SIZE / 2) /* distance btw non-SOP and SOP space */
#define PIO_BLOCK_MASK (PIO_BLOCK_SIZE - 1)
#define PIO_BLOCK_QWS (PIO_BLOCK_SIZE / sizeof(u64)) /* num QWs in a block */

u64 hfi2_get_all_cpu_total(u64 __percpu *cntr);
void hfi2_start_cleanup(struct hfi2_devdata *dd);
void hfi2_clear_tids(struct hfi2_ctxtdata *rcd);
void hfi2_init_ctxt(struct send_context *sc);
void hfi2_wfr_put_tid(struct hfi2_ctxtdata *rcd, u32 index, u32 type,
		      unsigned long pa, u16 order, bool flush);
void hfi2_wfr_rcv_array_wc_fill(struct hfi2_ctxtdata *rcd, u32 index, u32 type);
void hfi2_wfr_set_port_tid_config(struct hfi2_devdata *dd, int pidx, u16 ctxt,
				  u32 eager_base, u16 alloced,
				  u32 expected_base, u32 expected_count);
void hfi2_quiet_serdes(struct hfi2_pportdata *ppd);
u64 hfi2_rctxt_ctrl_op(struct hfi2_devdata *dd, u16 ctxt, unsigned int op);
void hfi2_rcvctrl(struct hfi2_devdata *dd, unsigned int op,
		  struct hfi2_ctxtdata *rcd);
bool hfi2_is_control_context(struct hfi2_ctxtdata *rcd);
bool hfi2_is_kernel_context(struct hfi2_ctxtdata *rcd);
bool hfi2_is_dynamic_context(struct hfi2_ctxtdata *rcd);
bool hfi2_is_user_context(struct hfi2_ctxtdata *rcd);
u32 hfi2_read_cntrs(struct hfi2_devdata *dd, char **namep, u64 **cntrp);
u32 hfi2_read_portcntrs(struct hfi2_pportdata *ppd, char **namep, u64 **cntrp);
int hfi2_get_ib_cfg(struct hfi2_pportdata *ppd, int which);
int hfi2_set_ib_cfg(struct hfi2_pportdata *ppd, int which, u32 val);
int hfi2_set_ctxt_jkey(struct hfi2_devdata *dd, struct hfi2_ctxtdata *rcd,
		       u16 jkey);
int hfi2_clear_ctxt_jkey(struct hfi2_devdata *dd, struct hfi2_ctxtdata *ctxt);
int hfi2_set_ctxt_pkey(struct hfi2_devdata *dd, struct hfi2_ctxtdata *ctxt,
		       u16 pkey);
int hfi2_clear_ctxt_pkey(struct hfi2_devdata *dd, struct hfi2_ctxtdata *ctxt);
void hfi2_wfr_read_link_quality(struct hfi2_pportdata *ppd, u8 *link_quality);

irqreturn_t hfi2_general_interrupt(int irq, void *data);
irqreturn_t hfi2_sdma_interrupt(int irq, void *data);
irqreturn_t hfi2_sdma_interrupt_thr(int irq, void *data);
irqreturn_t hfi2_receive_context_interrupt(int irq, void *data);
irqreturn_t hfi2_receive_context_thread(int irq, void *data);
irqreturn_t hfi2_receive_context_interrupt_napi(int irq, void *data);

int hfi2_set_intr_bits(struct hfi2_devdata *dd, u16 first, u16 last, bool set);
void hfi2_init_qsfp_int(struct hfi2_pportdata *ppd);
void hfi2_clear_all_interrupts(struct hfi2_devdata *dd);
void hfi2_remap_intr(struct hfi2_devdata *dd, int isrc, int msix_intr);
void hfi2_remap_sdma_interrupts(struct hfi2_devdata *dd, int engine,
				int msix_intr);
void hfi2_reset_interrupts(struct hfi2_devdata *dd);
u16 hfi2_get_qp_map(struct hfi2_pportdata *ppd, u16 idx);
void hfi2_init_aip_rsm(struct hfi2_pportdata *ppd);
void hfi2_deinit_aip_rsm(struct hfi2_pportdata *ppd);
int hfi2_init_rxe_rsm(struct hfi2_devdata *dd, struct hfi2_devrsrcs *dr);
void hfi2_init_other(struct hfi2_devdata *dd);
void hfi2_init_early_variables(struct hfi2_devdata *dd);
void hfi2_wfr_set_port_max_mtu(struct hfi2_pportdata *ppd, u32 maxvlmtu);
u32 hfi2_slow_rhf_rcv_seq(struct hfi2_ctxtdata *rcd, u64 rhf);
void hfi2_release_rsm_rules(struct hfi2_devdata *dd);

/*
 * Interrupt source table.
 *
 * Each entry is an interrupt source "type".  It is ordered by increasing
 * number.
 */
struct hfi2_is_table {
	int start; /* interrupt source type start */
	int end; /* interrupt source type end */
	/* routine that returns the name of the interrupt source */
	char *(*is_name)(char *name, size_t size, unsigned int source);
	/* routine to call when receiving an interrupt */
	void (*is_int)(struct hfi2_devdata *dd, unsigned int source);
};

extern const struct hfi2_is_table hfi2_is_table[];

/* table entry for general interrupt enable */
struct gi_enable_entry {
	u32 start; /* starting source number */
	u32 end; /* ending source number */
};

extern const struct gi_enable_entry hfi2_wfr_gi_enable_table[];

/* interrupt clear down register type */
enum icd_type {
	ICD_NORMAL, /* non-indexed register */
	ICD_SDMA, /* indexed SDMA register */
	ICD_INGRESS, /* indexed ingress register */
	ICD_EGRESS, /* indexed egress register */
};

/*
 * Error interrupt table entry.  This is used as input to the interrupt
 * "clear down" routine used for all second tier error interrupt registers.
 * Second tier interrupt registers have a single bit representing them
 * in the top-level CceIntStatus.
 */
struct err_reg_info {
	u32 status; /* status CSR offset */
	u32 clear; /* clear CSR offset */
	u32 mask; /* mask CSR offset */
	enum icd_type type; /* register type */
	void (*handler)(struct hfi2_devdata *dd, u32 source, u64 reg);
	const char *desc;
};

/* helpers for filling out struct err_reg_info */
#define EE_N(reg, handler, desc)                                            \
	{                                                                   \
		reg##_STATUS, reg##_CLEAR, reg##_MASK, ICD_NORMAL, handler, \
			desc                                                \
	}

#define EE_S(reg, handler, desc)                                               \
	{                                                                      \
		reg##_STATUS, reg##_CLEAR, reg##_MASK, ICD_SDMA, handler, desc \
	}

#define EE_I(reg, handler, desc)                                             \
	{                                                                    \
		reg##_STATUS, reg##_CLEAR, reg##_MASK, ICD_INGRESS, handler, \
			desc                                                 \
	}

#define EE_E(reg, handler, desc)                                            \
	{                                                                   \
		reg##_STATUS, reg##_CLEAR, reg##_MASK, ICD_EGRESS, handler, \
			desc                                                \
	}

char *hfi2_is_sdma_eng_err_name(char *buf, size_t bsize, unsigned int source);
char *hfi2_is_sendctxt_err_name(char *buf, size_t bsize, unsigned int source);
char *hfi2_is_sdma_eng_name(char *buf, size_t bsize, unsigned int source);
char *hfi2_is_rcv_avail_name(char *buf, size_t bsize, unsigned int source);
char *hfi2_is_rcv_urgent_name(char *buf, size_t bsize, unsigned int source);
char *hfi2_is_send_credit_name(char *buf, size_t bsize, unsigned int source);

void hfi2_interrupt_clear_down(struct hfi2_devdata *dd, u32 context,
			       const struct err_reg_info *eri);
void hfi2_handle_sdma_eng_err(struct hfi2_devdata *dd, unsigned int context,
			      u64 err_status);
void hfi2_is_sdma_eng_int(struct hfi2_devdata *dd, unsigned int source);
void hfi2_is_sendctxt_err_int(struct hfi2_devdata *dd, unsigned int hw_context);
void hfi2_is_rcv_avail_int(struct hfi2_devdata *dd, unsigned int source);
void hfi2_is_rcv_urgent_int(struct hfi2_devdata *dd, unsigned int source);
void hfi2_is_send_credit_int(struct hfi2_devdata *dd, unsigned int source);
void hfi2_handle_temp_err(struct hfi2_devdata *dd);
void hfi2_handle_pio_err(struct hfi2_devdata *dd, u32 unused, u64 reg);
void hfi2_handle_sdma_err(struct hfi2_devdata *dd, u32 unused, u64 reg);
void hfi2_handle_rxe_err(struct hfi2_devdata *dd, u32 pidx, u64 reg);
void hfi2_handle_egress_err(struct hfi2_devdata *dd, u32 pidx, u64 reg);

void hfi2_update_statusp(struct hfi2_pportdata *ppd, u32 state);
const char *hfi2_link_state_name(u32 state);
const char *hfi2_link_state_reason_name(struct hfi2_pportdata *ppd, u32 state);
void hfi2_log_state_transition(struct hfi2_pportdata *ppd, u32 state);
void hfi2_update_xmit_counters(struct hfi2_pportdata *ppd, u16 link_width);
void hfi2_restore_qpmap_table(struct hfi2_devdata *dd);

u32 hfi2_encoded_size(u32 size);

struct cntr_entry {
	/* counter name */
	char *name;
	/* csr to read for name (if applicable) */
	u64 csr;
	/* offset into dd or ppd to store the counter's value */
	int offset;
	/* flags */
	u8 flags;
	/* accessor for stat element, context either dd or ppd */
	u64 (*rw_cntr)(const struct cntr_entry *entry, void *context, int vl,
		       int mode, u64 data);
};

extern struct cntr_entry hfi2_wfr_dev_cntrs[];
extern struct cntr_entry hfi2_jkr_dev_cntrs[];
extern struct cntr_entry hfi2_wfr_port_cntrs[];
extern struct cntr_entry hfi2_jkr_port_cntrs[];
extern struct cntr_entry hfi2_shared_dev_cntrs[];
extern struct cntr_entry hfi2_shared_port_cntrs[];

struct flag_table {
	u64 flag; /* the flag */
	char *str; /* description string */
	u16 extra; /* extra information */
	u16 unused0;
	u32 unused1;
};

struct flag_data {
	const struct flag_table *table;
	u32 size;
};

extern const struct flag_data hfi2_wfr_egress_err_info_data;

#endif /* _CHIP_H */
