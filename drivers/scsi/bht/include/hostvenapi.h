/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: hostvenapi.h
 *
 * Abstract: This File is used to define interface for BHT host operations
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

bool hostven_dll_input_tuning_init(sd_host_t *host);
bool hostven_fix_output_tuning(sd_host_t *host, byte access_mode);
u8 hostven_tuning_type_selection(sd_host_t *host, byte sd_access_mode);
void hostven_optphaserw(sd_host_t *host, u8 output_opt, u8 input_opt);
void hostven_set_tuning_phase(sd_host_t *host, u32 input_n1, u32 output_n1,
			      bool off);
void hostven_set_output_tuning_phase(sd_host_t *host, u32 value, bool off);
void hostven_detect_refclk_count_range_init(sd_host_t *host);
void hostven_refclk_stable_detection_circuit(sd_host_t *host);
void hostven_pcie_phy_tx_amplitude_adjustment(sd_host_t *host);

bool hostven_chk_card_present(sd_host_t *host);

/*
 * init host type and feature
 */
void host_vendor_feature_init(sd_host_t *host);

bool hostven_chip_type_check(sd_host_t *host);

bool hostven_rtd3_check(sd_host_t *host);
void hostven_pm_mode_cfg(sd_host_t *host, pm_state_t *pm);
u32 hostven_d3_mode_sel(sd_host_t *host, u32 *d3_submode);
void hostven_main_power_ctrl(sd_host_t *host, bool is_keep_on);

void hostven_hw_timer_stop(sd_host_t *host);
void hostven_hw_timer_start(sd_host_t *host, u32 time_ms);
void hostven_set_pml0_requrest(sd_host_t *host, bool enable);
