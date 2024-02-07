// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xilinx Spartan6 and 7 Series Slave Serial SPI Driver
 *
 * Copyright (C) 2017 DENX Software Engineering
 *
 * Anatolij Gustschin <agust@denx.de>
 *
 * Manage Xilinx FPGA firmware that is loaded over SPI using
 * the slave serial configuration interface.
 */

#include "xilinx-core.h"

#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/of.h>
#include <linux/spi/spi.h>

struct xilinx_spi_conf {
	struct xilinx_fpga_core core;
	struct spi_device *spi;
};

#define to_xilinx_spi_conf(obj) container_of(obj, struct xilinx_spi_conf, core)

static int xilinx_spi_write(struct xilinx_fpga_core *core, const char *buf,
			    size_t count)
{
	struct xilinx_spi_conf *conf = to_xilinx_spi_conf(core);
	const char *fw_data = buf;
	const char *fw_data_end = fw_data + count;

	while (fw_data < fw_data_end) {
		size_t remaining, stride;
		int ret;

		remaining = fw_data_end - fw_data;
		stride = min_t(size_t, remaining, SZ_4K);

		ret = spi_write(conf->spi, fw_data, stride);
		if (ret) {
			dev_err(core->dev, "SPI error in firmware write: %d\n",
				ret);
			return ret;
		}
		fw_data += stride;
	}

	return 0;
}

static int xilinx_spi_probe(struct spi_device *spi)
{
	struct xilinx_spi_conf *conf;

	conf = devm_kzalloc(&spi->dev, sizeof(*conf), GFP_KERNEL);
	if (!conf)
		return -ENOMEM;

	conf->core.dev = &spi->dev;
	conf->core.write = xilinx_spi_write;
	conf->spi = spi;

	return xilinx_core_probe(&conf->core);
}

#ifdef CONFIG_OF
static const struct of_device_id xlnx_spi_of_match[] = {
	{
		.compatible = "xlnx,fpga-slave-serial",
	},
	{}
};
MODULE_DEVICE_TABLE(of, xlnx_spi_of_match);
#endif

static struct spi_driver xilinx_slave_spi_driver = {
	.driver = {
		.name = "xlnx-slave-spi",
		.of_match_table = of_match_ptr(xlnx_spi_of_match),
	},
	.probe = xilinx_spi_probe,
};

module_spi_driver(xilinx_slave_spi_driver)

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Anatolij Gustschin <agust@denx.de>");
MODULE_DESCRIPTION("Load Xilinx FPGA firmware over SPI");
