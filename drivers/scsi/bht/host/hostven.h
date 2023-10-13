/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: hostven.h
 *
 * Abstract: Delcare for the host vendor APIs
 *
 * Version: 1.00
 *
 * Author: Yuxiang
 *
 * Environment:	OS Independent
 *
 * History:
 *
 * 9/16/2014		Creation	Yuxiang
 */

void hostven_transfer_init(sd_host_t *host, bool enable);
void hostven_update_dmdn(sd_host_t *host, u32 dmdn);

bool hostven_hs400_host_chk(sd_host_t *host);

void hostven_drive_strength_cfg(sd_host_t *host);

bool host_chk_ocb_occur(sd_host_t *host);
void hostven_ocb_cfg(sd_host_t *host);
void hostven_pinshare_cfg(sd_host_t *host);
void hostven_cmd_low_cfg(sd_host_t *host);
void hostven_switch_flow_cfg(sd_host_t *host);
