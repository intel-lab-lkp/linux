/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AMD/Xilinx TSN Endpoint Ethernet MAC DSA switch driver.
 */
#ifndef _XILINX_TSN_H
#define _XILINX_TSN_H

#include <linux/types.h>
#include <net/dsa.h>

#define XLNX_TSN_NUM_PORTS	3
#define XLNX_TSN_CPU_PORT	0
#define XLNX_TSN_PORT_MAC1	1
#define XLNX_TSN_PORT_MAC2	2

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
