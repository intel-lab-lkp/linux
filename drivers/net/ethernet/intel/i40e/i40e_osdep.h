/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2013 - 2018 Intel Corporation. */

#ifndef _I40E_OSDEP_H_
#define _I40E_OSDEP_H_

#include <linux/dev_printk.h>
#include <linux/types.h>

/* get readq/writeq support for 32 bit kernels, use the low-first version */
#include <linux/io-64-nonatomic-lo-hi.h>

/* File to be the magic between shared code and
 * actual OS primitives
 */

struct i40e_hw;
struct device *i40e_hw_to_dev(struct i40e_hw *hw);

#define hw_dbg(hw, S, A...)							\
do {										\
	dev_dbg(i40e_hw_to_dev(hw), S, ##A);					\
} while (0)

#define wr32(a, reg, value)	writel((value), ((a)->hw_addr + (reg)))
#define rd32(a, reg)		readl((a)->hw_addr + (reg))

#define rd64(a, reg)		readq((a)->hw_addr + (reg))
#define i40e_flush(a)		readl((a)->hw_addr + I40E_GLGEN_STAT)

#define i40e_debug(h, m, s, ...)				\
do {								\
	if (((m) & (h)->debug_mask))				\
		pr_info("i40e %02x:%02x.%x " s,			\
			(h)->bus.bus_id, (h)->bus.device,	\
			(h)->bus.func, ##__VA_ARGS__);		\
} while (0)

#endif /* _I40E_OSDEP_H_ */
