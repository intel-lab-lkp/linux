// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: cfgmng.c
 *
 * Abstract: This source file used to mangage dynamic configuration
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

#include "../include/basic.h"
#include "../include/debug.h"
#include "../include/util.h"

#define TRUE 1
#define FALSE 0

#define DMDN_TYPE_CNT	4

/*
 *	0		1		2		3
 *	UHS2	UHS2M1	UHS2M2	UHS2M3
 *
 *	4		5		6		7
 *	SD200	SD200M1	SD200M2	SD200M3
 *
 *	8		9		10		11
 *	SD100M	SD100M1	SD100M2	SD100M3
 *
 *	12		13		14		15		16
 *	SD75M	DDR50	50MZ	25MZ	400K
 *
 *	17
 *	DDR50InputTuning
 *
 */

/* global definition about configuration structure */
cfg_item_t g_cfg[SUPPORT_CHIP_COUNT][2];

u32 g_dmdn_divider_tbl[DMDN_TYPE_CNT][MAX_FREQ_SUPP] = {
	/* SDS, Fujin2 */
	{
	 /* UHSII */
	 0x1f340002, 0x18230002, 0x181f0002, 0x181b0002,
	 /* SDR104=208M */
	 0x1f340000, 0x18230000, 0x181f0000, 0x181b0000,
	 /* 100M */
	 0x18270001, 0x18230001, 0x181F0001, 0x181B0001,
	 0x181d0001, 0x18270002, 0x18270002, 0x18270004, 0x182700FA,
	 0xFFFF0001,
	 /* 200M ~ 140M */
	 0x18270000, 0x18230000, 0x181f0000, 0x181b0000,
	 /* 50M  25M  400K */
	 0x18270002, 0x18270002, 0x18270004, 0x182700fa },

	/* Seabird, SeaEagle */
	{
	 /* UHSII */
	 0x2c280002, 0x27140002, 0x2B1C0002, 0x2C1A0002,
	 /* SDR104=200M */
	 0x2c280000, 0x27140000, 0x2B1C0000, 0x2c1A0000,
	 /* 100M */
	 0x25100001, 0x27140001, 0x2B1C0001, 0x2C1A0001,
	 0x250C0001, 0x25100002, 0x25100002, 0x25100004, 0x251000FA,
	 0x35100001,
	 /* 200M ~ 140M */
	 0x25100000, 0x27140000, 0x2b1c0000, 0x2c1a0000,
	 /* 50M  25M  400K */
	 0x25100002, 0x25100002, 0x25100004, 0x251000fa },

	/* SeaEagle2 */
	{
	 /* UHSII */
	 0x2c280002, 0x27140002, 0x2B1C0002, 0x2c1a0002,
	 /* SDR104=208M */
	 0x2c280000, 0x27140000, 0x2B1C0000, 0x2c1a0000,
	 /* 100M */
	 0x25100001, 0x27140001, 0x2b1c0001, 0x2c1a0001,
	 0x250c0001, 0x25100002, 0x25100002, 0x25100004, 0x251000fa,
	 0x35100001,
	 /* 200M ~ 140M */
	 0x25100000, 0x27140000, 0x2b1c0000, 0x2c1a0000,
	 /* 50M  25M  400K */
	 0x25100002, 0x25100002, 0x25100004, 0x251000fa },
	/* GG8 */
	{
	 /* UHSII */
	 0x2c500002, 0x251D0002, 0x251A0002, 0x25160002,
	 /* SDR104=208M */
	 0x2c500000, 0x251D0000, 0x251A0000, 0x25160000,
	 /* 100M */
	 0x25200001, 0x251D0001, 0x251A0001, 0x25160001,
	 0x25160001, 0x25200002, 0x25200002, 0x25200004, 0x252000fa,
	 0x25200002,
	 /* 200M ~ 140M */
	 0x25200000, 0x251D0000, 0x251A0001, 0x25160000,
	 /* 50M  25M  400K */
	 0x25200002, 0x25200002, 0x25200004, 0x252000fa,
	 /* DDR200 */
	 0x25200000, 0x251D0000, 0x251A0000, 0x25160000,
	 /* DDR225, 225MHz, 200MHz */
	 0x25240000, 0x25200000, 0x251D0000, 0x251A0000 }
};

#if (0)
/*
 *	UHS2  UHS2M1 UHS2M2 UHS2M3
 *	SD200 SD200M1 SD200M2 SD200M3
 *	SD100M SD100M1 SD100M2 SD100M3
 *	SD75M DDR50 50MZ 25MZ 400K
 */
u32 g_dmdn_divider_tbl_fpga[MAX_FREQ_SUPP] = {
	/* 208/4     187.5/4     166.7/4      145.8/4   UHSII */
	0x05030002, 0x06040002, 0x08060002, 0x07060002,
	/* 200/2     180/2       160.7/2      140.6/2   SDR104 */
	0x08050001, 0x0A070001, 0x09070001, 0x09080001,
	/* 100       90          80.4         70.3      100M */
	0x08050001, 0x0A070001, 0x09070001, 0x09080001,
	/* 75        50          50           25        400K */
	0x06050001, 0x08050002, 0x08050002, 0x08050004, 0x080500FA
};
#endif

/*
 *	UHS2  UHS2M1 UHS2M2 UHS2M3
 *	SD200 SD200M1 SD200M2 SD200M3
 *	SD100M SD100M1 SD100M2 SD100M3
 *	SD75M DDR50 50MZ 25MZ 400K
 */
u32 g_dmdn_divider_tbl_fpga[MAX_FREQ_SUPP] = {
	/* 208/4     187.5/4     166.7/4      145.8/4   UHSII */
	0x05030002, 0x05030002, 0x05030002, 0x05030002,
	/* 200/2     180/2       160.7/2      140.6/2   SDR104 */
	0x08050001, 0x08050002, 0x08050003, 0x08050001,
	/* 100       90          80.4         70.3      100M */
	0x08050001, 0x08050001, 0x08050001, 0x08050001,
	/* 75        50          50           25        400K */
	0x08050002, 0x08050002, 0x08050002, 0x08050004, 0x080500FA,
	/* DDR50 Inputtuning */
	0x080a0001,
	/* use for eMMC */
	0x08050001, 0x08050001, 0x08050001, 0x08050001, 0x08050002, 0x08050002,
	0x08050004, 0x080500FA,
	/* 200/2     180/2       160.7/2      140.6/2   DDR200 */
	0x08050001, 0x08050001, 0x08050001, 0x08050001
};

static void cfg_parse(cfg_item_t *cfg, e_chip_type chip_type);
static void cfg_set_default_val(cfg_item_t *cfg, e_chip_type type);

void cfgmng_init_chipcfg(e_chip_type chip_type, cfg_item_t *cfg, bool reinit)
{
	cfg_driver_item_t driver_item;
	cfg_psd_mode_t psd_mode;
	u32 bit64 = 0;

	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, NOT_TO_RAM,
		"========================================\n");
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, NOT_TO_RAM,
		"ChipType=%d, boot_flag = %d\n", chip_type, cfg->boot_flag);

	if (reinit) {
		driver_item = cfg->driver_item;
		psd_mode = cfg->feature_item.psd_mode;
		bit64 =
		    cfg->host_item.test_dma_mode_setting.enable_dma_64bit_address;
	}
	cfg_set_default_val(cfg, chip_type);
	os_cfg_load(cfg, chip_type);
	cfg_parse(cfg, chip_type);
	if (reinit) {
		cfg->driver_item = driver_item;
		cfg->feature_item.psd_mode = psd_mode;
		cfg->host_item.test_dma_mode_setting.enable_dma_64bit_address =
		    bit64 ? 1 : 0;
	}
}

/*
 *
 * Function Name: cfgmng_init
 *
 * Abstract:
 *			 1. Read different chip type registry information
 *             2. parse reigstry information
 *
 * Input:
 *            None
 *
 * Output:
 *			 None
 *
 * Return value:
 *            None
 *
 * Notes:
 *           Caller: DriverEntry
 */
void cfgmng_init(void)
{
	u8 i = 0;
	e_chip_type chip_type;

	for (i = 0; i < SUPPORT_CHIP_COUNT; i++) {
		/* for non-boot cfg */

		cfg_item_t *cfg = &g_cfg[i][0];

		chip_type = (e_chip_type) i;
		cfg->boot_flag = FALSE;
		cfgmng_init_chipcfg(chip_type, cfg, FALSE);

		/* for boot config */
		cfg = &g_cfg[i][1];
		chip_type = (e_chip_type) i;
		cfg->boot_flag = TRUE;

		cfgmng_init_chipcfg(chip_type, cfg, FALSE);

	}
}

/*
 *
 * Function Name: cfgmng_get
 *
 * Abstract:
 *			 1. transfer the (cfg_item_t *) structure
 *
 * Input:
 *           e_chip_type chip_type;
 *
 * Output:
 *			pointer to cfg_item_t
 *
 * Return value:
 *           None
 *
 * Notes:
 *           Caller:
 */
cfg_item_t *cfgmng_get(void *pdx, e_chip_type chip_type, bool boot)
{
	int i = boot ? 1 : 0;
	cfg_item_t *cfg = &g_cfg[(u32) chip_type][i];
	u32 *tbl = NULL;

	switch ((u32) chip_type) {
	case CHIP_SDS0:
	case CHIP_SDS1:
	case CHIP_FUJIN2:
		tbl = &g_dmdn_divider_tbl[0][0];
		break;

	case CHIP_SEABIRD:
	case CHIP_SEAEAGLE:
		tbl = &g_dmdn_divider_tbl[1][0];
		break;
	case CHIP_SEAEAGLE2:
		tbl = &g_dmdn_divider_tbl[2][0];
		break;
	case CHIP_GG8:
	case CHIP_ALBATROSS:
		tbl = &g_dmdn_divider_tbl[3][0];
		break;
	}

	if (cfg->fpga_item.fpga_ctrl.is_fpga_chip) {
		tbl = g_dmdn_divider_tbl_fpga;
		DbgInfo(MODULE_CFG_MNG, FEATURE_DRIVER_INIT, NOT_TO_RAM,
			"Use Dmdn table For FPGA\n");
	}

	if (tbl != NULL)
		os_memcpy(cfg->dmdn_tbl, tbl, sizeof(g_dmdn_divider_tbl_fpga));

	DbgInfo(MODULE_CFG_MNG, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"chip type =%d DMDN  basetbl = %p realtbl=%p\n", chip_type,
		&g_dmdn_divider_tbl[0][0], cfg->dmdn_tbl);
	cfg_print_debug(cfg);
	cfg->pcr_item.cnt = 0;

	/* clear pcr cfg */
	os_memset(&cfg->pcr_item, 0, sizeof(cfg_pcr_item_t));

	os_enum_reg_cfg(cfg, chip_type, (byte *) "\\pcr", os_load_pcr_cb);
	os_enum_reg_cfg(cfg, chip_type, (byte *) "\\dmdn", os_load_dmdn_cb);

	return cfg;
}

/*
 *
 * Function Name: print_registry_value
 *
 * Abstract:
 *			 1. print the registry name and it's value
 *
 * Input:
 *			PVOID cfg_item: Pointer to the registry configuration structure
 *
 * Output:
 *			None
 *
 * Return value:
 *           None
 *
 * Notes:
 *           Caller: registry_load
 */
void cfg_print_debug(PVOID cfg_item)
{
#if DBG || _DEBUG
	cfg_item_t *cfg = (cfg_item_t *) cfg_item;

	cfg_card_item_t card_item = cfg->card_item;
	cfg_host_item_t host_item = cfg->host_item;

	cfg_feature_item_t feature_item = cfg->feature_item;
	cfg_timer_item_t timer_item = cfg->timer_item;
	cfg_timeout_item_t timeout_item = cfg->timeout_item;

	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"-------------Registry item Check Start--------------------\n");

	/* -------------------- card item name as below------------------ */
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"sd_card_mode_dis:                    0x%08x\n",
		card_item.sd_card_mode_dis);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"test_max_access_mode:                0x%08x\n",
		card_item.test_max_access_mode);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"test_driver_strength_sel:            0x%08x\n",
		card_item.test_driver_strength_sel);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"test_max_power_limit:                0x%08x\n",
		card_item.test_max_power_limit);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"mmc_mode_dis:                        0x%08x\n",
		card_item.mmc_mode_dis);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"emmc_mode:                           0x%08x\n",
		card_item.emmc_mode);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"sd7_sdmode_switch_control:           0x%08x\n",
		card_item.sd7_sdmode_switch_control);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"test_uhs2_setting:                   0x%08x\n",
		card_item.uhs2_setting);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"test_uhs2_setting2:                  0x%08x\n",
		card_item.test_uhs2_setting2);

	/* -------------------- host item name as below ------------------ */
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"test_dma_mode_setting:               0x%08x\n",
		host_item.test_dma_mode_setting);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"test_infinite_transfer_mode:         0x%08x\n",
		host_item.test_infinite_transfer_mode);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"test_sdma_boun_setting:              0x%08x\n",
		host_item.test_sdma_boundary);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"test_tag_queue_capability:           0x%08x\n",
		host_item.test_tag_queue_capability);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"test_ocb_ctrl:                       0x%08x\n",
		host_item.test_ocb_ctrl);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"bios_l1_substate:                    0x%08x\n",
		host_item.bios_l1_substate);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"vdd_power_source_item:               0x%08x\n",
		host_item.vdd_power_source_item);

	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"host_drive_strength:                 0x%08x\n",
		host_item.host_drive_strength);

	/* -------------------- issue fix item name as below------------------ */

	/* -------------------- feature item name as below-------------------- */
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"psd_mode:                            0x%08x\n",
		feature_item.psd_mode);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"pcie_wake_setting:                   0x%08x\n",
		feature_item.pcie_wake_setting);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"test_main_ldo_setting:               0x%08x\n",
		feature_item.test_main_ldo_setting);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"output_tuning_item:                  0x%08x\n",
		feature_item.output_tuning_item);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"hsmux_vcme_enable:                   0x%08x\n",
		feature_item.hsmux_vcme_enable);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"refclk_stable_detection_counter1:    0x%08x\n",
		feature_item.refclk_stable_detection_counter1);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"refclk_stable_detection_counter2:    0x%08x\n",
		feature_item.refclk_stable_detection_counter2);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"refclk_stable_detection_counter3:    0x%08x\n",
		feature_item.refclk_stable_detection_counter3);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"auto_detect_refclk_counter_range_ctl:0x%08x\n",
		feature_item.auto_detect_refclk_counter_range_ctl);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"l1_enter_exit_logic_ctl:             0x%08x\n",
		feature_item.l1_enter_exit_logic_ctl);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"pcie_phy_amplitude_adjust:           0x%08x\n",
		feature_item.pcie_phy_amplitude_adjust);

	/* -------------------- timer item name as below------------------ */
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"auto_sleep_control:                  0x%08x\n",
		timer_item.auto_sleep_control);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"auto_dormant_timer:                  0x%08x\n",
		timer_item.auto_dormant_timer);

	/* -------------------- timeout item name as below------------------ */
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"power_wait_time:                     0x%08x\n",
		timeout_item.power_wait_time);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"test_write_data_timeout:             0x%08x\n",
		timeout_item.test_write_data_timeout);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"test_read_data_timeout:              0x%08x\n",
		timeout_item.test_read_data_timeout);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"test_non_data_timeout:               0x%08x\n",
		timeout_item.test_non_data_timeout);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"test_r1b_data_timeout:               0x%08x\n",
		timeout_item.test_r1b_data_timeout);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"test_card_init_timeout:              0x%08x\n",
		timeout_item.test_card_init_timeout);

	/* -------------------- fpga item name as below------------------ */

	/* -------------------- driver item name as below------------------ */
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"driver item:                         0x%08x\n",
		cfg->driver_item);
	DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, 0,
		"-------------Registry item Check End--------------------\n");
#endif
}

/*
 *
 * Function Name: fill_registry_struct
 *
 * Abstract:
 *			1. call memcpy function to fill the registry structure
 *
 * Input:
 *			PVOID cfg_item: Pointer to single registry key address
 *           u32 cfg_def_val: the value of the single registry key structure
 *           u32 data_len: memcpy data length
 * Output:
 *			None
 *
 * Return value:
 *           None
 *
 * Notes:
 *           Caller: cfg_set_default_val
 */
static void fill_registry_struct(PVOID cfg_item, u32 cfg_def_val, u32 data_len)
{
	u32 fill_registry_val = cfg_def_val;

	os_memcpy(cfg_item, &fill_registry_val, data_len);
}

/*
 *
 * Function Name: cfg_set_default_val
 *
 * Abstract:
 *			 1. set the default value for every registry key
 *
 * Input:
 *			PVOID cfg_item: Pointer to the registry configuration structure
 *            e_chip_type  type:
 *
 * Output:
 *			None
 *
 * Return value:
 *           None
 *
 * Notes:
 *           Caller: cfgmng_init
 */
static void cfg_set_default_val(cfg_item_t *cfg, e_chip_type type)
{
	u32 dma_mode, uhs2_setting, tag_queue_capability;

	if (type == CHIP_SEAEAGLE2 || type == CHIP_GG8
	    || type == CHIP_ALBATROSS) {
		dma_mode = 0x80000071;
		tag_queue_capability = 0x80010010;
	} else {
#if GLOBAL_ENABLE_BOOT
		tag_queue_capability = 0x80010020;
#else
		tag_queue_capability = 0x80010010;
#endif
#ifdef CFG_OS_LINUX
		dma_mode = 0x80000071;
#else
		dma_mode = 0x80000006;
#endif
	}

	if (type == CHIP_SEAEAGLE)
		uhs2_setting = 0x97080112;
	else if (type == CHIP_SEAEAGLE2)
		uhs2_setting = 0x97400012;
	else if (type == CHIP_GG8 || type == CHIP_ALBATROSS)
		uhs2_setting = 0x97400012;
	else
		uhs2_setting = 0x97100112;

	/* -------------------- card item name ------------------ *  */
	fill_registry_struct(&(cfg->card_item.sd_card_mode_dis), 0, 4);
	fill_registry_struct(&cfg->card_item.test_max_access_mode, 0x80000003,
			     4);
	fill_registry_struct(&cfg->card_item.test_driver_strength_sel, 0, 4);
	fill_registry_struct(&cfg->card_item.test_max_power_limit, 0x80000003,
			     4);
	fill_registry_struct(&cfg->card_item.mmc_mode_dis, 0, 4);
#if GLOBAL_ENABLE_BOOT
	fill_registry_struct(&cfg->card_item.emmc_mode, GLOBAL_EMMC_BOOT_CFG,
			     4);
#else
	fill_registry_struct(&cfg->card_item.emmc_mode, 0, 4);
#endif

	fill_registry_struct(&cfg->card_item.sd7_sdmode_switch_control,
			     0x00000030, 4);

	fill_registry_struct(&cfg->card_item.uhs2_setting, uhs2_setting, 4);

	fill_registry_struct(&cfg->card_item.test_uhs2_setting2, 0x80000006, 4);
	/* ------------------- card item name end --------------------- */

	/* -------------------- host item name ------------------ *  */
	fill_registry_struct(&cfg->host_item.test_dma_mode_setting, dma_mode,
			     4);
	if (type == CHIP_SDS0 || type == CHIP_SDS1)
		fill_registry_struct(&cfg->host_item.test_infinite_transfer_mode,
				     0x00000000, 4);
	else
		fill_registry_struct(&cfg->host_item.test_infinite_transfer_mode,
				     0x8000000F, 4);
	fill_registry_struct(&cfg->host_item.test_sdma_boundary, 0x20, 4);
	fill_registry_struct(&cfg->host_item.test_tag_queue_capability,
			     tag_queue_capability, 4);
	fill_registry_struct(&cfg->host_item.test_ocb_ctrl, 0x0, 4);
	fill_registry_struct(&cfg->host_item.bios_l1_substate, 0x8000000f, 4);

	fill_registry_struct(&cfg->host_item.vdd_power_source_item, 0x00091B05,
			     4);
	fill_registry_struct(&cfg->host_item.host_drive_strength, 0x00000000,
			     4);
	/* -------------------- host item name end-------------------- */

#ifdef CFG_OS_LINUX
	fill_registry_struct(&cfg->feature_item.psd_mode, 0x80000000, 4);
#else
	fill_registry_struct(&cfg->feature_item.psd_mode, 0x0a0a, 4);
#endif

	fill_registry_struct(&cfg->feature_item.psd_mode, 0x0, 4);

	fill_registry_struct(&cfg->feature_item.test_main_ldo_setting, 0, 4);

	fill_registry_struct(&cfg->feature_item.output_tuning_item, 0xC01F17DC,
			     4);

	/* -------------------- feature item name end------------------ */
	fill_registry_struct(&cfg->feature_item.hsmux_vcme_enable, 0x00000000,
			     4);

	fill_registry_struct(&cfg->feature_item.refclk_stable_detection_counter1,
			     0x00000003, 4);
	fill_registry_struct(&cfg->feature_item.refclk_stable_detection_counter2,
			     0x001e044c, 4);
	fill_registry_struct(&cfg->feature_item.refclk_stable_detection_counter3,
			     0x04b024b0, 4);
	fill_registry_struct(&cfg->feature_item.auto_detect_refclk_counter_range_ctl,
			     0x00000000, 4);
	fill_registry_struct(&cfg->feature_item.l1_enter_exit_logic_ctl,
			     0x00000000, 4);
	fill_registry_struct(&cfg->feature_item.pcie_phy_amplitude_adjust,
			     0x0000006a, 4);

	fill_registry_struct(&cfg->timer_item.auto_sleep_control, 0, 4);
	fill_registry_struct(&cfg->timer_item.auto_dormant_timer, 0x80000025,
			     4);
	/* -------------------- timer item name end------------------ *     44 */

	/* -------------------- timeout item name------------------ */
	fill_registry_struct(&cfg->timeout_item.power_wait_time, 0x0024000a, 4);

	fill_registry_struct(&cfg->timeout_item.test_write_data_timeout,
			     0x80001770, 4);
	fill_registry_struct(&cfg->timeout_item.test_read_data_timeout,
			     0x80001770, 4);
	fill_registry_struct(&cfg->timeout_item.test_non_data_timeout,
			     0x800003e8, 4);
	fill_registry_struct(&cfg->timeout_item.test_r1b_data_timeout,
			     0x80001194, 4);
	fill_registry_struct(&cfg->timeout_item.test_card_init_timeout,
			     0x800005dc, 4);
	/* -------------------- timeout item name end------------------ *     58 */

	/* -------------------- fpga item name----------------------- */
	fill_registry_struct(&cfg->fpga_item.fpga_ctrl, 0x00000000, 4);
	/* -------------------- fpga item name end ------------------ */

	/* -------------------- driver item name------------------ *  */
	fill_registry_struct(&(cfg->driver_item), 0x8080000a, 4);

	os_memset(&cfg->test_item, 0, sizeof(cfg->test_item));
}

/*
 *
 * Function Name: cfg_parse_card_item
 *
 * Abstract:
 *			 1. parse the card related registry information
 *                 - If the registry valid bit is invalid state, then set the default configuration
 *
 * Input:
 *           cfg_item_t *cfg: Pointer to the config structure
 *			       e_chip_type chip_type: chip type index
 *
 * Output:
 *			None
 *
 * Return value:
 *           None
 *
 * Notes:
 *           Caller: cfg_parse
 */
static void cfg_parse_card_item(cfg_item_t *cfg, e_chip_type chip_type)
{
	cfg_card_item_t *card_item = &cfg->card_item;

	/* sd_card_mode_dis */
	if (card_item->sd_card_mode_dis.sd_mode_dis_enable == 0) {
		card_item->sd_card_mode_dis.dis_sd30_card = 0;
		card_item->sd_card_mode_dis.dis_sd40_card = 0;
	}

	/* max_access_mode */
	if (card_item->test_max_access_mode.reserve == 0)
		card_item->test_max_access_mode.value = 3;

	/* max_power_limit */
	if (card_item->test_max_power_limit.reserve == 0)
		card_item->test_max_power_limit.value = 3;

	/* emmc_mode */
	if (card_item->emmc_mode.emmc_enable) {
		if ((chip_type != CHIP_SEAEAGLE2) && (chip_type != CHIP_GG8)
		    && (chip_type != CHIP_ALBATROSS)) {
			card_item->emmc_mode.enable_12_vccq = 0;
			card_item->emmc_mode.enable_18_vcc = 0;
			card_item->emmc_mode.enable_force_hs400 = 0;
		}

		if ((card_item->emmc_mode.dis_4bit_bus_width == 1) &&
		    (card_item->emmc_mode.dis_8bit_bus_width == 1)
		    ) {
			card_item->emmc_mode.enable_ddr_mode = 0;
		}

	} else {
		card_item->emmc_mode.dis_hs = 0;
		card_item->emmc_mode.dis_4bit_bus_width = 0;
		card_item->emmc_mode.dis_8bit_bus_width = 0;
		card_item->emmc_mode.enable_ddr_mode = 0;
		card_item->emmc_mode.enable_18_vccq = 0;
		card_item->emmc_mode.enable_force_hs200 = 0;
		card_item->emmc_mode.enable_12_vccq = 0;
		card_item->emmc_mode.enable_18_vcc = 0;
		card_item->emmc_mode.enable_force_hs400 = 0;
	}

	/* uhs2_setting */
	if (card_item->uhs2_setting.reserve == 0) {
		if (card_item->uhs2_setting.reserve_syn_dir_gap == 0) {
			card_item->uhs2_setting.min_lss_syn = 2;
			card_item->uhs2_setting.min_lss_dir = 1;
			if (chip_type == CHIP_SEAEAGLE2 || chip_type == CHIP_GG8
			    || chip_type == CHIP_ALBATROSS)
				card_item->uhs2_setting.min_data_gap_sel = 0;
			else
				card_item->uhs2_setting.min_data_gap_sel = 1;
		}

		if (card_item->uhs2_setting.reserve_nfcu == 0) {
			if (chip_type == CHIP_SEAEAGLE)
				card_item->uhs2_setting.max_nfcn_sel = 0x8;
			else if (chip_type == CHIP_SEAEAGLE2
				 || chip_type == CHIP_GG8
				 || chip_type == CHIP_ALBATROSS)
				card_item->uhs2_setting.max_nfcn_sel = 0x40;
			else
				card_item->uhs2_setting.max_nfcn_sel = 0x10;
		}

		/* half */
		card_item->uhs2_setting.half_full_sel = 1;
		/* fast mode */
		card_item->uhs2_setting.fast_low_pwr_sel = 0;
		/* range-B */
		card_item->uhs2_setting.max_speed_range_sel = 1;
	}

	/* test_uhs2_setting2 */
	if (card_item->test_uhs2_setting2.reserve == 0) {
		card_item->test_uhs2_setting2.enable_power_off_vdd1 = 0;
		card_item->test_uhs2_setting2.enable_full_reset_reinit = 1;
		card_item->test_uhs2_setting2.enable_internal_clk_dormant = 1;
		card_item->test_uhs2_setting2.disable_scramb_mode = 0;
	}
}

/*
 *
 * Function Name: cfg_parse_host_item
 *
 * Abstract:
 *            1. set the host cfg pointer
 *			 2. parse the host related registry information
 *                 - If the registry valid bit is invalid state, then set the default configuration
 *
 * Input:
 *            cfg_item_t *cfg: Pointer to the config structure
 *			e_chip_type chip_type: chip type index
 *
 * Output:
 *			None
 *
 * Return value:
 *           None
 *
 * Notes:
 *           Caller: cfg_parse
 */
static void cfg_parse_host_item(cfg_item_t *cfg, e_chip_type chip_type)
{
	cfg_host_item_t *host_item = &cfg->host_item;

	/* dma_mode_setting */
	if (host_item->test_dma_mode_setting.reserve == 0) {
		/* adma2 */
		host_item->test_dma_mode_setting.dma_mode = 1;
		host_item->test_dma_mode_setting.enable_dma_26bit_len = 0;
		host_item->test_dma_mode_setting.enable_dma_64bit_address = 0;
		host_item->test_dma_mode_setting.enable_dma_32bit_blkcount = 0;
	}

	if ((chip_type != CHIP_SEAEAGLE2) && (chip_type != CHIP_GG8)
	    && (chip_type != CHIP_ALBATROSS)) {
		/* only SE2 support merge */
		host_item->test_tag_queue_capability.enable_srb_merge = 0;
	}

	/* infinite_transfer_mode */
	if (host_item->test_infinite_transfer_mode.enable_inf == 0) {
		host_item->test_infinite_transfer_mode.enable_legacy_inf = 0;
		host_item->test_infinite_transfer_mode.enable_sd40_inf = 0;
		host_item->test_infinite_transfer_mode.enable_mmc_inf = 0;
		host_item->test_infinite_transfer_mode.enable_emmc_inf = 0;
	}

	/* sdma_boundary_len_setting */
	if (host_item->test_sdma_boundary.reserve) {
		/* SDMA */
		if (host_item->test_dma_mode_setting.dma_mode == 0) {
			if (host_item->test_sdma_boundary.value < 4)
				host_item->test_sdma_boundary.value = 4;
		}
	} else {
		host_item->test_sdma_boundary.value = 32;
	}

	/* tag_queue_capability */
	if (host_item->test_tag_queue_capability.reserve == 0) {
		/* 16 SRBs */
		host_item->test_tag_queue_capability.max_srb = 0x10;
		host_item->test_tag_queue_capability.enable_srb_merge = 0;
	} else {
		if (host_item->test_tag_queue_capability.max_srb == 0)
			host_item->test_tag_queue_capability.max_srb = 0x10;
	}

	if (host_item->test_ocb_ctrl.sw_pwroff_en)
		host_item->test_ocb_ctrl.int_check_en = 1;
}

/*
 *
 * Function Name: cfg_parse_issue_fix_item
 *
 * Abstract:
 *            1. set the issue ifx item cfg pointer
 *			 2. parse the issue fix item related registry information
 *                 - If the registry valid bit is invalid state, then set the default configuration
 *
 * Input:
 *            cfg_item_t *cfg: Pointer to the config structure
 *
 * Output:
 *			None
 *
 * Return value:
 *           None
 *
 * Notes:
 *           Caller: cfg_parse
 */
static void cfg_parse_issue_fix_item(cfg_item_t *cfg)
{

}

/*
 *
 * Function Name: cfg_parse_feature_item
 *
 * Abstract:
 *            1. set the feature item cfg pointer
 *			 2. parse the feature item related registry information
 *                 - If the registry valid bit is invalid state, then set the default configuration
 *
 * Input:
 *            cfg_item_t *cfg: Pointer to the config structure
 *
 * Output:
 *			None
 *
 * Return value:
 *           None
 *
 * Notes:
 *           Caller: cfg_parse
 */
static void cfg_parse_feature_item(cfg_item_t *cfg, e_chip_type chip_type)
{
	if (chip_type != CHIP_SEAEAGLE && chip_type != CHIP_SEAEAGLE2
	    && chip_type != CHIP_GG8 && chip_type != CHIP_ALBATROSS) {
		cfg->feature_item.output_tuning_item.enable_dll_divider = 0;
	}
}

/*
 *
 * Function Name: cfg_parse_timeout_item
 *
 * Abstract:
 *            1. set the timeout item cfg pointer
 *			 2. parse the timeout item related registry information
 *                 - If the registry valid bit is invalid state, then set the default configuration
 *
 * Input:
 *            cfg_item_t *cfg: Pointer to the config structure
 *
 * Output:
 *			None
 *
 * Return value:
 *           None
 *
 * Notes:
 *           Caller: cfg_parse
 */
static void cfg_parse_timeout_item(cfg_item_t *cfg)
{
	cfg_timeout_item_t *timeout_item = &cfg->timeout_item;

	if (timeout_item->test_write_data_timeout.reserve == 0)
		/* default:6s */
		timeout_item->test_write_data_timeout.value = 6000;

	if (timeout_item->test_read_data_timeout.reserve == 0)
		/* default:6s */
		timeout_item->test_read_data_timeout.value = 6000;

	if (timeout_item->test_non_data_timeout.reserve == 0)
		timeout_item->test_non_data_timeout.value = 1000;

	if (timeout_item->test_r1b_data_timeout.reserve == 0)
		timeout_item->test_r1b_data_timeout.value = 4500;

	if (timeout_item->test_card_init_timeout.reserve == 0)
		/* delay ACMD41/CMD1 return ready 1.5s (max) */
		timeout_item->test_card_init_timeout.value = 1500;
}

/*
 *
 * Function Name: cfg_parse_fpga_item
 *
 * Abstract:
 *            1. set the fpga item cfg pointer
 *			 2. parse the fpga item related registry information
 *                 - If the registry valid bit is invalid state, then set the default configuration
 *
 * Input:
 *            cfg_item_t *cfg: Pointer to the config structure
 *
 * Output:
 *			None
 *
 * Return value:
 *           None
 *
 * Notes:
 *           Caller: cfg_parse
 */
static void cfg_parse_fpga_item(cfg_item_t *cfg)
{

}

/*
 *
 * Function Name: cfg_parse_driver_item
 *
 * Abstract:
 *            1. set the driver item cfg pointer
 *			 2. parse the driver item related registry information
 *                 - If the registry valid bit is invalid state, then set the default configuration
 *
 * Input:
 *            cfg_item_t *cfg: Pointer to the config structure
 *
 * Output:
 *			None
 *
 * Return value:
 *           None
 *
 * Notes:
 *           Caller: cfg_parse
 */
static void cfg_parse_driver_item(cfg_item_t *cfg, e_chip_type chip_type)
{
	cfg_driver_item_t *driver_item = &cfg->driver_item;
	/* driver item */
	if (driver_item->reserve == 0) {

		driver_item->dis_patch_ntfs_verify_rtd3 = FALSE;
		driver_item->dis_patch_rtd3_idle_ref_cnt = FALSE;
		driver_item->delay_for_failsafe_s3resume = 3;
		driver_item->failsafe_en = 0;

	}
#if GLOBAL_ENABLE_BOOT
	driver_item->removable = FALSE;
	driver_item->removable_pnp = FALSE;
#endif

}

/*
 *
 * Function Name: cfg_parse
 *
 * Abstract:
 *			 1. parse the registry information
 *                 - If the registry valid bit is invalid state, then set the default configuration
 *
 * Input:
 *			cfg_item_t *cfg: Pointer to the cfg_item structure
 *            e_chip_type chip_type: chip type index
 *
 * Output:
 *			None
 *
 * Return value:
 *           None
 *
 * Notes:
 *           Caller: cfgmng_init
 */
static void cfg_parse(cfg_item_t *cfg, e_chip_type chip_type)
{
	cfg_parse_card_item(cfg, chip_type);
	cfg_parse_host_item(cfg, chip_type);
	cfg_parse_issue_fix_item(cfg);
	cfg_parse_feature_item(cfg, chip_type);
	cfg_parse_timeout_item(cfg);
	cfg_parse_fpga_item(cfg);
	cfg_parse_driver_item(cfg, chip_type);

}

void cfg_dma_mode_dec(cfg_item_t *cfg, u32 dec_dma_mode)
{
	cfg->host_item.test_dma_mode_setting.dma_mode = dec_dma_mode;
	DbgInfo(MODULE_CFG_MNG, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Change DMA mode to 0x%08x\n", dec_dma_mode);
}

void cfg_dma_addr_range_dec(cfg_item_t *cfg, u32 dma_range)
{
	cfg->host_item.test_dma_mode_setting.enable_dma_64bit_address =
	    dma_range;
	DbgInfo(MODULE_CFG_MNG, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Change DMA addr(64bit Address) to %d\n", dma_range);
}

void cfgmng_update_dumpmode(cfg_item_t *cfg, e_chip_type chip_type)
{
	/* Use Non-Infinite adma2 mode for dump_mode */
	cfg->host_item.test_dma_mode_setting.dma_mode = CFG_TRANS_MODE_ADMA2;

	cfg->timer_item.auto_dormant_timer.enable_dmt_func = 0;

	/* disable rtd3 */
	cfg->feature_item.psd_mode.enable_rtd3 = FALSE;

	/* disable output tuning */
	cfg->feature_item.output_tuning_item.enable_dll = 0;
	cfg->host_item.test_tag_queue_capability.max_srb = 1;

	cfg->boot_flag = TRUE;

	DbgInfo(MODULE_CFG_MNG, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"update cfg for dump_mode setting\n");
}

/* bus_width 0: 1bit   1: 4bit     2: 8bit */
void cfg_emmc_busw_supp(cfg_emmc_mode_t *emmc_mode, u8 bus_width)
{
	emmc_mode->dis_8bit_bus_width = 1;
	emmc_mode->dis_4bit_bus_width = 1;

	if (bus_width == 1) {
		emmc_mode->dis_8bit_bus_width = 1;
		emmc_mode->dis_4bit_bus_width = 0;
	} else if (bus_width == 2) {
		emmc_mode->dis_8bit_bus_width = 0;
		emmc_mode->dis_4bit_bus_width = 0;
	}

	DbgInfo(MODULE_CFG_MNG, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Change MMC bus width to %d\n", bus_width);
}

void os_load_pcr_cb(void *cfgp, u32 type, u32 idx, u32 addr, u32 value)
{
	cfg_item_t *cfg = cfgp;

	if (idx >= MAX_PCR_SETTING_SIZE) {
		DbgErr("%s idx(%d) ovf(%d)\n", __func__, idx,
		       MAX_PCR_SETTING_SIZE);
		goto exit;
	}
	if (cfg->pcr_item.cnt < MAX_PCR_SETTING_SIZE) {
		cfg_pcr_t *pcr = &cfg->pcr_item.pcr_tb[idx];

		pcr->valid_flg = 1;
		pcr->type = type;
		pcr->addr = (u16) addr;
		pcr->mask = (u16) (value >> 16);
		pcr->val = (u16) (value & 0xffff);
		cfg->pcr_item.cnt++;
		DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, NOT_TO_RAM,
			"[%d] PCR Addr: 0x%04X vlaue=0x%8X\n", idx, addr,
			value);
		DbgErr("[%d] PCR Addr: 0x%04X vlaue=0x%8X\n", idx, addr, value);
	} else {
		DbgErr("%s cnt(%d) ovf(%d)\n", __func__, cfg->pcr_item.cnt,
		       MAX_PCR_SETTING_SIZE);
		goto exit;
	}
exit:
	;
}

void os_load_dmdn_cb(void *cfgp, u32 type, u32 idx, u32 addr, u32 value)
{
	cfg_item_t *cfg = cfgp;

	if (addr < MAX_FREQ_SUPP) {
		cfg->dmdn_tbl[addr] = value;
		DbgInfo(MODULE_CFG_MNG, FEATURE_CFG_TRACE, NOT_TO_RAM,
			"DMDN Table idx: 0x%x, Value: 0x%08x\n", addr, value);
	}
}

bool cfg_dma_need_sdma_like_buffer(u32 dma_mode)
{
	if ((dma_mode == CFG_TRANS_MODE_ADMA2_SDMA_LIKE) ||
	    (dma_mode == CFG_TRANS_MODE_ADMA3_SDMA_LIKE) ||
	    (dma_mode == CFG_TRANS_MODE_SDMA) ||
	    (dma_mode == CFG_TRANS_MODE_ADMA_MIX_SDMA_LIKE))
		return TRUE;
	else
		return FALSE;

}
