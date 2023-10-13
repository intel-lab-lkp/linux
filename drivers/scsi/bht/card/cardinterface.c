// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: cardinterface.c
 *
 * Abstract:
 *           1. Card initialization main entry
 *           2. Interface for card operations
 *
 * Version: 1.00
 *
 * Author: Samuel
 *
 * Environment:	OS Independent
 *
 * History:
 *
 * 9/3/2014   Creation    Samuel
 */

#include "../include/basic.h"
#include "../include/card.h"
#include "../include/cardapi.h"
#include "../include/hostapi.h"
#include "../include/transhapi.h"
#include "../include/hostvenapi.h"
#include "../include/util.h"
#include "../include/debug.h"
#include "../include/cmdhandler.h"
#include "../host/hostven.h"
#include "../include/card.h"
#include "../host/hostreg.h"
#include "../include/funcapi.h"
#include "../tagqueue/tq_trans_api.h"
#include "../include/cmdhandler.h"
#include "cardcommon.h"

/* Thomas add for direct remove 7.0 */
extern void bht_sd_remove(struct pci_dev *pdev);

bool sd_thermal_control(sd_card_t *card);
void uhs2_degrade_policy(sd_card_t *card, sd_command_t *sd_cmd);
bool uhs2_sd_error_recovery(sd_card_t *card, sd_command_t *sd_cmd);
void sd_degrade_policy(sd_card_t *card);
void mmc_degrade_policy(sd_card_t *card);
u32 card_get_uhs2_freq(sd_card_t *card);
u32 sdr104_sdr50_output_tuning(sd_card_t *card, u32 address);
u32 ddr200_output_tuning(sd_card_t *card, u32 address);

bool sd_dll_divider(sd_card_t *card, sd_command_t *pcmd);

byte tuning_address_content_buf[512] = { 0 };

bool store_tuning_address_content(sd_card_t *card, u64 tuning_address)
{
	bool ret = 0;
	sd_command_t sd_cmd;
	u32 cmdflag;
	sd_host_t *host = card->host;

	card->read_signal_block_flag = TRUE;
	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Enter %s\n",
		__func__);
	/* Save Current DMA mode */
	host_transfer_init(card->host, FALSE, TRUE);
	cmdflag = CMD_FLG_RESCHK | CMD_FLG_R1 | CMD_FLG_ADMA_SDMA;

	ret =
	    card_send_sdcmd_timeout(card, &sd_cmd, SD_CMD17,
				    (u32) tuning_address, (cmdflag),
				    DATA_DIR_IN, tuning_address_content_buf,
				    512, 500);
	if (ret == FALSE) {
		host_reset(host, SDHCI_RESET_CMD);
		host_reset(host, SDHCI_RESET_DATA);
		card->read_signal_block_flag = FALSE;
		DbgErr("Read data FAILED when store tuning address content\n");
	}

	/* Resorte current DMA mode */
	host_transfer_init(card->host, card->inf_trans_enable, FALSE);

	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return ret;
}

bool restore_tuning_address_content(sd_card_t *card, u64 tuning_address)
{
	bool ret = 0;
	int i = 0;
	sd_command_t sd_cmd;
	u32 cmdflag;
	byte tuning_temp_buf[512];
	bool gg8_ddr200 = 0;

	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if (card->host->chip_type == CHIP_GG8
	    && card->info.sw_cur_setting.sd_access_mode == SD_FNC_AM_DDR200) {
		/* Save Current DMA mode */
		host_transfer_init(card->host, TRUE, FALSE);
		gg8_ddr200 = 1;
		cmdflag =
		    CMD_FLG_RESCHK | CMD_FLG_R1 | CMD_FLG_ADMA_SDMA |
		    CMD_FLG_DDR200_WORK_AROUND | CMD_FLG_INF_BUILD;
	} else {
		/* Save Current DMA mode */
		host_transfer_init(card->host, FALSE, TRUE);
		cmdflag = CMD_FLG_RESCHK | CMD_FLG_R1 | CMD_FLG_ADMA_SDMA;
	}

	ret =
	    card_send_sdcmd_timeout(card, &sd_cmd,
				    gg8_ddr200 ? SD_CMD25 : SD_CMD24,
				    (u32) tuning_address, (cmdflag),
				    DATA_DIR_OUT, tuning_address_content_buf,
				    512, 500);
	if (ret == FALSE) {
		DbgErr
		    ("Write data FAILED when restore tuning address content\n");
		goto exit;
	}

	ret =
	    card_send_sdcmd_timeout(card, &sd_cmd,
				    gg8_ddr200 ? SD_CMD18 : SD_CMD17,
				    (u32) tuning_address,
				    (cmdflag | CMD_FLG_ADMA_SDMA), DATA_DIR_IN,
				    tuning_temp_buf, 512, 500);
	if (ret == FALSE) {
		DbgErr("Read data FAILED when store tuning address content\n");
		goto exit;
	}

	for (i = 0; i < 512; i++) {
		if (tuning_temp_buf[i] != tuning_address_content_buf[i]) {
			DbgErr("Tuning address compare err!!!Write data 0x%x, Read out data 0x%x, Offset %d\n",
			tuning_address_content_buf[i], tuning_temp_buf[i], i);
			ret = FALSE;
			break;
		}
		DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
			"Write data 0x%x, Read out data 0x%x, Offset %d\n",
			tuning_address_content_buf[i], tuning_temp_buf[i], i);
	}

exit:
	/* Resorte current DMA mode */
	host_transfer_init(card->host, card->inf_trans_enable, FALSE);
	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return ret;
}

bool card_output_tuning(sd_card_t *card, u64 tuning_address)
{
	sd_host_t *host = card->host;
	int ii, jj, pattern_i, first_0, dll_i_mod;
	int dat_cmp, dll_result[16];
	byte test_patern[6] = { 0x55, 0xaa, 0x00, 0xff, 0xf0, 0x0f };
	u32 dll_i, window_pass_number[16],
	    window_start_adr[16], window_pass_number_max, dll_mod;
	u32 ret = FALSE;
	bool result = FALSE;
	sd_command_t sd_cmd;
	u32 cmdflag;
	u8 phase_count = 11;

	byte *test_buf = kcalloc(512, sizeof(unsigned char), GFP_KERNEL);
	byte *test_buf_read = kcalloc(512, sizeof(unsigned char), GFP_KERNEL);

	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if (test_buf == NULL || test_buf_read == NULL) {
		DbgErr("kcalloc buffer failed\n");
		if (test_buf != NULL)
			kfree(test_buf);
		if (test_buf_read != NULL)
			kfree(test_buf_read);
		return FALSE;
	}

	host->output_tuning.start_block = (u32) tuning_address;
	host->output_tuning.auto_phase_flag = FALSE;

	/* Save Current DMA mode */
	if (card->host->chip_type == CHIP_GG8
	    && card->info.sw_cur_setting.sd_access_mode != SD_FNC_AM_DDR200)
		host_transfer_init(host, FALSE, TRUE);
	else
		host_transfer_init(host, TRUE, FALSE);

	if (host->chip_type == CHIP_GG8 || host->chip_type == CHIP_ALBATROSS) {
		if (card->info.sw_cur_setting.sd_access_mode ==
		    SD_FNC_AM_DDR200)
			ret = ddr200_output_tuning(card, (u32) tuning_address);
		else
			ret =
			    sdr104_sdr50_output_tuning(card,
						       (u32) tuning_address);

		if (ret == 0)
			result = TRUE;
	} else {
		cmdflag = CMD_FLG_RESCHK | CMD_FLG_R1 | CMD_FLG_ADMA_SDMA;
		window_pass_number_max = 0;
		for (dll_i = 0; dll_i < phase_count; dll_i++)
			dll_result[dll_i] = TRUE;

		for (dll_i = 0; dll_i < 512; dll_i++)
			test_buf[dll_i] = test_patern[dll_i % 6];

		host_cmddat_line_reset(host);

		if (host->chip_type != CHIP_GG8
		    || host->chip_type == CHIP_ALBATROSS) {
			if (card_check_rw_ready(card, &sd_cmd, 600) != TRUE) {
				DbgErr
				    ("Error when output_tuning,  card_check_rw_ready fail\n");
				goto exit;
			}
		}

		for (dll_i = 0; dll_i < phase_count; dll_i++) {
			DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER,
				NOT_TO_RAM, " - DLL Adjust Test %d\n", dll_i);

			if (card->card_present == FALSE) {
				DbgErr
				    ("Error when output_tuning,  card is removed\n");
				goto exit;
			}
			host_cmddat_line_reset(host);
			host_set_output_tuning_phase(host, dll_i);

			if (card->info.sw_cur_setting.sd_access_mode ==
			    SD_FNC_AM_SDR104
			    || card->info.sw_cur_setting.sd_access_mode ==
			    SD_FNC_AM_SDR50
			    || card->info.sw_cur_setting.sd_access_mode ==
			    SD_FNC_AM_DDR200) {

				ret = sd_tuning(card, &sd_cmd, 150);
				if (ret == FALSE) {
					DbgErr("Error when output_tuning, sd_tuning fail at phase %d\n", dll_i);
					dll_result[dll_i] = FALSE;
					continue;
				}
			}

			for (pattern_i = 0; pattern_i < 1; pattern_i++) {

				ret = card_send_sdcmd_timeout(card, &sd_cmd,
							    SD_CMD24,
							    host->output_tuning.start_block,
							    (cmdflag),
							    DATA_DIR_OUT,
							    test_buf, 512, 500);
				if (ret == FALSE) {
					DbgErr("Write data FAILED when output_tuning\n");
					dll_result[dll_i] = FALSE;
					host_cmddat_line_reset(host);
					card_send_command12(card, &sd_cmd);
					if (card_check_rw_ready
					    (card, &sd_cmd, 600) != TRUE) {
						DbgErr("Error when output_tuning write CMD, card_check_rw_ready fail\n");
						goto exit;
					}
					break;
				}

				ret = card_send_sdcmd_timeout(card, &sd_cmd,
							    SD_CMD17,
							    host->output_tuning.start_block,
							    (cmdflag),
							    DATA_DIR_IN,
							    test_buf_read, 512,
							    500);
				if (ret == FALSE) {
					DbgErr("Read data FAILED when output_tuning\n");
					dll_result[dll_i] = FALSE;
					host_cmddat_line_reset(host);
					card_send_command12(card, &sd_cmd);
					if (card_check_rw_ready(card, &sd_cmd, 600) != TRUE) {
						DbgErr("Error when output_tuning read CMD, card_check_rw_ready fail\n");
						goto exit;
					}
					break;
				}

				dat_cmp = TRUE;
				for (ii = 0; ii < (1 * 512); ii++) {
					if (*(test_buf + ii) !=
					    *(test_buf_read + ii)) {
						dat_cmp = FALSE;
						dll_result[dll_i] = FALSE;
						break;
					}
				}
				if (dat_cmp == FALSE)
					DbgErr("Compare data FAILED at index %d!!!\n", ii);

			}
		}

		for (ii = 0; ii < 16; ii++) {
			window_pass_number[ii] = 0;
			window_start_adr[ii] = 0;
		}

		first_0 = 0;
		for (dll_i = 0; dll_i < phase_count; dll_i++) {
			if (dll_result[dll_i] != TRUE) {
				first_0 = dll_i;
				break;
			}
		}

		jj = 0;
		for (dll_i = 0; dll_i < phase_count; dll_i++) {
			dll_i_mod = (first_0 + dll_i) % phase_count;
			DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER,
				NOT_TO_RAM, "DLL phase [%x] result %d.\n",
				dll_i_mod, dll_result[dll_i_mod]);
			if (dll_result[dll_i_mod] == TRUE)
				window_pass_number[jj]++;
			else {
				if (window_pass_number[jj] > 0)
					jj++;
			}
			if (window_pass_number[jj] == 1)
				window_start_adr[jj] = dll_i_mod;
		}

		for (ii = 0; ii < phase_count; ii++) {
			if (window_pass_number_max < window_pass_number[ii]) {
				window_pass_number_max = window_pass_number[ii];
				jj = ii;
			}
		}

		if (window_pass_number_max == 0)
			DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER,
				NOT_TO_RAM,
				"DLL test result: All DLL test FAIL\n");
		else {
			DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER,
				NOT_TO_RAM,
				"DLL test result: Total	%d DLL test PASS\n",
				window_pass_number_max);
			window_pass_number_max = window_pass_number_max >> 1;
			dll_mod = window_start_adr[jj] + window_pass_number_max;
			dll_mod = dll_mod % phase_count;

			DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER,
				NOT_TO_RAM, "select DLL phase Number %d\n",
				dll_mod);
			host->output_tuning.auto_phase = dll_mod;
			host->output_tuning.auto_phase_flag = TRUE;
			result = TRUE;
			host_set_output_tuning_phase(host,
				host->output_tuning.auto_phase);
			if (card->info.sw_cur_setting.sd_access_mode ==
			    SD_FNC_AM_SDR104
			    || card->info.sw_cur_setting.sd_access_mode ==
			    SD_FNC_AM_SDR50
			    || card->info.sw_cur_setting.sd_access_mode ==
			    SD_FNC_AM_DDR200) {
				ret = sd_tuning(card, &sd_cmd, 150);
				if (ret == FALSE) {
					DbgErr
					    ("Error when output_tuning,  sd_tuning fail\n");
					result = FALSE;
					goto exit;
				}
			}

		}

	}
exit:

	/* Resorte current DMA mode */
	host_transfer_init(host, card->inf_trans_enable, FALSE);
	if (result == FALSE)
		hostven_set_output_tuning_phase(host, 0, TRUE);
	host_cmddat_line_reset(host);
	host->output_tuning.auto_flag = FALSE;

	kfree(test_buf);
	kfree(test_buf_read);

	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Exit %s result is %d\n", __func__, result);

	return result;
}

static bool legacy_error_recovery(sd_card_t *card, sd_command_t *pcmd)
{
	bool ret;
	sd_command_t sd_cmd;
	card_info_t *card_info = &(card->info);
	sd_host_t *host = card->host;
	bht_dev_ext_t *pdx = host->pdx;
	cfg_output_tuning_item_t *cfg =
	    &pdx->cfg->feature_item.output_tuning_item;

	DbgInfo(MODULE_LEGACY_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Enter %s\n", __func__);

	/* Follow SD Host Spec V4.10 Section 3.10.1 Error Interrupt Recovery flow (Page 179) */
	card_send_command12(card, &sd_cmd);

	/* Call host api to do host related recovery stage2 */
	ret =
	    host_error_int_recovery_stage2(card->host,
					   sd_cmd.err.legacy_err_reg);

	if (ret == FALSE)
		goto exit;

	ret = card_check_rw_ready(card, &sd_cmd, 150);

	if (ret == FALSE) {
		DbgErr("Card status is not ready after error recovery");
		goto exit;
	}

	if (pcmd != NULL) {
		/* Crc error */
		if (pcmd->err.legacy_err_reg & (BIT1 | BIT5)) {
			if (host->cfg->feature_item.output_tuning_item.enable_dll
				== 0) {
				if (card->card_type == CARD_SD)
					ret = sd_tuning(card, &sd_cmd, 0);
				else if (card->card_type == CARD_EMMC)
					ret = emmc_tuning(card, &sd_cmd);
			} else {

				if (card->card_type == CARD_SD &&
				    pcmd->data &&
				    pcmd->data->dir == DATA_DIR_OUT &&
				    ((cfg->enable_dll == 1)
				     && (cfg->enable_dll_divider == 1))
				    && (card_info->sw_cur_setting.sd_access_mode ==
					SD_FNC_AM_DDR50)) {
					ret = sd_dll_divider(card, pcmd);
					if (ret)
						goto exit;
				} else if (pcmd->data
					   && pcmd->data->dir == DATA_DIR_OUT) {
					if (card->card_type == CARD_SD
					    || card->card_type == CARD_EMMC) {
						if (hostven_fix_output_tuning(card->host,
							card_info->sw_cur_setting.sd_access_mode)
						    == FALSE) {
							ret = card_output_tuning(card,
								pcmd->argument);
							if (ret)
								goto exit;
						}
					}
				} else if (pcmd->data
					   && pcmd->data->dir == DATA_DIR_IN) {
					if (card->card_type == CARD_SD) {
						ret =
						    sd_tuning(card, &sd_cmd, 0);
					} else if (card->card_type == CARD_EMMC) {
						ret =
						    emmc_tuning(card, &sd_cmd);
					}
				}
			}
		}

	}

exit:
	DbgInfo(MODULE_LEGACY_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Exit %s\n", __func__);
	return ret;
}

/*
 * Function Name: card_degrade_policy
 *
 * Abstract: This Function is used set card degrade flag
 *           if blightway is set, this function can also do card operation which don't need reinit
 *
 * Input:
 *
 * sd_card_t *card : The Command will send to which  Card
 * sd_command_t *sd_cmd: if the init occurred at init stage this parameter will be null
 *
 * Output: None
 *
 * Return value: None
 *
 * Notes:
 *
 * Caller: card_init
 *
 */

void card_degrade_policy(sd_card_t *card, sd_command_t *sd_cmd)
{
	DbgInfo(MODULE_ALL_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Enter %s\n", __func__);
	switch (card->card_type) {
	case CARD_SD:
		sd_degrade_policy(card);
		break;
	case CARD_UHS2:
		uhs2_degrade_policy(card, sd_cmd);
		break;
	case CARD_EMMC:
	case CARD_MMC:
		mmc_degrade_policy(card);
		break;
	default:
		break;

	}
	DbgInfo(MODULE_ALL_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

/*
 * Function Name: card_rw_recovery
 *
 * Abstract: This Function is used to do card rw error recovery
 *
 * Input:
 *
 * sd_card_t *card : The Command will send to which  Card
 * sd_command_t *sd_cmd: if the init occurred at init stage this parameter will be null
 *
 * Output: None
 *
 * Return value: If the routine succeeds, it must return TRUE,
 *               and fill trans_reg_t  part. otherwize reutrn FALSE
 *
 * Notes:
 *
 * Caller: card_recovery_flow
 *
 */

static bool card_rw_recovery(sd_card_t *card, sd_command_t *sd_cmd)
{

	if (sd_cmd == NULL)
		return FALSE;

	switch (card->card_type) {
	case CARD_EMMC:
	case CARD_MMC:
	case CARD_SD:
		return legacy_error_recovery(card, sd_cmd);
	case CARD_UHS2:
		return uhs2_sd_error_recovery(card, sd_cmd);
	default:
		DbgErr("Error Card no RW error recovery\n");
		break;

	}

	return FALSE;
}

/*
 * Function Name: card_init_infinite
 *
 * Abstract: This Function is used to determine whehter use infinte or not according to card type
 *
 * Input:
 *
 * sd_card_t *card : The Command will send to which  Card
 * sd_host_t *host: Pointer to the host structure
 *
 * Output: None
 *
 * Return value: None
 *
 * Notes:
 *
 * Caller: card_init
 *
 */

static void card_init_transfer(sd_card_t *card, sd_host_t *host)
{
	bool autocmd23 = FALSE;

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	card->has_built_inf = FALSE;
	card->last_dir = DATA_DIR_NONE;
	card->last_sect = 0;

	if (host->cfg->host_item.test_infinite_transfer_mode.enable_inf ==
	    FALSE) {
		card->inf_trans_enable = FALSE;
		goto next;
	}

	switch (card->card_type) {
	case CARD_UHS2:
		card->inf_trans_enable =
		    (byte) host->cfg->host_item.test_infinite_transfer_mode.enable_sd40_inf;
		break;
	case CARD_SD:
		card->inf_trans_enable =
		    (byte) host->cfg->host_item.test_infinite_transfer_mode.enable_legacy_inf;
		break;
	case CARD_MMC:
		card->inf_trans_enable =
		    (byte) host->cfg->host_item.test_infinite_transfer_mode.enable_mmc_inf;
		break;
	case CARD_EMMC:
		card->inf_trans_enable =
		    (byte) host->cfg->host_item.test_infinite_transfer_mode.enable_emmc_inf;
		break;
	default:
		card->inf_trans_enable = FALSE;
		break;
	}

next:
	if ((card->card_type == CARD_SD) && (card->info.scr.cmd_support & 0x2))
		autocmd23 = TRUE;

	host_transfer_init(host, card->inf_trans_enable, FALSE);
	host_enable_cmd23(host, autocmd23);
	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

/*
 * Function Name: card_switch2_adma
 *
 * Abstract: call this function to switch to adma2 mode, caller must restore.
 *
 * Input:
 *
 * sd_card_t *card : The Command will send to which  Card
 * sd_command_t *sd_cmd: if the init occurred at init stage this parameter will be null
 *
 * Output: None
 *
 * Return value: If the routine succeeds, it must return TRUE, otherwize reutrn FALSE
 *
 * Notes:
 *
 * Caller:
 *
 */

bool card_switch2_adma(sd_card_t *card, sd_command_t *sd_cmd)
{
	bool ret = FALSE;

	DbgInfo(MODULE_ALL_CARD, FEATURE_ERROR_RECOVER | FEATURE_IOCTL_TRACE,
		NOT_TO_RAM, "Enter %s\n", __func__);
	/* stop inf */
	if (card->has_built_inf) {
		/* need stop first */
		ret = card_stop_infinite(card, FALSE, sd_cmd);
		if (ret == FALSE) {
			DbgErr("Stop Inf error for swithc2_adma\n");
			goto exit;
		}
	}

	host_transfer_init(card->host, FALSE, TRUE);
	ret = TRUE;
exit:
	DbgInfo(MODULE_ALL_CARD, FEATURE_ERROR_RECOVER | FEATURE_IOCTL_TRACE,
		NOT_TO_RAM, "Enter %s ret=%d\n", __func__, ret);
	return ret;
}

/*
 * Function Name: card_degrade_info_init
 *
 * Abstract: init card degrade info
 *
 * Input:
 *
 * sd_card_t *card : The Command will send to which  Card
 * sd_host_t *host: Pointer to the host structure
 *
 * Output: None
 *
 * Return value: None
 *
 * Notes:
 *
 * Caller: card_stuct_init
 *
 */

static void card_degrade_info_init(sd_card_t *card, sd_host_t *host)
{
	/* 1. Init the target access mode (Legacy mode) to the maximum access mode. */
	card->sw_target_setting.sd_access_mode =
	    (byte) host->cfg->card_item.test_max_access_mode.value;

	/* 2. Init the target drive strength */
	card->sw_target_setting.sd_drv_type =
	    (byte) host->cfg->card_item.test_driver_strength_sel.value;

	/* 3. Init the target power limit */
	card->sw_target_setting.sd_power_limit =
	    (byte) host->cfg->card_item.test_max_power_limit.value;

	card->degrade_uhs2_range = 0;
	card->degrade_uhs2_half = 0;
	card->degrade_uhs2_legacy = 0;
	card->degrade_final = 0;
	card->degrade_freq_level = 0;

	/* below item is used for thremal control */
	card->thermal_enable = 0;
	card->thermal_uhs2_range = 0;
	card->thermal_uhs2_half_dis = 0;
	card->thermal_uhs2_lpm = 0;
	card->thermal_access_mode = 0;
	card->thermal_power_limit = 0;

	card->continue_init_fail_cnt = 0;
	card->continue_rw_err_cnt = 0;
	card->adma_err_cnt = 0;

}

/*
 * Function Name: card_stuct_init
 *
 * Abstract:
 *
 *          1. init card control info, such as degrade info
 *          2. bind card to host and memset function
 *
 * Input:
 *
 * bht_dev_ext_t*	pdev_ext: Pointer to the device structure
 *
 * Output: None
 *
 * Return value: None
 *
 * Notes:
 *
 * Caller: req_global_init
 *
 */

void card_stuct_init(bht_dev_ext_t *pdev_ext)
{
	sd_card_t *card;
	sd_host_t *host;

	/* Support 1 virtual card so far. */
	card = &(pdev_ext->card);

	/* 1. Zero the card structure */
	os_memset(card, 0, sizeof(sd_card_t));

	/* 2. set the host point of card */
	card->host = &(pdev_ext->host);
	host = card->host;

	/* 3. Error Count clear */
	card->adma_err_cnt = 0;
	card->continue_init_fail_cnt = 0;
	card->continue_rw_err_cnt = 0;
	card->restore_tuning_content_fail = 0;
	card->read_signal_block_flag = 0;

	card_degrade_info_init(card, host);
	card->host->output_tuning.auto_phase_flag = FALSE;
	card->retry_output_fail_phase = 0xFF;
}

/*
 * Function Name: card_stuct_uinit
 *
 * Abstract:
 *
 *          1. this function is called by card remvoe and enter pm
 *          2. this function will only clear software  flag
 *
 * Input:
 *
 * sd_card_t *card : Pointer to the card structure
 *
 * Output: None
 *
 * Return value: None
 *
 * Notes:
 *
 * Caller: remove_card_handle
 *
 */

void card_stuct_uinit(sd_card_t *card)
{
	card->initialized_once = FALSE;
	card->card_type = CARD_NONE;
	os_memset(&card->info, 0, sizeof(card->info));
	card->has_built_inf = FALSE;
	card->inf_trans_enable = FALSE;
	card->last_dir = DATA_DIR_NONE;
	card->last_sect = 0;
	os_memset(&card->mmc, 0, sizeof(card->mmc));
	os_memset(&card->uhs2_info, 0, sizeof(card->uhs2_info));
	card->quirk = 0;
	card->quick_init = 0;

	card->adma_err_cnt = 0;
	card->continue_init_fail_cnt = 0;
	card->continue_rw_err_cnt = 0;
	card->restore_tuning_content_fail = 0;
	card->read_signal_block_flag = 0;

	card->state = CARD_STATE_POWEROFF;
	card->write_protected = FALSE;
	card_degrade_info_init(card, card->host);
	card->host->output_tuning.auto_phase_flag = FALSE;
	card->thread_init_card_flag = 0;
	card->retry_output_fail_phase = 0xFF;
}

static inline bool uhs2_support(sd_host_t *host)
{
	bool ret = TRUE;
	/* 1. Host do not support UHSII */
	if (!host->uhs2_supp)
		ret = FALSE;

	/* 1 TODO. correct the check condition */
	/* 2. Configuration settings to disable UHSII function */
	if (host->cfg->card_item.sd_card_mode_dis.dis_sd40_card)
		ret = FALSE;
	return ret;

}

static inline bool emmc_enabled(sd_host_t *host)
{
	/* 1. Configuration settings to disable eMMC function */
	bool ret = FALSE;

	if (host->cfg->card_item.emmc_mode.emmc_enable)
		ret = TRUE;

	return ret;
}

inline bool mmc_disabled(sd_host_t *host)
{
	/* 1. Configuration settings to disable eMMC function */
	bool ret = FALSE;

	if (host->cfg->card_item.mmc_mode_dis.dis_mmc_func)
		ret = TRUE;
	return ret;
}

static void card_variable_init(sd_card_t *card)
{
	card->info.card_ccs = 0;
	card->info.card_s18a = 0;
	card->info.rca = 0;
	card->info.ddr_flag = 0;
	card->info.io_signal_vol = 0;
	os_memset(&card->info.sw_cur_setting, 0, sizeof(sd_sw_func_t));

	card->uhs2_info.dev_id = 0;
	os_memset(&card->uhs2_info.uhs2_setting, 0, sizeof(uhs2_info_t));
	card->mmc.cur_buswidth = EMMC_1Bit_BUSWIDTH;
	card->mmc.cur_hs_type = 0;
}

/*
 * Related register setting and Driver behavior description
 * 1). SD7.0 Card capacibility detection register
 * 0x1e0[29:28] = 2’b11: Enable hardware capability detection interrupt;
 * 0x1e0[25:24] = 2’b11: enable hardware capability detection interrupt status.
 * 0x1e0[17:16]: write 1 to this bit to clear interrupt status.
 *
 */
static bool check_express_card_clkreqn_status(sd_card_t *card)
{
	u32 delay_us = 1;
	u32 delay_ms;
	loop_wait_t wait;
	u32 regval;
	bool ret = FALSE;
	sd_host_t *host = card->host;

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	switch (host->cfg->card_item.sd7_sdmode_switch_control.switch_method_ctrl) {
	case HW_DETEC_HW_SWITCH:
		/* hardware interrupt control */
		DbgInfo(MODULE_SD_HOST, FEATURE_INTR_TRACE, NOT_TO_RAM,
			"before clkreqn_status : %d\n", host->clkreqn_status);
		while (1) {
			if (os_atomic_read(&host->clkreqn_status) == 1) {
				ret = TRUE;
				DbgInfo(MODULE_SD_HOST, FEATURE_INTR_TRACE,
					NOT_TO_RAM,
					"Wait express clkreqn complete status ok\n");
				break;
			} else if (os_atomic_read(&host->clkreqn_status) == 2) {
				ret = FALSE;
				DbgInfo(MODULE_SD_HOST, FEATURE_INTR_TRACE,
					NOT_TO_RAM,
					"Wait express clkreqn timeout\n");
				break;
			}

			if (card->card_present == FALSE) {
				ret = FALSE;
				DbgErr("card is removed\n");
				break;
			}
		}
		DbgInfo(MODULE_SD_HOST, FEATURE_INTR_TRACE, NOT_TO_RAM,
			"after clkreqn_status : %d\n", host->clkreqn_status);
		break;

	case SW_POLL_SW_SWITCH:
	case SW_POLL_SWCTRL_SWITCH:
		/* software control */

		/* set polling tmie fix value 30ms */
		delay_ms = 30;
		DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
			"software control\n");

		util_init_waitloop(host->pdx, delay_ms, delay_us, &wait);
		while (!util_is_timeout(&wait)) {
			if (host_check_lost(host)) {
				ret = TRUE;
				break;
			}

			if (((sdhci_readl(host, 0x1e0) & (0x1)) == 0)) {
				ret = TRUE;
				break;
			}

			if (card->card_present == FALSE) {
				ret = FALSE;
				DbgErr("card is removed\n");
				break;
			}
		}

		break;

	case SW_POLL_INTER_SW_SWITCH:
	case SW_POLL_INTER_SWCRTL_SWITCH:
		/* hardware polling control */
		DbgInfo(MODULE_SD_HOST, FEATURE_INTR_TRACE, NOT_TO_RAM,
			"software polling control\n");
		while (1) {
			if (host_check_lost(host)) {
				ret = TRUE;
				DbgErr("chip lost, already switch to sd7.0\n");
				break;
			}

			regval = sdhci_readl(host, 0x1e0);
			if (regval & (1 << 16)) {

				DbgInfo(MODULE_SD_HOST, FEATURE_INTR_TRACE,
					NOT_TO_RAM,
					"Wait express clkreqn complete status ok\n");
				ret = TRUE;
				sdhci_or16(host, 0x1e2, 0x01);
				break;
			} else if (regval & (1 << 17)) {
				DbgInfo(MODULE_SD_HOST, FEATURE_INTR_TRACE,
					NOT_TO_RAM,
					"Wait express clkreqn timeout\n");
				ret = FALSE;
				sdhci_or16(host, 0x1e2, 0x02);
				break;
			}

			if (card->card_present == FALSE) {
				ret = FALSE;
				break;
				DbgErr("card is removed\n");
			}
		}
		break;

	default:
		DbgErr("no such value!\n");
		break;
	}
	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Exit %s (%d)\n", __func__, ret);

	return ret;
}

static bool Turn_on_vdd2_or_vdd3(sd_card_t *card, bool flag)
{
	sd_host_t *host = card->host;
	u32 regval;
	bool ret = FALSE;

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	regval = sdhci_readl(host, 0x1e0);
	switch (host->cfg->card_item.sd7_sdmode_switch_control.switch_method_ctrl) {
	case HW_DETEC_HW_SWITCH:
		/* set 0x1e0[29:28] = 2’b11, set 0x1e0[25:24] = 2’b11 */
		regval |= (0x33 << 24);
		break;

	case SW_POLL_SW_SWITCH:
	case SW_POLL_SWCTRL_SWITCH:
		/* set 0x1e0[29:28] = 2’b00, set 0x1e0[25:24] = 2’b00 */
		regval &= 0xccffffff;
		break;

	case SW_POLL_INTER_SW_SWITCH:
	case SW_POLL_INTER_SWCRTL_SWITCH:
		/* set 0x1e0[29:28] = 2’b00, set 0x1e0[25:24] = 2’b11 */
		regval |= (0x3 << 24);
		break;

	default:
		DbgErr("Error:no such value in registry sd7_sdmode_switch_control, use default value\n");
		regval |= (0x33 << 24);
		break;
	}
	sdhci_writel(host, 0x1e0, regval);

	/* 1:VDD3 0:VDD2 */
	if ((flag)
	    && (host->cfg->card_item.sd7_sdmode_switch_control.vdd3_control)) {

		/* Turn on vdd3 */
		os_atomic_set(&host->clkreqn_status, 0);
		host_set_vddx_power(host, VDD3, POWER_ON);
		ret = check_express_card_clkreqn_status(card);
		if (!ret) {
			DbgErr("check clkreq failed fater turn on vdd3\n");
			/* Turn off vdd3 */
			host_set_vddx_power(host, VDD3, POWER_OFF);

			/* Turn on vdd2 */
			os_atomic_set(&host->clkreqn_status, 0);
			host_set_vddx_power(host, VDD2, POWER_ON);
			ret = check_express_card_clkreqn_status(card);
			if (!ret) {
				DbgErr("check clkreq failed fater turn on vdd2\n");
				host_set_vddx_power(host, VDD2, POWER_OFF);
			}
		}
	} else {
		/* Turn on vdd2 */
		os_atomic_set(&host->clkreqn_status, 0);
		host_set_vddx_power(host, VDD2, POWER_ON);

		ret = check_express_card_clkreqn_status(card);
		if (!ret) {
			DbgErr("check clkreq failed fater turn on vdd2 derectily\n");
			host_set_vddx_power(host, VDD2, POWER_OFF);
		}
	}
	return ret;
}

bool pcie_mode_init(sd_card_t *card, bool code_flag)
{
	sd_host_t *host = card->host;
	u32 regval;
	bool ret;
	bool host_support_vdd3;

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* set flag that driver in sd_express mode */
	host->sd_express_flag = TRUE;
	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Set sd_express_flag\n");

	regval = sdhci_readl(host, 0x44);
	if (!(regval & (1 << 29)))
		host_support_vdd3 = FALSE;
	else
		host_support_vdd3 = TRUE;

	/* flag 1:for sd cmd code */
	if (code_flag) {

		host_enable_clock(host, FALSE);

		/* stop clk */
		if (shift_bit_func_enable(host)) {
			set_pattern_value(host, 0x34);
			return TRUE;
		}

		if (card->card_support_vdd3 && host_support_vdd3)
			ret = Turn_on_vdd2_or_vdd3(card, TRUE);
		else
			ret = Turn_on_vdd2_or_vdd3(card, FALSE);

	} else {
		/* flag 0 ：for trail run code */
		if (shift_bit_func_enable(host)) {
			set_pattern_value(host, 0x34);
			return TRUE;
		}
		ret = Turn_on_vdd2_or_vdd3(card, host_support_vdd3);
	}

	if (ret) {
		/*
		 * Software: if pcr 0x444[9]=1,
		 * set sd host register 054h[8]=1 to assert express_card_mode
		 */
		regval = pci_readl(host, 0x444);
		if (regval & (1 << 9)) {
			regval = sdhci_readl(host, 0x54);
			regval |= (1 << 8);
			sdhci_writel(host, 0x54, regval);
		}
		return TRUE;

	}

	DbgErr("Exit pcie mode init with FALSE\n");
	host->sd_express_flag = FALSE;
	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Clear sd_express_flag\n");
	return FALSE;
}

bool gg8_get_card_capability_flag(sd_card_t *card, bool check_uhs2_flag)
{
	bool ret;
	bool flag_f8 = FALSE;
	sd_command_t sd_cmd;
	sd_host_t *host = card->host;

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if (check_uhs2_flag) {
		host_init(host);
		card_variable_init(card);
		host_init_400k_clock(host);
		host_internal_clk_setup(host, TRUE);

		/* 1. Power on card */
		if (host_get_vdd1_state(host) == FALSE) {
			os_mdelay(10);
			/* host_set_vdd1_power(host, TRUE, SDHCI_POWER_VDD1_330); */
			if (shift_bit_func_enable(host))
				set_pattern_value(host, 0x11);

			host_set_vddx_power(host, VDD1, POWER_ON);
		}
	}
	/* 1 SD CLK Start */
	host_enable_clock(host, TRUE);

	/* 2 CMD0 */
	ret = card_reset_card(card, &sd_cmd);
	if (!ret) {
		/* Go Idle State command failed. exit directly. */
		DbgErr("Reset Card (CMD0) Failed.\n");
		return FALSE;

	}
	/* 3. Issue send IF condition command (CMD8) */
	if (check_uhs2_flag)
		ret = sd_send_if_cond(card, &sd_cmd, 0x000001AA);
	else
		ret = sd_send_if_cond(card, &sd_cmd, 0x000031AA);


	if (!ret) {
		/* 3.1 Error response */
		if (sd_cmd.err.error_code == ERR_CODE_RESP_ERR ||
		    sd_cmd.err.error_code == ERR_CODE_NO_CARD) {
			DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
				"CMD8 Response Error or no card.\n");
		} else {
			/* 5.2 No Response  (Standard Capacity Card) */
			DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
				"CMD8 No Responser.\n");
		}

		return FALSE;

	} else {
		/* 5.3 Good Response  (High Capacity card) */
		DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
			"CMD8 Good Responser\n");
		flag_f8 = TRUE;
	}

	/* 5.4.1 read R7 */

	/* sd_send_if_cond argument = 0x000031AA check card support pcie */
	if (check_uhs2_flag == FALSE) {
		/* host ask card's PCIe availability */
		if (!(sd_cmd.response[0] & 0x1000))
			card->card_support_pcie = FALSE;
		else
			card->card_support_pcie = TRUE;

		if (sd_cmd.response[0] & 0x2000) {
			/* host ask whether card support VDD3 */
			card->card_support_vdd3 = TRUE;
		} else
			card->card_support_vdd3 = FALSE;

	}

	/* if check_uhs2_flag == true, send ACMD41 to check response bit 29  */
	if (check_uhs2_flag) {
		ret = card_init_ready(card, &sd_cmd, flag_f8);

		if (!ret) {
			DbgErr("Wait for card ready (ACMD41) Failed.\n");
			return FALSE;
		}
	}

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return TRUE;
}

bool gg8_sd70_card_init(sd_card_t *card)
{
	bool ret;
	u32 regval;
	sd_host_t *host = card->host;

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	card->pcie_init_flag = TRUE;

	/* update card status */
	card->card_present = hostven_chk_card_present(host);

	/* check card exist? */
	if (card->card_present == FALSE || card->card_chg)
		return FALSE;


	if (INIT_DELAY & INIT_DELAY_EN_MASK) {
		os_mdelay(INIT_DELAY & INIT_DELAY_CFG_MASK);
		DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
			"%s init delay %d ms\n", __func__,
			(INIT_DELAY & INIT_DELAY_CFG_MASK));
	} else {
		os_mdelay(200);
		DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
			"%s init delay %d ms\n", __func__, 200);
	}

	/* 0 host side init */
	host_init(host);
	card_variable_init(card);
	host_init_400k_clock(host);
	host_internal_clk_setup(host, TRUE);

	/* 1. Power on card */
	if (shift_bit_func_enable(host) &&
		(host->cfg->card_item.sd7_sdmode_switch_control.card_init_flow_select)) {
		os_mdelay(10);

		if (shift_bit_func_enable(host))
			set_pattern_value(host, 0x10);

		host_set_vddx_power(host, VDD1, POWER_ON);
	} else {
		if (host_get_vdd1_state(host) == FALSE) {
			os_mdelay(10);

			if (shift_bit_func_enable(host))
				set_pattern_value(host, 0x10);

			host_set_vddx_power(host, VDD1, POWER_ON);
		}
	}

	regval = pci_readl(host, 0x444);
	if (!(regval & (0x1 << 11))) {
		/* CMD8 */

		/* FALSE no need to send ACMD41 */
		ret = gg8_get_card_capability_flag(card, FALSE);
		if (!ret) {
			DbgErr
			    ("gg8_get_card_capability_flag exit with faile\n");
		}

		regval = pci_readl(host, 0x444);
		if (!(regval & (0x7 << 8))) {
			DbgErr("host not support to switch to sd7.0\n");
			card->pcie_init_flag = FALSE;
			return FALSE;
		}

		if (card->card_support_pcie) {
			ret = pcie_mode_init(card, TRUE);
			if (!ret) {
				DbgErr("pci cmd mode init failed\n");
				card->pcie_init_flag = FALSE;
			} else {
				card->pcie_init_flag = TRUE;
			}
		} else {
			DbgErr("card not support pcie\n");
			card->pcie_init_flag = FALSE;
			return FALSE;
		}
	} else {

		/* trail run */
		ret = pcie_mode_init(card, FALSE);
		if (!ret) {
			DbgErr("pci trail run mode init failed\n");
			card->pcie_init_flag = FALSE;
		} else {
			card->pcie_init_flag = TRUE;
		}
	}

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
	/* Thomas test: call remove here. */
	if (card->pcie_init_flag == TRUE) {
		bht_sd_remove(host->pci_dev.pci_dev);

		return TRUE;
	}

	if (ret) {
		card_init_transfer(card, host);
		card->initialized_once = TRUE;
		card->state = CARD_STATE_WORKING;
		card->continue_init_fail_cnt = 0;
		if (host_wr_protect_pin(host) || card_wr_protect(card))
			card->write_protected = TRUE;
		else
			card->write_protected = FALSE;

		return TRUE;
	} else {
		return FALSE;
	}
}

/*
 * Function Name: card_pcie_support
 *
 * Abstract: check whether the card supports SD7.0 mode
 *
 * Input:
 *
 * sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *
 * Output: None
 *
 * Return value: return TRUE if the card supports SD7.0 mode, otherwise return FALSE
 *
 * Notes:
 *
 * Caller: card_init
 *
 */

bool card_pcie_support(sd_card_t *card)
{
	bool ret = FALSE;
	u32 regval = 0;
	bool host_support_sd70 = FALSE;
	bool sd_cmd_low = FALSE;
	bool registry_support_sd70 = TRUE;
	bool any_switch_case_enable = FALSE;
	sd_host_t *host = card->host;

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if ((sdhci_readl(host, 0x40) & (1 << 20)))
		host_support_sd70 = TRUE;

	if ((pci_readl(host, 0x444) & 0x700))
		any_switch_case_enable = TRUE;

	/* polling PCR 0x448[31]  */
	if ((pci_readl(host, 0x448) & (1 << 31))) {
		regval = pci_readl(host, 0x448);
		regval |= (1 << 31);
		pci_writel(host, 0x448, regval);
		card->cmd_low_reset_flag = TRUE;
	} else {
		sd_cmd_low = TRUE;
	}

	if (host->cfg->card_item.sd_card_mode_dis.dis_sd70_card)
		registry_support_sd70 = FALSE;

	if (host_support_sd70 && sd_cmd_low && registry_support_sd70
	    && any_switch_case_enable && (card->cmd_low_reset_flag == FALSE)) {
		if ((pci_readl(host, 0x444) & (1 << 10))) {
			if ((pci_readl(host, 0x444) & (1 << 15))) {
				if ((pci_readl(host, 0x50c) & (1 << 6)))
					ret = FALSE;
				else
					ret = TRUE;
			} else {
				if ((pci_readl(host, 0x50c) & (1 << 6)))
					ret = TRUE;
				else
					ret = FALSE;
			}
		} else {
			ret = TRUE;
		}
	} else {
		ret = FALSE;
	}

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Exit %s with(%d)\n", __func__, ret);
	return ret;
}

/*
 * Function Name: card_init
 *
 * Abstract: Main card initialize entry.
 *
 * Input:
 *
 * sd_card_t *card [in] [out]: Pointer to the virtual card structure
 * int retry_num [in]: Retry number if card init failed.
 * bool bfullreset: full reset flag
 *
 * Output: None
 *
 * Return value: Return TRUE if card init successfully, else return FALSE.
 *
 * Notes:
 *
 * Caller: thread_init_card
 *
 */

bool card_init(sd_card_t *card, int retry_num, bool bfullreset)
{
	bool ret = FALSE;
	bool stbl = FALSE;
	sd_host_t *host = card->host;
	bool first_init = TRUE;
	u32 regval;

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if (host->pdx == NULL) {
		DbgErr("host->pdx should not be NULL\n");
		return FALSE;
	}
	if (host_check_lost(host)) {
		DbgErr("Host lost at card init start\n");
		return FALSE;
	}

	if (shift_bit_func_enable(host) &&
		(host->cfg->card_item.sd7_sdmode_switch_control.card_init_flow_select))
		goto retry;
	else
		goto express_flow;

express_flow:
	/* SD7.0 card mode init flow */
	if (host->chip_type == CHIP_GG8) {
		if (card_pcie_support(card)) {
			ret = gg8_sd70_card_init(card);

			if (!ret) {
				regval = pci_readl(host, 0x444);
				regval &= (~(1 << 11));
				pci_writel(host, 0x444, regval);
			}

			if (card->pcie_init_flag == FALSE) {
				if (card->card_present == TRUE) {
					if (shift_bit_func_enable(host)
					    && (host->cfg->card_item.sd7_sdmode_switch_control.card_init_flow_select))
						goto legacy;
					else
						goto retry;
				} else
					return FALSE;
			} else {
				card->card_type = CARD_SD70;
				card->card_present = FALSE;
				return ret;
			}
		} else if (card->card_present == TRUE) {
			if (shift_bit_func_enable(host)
			    && (host->cfg->card_item.sd7_sdmode_switch_control.card_init_flow_select))
				goto legacy;
			else
				goto retry;
		} else {
			return FALSE;
		}
	}

retry:

	/* update card status */
	card->card_present = hostven_chk_card_present(host);
	if (first_init == TRUE) {
		first_init = FALSE;
	} else {

		/* check card exist? */
		if (card->card_present == FALSE || card->card_chg) {
			ret = FALSE;
			goto end;
		}

		if (INIT_DELAY & INIT_DELAY_EN_MASK) {
			os_mdelay(INIT_DELAY & INIT_DELAY_CFG_MASK);
			DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
				"%s init delay %d ms\n", __func__,
				(INIT_DELAY & INIT_DELAY_CFG_MASK));
		} else {
			os_mdelay(200);
			DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
				"%s init delay %d ms\n", __func__, 200);
		}

	}

	/* Do some Host side initialization */
	if (bfullreset == FALSE)
		host_init(host);
	card_variable_init(card);

	/* Check eMMC function enabled or not */
	if (emmc_enabled(host)) {
		hostven_set_pml0_requrest(host, FALSE);
		ret = emmc_init(card, TRUE);
		goto exit;
	}

	if (card_need_get_info(card) == FALSE) {
		switch (card->card_type) {
		case CARD_EMMC:
		case CARD_MMC:
			goto mmc;
		case CARD_SD:
			goto legacy;
		default:
			break;
		}
	}

	/* Check host and configuration support UHSII or not */
	if ((uhs2_support(host))
	    && (card->degrade_uhs2_legacy == 0)
	    && (card->card_type != CARD_MMC && card->card_type != CARD_SD)) {
		u32 clk_value = card_get_uhs2_freq(card);

		DbgErr("host support uhs2\n");
		DbgErr("uhs2 trail run mode\n");

		hostven_set_pml0_requrest(host, TRUE);
		host_uhs2_init(host, clk_value, bfullreset);
		ret = host_uhs2_phychk(host, FALSE, &stbl);

		/* phy init ok */
		if (ret) {
			ret = uhs2_card_init(card);
			if (ret) {
				if (shift_bit_func_enable(host))
					set_pattern_value(host, 0x32);

				goto exit;
			} else if (card->card_type == CARD_SDIO) {
				ret = FALSE;
				goto end;
			}
		}
		/* stbl check failed */
		else if (stbl == FALSE) {
			if (shift_bit_func_enable(host)
			    && (host->cfg->card_item.sd7_sdmode_switch_control.card_init_flow_select)) {
				host_uhs2_clear(host,
					(bool)host->cfg->card_item.test_uhs2_setting2.enable_power_off_vdd1);
				regval = pci_readl(host, 0x444);
				if (regval & (1 << 11)) {
					regval &= (~(1 << 11));
					pci_writel(host, 0x444, regval);
				}
				card_variable_init(card);
				hostven_set_pml0_requrest(host, FALSE);
				goto express_flow;
			}

			if (card->card_type == CARD_UHS2
			    && card->degrade_uhs2_legacy) {
				card->card_type = CARD_NONE;
				card->quick_init = 0;
				card->degrade_freq_level = 0;

				/* If card last stb.l is ok we continue try as UHS2 */
				goto exit;
			}

			host_uhs2_clear(host,
				(bool)host->cfg->card_item.test_uhs2_setting2.enable_power_off_vdd1);
			goto legacy;
		}

		DbgErr("UHS2 init failed\n");
		/* UHS2 init failed case, try again */
		goto exit;
	}

legacy:

	regval = pci_readl(host, 0x444);
	if (regval & (1 << 11)) {
		regval &= (~(1 << 11));
		pci_writel(host, 0x444, regval);
	}

	card_variable_init(card);
	/* Do SD Legacy card initialization */
	if (card->card_type != CARD_MMC) {
		hostven_set_pml0_requrest(host, FALSE);
		ret = sd_legacy_init(card);
		if (card->card_type == CARD_SDIO) {
			ret = FALSE;
			goto end;
		}

		if ((ret == FALSE)
		    && (card->sw_ctrl_swicth_to_express == FALSE)) {
			DbgErr("Legacy SD Init failed\n");
			goto mmc;
		} else
			goto exit;
	}

mmc:
	if ((card->card_type != CARD_SD) && (card->card_type != CARD_UHS2)) {
		if (mmc_disabled(host)) {
			DbgErr("Registry disable MMC card function!!\n");
			goto exit;
		}
		host_poweroff(host, card->card_type);
		host_init(host);
		card_variable_init(card);
		hostven_set_pml0_requrest(host, FALSE);
		ret = emmc_init(card, FALSE);
	}

exit:
	if (ret == TRUE) {
		card_init_transfer(card, host);
		card->initialized_once = TRUE;
		card->state = CARD_STATE_WORKING;
		card->continue_init_fail_cnt = 0;
		if (host_wr_protect_pin(host) || card_wr_protect(card))
			card->write_protected = TRUE;
		else
			card->write_protected = FALSE;

	} else {
		if (card->sw_ctrl_swicth_to_express == TRUE)
			goto end;

		card->continue_init_fail_cnt++;
		retry_num--;
		if ((retry_num == 0) ||
		    (card->card_present == FALSE) ||
		    (card->card_type == CARD_ERROR) || host_check_lost(host)) {
			goto end;
		}

		/* Call degarde policy if try_times >= 4 */
		if (card->continue_init_fail_cnt >= CARD_INIT_DEGARDE_TIME)
			card_degrade_policy(card, NULL);

		/* Need power cycle for retry, etc. */
		if (card->card_type == CARD_UHS2) {
			if (host->cfg->card_item.test_uhs2_setting2.enable_full_reset_reinit) {
				/* If last time not use fullreset, then use fullreset */
				bfullreset = bfullreset ? FALSE : TRUE;
				if (bfullreset) {
					uhs2_full_reset_card(card);
					DbgErr
					    ("Card Init failed do fullreset retry\n");
					goto retry;
				}
			}
		}

		host_poweroff(host, CARD_NONE);
		card->state = CARD_STATE_POWEROFF;
		DbgErr("Card Init failed do poweroff retry\n");
		goto retry;
	}

end:
	if (ret == FALSE) {
		host_poweroff(host, CARD_NONE);
		card->state = CARD_STATE_POWEROFF;
		if ((card->degrade_final) ||
		    (card->card_type == CARD_NONE
		     && card->continue_init_fail_cnt >= 5)) {
			DbgErr("Card finally Init failed\n");
			card->card_type = CARD_ERROR;
		}
		card->quick_init = 0;
	}
	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Exit %s ret=%d\n", __func__, ret);
	return ret;
}

bool card_init_stage2(sd_card_t *card)
{
	bool ret = FALSE;

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	switch (card->card_type) {
	case CARD_SD:
		ret = sd_init_stage2(card);
		break;
	case CARD_UHS2:
		ret = uhs2_init_stage2(card);
		break;
	case CARD_MMC:
	case CARD_EMMC:
		ret = emmc_init_stage2(card);
		break;
	default:

		break;
	}

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_INIT, NOT_TO_RAM,
		"Exit %s ret=%d\n", __func__, ret);
	return ret;
}

/*
 * Function Name: card_power_off
 *
 * Abstract: This function is used to set card to power off status
 *           1. Resume from Sleep mode if necessary
 *           2. Stop Infintie transfer if necessary
 *           3. Poweroff Card
 *
 * Input:
 *
 * sd_card_t *card [in] [out]: Pointer to the virtual card structure
 * bool	directly: If true means do card poweroff directly(often use at error case)
 *
 * Output: None
 *
 * Return value: None
 *
 * Notes:
 *
 * Caller: card_enter_sleep
 *
 */

void card_power_off(sd_card_t *card, bool directly)
{
	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_OPS, NOT_TO_RAM,
		"Enter %s directy=%d\n", __func__, directly);

	if (directly)
		goto next;

	/* If wake up failed than goto poweroff directly */
	if (card_resume_sleep(card, FALSE) == FALSE)
		goto next;

	if (card_stop_infinite(card, FALSE, NULL) == FALSE)
		goto next;
	else {
		/* go dormant for UHSII D3-hot */
		card_enter_sleep(card, FALSE, TRUE);
	}

next:
	if (card->state != CARD_STATE_POWEROFF)
		host_poweroff(card->host, card->card_type);
	card->state = CARD_STATE_POWEROFF;
	card->thread_init_card_flag = 0;
	card->has_built_inf = FALSE;
	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_OPS, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

/*
 * Function Name: card_thermal_control
 *
 * Abstract: This Function is used to do card thremal control, only for SD
 *
 * Input:
 *
 * sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *
 * Output: None
 *
 * Return value: TRUE means ok, others means error, caller need do error recovery
 *
 * Notes: run in thread context
 *
 * Caller: func_thermal_control
 *
 */

bool card_thermal_control(sd_card_t *card)
{
	bool ret = FALSE;

	DbgInfo(MODULE_ALL_CARD, FEATURE_FUNC_THERMAL, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if (card->card_present == FALSE)
		goto exit;

	if (card->card_type == CARD_SD)
		ret = sd_thermal_control(card);

exit:
	DbgInfo(MODULE_ALL_CARD, FEATURE_FUNC_THERMAL, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return ret;
}

bool card_stop_infinite(sd_card_t *card, bool recover, sd_command_t *pcmd)
{
	bool ret = TRUE;
	sd_command_t sd_cmd;
	sd_command_t *cmd = (pcmd == NULL) ? &sd_cmd : pcmd;

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_OPS, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if (card->card_present == FALSE || card->has_built_inf == FALSE)
		goto exit;

	ret = card_send_command12(card, cmd);
	if (ret == FALSE && recover) {
		DbgErr("Stop Inf failed for cmd12\n");
		ret = card_rw_recovery(card, cmd);
		if (ret == FALSE)
			goto exit;
	}

	if (ret == TRUE)
		ret = card_check_rw_ready(card, cmd, 150);

exit:

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_OPS, NOT_TO_RAM,
		"Exit %s ret=%d\n", __func__, ret);
	return ret;

}

bool card_enter_sleep(sd_card_t *card, bool recover, bool deepslp)
{
	bool ret = TRUE;
	sd_command_t sd_cmd;

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_OPS, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if (card->card_type == CARD_UHS2) {
		if (deepslp && card->uhs2_info.uhs2_cap.hibernate == 0)
			deepslp = FALSE;
		ret = card_stop_infinite(card, recover, &sd_cmd);
		if (ret == FALSE)
			goto exit;

		ret = uhs2_enter_dmt(card, &sd_cmd, card->host, deepslp);

		if (ret == TRUE) {
			card->state =
			    deepslp ? CARD_STATE_DEEP_SLEEP : CARD_STATE_SLEEP;
		}
	}

exit:
	if (ret == FALSE) {
		DbgErr("enter sleep failed\n");
		card_power_off(card, TRUE);
	}

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_OPS, NOT_TO_RAM,
		"Exit %s ret=%d\n", __func__, ret);
	return ret;

}

bool card_resume_sleep(sd_card_t *card, bool recover)
{
	bool ret = TRUE;
	bool deepslp = FALSE;
	sd_command_t sd_cmd;

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_OPS, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if (card->state != CARD_STATE_DEEP_SLEEP
	    && card->state != CARD_STATE_SLEEP)
		goto exit;

	if (card->card_type == CARD_UHS2) {
		deepslp = (card->state == CARD_STATE_DEEP_SLEEP) ? TRUE : FALSE;
		ret = uhs2_resume_dmt(card, &sd_cmd, card->host, deepslp);

		if (ret == TRUE)
			card->state = CARD_STATE_WORKING;
	}

exit:
	if (ret == FALSE) {
		DbgErr("resume sleep failed\n");
		if (recover)
			card_power_off(card, TRUE);
	}

	DbgInfo(MODULE_ALL_CARD, FEATURE_CARD_OPS, NOT_TO_RAM,
		"Exit %s ret=%d\n", __func__, ret);
	return ret;
}

bool card_piorw_data(sd_card_t *card, u32 sec_addr, u32 sec_cnt,
		     e_data_dir dir, byte *data)
{
	bool ret = FALSE;

	sd_command_t sd_cmd;
	sd_host_t *host = card->host;
	u8 cmd_index = 0;
	u32 cmd_flag = CMD_FLG_R1 | CMD_FLG_RESCHK;
	cfg_item_t *cfg = NULL;

	DbgInfo(MODULE_ALL_CARD, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Enter %s dir=%d seccnt=0x%08X secaddr=0x%08X\n", __func__,
		dir, sec_cnt, sec_addr);

	X_ASSERT(host != NULL);

	cfg = host->cfg;
	X_ASSERT(cfg != NULL);

	if (data == NULL)
		goto exit;

	if (sec_cnt > 1)
		cmd_flag |= CMD_FLG_MULDATA;

	if (cmd_flag & CMD_FLG_MULDATA) {
		if (dir == DATA_DIR_OUT)
			cmd_index = SD_CMD25;
		else
			cmd_index = SD_CMD18;
	} else {
		if (dir == DATA_DIR_OUT)
			cmd_index = SD_CMD24;
		else
			cmd_index = SD_CMD17;
	}

	cmd_set_auto_cmd_flag(card, &cmd_flag);
	ret =
	    card_send_sdcmd(card, &sd_cmd, cmd_index, sec_addr, cmd_flag, dir,
			    data, sec_cnt * 512);
	/* todo error recovery and cmd13 */

exit:
	if (ret == FALSE)
		DbgErr("Card Pio dir=%d seccnt=0x%08X secaddr=0x%08X failed\n",
		       dir, sec_cnt, sec_addr);
	DbgInfo(MODULE_ALL_CARD, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Exit %s ret=%d\n", __func__, ret);
	return ret;
}

static void card_cmd_copy(sd_command_t *dst, sd_command_t *src)
{
	sd_data_t *data = dst->data;

	os_memcpy(dst, src, sizeof(sd_command_t));
	dst->data = data;

	if (data && src->data)
		os_memcpy(dst->data, src->data, sizeof(sd_data_t));
	else
		src->data = NULL;
}

bool card_dma_rw_data(sd_card_t *card, u32 dma_mode, u32 sec_addr, u32 sec_cnt,
		      e_data_dir dir, byte *data, sg_list_t *sglist,
		      u32 sg_len, sd_command_t *cmd_err)
{
	bool ret = FALSE;

	sd_command_t sd_cmd;
	sd_data_t sd_data;
	sd_host_t *host = card->host;
	u8 cmd_index = 0;
	u32 cmd_flag = CMD_FLG_R1 | CMD_FLG_RESCHK;
	cfg_item_t *cfg = NULL;

	DbgInfo(MODULE_ALL_CARD, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Enter %s dir=%d seccnt=0x%08X secaddr=0x%08X\n", __func__,
		dir, sec_cnt, sec_addr);

	cfg = host->cfg;

	if (data == NULL && dma_mode != CFG_TRANS_MODE_ADMA2) {
		DbgErr("%s argument wrong\n", __func__);
		goto end;
	}

	if (sec_cnt > 1)
		cmd_flag |= CMD_FLG_MULDATA;

	if (cmd_flag & CMD_FLG_MULDATA) {
		if (dir == DATA_DIR_OUT)
			cmd_index = SD_CMD25;
		else
			cmd_index = SD_CMD18;
	} else {
		if (dir == DATA_DIR_OUT)
			cmd_index = SD_CMD24;
		else
			cmd_index = SD_CMD17;
	}

	cmd_set_auto_cmd_flag(card, &cmd_flag);
	/* set dma mode */
	if (dma_mode == CFG_TRANS_MODE_SDMA) {
		/* host_dma_select(card->host, TRANS_SDMA); */
		cmd_flag |= CMD_FLG_SDMA;
		if ((card->card_type != CARD_UHS2) &&
		    (cmd_flag & CMD_FLG_AUTO23)) {
			/* SDMA don't use auto CMD23 */
			cmd_flag &= ~CMD_FLG_AUTO23;
			cmd_flag |= CMD_FLG_AUTO12;
		}
	} else if (dma_mode == CFG_TRANS_MODE_ADMA2) {
		cmd_flag |= CMD_FLG_ADMA2;
	} else {
		/* host_dma_select(card->host, TRANS_ADMA2); */
		cmd_flag |= CMD_FLG_ADMA_SDMA;
	}

	os_memset(&sd_data, 0, sizeof(sd_data_t));
	ret =
	    build_dma_ctx(card->host->pdx, &sd_data, cmd_flag, dir, data,
			  sec_cnt * 512, sglist, sg_len);
	if (ret == FALSE) {
		DbgErr("build dma ctx failed\n");
		goto end;
	}

	ret =
	    card_send_sdcmd_dma_timeout(card, &sd_cmd, &sd_data, cmd_index,
					sec_addr, cmd_flag, dir, data,
					sec_cnt * 512, 0);

	if (ret == FALSE && cmd_err != NULL)
		card_cmd_copy(cmd_err, &sd_cmd);

end:
	if (ret == FALSE) {
		DbgErr("Card dma dir=%d seccnt=0x%08X secaddr=0x%08X failed\n",
		       dir, sec_cnt, sec_addr);
	}

	DbgInfo(MODULE_ALL_CARD, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Exit %s ret=%d\n", __func__, ret);
	return ret;
}

/*
 * Currently this function is used for dump mode only,
 * todo to let it support normal case(Add node2 init code for normal case)
 */
bool card_adma2_rw_inf(sd_card_t *card, u32 sec_addr, u32 sec_cnt,
		       e_data_dir dir, sg_list_t *sglist, u32 sg_len,
		       sd_command_t *cmd_err)
{
	u32 flg = 0;
	bool ret = FALSE;
	sd_command_t sd_cmd;
	dma_desc_buf_t *pdma = 0;
	bht_dev_ext_t *pdx = card->host->pdx;
	node_t *node = NULL;
	sd_data_t sd_data;
	bool data_26bit_len =
		pdx->cfg->host_item.test_dma_mode_setting.enable_dma_26bit_len ?
		TRUE : FALSE;
	DbgInfo(MODULE_ALL_CARD, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Enter %s dir=%d seccnt=0x%08X secaddr=0x%08X\n", __func__,
		dir, sec_cnt, sec_addr);

	/* Step1 Check can use Infinte or not */
	flg = cmd_can_use_inf(card, dir, sec_addr, sec_cnt);

	/* not continue case stop infinite first */
	if (card->has_built_inf && (flg != CMD_FLG_INF_CON)) {
		ret = card_stop_infinite(card, FALSE, &sd_cmd);
		if (ret == FALSE) {
			DbgErr("%s stop infinite failed\n", __func__);
			goto exit;
		}
	}

	/* Non Infinte Case */
	if (flg == 0) {
		ret =
		    card_dma_rw_data(card, CFG_TRANS_MODE_ADMA2, sec_addr,
				     sec_cnt, dir, NULL, sglist, sg_len,
				     cmd_err);
		goto end;
	}

	/* Step2 Build Infinte sd_cmd */
	node =
	    (pdx->dma_api.cur_node !=
	    &pdx->dma_api.dma_node) ? &pdx->dma_api.dma_node :
		&pdx->dma_api.dma_node2;
	os_memset(&sd_cmd, 0, sizeof(sd_command_t));
	pdx->dma_api.cur_node = node;
	if (dir == DATA_DIR_IN)
		sd_cmd.cmd_index = SD_CMD18;
	else
		sd_cmd.cmd_index = SD_CMD25;

	sd_cmd.argument = sec_addr;
	sd_cmd.cmd_flag |=
	    CMD_FLG_R1 | CMD_FLG_RESCHK | CMD_FLG_MULDATA | CMD_FLG_ADMA2 | flg;
	sd_cmd.sd_cmd = 1;

	/* Step3 alloc dma desc buf */
	pdma = node_get_desc_res(node, MAX_ADMA2_TABLE_LEN);
	if (pdma == NULL) {
		DbgErr("%s get desc res failed\n", __func__);
		ret = FALSE;
		goto exit;
	}

	node->phy_node_buffer.head = *pdma;
	node->phy_node_buffer.end =
	    build_adma2_desc(sglist, sg_len, (byte *) pdma->va, pdma->len,
			     card->host->bit64_enable, data_26bit_len);

	if (node->phy_node_buffer.end.va == NULL) {
		DbgErr("%s prepare dma buffer failed\n", __func__);
		ret = FALSE;
		goto exit;
	}

	if (flg & CMD_FLG_INF_CON)
		update_adma2_inf_tb(node->phy_node_buffer.end.va,
				    &(pdx->dma_api.adma2_inf_link_addr),
				    &node->phy_node_buffer.head.pa,
				    card->host->bit64_enable);
	else
		update_adma2_inf_tb(node->phy_node_buffer.end.va,
				    &(pdx->dma_api.adma2_inf_link_addr), NULL,
				    card->host->bit64_enable);

	/* Step4 Send Command12 */
	sd_cmd.data = &sd_data;
	sd_cmd.data->data_mng.driver_buff = NULL;
	sd_cmd.data->data_mng.offset = sd_cmd.data->data_mng.srb_cnt = 0;
	sd_cmd.data->dir = dir;
	sd_cmd.data->data_mng.total_bytess = sec_cnt * SD_BLOCK_LEN;
	sd_cmd.data->data_mng.sys_addr = node->general_desc_tbl.pa;

	cmd_generate_reg(card, &sd_cmd);
	/* 4.issue cmd */
	ret = cmd_execute_sync(card, &sd_cmd, NULL);

exit:
	if (ret == FALSE && cmd_err != NULL)
		card_cmd_copy(cmd_err, &sd_cmd);

end:
	DbgInfo(MODULE_ALL_CARD, FEATURE_RW_TRACE, NOT_TO_RAM,
		"Exit %s ret=%d\n", __func__, ret);
	return ret;

}

/*
 * Function Name: card_recovery_flow
 *
 * Abstract: This Function is used to do card rw error recovery flow
 *
 * Input:
 *
 * sd_card_t *card: The Command will send to which Card
 * sd_command_t *sd_cmd: if the init occurred at init stage this parameter will be null
 *
 * Output: None
 *
 * Return value:
 *              REQ_RESULT_NO_CARD: card not exist or not card
 *              REQ_RESULT_ACCESS_ERR: card rw recovery failed
 *              REQ_RESULT_OK: no error
 *
 * Notes: This function is called in thread context to do RW Error Recovery
 *
 * Caller: tag_queue_rw_data_issue_stage
 *
 */

e_req_result card_recovery_flow(sd_card_t *card, sd_command_t *sd_cmd)
{
	e_req_result result = REQ_RESULT_ACCESS_ERR;

	DbgInfo(MODULE_ALL_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Enter %s\n", __func__);

	os_mdelay(50);
	if (card->card_present == FALSE || card->card_chg
	    || host_check_lost(card->host) || card->sw_ctrl_swicth_to_express) {
		DbgErr("Error Recover for no card\n");
		result = REQ_RESULT_NO_CARD;
		goto exit;
	}

	card->continue_rw_err_cnt++;

	/* If Adma Error */
	if (cmd_is_adma_error(sd_cmd)) {
		DbgInfo(MODULE_ALL_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
			"Adma error\n", __func__);
		card->adma_err_cnt++;
		if (card->adma_err_cnt >= 3) {
			DbgInfo(MODULE_ALL_CARD, FEATURE_ERROR_RECOVER,
				NOT_TO_RAM, "continue adma err>=3\n",
				__func__);
			card_degrade_policy(card, sd_cmd);
			card->continue_rw_err_cnt = 0;
			card->adma_err_cnt = 0;
			/* card->thread_init_card_flag = 0; */
			card_power_off(card, TRUE);
			if (card_init(card, 1, FALSE) == FALSE) {
				if (card->card_type == CARD_ERROR) {
					DbgErr("Adma error recover fatal\n");
					result = REQ_RESULT_NO_CARD;
				} else {
					DbgErr("Adma error recover failed\n");
					result = REQ_RESULT_ACCESS_ERR;
				}
			} else
				result = REQ_RESULT_OK;
			goto exit;
		}
	}

	if (card_rw_recovery(card, sd_cmd) == FALSE) {
		card->continue_rw_err_cnt++;
		if (card->continue_rw_err_cnt >= 3) {
			DbgInfo(MODULE_ALL_CARD, FEATURE_ERROR_RECOVER,
				NOT_TO_RAM, "continue rw err>=3\n",
				__func__);
			card_degrade_policy(card, sd_cmd);
			card->continue_rw_err_cnt = 0;
			card->adma_err_cnt = 0;
		}

		card_power_off(card, TRUE);
		if (card_init(card, 1, FALSE) == FALSE) {
			if (card->card_type == CARD_ERROR) {
				DbgErr(" error recover fatal\n");
				result = REQ_RESULT_NO_CARD;
			} else {
				DbgErr("error recover failed\n");
				result = REQ_RESULT_ACCESS_ERR;
			}
		} else
			result = REQ_RESULT_OK;
	} else
		result = REQ_RESULT_OK;
exit:
	DbgInfo(MODULE_ALL_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "EXIT %s\n",
		__func__);
	return result;
}

bool card_set_blkcnt(sd_card_t *card, sd_command_t *sd_cmd, u32 blkcnt)
{
	bool ret = FALSE;

	DbgInfo(MODULE_LEGACY_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Enter %s blkcnt=%d\n", __func__, blkcnt);
	ret =
	    card_send_sdcmd(card, sd_cmd, SD_CMD23, blkcnt,
			    CMD_FLG_R1 | CMD_FLG_RESCHK, DATA_DIR_NONE, NULL,
			    0);
	if (ret == FALSE)
		DbgErr("issue cmd23 failed\n");

	DbgInfo(MODULE_LEGACY_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Exit %s ret=%d\n", __func__, ret);

	return ret;
}

/*
 * Function Name: card_is_poweroff
 *
 * Abstract: This Function is used to get card power state
 *
 * Input:
 *
 * sd_card_t *card : The target Card
 *
 * Output: None
 *
 * Return value:
 *              TRUE: card poweroff
 *              FALSE: card doesn't poweroff
 *
 * Notes:
 *
 * Caller: tag_queue_rw_data_issue_stage
 *
 */

bool card_is_poweroff(sd_card_t *card)
{
	if (card->state == CARD_STATE_POWEROFF)
		return TRUE;
	else
		return FALSE;
}

/*
 * Function Name: card_read_csd
 *
 * Abstract: De-select the card and send CMD9, and then select the card.
 *
 * Input:
 *
 * sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *
 * Output:
 *
 * byte *data: used for storing CSD data
 *
 * Return value:
 *              TRUE: read CSD successfully
 *              FALSE: occur error when read CSD
 *
 * Notes:
 *
 * Caller: thread_gen_io
 *
 */

bool card_read_csd(sd_card_t *card, byte *data)
{

	sd_command_t sd_cmd;

	os_memset(&sd_cmd, 0, sizeof(sd_command_t));

	return sd_read_csd(card, &sd_cmd, data);

}

/*
 * Function Name: card_program_csd
 *
 * Abstract: Program CSD by CMD27
 *
 * Input:
 *
 * sd_card_t *card [in] [out]: Pointer to the virtual card structure
 *
 * Output:
 *
 * byte *data: used for storing CSD data
 *
 * Return value: return TRUE if program CSD successfully, else return FALSE
 *
 * Notes:
 *
 * Caller: thread_gen_io
 *
 */

bool card_program_csd(sd_card_t *card, byte *data)
{

	sd_command_t sd_cmd;

	os_memset(&sd_cmd, 0, sizeof(sd_command_t));
	return sd_program_csd(card, &sd_cmd, data);

}
