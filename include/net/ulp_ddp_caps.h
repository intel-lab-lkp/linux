/* SPDX-License-Identifier: GPL-2.0
 *
 * ulp_ddp.h
 *  Author: Aurelien Aptel <aaptel@nvidia.com>
 *  Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.  All rights reserved.
 */
#ifndef _ULP_DDP_CAPS_H
#define _ULP_DDP_CAPS_H

#include <linux/types.h>

enum {
	ULP_DDP_C_NVME_TCP_BIT,
	ULP_DDP_C_NVME_TCP_DDGST_RX_BIT,

	/*
	 * add capabilities above and keep in sync with
	 * Documentation/netlink/specs/ulp_ddp.yaml
	 */
	ULP_DDP_C_COUNT,
};

struct ulp_ddp_netdev_caps {
	DECLARE_BITMAP(active, ULP_DDP_C_COUNT);
	DECLARE_BITMAP(hw, ULP_DDP_C_COUNT);
};

static inline bool ulp_ddp_cap_turned_on(unsigned long *old,
					 unsigned long *new,
					 int bit_nr)
{
	return !test_bit(bit_nr, old) && test_bit(bit_nr, new);
}

static inline bool ulp_ddp_cap_turned_off(unsigned long *old,
					  unsigned long *new,
					  int bit_nr)
{
	return test_bit(bit_nr, old) && !test_bit(bit_nr, new);
}

#endif
