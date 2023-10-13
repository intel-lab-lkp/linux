// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: linux_api.c
 *
 * Abstract: Linux api definition
 *
 * Version: 1.00
 *
 * Author: Peter
 *
 * Environment:	Linux
 *
 * History:
 *
 * 5/20/2015		Creation	Peter.Guo
 */

#include <linux/version.h>
#include "../include/basic.h"
#include "../include/debug.h"
#include "../include/util.h"
#include "../include/funcapi.h"
#include "../include/hostapi.h"
#include "../include/hostvenapi.h"
#include <linux/moduleparam.h>
#include <linux/kthread.h>
#include <linux/random.h>
#include <scsi/scsi_device.h>
#include "../linux_os/linux_scsi.h"
#if BHT_LINUX_ENABLE_RTD3
#include <linux/pm_runtime.h>
#endif

/* ----------linux Module param management---------- */

/* This Flag is used to control emmc */
static ulong m_emmc_mode;
module_param(m_emmc_mode, ulong, 0444);

/* Use to select dma mode, adma2 adma3 */
static ulong m_dma_mode;
module_param(m_dma_mode, ulong, 0444);

/* use to enable or disable infinite transfer */
static ulong m_infinite_ctrl = 0x8000000f;
module_param(m_infinite_ctrl, ulong, 0444);

/* use for tag queue capability */
static ulong m_tag_cap;
module_param(m_tag_cap, ulong, 0444);

/* use for sdmode disable */
static ulong m_sdmode_dis;
module_param(m_sdmode_dis, ulong, 0444);

/* use to disable mmc support */
static ulong m_mmc_dis;
module_param(m_mmc_dis, ulong, 0444);

/* uhs2 range/half/low select */
static ulong m_uhs2_ctrl = 0x97400012;
module_param(m_uhs2_ctrl, ulong, 0444);

/* output tuning control */
static ulong m_output_tuning = 0x98030036;
module_param(m_output_tuning, ulong, 0444);

/* auto stop infinite control */
static ulong m_auto_stopinf;
module_param(m_auto_stopinf, ulong, 0444);

/* auto poweroff card function */
static ulong m_auto_poweroff;
module_param(m_auto_poweroff, ulong, 0444);

/* uhs2 auto go dormant/hbr function */
static ulong m_auto_dmt;
module_param(m_auto_dmt, ulong, 0444);

/* Enable need set to 0x8004_0100 */
static ulong m_psd_mode = 0x80000000;
module_param(m_psd_mode, ulong, 0444);

/* Enable need set to 0x8004_0100 */
static ulong m_sw_sel_inject_sdr50 = 0x67f;
module_param(m_sw_sel_inject_sdr50, ulong, 0444);

static unsigned int pcr_setting_count;
char *pcr_settings[MAX_PCR_SETTING_SIZE] = { 0 };

module_param_array(pcr_settings, charp, &pcr_setting_count, 0444);
MODULE_PARM_DESC(pcr_settings,
		 "pcr_settings is a reg=value like string list split by comma");

static unsigned int dmdn_setting_count;
char *dmdn_settings[MAX_FREQ_SUPP] = { 0 };

module_param_array(dmdn_settings, charp, &dmdn_setting_count, 0444);
MODULE_PARM_DESC(dmdn_settings,
		 "dmdn_settings is a reg=value like string list split by comma");

uint m_sd_3v3_clk_driver_strength = 2;
module_param(m_sd_3v3_clk_driver_strength, uint, 0444);

uint m_sd_3v3_cmddata_driver_strength = 2;
module_param(m_sd_3v3_cmddata_driver_strength, uint, 0444);

uint m_sd_1v8_clk_driver_strength = 2;
module_param(m_sd_1v8_clk_driver_strength, uint, 0444);

uint m_sd_1v8_cmddata_driver_strength = 1;
module_param(m_sd_1v8_cmddata_driver_strength, uint, 0444);

uint m_cnfg_drv;
module_param(m_cnfg_drv, uint, 0444);

uint m_cnfg_trm_code_tx = 8;
module_param(m_cnfg_trm_code_tx, uint, 0444);

uint m_cnfg_trm_code_rx = 7;
module_param(m_cnfg_trm_code_rx, uint, 0444);

uint m_cnfg_rint_code = 9;
module_param(m_cnfg_rint_code, uint, 0444);

uint m_ram_ema = 0x42492000;
module_param(m_ram_ema, uint, 0444);

uint m_vdd1_vdd2_source;
module_param(m_vdd1_vdd2_source, uint, 0444);

uint m_vdd18_debounce_time = 1;
module_param(m_vdd18_debounce_time, uint, 0444);

uint m_ssc_enable;
module_param(m_ssc_enable, uint, 0444);

static ulong m_driver_item = 0x8080000a;
module_param(m_driver_item, ulong, 0444);

/* sd7_sd_mode_switch_control, 300: camera, 100: pc */
static ulong m_sd7_sdmode_switch_control = 0x00000300;
module_param(m_sd7_sdmode_switch_control, ulong, 0444);

/* mmc_mode_dis */
static ulong m_mmc_mode_dis = 0x80000000;
module_param(m_mmc_mode_dis, ulong, 0444);

/* pcie_wake_setting */
static ulong m_pcie_wake_setting;
module_param(m_pcie_wake_setting, ulong, 0444);

/* sd_card_mode_dis */
static ulong m_sd_card_mode_dis = 0x80000021;
module_param(m_sd_card_mode_dis, ulong, 0444);

/* vdd_power_source_item */
static ulong m_vdd_power_source_item = 0x00090a0c;
module_param(m_vdd_power_source_item, ulong, 0444);

/* hsmux_vcme_enable */
static ulong m_hsmux_vcme_enable;
module_param(m_hsmux_vcme_enable, ulong, 0444);

/* test_max_access_mode */
static ulong m_test_max_access_mode = 0x80000005;
module_param(m_test_max_access_mode, ulong, 0444);

/* host_drive_strength */
static ulong m_host_drive_strength = 0x80001125;
module_param(m_host_drive_strength, ulong, 0444);

/* auto_detect_refclk_counter_range_ctl */
static ulong m_auto_detect_refclk_counter_range_ctl = 0x80640000;
module_param(m_auto_detect_refclk_counter_range_ctl, ulong, 0444);

/* refclk_stable_detection_counter1 */
static ulong m_refclk_stable_detection_counter1 = 0x00000003;
module_param(m_refclk_stable_detection_counter1, ulong, 0444);

static ulong m_refclk_stable_detection_counter2 = 0x001e044c;
module_param(m_refclk_stable_detection_counter2, ulong, 0444);

static ulong m_refclk_stable_detection_counter3 = 0x04b024b0;
module_param(m_refclk_stable_detection_counter3, ulong, 0444);

static ulong m_l1_enter_exit_logic_ctl;
module_param(m_l1_enter_exit_logic_ctl, ulong, 0444);

/* pcie_phy_amplitude_adjust */
static ulong m_pcie_phy_amplitude_adjust = 0x0000006a;
module_param(m_pcie_phy_amplitude_adjust, ulong, 0444);

static ulong m_output_tuning_item = 0xc01837bb;
module_param(m_output_tuning_item, ulong, 0444);

static ulong m_test_driver_strength_sel;
module_param(m_test_driver_strength_sel, ulong, 0444);

static ulong m_test_max_power_limit = 0x80000003;
module_param(m_test_max_power_limit, ulong, 0444);

static ulong m_test_uhs2_setting2 = 0x80000006;
module_param(m_test_uhs2_setting2, ulong, 0444);

static ulong m_test_sdma_boundary = 0x00000020;
module_param(m_test_sdma_boundary, ulong, 0444);

static ulong m_test_dma_mode_setting = 0x80000071;
module_param(m_test_dma_mode_setting, ulong, 0444);

static ulong m_test_ocb_ctrl;
module_param(m_test_ocb_ctrl, ulong, 0444);

static ulong m_bios_l1_substate = 0x8000000f;
module_param(m_bios_l1_substate, ulong, 0444);

static ulong m_test_main_ldo_setting;
module_param(m_test_main_ldo_setting, ulong, 0444);

static ulong m_auto_sleep_control;
module_param(m_auto_sleep_control, ulong, 0444);

static ulong m_power_wait_time = 0x0024000a;
module_param(m_power_wait_time, ulong, 0444);

static ulong m_test_write_data_timeout = 0x80001770;
module_param(m_test_write_data_timeout, ulong, 0444);

static ulong m_test_read_data_timeout = 0x80001770;
module_param(m_test_read_data_timeout, ulong, 0444);

static ulong m_test_non_data_timeout = 0x800003e8;
module_param(m_test_non_data_timeout, ulong, 0444);

static ulong m_test_r1b_data_timeout = 0x80001194;
module_param(m_test_r1b_data_timeout, ulong, 0444);

static ulong m_test_card_init_timeout = 0x800005dc;
module_param(m_test_card_init_timeout, ulong, 0444);

extern cfg_item_t g_cfg[SUPPORT_CHIP_COUNT][2];

struct kmem_cache *srb_ext_cachep;

/*
 *  SD Spec Standard Memory register access
 */

u32 sdhci_readl(sd_host_t *host, u16 offset)
{
	u32 regval = 0;

	regval = readl(host->pci_dev.membase + offset);
	DbgInfo(MODULE_SD_HOST, FEATURE_SDREG_TRACER, NOT_TO_RAM,
		"%s(0x%08X): 0x%08X\n", __func__, offset, regval);

	return regval;
}

void sdhci_writel(sd_host_t *host, u16 offset, u32 value)
{
	DbgInfo(MODULE_SD_HOST, FEATURE_SDREG_TRACEW, NOT_TO_RAM,
		"%s(0x%08X): 0x%08X\n", __func__, offset, value);
	writel(value, host->pci_dev.membase + offset);
}

void sdhci_or32(sd_host_t *host, u16 offset, u32 value)
{
	u32 regval = 0;

	regval = sdhci_readl(host, offset);
	regval |= value;
	sdhci_writel(host, offset, regval);
}

void sdhci_and32(sd_host_t *host, u16 offset, u32 value)
{
	u32 regval = 0;

	regval = sdhci_readl(host, offset);
	regval &= (value);
	sdhci_writel(host, offset, regval);

}

void sdhci_writew(sd_host_t *host, u16 offset, u16 value)
{
	writew(value, host->pci_dev.membase + offset);
	DbgInfo(MODULE_SD_HOST, FEATURE_SDREG_TRACEW, NOT_TO_RAM,
		"%s(0x%04X): 0x%08X\n", __func__, offset, value);

}

u16 sdhci_readw(sd_host_t *host, u16 offset)
{
	u16 regval = 0;

	regval = readw(host->pci_dev.membase + offset);
	DbgInfo(MODULE_SD_HOST, FEATURE_SDREG_TRACER, NOT_TO_RAM,
		"%s(0x%04X): 0x%08X\n", __func__, offset, regval);

	return regval;
}

void sdhci_or16(sd_host_t *host, u16 offset, u16 value)
{
	u16 regval = 0;

	regval = sdhci_readw(host, offset);
	regval |= value;
	sdhci_writew(host, offset, regval);
}

void sdhci_and16(sd_host_t *host, u16 offset, u16 value)
{
	u16 regval = 0;

	regval = sdhci_readw(host, offset);
	regval &= value;
	sdhci_writew(host, offset, regval);
}

/*
 *  Vendor Memory register access
 */

/* 16bit access */
u16 ven_readw(sd_host_t *host, u16 offset)
{
	u16 regval = 0;

	regval = readw(host->pci_dev.membase2 + offset);

	DbgInfo(MODULE_SD_HOST, FEATURE_VENREG_TRACER, NOT_TO_RAM,
		"[Memory Base 2]  Readw(0x%08X): 0x%08X\n", offset, regval);
	return regval;
}

void ven_writew(sd_host_t *host, u16 offset, u16 value)
{

	writew(value, host->pci_dev.membase2 + offset);
	DbgInfo(MODULE_SD_HOST, FEATURE_VENREG_TRACEW, NOT_TO_RAM,
		"[Memory Base 2] Writew(0x%08X): 0x%08X\n", offset, value);

}

void ven_or16(sd_host_t *host, u16 offset, u16 value)
{
	u16 regval = 0;

	regval = ven_readw(host, offset);
	regval |= value;
	ven_writew(host, offset, regval);
}

void ven_and16(sd_host_t *host, u16 offset, u16 value)
{
	u16 regval = 0;

	regval = ven_readw(host, offset);
	regval &= (value);
	ven_writew(host, offset, regval);
}

/* 32bit access */
u32 ven_readl(sd_host_t *host, u16 offset)
{
	u32 regval = 0;

	regval = readl(host->pci_dev.membase2 + offset);

	DbgInfo(MODULE_SD_HOST, FEATURE_VENREG_TRACER, NOT_TO_RAM,
		"[Memory Base 2]  Readl(0x%08X): 0x%08X\n", offset, regval);
	return regval;
}

void ven_writel(sd_host_t *host, u16 offset, u32 value)
{

	writel(value, host->pci_dev.membase2 + offset);

	DbgInfo(MODULE_SD_HOST, FEATURE_VENREG_TRACEW, NOT_TO_RAM,
		"[Memory Base 2] Writel(0x%08X): 0x%08X\n", offset, value);

}

void ven_or32(sd_host_t *host, u16 offset, u32 value)
{
	u32 regval = 0;

	regval = ven_readl(host, offset);
	regval |= value;
	ven_writel(host, offset, regval);

}

void ven_and32(sd_host_t *host, u16 offset, u32 value)
{
	u32 regval = 0;

	regval = ven_readl(host, offset);
	regval &= (value);
	ven_writel(host, offset, regval);

}

/*
 *  PCI config register accessing
 */
void pci_port_writel(sd_host_t *host, u32 port, u32 data)
{

	pci_write_config_dword(host->pci_dev.pci_dev, port, data);

	DbgInfo(MODULE_SD_HOST, FEATURE_PCIREG_TRACEW, NOT_TO_RAM,
		"%s(0x%08X): 0x%08X\n", __func__, port, data);

}

u32 pci_port_readl(sd_host_t *host, u32 port)
{
	u32 regval = 0;

	pci_read_config_dword(host->pci_dev.pci_dev, port, &regval);

	DbgInfo(MODULE_SD_HOST, FEATURE_PCIREG_TRACER, NOT_TO_RAM,
		"%s(0x%08X): 0x%08X\n", __func__, port, regval);

	return regval;
}

/*
 *  PCI config register accessing by configuration IO method
 */
void pci_cfgio_writel(sd_host_t *host, u16 offset, u32 value)
{
	pci_write_config_dword(host->pci_dev.pci_dev, offset, value);
}

u32 pci_cfgio_readl(sd_host_t *host, u16 offset)
{
	u32 regval = 0;

	pci_read_config_dword(host->pci_dev.pci_dev, offset, &regval);
	DbgInfo(MODULE_SD_HOST, FEATURE_PCIREG_TRACER, NOT_TO_RAM,
		"%s(0x%08X): 0x%08X\n", __func__, offset, regval);

	return regval;
}

/*
 *
 * Function Name: timer_auto_cb
 *
 * Abstract:
 *
 *			for timer callback
 *
 * Input:
 *
 *			PVOID	pctx [in]: Pointer to  pdx
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			null
 *
 * Notes:
 *
 *           Caller: OS
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0)
void timer_auto_cb2(struct timer_list *t)
{
	os_struct *o = from_timer(o, t, timer);
	bht_dev_ext_t *pdx = container_of(o, bht_dev_ext_t, os);

	func_timer_callback(pdx);
}
#else
void timer_auto_cb2(PVOID pdx)
{
	func_timer_callback(pdx);
}
#endif

/*
 *
 * Function Name: timer_subid_cb
 *
 * Abstract:
 *
 *			  for timer callback
 *
 * Input:
 *
 *			PVOID	pctx [in]: Pointer to  pdx
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			null
 *
 * Notes:
 *
 *           Caller: OS
 */

void timer_subid_cb(PVOID ctx, PVOID pdx)
{
	/* todo */
}

/*
 * Function Name: os_start_timer
 * Abstract: This Function is used to start a timer
 *
 * Input:
 *			os_struct *os,
 *			e_timer_t t timer id
 *			u32 time_ms
 *
 *
 *
 * IRQL: any
 *
 *        so giving the routine another name requires you to modify the build tools.
 */

void os_start_timer(void *p, os_struct *os, e_timer_t t, u32 time_ms)
{
	if (t >= MAX_TIMER_NUM)
		return;

	if ((u32) t != 0) {
		DbgErr("Timer %d not support now\n", t);
		return;
	}

	mod_timer(&os->timer, jiffies + msecs_to_jiffies(time_ms));
}

/*
 * Function Name: os_cancel_timer
 * Abstract: This Function is used to start a timer
 *
 * Input:
 *			os_struct *os,
 *			e_timer_t t timer id
 *			u32 time_ms
 *
 *
 * IRQL:any
 *
 *        so giving the routine another name requires you to modify the build tools.
 */
void os_cancel_timer(void *p, os_struct *os, e_timer_t t)
{

	if (t >= MAX_TIMER_NUM)
		return;

	del_timer(&os->timer);

}

void os_stop_timer(void *p, os_struct *os, e_timer_t t)
{

	if (t >= MAX_TIMER_NUM)
		return;
}

/*
 *
 * Function Name:  os_set_event
 *
 * Abstract:
 *
 *			 1. set event to thread
 *
 * Input:
 *
 *			os_struct *os [in]: pointer to the OS structure
 *			e_event_t event [in]: the event need set
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			None
 *
 * IRQL:any
 *
 *
 */

void os_set_event(os_struct *os, e_event_t event)
{
	spin_lock(&os->event.lock);

	set_bit((int)event, &os->event.evt_flag);

	atomic_set(&os->event.evt_comming, 1);

	spin_unlock(&os->event.lock);

	wake_up_interruptible(&os->event.evt_control);
}

/*
 *
 * Function Name:  os_clear_event
 *
 * Abstract:
 *
 *			 1. clear event to thread
 *
 * Input:
 *
 *			os_struct *os [in]: pointer to the OS structure
 *			e_event_t event [in]: the event need send
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			None
 *
 *
 *
 */

void os_clear_event(os_struct *os, e_event_t event)
{
	/* nothing to do as event is auto cleared  by os_wait_event */
}

/*
 *
 * Function Name:  os_wait_event
 *
 * Abstract:
 *
 *			 1. wait any one signaled event of  multiple events
 *
 * Input:
 *
 *			os_struct *os [in]: pointer to the OS structure
 *			s32 timeout [in]: timeout value
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			None
 *
 *
 *
 */
e_event_t os_wait_event(os_struct *os)
{
	e_event_t event = EVENT_NONE;
	int i;

	if (atomic_read(&os->event.evt_comming))
		goto next;

	wait_event_interruptible(os->event.evt_control,
				 ((atomic_read(&os->event.evt_comming) != 0)
				  || (kthread_should_stop())));

next:
	if (kthread_should_stop()) {
		event = EVENT_TERMINATE;
		clear_bit(event, &os->event.evt_flag);
		atomic_set(&os->event.evt_comming, 0);
		goto exit;
	}

	if (atomic_read(&os->event.evt_comming) == 0)
		goto exit;

	spin_lock(&(os->event.lock));

	for (i = 0; i < EVENT_NONE; i++) {
		if (test_bit(i, &os->event.evt_flag)) {
			event = i;
			clear_bit(i, &os->event.evt_flag);
			break;
		}
	}

	if (os->event.evt_flag == 0)
		atomic_set(&os->event.evt_comming, 0);

	spin_unlock(&(os->event.lock));

exit:
	return event;

}

/*
 *
 * Function Name:  os_create_thread
 *
 * Abstract:
 *
 *			 1. create thread
 *
 * Input:
 *
 *			thread_t *[in]: pointer to the thread entry
 *			void *param, [in]:  the parameter which pass to thread entity
 *			thread_cb_t func [in]: the entry of thread enity
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			TRUE: create ok
 *			FALSE: create failed
 *
 *
 *
 */
bool os_create_thread(thread_t *thr, void *param, thread_cb_t func)
{

	thr->pthread =
	    kthread_run((void *)func, (PVOID) param, "bhtsd_scsi_thread");
	if (thr->pthread == NULL)
		return FALSE;
	return TRUE;
}

/*
 *
 * Function Name:  os_stop_thread
 *
 * Abstract:
 *
 *			 1. stop the thread
 *
 * Input:
 *
 *			thread_t *[in]: pointer to the thread entry
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			TRUE: stop ok
 *			FALSE:stop failed
 *
 *
 *
 */
bool os_stop_thread(os_struct *os, thread_t *thr)
{
	bool ret = FALSE;
	int i = 0;

	DbgInfo(MODULE_OS_API, FEATURE_THREAD_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if (thr != NULL && thr->pthread != NULL && os != NULL) {
		os->thread.freeze = TRUE;
		os_set_event(os, EVENT_TERMINATE);
		kthread_stop(os->thread.pthread);
		while (test_bit(EVENT_TERMINATE, &os->event.evt_flag)) {
			os_mdelay(20);
			i++;
			if (i > 150)
				goto exit;
		}

		ret = TRUE;
	}
exit:
	DbgInfo(MODULE_OS_API, FEATURE_THREAD_TRACE, NOT_TO_RAM,
		"Exit %s  ret=%xh\n", __func__, ret);
	return ret;
}

void os_kill_thread(os_struct *os, thread_t *thr)
{
}

bool os_thread_is_freeze(void *pdx)
{
	os_struct *os = &((bht_dev_ext_t *) pdx)->os;

	if (os->thread.freeze || os->thread.pthread == NULL)
		return TRUE;
	else
		return FALSE;
}

/*
 *
 * Function Name:  os_pending_thread
 *
 * Abstract:
 *
 *			 pending thread or resume thread
 *
 *
 * Input:
 *
 *			thread_t *[in]: pointer to the thread entry
 *			bool pending [in]: TRUE means pending thread
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *			TRUE: operate ok
 *			FALSE:operate failed
 *
 * IRQL:ANY
 *
 *
 */
bool os_pending_thread(void *pdx, bool pending)
{
	os_struct *os = &((bht_dev_ext_t *) pdx)->os;
	u32 i = 600;
	bool ret = TRUE;

	DbgInfo(MODULE_OS_API, FEATURE_THREAD_TRACE, NOT_TO_RAM, "Enter %s\n",
		__func__);
	if (os->thread.pthread == NULL) {
		DbgErr("thread obj null\n");
		return FALSE;
	}

	os->thread.pending_lock = FALSE;
	if (pending == TRUE) {
		ret = FALSE;
		os_init_completion(pdx, &os->thread.break_pending);
		os->thread.freeze = TRUE;
		os_set_event(os, EVENT_PENDING);
		while (i--) {
			if (os->thread.pending_lock == TRUE) {
				ret = TRUE;
				break;
			}
			os_mdelay(20);
		}

	} else {
		os_finish_completion(pdx, &os->thread.break_pending);
		os->thread.freeze = FALSE;

	}

	if (ret == FALSE)
		DbgErr("%s timeout\n", __func__);
	DbgInfo(MODULE_OS_API, FEATURE_THREAD_TRACE, NOT_TO_RAM,
		"Exit %s  ret=%xh\n", __func__, ret);
	return ret;
}

/*
 * completion API
 */

/*
 *
 * Function Name:  os_init_completion
 *
 * Abstract:
 *
 *			 1.init the completion.
 *
 * Input:
 *
 *			completion_t *p[in]: pointer to completion
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *
 * IRQL:any
 *
 *
 */

void os_init_completion(void *pdx, completion_t *p)
{
	init_completion(p);
}

/*
 *
 * Function Name: os_finish_completion
 *
 * Abstract:
 *
 *			 1.wake the thread which wait for this completion.
 *
 * Input:
 *
 *			completion_t *p[in]: pointer to completion
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *
 * Notes:
 *
 *
 */
void os_finish_completion(void *pdx, completion_t *p)
{
	complete(p);
}

/*
 *
 * Function Name: os_finish_completion
 *
 * Abstract:
 *
 *			 1.wake the thread which wait for this completion.
 *
 * Input:
 *
 *			completion_t *p[in]: pointer to completion
 *			s32 timeout [in]: unit ms .if timeout =0,
 *			mean wait for signal until it completed
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *
 * Notes:
 *
 *
 */

bool os_wait_for_completion(void *pdx, completion_t *p, s32 timeout)
{
	s32 ret = wait_for_completion_timeout(p, msecs_to_jiffies(timeout));

	if (ret == 0)
		return FALSE;
	else
		return TRUE;
}

/*
 *
 * Function Name:os_list_locked_remove_head
 *
 * Abstract:
 *
 *     remove the list_entry to list head node
 *
 * Input:
 *
 *   list_t *p [in]: pointer to list
 *
 * Output:
 *
 *    None
 *
 * Return value:
 *
 *
 * IRQL:ANY
 *
 *
 */
list_entry *os_list_locked_remove_head(list_t *p)
{
	unsigned long flags;
	list_entry *entry = NULL;

	spin_lock_irqsave(&p->lock, flags);

	if (list_empty(&p->list_hd))
		goto exit;

	entry = p->list_hd.next;
	list_del_init(entry);

exit:
	spin_unlock_irqrestore(&p->lock, flags);
	/* need with verify with chuanjing */
	return entry;
}

/*
 *
 * Function Name: os_list_locked_insert_tail
 *
 * Abstract:
 *
 *     insert the list etry to list tail
 *
 * Input:
 *
 *   list_t *p [in]: pointer to list
 *
 * Output:
 *
 *    None
 *
 * Return value:
 *
 *
 * Notes:
 *
 *
 */
void os_list_locked_insert_tail(list_t *p, list_entry *entry)
{
	unsigned long flags;

	spin_lock_irqsave(&p->lock, flags);

	list_add_tail(entry, &p->list_hd);

	spin_unlock_irqrestore(&p->lock, flags);
}

/*
 *
 * Function Name: os_list_locked_insert_head
 *
 * Abstract:
 *
 *     insert the node to list head
 *
 * Input:
 *
 *   list_t *p [in]: pointer to list
 *
 * Output:
 *
 *    None
 *
 * Return value:
 *
 *
 * Notes:
 *
 *
 */
void os_list_locked_insert_head(list_t *p, list_entry *entry)
{
	unsigned long flags;

	spin_lock_irqsave(&p->lock, flags);

	list_add(entry, &p->list_hd);

	spin_unlock_irqrestore(&p->lock, flags);

}

/*
 *
 * Function Name: os_list_init
 *
 * Abstract:
 *
 *			  init the list with lock
 *
 * Input:
 *
 *			list_t *p [in]: pointer to list
 *
 * Output:
 *
 *			 None
 *
 * Return value:
 *
 *
 * Notes:
 *
 *
 */
void os_list_init(list_t *p)
{
	p->lock = __SPIN_LOCK_UNLOCKED(p->lock);
	INIT_LIST_HEAD(&p->list_hd);
	os_atomic_set(&p->cnt, 0);

}

/*
 *
 * Function Name:  os_layer_init
 *
 * Abstract:
 *
 *			 1.init the os layer.
 *
 * Input:
 *
 *			os_struct *os [in]: pointer to os structure
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *
 * irql:any
 *
 *
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0)
typedef void (*func_timer)(struct timer_list *);
#else
typedef void (*func_timer)(unsigned long);
#endif

bool os_layer_init(void *p, os_struct *os)
{

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0)
	timer_setup(&os->timer, timer_auto_cb2, 0);
#else
	init_timer(&os->timer);
	os->timer.data = (unsigned long)pdx;
	os->timer.function = (func_timer) timer_auto_cb2;
#endif
	os->lock = __SPIN_LOCK_UNLOCKED(os->lock);

	atomic_set(&os->event.evt_comming, 0);
	init_waitqueue_head(&os->event.evt_control);
	os->rt_pm_cnt = 0;
	os->event.evt_flag = 0;
	os->dma_mapped = 0;
	os->virt_buff = vmalloc(CFG_MAX_TRANSFER_LENGTH);

	if (os->virt_buff == NULL) {
		DbgErr("Vmalloc failed for %d length\n",
		       CFG_MAX_TRANSFER_LENGTH);
	}

	DbgInfo(MODULE_OS_API, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return TRUE;
}

/*
 *
 * Function Name:  os_layer_uinit
 *
 * Abstract:
 *
 *			 1.uninit the os layer
 *
 * Input:
 *
 *			os_struct *os [in]: pointer to os structure
 *
 * Output:
 *
 *			None
 *
 * Return value:
 *
 *
 * Notes:
 *
 *
 */
bool os_layer_uinit(void *pdx, os_struct *os)
{
	DbgInfo(MODULE_OS_API, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	os_cancel_timer(pdx, os, (e_timer_t) 0);
	if (os->virt_buff != NULL)
		vfree(os->virt_buff);

	return TRUE;
}

/*
 *
 * Function Name:  os_alloc_dma_buffer
 *
 * Abstract:
 *
 *			 1. allocate DMA buffer resource
 *
 * Input:
 *
 *			bht_dev_ext_t *pdx [in]: pointer to device extension
 *			PPORT_CONFIGURATION_INFORMATION ConfigInfo [in]:
 *				pointer to config information
 *			u32 nbytes [in]: the size allocate
 *			dma_desc_buf_t *pdma [in]: pointer to the DMA information,
 *				include physical address & virtual address
 *
 * Output:
 *
 *			dma_desc_buf_t *pdma [out]: pointer to the DMA information,
 *				include physical address & virtual address
 *
 * Return value:
 *
 *
 * Notes:
 *
 *
 */
bool os_alloc_dma_buffer(void *p, void *ctx, u32 nbytes, dma_desc_buf_t *pdma)
{
#define DMA_BUF_ALIGN_SIZE (1<<12)
	u32 retry = 0;
	bht_dev_ext_t *pdx = p;

	if (pdma->len < DMA_BUF_ALIGN_SIZE)
		pdma->len = DMA_BUF_ALIGN_SIZE;

	pdma->len = nbytes;
	DbgInfo(MODULE_OS_API, FEATURE_DRIVER_INIT, NOT_TO_RAM,
		"Enter %s size=%xh\n", __func__, nbytes);

	for (retry = 1; retry < 10; retry++) {
		pdma->pa = 0;
		pdma->va =
		    (void *)dmam_alloc_coherent(&pdx->host.pci_dev.pci_dev->dev,
						pdma->len,
						(dma_addr_t *) (&pdma->pa),
						GFP_KERNEL);

		if (pdma->va == NULL) {
			DbgInfo(MODULE_OS_API, FEATURE_DRIVER_INIT, 0,
				"os alloc dma buffer failed!!!,retry %d\n",
				retry);
		} else {
			pdx->os.dma_info = *pdma;
			break;
		}
	}
	if (pdma->va == NULL) {
		memset(&pdx->os.dma_info, 0, sizeof(dma_desc_buf_t));
		DbgErr("dmam_alloc_coherent(%xh) failed!!!\n", pdma->len);
		return FALSE;
	} else {
		DbgInfo(MODULE_OS_API, FEATURE_DRIVER_INIT, 0,
			"GetUncachedExtension return OK with length 0x%x\n",
			pdma->len);
		DbgInfo(MODULE_OS_API, FEATURE_DRIVER_INIT, 0,
			" - Before alignment, DMA buffer physical address =h[%x] l[%X]\n",
			os_get_phy_addr32h(pdma->pa),
			os_get_phy_addr32l(pdma->pa));
		DbgInfo(MODULE_OS_API, FEATURE_DRIVER_INIT, 0,
			" - Before alignment, DMA buffer virtual address = 0x%Xh\n",
			pdma->va);
	}
	DbgInfo(MODULE_OS_API, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);

	return TRUE;
}

/*
 *
 * Function Name: os_free_dma_buffer
 *
 * Abstract:
 *
 *			 1. free DMA buffer resource
 *
 * Input:
 *
 *			bht_dev_ext_t *pdx [in]: pointer to device extension
 *			dma_desc_buf_t *pdma [in]: pointer to the DMA information,
 *				include physical address & virtual address
 *
 * Output:
 *
 *
 *
 * Return value:
 *
 *
 * Notes:
 */
bool os_free_dma_buffer(void *p, dma_desc_buf_t *dma)
{
	bht_dev_ext_t *pdx = p;
	dma_desc_buf_t *pdma = &pdx->os.dma_info;

	DbgInfo(MODULE_OS_API, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);

	if (pdma->len == 0)
		goto exit;

	dmam_free_coherent(&pdx->host.pci_dev.pci_dev->dev, pdma->len, pdma->va,
			   (dma_addr_t) pdma->pa);
	pdma->pa = 0;
	pdma->va = 0;
	pdma->len = 0;
exit:
	DbgInfo(MODULE_OS_API, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);
	return TRUE;
}

/*
 *
 * Function Name: _os_sleep
 *
 * Abstract:
 *
 *			 1. sleep current thread
 *
 * Input:
 *
 *			u32 time_ms [in]:  sleep times
 *
 * Output:
 *
 *
 *
 * Return value:
 *			FALSE : means can't sleep, reture immediately
 *			TRUE: sleep ok
 *
 * Notes:
 *			Delay in ms. It won't ties up the CPU.
 *			But it is not so accurate. And it must be called in PASSIVE_LEVEL
 *
 */
void os_sleep(u32 time_ms)
{
	u32 tick = os_get_cur_tick();

	while (os_is_timeout(tick, time_ms) == FALSE)
		schedule();
}

/*
 *
 * Function Name: os_udelay
 *
 * Abstract:
 *
 *			 1. CPU busy delay
 *
 * Input:
 *
 *			u32 time_us [in]:  busy delay times
 *
 * Output:
 *
 *
 *
 * Return value:
 *
 * Notes:
 *
 */

void os_udelay(u32 time_us)
{
	udelay(time_us);
}

/*
 *
 * Function Name: os_mdelay
 *
 * Abstract:
 *
 *			 1. CPU busy delay
 *
 * Input:
 *
 *			u32 time_ms [in]:  busy delay times, unit ms
 *
 * Output:
 *
 *
 *
 * Return value:
 *
 * Notes:
 *
 */
void os_mdelay(u32 time_ms)
{
	mdelay(time_ms);
}

/*
 *
 * Function Name: os_print
 *
 * Abstract:
 *
 *			 1. print string to debugview
 *
 * Input:
 *
 *			byte * s [in]:  pointer to string
 *
 * Output:
 *
 *
 *
 * Return value:
 *
 * Notes:
 *
 */

void os_print(byte *s)
{
	printk(s);
}

/*
 *
 * Function Name: os_alloc_vbuff
 *
 * Abstract:
 *
 *			 1. allocate virtual buffer
 *
 * Input:
 *
 *			u32 length [in]:  buffer length to allocate
 *
 * Output:
 *
 *
 *
 * Return value:
 *
 *			the buffer address which allocate
 *
 * Notes:
 *
 */

void *os_alloc_vbuff(u32 length)
{
	return kmalloc(length, GFP_KERNEL);
}

/*
 *
 * Function Name: os_free_vbuff
 *
 * Abstract:
 *
 *			 1. free the virtual buffer
 *
 * Input:
 *
 *			u32 length [in]:  buffer length to free
 *
 * Output:
 *
 *
 *
 * Return value:
 *
 *			none
 *
 * Notes:
 *
 */
void os_free_vbuff(void *vbuff)
{
	kfree(vbuff);
}

/*
 *
 * Function Name: os_get_phy_addr32l
 *
 * Abstract:
 *
 *			 1.  get physical address low 32bit value
 *
 * Input:
 *
 *
 *
 * Output:
 *
 *
 *
 * Return value:
 *
 *			none
 *
 * Notes:
 *
 */
u32 os_get_phy_addr32l(phy_addr_t phy_addr)
{
	return (phy_addr & 0x000000FFFFFFFF);
}

u64 os_get_phy_addr64(phy_addr_t phy_addr)
{
	return phy_addr;
}

/*
 *
 * Function Name: os_get_phy_addr32h
 *
 * Abstract:
 *
 *			 1.  get physical address high 32bit value
 *
 * Input:
 *
 *
 *
 * Output:
 *
 *
 *
 * Return value:
 *
 *			none
 *
 * Notes:
 *
 */
u32 os_get_phy_addr32h(phy_addr_t phy_addr)
{
	return ((phy_addr & 0xFFFFFFFF00000000) >> 32);
}

/*
 *
 * Function Name: os_set_phy_addr32l
 *
 * Abstract:
 *
 *			 1.  set physical address low 32bit value
 *
 * Input:
 *
 *
 *
 * Output:
 *
 *
 *
 * Return value:
 *
 *			none
 *
 * Notes:
 *
 */
void os_set_phy_addr32l(phy_addr_t *phy_addr, u32 addr)
{
	u32 *haddr = (u32 *) phy_addr;
	*haddr = addr;

}

/*
 *
 * Function Name: os_set_phy_addr32h
 *
 * Abstract:
 *
 *			 1.  set physical address high 32bit value
 *
 * Input:
 *
 *
 *
 * Output:
 *
 *
 *
 * Return value:
 *
 *			none
 *
 * Notes:
 *
 */
void os_set_phy_addr32h(phy_addr_t *phy_addr, u32 addr)
{
	u32 *haddr = (u32 *) phy_addr;

	haddr++;
	*haddr = addr;
}

/*
 *
 * Function Name: os_set_phy_add64
 *
 * Abstract:
 *
 *			 1.  set 64bit physical address
 *
 * Input:
 *
 *
 *
 * Output:
 *
 *
 *
 * Return value:
 *
 *			none
 *
 * Notes:
 *
 */
void os_set_phy_add64(phy_addr_t *phy_addr, u64 addr)
{

	*phy_addr = addr;

}

/*
 *
 * Function Name: srb_parse_sgl
 *
 * Abstract:
 *
 *			 1.  get the SG list information for the SRB
 *
 * Input:
 *
 *
 *
 * Output:
 *
 *
 *
 * Return value:
 *
 *			none
 *
 * Notes:
 *
 */
u32 os_get_sg_list(void *p, scsi_srb *pSrb, sg_list_t *sg)
{
	bht_dev_ext_t *pdx = p;
	u32 sg_count = 0;
	u32 i = 0;
	u32 k = 0;
	struct scatterlist *pAddList = NULL;
	u32 dma_64bit =
	    pdx->cfg->host_item.test_dma_mode_setting.enable_dma_64bit_address;

	sg_count =
	    dma_map_sg(&pdx->host.pci_dev.pci_dev->dev, scsi_sglist(pSrb),
		       scsi_sg_count(pSrb), pSrb->sc_data_direction);
	if (sg_count > 0)
		pdx->os.dma_mapped = TRUE;

	scsi_for_each_sg(pSrb, pAddList, sg_count, k) {
		if (dma_64bit)
			sg[i].Address =
			    os_get_phy_addr64(sg_dma_address(pAddList));
		else
			sg[i].Address =
			    os_get_phy_addr32l(sg_dma_address(pAddList));

		sg[i].Length = sg_dma_len(pAddList);
		if (sg[i].Length % 4) {
			DbgErr
			    ("PortGetPhysicalAddress() return unaligned length: 0x%x!\n",
			     sg[i].Length);
			return 0;
		}
		i++;
	}

	return i;
}

void os_free_sg_list(void *p, scsi_srb *Srb)
{
	bht_dev_ext_t *pdx = p;

	if (pdx->os.dma_mapped) {
		pdx->os.dma_mapped = FALSE;
		dma_unmap_sg(&pdx->host.pci_dev.pci_dev->dev, scsi_sglist(Srb),
			     scsi_sg_count(Srb), Srb->sc_data_direction);
	}
}

/*
 *
 * Function Name: os_set_dev_busy
 *
 * Abstract:
 *
 *			 1. notify Power manager device is busy state
 *
 * Input:
 *
 *			void *p [in]: pointer to device extension
 *
 * Output:
 *
 *
 *
 * Return value:
 *
 *			none
 *
 * Notes:
 *
 */
#if BHT_LINUX_ENABLE_RTD3
void os_set_dev_busy(void *p)
{
	bht_dev_ext_t *pdx = p;

	if (pdx->pm_state.rtd3_en == FALSE)
		return;
	pdx->os.rt_pm_cnt++;
	if (pdx->os.rt_pm_cnt == 1)
		pm_runtime_get_sync(&pdx->host.pci_dev.pci_dev->dev);
}

void os_set_dev_idle(void *p)
{
	bht_dev_ext_t *pdx = p;

	if (pdx->pm_state.rtd3_en == FALSE)
		return;
	if (pdx->os.rt_pm_cnt == 0) {
		DbgErr("rt pm set idle cnt not right\n");
	} else {

		pdx->os.rt_pm_cnt--;
		if (pdx->os.rt_pm_cnt == 0) {
			pm_runtime_mark_last_busy(&pdx->host.pci_dev.pci_dev->dev);
			pm_runtime_put_autosuspend(&pdx->host.pci_dev.pci_dev->dev);
		}
	}
}
#else
void os_set_dev_busy(void *p)
{
}

void os_set_dev_idle(void *pdx)
{
}

#endif

void os_bus_change(void *p)
{
	bht_dev_ext_t *pdx = p;

	DbgErr("Enter os bus change with (%d)\n", pdx->card.card_present);

	if (pdx->card.card_present)
		bht_scsi_init(pdx, pdx->dev);
	else
		bht_scsi_uinit(pdx);

}

u32 os_get_cur_tick(void)
{
	return jiffies_to_msecs(jiffies);
}

bool os_is_timeout(u32 start_tck, u32 time_ms)
{
	if ((start_tck + time_ms) <= os_get_cur_tick())
		return TRUE;
	else
		return FALSE;
}

/*
 *
 * Function Name: os_memset
 *
 * Abstract:
 *
 *			Fill a block of memory with zeros,
 *			given a pointer to the block and the length,
 *			in bytes, to be filled.
 *
 * Input:
 *
 *
 *                          memory_len [in]: The number of bytes to fill with zeros.
 *
 *
 * Output:
 *
 *			buffer [out]: A pointer to the memory block to be filled with zeros.
 *
 * Return value:
 *
 *			None
 *
 * Notes:
 *
 *                         IRQL Any level
 *
 */
void os_memset(void *buffer, byte fill, s32 len)
{
	memset(buffer, fill, len);
}

/*
 *
 * Function Name: os_memcpy
 *
 * Abstract:
 *
 *			Copy the contents of a source memory block to a destination memory block.
 *
 * Input:
 *
 *			sbuf [in]: A pointer to the source memory block to copy the bytes from.
 *			memory_len [in]: The number of bytes to copy from
 *				the source to the destination.
 *
 * Output:
 *
 *			dbuf [out]: A pointer to the destination memory block to copy the bytes to.
 *
 * Return value:
 *
 *			None
 *
 * Notes:
 *
 *                         IRQL Any level
 *
 */
void os_memcpy(void *dbuf, void *sbuf, s32 len)
{
	memcpy(dbuf, sbuf, len);
}

s32 os_memcpr(void *dbuf, void *sbuf, s32 len)
{
	return memcmp(dbuf, sbuf, len);
}

void os_enum_reg_cfg(void *cfg_item, e_chip_type chip_type, const byte *ustr,
		     cb_enum_reg_t func)
{
	char *idx_string = NULL;
	char *addr_string = NULL;
	char *value_string = NULL;
	char *separator = NULL;
	u32 i = 0;
	u32 idx = 0;
	u32 type = (u32) -1;
	u32 addr = 0;
	u32 value = 0;
	int err = 0;
	char cfg_buf[512];

	if (strcmp(ustr, "\\pcr") == 0) {
		for (i = 0; i < pcr_setting_count; i++) {
			PrintMsg("pcr_settings[%d] = %s\n", i, pcr_settings[i]);

			if (strlen(pcr_settings[i]) >= sizeof(cfg_buf)) {
				DbgErr("pcr setting string %d is too long\n",
				       i);
				continue;
			} else if (strlen(pcr_settings[i]) < 14) {
				DbgErr("pcr setting string %d is too short\n",
				       i);
				continue;
			}
			strcpy(cfg_buf, pcr_settings[i]);
			idx_string = cfg_buf;
			if (idx_string[3] != '_') {
				DbgErr
				    ("failed to find separator between idx and type\n");
				continue;
			}
			idx_string[3] = '\0';
			err = kstrtou32(idx_string, 16, &idx);
			if (err) {
				DbgErr
				    ("failed to parse idx from pcr setting: %s\n",
				     pcr_settings[i]);
				continue;
			}
			if (idx_string[4] == 'P' || idx_string[4] == 'p')
				type = 0;
			else if (idx_string[4] == 'M' || idx_string[4] == 'm')
				type = 1;
			else if (idx_string[4] == 'V' || idx_string[4] == 'v')
				type = 2;
			else {
				DbgErr
				    ("failed to parse type from pcr setting: %s\n",
				     pcr_settings[i]);
				continue;
			}
			addr_string = cfg_buf + 5;
			separator = strchr(addr_string, '=');
			if (separator == NULL) {
				DbgErr
				    ("failed to find separator between addr and value\n");
				continue;
			}
			*separator = '\0';
			value_string = ++separator;
			err = kstrtou32(addr_string, 16, &addr);
			if (err) {
				DbgErr
				    ("failed to parse addr from pcr setting: %s\n",
				     pcr_settings[i]);
				continue;
			}
			err = kstrtou32(value_string, 16, &value);
			if (err) {
				DbgErr
				    ("failed to parse value from pcr setting: %s\n",
				     pcr_settings[i]);
				continue;
			}
			PrintMsg("pcr[0x%08x]=0x%08x, type = %d, idx=%d\n",
				 addr, value, type, idx);
			if (func)
				func(cfg_item, type, idx, addr, value);
		}
	} else if (strcmp(ustr, "\\dmdn") == 0) {
		for (i = 0; i < dmdn_setting_count; i++) {
			if (strlen(pcr_settings[i]) >= sizeof(cfg_buf)) {
				DbgErr("dmdn setting string %d is too long\n",
				       i);
				continue;
			}
			strcpy(cfg_buf, dmdn_settings[i]);
			addr_string = cfg_buf;
			separator = strchr(addr_string, '=');
			if (separator == NULL) {
				DbgErr
				    ("failed to parse value from dmdn setting: %s\n",
				     dmdn_settings[i]);
				continue;
			}
			*separator = '\0';
			value_string = ++separator;
			err = kstrtou32(addr_string, 16, &addr);
			if (err) {
				DbgErr
				    ("failed to parse addr from dmdn setting: %s\n",
				     dmdn_settings[i]);
				continue;
			}
			err = kstrtou32(value_string, 16, &value);
			if (err) {
				DbgErr
				    ("failed to parse value from dmdn setting: %s\n",
				     dmdn_settings[i]);
				continue;
			}
			PrintMsg("dmdn[0x%08x]=0x%08x\n", addr, value);
			if (func)
				func(cfg_item, 0, 0, addr, value);
		}
	} else
		PrintMsg("current only support pcr or dmdnsettings\n",
			 __func__);
}

void os_pm_init(void *dev_evt)
{
	DbgInfo(MODULE_OS_API, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Enter %s\n",
		__func__);
	/* todo for PM part */

	DbgInfo(MODULE_OS_API, FEATURE_DRIVER_INIT, NOT_TO_RAM, "Exit %s\n",
		__func__);

}

bool os_pcr_pesistent_restore(u16 *addr_tb, u32 *val_tb, u32 tb_len)
{
	return FALSE;
}

bool os_pcr_pesistent_save(u16 *addr_tb, u32 *val_tb, u32 tb_len)
{
	return FALSE;
}

void os_random_init(void)
{
	/* no implement for linux */

}

u32 os_random_get(u32 max_value)
{
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 9, 0)
	return (get_random_long() % max_value);
#else
	return random32();
#endif
}

void os_bak_reg_hibernate(void)
{
	/* no implement for linux */

}

u64 os_get_performance_tick(u64 *cpu_freq)
{
	/* no implement for linux */
	return 0;
}

/*
 *
 * Function Name: os_cfg_load
 *
 * Abstract:
 *			 1. Read total reigstry information
 *
 * Input:
 *            cfg_item_t *cfg: Pointer to the registry config structure
 *            e_chip_type chip_type;
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
void os_cfg_load(void *cfg_item, e_chip_type chip_type)
{
	cfg_item_t *cfg = cfg_item;

	if (m_emmc_mode & BIT31)
		os_memcpy(&cfg->card_item.emmc_mode, &m_emmc_mode, 4);

	if (m_dma_mode & BIT31) {
		os_memcpy(&cfg->host_item.test_dma_mode_setting, &m_dma_mode,
			  4);
	}

	if (m_infinite_ctrl & BIT31) {
		os_memcpy(&cfg->host_item.test_infinite_transfer_mode,
			  &m_infinite_ctrl, 4);
	}

	if (m_tag_cap & BIT31) {
		os_memcpy(&cfg->host_item.test_tag_queue_capability, &m_tag_cap,
			  4);
	}

	if (m_sdmode_dis & BIT31)
		os_memcpy(&cfg->card_item.sd_card_mode_dis, &m_sdmode_dis, 4);

	if (m_mmc_dis & BIT31)
		os_memcpy(&cfg->card_item.mmc_mode_dis, &m_mmc_dis, 4);

	if (m_uhs2_ctrl & BIT31)
		os_memcpy(&cfg->card_item.uhs2_setting, &m_uhs2_ctrl, 4);

	if (m_output_tuning & BIT31) {
		os_memcpy(&cfg->feature_item.output_tuning_item,
			  &m_output_tuning, 4);
	}

	if (m_auto_dmt & BIT31)
		os_memcpy(&cfg->timer_item.auto_dormant_timer, &m_auto_dmt, 4);

	os_memcpy(&cfg->driver_item, &m_driver_item, 4);

	os_memcpy(&cfg->card_item.sd7_sdmode_switch_control,
		  &m_sd7_sdmode_switch_control, 4);
	os_memcpy(&cfg->card_item.sd_card_mode_dis, &m_sd_card_mode_dis, 4);
	os_memcpy(&cfg->card_item.test_max_access_mode, &m_test_max_access_mode,
		  4);
	os_memcpy(&cfg->card_item.test_uhs2_setting2, &m_test_uhs2_setting2, 4);
	os_memcpy(&cfg->card_item.test_driver_strength_sel,
		  &m_test_driver_strength_sel, 4);
	os_memcpy(&cfg->card_item.test_max_power_limit, &m_test_max_power_limit,
		  4);
	os_memcpy(&cfg->card_item.mmc_mode_dis, &m_mmc_mode_dis, 4);

	os_memcpy(&cfg->feature_item.pcie_wake_setting, &m_pcie_wake_setting,
		  4);
	os_memcpy(&cfg->feature_item.hsmux_vcme_enable, &m_hsmux_vcme_enable,
		  4);

	os_memcpy(&cfg->host_item.vdd_power_source_item,
		  &m_vdd_power_source_item, 4);
	os_memcpy(&cfg->host_item.host_drive_strength, &m_host_drive_strength,
		  4);
	os_memcpy(&cfg->host_item.test_sdma_boundary, &m_test_sdma_boundary, 4);
	os_memcpy(&cfg->host_item.test_dma_mode_setting,
		  &m_test_dma_mode_setting, 4);
	os_memcpy(&cfg->host_item.test_ocb_ctrl, &m_test_ocb_ctrl, 4);
	os_memcpy(&cfg->host_item.bios_l1_substate, &m_bios_l1_substate, 4);

	os_memcpy(&cfg->feature_item.auto_detect_refclk_counter_range_ctl,
		  &m_auto_detect_refclk_counter_range_ctl, 4);
	os_memcpy(&cfg->feature_item.refclk_stable_detection_counter1,
		  &m_refclk_stable_detection_counter1, 4);
	os_memcpy(&cfg->feature_item.refclk_stable_detection_counter2,
		  &m_refclk_stable_detection_counter2, 4);
	os_memcpy(&cfg->feature_item.refclk_stable_detection_counter3,
		  &m_refclk_stable_detection_counter3, 4);
	os_memcpy(&cfg->feature_item.l1_enter_exit_logic_ctl,
		  &m_l1_enter_exit_logic_ctl, 4);
	os_memcpy(&cfg->feature_item.pcie_phy_amplitude_adjust,
		  &m_pcie_phy_amplitude_adjust, 4);
	os_memcpy(&cfg->feature_item.output_tuning_item, &m_output_tuning_item,
		  4);
	os_memcpy(&cfg->feature_item.test_main_ldo_setting,
		  &m_test_main_ldo_setting, 4);

	os_memcpy(&cfg->timeout_item.power_wait_time, &m_power_wait_time, 4);
	os_memcpy(&cfg->timeout_item.test_write_data_timeout,
		  &m_test_write_data_timeout, 4);
	os_memcpy(&cfg->timeout_item.test_read_data_timeout,
		  &m_test_read_data_timeout, 4);
	os_memcpy(&cfg->timeout_item.test_non_data_timeout,
		  &m_test_non_data_timeout, 4);
	os_memcpy(&cfg->timeout_item.test_r1b_data_timeout,
		  &m_test_r1b_data_timeout, 4);
	os_memcpy(&cfg->timeout_item.test_card_init_timeout,
		  &m_test_card_init_timeout, 4);

#if BHT_LINUX_ENABLE_RTD3
	os_memcpy(&cfg->feature_item.psd_mode, &m_psd_mode, 4);
#else
	cfg->feature_item.psd_mode.enable_rtd3 = 0;

	/* software control */
	cfg->feature_item.psd_mode.rtd3_ctrl_mode = 1;
#endif

}

/* currently don't implement */
void os_set_sdio_val(void *p, u8 val, bool need_set_did)
{
}

void os_rtd3_req_wait_wake(void *pdx)
{

}
