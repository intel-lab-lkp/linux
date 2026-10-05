/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * NXP vendor command definitions for nl80211
 *
 * Copyright 2026 NXP
 */

#ifndef __VENDOR_CMD_NXP_H__
#define __VENDOR_CMD_NXP_H__

#define NXP_OUI		0x006037

/**
 * enum nxp_vendor_cmd - supported NXP vendor commands
 *
 * @NXP_VENDOR_CMD_TXPOWER_LIMIT: Access firmware Transmit Rate-based
 *      Power Control (TRPC) configuration. The command uses the
 *      attributes defined by &enum nxp_txpwrlimit_attrs.
 *
 *      SET operations provide TRPC table data through
 *      %NXP_ATTR_TXPWRLIMIT_DATA. GET operations return the requested TRPC
 *      table in %NXP_ATTR_TXPWRLIMIT_DATA.
 *
 *      The kernel does not interpret the TRPC table contents and only
 *      passes the table data between userspace and firmware.
 */
enum nxp_vendor_cmd {
	NXP_VENDOR_CMD_TXPOWER_LIMIT,
};

/**
 * enum nxp_txpwrlimit_action - operation for %NXP_ATTR_TXPWRLIMIT_ACTION
 *
 * @NXP_TXPWRLIMIT_ACTION_GET: Retrieve the TRPC table for the selected
 *	subband.
 * @NXP_TXPWRLIMIT_ACTION_SET: Update the TRPC table for the selected
 *	subband using data from %NXP_ATTR_TXPWRLIMIT_DATA.
 */
enum nxp_txpwrlimit_action {
	NXP_TXPWRLIMIT_ACTION_GET,
	NXP_TXPWRLIMIT_ACTION_SET,
};

/**
 * enum nxp_txpwrlimit_subband - firmware TRPC subband selectors
 *
 * The selector values are defined by the firmware Transmit Rate-based
 * Power Control (TRPC) interface.
 *
 * @NXP_TXPWRLIMIT_SUBBAND_2G: 2.4 GHz band.
 * @NXP_TXPWRLIMIT_SUBBAND_5G_0: 5 GHz subband 0.
 * @NXP_TXPWRLIMIT_SUBBAND_5G_1: 5 GHz subband 1.
 * @NXP_TXPWRLIMIT_SUBBAND_5G_2: 5 GHz subband 2.
 * @NXP_TXPWRLIMIT_SUBBAND_5G_3: 5 GHz subband 3.
 */
enum nxp_txpwrlimit_subband {
	NXP_TXPWRLIMIT_SUBBAND_2G	= 0x00,
	NXP_TXPWRLIMIT_SUBBAND_5G_0	= 0x10,
	NXP_TXPWRLIMIT_SUBBAND_5G_1	= 0x11,
	NXP_TXPWRLIMIT_SUBBAND_5G_2	= 0x12,
	NXP_TXPWRLIMIT_SUBBAND_5G_3	= 0x13,
};

/**
 * enum nxp_txpwrlimit_attrs - TX power limit vendor attributes
 * @NXP_ATTR_TXPWRLIMIT_UNSPEC: Unspecified attribute.
 * @NXP_ATTR_TXPWRLIMIT_ACTION: Operation from &enum nxp_txpwrlimit_action.
 * @NXP_ATTR_TXPWRLIMIT_SUBBAND: Target subband.
 * @NXP_ATTR_TXPWRLIMIT_DATA: Firmware-defined TRPC table data.
 */
enum nxp_txpwrlimit_attrs {
	NXP_ATTR_TXPWRLIMIT_UNSPEC,
	NXP_ATTR_TXPWRLIMIT_ACTION,
	NXP_ATTR_TXPWRLIMIT_SUBBAND,
	NXP_ATTR_TXPWRLIMIT_DATA,

	__NXP_ATTR_TXPWRLIMIT_AFTER_LAST,
	NXP_ATTR_TXPWRLIMIT_MAX = __NXP_ATTR_TXPWRLIMIT_AFTER_LAST - 1
};

#endif /* __VENDOR_CMD_NXP_H__ */
