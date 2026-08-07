/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AMD/Xilinx TSN Endpoint Ethernet MAC DSA switch driver.
 */
#ifndef _XILINX_TSN_H
#define _XILINX_TSN_H

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/types.h>
#include <net/dsa.h>

#define XLNX_TSN_NUM_PORTS	3
#define XLNX_TSN_CPU_PORT	0
#define XLNX_TSN_PORT_MAC1	1
#define XLNX_TSN_PORT_MAC2	2

#define TSN_SW_STATUS_OFFSET		0x00000
/* Poll this before changing port state. */
#define TSN_SW_STATUS_READY		BIT(0)

/* Switch Port State Control register: packs per-port STP state and
 * change-commit bits into one 32-bit word.
 */
#define TSN_PORT_STATE_CTRL_OFFSET	0x0004c

#define EP_PORT_STATUS_CHG_BIT		BIT(0)
#define EP_PORT_STATUS_MASK		GENMASK(3, 1)
#define MAC1_PORT_STATUS_CHG_BIT	BIT(8)
#define MAC1_PORT_STATUS_MASK		GENMASK(11, 9)
#define MAC2_PORT_STATUS_CHG_BIT	BIT(16)
#define MAC2_PORT_STATUS_MASK		GENMASK(19, 17)

/* readl_poll_timeout() parameters (in microseconds): poll until
 * the port-state change-commit bit self-clears.
 */
#define TSN_SW_POLL_DELAY_US		10
#define TSN_SW_POLL_TIMEOUT_US		5000

enum tsn_port_state {
	TSN_PORT_STATE_DISABLED = 0,
	TSN_PORT_STATE_BLOCKING,
	TSN_PORT_STATE_LISTENING,
	TSN_PORT_STATE_LEARNING,
	TSN_PORT_STATE_FORWARDING,
};

/**
 * struct xlnx_tsn - per-IP switch state
 * @ds: DSA switch
 * @dev: backing device
 * @sw_base: switch fabric register window
 * @mac_base: per-MAC register windows, indexed by user-port number
 *	      (index 0 unused; MAC1 at [1], MAC2 at [2])
 */
struct xlnx_tsn {
	struct dsa_switch ds;
	struct device *dev;
	void __iomem *sw_base;
	void __iomem *mac_base[XLNX_TSN_NUM_PORTS];
};

#endif /* _XILINX_TSN_H */
