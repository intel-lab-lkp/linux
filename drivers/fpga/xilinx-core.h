/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __XILINX_CORE_H
#define __XILINX_CORE_H

#include <linux/device.h>

struct xilinx_fpga_core;

typedef int (*xilinx_write_func)(struct xilinx_fpga_core *core, const char *buf,
				 size_t count);
typedef int (*xilinx_write_one_dummy_byte_func)(struct xilinx_fpga_core *core);

struct xilinx_fpga_core {
	struct device *dev;
	xilinx_write_func write;
	xilinx_write_one_dummy_byte_func write_one_dummy_byte;
	struct gpio_desc *prog_b;
	struct gpio_desc *init_b;
	struct gpio_desc *done;
};

int xilinx_core_probe(struct xilinx_fpga_core *core, struct device *dev,
		      xilinx_write_func write,
		      xilinx_write_one_dummy_byte_func write_one_dummy_byte);

#endif /* __XILINX_CORE_H */
