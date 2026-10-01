/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SGI PCI Xtalk Bridge
 */

#ifndef PLATFORM_DATA_XTALK_BRIDGE_H
#define PLATFORM_DATA_XTALK_BRIDGE_H

#include <asm/sn/types.h>

struct xtalk_bridge_platform_data {
	struct resource	mem;
	struct resource io;
	unsigned long bridge_addr;
	unsigned long intr_addr;
	unsigned long mem_offset;
	unsigned long io_offset;
	nasid_t	nasid;
	int	masterwid;
	bool	is_xbridge;
	bool	is_pic;
};

#endif /* PLATFORM_DATA_XTALK_BRIDGE_H */
