/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: host.h
 *
 * Abstract: This Include file define host structure
 *
 * Version: 1.00
 *
 * Author: Peter.Guo
 *
 * Environment:	OS Independent
 *
 * History:
 *
 * 8/25/2014		Creation	Peter.Guo
 */

#ifndef _HOST_H
#define _HOST_H
#include "../include/basic.h"

/* 200MHz */
#define	SD_CLK_BASE				200000
/* ID Clock 400KHz */
#define	SD_CLK_ID_400K			400
/* 50MHz (HS,SDR25)		*/
#define	SD_CLK_50M			50000
/* 25MHz (DS,SDR12) */
#define   SD_CLK_25M             25000
/* 100MHz (SDR50) */
#define   SD_CLK_100M             100000
/* 200MHz (SDR104) */
#define   SD_CLK_200M             200000
/* 225MHz (DDR200) */
#define   SD_CLK_225M             225000
/* 75MHz (Lightning Mode) */
#define   SD_CLK_75M             75000

/* Example is 250ms for Abort time		*/
#define	RESET_FOR_ALL_ABRT_TM		250

typedef struct {
	/* host support max LSS DIR */
	u8 n_lss_dir:4,
	    /* host support max LSS SYN     */
	 n_lss_syn:4;
	/* host support max N_FCU   */
	u8 n_fcu;
	/* host support max DIDL between data packets */
	u8 n_data_gap;
	/* device alloc power host max support      */
	u8 gap:4,
	    /* group alloc power host max support */
	 dap:4;
	/* max spedd range host support */
	u8 speed_range:2,
	    /* max number of lane host support */
	 num_of_lane:6;
	/* max devices host support */
	u8 max_devices:4,
	    /*host support max retry count  */
	 retry_cnt:2;
	/* host support max block len        */
	u16 max_blk_len;
	/* host max support vdd2 power      */
	u32 vdd2_18_maxpower:29,
	    /* host vdd2 support    */
	 vdd2_ocr:3;

	/* base offset for uhs2 capability regs */
	u16 cap_base;
	/* base offset for uhs2 setting regs */
	u16 set_base;
	/* base offset for uhs2 test regs */
	u16 tst_base;
	/* base offset for uhs2 vendor regs */
	u16 vnd_base;
} host_uhs2_cap;

typedef struct {
	byte l1_substate:1;
	byte ltr:1;
	byte d3_silence:1;
	byte rtd3_hot:1;
	byte rtd3_cold:1;
} vendor_pm_feature;

typedef struct {
	vendor_pm_feature pm;
	u32 reserved;
	byte non_removalbe;

	/*
	 * Rule: The Capability which get our PCR or vendor mem area
	 */
} host_vendor_cap;

typedef struct {
	/* Hardware can check card response */
	byte hw_resp_chk:1;
	byte hw_autocmd:1;
	byte hw_pll_enable:1;
	byte hw_led_fix:1;
	/* 4.1 host support */
	byte hw_41_supp:1;

	/*
	 * Rule: The Feature the chip support
	 */
} host_feature;

typedef struct {
	u32 error_code;
	u16 legacy_err_reg;
	u32 uhs2_err_reg;
	byte app_stage;

	u32 resp_err;
} cmd_err_t;

#define INTR_CB_ERR	-1
#define INTR_CB_OK	0
#define INTR_CB_NOEND	1

typedef u32(*intr_callback) (void *card, void *host_request);
typedef u32(*req_callback) (void *pdx, cmd_err_t *err);
typedef u32(*issue_post_callback) (void *pdx);

/*
 * Card Response Error Type
 */
#define    RESP_OUT_OF_RANGE		0x8000
#define    RESP_ADDRESS_ERROR		0x4000
#define    RESP_BLOCK_LEN_ERROR         0x2000
#define    RESP_ERASE_SEQ_ERROR         0x1000
#define    RESP_ERASE_PARAM             0x0800
#define    RESP_WP_VIOLATION            0x0400
#define    RESP_LOCK_UNLOCK_FAILED      0x0100
#define    RESP_COM_CRC_ERROR           0x0080
#define    RESP_ILLEGAL_COMMAND         0x0040
#define    RESP_CARD_ECC_FAILED         0x0020
#define    RESP_CC_ERROR                0x0010
#define    RESP_ERROR                   0x0008
#define    RESP_UNDERRUN                0x0004
#define    RESP_OVERRUN                 0x0002
#define    RESP_CIDCSD_OVERWRITE        0x0001

/*
 * Error Code definition(0 means ok)
 */
#define	ERR_CODE_NO_CARD	1
#define ERR_CMDLINE_INHABIT	2
#define ERR_DATLINE_INHABIT	3
#define ERR_CODE_INVALID_CMD	4
#define ERR_CODE_RESP_ERR	5
#define ERR_CODE_TIMEOUT	6
#define ERR_CODE_INTR_ERR	7
#define ERR_CODE_EXCEPT_STOP 8
#define  ERR_CODE_AUTORESP_ERR	9
#define	ERR_CODE_SOFTARE_ARG	10

#define RESP_ERR_TYPE_OUT_OF_RANGE		(1<<31)
#define  RESP_ERR_TYPE_ADDRESS_ERROR	(1<<30)
#define  RESP_ERR_TYPE_BLOCK_LEN_ERROR	(1<<29)
#define  RESP_ERR_TYPE_ERASE_SEQ_ERROR	(1<<28)
#define  RESP_ERR_TYPE_ERASE_PARAM		(1<<27)
#define  RESP_ERR_TYPE_WP_VIOLATION		(1<<26)
#define  RESP_ERR_TYPE_LOCK_UNLOCK      (1<<24)
#define  RESP_ERR_TYPE_COM_CRC_ERROR	(1<<23)
#define  RESP_ERR_TYPE_ILLEGAL_CMD		(1<<22)
#define  RESP_ERR_TYPE_CARD_ECC_FAILED	(1<<21)
#define  RESP_ERR_TYPE_CC_ERROR			(1<<20)
#define  RESP_ERR_TYPE_ERROR			(1<<19)

#define  RESP_ERR_TYPE_CSD_OVERWRITE	(1<<16)

#define RESP_ERR_TYPE_FUNC_NUM		(1<<1)

typedef struct {
	bool auto_flag;
	u32 sdr104_auto_flag;
	u32 sdr50_auto_flag;
	u32 ddr50_auto_flag;
	u32 sdhs_auto_flag;
	u32 start_block;
	u32 auto_phase;
	bool auto_phase_flag;
} output_tuning_t;

typedef struct host_cmd_req_s {
	u16 int_flag_wait;
	u16 int_flag_err;
	u32 int_flag_uhs2_err;

	completion_t done;

	e_infinite_mode inf_mode;
	e_card_type card_type;
	e_trans_type trans_type;

	/* to sd_cmd */
	void *private;
	/* pointer to sd_card_t */
	void *card;

	intr_callback cb_response;
	intr_callback cb_buffer_ready;
	intr_callback cb_trans_complete;
	intr_callback cb_boundary;

	req_callback cb_req_complete;
	issue_post_callback issue_post_cb;
} host_cmd_req_t;

typedef struct {
	u16 vendor_id;
	u16 device_id;
	u16 revision_id;
	/* PCR 0xDC[31:24] */
	u16 sub_version;

	e_chip_type chip_type;

	t_pci_dev pci_dev;
	cfg_item_t *cfg;

	u32 ocr_avail;
	u32 mmc_ocr_avail;

	u32 vdd2_12v_supp:1,
	    vdd2_18v_supp:1,
	    bit64_v3_supp:1,
	    bit64_v4_supp:1,
	    adma3_supp:1,
	    uhs2_supp:1, adma2_supp:1, sdma_supp:1, hs_supp:1, bus_8bit_supp:1;
	u16 max_block_len;

	u16 max_vdd2_current;
	u16 max_18vdd1_current;
	u16 max_30vdd1_current;
	u16 max_33vdd1_current;

	host_uhs2_cap uhs2_cap;
	host_vendor_cap ven_cap;
	host_feature feature;

	host_cmd_req_t *cmd_req;

	u16 sdma_boundary_val;
	u32 sdma_boundary_kb;
	/* 64bit DMA enable */
	byte bit64_enable;
	byte sd_host4_enable;
	bool led_on;
	bool uhs2_flag;
	bool sd_express_flag;
	bool dump_mode;
	/* NON INTERRUPT */
	bool poll_mode;
	atomic_t clkreqn_status;

	void *pdx;
	output_tuning_t output_tuning;
	u8 cur_output_phase;

	/* ONLY for camera mode: polling card state */
#define CARD_INSERTED 1
#define CARD_DESERTED 0
	bool camera_mode_card_state;

} sd_host_t;

typedef struct {
	u32 trans_mode;
	u32 payload[5];
	u32 block_cnt;
	u32 block_size;
} host_trans_reg_t;

#endif
