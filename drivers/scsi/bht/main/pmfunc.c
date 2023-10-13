// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: pmfunc.c
 *
 * Abstract: This source file used to implement power management functions
 *
 * Version: 1.00
 *
 * Author: Yuxiang
 *
 * Environment:	OS Independent
 *
 * History:
 *
 * 9/2/2014		Creation	Yuxiang
 */

#include "../include/basic.h"
#include "../include/debug.h"
#include "../include/reqapi.h"
#include "../include/funcapi.h"
#include "../include/hostapi.h"
#include "../include/hostvenapi.h"
#include "../include/cardapi.h"
#include "../include/cmdhandler.h"

void pm_init(bht_dev_ext_t *pdx)
{

	DbgInfo(MODULE_MAIN_PM, FEATURE_DRIVER_INIT, 0, "Enter %s chip:%xh\n",
		__func__, pdx->host.chip_type);

	/* TODO: RTD3 enable From the registry */
	pdx->pm_state.s3s4_entered = FALSE;
	pdx->pm_state.rtd3_en = hostven_rtd3_check(&pdx->host);
	pdx->pm_state.rtd3_entered = FALSE;
	pdx->pm_state.s5_entered = FALSE;
	pdx->pm_state.warm_boot_entered = FALSE;
	hostven_pm_mode_cfg(&pdx->host, &(pdx->pm_state));
	os_pm_init(pdx);

	DbgInfo(MODULE_MAIN_PM, FEATURE_PM_TRACE, 0, "Exit %s\n", __func__);
}

extern void host_internal_clk_setup(sd_host_t *host, bool on);

static void req_enter_d0_internal(bht_dev_ext_t *pdx, bool reinit_async)
{
	DbgInfo(MODULE_MAIN_PM, FEATURE_PM_TRACE, 0,
		"Enter %s rtd3=%d card_init status=%d\n", __func__,
		pdx->pm_state.rtd3_en, pdx->card.thread_init_card_flag);

#ifdef MultiThread
	os->thread_auto_timer_runing = FALSE;
	os->thread_card_init_runing = FALSE;
	os->thread_card_remove_runing = FALSE;
	os->thread_pending_runing = FALSE;
	os->thread_gen_io_runing = FALSE;
	os->thread_tag_io_runing = FALSE;
	os->thread_rtd3_runing = FALSE;
	os->thread_terminate_runing = FALSE;
#else

#endif

	host_vendor_feature_init(&(pdx->host));
	host_init(&(pdx->host));

	if (hostven_chk_card_present(&pdx->host)) {
		pdx->card.card_present = TRUE;
		/* clean card_init flag status to wait for initialization */
		pdx->card.thread_init_card_flag = 0;

	} else {
		pdx->scsi.scsi_eject = FALSE;

		pdx->card.card_present = FALSE;
		card_stuct_uinit(&pdx->card);
	}
	thermal_uninit(pdx);
	if (reinit_async) {
		if (pdx->scsi.scsi_eject == 0) {

#if CFG_OS_LINUX
			os_set_event(&pdx->os, EVENT_CARD_CHG);
#else
			os_set_event(pdx, &pdx->os, EVENT_TASK_OCCUR,
				     TASK_CARD_CHG);
#endif
		} else {
			host_internal_clk_setup(&(pdx->host), TRUE);
		}
	}

	pdx->pm_state.s3s4_entered = FALSE;
	pdx->pm_state.s5_entered = FALSE;
	pdx->pm_state.warm_boot_entered = FALSE;
	if (pdx->pm_state.rtd3_en)
		pdx->pm_state.rtd3_entered = FALSE;

	DbgInfo(MODULE_MAIN_PM, FEATURE_PM_TRACE, 0,
		"Exit %s card_init status=%d\n", __func__,
		pdx->card.thread_init_card_flag);
}

void req_enter_d0(bht_dev_ext_t *pdx)
{
	req_enter_d0_internal(pdx, TRUE);
}

void req_enter_d0_sync(bht_dev_ext_t *pdx)
{
	req_enter_d0_internal(pdx, FALSE);
}

void req_pre_enter_d3(bht_dev_ext_t *pdx)
{
	DbgInfo(MODULE_MAIN_PM, FEATURE_PM_TRACE, 0, "Enter %s\n",
		__func__);

	/* for thread sync : such as autotimer stop infinite */
	if (os_pending_thread(pdx, TRUE) == FALSE)
		DbgErr("%s pending thread failed\n", __func__);

	if (card_stop_infinite(&pdx->card, FALSE, NULL) == FALSE)
		card_power_off(&(pdx->card), TRUE);

	os_pending_thread(pdx, FALSE);

	DbgInfo(MODULE_MAIN_PM, FEATURE_PM_TRACE, 0, "Exit %s\n", __func__);
}

#if (0)
void pcr_part_a_backup(bht_dev_ext_t *pdx)
{
	if (pdx->cfg->driver_item.backup_part_a) {
		/* enable or disable LED output or not. */
		pdx->pm_state.reg_0xdc = pci_readl(&pdx->host, 0xdc);

		/* disable or enable RTD3 */
		pdx->pm_state.reg_0x3e0 = pci_readl(&pdx->host, 0x3e0);

		pdx->pm_state.reg_0x3e8 = pci_readl(&pdx->host, 0x3e8);

		pdx->pm_state.reg_0x3ec = pci_readl(&pdx->host, 0x3ec);

		/* Change the ASPM L0s Exit Latency */
		pdx->pm_state.reg_0xf4 = pci_readl(&pdx->host, 0xf4);

		/* ep nfts value */
		pdx->pm_state.reg_0x74 = pci_readl(&pdx->host, 0x74);

		/* Power saving mode setting */
		pdx->pm_state.reg_0xf0 = pci_readl(&pdx->host, 0xf0);

		/* enable or disable ASPM L0s&L1 */
		pdx->pm_state.reg_0x90 = pci_readl(&pdx->host, 0x90);

	}

}

void pcr_part_b_backup(bht_dev_ext_t *pdx)
{
	if (pdx->cfg->driver_item.backup_part_b) {
		pdx->pm_state.reg_0x64 = pci_readl(&pdx->host, 0x64);
		/* skt power control output enable */
		pdx->pm_state.reg_0xec = pci_readl(&pdx->host, 0xec);
		/* ocb cntl timer */
		/* ocb cntl enable */
		pdx->pm_state.reg_0xd4 = pci_readl(&pdx->host, 0xd4);
		pdx->pm_state.reg_0x3e4 = pci_readl(&pdx->host, 0x3e4);
		/* PLL DM */
		pdx->pm_state.reg_0x304 = pci_readl(&pdx->host, 0x304);

		/* Set Base Clock Frequency */
		pdx->pm_state.reg_0x328 = pci_readl(&pdx->host, 0x328);

		/* Set DLL tuning window */
		pdx->pm_state.reg_0x300 = pci_readl(&pdx->host, 0x300);

		/* aux power LDO */
		pdx->pm_state.reg_0x68 = pci_readl(&pdx->host, 0x68);

		/* Adjust the output delay for SD2.0 high speed mode */
		pdx->pm_state.reg_0x350 = pci_readl(&pdx->host, 0x350);
		/* T_EIDL_ENTRY */
		pdx->pm_state.reg_0x35c = pci_readl(&pdx->host, 0x35c);

		/* disable bit for UHSII term_resistor_calibration */
		pdx->pm_state.reg_0x3e0 = pci_readl(&pdx->host, 0x3e0);

		/* Set the Max power supply capability of SD host */
		pdx->pm_state.reg_0x334 = pci_readl(&pdx->host, 0x334);

		/* external enable polarity control pin */
		pdx->pm_state.reg_0xd8 = pci_readl(&pdx->host, 0xd8);

		/* AOSC off support */
		pdx->pm_state.reg_0x3f0 = pci_readl(&pdx->host, 0x3f0);

		/* max read request size */
		pdx->pm_state.reg_0x88 = pci_readl(&pdx->host, 0x88);

		/* UHSII DLL watch dog */
		pdx->pm_state.reg_0x33c = pci_readl(&pdx->host, 0x33c);

		/* PCI-PM L1 entrance timer */
		pdx->pm_state.reg_0xe0 = pci_readl(&pdx->host, 0xe0);

		/* ASPM L1 entrance timer */
		pdx->pm_state.reg_0xfc = pci_readl(&pdx->host, 0xfc);

	}
}

void pcr_part_a_restore(bht_dev_ext_t *pdx)
{
	if (pdx->cfg->driver_item.backup_part_a) {
		pci_writel(&pdx->host, 0xdc, pdx->pm_state.reg_0xdc);
		pci_writel(&pdx->host, 0x3e0, pdx->pm_state.reg_0x3e0);
		pci_writel(&pdx->host, 0x3e8, pdx->pm_state.reg_0x3e8);
		pci_writel(&pdx->host, 0x3ec, pdx->pm_state.reg_0x3ec);
		pci_writel(&pdx->host, 0xf4, pdx->pm_state.reg_0xf4);
		pci_writel(&pdx->host, 0x74, pdx->pm_state.reg_0x74);
		pci_writel(&pdx->host, 0xf0, pdx->pm_state.reg_0xf0);
		pci_cfgio_writel(&pdx->host, 0x90, pdx->pm_state.reg_0x90);
	}
}

void pcr_part_b_restore(bht_dev_ext_t *pdx)
{
	if (pdx->cfg->driver_item.backup_part_b) {
		pci_writel(&pdx->host, 0x64, pdx->pm_state.reg_0x64);
		pci_writel(&pdx->host, 0xec, pdx->pm_state.reg_0xec);
		pci_writel(&pdx->host, 0xd4, pdx->pm_state.reg_0xd4);
		pci_writel(&pdx->host, 0x304, pdx->pm_state.reg_0x304);
		pci_writel(&pdx->host, 0x328, pdx->pm_state.reg_0x328);
		pci_writel(&pdx->host, 0x300, pdx->pm_state.reg_0x300);
		/* Don't need to restore as it is the stick register */
		pci_writel(&pdx->host, 0x3e4, pdx->pm_state.reg_0x3e4);
		pci_writel(&pdx->host, 0x68, pdx->pm_state.reg_0x68);

		pci_writel(&pdx->host, 0x350, pdx->pm_state.reg_0x350);
		pci_writel(&pdx->host, 0x35c, pdx->pm_state.reg_0x35c);
		pci_writel(&pdx->host, 0x3e0, pdx->pm_state.reg_0x3e0);
		pci_writel(&pdx->host, 0x334, pdx->pm_state.reg_0x334);
		pci_writel(&pdx->host, 0xd8, pdx->pm_state.reg_0xd8);
		pci_writel(&pdx->host, 0x3f0, pdx->pm_state.reg_0x3f0);
		pci_cfgio_writel(&pdx->host, 0x88, pdx->pm_state.reg_0x88);
		pci_writel(&pdx->host, 0x33c, pdx->pm_state.reg_0x33c);
		pci_writel(&pdx->host, 0xe0, pdx->pm_state.reg_0xe0);
		pci_writel(&pdx->host, 0xfc, pdx->pm_state.reg_0xfc);
	}
}
#endif

void failsafe_fct(bht_dev_ext_t *pdx)
{
	DbgInfo(MODULE_MAIN_PM, FEATURE_PM_TRACE, 0,
		"Enter %s, fail-safe: %d, chip type: 0x%x\n", __func__,
		pdx->cfg->driver_item.failsafe_en, pdx->host.chip_type);

	/* add s5_entered here to fix GG8-MP FPGA PM issue#2 */
	if ((pdx->pm_state.s3s4_entered) || (pdx->pm_state.s5_entered)) {
		if (pdx->cfg->driver_item.failsafe_en == 1) {
			if (pdx->host.chip_type == CHIP_SEAEAGLE) {
				/* Failsafe enable */
				pci_orl(&(pdx->host), 0x3E0, BIT6);
			}

			if (pdx->host.chip_type == CHIP_GG8
			    || pdx->host.chip_type == CHIP_ALBATROSS) {

				pci_andl(&(pdx->host), 0x408, ~(0x84210842));
				pci_andl(&(pdx->host), 0x410, ~(0x02108410));

			}
		}
	}

	DbgInfo(MODULE_MAIN_PM, FEATURE_PM_TRACE, 0, "Exit %s\n", __func__);
}

void pcie_weakup(bht_dev_ext_t *pdx, u32 Sx_flag, bool enable)
{
	DbgInfo(MODULE_MAIN_PM, FEATURE_PM_TRACE, 0, "Enter %s(%d) with S%d\n",
		__func__, enable, Sx_flag);

	if (pdx->host.chip_type != CHIP_GG8
	    && pdx->host.chip_type != CHIP_ALBATROSS) {
		DbgErr("Error: pcie weakup function only support GG8\n");
		return;
	}

	if (enable) {
		/* Driver load, Driver re-enable Card insert/remove source bit(0x468[20] = 1). */
		pci_orl(&(pdx->host), 0x468, (1 << 20));
	} else {
		switch (Sx_flag) {
		case ENTRY_S3:
			if (pdx->cfg->feature_item.pcie_wake_setting.s3_disable_wakeup)
				pci_andl(&(pdx->host), 0x468, ~(1 << 20));
			else
				DbgErr
				    ("Warning:registry setting for s3 is not ready\n");

			break;

		case ENTRY_S4:
			if (pdx->cfg->feature_item.pcie_wake_setting.s4_disable_wakeup)
				pci_andl(&(pdx->host), 0x468, ~(1 << 20));
			else
				DbgErr
				    ("Warning:registry setting for s4 is not ready\n");

			break;

		case ENTRY_S5:
			if (pdx->cfg->feature_item.pcie_wake_setting.s5_disable_wakeup)
				pci_andl(&(pdx->host), 0x468, ~(1 << 20));
			else
				DbgErr
				    ("Warning:registry setting for s5 is not ready\n");

			break;

		default:
			DbgErr
			    ("Error: only support 3/4/5!!! current value is %d\n",
			     Sx_flag);
			break;

		}
	}

	DbgInfo(MODULE_MAIN_PM, FEATURE_PM_TRACE, 0, "Exit %s\n", __func__);
}

void req_enter_d3(bht_dev_ext_t *pdx)
{
	DbgInfo(MODULE_MAIN_PM, FEATURE_PM_TRACE, 0, "Enter %s\n",
		__func__);

	func_autotimer_stop(pdx);
	card_power_off(&(pdx->card), FALSE);
	thermal_uninit(pdx);
	host_int_sig_dis(&(pdx->host), 0xffffffff);

	pdx->card.initialized_once = FALSE;
	card_stuct_init(pdx);

	DbgInfo(MODULE_MAIN_PM, FEATURE_PM_TRACE, 0, "Exit %s\n", __func__);

}
