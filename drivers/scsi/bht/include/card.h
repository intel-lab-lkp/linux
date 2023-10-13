/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: card.h
 *
 * Abstract: This Include file define card structure
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

#ifndef _CARD_H

#define _CARD_H

#include "host.h"

typedef struct {
	u32 manfid;
	byte prod_name[8];
	byte prv;
	u32 serial;
	u16 oemid;
	u16 reserved;

} cid_t;

typedef struct {
	/* CSD structure                 */
	byte csd_structure;
	/* MMC spec version              */
	byte mmc_spec_vers;
	/* read data block length        */
	byte read_bl_len;
	/* device size                   */
	u32 c_size;
	/* c_size_mult                   */
	u32 c_size_mult;
	/* erase sector size             */
	u16 sector_size;
	/* max.data transfer rate        */
	byte tran_speed;
	/* taac                          */
	byte taac;
	/* nsac                          */
	byte nsac;

	/* permanent write protection    */
	byte parm_protect;
	/* temporary write protection    */
	byte temp_protect;

#if (0)
	u16 cmdclass;
	u16 tacc_clks;
	u32 tacc_ns;
	u32 c_size;
	u32 r2w_factor;
	u32 max_dtr;
	/* In sectors */
	u32 erase_size;
	u32 read_blkbits;
	u32 write_blkbits;
	u32 capacity;
	u32 read_partial:1, read_misalign:1, write_partial:1, write_misalign:1;

#endif
} csd_t;

typedef struct {
#if (0)
	byte rev;
	byte erase_group_def;
	byte sec_feature_support;
	byte rel_sectors;
	byte rel_param;
	byte part_config;
	byte cache_ctrl;
	byte rst_n_function;
	byte max_packed_writes;
	byte max_packed_reads;
	byte packed_event_en;
	/* Units: ms */
	u32 part_time;
	/* Units: 100ns */
	u32 sa_timeout;
	/* Units: 10ms */
	u32 generic_cmd6_time;
	/* Units: ms */
	u32 power_off_longtime;
	/* state */
	byte power_off_notification;
	u32 hs_max_dtr;
#define MMC_HIGH_26_MAX_DTR	26000000
#define MMC_HIGH_52_MAX_DTR	52000000
#define MMC_HIGH_DDR_MAX_DTR	52000000
#define MMC_HS200_MAX_DTR	200000000
	u32 sectors;
	u32 card_type;
	/* In sectors */
	u32 hc_erase_size;
	/* In milliseconds */
	u32 hc_erase_timeout;
	/* Secure trim multiplier  */
	u32 sec_trim_mult;
	/* Secure erase multiplier */
	u32 sec_erase_mult;
	/* In milliseconds */
	u32 trim_timeout;
	/* enable bit */
	byte enhanced_area_en;
	/* Units: Byte */
	unsigned long long enhanced_area_offset;
	/* Units: KB */
	u32 enhanced_area_size;
	/* Units: KB */
	u32 cache_size;
	/* HPI enablebit */
	byte hpi_en;
	/* HPI support bit */
	byte hpi;
	/* cmd used as HPI */
	u32 hpi_cmd;
	/* background support bit */
	byte bkops;
	/* background enable bit */
	byte bkops_en;
	/* 512 bytes or 4KB */
	u32 data_sector_size;
	/* DATA TAG UNIT size */
	u32 data_tag_unit_size;
	/* ro lock support */
	u32 boot_ro_lock;
	bool boot_ro_lockable;
	/* 54 */
	byte raw_exception_status;
	/* 160 */
	byte raw_partition_support;
	/* 168 */
	byte raw_rpmb_size_mult;
	/* 181 */
	byte raw_erased_mem_count;
	/* 194 */
	byte raw_ext_csd_structure;
	/* 196 */
	byte raw_card_type;
	/* 198 */
	byte out_of_int_time;
	/* 200 */
	byte raw_pwr_cl_52_195;
	/* 201 */
	byte raw_pwr_cl_26_195;
	/* 202 */
	byte raw_pwr_cl_52_360;
	/* 203 */
	byte raw_pwr_cl_26_360;
	/* 217 */
	byte raw_s_a_timeout;
	/* 221 */
	byte raw_hc_erase_gap_size;
	/* 223 */
	byte raw_erase_timeout_mult;
	/* 224 */
	byte raw_hc_erase_grp_size;
	/* 229 */
	byte raw_sec_trim_mult;
	/* 230 */
	byte raw_sec_erase_mult;
	/* 231 */
	byte raw_sec_feature_support;
	/* 232 */
	byte raw_trim_mult;
	/* 236 */
	byte raw_pwr_cl_200_195;
	/* 237 */
	byte raw_pwr_cl_200_360;
	/* 238 */
	byte raw_pwr_cl_ddr_52_195;
	/* 239 */
	byte raw_pwr_cl_ddr_52_360;
	/* 246 */
	byte raw_bkops_status;
	/* 212 - 4 bytes */
	byte raw_sectors[4];

	u32 feature_support;
#define MMC_DISCARD_FEATURE	1	/* CMD38 feature */
#endif

	/* 196 */
	u8 card_type;
	/* 197 */
	u8 driver_strength_type;
	/* 200 */
	u8 pwr_cl_52_195;
	/* 201 */
	u8 pwr_cl_26_195;
	/* 202 */
	u8 pwr_cl_52_360;
	/* 203 */
	u8 pwr_cl_26_360;
	/* 238 */
	u8 pwr_cl_ddr_52_195;
	/* 239 */
	u8 pwr_cl_ddr_52_360;
	/* 212 ~ 215 */
	u32 sec_cnt;
} extcsd_t;

#define CID_LEN	(16)

#define SD_SCR_BUS_WIDTH_1	(1<<0)
#define SD_SCR_BUS_WIDTH_4	(1<<2)
#define SD_SCR_CMD20_SUPPORT   (1<<0)
#define SD_SCR_CMD23_SUPPORT   (1<<1)

typedef struct {
	/* SD Spec Version               */
	byte sd_spec;

	/* SD Spec Version3              */
	unsigned char sd_spec3;
	byte sd_specx;
	byte reserved_B0;
	byte reserved_B1;
	/* CMD Support                   */
	byte cmd_support;
	/* reserved                      */
	u16 reserved;

} sd_scr_t;

typedef struct {
	/* Size of AU */
	u32 au_size;
	/* Speed Class of the card */
	u32 speed_class;
	/* In second */
	u32 erase_timeout;
	/* In second */
	u32 erase_offset;
} sd_ssr_t;

/* Access Mode */
#define UHS_SDR12_BUS_SPEED	(0)
#define HIGH_SPEED_BUS_SPEED	(1)
#define UHS_SDR25_BUS_SPEED	(1)
#define UHS_SDR50_BUS_SPEED	(2)
#define UHS_SDR104_BUS_SPEED	(3)
#define UHS_DDR50_BUS_SPEED	(4)

/* Drive Strength */
#define SD_DRIVER_TYPE_B	(1 << 0)
#define SD_DRIVER_TYPE_A	(1 << 1)
#define SD_DRIVER_TYPE_C	(1 << 2)
#define SD_DRIVER_TYPE_D	(1 << 3)

/* Power Limit */
#define SD_POWER_LIMIT_200	(1 << 0)
#define SD_POWER_LIMIT_400	(1 << 1)
#define SD_POWER_LIMIT_600	(1 << 2)
#define SD_POWER_LIMIT_800	(1 << 3)
#define	SD_POWER_LIMIT_180W	(1 << 4)

#define	SD_FNC_CHK			0x00000000UL
#define	SD_FNC_SW				0x80000000UL
#define	SD_FNC_GET				0x0000000FUL
#define	SD_FNC_NOINFL			0x00FFFFFFUL
#define	SD_FNC_G1_INFL			0x00FFFFF0UL
#define	SD_FNC_G2_INFL			0x00FFFF0FUL
#define	SD_FNC_G3_INFL			0x00FFF0FFUL
#define	SD_FNC_G4_INFL			0x00FF0FFFUL
#define	SD_FNC_G5_INFL			0x00F0FFFFUL
#define	SD_FNC_G6_INFL			0x000FFFFFUL
#define   SD_FNC_G2_VEN              0x000FFFEFUL
#define	SD_FNC_GRP1			1U
#define	SD_FNC_GRP2			2U
#define	SD_FNC_GRP3			3U
#define	SD_FNC_GRP4			4U
#define	SD_FNC_GRP5			5U
#define	SD_FNC_GRP6			6U

/* SD 2.0 default speed 25M */
#define	SD_FNC_NO_DS			0x100
/* SD 2.0 default speed 50M */
#define	SD_FNC_NO_HS			0x101
#define	SD_FNC_NO_0			0x0
#define	SD_FNC_NO_1			0x1
#define	SD_FNC_NO_2			0x2
#define	SD_FNC_NO_3			0x3
#define	SD_FNC_NO_4			0x4
#define	SD_FNC_NO_5			0x5
#define	SD_FNC_NO_6			0x6
#define	SD_FNC_NO_7			0x7
#define	SD_FNC_NO_8			0x8
#define	SD_FNC_NO_9			0x9
#define	SD_FNC_NO_A			0xA
#define	SD_FNC_NO_B			0xB
#define	SD_FNC_NO_C			0xC
#define	SD_FNC_NO_D			0xD
#define	SD_FNC_NO_E			0xE
#define	SD_FNC_NO_F			0xF

#define	SD_FNC_AM_SDR12		0x0
#define	SD_FNC_AM_SDR25		0x1
#define	SD_FNC_AM_HS		0x1
#define	SD_FNC_AM_DS		0x0

#define	SD_FNC_AM_SDR50		0x2
#define	SD_FNC_AM_SDR104		0x3
#define	SD_FNC_AM_DDR50		0x4
#define	SD_FNC_AM_DDR200		0x5

#define GROUP_FN4_POWERLIMIT_288W_CAP    (4)
#define GROUP_FN4_POWERLIMIT_216W_CAP    (3)
#define GROUP_FN4_POWERLIMIT_180W_CAP    (2)
#define GROUP_FN4_POWERLIMIT_144W_CAP    (1)
#define GROUP_FN4_POWERLIMIT_072W_CAP    (0)

#define SD_FNC_PL_288W       (3)
#define SD_FNC_PL_216W       (2)
#define SD_FNC_PL_180W       (4)
#define SD_FNC_PL_144W       (1)
#define SD_FNC_PL_072W       (0)

typedef struct {
	byte sd_access_mode;
	byte sd_command_system;
	byte sd_drv_type;
	byte sd_power_limit;
} sd_sw_func_t;

typedef struct {
	/* device-specify number of DIR LSS */
	byte n_lss_dir:4,
	    /* device-specify number of DIR SYN */
	 n_lss_syn:4;
	/* max block number in a flow control unit */
	byte n_fcu;
	/* number of DIDL between DATA packets      */
	byte n_data_gap;
	/* Device Range support     */
	byte speed_range:2,
	    /* device support hibernate or not  */
	 hibernate:1,
	    /* number of lanes  */
	 lanes:4,
	    /* device support power mode        */
	 pwr_mode:1;
	/* card max block length    */
	u16 max_blk_len:12,
	    /* card retry cnt setting       */
	 retry_cnt:2, half_supp:1, reserved:1;
} uhs2_info_t;

#define MID_SANDISK   (0x3)

typedef struct {

	u16 rca;

	byte raw_cid[16];
	byte raw_csd[16];
	csd_t csd;
	cid_t cid;

	/* for SD only */
	byte raw_scr[8];
	/* for SD only */
	byte raw_ssr[64];
	/* sd_ssr_t ssr; */
	sd_scr_t scr;

	/*
	 * Card Capacity Status (ACMD41 response).
	 * 0: SDSC; 1: SDHC or SDXC.
	 * This flag also can be used for MMC
	 * (OCR Access mode is sector mode or not)
	 */
	byte card_ccs;
	/* S18A (ACMD41 response) Switching to 1.8v Accepted */
	byte card_s18a;

	/* work at DDR mode */
	byte ddr_flag;

	/* Indicate which Io signal the card work ok */
	byte io_signal_vol;
#define CARD_IO_VDD_33V	0
#define CARD_IO_VDD_18V	1
#define CARD_IO_VDD_12V	2

	sd_sw_func_t sw_func_cap;
	/*
	 * current settings,
	 * especially access mode should not > target_access_mode and sw_func_cap.
	 */
	sd_sw_func_t sw_cur_setting;

} card_info_t;

/*
 * SCR field definitions
 */
#define SCR_SPEC_VER_0		0	/* Version 1.0 and 1.01 */
#define SCR_SPEC_VER_1		1	/* Version 1.10 */
#define SCR_SPEC_VER_2		2	/* Version 2.00 or Version 3.00 */

typedef struct {
	/* UHS2 Device Id; */
	byte dev_id;

	uhs2_info_t uhs2_cap;
	uhs2_info_t uhs2_setting;
} uhs2_card_info_t;

typedef struct {
	u32 part_capacity;
	u32 part_idx;
	bool write_protected;
} mmc_part_info_t;

#define EMMC_MODE_NONE  0
#define EMMC_MODE_HS200	1
#define EMMC_MODE_HS400 2

#define EMMC_1Bit_BUSWIDTH   0
#define EMMC_4Bit_BUSWIDTH   1
#define EMMC_8Bit_BUSWIDTH   2

typedef struct {
	byte raw_extcsd[512];
	extcsd_t ext_csd;
#if (0)
	u32 partnum;
	mmc_part_info_t part_info[MAX_EMMC_PARTION];
#endif
	byte cur_hs_type;
	/* 0: 1-bit    1: 4-bit     2: 8-bit */
	byte cur_buswidth;
	byte drv_strength;
} mmc_card_info_t;

typedef enum {
	/* Card not present, or card initialize not start/finished, or card init failed */
	CARD_STATE_POWEROFF = 0,
	CARD_STATE_SLEEP,
	CARD_STATE_DEEP_SLEEP,
	/* set to this state when card can read write */
	CARD_STATE_WORKING,
} card_pm_state_t;

typedef struct {
	card_info_t info;
	mmc_card_info_t mmc;
	uhs2_card_info_t uhs2_info;

	e_card_type card_type;
	/* pointer to sd_host structure */
	sd_host_t *host;

	/* Infinite transfer built or not */
	byte has_built_inf;
	e_data_dir last_dir;
	u32 last_sect;
	byte inf_trans_enable;
	/* This is used for case which set accroding to card CID */
	u32 quirk;

	/* Indicate card was initialized successfully once. */
	bool initialized_once;
	bool quick_init;

	bool thread_init_card_flag;
	/* this flag indicate whether card exist or not */
	bool card_present;
	bool sw_ctrl_swicth_to_express;
	card_pm_state_t state;
	/* this flag indicated whether card is locked or not */
	bool locked;

	/* Card specific information */
	/* Card Capacity */
	u64 sec_count;
	bool write_protected;

	/* to indicate whether card is changed */
	bool card_chg;
	/*
	 * This flag is used to store card last taraget setting according to
	 * register and capability(not include thermal)
	 */
	sd_sw_func_t sw_target_setting;

	/* below 2 field is used for degrade and error recovery it is set by degrade policy */
	/* 0 means not degrade */
	u8 degrade_uhs2_range:1,
	    /* 0 means not degrade */
	 degrade_uhs2_half:1,
	    /* 1 means degrade to legacy */
	 degrade_uhs2_legacy:1,
	    /* 1 means degrade to last mode, if init still failed set error flag of card */
	 degrade_final:1;
	/* the level is a index add to card start of  base freqncy table */
	u16 degrade_freq_level;

	/*
	 * below item is used for thremal control, it is set by
	 * thermal control function and used by card init and apis
	 */

	/* 1 means card_init need to set card last mode according to this info */
	u16 thermal_enable:1,
	    /* 1 means RangeB, 0 means RangeA */
	 thermal_uhs2_range:1,
	    /* 0 means use Half, 1 means not use Half */
	 thermal_uhs2_half_dis:1,
	    /* 1 means low power, 0 means normal */
	 thermal_uhs2_lpm:1,
	    /* 1 means change to higher work mode, 0 means change to lower work mode */
	 thermal_heat:1, thermal_access_mode:5, thermal_power_limit:5;

	/* gg8 */
	bool uhs2_card;
	bool card_support_pcie;
	bool card_support_vdd3;
	bool pcie_init_flag;
	bool uhs2_trail_run;
	bool check_result;
	bool cmd_check_uhs2_flag;
	bool read_signal_block_flag;
	bool restore_tuning_content_fail;
	bool cmd_low_reset_flag;
	bool ddr225_card_flag;
	/* Error recover counter */
	u32 continue_init_fail_cnt;
	u32 adma_err_cnt;
	u32 continue_rw_err_cnt;
	host_cmd_req_t cmd_req;

	/* output tuning */
	u8 input_phase_all_pass;
	u8 retry_output_fail_phase;
	u8 output_input_phase_pair[14];
} sd_card_t;

typedef struct {
	byte *driver_buff;
	/* this is used for dma case, for pio it is not used */
	virt_buffer_t srb_buffer[MAX_WORK_QUEUE_SIZE];
	u32 total_bytess;
	u32 offset;
	u32 srb_cnt;
	phy_addr_t sys_addr;
/* this is used for sdma(like) & pio */
} data_dma_mng_t;

typedef struct {
	e_data_dir dir;
	u32 block_size;
	u32 block_cnt;
	data_dma_mng_t data_mng;
} sd_data_t;

typedef struct {
	u32 argument;
	int payload_cnt;
	u32 uhs2_header;
	byte cmd_index;
	/* set array to 2 for legacy acmd */
	host_trans_reg_t trans_reg[2];
	byte trans_reg_cnt;

	/*
	 * for R2 Resp the byte order is byte[0]:127:120 byte[1]:119:112 ... ;
	 * for other is byte[0] is 15:8 ,byte[1]: 23:16...
	 */
	u32 response[4];

	sd_data_t *data;

	byte app_cmd:1,
	    uhs2_cmd:1, muldat_cmd:1, sd_cmd:1, uhs2_set_pld:1, hw_resp_chk:1;
	u32 cmd_flag;

	/* this flag is used to return whether command complete is occur */
	byte cmd_done:1,
	    /* this flag is used to indicate whether uhs2 command is nack */
	 uhs2_nack:1;

	cmd_err_t err;
	u32 timeout;
	bool gg8_ddr200_workaround;

} sd_command_t;

void host_init_400k_clock(sd_host_t *host);
void host_set_vdd2_power(sd_host_t *host, bool on, u32 vol_sel);
void host_internal_clk_setup(sd_host_t *host, bool on);
bool host_get_vdd1_state(sd_host_t *host);
void host_set_vdd1_power(sd_host_t *host, bool on, u32 vol_sel);
void host_enable_clock(sd_host_t *host, bool on);

/* uniformed interface for vdd1,2,3 power set, controlled by registry */
void host_set_vddx_power(sd_host_t *host, u8 vddx, bool on);

bool sd_send_if_cond(sd_card_t *card, sd_command_t *sd_cmd, u32 argument);

bool card_init_ready(sd_card_t *card, sd_command_t *sd_cmd, bool flag_f8);

inline bool uhs1_support(sd_host_t *host);
bool send_acmd(sd_card_t *card, sd_command_t *sd_cmd);

bool pcie_mode_init(sd_card_t *card, bool code_flag);
bool gg8_uhs1_init(sd_card_t *card);
bool gg8_uhs2_init(sd_card_t *card);
bool gg8_get_card_capability_flag(sd_card_t *card, bool flag);
#endif
