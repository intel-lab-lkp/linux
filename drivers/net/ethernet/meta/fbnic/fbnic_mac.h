/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#ifndef _FBNIC_MAC_H_
#define _FBNIC_MAC_H_

#include <linux/types.h>

struct fbnic_dev;

#define FBNIC_MAX_JUMBO_FRAME_SIZE	9742

enum {
	FBNIC_LINK_DISABLED	= 0,
	FBNIC_LINK_DOWN		= 1,
	FBNIC_LINK_UP		= 2,
	FBNIC_LINK_EVENT	= 3,
};

enum {
	FBNIC_LED_STROBE_INIT,
	FBNIC_LED_ON,
	FBNIC_LED_OFF,
	FBNIC_LED_RESTORE,
};

/* Treat the FEC bits as a bitmask laid out as follows:
 * Bit 0: RS Enabled
 * Bit 1: BASER(Firecode) Enabled
 * Bit 2: Autoneg FEC
 */
enum {
	FBNIC_FEC_OFF		= 0,
	FBNIC_FEC_RS		= 1,
	FBNIC_FEC_BASER		= 2,
	FBNIC_FEC_AUTO		= 4,
};

#define FBNIC_FEC_MODE_MASK	(FBNIC_FEC_AUTO - 1)

/* Treat the link modes as a set of moldulation/lanes bitmask:
 * Bit 0: Lane Count, 0 = R1, 1 = R2
 * Bit 1: Modulation, 0 = NRZ, 1 = PAM4
 * Bit 2: Autoneg Modulation/Lane Configuration
 */
enum {
	FBNIC_LINK_25R1		= 0,
	FBNIC_LINK_50R2		= 1,
	FBNIC_LINK_50R1		= 2,
	FBNIC_LINK_100R2	= 3,
	FBNIC_LINK_AUTO		= 4,
};

#define FBNIC_LINK_MODE_R2	(FBNIC_LINK_50R2)
#define FBNIC_LINK_MODE_PAM4	(FBNIC_LINK_50R1)
#define FBNIC_LINK_MODE_MASK	(FBNIC_LINK_AUTO - 1)

/* This structure defines the interface hooks for the MAC. The MAC hooks
 * will be configured as a const struct provided with a set of function
 * pointers.
 *
 * bool (*get_link)(struct fbnic_dev *fbd);
 *	Get the current link state for the MAC.
 * int (*get_link_event)(struct fbnic_dev *fbd)
 *	Get the current link event status, reports true if link has
 *	changed to either up (1) or down (-1).
 * void (*enable)(struct fbnic_dev *fbd);
 *	Configure and enable MAC to enable link if not already enabled
 * void (*disable)(struct fbnic_dev *fbd);
 *	Shutdown the link if we are the only consumer of it.
 * void (*init_regs)(struct fbnic_dev *fbd);
 *	Initialize MAC registers to enable Tx/Rx paths and FIFOs.
 */
struct fbnic_mac {
	bool (*get_link)(struct fbnic_dev *fbd);
	int (*get_link_event)(struct fbnic_dev *fbd);
	int (*enable)(struct fbnic_dev *fbd);
	void (*disable)(struct fbnic_dev *fbd);
	void (*init_regs)(struct fbnic_dev *fbd);
};

int fbnic_mac_init(struct fbnic_dev *fbd);
#endif /* _FBNIC_MAC_H_ */
