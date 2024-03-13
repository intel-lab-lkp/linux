/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __XILINX_CORE_H
#define __XILINX_CORE_H

#include <linux/device.h>

/**
 * struct xilinx_fpga_core - interface between the driver and the core manager
 *                           of Xilinx 7 Series FPGA manager
 * @dev:       device node
 * @write:     write callback of the driver
 * @prog_b:    PROGRAM_B gpio descriptor
 * @init_b:    INIT_B gpio descriptor
 * @done:      DONE gpio descriptor
 */
struct xilinx_fpga_core {
/* public: */
	struct device *dev;
	int (*write)(struct xilinx_fpga_core *core, const char *buf,
		     size_t count);
/* private: handled by xilinx-core */
	struct gpio_desc *prog_b;
	struct gpio_desc *init_b;
	struct gpio_desc *done;
};

int xilinx_core_probe(struct xilinx_fpga_core *core);

#endif /* __XILINX_CORE_H */
