/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SN1 (Bedrock) hub register windows.
 *
 * Copyright (C) 2026 Imre Kaloz <kaloz@kernel.org>
 */
#ifndef _ASM_SN_SN1_HUB_H
#define _ASM_SN_SN1_HUB_H

#define HUBLBBASE_IP35		0x00600000
#define HUBNIBASE_IP35		0x00680000
#define HUBXBBASE_IP35		0x00700000
#define HUBMDBASE_IP35		0x00780000

/*
 * LB_REV_ID, at the base of the LB block and so at Hub's NI_STATUS_REV_ID
 * offset, holds the node id in bits 39:32.
 */
#define LRI_NODEID_SHFT_IP35	32
#define LRI_NODEID_MASK_IP35	0xff

/*
 * Bits 47:46 give the system size: 0, 1 or 2 is log2 of the nodes per
 * region, and 2 is N mode.
 */
#define LRI_SYSTEM_SIZE_IP35(id)	(((id) >> 46) & 0x3)
#define LRI_SYSTEM_SIZE_NMODE_IP35	2

/*
 * Network interface port reset: bit 0 sends a link reset, bit 2 resets
 * the local Bedrock.  Bit 0 of the reset enable register lets a reset
 * arriving over the link reset this Bedrock.
 */
#define NI_PORT_RESET_IP35	(HUBNIBASE_IP35 + 0x000008)
#define NI_RESET_ACTION_IP35	0x01
#define NI_RESET_LOCAL_IP35	0x04
#define NI_RESET_ENABLE_IP35	(HUBNIBASE_IP35 + 0x000010)
#define NI_RESET_OK_IP35	0x01

#endif /* _ASM_SN_SN1_HUB_H */
