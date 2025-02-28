// SPDX-License-Identifier: GPL-2.0-only
/*
 * FPGA Manager Driver for Efinix
 *
 * Copyright (C) 2025 iris-GmbH infrared & intelligent sensors
 *
 * Ian Dannapel <iansdannapel@gmail.com>
 *
 * Load Efinix FPGA firmware over SPI using the serial configuration interface.
 *
 * Note 1: Only passive mode (host initiates transfer) is currently supported.
 * Note 2: Topaz and Titanium support is based on documentation but remains
 * untested.
 */

#include <linux/delay.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/spi/spi.h>

struct efinix_spi_conf {
	struct spi_device *spi;
	struct gpio_desc *cdone;
	struct gpio_desc *reset;
};

static void efinix_spi_reset(struct efinix_spi_conf *conf)
{
	gpiod_set_value(conf->reset, 1);
	/* tCRESET_N > 320 ns */
	usleep_range(1, 2);
	gpiod_set_value(conf->reset, 0);

	/* tDMIN > 32 us */
	usleep_range(35, 40);
}

static enum fpga_mgr_states efinix_spi_state(struct fpga_manager *mgr)
{
	struct efinix_spi_conf *conf = mgr->priv;

	if (conf->cdone && gpiod_get_value(conf->cdone) == 1)
		return FPGA_MGR_STATE_OPERATING;

	return FPGA_MGR_STATE_UNKNOWN;
}

static int efinix_spi_write_init(struct fpga_manager *mgr,
				 struct fpga_image_info *info,
				 const char *buf, size_t count)
{
	if (info->flags & FPGA_MGR_PARTIAL_RECONFIG) {
		dev_err(&mgr->dev, "Partial reconfiguration not supported\n");
		return -EOPNOTSUPP;
	}
	return 0;
}

static int efinix_spi_write(struct fpga_manager *mgr, const char *buf,
			    size_t count)
{
	struct efinix_spi_conf *conf = mgr->priv;
	int ret;
	struct spi_message message;
	struct spi_transfer assert_cs = {
		.cs_change = 1
	};
	struct spi_transfer write_xfer = {
		.tx_buf = buf,
		.len = count
	};
	struct spi_transfer clk_cycles = {
		.len = 13,  // > 100 clock cycles
		.tx_buf = NULL
	};
	u8 *dummy_buf;

	dummy_buf = kzalloc(13, GFP_KERNEL);
	if (!dummy_buf) {
		ret = -ENOMEM;
		goto fail;
	}

	spi_bus_lock(conf->spi->controller);
	spi_message_init(&message);
	spi_message_add_tail(&assert_cs, &message);
	ret = spi_sync_locked(conf->spi, &message);
	if (ret)
		goto fail_unlock;

	/* reset with asserted cs */
	efinix_spi_reset(conf);

	spi_message_init(&message);
	spi_message_add_tail(&write_xfer, &message);

	clk_cycles.tx_buf = dummy_buf;
	spi_message_add_tail(&clk_cycles, &message);

	ret = spi_sync_locked(conf->spi, &message);
	if (ret)
		dev_err(&mgr->dev, "SPI error in firmware write: %d\n", ret);

fail_unlock:
	spi_bus_unlock(conf->spi->controller);
	kfree(dummy_buf);
fail:
	return ret;
}

static int efinix_spi_write_complete(struct fpga_manager *mgr,
				     struct fpga_image_info *info)
{
	struct efinix_spi_conf *conf = mgr->priv;
	unsigned long timeout =
		jiffies + usecs_to_jiffies(info->config_complete_timeout_us);
	bool expired = false;
	int done;

	if (conf->cdone) {
		while (!expired) {
			expired = time_after(jiffies, timeout);

			done = gpiod_get_value(conf->cdone);
			if (done < 0)
				return done;

			if (done)
				break;
		}
	}

	if (expired)
		return -ETIMEDOUT;

	/* tUSER > 25 us */
	usleep_range(30, 35);
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

	conf->reset = devm_gpiod_get(&spi->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(conf->reset))
		return dev_err_probe(&spi->dev, PTR_ERR(conf->reset),
				     "Failed to get RESET gpio\n");

	if (!(spi->mode & SPI_CPHA) || !(spi->mode & SPI_CPOL))
		return dev_err_probe(&spi->dev, -EINVAL,
				     "Unsupported SPI mode, set CPHA and CPOL\n");

	conf->cdone = devm_gpiod_get_optional(&spi->dev, "cdone", GPIOD_IN);
	if (IS_ERR(conf->cdone))
		return dev_err_probe(&spi->dev, PTR_ERR(conf->cdone),
				     "Failed to get CDONE gpio\n");

	mgr = devm_fpga_mgr_register(&spi->dev,
				     "Efinix FPGA Manager",
				     &efinix_spi_ops, conf);

	return PTR_ERR_OR_ZERO(mgr);
}

static const struct of_device_id efinix_spi_of_match[] = {
	{ .compatible = "efinix,trion-spi", },
	{ .compatible = "efinix,titanium-spi", },
	{ .compatible = "efinix,topaz-spi", },
	{ .compatible = "efinix,fpga-spi", },
	{}
};
MODULE_DEVICE_TABLE(of, efinix_spi_of_match);

static const struct spi_device_id efinix_ids[] = {
	{ "trion-spi", 0 },
	{ "titanium-spi", 0 },
	{ "topaz-spi", 0 },
	{},
};
MODULE_DEVICE_TABLE(spi, efinix_ids);

static struct spi_driver efinix_spi_driver = {
	.driver = {
		.name = "efinix-spi",
		.of_match_table = efinix_spi_of_match,
	},
	.probe = efinix_spi_probe,
	.id_table = efinix_ids,
};

module_spi_driver(efinix_spi_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ian Dannapel <iansdannapel@gmail.com>");
MODULE_DESCRIPTION("Efinix FPGA SPI Programming Driver (Topaz/Titanium untested)");
