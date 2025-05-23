/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2024 Sean Anderson <sean.anderson@seco.com>
 */

#ifndef PCS_XILINX_H
#define PCS_XILINX_H

struct device;
struct phylink_pcs;

struct phylink_pcs *axienet_xilinx_pcs_get(struct device *dev,
					   const unsigned long *interfaces);

#endif /* PCS_XILINX_H */
