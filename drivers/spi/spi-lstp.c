// SPDX-License-Identifier: GPL-2.0-only
/*
 * SPI driver for LSTP USB interface.
 *
 * Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES.  All rights reserved.
 */

#include <linux/spi/spi.h>
#include <linux/acpi.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/property.h>

#include <linux/usb/lstp.h>

enum lstp_spi_cmd {
	LSTP_SPI_CMD_READ = 0x01,
	LSTP_SPI_CMD_WRITE = 0x02,
	LSTP_SPI_CMD_WRITE_READ = 0x03,
	LSTP_SPI_CMD_POSTED_WRITE = 0x04
};

/* clang-format off */
enum lstp_spi_cmd_cs_toggle {
	LSTP_SPI_CMD_CS_ASSERT = 0x20,
	LSTP_SPI_CMD_CS_DEASSERT = 0x10
};

enum lstp_spi_cmd_cs_sel {
	LSTP_SPI_CMD_CS0 = 0x00,
	LSTP_SPI_CMD_CS1 = 0x40
};

static const u8 lstp_spi_cs_map[] = {
	LSTP_SPI_CMD_CS0,
	LSTP_SPI_CMD_CS1
}; /* clang-format on */

struct lstp_spi_read_req {
	__le32 len;
} __packed;

struct lstp_spi_config {
	u8 num_devices;
	__le32 speed_hz;
} __packed;

struct lstp_spi_priv {
	struct spi_controller *ctrl;
	u32 current_speed_hz;
};

/* A transfer without data still sends a zero-length WRITE when @cs_flags moves CS. */
static int lstp_spi_do_transfer(struct lstp_channel *ch, struct spi_transfer *xfer, u8 cs_bits,
				u8 cs_flags)
{
	struct lstp_spi_priv *priv = ch->priv;

	if (xfer->word_delay.value)
		dev_warn_once(ch->dev, "%s: ch_%d: word_delay not supported, ignoring\n", __func__,
			      ch->ch_id);

	xfer->error = SPI_TRANS_FAIL_NO_START;
	xfer->effective_speed_hz = priv->current_speed_hz;

	if (xfer->len && !xfer->tx_buf && !xfer->rx_buf) {
		dev_err_ratelimited(ch->dev, "%s: ch_%d: Transfer of %u bytes has no buffer\n",
				    __func__, ch->ch_id, xfer->len);
		return -EINVAL;
	}
	if (xfer->tx_buf && xfer->len > ch->max_tx_payload) {
		dev_err_ratelimited(ch->dev,
				    "%s: ch_%d: TX transfer length %u exceeds capacity %zu\n",
				    __func__, ch->ch_id, xfer->len, ch->max_tx_payload);
		return -EINVAL;
	}
	if (xfer->rx_buf && xfer->len > ch->max_rx_payload) {
		dev_err_ratelimited(ch->dev,
				    "%s: ch_%d: RX transfer length %u exceeds capacity %zu\n",
				    __func__, ch->ch_id, xfer->len, ch->max_rx_payload);
		return -EINVAL;
	}

	int ret;

	if (xfer->tx_buf && xfer->rx_buf) {
		u8 cmd = LSTP_SPI_CMD_WRITE_READ | cs_bits | cs_flags;

		ret = lstp_request(ch, cmd, xfer->tx_buf, xfer->len, xfer->rx_buf, xfer->len, NULL,
				   NULL);
	} else if (xfer->tx_buf) {
		u8 cmd = LSTP_SPI_CMD_WRITE | cs_bits | cs_flags;

		ret = lstp_request(ch, cmd, xfer->tx_buf, xfer->len, NULL, 0, NULL, NULL);
	} else if (xfer->rx_buf) {
		struct lstp_spi_read_req req = { .len = cpu_to_le32(xfer->len) };
		u8 cmd = LSTP_SPI_CMD_READ | cs_bits | cs_flags;

		ret = lstp_request(ch, cmd, &req, sizeof(req), xfer->rx_buf, xfer->len, NULL, NULL);
	} else if (cs_flags) {
		u8 cmd = LSTP_SPI_CMD_WRITE | cs_bits | cs_flags;

		ret = lstp_request(ch, cmd, NULL, 0, NULL, 0, NULL, NULL);
	} else {
		/* No data and no CS change; only the caller's delay is left to do. */
		ret = 0;
	}

	if (ret) {
		if (ret != -ENODEV)
			dev_err_ratelimited(ch->dev, "%s: ch_%d: Transfer failed (%pe)\n", __func__,
					    ch->ch_id, ERR_PTR(ret));
		xfer->error = SPI_TRANS_FAIL_IO;
		return ret;
	}

	xfer->error = 0;
	return 0;
}

/*
 * CS edges ride in each transfer's command byte rather than in a set_cs callback, which would
 * cost a USB round trip per edge.
 */
static int lstp_spi_transfer_one_message(struct spi_controller *ctrl, struct spi_message *mesg)
{
	struct lstp_channel *ch = spi_controller_get_devdata(ctrl);
	struct spi_transfer *xfer;
	u8 chip_select = spi_get_chipselect(mesg->spi, 0);
	int ret = 0;
	bool cs_active = false;

	if (chip_select >= ctrl->num_chipselect) {
		dev_err_ratelimited(ch->dev, "%s: ch_%d: Invalid chip select %u (max %u)\n",
				    __func__, ch->ch_id, chip_select, ctrl->num_chipselect - 1);
		ret = -EINVAL;
		goto finalize;
	}

	u8 cs_bits = lstp_spi_cs_map[chip_select];

	list_for_each_entry(xfer, &mesg->transfers, transfer_list) {
		u8 cs_flags = 0;
		bool is_last = list_is_last(&xfer->transfer_list, &mesg->transfers);
		bool no_data = !xfer->tx_buf && !xfer->rx_buf;

		if (!cs_active) {
			cs_flags |= LSTP_SPI_CMD_CS_ASSERT;
			cs_active = true;
		}

		if ((is_last && !xfer->cs_change) || (!is_last && xfer->cs_change)) {
			cs_flags |= LSTP_SPI_CMD_CS_DEASSERT;
			cs_active = false;
		}

		/* A data transfer fuses delay and CS edge into one command; CS-only waits first. */
		if (no_data)
			spi_transfer_delay_exec(xfer);

		ret = lstp_spi_do_transfer(ch, xfer, cs_bits, cs_flags);
		if (ret)
			break;

		mesg->actual_length += xfer->len;

		if (!is_last && xfer->cs_change)
			spi_transfer_cs_change_delay_exec(mesg, xfer);

		if (!no_data)
			spi_transfer_delay_exec(xfer);
	}

finalize:
	mesg->status = ret;
	spi_finalize_current_message(ctrl);
	return ret;
}

/* spi_setup() path: firmware owns CS, so cs_setup/cs_hold/cs_inactive are only warned about. */
static int lstp_spi_set_cs_timing(struct spi_device *spi)
{
	struct lstp_channel *ch = spi_controller_get_devdata(spi->controller);

	if (spi->cs_setup.value || spi->cs_hold.value || spi->cs_inactive.value)
		dev_warn_ratelimited(ch->dev,
				     "%s: ch_%d: CS timing delays not supported, ignoring\n",
				     __func__, ch->ch_id);
	return 0;
}

static size_t lstp_spi_max_xfer_size(struct spi_device *spi)
{
	struct lstp_channel *ch = spi_controller_get_devdata(spi->controller);

	return min(ch->max_tx_payload, ch->max_rx_payload);
}

/*
 * Device tree lists peripherals as children of the channel node, so an empty node is an
 * incomplete description. ACPI resolves SpiSerialBus() resources from anywhere in the namespace,
 * and the SPI core enumerates nothing from a software node.
 */
static void lstp_spi_enumerate(struct lstp_channel *ch, struct spi_controller *ctrl)
{
	if (is_of_node(ch->fwnode)) {
		unsigned int child_count = 0;
		struct fwnode_handle *child;

		fwnode_for_each_available_child_node(ch->fwnode, child)
			child_count++;

		if (child_count == 0) {
			dev_warn(ch->dev,
				 "%s: ch_%d: Firmware node present but defines no SPI devices\n",
				 __func__, ch->ch_id);
		} else {
			dev_info(ch->dev, "%s: ch_%d: Firmware node present with %u SPI devices\n",
				 __func__, ch->ch_id, child_count);
		}
	} else if (is_acpi_node(ch->fwnode)) {
		dev_info(ch->dev,
			 "%s: ch_%d: ACPI describes this channel; its peripherals resolve from the namespace\n",
			 __func__, ch->ch_id);
	} else if (ch->fwnode) {
		dev_warn(ch->dev,
			 "%s: ch_%d: Software node describes this channel, so no SPI devices are enumerated for it\n",
			 __func__, ch->ch_id);
	}
}

static int lstp_spi_probe(struct auxiliary_device *auxdev, const struct auxiliary_device_id *id)
{
	struct lstp_channel *ch = lstp_channel_from_auxdev(auxdev);
	const struct lstp_channel_init_info *info = dev_get_platdata(&auxdev->dev);

	if (!info)
		return -ENODEV;

	if (info->config_len < sizeof(struct lstp_spi_config)) {
		dev_err(ch->dev, "%s: ch_%d: SPI config too short: %zu < %zu\n", __func__,
			ch->ch_id, info->config_len, sizeof(struct lstp_spi_config));
		return -EINVAL;
	}

	const struct lstp_spi_config *config = info->config;
	struct lstp_spi_priv *priv = devm_kzalloc(ch->dev, sizeof(*priv), GFP_KERNEL);

	if (!priv)
		return -ENOMEM;

	struct spi_controller *ctrl = devm_spi_alloc_host(ch->dev, 0);

	if (!ctrl)
		return -ENOMEM;

	u8 num_devices = config->num_devices;
	u32 speed_hz = le32_to_cpu(config->speed_hz);

	if (num_devices == 0 || num_devices > ARRAY_SIZE(lstp_spi_cs_map)) {
		dev_err(ch->dev, "%s: ch_%d: Invalid num_devices %u (max %zu)\n", __func__,
			ch->ch_id, num_devices, ARRAY_SIZE(lstp_spi_cs_map));
		return -EINVAL;
	}

	if (speed_hz == 0) {
		dev_err(ch->dev, "%s: ch_%d: Device returned SPI speed of %u Hz\n", __func__,
			ch->ch_id, speed_hz);
		return -EINVAL;
	}

	spi_controller_set_devdata(ctrl, ch);
	ctrl->bus_num = -1;
	ctrl->num_chipselect = num_devices;
	ctrl->transfer_one_message = lstp_spi_transfer_one_message;
	ctrl->max_transfer_size = lstp_spi_max_xfer_size;
	ctrl->set_cs_timing = lstp_spi_set_cs_timing;

	device_set_node(&ctrl->dev, ch->fwnode);

	priv->ctrl = ctrl;
	priv->current_speed_hz = speed_hz;
	/* Registration probes peripherals, which may transfer at once; transfers read @ch->priv. */
	ch->priv = priv;

	int ret = devm_spi_register_controller(ch->dev, ctrl);

	if (ret) {
		dev_err(ch->dev, "%s: ch_%d: Could not register SPI controller (%pe)\n", __func__,
			ch->ch_id, ERR_PTR(ret));
		return ret;
	}

	lstp_spi_enumerate(ch, ctrl);

	dev_info(ch->dev, "%s: ch_%d: Started as %s (devices=%u, speed=%u Hz)\n", __func__,
		 ch->ch_id, ch->display_name, ctrl->num_chipselect, priv->current_speed_hz);
	return 0;
}

static const struct auxiliary_device_id lstp_spi_id_table[] = { { .name = "lstp.spi" }, {} };
MODULE_DEVICE_TABLE(auxiliary, lstp_spi_id_table);

static struct lstp_auxiliary_driver lstp_spi_drv = {
	.adrv = {
		.name = "spi",
		.probe = lstp_spi_probe,
		.id_table = lstp_spi_id_table,
	},
};
module_lstp_auxiliary_driver(lstp_spi_drv);

MODULE_IMPORT_NS("LSTP");
MODULE_DESCRIPTION("LSTP SPI driver");
MODULE_LICENSE("GPL");
