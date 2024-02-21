/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __XILINX_CORE_H
#define __XILINX_CORE_H

#include <linux/device.h>

/**
 * struct xilinx_fpga_core - interface between the driver and the core manager
 *                           of Xilinx 7 Series FPGA manager
 * @dev:       device node, must be set by the driver
 * @write:     write callback of the driver, must be set by the driver
 * @prog_b:    PROGRAM_B gpio, handled by the core manager
 * @init_b:    INIT_B gpio, handled by the core manager
 * @done:      DONE gpio, handled by the core manager
 */
struct xilinx_fpga_core {
	struct device *dev;
	int (*write)(struct xilinx_fpga_core *core, const char *buf,
		     size_t count);
	struct gpio_desc *prog_b;
	struct gpio_desc *init_b;
	struct gpio_desc *done;
};

int xilinx_core_probe(struct xilinx_fpga_core *core);

#endif /* __XILINX_CORE_H */
