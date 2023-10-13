/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: cfgmng.h
 *
 * Abstract: This File is used to define interface for Dynamic configuration
 *
 * Version: 1.00
 *
 * Author: Amma.Li
 *
 * Environment:	OS Independent
 *
 * History:
 *
 * 8/25/2014		Creation	Amma.Li
 */

#ifndef _CFGMNG_H
#define _CFGMNG_H

/* not contain the subitem "Parameters" */
#define REGISTRY_KEY_NUM  (100)

#define MAX_FREQ_SUPP  34

#define CFG_TRANS_MODE_SDMA    0
#define CFG_TRANS_MODE_ADMA2  1
#define CFG_TRANS_MODE_ADMA3  2
#define CFG_TRANS_MODE_ADMA2_SDMA_LIKE  3
#define CFG_TRANS_MODE_ADMA3_SDMA_LIKE  4
#define CFG_TRANS_MODE_ADMA_MIX  5
#define CFG_TRANS_MODE_ADMA_MIX_SDMA_LIKE	6
#define CFG_TRANS_MODE_ADMA2_ONLY  7
#define CFG_TRANS_MODE_ADMA2_ONLY_SDMA_LIKE	8

#define CFG_TRANS_MODE_PIO        0xF

typedef struct {
	/* output tuning SDR104 mode fixed value. */
	u32 fixed_value_sdr104:4;
	/* output tuning SDR50 mode fixed value. */
	u32 fixed_value_sdr50:4;
	u32 sdr50_input_fix_phase_value:4;
	/* output tuning DDR200 mode fixed value. */
	u32 fixed_value_ddr200:4;
	/* output tuning SDR104 mode enable flag: 1 means enable, 0 means disable(default) . */
	u32 enable_sdr104:1;
	/* output tuning SDR50 mode enable flag: 1 means enable, 0 means disable(default) */
	u32 enable_sdr50:1;

	u32 resrv:1;
	/* output tuning DDR200 mode enable flag: 1 means enable, 0 means disable(default) . */
	u32 enable_ddr200:1;
	/*
	 * select output tuning Auto mode or fixed mode:
	 * 1 means Auto mode, 0 means fixed mode(default).
	 */
	u32 auto_or_fixed:1;
	/*
	 * select output tuning Auto mode or fixed mode:
	 * 1 means Auto mode, 0 means fixed mode(default).
	 */
	u32 enable_dll_divider:1;
	/*
	 * output tuning eMMC HS400 mode enable flag:
	 * 1 means enable, 0 means disable(default).
	 */
	u32 enable_emmc_hs400:1;
	/* output tuning eMMC HS400 mode fixed value. */
	u32 fixed_value_emmc_hs400:4;
	u32 sdr104_input_fix_phase_value:4;
	/*
	 * output tuning function Global enable flag:
	 * 1 means enable, 0 means disable(default)
	 */
	u32 enable_dll:1;
} cfg_output_tuning_item_t;

typedef struct {
	/*
	 * 0: SDMA, 1: ADMA2, 2: ADMA3, 3: ADMA2_SDMA_Like,
	 * 4: ADMA3_SDMA_Like, 0xF: PIO, other: Reserved
	 */
	u32 dma_mode:4;

	/* Enable dma 26bit data length,  0: Disable, 1: Enable */
	u32 enable_dma_26bit_len:1;
	/* Enable dma 64bit address  0: Disable, 1: Enable */
	u32 enable_dma_64bit_address:1;
	/* enable 32bit block count   0: Disable, 1: Enable */
	u32 enable_dma_32bit_blkcount:1;
	u32 reserved_1:24;
	/* dma transfer mode setting valid,  0: invalid  1: valid */
	u32 reserve:1;
} cfg_dma_mode_setting_t;

typedef struct {
	/* enable SD3.0 card infinite transfer mode    0: disable, 1: enable */
	u32 enable_legacy_inf:1;
	/* enable SD4.0 card infinite transfer mode    0: disable, 1: enable */
	u32 enable_sd40_inf:1;
	/* enable MMC card infinite transfer mode    0: disable, 1: enable */
	u32 enable_mmc_inf:1;
	/* enable eMMC card infinite transfer mode    0: disable, 1: enable */
	u32 enable_emmc_inf:1;
	u32 reserved_1:27;
	/* infinite transfer mode main switch  0: disable, 1: enable */
	u32 enable_inf:1;
} cfg_infinite_transfer_mode_t;

typedef struct {
	/* 0: disable  1: enable */
	u32 enable_cmdio_packet_test:1;
	/* ADMA descriptor item length test    0: Disable, 1: Enable */
	u32 enable_adma_descitem_test:1;
	u32 reserved_1:29;
	/* enable ADMA transfer test, 0: disable  1: enable */
	u32 enable_adma_test:1;
} cfg_adma_test_setting_t;

typedef struct {
	/* unit: Kb, 0x4 = 4KB, 0x400 = 1MB */
	u32 value:16;
	u32 reserved_1:15;
	/*
	 * nable SDMA boundary length Setting
	 * 0: use the default boundary length
	 * 1: enable SDMA boundary length Setting
	 */
	u32 reserve:1;
} cfg_sdma_boundary_t;

typedef struct {
	/* Enable SDMA 4-bytes test mode  0: disable, 1: enable */
	u32 enable_sdma_4bit_test:1;
	/* disable boundary alignment buffer for SDMA  0: enable, 1: disable */
	u32 dis_sdma_buffer_alignment:1;
	u32 reserved_1:29;
	/* enable sdma transfer test, 0: disable  1: enable */
	u32 enable_sdma_test:1;
} cfg_sdma_test_setting_t;

typedef struct {
	/* DMA buffer size ,(unit: 64kb) */
	u32 dma_buffer_size:16;
	/* (unit: KB) */
	u32 dma_buffer_align:4;
	u32 reserved_1:11;
	/*
	 * enable set DMA buffer size
	 * 0: use the default buffer size (1MB), 1: enable set new buffer size
	 */
	u32 reserve:1;
} cfg_dma_buffer_size_t;

typedef struct {
	/* max transfer length value (unit: 512byte) */
	u32 value:16;
	u32 reserved_1:15;
	/*
	 * enable set max transfer length
	 * 0: use the default max transfer length (1MB)  1: enable set
	 */
	u32 reserve:1;
} cfg_max_trans_len_t;

typedef struct {
	/* max number of SRB  ADMA2: set 1 SRBs  0: means 16 SRBs(default) */
	u32 max_srb:16;
	/* 0: disable  1: enable */
	u32 enable_srb_merge:1;
	/* 0: disable  1: enable */
	u32 reserved_1:14;
	/* tag queue settings valid 0: invalid  1: valid */
	u32 reserve:1;
} cfg_tag_queue_capability_t;

typedef struct {
	/* [0]    reserved */
	u32 resevred:1;
	/* [1]    disable SD 4.0 card mode, 0: enable   1: disable */
	u32 dis_sd40_card:1;
	/* [2]    disable SD 3.0 card mode, 0: enable   1: disable */
	u32 dis_sd30_card:1;
	/* [3]    reserved */
	u32 reserve_3:1;
	/* [4]    disable SD 7.0 card mode, 0: enable   1: disable  */
	u32 dis_sd70_card:1;
	/* [6:5]  reserved */
	u32 reserve_2:2;
	/* [30:7] reserved */
	u32 reserve_1:24;
	/* [31]SD card mode disable settings valid, 0: invalid  1: valid */
	u32 sd_mode_dis_enable:1;
} cfg_sd_card_mode_dis_t;

#define CFG_ACCESS_MODE_SDR12  0
#define CFG_ACCESS_MODE_SDR25  1
#define CFG_ACCESS_MODE_SDR50  2
#define CFG_ACCESS_MODE_SDR104  3
#define CFG_ACCESS_MODE_DDR50    4

#define CFG_DRIVER_TYPE_A  1
#define CFG_DRIVER_TYPE_B  0
#define CFG_DRIVER_TYPE_C  2
#define CFG_DRIVER_TYPE_D  3
typedef struct {
	/* Driver strength select, 0h: Type B, 1h: Type A, 2h: Type C, 3h: Type D. Default is 0h. */
	u32 value:8;
	u32 reserved_1:23;
	/* enable driver strength select, 0: disable  1: enable */
	u32 enable_set:1;
} cfg_driver_strength_sel_t;
#define CFG_POWER_LIMIT_72   0
#define CFG_POWER_LIMIT_144   1
#define CFG_POWER_LIMIT_180   2
#define CFG_POWER_LIMIT_216   3
#define CFG_POWER_LIMIT_288   4

#define CFG_TUNING_MODE_SW   0
#define CFG_TUNING_MODE_HW   1

typedef struct {
	u32 reserved_1:31;
	/* Disable MMC function.    0: enable      1: disable */
	u32 dis_mmc_func:1;
} cfg_mmc_mode_dis_t;

typedef struct {
	/* [0] eMMC enable DDR mode, 0: disable, 1: enable (Default: 0) */
	u32 enable_ddr_mode:1;
	/* [1]eMMC card use 1.8V Vccq   0: no use   1: use */
	u32 enable_18_vccq:1;
	/* [2]eMMC disable high speed, 0: Don't disable, 1: Disable (Default: 0) */
	u32 dis_hs:1;
	/* [3]eMMC disable 4-bit bus, 0: Don't disable, 1: Disable (Default: 0) */
	u32 dis_4bit_bus_width:1;
	/* [4]eMMC disable 8-bit bus, 0: Don't disable, 1: Disable (Default: 0) */
	u32 dis_8bit_bus_width:1;
	/*
	 * [5]switch eMMC to hs200 mode force or automatically
	 * 0: automatically switch    1: force switch
	 */
	u32 enable_force_hs200:1;
	/*
	 * [6]switch eMMC to hs400 mode force or automatically
	 * 0: automatically switch    1: force switch
	 */
	u32 enable_force_hs400:1;
	/* [7]eMMC card use 1.2V Vccq   0: no use   1: use */
	u32 enable_12_vccq:1;
	/* [8]eMMC card use 1.8V Vcc   0: use 3.3v Vcc   1: use 1.8v Vcc */
	u32 enable_18_vcc:1;
	/*
	 * [12:9]Set the driver strength for emmc hs400/hs200 mode
	 * (0: Type0,1: Type1,2: Type2,3: Type3,4: Type4)
	 */
	u32 drv_strength:4;
	/*
	 * [13]force switch HS timing no matter whether
	 * the card support HS200/HS400
	 * 0: disable   1: enable
	 */
	u32 enable_force_hs:1;
	/* [14:30] */
	u32 reserved_1:17;
	/* [31] force eMMC work flow     0: auto init work flow     1: force eMMC work flow */
	u32 emmc_enable:1;
} cfg_emmc_mode_t;

typedef struct {
	/* [2:0] */
	u32 switch_method_ctrl:3;
	/* [3]   SW_CTL signal polarity selection */
	u32 sw_ctrl_polarit:1;
	/* [4]   camera mode enable/disable control */
	u32 camera_mode_enable:1;
	/* [5] */
	u32 sd_cmd_low_function_en:1;
	/* [6] */
	u32 vdd3_control:1;
	/* [7]   0:SD CMD8, 1:Trail run */
	u32 sd70_trail_run:1;
	/* [8]   0:Shift byte disable, 1:Shift byte enable */
	u32 shift_byte_en:1;
	/* [9]   select card flow 0: sd7.0->uhs2->uhs1 1:uhs2->sd7.0->uhs1 */
	u32 card_init_flow_select:1;
	/* [31:10] */
	u32 reserve:22;
} cfg_sd7_sdmode_switch_control_t;

#define CFG_UHS2_FULL_MODE  0
#define CFG_UHS2_HALF_MODE  1
#define CFG_UHS2_FAST_MODE  0
#define CFG_UHS2_LOW_POWER  1
#define CFG_UHS2_RANGE_A  0
#define CFG_UHS2_RANGE_B  1

typedef struct {
	/* Min N_LSS_SYN select. 0h: 4*16 LSS, 1h...Fh: 4*(1...15) LSS */
	u32 min_lss_syn:4;
	/* Min N_LSS_DIR select. 0h: 8*16 LSS, 1h...Fh: 8*(1...15) LSS */
	u32 min_lss_dir:4;
	/* Min N_DATA_GAP select. 00h: Reserved 01h: 1 LSS, FFh: 255 LSS */
	u32 min_data_gap_sel:8;
	/* Max N_FCU select. 00h: 256 blocks, FFh: 255 blocks */
	u32 max_nfcn_sel:8;
	/*
	 * Use the registry value or use the driver to decide the host capability of the NFCU value.
	 * 0 means to use the driver, 1 means to use the registry value
	 */
	u32 reserve_nfcu:1;
	/* N_LSS_SYN/N_LSS_DIR/N_DATA_GAP setting mode, 0: Spec mode, 1: Registry priority mode */
	u32 reserve_syn_dir_gap:1;
	/* 0x9ch[15]:Half/Full Selection For DCMD: 0: Full/ 1:Half */
	u32 half_full_sel:1;
	/* 0x140h[0]:Power Mode:  0: Fast Mode/ 1: Low Power Mode */
	u32 fast_low_pwr_sel:1;
	/* 0x144h[7:6]: Max Speed Range:  0:Rang A/ 1:Rang B */
	u32 max_speed_range_sel:1;
	u32 l1_requirement_source:2;
	/* uhs2 setting valid, 0: invalid   1:valid */
	u32 reserve:1;
} cfg_uhs2_setting_t;

typedef struct {
	/*
	 * power off vdd1 from UHSII to UHSI switch.
	 * 1 means enable this function, 0 means disable this function.
	 */
	u32 enable_power_off_vdd1:1;
	/*
	 * The driver will use fullreset reinit instead power cycle reinit when
	 * Init UHS2 first time failed. 1:enable, 0:disable
	 */
	u32 enable_full_reset_reinit:1;
	/* Enable disable internal clock when go dormant  0:Disable 1:Enable */
	u32 enable_internal_clk_dormant:1;
	/* 1: Disable, 0: Enable */
	u32 disable_scramb_mode:1;
	u32 reserved_1:27;
	/* UHS2 setting valid, 0: invalid   1:valid */
	u32 reserve:1;
} cfg_uhs2_setting2_t;

#define CFG_UHS2_TEST_BLOCK  0
#define CFG_UHS2_TEST_BYTE    1
typedef struct {
	/* PIO & DMA DCMD test option */
	u32 data_dcmd_test_option:1;
	/* Byte or block mode, 0h: Block mode, 1h: Byte mode */
	u32 byte_block_sel:1;
	/* UHS2 initialize loop test, 0h: Disable, 1h: Enable */
	u32 enable_uhs2_init_loop:1;
	/* UHS2 mode auto test, 0h: Disable, 1h: Enable */
	u32 enable_uhs2_auto_test:1;
	/* Dormant stress test, 0: Disable, 1: Enable */
	u32 enable_dormant_test:1;
	/* enable GPIO test for UHS2 (PCR 0xd0 0xd4)   0: disable   1: enable */
	u32 enable_gpio_test:1;
	u32 reserved_1:25;
	/* enable UHS2 card test, 0: disable   1: enable */
	u32 enable_uhs2_card_test:1;
} cfg_uhs2_card_test_item_t;

#define CFG_DCM_LOW_MODE 0
#define CFG_DCM_HIGH_MODE 1

typedef struct {
	/* DM/DN/Divider settings in registry 0: Low mode, >0: High mode */
	u32 value:1;
	u32 reserved_1:30;
	/* set DM/DN for FPGA chip valid, 0: invalid  1: valid */
	u32 reserve:1;
} cfg_fpga_dcm_mode_t;

typedef struct {
	u32 value:31;
	u32 enable_set:1;
} cfg_clk_driver_strength_33v_t;

typedef struct {
	u32 value:31;
	u32 enable_set:1;
} cfg_data_driver_strength_33v_t;

typedef struct {
	u32 value:31;
	u32 enable_set:1;
} cfg_data_driver_strength_18v_t;

typedef struct {
	u32 value:31;
	u32 enable_set:1;
} cfg_clk_driver_strength_18v_t;

typedef struct {
	/* auto power off time. unit second (Default: 10) */
	u32 time_s:31;
	/* Enable <Auto Power OFF> function, 0: Disable, 1: Enable (Default: 1) */
	u32 reserve:1;
} cfg_auto_poweroff_time_t;

typedef struct {
	/* The time to enter sleep after the card power off */
	u32 time_ms:8;
	u32 reserved_1:23;
	/* 0: Disable 1: Enable */
	u32 enable_sleep_func:1;
} cfg_auto_sleep_t;

typedef struct {
	/*
	 * The time interval from read/write request complete
	 * to stop the infinite transfer. Unit: ms
	 */
	u32 time_ms:31;
	/* enable/disable stop infinite transfer timer 0: disable 1: enable */
	u32 enable:1;
} cfg_stop_inf_timer_t;

typedef struct {
	/*
	 * The time to send the go dormant command after R/W SRB finished,
	 * The unit is millisecond.
	 */
	u32 time_ms:30;
	/* 1: means to use hibernate command, 0 means to use the dormant command. */
	u32 enable_hbr:1;
	/* 0: Disable 1: Enable */
	u32 enable_dmt_func:1;
} cfg_auto_uhs2dmt_timer_t;

typedef struct {
	/*
	 * the delay time before check data[0] line state after
	 * inserting card in the card initialization (unit: 100us)
	 */
	u32 delay_us:8;
	u32 reserved_1:23;
	/*
	 * enable check data[0] line state at the beginning of inserting card
	 * 0: disable   1: enable
	 */
	u32 enable_set:1;
} cfg_check_data0_line_t;

typedef struct {
	u32 reserved_1:31;
	/*
	 * enable/disable the function to
	 * backup/restore PCI bug fix register values.
	 * 1: enable, 0: disable
	 */
	u32 enable_save_pci_register:1;
} cfg_pci_bus_driver_bug_fix_t;

typedef struct {
	/*
	 * part A registers values backup and restore enable bit at D3/resume.
	 * 1:enable the function,  0 : disable
	 */
	u32 enable_bak_parta:1;
	/*
	 * part B registers values backup and restore enable bit at D3/resume..
	 * 1:enable the function  0 : disable
	 */
	u32 enable_bak_partb:1;
	/*
	 * Enable/disable the registry to
	 * backup/restore for PCI bug fix/PartA/PartB.
	 * 1: enable, 0: disable
	 */
	u32 enable_registry_bak_setting:1;
	u32 reserved_1:28;
	/* part a part b recover setting valid, 0: invalid  1: valid */
	u32 reserve:1;
} cfg_pci_register_backup_t;

typedef struct {
	/* max snoop latency value equals PCR 0x234[9:0] */
	u32 max_snoop_val:10;
	/* max snoop latency scale value equals PCR 0x234[12:10] */
	u32 max_snoop_scale:3;
	u32 reserved_1:3;
	/* max no-snoop latency value equals PCR 0x234[25:16] */
	u32 max_no_snoop_val:10;
	/* max no-snoop latency scale value equals PCR 0x234[28:26] */
	u32 max_no_snoop_scale:3;
	u32 reserved_2:3;
} cfg_bios_snoop_t;

#define CFG_ENABLE_PCI_L12    1
#define CFG_ENABLE_PCI_L11    2
#define CFG_ENABLE_ASPM_L12 4
#define CFG_ENABLE_ASPM_L11 8
typedef struct {
	/*
	 * The value equals PCR 0x248[3:0] (ASPM L1.1:0x248[3]
	 * ASPM L1.2: 0x248[2]  PCI-PM L1.1:0x248[1]  PCI-PM L1.2: 0x248[0])
	 */
	u32 pm_mode_sel:4;
	/* The value equals the PCR 0x248[25:16]. The LTR L1.2 threshold value */
	u32 l12_threshold_val:10;
	/* The value equals the PCR 0x248[31:29]. The LTR L1.2 threshold scale */
	u32 l12_threshold_scale:3;
	/*
	 * value equals PCR 0x3E8[31]: nonsnoop latency
	 * requirement bit for slow LTR
	 * (1. requirement 0: no requirement)
	 */
	u32 enable_req_slow_ltr:1;
	/*
	 * value equals PCR 0x3Ec[31]: nonsnoop latency
	 * requirement bit for fast LTR
	 * (1. requirement 0: no requirement)
	 */
	u32 enable_req_fast_ltr:1;
	/*
	 * Enable/disable the function to set the pm_mode_sel value to PCR 0x248.
	 * (0 means disable, 1 means enable)
	 */
	u32 enable_pm_mode_sel:1;
	u32 reversed_1:11;
	/* value equals bit[28] of PCR 0x3E0,disable or enable L1 substate */
	u32 dis_l1_substate:1;
} cfg_bios_l1_substate_t;

typedef struct {
	u32 pcr_ctx_en:1;
	u32 new_flow_en:1;
	u32 reversed:28;
	u32 sw_en:1;
	u32 hw_sw_control_sel:1;
} cfg_d3silence_t;

typedef struct {
	/* The value equals the PCR 0x74[7:0]: EP_NFTS_Value, NFTS value, default: 8'h10 */
	u32 ep_nfts_value:8;
	/* The value equals PCR 0xf4[21:19], the ASPM L0s Exit Latency. The default value is 3. */
	u32 aspm_l0s_exit_latency:3;
	/* The value equals PCR A8h[10],enable or disable LTR mechanism. */
	u32 enable_ltr_mechanism:1;
	/*
	 * The value equals PCR 0xdc[13], enable or disable LED output or not.
	 * 1: Turn off MMI LED  0: Turn on
	 */
	u32 dis_mmi_led_pwroff:1;
	/* disable RTD3 function, the value equals PCR 0x3e0[29] (1'b1: disable   1'b0: enable) */
	u32 dis_rtd3:1;
	/*
	 * value equals PCR 0xf0[0]. Insert card and power on, it is HPA mode,
	 * pull out card, it is LPB mode, default 1'b1
	 */
	u32 aspml1_mode_hpab:1;
	/*
	 * The value will be set to the PCR 0xf0[5] in FJ2/Seabird and
	 * PCR 0xf0[1] in SeaEagle A0/A1.
	 */
	u32 aspml1_mode_lpb:1;
	/* value equals PCR 0x90[8], enable clock power management. */
	u32 enable_clock_power_mng:1;
	/* The value equals PCR 0x90[1:0], enable or disable ASPM L0s&L1. */
	u32 enable_aspm_mode:2;
	u32 reserved_1:12;
	/*
	 * Enable Use the driver to set the class A of the BIOS setting registers.
	 * 1 means use the driver, 0 means don't use.
	 */
	u32 enable_bios_part_a:1;
	cfg_bios_snoop_t bios_snoop;
} cfg_bios_part_a_t;

typedef struct {
	/*
	 * The value equals the PCR 0xe0[31:28]: PCI-PM L1 Entrance Timer
	 * (0: 2us    1: 4us    2: 8us)
	 */
	u32 enter_pcil1_delay_us:4;
	/*
	 * The value equals the PCR 0xfc[19:16]: ASPM L1 Entrance Timer
	 * (0: 2us    1: 4us    2: 8us)
	 */
	u32 enter_aspml1_delay_us:4;
	/*
	 * disable use the default max payload size
	 * 0: use default max payload size (128bytes)
	 * 1: use the function to get the max payload size
	 */
	u32 dis_default_payload:1;
	u32 reserved_1:22;
	/*
	 * Use the driver to set the class B of the BIOS setting registers.
	 * 1 means use the driver, 0 means don't use.
	 */
	u32 enable_set_part_b:1;
} cfg_bios_part_b_t;

typedef struct {
	/* equals PCR 0xf0[9:8] (2'b00: 1s; 2'b01: 5s; 2'b10: 10s; 2'b11: 60s) */
	u32 aspm_int_timer_s:2;
	/*
	 * enable or disable the interrupt timer for the scsiport driver. (PCR 0xf0[13]
	 * 1:enable the function, 0: disable the function)
	 */
	u32 enable_int_active:1;
	u32 reserved_1:28;
	/* enable or disable to set interrupt active function */
	u32 enable_set:1;
} cfg_interrupt_timer_t;

typedef struct {
	u32 reserved_1:31;
	/* Whether the device is FPGA. 1 means FPGA, 0 means ASIC. */
	u32 is_fpga_chip:1;
} cfg_device_type_ctrl_t;

typedef struct {
	/*
	 * check OCB status in the interrupt work flow
	 * 0: disable check ocb in the interrupt work flow
	 * 1: enable in the interrupt check
	 */
	u32 int_check_en:1;
	u32 reserved_1:30;
	/*
	 * Enable software power off FET/LDO for OCB
	 * 0: Hardware power off FET/LDO  1: Software power off FET/LDO
	 */
	u32 sw_pwroff_en:1;

} cfg_ocb_ctrl_t;

#define CFG_POLARITY_PIN_LOW   0
#define CFG_POLARITY_PIN_HIGH  1
typedef struct {
	/* polarity control pin, 1: high   0: low */
	u32 polarity_ctrl_pin:1;
	u32 reserved_1:30;
	/*
	 * enable control polarity control pin, It is only valid with the FJ2 chip.
	 * 0: disable     1: enable
	 */
	u32 enable_ctrl_polarity_pin:1;
} cfg_polarity_ctrl_t;

typedef struct {
	u32 reversed_1:31;
	/*
	 * 1: enable LTR patch. 0:disable LTR patch.
	 * if LTR patch enable, it only affect  FJ2 & SeaBird chip.
	 */
	u32 enable_ltr_patch:1;
} cfg_ltr_c10_patch_t;

typedef struct {
	/* The delay for the power off. The unit is ms. */
	u32 power_off_wait_ms:16;
	/* The delay for the power on. The unit is ms. */
	u32 power_on_wait_ms:16;
} cfg_power_wait_time_t;

#define CFG_HW_CTRL_RTD3	0
#define CFG_SFT_CTRL_RTD3	1

typedef struct {
	/* system enter D3 time, the unit is 1 second. */
	u32 adapter_idle_time_s:8;
	/* driver enter request D3 function time, the unit is second */
	u32 disk_idle_time_s:8;
	/* D3 work mode select (PCR0x3f0[29:28]) */
	u32 reserve:2;
	/* enable/disable RTD3 function (software). 0:disable, 1:Enable. */
	u32 enable_rtd3:1;
	/* enable D3Silence function  0:disable    1: enable */
	u32 reserve_1:1;
	/* select D3 silence sub mode   0:sub mode 1    1: sub mode 2 */
	u32 d3silence_submode_sel:1;
	u32 reserved_1:10;
	/*
	 * hardware/software select bit.
	 * 1: use the registry to control RTD3 disable/enable, PSD_MODE[18].
	 * 0: use the hardware register to control RTD3 disable/enable,  PCR 0x3e0[29]
	 */
	u32 rtd3_ctrl_mode:1;

} cfg_psd_mode_t;
#define CFG_THERMAL_SENSOR_HOT    0
#define CFG_THERMAL_SENSOR_COOL  1
#define CFG_THERMAL_USE_I2C           1
#define CFG_THERMAL_USE_GPIO         0

typedef struct {
	/*
	 * [0]When Driver receives S3 entry message,
	 * Driver can disable Card insert/remove source bit (0x468 [20] = 0)
	 */
	u32 s3_disable_wakeup:1;
	/*
	 * [1]When Driver receives S4 entry message,
	 * Driver can disable Card insert/remove source bit (0x468 [20] = 0)
	 */
	u32 s4_disable_wakeup:1;
	/*
	 * [2]When Driver receives S5 entry message,
	 * Driver can disable Card insert/remove source bit (0x468 [20] = 0)
	 */
	u32 s5_disable_wakeup:1;
	/* [31:3] reserved */
	u32 reserve:29;
} cfg_pcie_wake_setting_t;

typedef struct {
	/*
	 * 0: GPIO High means Thermal Sensor Hot state, GPIO Low means Thermal Sensor Cool state
	 * 1: GPIO High means Thermal Sensor Cool state, GPIO Low means Thermal Sensor Hot state
	 */
	u32 gpio_level:1;
	/*
	 * The GPIO/I2C selection for thermal control function.
	 * 1 means to use I2C, 0 means to use GPIO.
	 */
	u32 gpio_i2c_sel:1;
	u32 reserved_1:29;
	/* Enable the thermal control function. 1 means enable, 0 means disable. */
	u32 enable_thermal_func:1;
} cfg_thermal_control_en_t;

typedef struct {
	u32 reserved_1:31;
	/* enable/disable timer for thermal check  0: disable 1: enable */
	u32 enable_check_timer:1;
} cfg_thermal_timer_ctrl_t;

typedef struct {
	/* the timer value for the Thermal Check (GPIO check), unit: ms */
	u32 timer_ms;
} cfg_thermal_control_timer_t;

typedef struct {
	/* Enable UHS2 Range Mode when Do thermal control. 0:disable(default) 1:enable */
	u32 enable_uhs2_range_mode:1;
	/* Enable UHS2 Duplex Mode switch when do thermal control. 0:disable(default) 1:enable */
	u32 enable_uhs2_duplex_mode:1;
	/* Enable UHS2 Power Mode switch when do thermal control. 0:disable(default) 1:enable */
	u32 enable_uhs2_power_mode:1;
	u32 reserved_1:28;
	/*
	 * Enable Legacy SD card Access Mode switch when
	 * do thermal control 0:disable(default) 1:enable
	 */
	u32 enable_legacy_access_mode:1;
} cfg_thermal_control_ctrl_t;

typedef struct {
	/* the upper limitation temperature for I2C thermal control function */
	u32 value:16;
	u32 reserved_1:15;
	/*
	 * set the upper limitation temperature for I2C thermal control function  valid
	 * 0: use the default mode      1: set the upper limitation temperature
	 */
	u32 reserve:1;
} cfg_thermal_temperature_t;

typedef struct {
	/*
	 * main ldo setting for uhs2,
	 * if enable then write [1:0] to 0x68[11:10] for uhs2 when power on
	 */
	u32 value:2;
	u32 reserved_1:29;
	/* enable bit */
	u32 enable:1;
} cfg_ldo_t;

typedef struct {
	/*
	 * if failsafe patch enable,it control Delay time before
	 * software-reset-all in failsafe patch function. Unit ms.
	 */
	u32 soft_reset_delay_ms:30;
	/* enable delay time before software-reset-all in failsafe patch  0:disable  1:enable */
	u32 enable_soft_reset_delay:1;
	/*
	 * 1: means enable software reset all & fail safe patch.
	 * it only affect SeaEagle chip.0:mean disable this patch
	 */
	u32 reserve:1;
} cfg_sw_reset_all_cfg_t;

typedef struct {
	/*
	 * if fails safe patch enable,
	 * it control delay time after do disable failsafe action(ms).
	 */
	u32 dis_failsafe_delay_ms:4;
	u32 reserved_1:27;
	/*
	 * if fail safe patch enable, enable delay time after disable failsafe action.
	 * 0: disable  1:enable
	 */
	u32 enable_aft_disable_failsafe_delay:1;
} cfg_disable_failsafe_delay_ctrl_t;

typedef struct {
	u32 value:16;
	u32 reserved_1:15;
	/* enable set the card initialize (ACMD41) ready timeout. Default: 1500ms */
	u32 reserve:1;

} cfg_timeout_t;

typedef struct {
	/* The debounce count value, it will be set to the PCR 0x324[30:0]. */
	u32 value:31;
	/*
	 * enable/disable the function to modify the debounce count.
	 * 0 means disable, 1 means enable.
	 */
	u32 enable_deb_count:1;
} cfg_cd_debounce_count_t;

/* ----------------------- Registry classify -------------------- */

typedef struct {
	/*
	 * Maximum Access mode select, 0h: SDR12, 1h: SDR25,
	 * 2h: SDR50, 3h: SDR104, 4h: DDR50 5h: DDR200 Default is 3h.
	 */
	u32 value:8;
	u32 reserved_1:23;
	/* Maximum access mode valid, 0: invalid   1: valid */
	u32 reserve:1;
} cfg_test_max_access_mode_t;

typedef struct {
	/*
	 * Maximum Access mode select, 0h: SDR12, 1h: SDR25, 2h: SDR50,
	 * 3h: SDR104, 4h: DDR50 5h: DDR200 Default is 3h.
	 */
	u32 value:8;
	u32 reserved_1:23;
	/* Maximum access mode valid, 0: invalid   1: valid */
	u32 reserve:1;
} cfg_max_power_limit_t;

typedef struct {
	cfg_uhs2_setting_t uhs2_setting;
	cfg_uhs2_setting2_t test_uhs2_setting2;
} cfg_uhs2_card_item_t;

typedef struct {
	/* 0: internal, 1: external */
	u32 vdd1_power_source:1;
	/* 0: 1.2V, 1: 1.8V, 2: 3.3V */
	u32 vdd1_voltage:2;
	/* 0: active low, 1: active high */
	u32 vdd1_onoff_polarity:1;
	u32 vdd1_reserved:4;
	/* 0: internal, 1: external */
	u32 vdd2_power_source:1;
	/* 0: 1.2V, 1: 1.8V, 2: 3.3V */
	u32 vdd2_voltage:2;
	/* 0: active low, 1: active high */
	u32 vdd2_onoff_polarity:1;
	u32 vdd2_use_gpio1:1;
	u32 vdd2_reserved:3;
	/* 0: internal, 1: external */
	u32 vdd3_power_source:1;
	/* 0: 1.2V, 1: 1.8V, 2: 3.3V */
	u32 vdd3_voltage:2;
	/* 0: active low, 1: active high */
	u32 vdd3_onoff_polarity:1;
	u32 vdd3_reserved:4;
	u32 reserved:8;
} cfg_vdd_power_source_item_t;
/* -------------------- vdd_power_source item end ----------------------// */

#define INIT_DELAY 0x00000000
#define INIT_DELAY_EN_MASK 0x000000001
#define INIT_DELAY_CFG_MASK 0xFFFFFFFE

typedef struct {
	cfg_sd_card_mode_dis_t sd_card_mode_dis;
	cfg_test_max_access_mode_t test_max_access_mode;
	cfg_driver_strength_sel_t test_driver_strength_sel;
	cfg_max_power_limit_t test_max_power_limit;

	cfg_mmc_mode_dis_t mmc_mode_dis;
	cfg_emmc_mode_t emmc_mode;
	cfg_sd7_sdmode_switch_control_t sd7_sdmode_switch_control;

	cfg_uhs2_setting_t uhs2_setting;
	cfg_uhs2_setting2_t test_uhs2_setting2;

} cfg_card_item_t;
/* -------------------- card item end ---------------------- */

/* 1 hw tuning0 sw tuning */
#define TUNING_MODE 0x1

typedef struct {
	u32 start:16, end:16;
} cfg_freq_item_t;

/* [31:16]	degrade frequency index from the frequency table */
#define FREQ_DEGRE_INDEX_MASK   0xFFFF0000
/* [15:0]	start index from the frequency table */
#define FREQ_START_INDEX_MASK   0x0000FFFF
#define FREQ_UHS2M              0x00030000
#define FREQ_200M               0x00070004
#define FREQ_100M               0x000B0008
#define FREQ_DDR50M             0x000C000C
#define FREQ_DDR50_INPUT_TUNIN  0x00110011
#define FREQ_75M                0x000D000D
#define FREQ_50M                0x000E000E
#define FREQ_25M                0x000F000F
#define FREQ_400K               0x00100010
#define FREQ_EMMC_200M          0x00150012
#define FREQ_EMMC_DDR50M        0x00160016
#define FREQ_EMMC_50M           0x00170017
#define FREQ_EMMC_25M           0x00180018
#define FREQ_EMMC_400K          0x00190019
#define FREQ_DDR200M            0x001D001A
#define FREQ_DDR225M            0x0021001E

#define FREQ_UHS2M_DEGRE_INDEX ((FREQ_UHS2M & FREQ_DEGRE_INDEX_MASK) >>  16)
#define FREQ_UHS2M_START_INDEX (FREQ_UHS2M & FREQ_START_INDEX_MASK)

#define FREQ_200M_DEGRE_INDEX ((FREQ_200M & FREQ_DEGRE_INDEX_MASK) >> 16)
#define FREQ_200M_START_INDEX (FREQ_200M & FREQ_START_INDEX_MASK)

#define FREQ_100M_DEGRE_INDEX ((FREQ_100M & FREQ_DEGRE_INDEX_MASK) >> 16)
#define FREQ_100M_START_INDEX (FREQ_100M & FREQ_START_INDEX_MASK)

#define FREQ_DDR50M_DEGRE_INDEX ((FREQ_DDR50M & FREQ_DEGRE_INDEX_MASK) >> 16)
#define FREQ_DDR50M_START_INDEX (FREQ_DDR50M & FREQ_START_INDEX_MASK)

#define FREQ_DDR50_INPUT_TUNING_DEGRE_INDEX \
	((FREQ_DDR50_INPUT_TUNIN & FREQ_DEGRE_INDEX_MASK) >> 16)
#define FREQ_DDR50_INPUT_TUNIN_START_INDEX \
	(FREQ_DDR50_INPUT_TUNIN & FREQ_START_INDEX_MASK)

#define FREQ_75M_DEGRE_INDEX ((FREQ_75M & FREQ_DEGRE_INDEX_MASK) >> 16)
#define FREQ_75M_START_INDEX (FREQ_75M & FREQ_START_INDEX_MASK)

#define FREQ_50M_DEGRE_INDEX ((FREQ_50M & FREQ_DEGRE_INDEX_MASK) >> 16)
#define FREQ_50M_START_INDEX (FREQ_50M & FREQ_START_INDEX_MASK)

#define FREQ_25M_DEGRE_INDEX ((FREQ_25M & FREQ_DEGRE_INDEX_MASK) >> 16)
#define FREQ_25M_START_INDEX (FREQ_25M & FREQ_START_INDEX_MASK)

#define FREQ_400K_DEGRE_INDEX ((FREQ_400K & FREQ_DEGRE_INDEX_MASK) >> 16)
#define FREQ_400K_START_INDEX (FREQ_400K & FREQ_START_INDEX_MASK)

#define FREQ_EMMC_200M_DEGRE_INDEX ((FREQ_EMMC_200M & FREQ_DEGRE_INDEX_MASK) >> 16)
#define FREQ_EMMC_200M_START_INDEX (FREQ_EMMC_200M & FREQ_START_INDEX_MASK)

#define FREQ_EMMC_DDR50M_DEGRE_INDEX ((FREQ_EMMC_DDR50M & FREQ_DEGRE_INDEX_MASK) >> 16)
#define FREQ_EMMC_DDR50M_START_INDEX (FREQ_EMMC_DDR50M & FREQ_START_INDEX_MASK)

#define FREQ_EMMC_50M_DEGRE_INDEX ((FREQ_EMMC_50M & FREQ_DEGRE_INDEX_MASK) >> 16)
#define FREQ_EMMC_50M_START_INDEX (FREQ_EMMC_50M & FREQ_START_INDEX_MASK)

#define FREQ_EMMC_25M_DEGRE_INDEX ((FREQ_EMMC_25M & FREQ_DEGRE_INDEX_MASK) >> 16)
#define FREQ_EMMC_25M_START_INDEX (FREQ_EMMC_25M & FREQ_START_INDEX_MASK)

#define FREQ_EMMC_400K_DEGRE_INDEX ((FREQ_EMMC_400K & FREQ_DEGRE_INDEX_MASK) >> 16)
#define FREQ_EMMC_400K_START_INDEX (FREQ_EMMC_400K & FREQ_START_INDEX_MASK)

#define FREQ_DDR200M_DEGRE_INDEX ((FREQ_DDR200M & FREQ_DEGRE_INDEX_MASK) >> 16)
#define FREQ_DDR200M_START_INDEX (FREQ_DDR200M & FREQ_START_INDEX_MASK)

#define FREQ_DDR225M_DEGRE_INDEX ((FREQ_DDR225M & FREQ_DEGRE_INDEX_MASK) >> 16)
#define FREQ_DDR225M_START_INDEX (FREQ_DDR225M & FREQ_START_INDEX_MASK)
typedef struct {
	/* setting clkreqn timeout value, unit : ms, (default: 5ms which is useful when[31] = 0 */
	u32 clkreqn_timeout_value:16;
	/* Reserved */
	u32 reserved:15;
	/*
	 * enable set clkreqn timeout,
	 * 0: use clkreqn timeout default value(1ms)
	 * 1 : enable set the clkreqn timeout(default:1)
	 */
	u32 enable_clkreqn_timeout:1;
} cfg_sd7_clkreqn_status_detect_timeout_t;

typedef struct {
	/* [0]     Reserved */
	u32 reserve_1:1;
	/*
	 * [3:1]
	 * SD 1.8v data / cmd driving strength when select PCIe external control drive strength
	 */
	u32 cmd_driver_strength_1_8v:3;
	/*
	 * [6:4]
	 * SD 1.8v clk driving strength when select PCIe external control drive strength
	 */
	u32 clk_driver_strength_1_8v:3;
	/* [7]        Reserved */
	u32 reserve_2:1;
	/*
	 * [10:8]
	 * SD 3.3v data / cmd driving strength when select PCIe external control drive strength
	 */
	u32 data_cmd_driver_strength_3_3v:3;
	/* [11]       Reserved */
	u32 reserve_3:1;
	/* [14:12] SD 3.3v clk driving strength when select PCIe external control drive strength */
	u32 clk_driver_strength_3_3v:3;
	/* [30:15] Reserved */
	u32 reserve_4:16;
	/*
	 * [31] ds_selection_enable(default:0)
	 * 1 : driver strength method select enable 0 : driver strength method select enable
	 */
	u32 ds_selection_enable:1;
} cfg_host_drive_strength_item_t;

typedef struct {
	cfg_dma_mode_setting_t test_dma_mode_setting;
	cfg_infinite_transfer_mode_t test_infinite_transfer_mode;
	cfg_sdma_boundary_t test_sdma_boundary;
	cfg_tag_queue_capability_t test_tag_queue_capability;
	cfg_ocb_ctrl_t test_ocb_ctrl;
	cfg_bios_l1_substate_t bios_l1_substate;
	cfg_vdd_power_source_item_t vdd_power_source_item;

	cfg_host_drive_strength_item_t host_drive_strength;
} cfg_host_item_t;
/* -------------------- host item end ---------------------- */

typedef struct {
	/* SW cfg */
	u32 value:1;
	u32 reserved_1:29;
	/* 1: sw, 0: hw */
	u32 sw_hw_select:1;
	/* SW enable bit, or vendor PCR control */
	u32 enable:1;
} cfg_sw_write_protect_item_t;

typedef struct {
	/* 1: enable, 0: disable */
	u32 rc_tx_vcme:1;
	/* 1: enable, 0: disable */
	u32 sd_rx_vcme:1;
	/* 1: enable, 0: disable */
	u32 sd_tx_vcme:1;
	/* 1: enable, 0: disable */
	u32 rc_rx_vcme:1;
	u32 reserved_1:27;
	/* 0: disable, 1: enable */
	u32 enable:1;
} cfg_hsmux_vcme_enable_item_t;

typedef struct {
	/* [15:00] */
	u32 required_refclk_compare_count:16;
	/* [29:16] */
	u32 reserve:14;
	/* [30] 1: Synchronize parameter 0: Not synchronize parameter (default :0) */
	u32 chk_refclk_parameter_en:1;
	/* [31] 1:enable 0:disable */
	u32 enable:1;
} cfg_refclk_stable_detection_counter1_item_t;

typedef struct {
	/* [15:00] */
	u32 required_refclk_compare_timeout_d0l11:16;
	/* [31:16] */
	u32 required_refclk_compare_timeout_d0l10:16;
} cfg_refclk_stable_detection_counter2_item_t;

typedef struct {
	/* [15:00] */
	u32 required_refclk_compare_timeout_d3l12:16;
	/* [31:16] */
	u32 required_refclk_compare_timeout_d0l12:16;
} cfg_refclk_stable_detection_counter3_item_t;

typedef struct {
	/* [07:00]  required_refclk_count_max */
	u32 req_refclkcnt_max:8;
	/* [15:08]  required_refclk_count_min */
	u32 req_refclkcnt_min:8;
	/* [23:16]  Refclk_range_detect_cnt */
	u32 refclk_range_detect_cnt:8;
	/* [28:24]  Reserve */
	u32 reserve:5;
	/* [29]     required_refclk_count_min_max_source_selection */
	u32 req_refclkcnt_minmax_source_sel:1;
	/* [30]     refclk_cnt_range_detect_soft_reset */
	u32 refclkcnt_range_detect_softreset:1;
	/* [31]     enable bit 1: enable 0 : disable (default: 1)  */
	u32 enable:1;
} cfg_auto_detect_refclk_counter_range_ctl_item_t;

typedef struct {
	/*
	 * [0]	Disable PCIe Phy Reference clock active detection logic.
	 * 1: Disable 0 : Enable(Default)
	 */
	u32 disable_pcie_phy_clk:1;
	/*
	 * [1]	Disable tx commond mode can be off only when l1sub_state == S_L1_N
	 * 1: Disable, 0 : Enable(Default)
	 */
	u32 disable_tx_command_mode:1;
	/* [30:2] Reserve */
	u32 reserve:29;
	/* [31]       Enable bit 1: enable 0 : disable (default: 0)  */
	u32 enable:1;
} cfg_l1_enter_exit_logic_ctl_item_t;

typedef struct {
	/* [15:00] */
	u32 pcietx_amplitude_setting:16;
	/* [30:16] Reserve */
	u32 reserve:15;
	/* [31]       Enable bit 1: enable 0 : disable (default: 0)  */
	u32 pcietx_amplitude_chg_en:1;
} cfg_pcie_phy_amplitude_adjust_item_t;

typedef struct {
	cfg_psd_mode_t psd_mode;
	cfg_pcie_wake_setting_t pcie_wake_setting;
	cfg_ldo_t test_main_ldo_setting;
	cfg_output_tuning_item_t output_tuning_item;
	cfg_hsmux_vcme_enable_item_t hsmux_vcme_enable;
	 cfg_refclk_stable_detection_counter1_item_t
	    refclk_stable_detection_counter1;
	 cfg_refclk_stable_detection_counter2_item_t
	    refclk_stable_detection_counter2;
	 cfg_refclk_stable_detection_counter3_item_t
	    refclk_stable_detection_counter3;
	 cfg_auto_detect_refclk_counter_range_ctl_item_t
	    auto_detect_refclk_counter_range_ctl;
	cfg_l1_enter_exit_logic_ctl_item_t l1_enter_exit_logic_ctl;
	cfg_pcie_phy_amplitude_adjust_item_t pcie_phy_amplitude_adjust;
} cfg_feature_item_t;
/* -------------------- feature item end ---------------------- */

typedef struct {
	cfg_auto_sleep_t auto_sleep_control;
	cfg_auto_uhs2dmt_timer_t auto_dormant_timer;
} cfg_timer_item_t;
/* -------------------- timer item end ---------------------- */

#define RESUME_DALAY_US 0x00000000
#define RESUME_POWER_ON_DELAY_MS 0x00000024
#define DMT_DELAY_BEF_STOP_CLK_US 0x00000000
#define DMT_DEALY_AFT_STOP_CLK_US 0x00000014
#define DMT_DELAY_AFT_PWROFF_MS 0x00000005
#define DMT_DELAY_AFT_ST_REFCLK_US 0x00000000

typedef struct {
	cfg_power_wait_time_t power_wait_time;
	cfg_timeout_t test_write_data_timeout;
	cfg_timeout_t test_read_data_timeout;
	cfg_timeout_t test_non_data_timeout;
	cfg_timeout_t test_r1b_data_timeout;
	cfg_timeout_t test_card_init_timeout;
} cfg_timeout_item_t;

#define UHS2_NATIVE_DATA_TIMEOUT  2000

/* -------------------- time item end ---------------------- */
#define TUNING_TIMEOUT 0x80000096

typedef struct {
	cfg_device_type_ctrl_t fpga_ctrl;
} cfg_fpga_item_t;
/* -------------------- fpga item end ---------------------- */

typedef struct {
	/* [0:5] */
	u32 reserv_2:6;
	/* [6] */
	u32 dis_patch_ntfs_verify_rtd3:1;
	/* [7] */
	u32 dis_patch_rtd3_idle_ref_cnt:1;
	/* [8] */
	u32 reserve_3:1;
	/* [9:12] fix SeaEagle s3 resume no card stress test issue  */
	u32 delay_for_failsafe_s3resume:4;
	/* [13] fix SeaEagle s3 resume no card stress test issue  */
	u32 failsafe_en:1;
	/*
	 * [14] control SDR50_OPCLK to select
	 * OPECLK clock as sampling lock (sdr50_input_tuning_en=0) or
	 * DLL CLKOUTA as sampling clock (sdr50_input_tuning_en=1, default setting).
	 */
	u32 reserve_1:1;
	/* [15] 0: disable(default);  1: enable */
	u32 card_details_display_enable:1;
	/* [16:17] */
	u32 sw_ctl_led_gpio0:2;
	/* [18:19] */
	u32 led_gpio1:2;
	/* [20:21] */
	u32 led_gpio2:2;
	/* [22:23] */
	u32 wp_led_gpio3:2;
	/* [24] */
	u32 led_polarity:1;
	/* [27:25] */
	u32 reserve_4:3;
	/* [28] */
	u32 camera_mode_ctrl_vdd1_vdd2_cd:1;
	/* [31:29] */
	u32 reserve:3;
} cfg_driver_item_t;
/* -------------------- driver item end ---------------------- */
#define CARD_READER FALSE
#define REMOVABLE TRUE
#define HW_TIMER_CFG FALSE
#define REMOVABLE_PNP TRUE

/* -------------------- error recovery item end ---------------------- */

#define MAX_PCR_SETTING_SIZE 256

typedef struct {
	/* PCR address */
	u16 addr;
	/* PCR mask */
	u16 mask;
	/* PCR value */
	u16 val;
	u16 valid_flg;
	/* 0 : PCR; 1: bar0; */
	u32 type;
} cfg_pcr_t;

typedef struct {
	cfg_pcr_t pcr_tb[MAX_PCR_SETTING_SIZE];
	/* valid counter for pcr settings */
	u32 cnt;
} cfg_pcr_item_t;

typedef struct {
	u32 test_id;
	/* 0 means infinite */
	u32 test_loop;
	u32 test_param1;
	u32 test_param2;
} cfg_testcase_t;

typedef struct {
	cfg_card_item_t card_item;
	cfg_host_item_t host_item;

	cfg_feature_item_t feature_item;
	cfg_timer_item_t timer_item;
	cfg_timeout_item_t timeout_item;
	cfg_fpga_item_t fpga_item;
	cfg_driver_item_t driver_item;
	cfg_testcase_t test_item;

	/*          mem variable    */
	cfg_pcr_item_t pcr_item;
	/* Directly values: */
	u32 dmdn_tbl[MAX_FREQ_SUPP];
	bool boot_flag;

} cfg_item_t;

struct amplitude_configuration {
	u32 pcietx_amplitude;
	char amplitude[5];
};

#define HW_DETEC_HW_SWITCH 0x0
#define SW_POLL_SW_SWITCH  0x4
#define SW_POLL_SWCTRL_SWITCH 0x5
#define SW_POLL_INTER_SW_SWITCH 0x6
#define SW_POLL_INTER_SWCRTL_SWITCH 0x7

void cfgmng_init(void);
void cfg_print_debug(PVOID cfg_item);
cfg_item_t *cfgmng_get(void *pdx, e_chip_type chip_type, bool boot);

void cfg_dma_mode_dec(cfg_item_t *cfg, u32 dec_dma_mode);
void cfg_dma_addr_range_dec(cfg_item_t *cfg, u32 dma_range);
/* 0: 1bit   1: 4bit     2: 8bit */
void cfg_emmc_busw_supp(cfg_emmc_mode_t *emmc_mode, u8 bus_width);
bool cfg_dma_need_sdma_like_buffer(u32 dma_mode);

void os_load_pcr_cb(void *cfgp, u32 type, u32 idx, u32 addr, u32 value);
void os_load_dmdn_cb(void *cfgp, u32 type, u32 idx, u32 addr, u32 value);

void cfgmng_init_chipcfg(e_chip_type chip_type, cfg_item_t *cfg, bool reinit);
void cfgmng_update_dumpmode(cfg_item_t *cfg, e_chip_type chip_type);

extern cfg_item_t g_cfg[SUPPORT_CHIP_COUNT][2];

#endif
