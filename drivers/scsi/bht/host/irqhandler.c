// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: irqhandler.c
 *
 * Abstract: handle IRQ
 *
 * Version: 1.00
 *
 * Author: Chuanjin
 *
 * Environment:	OS Independent
 *
 * History:
 *
 * 9/2/2014		Creation	Chuanjin
 */

#include "../include/basic.h"
#include "../include/hostapi.h"
#include "../include/funcapi.h"
#include "../include/cardapi.h"
#include "hostreg.h"
#include "../include/debug.h"
#include "../include/hostvenapi.h"
#include "../include/cmdhandler.h"

void host_error_int_recovery_stage1(sd_host_t *host, u16 error_int_state,
				    bool check);

#define SDHCI_MAX_INT_RETRY 16

/*
 *	static void sw_int_issue(sd_host_t *host)
 *	{
 *		u32 reg = sdhci_readl(host, SDHCI_DRIVER_CTRL_REG);
 *
 *		reg = reg | (SDHCI_VENDOR_SW_INT_BIT);
 *		sdhci_writel(host, SDHCI_DRIVER_CTRL_REG, reg);
 *	}
 */

bool thread_exec_high_prio_job(bht_dev_ext_t *pdx, cb_soft_intr_t func,
			       void *data)
{
	bool result = TRUE;
#ifndef CFG_SCSIPORT_DRIVER
	if (func)
		func(data);
	return result;
#else
	os_init_completion(pdx, &pdx->soft_irq.completion);
	pdx->soft_irq.data = data;
	pdx->soft_irq.cb_func = func;
	pdx->soft_irq.enable = 1;
	sw_int_issue(&pdx->host);
	result =
	    os_wait_for_completion(pdx, &pdx->soft_irq.completion,
				   SOFT_INTR_TIMEOUT);
	pdx->soft_irq.cb_func = NULL;
	pdx->soft_irq.data = NULL;
	pdx->soft_irq.enable = 0;
	if (result == FALSE)
		DbgErr("Software Intr is not ocurr\n");
	return result;
#endif

}

void irq_disable_sdcmd_int(sd_host_t *host)
{
	host_int_sig_update(host, SDHCI_INT_INSERT_REMOVE_CARD_BITS);
	if (host->uhs2_flag)
		host_uhs2_err_sig_update(host, 0);
}

void send_req_complete_evt(sd_host_t *host, void *pdx)
{
	host_cmd_req_t *req = host->cmd_req;
	sd_command_t *sd_cmd = NULL;

	DbgInfo(MODULE_SD_HOST, FEATURE_INTR_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if (req != NULL)
		sd_cmd = req->private;
	if (sd_cmd == NULL) {
		DbgErr("sd_cmd is NULL at send req complete evt!\n");
		goto exit;
	}

	if (sd_cmd->err.error_code == 0) {
		if (req->cb_req_complete)
			req->cb_req_complete(pdx, &sd_cmd->err);
	}
	irq_disable_sdcmd_int(host);
	os_finish_completion(pdx, &req->done);
exit:
	DbgInfo(MODULE_SD_HOST, FEATURE_INTR_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);

}

void insert_card_handle(bht_dev_ext_t *pdx)
{
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT | FEATURE_INTR_TRACE, TO_RAM,
		"Enter %s\n", __func__);

	pdx->card.card_present = TRUE;
	pdx->card.card_chg = TRUE;
	pdx->scsi.scsi_eject = FALSE;

	/* Keep chip power on. */
	hostven_main_power_ctrl(&pdx->host, TRUE);

#if CFG_OS_LINUX
	os_set_event(&pdx->os, EVENT_CARD_CHG);
#endif

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT | FEATURE_INTR_TRACE, TO_RAM,
		"Exit %s\n", __func__);
}

void remove_card_handle(bht_dev_ext_t *pdx)
{

	u32 regval;
	sd_host_t *host = &pdx->host;

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT | FEATURE_INTR_TRACE, TO_RAM,
		"Enter %s\n", __func__);

	if (shift_bit_func_enable(&pdx->host))
		set_pattern_value(&pdx->host, 0x00);
	if (host->cfg->card_item.sd7_sdmode_switch_control.sd70_trail_run) {
		regval = pci_readl(host, 0x444);
		regval |= (1 << 11);
		pci_writel(host, 0x444, regval);
	} else {
		regval = pci_readl(host, 0x444);
		regval &= (~(1 << 11));
		pci_writel(host, 0x444, regval);
	}

	pdx->card.card_present = FALSE;
	pdx->scsi.scsi_eject = FALSE;
	card_stuct_uinit(&pdx->card);

#if CFG_OS_LINUX
	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT | FEATURE_INTR_TRACE, TO_RAM,
		"set event\n");

	os_set_event(&pdx->os, EVENT_CARD_CHG);
#else
	os_set_event(pdx, &pdx->os, EVENT_TASK_OCCUR, TASK_CARD_CHG);
#endif
	/* add for driver controlled UHS2 VDD2 power off when card remove */
	host_uhs2_clear(&pdx->host, TRUE);
	hostven_main_power_ctrl(&pdx->host, FALSE);

	DbgInfo(MODULE_SD_HOST, FEATURE_CARD_INIT | FEATURE_INTR_TRACE, TO_RAM,
		"Exit %s\n", __func__);
}

void sw_int_clear(sd_host_t *host)
{
	u32 reg = sdhci_readl(host, SDHCI_DRIVER_CTRL_REG);

	reg = reg & (~SDHCI_VENDOR_SW_INT_BIT);
	sdhci_writel(host, SDHCI_DRIVER_CTRL_REG, reg);
}

bool sw_int_occur(sd_host_t *host)
{
	u32 reg = sdhci_readl(host, SDHCI_DRIVER_CTRL_REG);

	if (reg & SDHCI_VENDOR_SW_INT_BIT)
		return TRUE;
	else
		return FALSE;
}

void host_chk_ocb_occur(sd_host_t *host)
{
	u32 ocb_status = 0;

	bht_dev_ext_t *pdx = (bht_dev_ext_t *) (host->pdx);

	if (host->chip_type == CHIP_ALBATROSS)
		return;

	ocb_status = sdhci_readl(host, SDHCI_DRIVER_CTRL_REG);
	DbgInfo(MODULE_SD_HOST, FEATURE_FUNC_TRACE, NOT_TO_RAM,
		"Enter %s, ocb status =0x%08x\n", __func__, ocb_status);
	if (ocb_status & SDHCI_OCB_FET_INT_ACTIVE) {
		DbgErr("OCB FET is active!\n");

		host_cmddat_line_reset(host);
		host_int_dis_sig_all(host, FALSE);
		host_poweroff(host, CARD_ERROR);

		pdx->card.card_present = FALSE;
		card_stuct_uinit(&pdx->card);

		/* host 0x1c0 [22] */
		if ((ocb_status & SDHCI_OCB_FET_INT_DENOUNCE) !=
		    SDHCI_OCB_FET_INT_DENOUNCE) {
			ocb_status &= ~SDHCI_OCB_FET_INT_ACTIVE;
			ocb_status |= SDHCI_OCB_FET_INT_DENOUNCE;
			sdhci_writel(host, SDHCI_DRIVER_CTRL_REG, ocb_status);
		}

		/* clear 0x1c0 [5] */
		if (host->cfg->host_item.test_ocb_ctrl.int_check_en == 0) {
			ocb_status &= ~SDHCI_OCB_FET_INT_ACTIVE;
			ocb_status &= ~SDHCI_OCB_INT_MASK;
			sdhci_writel(host, SDHCI_DRIVER_CTRL_REG, ocb_status);
		} else {
			ocb_status &= ~SDHCI_OCB_FET_INT_ACTIVE;
			ocb_status |= SDHCI_OCB_INT_MASK;
			sdhci_writel(host, SDHCI_DRIVER_CTRL_REG, ocb_status);
		}

	}

	DbgInfo(MODULE_SD_HOST, FEATURE_FUNC_TRACE, NOT_TO_RAM, "Exit %s\n",
		__func__);
}

/*
 * model:
 * 1. support async mode or sync with binding different evt(last one for ADMA3).
 * 2. list all need complete SRB.
 */

void sw_int_handle(bht_dev_ext_t *pdx)
{
	cb_soft_intr_t func = pdx->soft_irq.cb_func;
	void *data = pdx->soft_irq.data;

	if (pdx->soft_irq.enable) {
		if (func)
			func(data);
	}
}

void cb_handle(u16 *wait_flag, u16 clr_bit, s32 cb_ret, bool *hascmd,
	       bht_dev_ext_t *pdx)
{
	sd_host_t *host = &pdx->host;

	DbgInfo(MODULE_SD_HOST, FEATURE_INTR_TRACE, NOT_TO_RAM,
		"Enter %s wait_flag=%xh,clr_bit=%xh,cb_ret=%xh,hascmd=%x\n",
		__func__, wait_flag, clr_bit, cb_ret, *hascmd);
	switch (cb_ret) {
	case INTR_CB_ERR:
		/* callback must set error condition flag */
		*wait_flag = 0;
		break;
	case INTR_CB_OK:
		(*wait_flag) &= clr_bit;
		break;
	case INTR_CB_NOEND:
	default:
		break;
	}
	if (*wait_flag == 0) {
		send_req_complete_evt(host, pdx);
		*hascmd = FALSE;
	}
	DbgInfo(MODULE_SD_HOST, FEATURE_INTR_TRACE, NOT_TO_RAM,
		"Exit %s hascmd=%x\n", __func__, *hascmd);
}

bool device_power_enter_non_D0(void *param)
{
	bht_dev_ext_t *pdx = (bht_dev_ext_t *) param;

	if (pdx->pm_state.rtd3_entered ||
	    pdx->pm_state.s3s4_entered ||
	    pdx->pm_state.s5_entered || pdx->pm_state.warm_boot_entered)
		return TRUE;
	else
		return FALSE;
}

bool sdhci_irq(void *param)
{
	bht_dev_ext_t *pdx = (bht_dev_ext_t *) param;
	u32 int_status = 0;
	bool ret = TRUE;
	sd_host_t *host = &pdx->host;
	sd_card_t *card = &pdx->card;
	host_cmd_req_t *req = host->cmd_req;
	cfg_item_t *cfg = host->cfg;
	sd_command_t *sd_cmd = NULL;
	bool hascmd = TRUE;
	s32 cb_ret = 0;
	u32 out_of_range = 0;
	u16 max_loops = SDHCI_MAX_INT_RETRY;
	u32 hw_timer_intr = 0;
	u32 regval;
	u16 expected_range = 0;
	u32 time_cnt_100us = 0;
	bool card_insert_flag = 0;
	bool gpio_interrupt = 0;

	DbgInfo(MODULE_SD_HOST, FEATURE_INTR_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if (host->chip_type == CHIP_GG8) {
		if (host->cfg->card_item.sd7_sdmode_switch_control.switch_method_ctrl ==
		    HW_DETEC_HW_SWITCH) {
			DbgInfo(MODULE_SD_HOST, FEATURE_INTR_TRACE, NOT_TO_RAM,
				"Hardware\n");

			regval = sdhci_readl(host, 0x1e0);
			if (regval & (1 << 16)) {
				os_atomic_set(&host->clkreqn_status, 1);
				sdhci_or16(host, 0x1e2, 0x01);
				return TRUE;
			} else if (regval & (1 << 17)) {
				os_atomic_set(&host->clkreqn_status, 2);
				sdhci_or16(host, 0x1e2, 0x02);
				return TRUE;
			}
		}

		regval = pci_readl(host, 0x51C);
		if (regval & 0x2) {
			regval |= 0x2;
			pci_writel(host, 0x51C, regval);

			regval = sdhci_readl(host, 0x3E);
			if (regval & (1 << 8)) {
				regval &= ~(1 << 8);
				sdhci_writel(host, 0x3E, regval);
			}
			card->sw_ctrl_swicth_to_express = TRUE;

			/* turn off VDD2/VDD1 */
			host_set_vddx_power(host, VDD2, POWER_OFF);
			host_set_vddx_power(host, VDD1, POWER_OFF);

			card_stuct_uinit(&pdx->card);

#if CFG_OS_LINUX
			os_set_event(&pdx->os, EVENT_CARD_CHG);
#endif

			card->state = CARD_STATE_POWEROFF;
			return TRUE;
		}

		/* check PCIe reference clock detection timeout status */
		if (sdhci_readl(host, 0x1E0) & BIT15) {
			DbgInfo(MODULE_VEN_HOST, FEATURE_DRIVER_INIT,
				NOT_TO_RAM,
				"PCIe reference clock detection timeout\n");

			/* Clear interrupt */
			sdhci_or16(host, 0x1E0, BIT15);

			if ((cfg->feature_item.auto_detect_refclk_counter_range_ctl.enable
				== 0) ||
			    ((cfg->feature_item.auto_detect_refclk_counter_range_ctl.enable
				== 1) &&
				(cfg->feature_item.auto_detect_refclk_counter_range_ctl.req_refclkcnt_minmax_source_sel
				== 0))) {
				/* set refclk_cnt_range_detect_soft_reset */
				pci_orw(host, 0x462, BIT14);
				DbgInfo(MODULE_VEN_HOST, FEATURE_DRIVER_INIT,
					NOT_TO_RAM,
					"Start auto detect refclk counter range\n");

				/* Polling refclk_cnt_range_detect_done */
				time_cnt_100us = 0;
				while (0 == (pci_readl(host, 0x460) & BIT31)) {
					if (time_cnt_100us >=
					    ADJUST_EXPEXTED_RANGE_TIMEOUT_COUNT) {
						DbgInfo(MODULE_VEN_HOST,
							FEATURE_DRIVER_INIT,
							NOT_TO_RAM,
							"Auto detect expected range timeout\n");
						return ret;
					}
					os_udelay(100);
					time_cnt_100us++;
				}

				expected_range = pci_readw(host, 0x460);
				DbgInfo(MODULE_VEN_HOST, FEATURE_DRIVER_INIT,
					NOT_TO_RAM,
					"Software invoked Auto detect refclk counter range done, range min %#x , max %#x\n",
					(expected_range >> 8),
					(expected_range & 0xFF));
			}
		}
	}

	if (HW_TIMER_CFG == TRUE) {
		hw_timer_intr = pci_readl(host, 0x51c) & BIT7;
		if (hw_timer_intr & BIT7)
			ven_writel(host, 0x51c, BIT7);
	}

	int_status = sdhci_readl(host, SDHCI_INT_STATUS);

	if (shift_bit_func_enable(host)) {
		if (ven_readl(host, 0x51c) & (1 << 2))
			gpio_interrupt = TRUE;
	}

int_again:
	if (((!int_status) || (int_status == 0xffffffff))
	    && (hw_timer_intr == 0) && (gpio_interrupt == FALSE)) {
		if (int_status == 0xFFFFFFFF)
			DbgErr("%s !!!int %x\n", __func__, int_status);
		ret = FALSE;
		goto out;
	}
	req = host->cmd_req;

	if (req == NULL)
		sd_cmd = NULL;
	else
		sd_cmd = (sd_command_t *) req->private;

	if (req == NULL || sd_cmd == NULL)
		hascmd = FALSE;

	DbgInfo(MODULE_SD_HOST, FEATURE_INTR_TRACE, NOT_TO_RAM,
		"%s int_status=0x%08X\n", __func__, int_status);
	/* error int handle */
	if (int_status & SDHCI_INT_ERROR) {
		u16 error_status = (int_status >> 16);

		/* clear error interrupt */
		sdhci_writel(host, SDHCI_INT_STATUS, (int_status & 0xFFFF8000));

		DbgErr("error intr=0x%04X hascmd=%d\n", error_status, hascmd);
		switch (hascmd) {
		case TRUE:
			if ((sd_cmd->cmd_index == SD_CMD17 ||
			     sd_cmd->cmd_index == SD_CMD18 ||
			     sd_cmd->cmd_index == SD_CMD24 ||
			     sd_cmd->cmd_index == SD_CMD25) &&
			    (((error_status & SDHCI_INT_ACMD12ERR)
			      && (RESP_ERR_TYPE_OUT_OF_RANGE &
				  sdhci_readl(host, SDHCI_RESPONSE + 12)))
			     || (!(error_status & SDHCI_INT_ACMD12ERR)
				 && (RESP_ERR_TYPE_OUT_OF_RANGE &
				     sdhci_readl(host, SDHCI_RESPONSE))))
			    ) {
				PrintMsg
				    ("ignore out of range when use SD_CMD18 read %d sector, int_status=0x%08x\n",
				     sd_cmd->argument, int_status);
				int_status |=
				    (SDHCI_INT_TRANSFER_COMP |
				     SDHCI_INT_NORMAL_BITS);
				int_status &= ~SDHCI_INT_ERROR;
				int_status &=
				    ~((SDHCI_INT_ADMA_ERROR |
				       SDHCI_INT_ACMD12ERR |
				       SDHCI_INT_RESP_ERROR) >> 16);
				out_of_range = 1;
				break;
			}
			fallthrough;
		default:
			if (host->uhs2_flag == TRUE) {
				u32 uhs2_err_int_status = 0;

				uhs2_err_int_status =
				    sdhci_readl(host, SDHCI_UHS2_ERRINT_STS);
				sdhci_writel(host, SDHCI_UHS2_ERRINT_STS,
					     uhs2_err_int_status);
				DbgErr
				    ("error=0x%08x,UHS2 Error intr occur=0x%08X hascmd=%d\n",
				     int_status, uhs2_err_int_status, hascmd);
				if (hascmd) {
					if (uhs2_err_int_status &
					    req->int_flag_uhs2_err) {
						sd_cmd->err.uhs2_err_reg =
						    uhs2_err_int_status &
						    req->int_flag_uhs2_err;
						sd_cmd->err.error_code |=
						    ERR_CODE_INTR_ERR;
						if ((uhs2_err_int_status &
						     SDHCI_UHS2_ERR_RESP)
						    && sd_cmd->hw_resp_chk
						    && req->cb_response) {
							req->cb_response(card,
									 req);
						}
						cb_handle(&(req->int_flag_wait),
							  0, INTR_CB_ERR,
							  &hascmd, pdx);
					}
				}
			} else {
				u16 err_status = (u16) (int_status >> 16);

				DbgErr("error intr=0x%04X hascmd=%d\n",
				       err_status, hascmd);
				if (hascmd) {
					if (err_status & req->int_flag_err) {
						sd_cmd->err.legacy_err_reg =
						    err_status &
						    req->int_flag_err;
						sd_cmd->err.error_code |=
						    ERR_CODE_INTR_ERR;
						if ((err_status &
						     SDHCI_INT_RESP_ERROR)
						    && req->cb_response
						    && sd_cmd->hw_resp_chk) {
							req->cb_response(card,
									 req);
						}
						cb_handle(&(req->int_flag_wait),
							  0, INTR_CB_ERR,
							  &hascmd, pdx);
					}
				}
				host_error_int_recovery_stage1(host, err_status,
							       FALSE);
			}
			break;
		}
	}

	/* If not in a SD command execute context, don't call sd cmd call back */
	if (hascmd == FALSE)
		goto next;

	/* normal interrupt handle */
	if (int_status & SDHCI_INT_NORMAL_BITS) {

		if (int_status & req->int_flag_wait) {
			u16 irq_wait = (u16) (int_status & req->int_flag_wait);

			if (irq_wait & SDHCI_INT_CMD_COMP) {
				/* clear interrupt status */
				sdhci_writel(host, SDHCI_INT_STATUS,
					     SDHCI_INT_CMD_COMP);
				if (req->cb_response)
					cb_ret = req->cb_response(card, req);
				else
					cb_ret = INTR_CB_OK;
				sd_cmd->cmd_done = 1;
				cb_handle(&(req->int_flag_wait),
					  ~SDHCI_INT_CMD_COMP, cb_ret, &hascmd,
					  pdx);
			}
			if (irq_wait & SDHCI_INT_TRANSFER_COMP) {
				/* clear interrupt status */
				sdhci_writel(host, SDHCI_INT_STATUS,
					     SDHCI_INT_TRANSFER_COMP);
				if (req->cb_trans_complete)
					cb_ret = req->cb_trans_complete(card, req);
				else
					cb_ret = INTR_CB_OK;
				cb_handle(&(req->int_flag_wait),
					  ~SDHCI_INT_TRANSFER_COMP, cb_ret,
					  &hascmd, pdx);

			}
			if (irq_wait & SDHCI_INT_BUFFER_READY_BITS) {
				/* clear interrupt status */
				sdhci_writel(host, SDHCI_INT_STATUS,
					     SDHCI_INT_BUFFER_READY_BITS);
				if (req->cb_buffer_ready)
					cb_ret = req->cb_buffer_ready(card, req);
				else
					cb_ret = INTR_CB_OK;
				cb_handle(&(req->int_flag_wait),
					  ~SDHCI_INT_BUFFER_READY_BITS, cb_ret,
					  &hascmd, pdx);
			}
			if (irq_wait & SDHCI_INT_DMA_END) {
				/* clear interrupt status */
				sdhci_writel(host, SDHCI_INT_STATUS,
					     SDHCI_INT_DMA_END);
				if (req->cb_boundary)
					cb_ret = req->cb_boundary(card, req);
				else
					cb_ret = INTR_CB_OK;

				/* avoid wrongly clear DMA int wait flag for ddr200 workaround  */
				if (sd_cmd != NULL
				    && sd_cmd->gg8_ddr200_workaround == 0)
					cb_handle(&(req->int_flag_wait),
						  ~SDHCI_INT_DMA_END, cb_ret,
						  &hascmd, pdx);
				else if (sd_cmd != NULL)
					sd_cmd->gg8_ddr200_workaround = 0;
			}

		}

	}

next:
	/* insert or remove card handle */
	if (device_power_enter_non_D0(param) == FALSE) {

		if (shift_bit_func_enable(host)) {
			/* if (ven_readl(host, 0x51c) & (1 << 2)) */
			if (gpio_interrupt == TRUE) {
				/* clear interrupt status */
				ven_or16(host, 0x51c, (1 << 2));

				regval = ven_readl(host, 0x510);

				if (!(regval & (1 << 6)))
					card_insert_flag = TRUE;
				else if (regval & (1 << 6))
					card_insert_flag = FALSE;
			}

		} else {
			if (int_status & SDHCI_INT_CARD_INSERT) {
				card_insert_flag = TRUE;
				/* clear interrupt status */
				sdhci_writel(host, SDHCI_INT_STATUS,
					     SDHCI_INT_CARD_INSERT);
			} else if (int_status & SDHCI_INT_CARD_REMOVE) {
				card_insert_flag = FALSE;
				/* clear interrupt status */
				sdhci_writel(host, SDHCI_INT_STATUS,
					     SDHCI_INT_CARD_REMOVE);
			}

		}

		if ((gpio_interrupt && shift_bit_func_enable(host))
		    || (!shift_bit_func_enable(host)
			&& (int_status & SDHCI_INT_CARD_INSERT))
		    || (!shift_bit_func_enable(host)
			&& (int_status & SDHCI_INT_CARD_REMOVE))) {
			if (card_insert_flag) {
				pdx->scsi_init_flag = 0;
				insert_card_handle(pdx);
				/* fixed uhs1 issue#120 */
				pdx->card.cmd_low_reset_flag = FALSE;
			} else {
				if (hascmd) {
					if (req->int_flag_wait) {
						sd_cmd->err.error_code |=
						    ERR_CODE_NO_CARD;
						cb_handle(&(req->int_flag_wait),
							  0, INTR_CB_ERR,
							  &hascmd, pdx);
					}
				}
				remove_card_handle(pdx);
			}
		}

	}
#if (0)
	/* ptest */
	if (hascmd && req) {
		if (req->int_flag_wait == 0)
			goto out;
	}
#endif
	int_status = sdhci_readl(host, SDHCI_INT_STATUS);

	if (device_power_enter_non_D0(param))
		int_status &=
		    ~(SDHCI_INT_ROC_BITS | SDHCI_INT_CARD_INSERT |
		      SDHCI_INT_CARD_REMOVE);
	else
		int_status &= ~(SDHCI_INT_ROC_BITS);
	if (HW_TIMER_CFG == TRUE) {
		if (hw_timer_intr & BIT7)
			func_timer_callback(pdx);

		hw_timer_intr = pci_readl(host, 0x51c) & BIT7;

		if (hw_timer_intr & BIT7)
			ven_writel(host, 0x51c, BIT7);
	}

	if (host->dump_mode)
		goto out;

	if ((int_status || hw_timer_intr) && --max_loops)
		goto int_again;

out:

	if (host->cfg->host_item.test_ocb_ctrl.int_check_en)
		host_chk_ocb_occur(host);
	DbgInfo(MODULE_SD_HOST, FEATURE_INTR_TRACE, NOT_TO_RAM,
		"Exit %s ret=%x\n", __func__, ret);
	return ret;
}

bool irq_poll_cmd_done(bht_dev_ext_t *pdx, completion_t *p, s32 timeout_ms)
{
	bool ret = FALSE;
	sd_host_t *host = &pdx->host;
	sd_card_t *card = &pdx->card;
	host_cmd_req_t *req = host->cmd_req;
	sd_command_t *sd_cmd = NULL;
	/* in order sdhci_irq need some time so just multiple 400 */
	u32 timeout = timeout_ms * 400;

	DbgInfo(MODULE_SD_HOST, FEATURE_INTR_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if (req == NULL)
		sd_cmd = NULL;
	else
		sd_cmd = (sd_command_t *) req->private;

	if (req == NULL || sd_cmd == NULL) {
		DbgErr("no command need do poll wait\n");
		goto exit;
	}

	while (timeout > 0) {
		ret = sdhci_irq(pdx);
		if (ret == FALSE || card->card_present == FALSE)
			goto exit;

		if (req->int_flag_wait == 0) {
			ret = TRUE;
			break;
		}
		timeout--;
		os_udelay(1);
	}

exit:
	DbgInfo(MODULE_SD_HOST, FEATURE_INTR_TRACE, NOT_TO_RAM,
		"Exit %s ret=%x\n", __func__, ret);
	return ret;
}
