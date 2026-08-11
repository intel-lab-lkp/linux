/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
 *
 * ZTE DingHai Rdma driver
 * Copyright (c) 2022-2026, ZTE Corporation
 */

#ifndef ZRDMA_CTRL_H
#define ZRDMA_CTRL_H

struct zxdh_hw {
	u8 __iomem *hw_addr;
	struct device *device;
};

struct zxdh_sc_dev {
	struct zxdh_hw *hw;
	u32 max_ceqs;
	u8 ep_id;
	u8 driver_load;
};

#endif
