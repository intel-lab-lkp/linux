/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * NXP Wireless LAN device driver: vendor command interface
 *
 * Copyright 2011-2026 NXP
 */

#ifndef __NXPWIFI_VENDOR_H__
#define __NXPWIFI_VENDOR_H__

#define NXP_OUI	0x006037

enum nxpwifi_vendor_commands {
	NXPWIFI_VENDOR_CMD_TXPOWER_LIMIT,
};

/* Actions for NXPWIFI_ATTR_TXPWR_ACTION. */
enum nxpwifi_txpwr_action {
	NXPWIFI_TXPWR_ACTION_GET,
	NXPWIFI_TXPWR_ACTION_SET,
};

/* Subband selectors for NXPWIFI_ATTR_TXPWR_SUBBAND: 0x00 is the 2.4 GHz
 * band and 0x10..0x13 are the four 5 GHz subbands.
 */
enum nxpwifi_txpwr_subband {
	NXPWIFI_TXPWR_SUBBAND_2G	= 0x00,
	NXPWIFI_TXPWR_SUBBAND_5G_0	= 0x10,
	NXPWIFI_TXPWR_SUBBAND_5G_1	= 0x11,
	NXPWIFI_TXPWR_SUBBAND_5G_2	= 0x12,
	NXPWIFI_TXPWR_SUBBAND_5G_3	= 0x13,
};

/* Attributes for NXPWIFI_VENDOR_CMD_TXPOWER_LIMIT. */
enum nxpwifi_txpwr_attrs {
	NXPWIFI_ATTR_TXPWR_UNSPEC,
	NXPWIFI_ATTR_TXPWR_ACTION,	/* u8, enum nxpwifi_txpwr_action */
	NXPWIFI_ATTR_TXPWR_SUBBAND,	/* u8, subband selector for GET */
	NXPWIFI_ATTR_TXPWR_DATA,	/* binary, per-subband TRPC TLVs */

	__NXPWIFI_ATTR_TXPWR_AFTER_LAST,
	NXPWIFI_ATTR_TXPWR_MAX = __NXPWIFI_ATTR_TXPWR_AFTER_LAST - 1
};

void nxpwifi_set_vendor_commands(struct wiphy *wiphy);
#endif /* __NXPWIFI_VENDOR_H__ */
