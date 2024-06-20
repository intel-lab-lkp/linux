// SPDX-License-Identifier: GPL-2.0-only
/*
 * Trion and Titanium Series FPGA SPI Passive Programming Driver
 *
 * Copyright (C) 2024 iris-GmbH infrared & intelligent sensors
 *
 * Ian Dannapel <iansdannapel@gmail.com>
 *
 * Manage Efinix FPGA firmware that is loaded over SPI using
 * the serial configuration interface.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/of.h>
#include <linux/spi/spi.h>
#include <linux/sizes.h>

struct efinix_spi_conf {
	struct spi_device *spi;
	struct gpio_desc *done;
	struct gpio_desc *reset;
	struct gpio_desc *cs;
	enum fpga_mgr_states state;
};

static int get_done_gpio(struct fpga_manager *mgr)
{
	struct efinix_spi_conf *conf = mgr->priv;
	int ret = 0;

	if (conf->done) {
		ret = gpiod_get_value(conf->done);
		if (ret < 0)
			dev_err(&mgr->dev, "Error reading DONE (%d)\n", ret);
	}

	return ret;
}

static void reset(struct fpga_manager *mgr)
{
	struct efinix_spi_conf *conf = mgr->priv;

	gpiod_set_value(conf->reset, 1);
	/* wait tCRESET_N */
	usleep_range(5, 15);
	gpiod_set_value(conf->reset, 0);
	conf->state = FPGA_MGR_STATE_RESET;
}

static enum fpga_mgr_states efinix_spi_state(struct fpga_manager *mgr)
{
	struct efinix_spi_conf *conf = mgr->priv;

	return conf->state;
}

static int efinix_spi_apply_clk_cycles(struct fpga_manager *mgr)
{
	struct efinix_spi_conf *conf = mgr->priv;
	char data[13] = {0};

	return spi_write(conf->spi, data, sizeof(data));
}

static int efinix_spi_write_init(struct fpga_manager *mgr,
				 struct fpga_image_info *info,
				 const char *buf, size_t count)
{
	struct efinix_spi_conf *conf = mgr->priv;

	if (info->flags & FPGA_MGR_PARTIAL_RECONFIG) {
		dev_err(&mgr->dev, "Partial reconfiguration not supported\n");
		return -EINVAL;
	}

	/* reset with chip select active */
	gpiod_set_value(conf->cs, 1);
	usleep_range(5, 15);
	reset(mgr);

	/* wait tDMIN */
	usleep_range(100, 150);

	return 0;
}

static int efinix_spi_write(struct fpga_manager *mgr, const char *buf,
			    size_t count)
{
	struct efinix_spi_conf *conf = mgr->priv;
	int ret;

	ret = spi_write(conf->spi, buf, count);
	if (ret) {
		dev_err(&mgr->dev, "SPI error in firmware write: %d\n",
			ret);
		return ret;
	}

	/* append at least 100 clock cycles */
	efinix_spi_apply_clk_cycles(mgr);

	/* release chip select */
	gpiod_set_value(conf->cs, 0);

	return 0;
}

static int efinix_spi_write_complete(struct fpga_manager *mgr,
				     struct fpga_image_info *info)
{
	struct efinix_spi_conf *conf = mgr->priv;
	unsigned long timeout =
		jiffies + usecs_to_jiffies(info->config_complete_timeout_us);
	bool expired = false;
	int done;

	if (conf->done) {
		while (!expired) {
			expired = time_after(jiffies, timeout);

			done = get_done_gpio(mgr);
			if (done < 0)
				return done;

			if (done)
				break;
		}
	}

	if (expired)
		return -ETIMEDOUT;

	/* wait tUSER */
	usleep_range(75, 125);

	return 0;
}

static const struct fpga_manager_ops efinix_spi_ops = {
	.state = efinix_spi_state,
	.write_init = efinix_spi_write_init,
	.write = efinix_spi_write,
	.write_complete = efinix_spi_write_complete,
};

static int efinix_spi_probe(struct spi_device *spi)
{
	struct efinix_spi_conf *conf;
	struct fpga_manager *mgr;

	conf = devm_kzalloc(&spi->dev, sizeof(*conf), GFP_KERNEL);
	if (!conf)
		return -ENOMEM;

	conf->spi = spi;
	conf->state = FPGA_MGR_STATE_UNKNOWN;

	conf->reset = devm_gpiod_get(&spi->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(conf->reset))
		return dev_err_probe(&spi->dev, PTR_ERR(conf->reset),
				"Failed to get RESET gpio\n");

	conf->cs = devm_gpiod_get(&spi->dev, "cs", GPIOD_OUT_HIGH);
	if (IS_ERR(conf->cs))
		return dev_err_probe(&spi->dev, PTR_ERR(conf->cs),
				"Failed to get CHIP_SELECT gpio\n");

	if (!(spi->mode & SPI_CPHA) || !(spi->mode & SPI_CPOL))
		return dev_err_probe(&spi->dev, PTR_ERR(conf->cs),
				"Unsupported SPI mode, set CPHA and CPOL\n");

	conf->done = devm_gpiod_get_optional(&spi->dev, "done", GPIOD_IN);
	if (IS_ERR(conf->done))
		return dev_err_probe(&spi->dev, PTR_ERR(conf->done),
				"Failed to get DONE gpio\n");

	mgr = devm_fpga_mgr_register(&spi->dev,
				"Efinix SPI Passive Programming FPGA Manager",
					&efinix_spi_ops, conf);

	return PTR_ERR_OR_ZERO(mgr);
}

#ifdef CONFIG_OF
static const struct of_device_id efnx_spi_of_match[] = {
	{ .compatible = "efnx,fpga-spi-passive", },
	{}
};
MODULE_DEVICE_TABLE(of, efnx_spi_of_match);
#endif

static const struct spi_device_id efinix_ids[] = {
	{ "fpga-spi-passive", 0 },
	{ },
};
MODULE_DEVICE_TABLE(spi, efinix_ids);


static struct spi_driver efinix_spi_passive_driver = {
	.driver = {
		.name = "efnx-fpga-spi-passive",
		.of_match_table = of_match_ptr(efnx_spi_of_match),
	},
	.probe = efinix_spi_probe,
	.id_table = efinix_ids,
};

module_spi_driver(efinix_spi_passive_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ian Dannapel <iansdannapel@gmail.com>");
MODULE_DESCRIPTION("Load Efinix FPGA firmware over SPI passive");
