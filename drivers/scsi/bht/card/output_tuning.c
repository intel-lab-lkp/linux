// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022 BHT Inc.
 *
 * File Name: output_tuning.c
 *
 * Abstract:
 *      1. Card tuning main entry
 *      2. Interface for card tuning
 *
 * Version: 1.00
 *
 * Author: Chevron
 *
 * Environment:	OS Independent
 *
 * History:
 *
 * 3/8/2022		Creation	Chevron
 */

#include "../include/basic.h"
#include "../include/card.h"
#include "../include/cardapi.h"
#include "cardcommon.h"
#include "../include/hostapi.h"
#include "../include/transhapi.h"
#include "../include/hostvenapi.h"
#include "../include/util.h"
#include "../include/debug.h"
#include "../include/cmdhandler.h"
#include "../host/hostven.h"
#include  "../include/card.h"
#include "../host/hostreg.h"

/* None Device error */
#define		SD_SUCCESS				0x00000000
/* Device error */
#define		SD_ERR_DEVICE			0x00000001
/*mmio value set timeout */
#define		SD_ERR_MMIO_SET_TIMEOUT	0x00000002
#define		SD_ERR_ALL_PHASE_PASS	0x00000003
#define		SD_ERR_FATAL			0x00000004
/* Device error need degrade */
#define		SD_ERR_DEVICE_DEGRADE	0x00000005
/* Device error need retry */
#define		SD_ERR_DEVICE_RETRY		0x00000006
/* Device error need increase drive strength */
#define		SD_ERR_DEVICE_DS_INS	0x00000007
/* Retry Over */
#define    SD_ERR_RETRY_OVER        0x80000000
/* CRC Error */
#define    SD_ERR_CRC_MISSMACH      0x40000000
/* No Response */
#define    SD_ERR_NO_RESPONSE       0x20000000
/* No Response */
#define    SD_ERR_NO_RESPONSE       0x20000000

u16 tuning_phase_result(sd_card_t *card)
{
	u16 result[4] = { 0 };
	u8 phase_count = 11;
	u16 phase_mask = 0x7FF;
	/* u32 device_status; */
	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Enter %s\n",
		__func__);
	/* Only GG8 support 14 phase tuning */
	if (card->host->chip_type == CHIP_GG8
	    || card->host->chip_type == CHIP_ALBATROSS) {
		phase_count = 14;
		phase_mask = 0x3FFF;
	}

	/* get tuning result of 3 cycle */
	result[0] =
	    sdhci_readl(card->host, SDHCI_SAMPLE_CLK_RESULT_LOW) & phase_mask;
	result[1] =
	    (sdhci_readl(card->host, SDHCI_SAMPLE_CLK_RESULT_LOW) >>
	     phase_count) & phase_mask;

	/* Low bits result */
	result[2] =
	    ((sdhci_readl(card->host, SDHCI_SAMPLE_CLK_RESULT_LOW) >>
	      (phase_count << 1) & phase_mask));

	/* Result of full bits */
	if (card->host->chip_type == CHIP_GG8
	    || card->host->chip_type == CHIP_ALBATROSS)
		result[2] |=
		    (sdhci_readl(card->host, SDHCI_SAMPLE_CLK_RESULT_UP) &
		     0x3FF) << 4;
	else
		result[2] |=
		    (sdhci_readl(card->host, SDHCI_SAMPLE_CLK_RESULT_UP) &
		     0x3FF) << 1;

	result[3] = result[0] & result[1] & result[2];

	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Exit %s, result 0x%x, phase count %d\n", __func__,
		result[3], phase_count);
	return result[3];
}

u8 select_tuning_phase(u16 tuning_phase, u8 phase_cnt)
{
	u8 temp[14] = { 0 };
	u8 cnt[14] = { 0 };
	u8 sel_phase, val, pos, start_phase;
	u8 i, j;

	i = j = val = pos = sel_phase = 0;

	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Enter %s\n",
		__func__);
	for (i = 0; i < phase_cnt; i++)
		temp[i] = (tuning_phase >> i) & 0x01;

	for (i = 0; i < phase_cnt; i++) {
		for (j = 0; j < phase_cnt; j++) {
			if (temp[(i + j) % phase_cnt])
				cnt[i]++;
			else
				break;
		}
	}

	val = cnt[0];
	for (i = 0; i < phase_cnt - 1; i++) {
		if (cnt[i + 1] > val) {
			val = cnt[i + 1];
			pos = i + 1;
		}
	}

	start_phase = (phase_cnt == 14 ? 9 : 8);
	sel_phase = ((start_phase + pos + cnt[pos] / 2) % phase_cnt);

	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Exit %s select phase %d\n", __func__, sel_phase);
	return sel_phase;
}

void set_input_tuning_phase(sd_card_t *card, u8 sel_phase)
{
	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Enter %s\n",
		__func__);

	host_enable_clock(card->host, FALSE);

	/* Clear origin phase */
	sdhci_and32(card->host, SDHCI_DLL_PHASE_CFG, ~0x1F000000);
	/* select the 1B0h[27:24] to config the phase selection */
	sdhci_or32(card->host, SDHCI_DLL_PHASE_CFG, BIT28);
	/* set new phase */
	sdhci_or32(card->host, SDHCI_DLL_PHASE_CFG, sel_phase << 24);

	host_enable_clock(card->host, TRUE);
	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Exit %s select phase %d\n", __func__, sel_phase);
}

void generate_traverse_range(sd_card_t *card, u8 center_point, u8 offset,
			     u8 *start_point, u8 *end_point)
{
	u16 phase_mask_all_pass = 0;
	u8 phase_cnt = 11;

	if (card->host->chip_type == CHIP_GG8
	    || card->host->chip_type == CHIP_ALBATROSS)
		phase_cnt = 14;

	if (phase_cnt == 14)
		phase_mask_all_pass = 0x3FFF;
	else
		phase_mask_all_pass = 0x7FF;

	if (center_point < offset) {
		*start_point = phase_cnt + center_point - offset;
		*end_point = phase_cnt + center_point + offset;
	} else {
		*start_point = center_point - offset;
		*end_point = center_point + offset;
	}
}

u8 get_output_fix_phase(sd_card_t *card)
{
	cfg_output_tuning_item_t *output_tuning =
	    &card->host->cfg->feature_item.output_tuning_item;

	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if (card->info.sw_cur_setting.sd_access_mode == SD_FNC_AM_DDR200) {
		DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
			"output_tuning_item DDR200\n");
		return (u8) output_tuning->fixed_value_ddr200;
	} else if (card->info.sw_cur_setting.sd_access_mode == SD_FNC_AM_SDR104) {
		DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
			"output_tuning_item SDR104\n");
		return (u8) output_tuning->fixed_value_sdr104;
	} else if (card->info.sw_cur_setting.sd_access_mode == SD_FNC_AM_SDR50) {
		DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
			"output_tuning_item SDR50\n");
		return (u8) output_tuning->fixed_value_sdr50;
	} else {
		/* Not support this mode at Bayhub Driver */
		DbgErr("sd_access_mode %d isn't supported !!!",
		       card->info.sw_cur_setting.sd_access_mode);
		return 0;
	}

}

/* Use to find a output phase in which input tuning result is not all pass */
u32 find_input_phase_fail_point(sd_card_t *card, u8 *output_phase,
				u16 *input_tuning_result)
{
	u32 result = 0;
	u8 start_phase, end_phase, index_phase;
	u8 output_fix_phase = 0;
	u8 i = 0;
	u16 phase_mask_all_pass = 0;
	int ret = 0;
	sd_command_t sd_cmd;
	u8 phase_cnt = 11;

	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Enter %s\n",
		__func__);
	if (card->host->chip_type == CHIP_GG8
	    || card->host->chip_type == CHIP_ALBATROSS)
		phase_cnt = 14;

	output_fix_phase = get_output_fix_phase(card);

	if (phase_cnt == 14)
		phase_mask_all_pass = 0x3FFF;
	else
		phase_mask_all_pass = 0x7FF;
	generate_traverse_range(card, output_fix_phase, 3, &start_phase,
				&end_phase);

	for (i = start_phase; i <= end_phase; i++) {
		index_phase = i % phase_cnt;

		host_set_output_tuning_phase(card->host, index_phase);

		ret = sd_tuning(card, &sd_cmd, 150);
		if (!ret && sd_cmd.err.error_code) {
			DbgErr("Uncorrect fix output phase!!!\n");
			result = SD_ERR_FATAL;
			break;
		} else if (!ret) {
			result = SD_ERR_DEVICE_DEGRADE;
			break;
		}

		*input_tuning_result = tuning_phase_result(card);

		DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
			"output phase is %d, input result is 0x%x\n",
			index_phase, *input_tuning_result);
		if (*input_tuning_result == phase_mask_all_pass) {
			result = SD_ERR_ALL_PHASE_PASS;
			continue;
		} else {
			*output_phase = index_phase;
			result = SD_SUCCESS;
			break;
		}
	}
	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return result;
}

u32 generate_output_input_phase_pair(sd_card_t *card, u8 input_fix_phase,
				     u8 ddr200)
{
	u32 result = 0;
	u8 output_phase = 0;
	u8 input_phase = 0;
	u16 input_tuning_result = 0;
	u8 output_fix_phase = 0;
	u8 start_phase, end_phase, index_phase, offset;
	u8 i = 0;
	u8 phase_cnt = 11;
	u16 phase_mask_all_pass = 0x7FF;
	sd_command_t sd_cmd;
	int ret = 0;

	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Enter %s\n",
		__func__);
	if (card->host->chip_type == CHIP_GG8
	    || card->host->chip_type == CHIP_ALBATROSS) {
		phase_cnt = 14;
		phase_mask_all_pass = 0x3FFF;
	}
	if (ddr200) {
		output_fix_phase =
		(u8) card->host->cfg->feature_item.output_tuning_item.fixed_value_sdr104;
		offset = 2;
	} else {
		output_fix_phase = get_output_fix_phase(card);
		offset = 3;
	}
	generate_traverse_range(card, output_fix_phase, offset, &start_phase,
				&end_phase);

	for (i = start_phase; i <= end_phase; i++) {
		index_phase = i % phase_cnt;

		host_set_output_tuning_phase(card->host, index_phase);

		ret = sd_tuning(card, &sd_cmd, 150);
		if (!ret && sd_cmd.err.error_code) {
			DbgErr("Uncorrect fix output phase!!!\n");
			result = SD_ERR_FATAL;
			break;
		} else if (!ret) {
			result = SD_ERR_DEVICE_DEGRADE;
			break;
		}

		input_tuning_result = tuning_phase_result(card);

		DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
			"output phase is %d, input result is 0x%x\n",
			index_phase, input_tuning_result);
		if (ddr200) {
			card->output_input_phase_pair[index_phase] =
			    select_tuning_phase(input_tuning_result,
						phase_cnt);
			continue;
		}

		if (input_tuning_result == phase_mask_all_pass) {
			result = SD_ERR_ALL_PHASE_PASS;
			continue;
		} else {
			output_phase = index_phase;
			result = SD_SUCCESS;
			break;
		}
	}

	/* result = find_input_phase_fail_point(card, &output_phase, &input_tuning_result); */
	if (ret && ddr200) {
		u8 temp_phase_pair[14] = { 0 };
		u8 j;

		/* add all input phase */
		for (i = start_phase; i <= end_phase; i++) {

			input_phase =
			    card->output_input_phase_pair[i % phase_cnt];
			/* caclute the <output,input> phase pair */
			for (j = i; j < (i + phase_cnt); j++) {
				index_phase = j % phase_cnt;
				temp_phase_pair[index_phase] += input_phase;
				DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER,
					NOT_TO_RAM,
					"caclute ddr output-input pair:0x%x-0x%x\n",
					index_phase, input_phase);
				input_phase = (input_phase + 1) % phase_cnt;
			}
		}

		/* caclute the avargae value for other <output,input> phase pair */
		for (i = end_phase + 1; i < (start_phase + phase_cnt); i++) {
			index_phase = i % phase_cnt;
			card->output_input_phase_pair[index_phase] =
			    temp_phase_pair[index_phase] / (1 + (offset << 1));
			DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER,
				NOT_TO_RAM,
				"###caclute ddr output-input pair:0x%x-0x%x\n",
				index_phase,
				card->output_input_phase_pair[index_phase]);
		}

	} else if ((result == SD_SUCCESS) || (result == SD_ERR_ALL_PHASE_PASS)) {
		if (result == SD_SUCCESS) {
			start_phase = output_phase;
			end_phase = output_phase + phase_cnt;
			input_phase =
			    select_tuning_phase(input_tuning_result, phase_cnt);
		} else {
			start_phase = output_fix_phase;
			end_phase = output_fix_phase + phase_cnt;
			input_phase = input_fix_phase;
			card->input_phase_all_pass = 1;
			result = SD_SUCCESS;
		}
		DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
			"output-input result:0x%x-0x%x\n", output_phase,
			input_tuning_result);

		for (i = start_phase; i < end_phase; i++) {
			index_phase = i % phase_cnt;
			card->output_input_phase_pair[index_phase] =
			    input_phase;
			DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER,
				NOT_TO_RAM, "output-input pair:0x%x-0x%x\n",
				index_phase, input_phase);
			input_phase = (input_phase + 1) % phase_cnt;
		}
	}

	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return result;
}

int output_tuning(sd_card_t *card, u32 address, bool ddr200)
{
	int ret = 0;
	u8 test_patern[6] = { 0x55, 0xaa, 0x00, 0xff, 0xf0, 0x0f };
	u8 input_tuning_pattern[64] = {
		0xff, 0xff, 0x00, 0xff, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0xcc,
		0xcc, 0xcc, 0x33, 0xcc, 0xcc,
		0xcc, 0x33, 0x33, 0xcc, 0xcc, 0xcc, 0xff, 0xff, 0xff, 0xee,
		0xff, 0xff, 0xff, 0xee, 0xee, 0xff,
		0xff, 0xff, 0xdd, 0xff, 0xff, 0xff, 0xdd, 0xdd, 0xff, 0xff,
		0xff, 0xbb, 0xff, 0xff, 0xff, 0xbb,
		0xbb, 0xff, 0xff, 0xff, 0x77, 0xff, 0xff, 0xff, 0x77, 0x77,
		0xff, 0x77, 0xbb, 0xdd, 0xee, 0xff
	};
	int j, k, pattern_i;
	u32 result = 0;
	sd_command_t sd_cmd;
	u32 cmdflag;

	u8 *test_buf = kcalloc(512, sizeof(unsigned char), GFP_KERNEL);
	u8 *test_buf_read = kcalloc(512, sizeof(unsigned char), GFP_KERNEL);

	if (test_buf == NULL || test_buf_read == NULL) {
		DbgErr("kcalloc buffer failed\n");
		if (test_buf != NULL)
			kfree(test_buf);
		if (test_buf_read != NULL)
			kfree(test_buf_read);
		return SD_ERR_FATAL;
	}

	if (ddr200)
		cmdflag =
		    CMD_FLG_RESCHK | CMD_FLG_R1 | CMD_FLG_ADMA_SDMA |
		    CMD_FLG_DDR200_WORK_AROUND | CMD_FLG_INF_BUILD;
	else
		cmdflag = CMD_FLG_RESCHK | CMD_FLG_R1 | CMD_FLG_ADMA_SDMA;
	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Enter %s\n",
		__func__);

	/* the output tuning pattern make up by four part */
	for (pattern_i = 0; pattern_i < 4; pattern_i++) {
		if (pattern_i < 3) {
			for (k = 0; k < 512; k++) {
				test_buf[k] =
				    test_patern[(k % 2) + 2 * pattern_i];
			}
		} else {
			for (j = 0; j < 8; j++) {
				for (k = 0; k < 64; k++) {
					if (j % 2) {
						test_buf[k + 64 * j] =
						    input_tuning_pattern[k];
					} else {
						test_buf[k + 64 * j] =
						    input_tuning_pattern[(k +
									  63) %
									 64];
					}
				}
			}
		}

		ret =
		    card_send_sdcmd_timeout(card, &sd_cmd,
					    ddr200 ? SD_CMD25 : SD_CMD24,
					    address, cmdflag, DATA_DIR_OUT,
					    test_buf, 512, 500);
		if (ret == FALSE) {
			if (card->input_phase_all_pass) {
				if (sd_cmd.err.legacy_err_reg & 0x0E) {
					DbgErr
					    ("Uncorrect fix input phase!!!\n");
					result = SD_ERR_FATAL;
					goto end;
				}
			}

			if ((sd_cmd.err.legacy_err_reg & 0x80F)
			    && (card->info.sw_cur_setting.sd_access_mode ==
				SD_FNC_AM_DDR200)) {
				DbgErr("Uncorrect fix output phase!!!\n");
				result = SD_ERR_FATAL;
				goto end;
			}

			if (card->info.sw_cur_setting.sd_access_mode !=
			    SD_FNC_AM_DDR200) {
				DbgErr
				    ("Recovery fail at tuning stage, need re-init!!!\n");
				result = SD_ERR_DEVICE_RETRY;
			} else {
				host_cmddat_line_reset(card->host);
				card_send_command12(card, &sd_cmd);
				if (card_check_rw_ready(card, &sd_cmd, 600) !=
				    TRUE) {
					DbgErr
					    ("Uncorrect fix output phase for DDR200!!!\n");
					result = SD_ERR_FATAL;
				} else
					result = SD_ERR_CRC_MISSMACH;
			}
			goto end;
		}

		ret =
		    card_send_sdcmd_timeout(card, &sd_cmd,
					    ddr200 ? SD_CMD18 : SD_CMD17,
					    address, (cmdflag), DATA_DIR_IN,
					    test_buf_read, 512, 500);
		if (ret == FALSE) {
			if (card->input_phase_all_pass) {
				if (sd_cmd.err.legacy_err_reg & 0x6E) {
					DbgErr
					    ("Uncorrect fix input phase!!!\n");
					result = SD_ERR_FATAL;
					goto end;
				}
			}

			if ((sd_cmd.err.legacy_err_reg & 0x80F)
			    && (card->info.sw_cur_setting.sd_access_mode ==
				SD_FNC_AM_DDR200)) {
				DbgErr("Uncorrect fix output phase!!!\n");
				result = SD_ERR_FATAL;
				goto end;
			}

			if (card->info.sw_cur_setting.sd_access_mode !=
			    SD_FNC_AM_DDR200) {
				DbgErr
				    ("Recovery fail at tuning stage, need re-init!!!\n");
				result = SD_ERR_DEVICE_RETRY;
			} else {
				host_cmddat_line_reset(card->host);
				card_send_command12(card, &sd_cmd);
				if (card_check_rw_ready(card, &sd_cmd, 600) !=
				    TRUE) {
					DbgErr
					    ("Uncorrect fix output phase for DDR200!!!\n");
					result = SD_ERR_FATAL;
				} else
					result = SD_ERR_CRC_MISSMACH;
			}
			goto end;
		}

		for (j = 0; j < (1 * 512); j++) {
			if (*(test_buf + j) != *(test_buf_read + j)) {
				result = SD_ERR_CRC_MISSMACH;
				DbgErr("Compare failed!!!\n");
				goto end;
			}
		}
	}
	result = SD_SUCCESS;
end:

	kfree(test_buf);
	kfree(test_buf_read);

	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Exit %s(%d)\n", __func__, result);
	return result;
}

u32 sdr104_sdr50_output_tuning(sd_card_t *card, u32 address)
{
	u8 start_phase, end_phase, index_phase;
	u8 best_output_phase = 0, best_input_phase = 0;
	u8 i = 0;
	u8 output_fix_phase = 0;
	u8 input_fix_phase = 0;
	u8 output_fail_phase = 0x0;
	u32 result = 0;
	u8 phase_cnt = 11;
	sd_command_t sd_cmd;

	output_fix_phase = get_output_fix_phase(card);

	if (card->info.sw_cur_setting.sd_access_mode == SD_FNC_AM_SDR104)
		input_fix_phase =
		    (u8) card->host->cfg->feature_item.output_tuning_item.sdr104_input_fix_phase_value;
	else
		input_fix_phase =
		    (u8) card->host->cfg->feature_item.output_tuning_item.sdr50_input_fix_phase_value;

	if (card->host->chip_type == CHIP_GG8
	    || card->host->chip_type == CHIP_ALBATROSS)
		phase_cnt = 14;

	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Enter %s, retry_output_fail_phase is 0x%x\n", __func__,
		card->retry_output_fail_phase);

	if (card->retry_output_fail_phase != 0xFF) {
		best_output_phase =
		    (card->retry_output_fail_phase +
		     (phase_cnt >> 1)) % (phase_cnt);
		best_input_phase =
		    card->output_input_phase_pair[best_output_phase];
		goto phase_set;
	}

	result = generate_output_input_phase_pair(card, input_fix_phase, 0);
	if (result == SD_ERR_DEVICE_DEGRADE || result == SD_ERR_FATAL)
		goto exit;
	else if (result == SD_SUCCESS) {
		start_phase = output_fix_phase + 4;
		end_phase = start_phase + phase_cnt;
		for (i = start_phase; i < end_phase; i++) {
			index_phase = i % phase_cnt;
			DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER,
				NOT_TO_RAM,
				"output phase is %d, input phase is %d\n",
				index_phase,
				card->output_input_phase_pair[index_phase]);

			host_set_output_tuning_phase(card->host, index_phase);
			set_input_tuning_phase(card,
					       card->output_input_phase_pair
					       [index_phase]);
			result = output_tuning(card, address, 0);
			if (result == SD_SUCCESS)
				continue;
			else if (result == SD_ERR_FATAL)
				goto exit;
			else {
				card->retry_output_fail_phase = index_phase;
				goto exit;
			}
		}

		if (i == end_phase)
			best_output_phase = output_fix_phase;
		else
			best_output_phase =
			    (output_fail_phase + (phase_cnt >> 1)) % phase_cnt;

		best_input_phase =
		    card->output_input_phase_pair[best_output_phase];
	}

phase_set:
	host_set_output_tuning_phase(card->host, best_output_phase);

	if (sd_tuning(card, &sd_cmd, 150) == FALSE)
		result = SD_ERR_FATAL;
	else
		result = SD_SUCCESS;

exit:

	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Exit %s result %d,best_output_phase 0x%x, best_input_phase  0x%x\n",
		__func__, result, best_output_phase, best_input_phase);

	return result;
}

u32 ddr200_output_tuning(sd_card_t *card, u32 address)
{
	u32 result = 0;
	u8 start_phase, end_phase, index_phase;
	u8 best_output_phase = 0;
	u8 output_fix_phase = 0;
	u8 i = 0;
	u8 cnt = 0;
	int pos = -1;
	u8 phase_cnt = 11;
	sd_host_t *host = card->host;
	u16 output_tuning_result = 0;

	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM, "Enter %s\n",
		__func__);
	if (card->host->chip_type == CHIP_GG8
	    || card->host->chip_type == CHIP_ALBATROSS)
		phase_cnt = 14;

	/* get output-input pair at DDR200 mode for DDR200_workaround */
	result =
	    generate_output_input_phase_pair(card,
					     (u8) card->host->cfg->feature_item.output_tuning_item.sdr104_input_fix_phase_value,
					     1);
	if (result == SD_ERR_DEVICE_DEGRADE || result == SD_ERR_FATAL)
		goto exit;

	output_fix_phase = get_output_fix_phase(card);

	generate_traverse_range(card, output_fix_phase, 4, &start_phase,
				&end_phase);

	for (i = start_phase; i <= end_phase; i++) {
		index_phase = i % phase_cnt;
		DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
			"output phase is %d\n", index_phase);

		/* Used to update input/output phase before write command */
		host->cur_output_phase = index_phase;
		result = output_tuning(card, address, 1);

		if (result == SD_SUCCESS) {
			output_tuning_result |= 1 << index_phase;
			if (pos == -1)
				pos = index_phase;
			cnt++;
		} else if (result == SD_ERR_FATAL)
			goto exit;
		else {
			/* Find the bad phase after good phase */
			if (pos != -1) {
				DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER,
					NOT_TO_RAM,
					"break output phase is %d\n",
					index_phase);
				break;
			}
		}
	}

	if (!cnt) {
		DbgErr("All phase failed when output_tuning!!!\n");
		result = SD_ERR_DEVICE_DEGRADE;
	} else {
		best_output_phase = (pos + (cnt >> 1)) % phase_cnt;
		host->cur_output_phase = best_output_phase;
		host_set_output_tuning_phase(card->host, best_output_phase);
		set_input_tuning_phase(card,
				       card->output_input_phase_pair
				       [best_output_phase]);

		result = SD_SUCCESS;
	}

exit:
	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"best_output_phase 0x%x, output_tuning_result  0x%x\n",
		best_output_phase, output_tuning_result);
	DbgInfo(MODULE_SD_CARD, FEATURE_ERROR_RECOVER, NOT_TO_RAM,
		"Exit %s, result %d\n", __func__, result);
	return result;
}
