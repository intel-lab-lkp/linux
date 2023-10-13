/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: hostapi.h
 *
 * Abstract: This file is used to define interface Standard Host Operation
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

#ifndef _HOSTAPI_H
#define _HOSTAPI_H

#include "host.h"
/*
 * Below 2 function called pm function to save and restore host setting
 */
void host_backup_register(sd_host_t *host);
void host_restore_register(sd_host_t *host);

/*
 * Init host capability
 */
void host_init_capbility(sd_host_t *host);

/*
 * Init emmc host clock to 400K
 */

/*
 * This function called by showdown os
 */
void host_shutdown(sd_host_t *host);

/*
 * called by req_global_init, host vendor init
 */
void host_init(sd_host_t *host);
void host_uninit(sd_host_t *host, bool disable_all_int);
void host_poweroff(sd_host_t *host, e_card_type type);
void host_force_pll_enable(sd_host_t *host, bool force);

/*
 * Api to all card
 */
void host_led_ctl(sd_host_t *host, bool on);
typedef enum {
	BUS_WIDTH1 = 0,
	BUS_WIDTH4,
	BUS_WIDTH8
} e_bus_width;

typedef enum {
	SIG_VOL_33 = 0,
	SIG_VOL_18,
	SIG_VOL_12
} e_sig_vol;

void host_set_buswidth(sd_host_t *host, e_bus_width e_width);
void host_1_8v_sig_set(sd_host_t *host, bool enable);
void host_sig_vol_set(sd_host_t *host, e_sig_vol sig_vol);

/*
 * emmc card host register setting
 */
bool host_emmc_init(sd_host_t *host, cfg_emmc_mode_t *emmc_mode);
void host_emmc_hs400_set(sd_host_t *host, bool b_hs400);
void host_emmc_ddr_set(sd_host_t *host, bool b_ddr);

/* Set the host timing */

void host_set_highspeed(sd_host_t *host, bool on);
void host_change_clock(sd_host_t *host, u32 value);
void host_set_uhs_mode(sd_host_t *host, byte access_mode);

bool host_enable_sd_signal18v(sd_host_t *host);
void host_sd_init(sd_host_t *host);

void host_transfer_init(sd_host_t *host, bool enable_infinite,
			bool force_adma);
void host_cmddat_line_reset(sd_host_t *host);

void host_int_dis_sig_all(sd_host_t *host, bool all);

bool host_error_int_recovery_stage2(sd_host_t *host, u16 error_int_state);

/*
 * UHS2 only apis
 */
bool host_uhs2_phychk(sd_host_t *host, bool fromslp, bool *stblfail);
void host_uhs2_init(sd_host_t *host, u32 clkvalue, bool bfullreset);
void host_uhs2_clear(sd_host_t *host, bool breset);
void host_uhs2_cfg_set(sd_host_t *host, uhs2_info_t *setting, bool stage2);
bool host_uhs2_resume_dmt(sd_host_t *host, bool hbr);
bool host_uhs2_go_dmt(sd_host_t *host, bool hbr);
void host_uhs2_reset(sd_host_t *host, bool fullreset);
void host_set_tuning_mode(sd_host_t *host, bool hw_mode);
bool host_chk_tuning_comp(sd_host_t *host, bool hwtuning);

/* Interrupt operation */

void host_int_sig_dis(sd_host_t *host, u32 int_val);
void host_int_clr_status(sd_host_t *host);

void host_int_en_cdc(sd_host_t *host);
bool host_wr_protect_pin(sd_host_t *host);

void host_enable_cmd23(sd_host_t *host, bool enable);

u32 sdhci_readl(sd_host_t *host, u16 offset);
void sdhci_writel(sd_host_t *host, u16 offset, u32 value);
void sdhci_or32(sd_host_t *host, u16 offset, u32 value);
void sdhci_and32(sd_host_t *host, u16 offset, u32 value);
void sdhci_writew(sd_host_t *host, u16 offset, u16 value);
u16 sdhci_readw(sd_host_t *host, u16 offset);
void sdhci_or16(sd_host_t *host, u16 offset, u16 value);
void sdhci_and16(sd_host_t *host, u16 offset, u16 value);

/* PCI config register accessing */

void pci_port_writel(sd_host_t *host, u32 port, u32 data);

u32 pci_port_readl(sd_host_t *host, u32 port);
u32 pci_get_bus_data(sd_host_t *host);

u16 pci_readw(sd_host_t *host, u16 offset);
void pci_writew(sd_host_t *host, u16 offset, u16 value);
void pci_orw(sd_host_t *host, u16 offset, u16 value);
void pci_andw(sd_host_t *host, u16 offset, u16 value);

void pci_writel(sd_host_t *host, u16 offset, u32 value);
u32 pci_readl(sd_host_t *host, u16 offset);
void pci_orl(sd_host_t *host, u16 offset, u32 value);
void pci_andl(sd_host_t *host, u16 offset, u32 value);

u16 ven_readw(sd_host_t *host, u16 offset);
void ven_writew(sd_host_t *host, u16 offset, u16 value);
void ven_or16(sd_host_t *host, u16 offset, u16 value);
void ven_and16(sd_host_t *host, u16 offset, u16 value);

u32 ven_readl(sd_host_t *host, u16 offset);
void ven_writel(sd_host_t *host, u16 offset, u32 value);

void ven_or32(sd_host_t *host, u16 offset, u32 value);
void ven_and32(sd_host_t *host, u16 offset, u32 value);

void pci_cfgio_writel(sd_host_t *host, u16 offset, u32 value);

bool host_check_lost(sd_host_t *host);
void host_reset(sd_host_t *host, u32 resetmode);
void host_set_output_tuning_phase(sd_host_t *host, u32 phase);
void host_int_sig_update(sd_host_t *host, u32 int_val);
void host_uhs2_err_sig_update(sd_host_t *host, u32 int_val);
void host_check_card_insert_desert(sd_host_t *host);

void set_pattern_value(sd_host_t *host, u8 value);
void shif_byte_pattern_bit_set(sd_host_t *host, bool bit_en, u8 pattern_case);
void set_gpio_levels(sd_host_t *host, bool gpio_num, bool signal_level);
bool shift_bit_func_enable(sd_host_t *host);
void power_control_with_card_type(sd_host_t *host, u8 vddx, bool power_en);
#endif
