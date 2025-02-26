// SPDX-License-Identifier: GPL-2.0+
/*
 * Generic MIPI-DSI/DPI(+SPI) Panel Driver
 *
 * Supported panels:
 * - A generic MIPI-DSI panel which implements basic DCS
 * - A generic MIPI-DPI panel which implements basic DCS over SPI
 *
 * Copyright (C) 2025, Hironori KIKUCHI <kikuchan98@gmail.com>
 */

#include <drm/drm_mipi_dbi.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <linux/backlight.h>
#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <video/mipi_display.h>

/*
 * The display panel configuration can be stored in a firmware file,
 * under the firmware directory of the system.
 *
 * The name of the file is `panels/<compatible>.panel`, where the
 * `<compatible>` is the first string of the `compatible` property
 * defined in the Device Tree for the device.
 *
 * File Layout:
 *     A binary file composed with the following data:
 *         <header>
 *         <config>
 *         <timings>
 *         <init-sequence>
 *
 * The 'header':
 *     A `struct panel_firmware_header`.
 *     The `file_format_version` must be `1`.
 *
 * The 'config':
 *     A `struct panel_firmware_config`.
 *     The values are in big-endian.
 *
 * The 'timings':
 *     An array of `struct panel_firmware_panel_timing`.
 *     Its length is `num_timings` in the config.
 *     The values are in big-endian.
 *
 * The 'init-sequence':
 *     MIPI commands to execute when the display pipeline is enabled.
 *     This is used to configure the display controller, and it may
 *     contains panel specific parameters as well.
 *
 *     The commands are encoded and stored in a byte array:
 *
 *         0x00             : sleep 10 ms
 *         0x01 <M>         : MIPI command <M> with no arguments
 *         0x02 <M> <a>     : MIPI command <M> with 1 argument <a>
 *         0x03 <M> <a> <b> : MIPI command <M> with 2 arguments <a> <b>
 *                :
 *         0x7f <M> <...>   : MIPI command <M> with 126 arguments <...>
 *         0x80             : sleep 100 ms
 *         0x81 - 0xff      : reserved
 *
 *     Example:
 *         command 0x11
 *         sleep 10ms
 *         command 0xb1 arguments 0x01 0x2c 0x2d
 *         command 0x29
 *         sleep 130ms
 *
 *     Byte sequence:
 *         0x01 0x11
 *         0x00
 *         0x04 0xb1 0x01 0x2c 0x2d
 *         0x01 0x29
 *         0x80 0x00 0x00 0x00
 *
 */
static const u8 panel_firmware_magic[15] = {
	0x50, 0x41, 0x4e, 0x45, 0x4c, 0x2d, 0x46, 0x49,
	0x52, 0x4d, 0x57, 0x41, 0x52, 0x45, 0x00,
};

struct panel_firmware_header {
	u8 magic[15];
	u8 file_format_version; /* must be 1 */
} __packed;

struct panel_firmware_config {
	u16 width_mm, height_mm;
	u16 rotation;
	u8 _reserved_1[2];
	u8 _reserved_2[8];

	u16 reset_delay; /* delay after the reset command, in ms */
	u16 init_delay; /* delay for sending the initial command sequence, in ms */
	u16 sleep_delay; /* delay after the sleep command, in ms */
	u16 backlight_delay; /* delay for enabling the backlight, in ms */
	u16 _reserved_3[4];

	u16 dsi_lanes; /* unsigned int */
	u16 dsi_format; /* enum mipi_dsi_pixel_format */
	u32 dsi_mode_flags; /* unsigned long */
	u32 bus_flags; /* struct drm_bus_flags */
	u8 _reserved_4[2];
	u8 preferred_timing;
	u8 num_timings;
} __packed;

struct panel_firmware_panel_timing {
	u16 hactive;
	u16 hfp;
	u16 hslen;
	u16 hbp;

	u16 vactive;
	u16 vfp;
	u16 vslen;
	u16 vbp;

	u32 dclk; /* in kHz */
	u32 flags; /* include/drm/drm_modes.h DRM_MODE_FLAG_* */

	u8 _reserved_1[8];
} __packed;

struct panel_commands {
	const u8 *data;
	size_t size;
};

struct panel_firmware {
	const struct firmware *blob;
	const struct panel_firmware_config *config;
	const struct panel_firmware_panel_timing *timings;
	struct panel_commands commands;
};

struct panel_mipi {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct mipi_dbi dbi;

	struct mutex lock;

	struct regulator_bulk_data supplies[2];
	struct gpio_desc *reset;

	unsigned int reset_delay;
	unsigned int init_delay;
	unsigned int sleep_delay;
	unsigned int backlight_delay;

	unsigned int dsi_lanes;
	enum mipi_dsi_pixel_format dsi_format;

	/* Panel info */
	u32 width_mm, height_mm;
	u32 bus_format, bus_flags;
	enum drm_panel_orientation orientation;

	struct list_head mode_list;

	struct panel_firmware *firmware;
	struct panel_commands commands;

	int (*write_command)(struct panel_mipi *mipi, const u8 *buf,
			     size_t len);
	int (*read_command)(struct panel_mipi *mipi, u8 cmd, u8 *buf,
			    size_t len);

	/* debugfs */
	struct dentry *debugfs;
	u8 recvbuf[1024];
	size_t recvlen;
	int display_id;
};

static inline struct panel_mipi *to_panel_mipi(struct drm_panel *panel)
{
	return container_of(panel, struct panel_mipi, panel);
}

static int panel_mipi_dsi_write(struct panel_mipi *mipi, const u8 *buf,
				size_t len)
{
	int ret;

	ret = mipi_dsi_dcs_write_buffer(mipi->dsi, buf, len);
	if (ret < 0)
		return ret;

	return 0;
}

static int panel_mipi_dbi_write(struct panel_mipi *mipi, const u8 *buf,
				size_t len)
{
	return mipi_dbi_command_stackbuf(&mipi->dbi, buf[0], buf + 1, len - 1);
}

#define panel_mipi_write(mipi, ...)                                \
	({                                                         \
		u8 _buf[] = { __VA_ARGS__ };                       \
		mipi->write_command(mipi, _buf, ARRAY_SIZE(_buf)); \
	})

static int panel_mipi_dsi_read(struct panel_mipi *mipi, u8 cmd, u8 *buf,
			       size_t len)
{
	int ret;

	ret = mipi_dsi_set_maximum_return_packet_size(mipi->dsi, len);
	if (ret)
		return ret;

	ret = mipi_dsi_dcs_read(mipi->dsi, cmd, buf, len);
	if (ret < 0)
		return ret;

	return 0;
}

static int panel_mipi_dbi_read(struct panel_mipi *mipi, u8 cmd, u8 *buf,
			       size_t len)
{
	void *recvbuf;
	int err;

	/* alloc DMA-safe buffer */
	recvbuf = kzalloc(len, GFP_KERNEL);
	if (!recvbuf)
		return -ENOMEM;

	err = mipi_dbi_command_buf(&mipi->dbi, cmd, recvbuf, len);
	if (!err)
		memcpy(buf, recvbuf, len);

	kfree(recvbuf);

	return err;
}

#define panel_mipi_read(mipi, cmd, buf, len) \
	(mipi)->read_command((mipi), (cmd), (buf), (len))

static unsigned long panel_mipi_dsi_set_mode_flags(struct panel_mipi *mipi,
						   unsigned long new_flags)
{
	unsigned long old_flags;

	if (!mipi->dsi)
		return 0;

	old_flags = mipi->dsi->mode_flags;

	mipi->dsi->mode_flags = new_flags;

	return old_flags;
}

static bool panel_mipi_dsi_set_lpm(struct panel_mipi *mipi, bool set)
{
	unsigned long old_flags;

	if (!mipi->dsi)
		return false;

	old_flags = panel_mipi_dsi_set_mode_flags(
		mipi, set ? mipi->dsi->mode_flags | MIPI_DSI_MODE_LPM :
			    mipi->dsi->mode_flags & ~MIPI_DSI_MODE_LPM);

	return old_flags & MIPI_DSI_MODE_LPM ? true : false;
}

#define PANEL_MIPI_TRANSACTION(mipi, stmt)   \
	do {                                 \
		mutex_lock(&(mipi)->lock);   \
		do {                         \
			stmt;                \
		} while (0);                 \
		mutex_unlock(&(mipi)->lock); \
	} while (0)

#define PANEL_MIPI_TRANSACTION_LPM(mipi, lpm, stmt)                       \
	PANEL_MIPI_TRANSACTION(                                           \
		mipi,                                                     \
		bool _lpm_backup = panel_mipi_dsi_set_lpm((mipi), (lpm)); \
		do { stmt; } while (0);                                   \
		panel_mipi_dsi_set_lpm((mipi), _lpm_backup))

static int panel_mipi_validate_commands(struct panel_mipi *mipi, const u8 *data,
					size_t size, size_t *valid_len)
{
	size_t i = 0;

	/* sanity check */
	while (i < size) {
		u8 inst = data[i];
		bool ext = (inst & 0x80) ? true : false;
		u8 len = inst & 0x7f;

		if (ext && len > 0)
			return -EINVAL; /* reserved */

		if (i + 1 + len > size)
			break;

		i += 1 + len;
	}

	*valid_len = i;

	return 0;
}

static int panel_mipi_load_commands(struct panel_mipi *mipi, const u8 *data,
				    size_t size)
{
	int err;
	size_t valid_len;

	err = panel_mipi_validate_commands(mipi, data, size, &valid_len);
	if (err)
		return err;

	if (valid_len != size)
		return -EINVAL;

	mipi->commands.data = data;
	mipi->commands.size = size;

	return 0;
}

static int panel_mipi_write_commands(struct panel_mipi *mipi, const u8 *data,
				     size_t size)
{
	const u8 *read_commands;
	unsigned int write_memory_bpw;
	size_t i = 0;
	int err = 0;

	if (!data)
		return 0;

	/*
	 * Disable interpretation for special commands for DBI
	 *
	 * This is required because the vendor specific custom commands
	 * may change its command set to the other.
	 */
	read_commands = mipi->dbi.read_commands;
	write_memory_bpw = mipi->dbi.write_memory_bpw;
	mipi->dbi.read_commands = NULL;
	mipi->dbi.write_memory_bpw = 8;

	while (i < size) {
		u8 inst = data[i++];
		bool ext = (inst & 0x80) ? true : false;
		u8 len = inst & 0x7f;

		if (len == 0x00) {
			msleep(ext ? 100 : 10);
			continue;
		}

		err = mipi->write_command(mipi, data + i, len);
		if (err)
			break;

		i += len;
	}

	/* restore */
	mipi->dbi.read_commands = read_commands;
	mipi->dbi.write_memory_bpw = write_memory_bpw;

	return err;
}

static int panel_mipi_read_firmware(const struct device *dev,
				    struct panel_mipi *mipi,
				    const struct panel_firmware *firmware)
{
	int rotation;
	int err;

	err = panel_mipi_load_commands(mipi, firmware->commands.data,
				       firmware->commands.size);
	if (err) {
		dev_err(dev, "firmware: Malformed command sequence\n");
		return err;
	}

	mipi->width_mm = be16_to_cpu(firmware->config->width_mm);
	mipi->height_mm = be16_to_cpu(firmware->config->height_mm);

	rotation = be16_to_cpu(firmware->config->rotation);
	if (rotation == 0)
		mipi->orientation = DRM_MODE_PANEL_ORIENTATION_NORMAL;
	else if (rotation == 90)
		mipi->orientation = DRM_MODE_PANEL_ORIENTATION_RIGHT_UP;
	else if (rotation == 180)
		mipi->orientation = DRM_MODE_PANEL_ORIENTATION_BOTTOM_UP;
	else if (rotation == 270)
		mipi->orientation = DRM_MODE_PANEL_ORIENTATION_LEFT_UP;
	else {
		dev_err(dev, "firmware: Invalid rotation %u\n", rotation);
		return -EINVAL;
	}

	mipi->reset_delay = be16_to_cpu(firmware->config->reset_delay);
	mipi->init_delay = be16_to_cpu(firmware->config->init_delay);
	mipi->sleep_delay = be16_to_cpu(firmware->config->sleep_delay);
	mipi->backlight_delay = be16_to_cpu(firmware->config->backlight_delay);

	mipi->bus_flags = be32_to_cpu(firmware->config->bus_flags);

	return 0;
}

static struct panel_firmware *panel_mipi_load_firmware(struct device *dev)
{
	const struct firmware *blob;
	const struct panel_firmware_header *hdr;
	const struct panel_firmware_config *config;
	const struct panel_firmware_panel_timing *timings;
	struct panel_firmware *firmware;
	const char *firmware_name;
	size_t command_offset;
	char filename[128];
	int err;

	err = of_property_read_string_index(dev->of_node, "compatible", 0,
					    &firmware_name);
	if (err)
		return ERR_PTR(err);

	snprintf(filename, sizeof(filename), "panels/%s.panel", firmware_name);
	err = request_firmware(&blob, filename, dev);
	if (err)
		return ERR_PTR(err);

	hdr = (const struct panel_firmware_header *)blob->data;

	if (blob->size < sizeof(*hdr)) {
		dev_err(dev, "firmware: %s: file size %zu is too small\n",
			filename, blob->size);
		err = -EINVAL;
		goto err;
	}

	if (memcmp(hdr->magic, panel_firmware_magic, sizeof(hdr->magic))) {
		dev_err(dev, "firmware: %s: Bad magic %15ph\n", filename,
			hdr->magic);
		err = -EINVAL;
		goto err;
	}

	if (hdr->file_format_version != 1) {
		dev_err(dev, "firmware: %s: version %u is not supported\n",
			filename, hdr->file_format_version);
		err = -EINVAL;
		goto err;
	}

	config = (struct panel_firmware_config *)(blob->data + sizeof(*hdr));
	timings = (struct panel_firmware_panel_timing *)(blob->data +
							 sizeof(*hdr) +
							 sizeof(*config));
	command_offset = sizeof(*hdr) + sizeof(*config) +
			 config->num_timings * sizeof(*timings);
	if (blob->size < command_offset) {
		dev_err(dev, "firmware: %s: file size %zu is too small\n",
			filename, blob->size);
		err = -EINVAL;
		goto err;
	}

	firmware = devm_kzalloc(dev, sizeof(*firmware), GFP_KERNEL);
	if (!firmware) {
		err = -ENOMEM;
		goto err;
	}

	firmware->blob = blob;
	firmware->config = config;
	firmware->timings = timings;
	firmware->commands.data = blob->data + command_offset;
	firmware->commands.size = blob->size - command_offset;

	dev_info(dev, "firmware: %s: loaded successfully\n", filename);

	return firmware;

err:
	release_firmware(blob);

	return ERR_PTR(err);
}

static int panel_mipi_read_display_id(struct panel_mipi *mipi)
{
	u8 buf[3];
	int err;

	err = panel_mipi_read(mipi, MIPI_DCS_GET_DISPLAY_ID, buf, sizeof(buf));

	return err ? err : ((buf[0] << 16) | (buf[1] << 8) | buf[2]);
}

static int panel_mipi_enable(struct drm_panel *panel)
{
	struct panel_mipi *mipi = to_panel_mipi(panel);
	int err;

	PANEL_MIPI_TRANSACTION(mipi, {
		err = panel_mipi_write(mipi, MIPI_DCS_SET_DISPLAY_ON);
	});
	if (err)
		return err;

	if (panel->backlight) {
		/* Wait for the picture to be ready before enabling backlight */
		msleep(mipi->backlight_delay);
	}

	return 0;
}

static int panel_mipi_disable(struct drm_panel *panel)
{
	struct panel_mipi *mipi = to_panel_mipi(panel);
	int err = 0;

	PANEL_MIPI_TRANSACTION(mipi, {
		err = panel_mipi_write(mipi, MIPI_DCS_SET_DISPLAY_OFF);
	});

	return err;
}

static int panel_mipi_prepare(struct drm_panel *panel)
{
	struct panel_mipi *mipi = to_panel_mipi(panel);
	int err, display_id;

	err = regulator_bulk_enable(ARRAY_SIZE(mipi->supplies), mipi->supplies);
	if (err)
		return err;

	PANEL_MIPI_TRANSACTION(mipi, {
		/* Reset the chip */
		if (mipi->reset) {
			gpiod_set_value_cansleep(mipi->reset, 1);
			msleep(mipi->reset_delay);
			gpiod_set_value_cansleep(mipi->reset, 0);
		}

		msleep(mipi->init_delay);

		/* Read the Display ID */
		mipi->display_id = panel_mipi_read_display_id(mipi);
		if (mipi->display_id >= 0)
			dev_info(panel->dev, "MIPI Display ID: %06x\n",
				 mipi->display_id);
		else
			dev_warn(panel->dev, "MIPI Display ID: Unknown\n");

		/* Write the init-sequence */
		err = panel_mipi_write_commands(mipi, mipi->commands.data,
						mipi->commands.size);
		if (err)
			break;

		/* Ensure to exit from a sleep mode */
		err = panel_mipi_write(mipi, MIPI_DCS_EXIT_SLEEP_MODE);
		if (err)
			break;
		msleep(mipi->sleep_delay);

		/*
		 * Read the Display ID again,
		 * because the init-sequence might have changed the ID.
		 */
		display_id = panel_mipi_read_display_id(mipi);
		if (display_id >= 0 && display_id != mipi->display_id)
			dev_info(panel->dev, "MIPI Display ID changed: %06x\n",
				 display_id);
	});

	if (err) {
		dev_err(panel->dev, "Failed to prepare the panel\n");
		regulator_bulk_disable(ARRAY_SIZE(mipi->supplies),
				       mipi->supplies);
		return err;
	}

	return 0;
}

static int panel_mipi_unprepare(struct drm_panel *panel)
{
	struct panel_mipi *mipi = to_panel_mipi(panel);
	int err;

	PANEL_MIPI_TRANSACTION(mipi, {
		err = panel_mipi_write(mipi, MIPI_DCS_ENTER_SLEEP_MODE);
		if (err)
			dev_warn(panel->dev, "Failed to enter a sleep mode\n");
		msleep(mipi->sleep_delay);
	});

	if (mipi->reset) {
		gpiod_set_value_cansleep(mipi->reset, 1);
		msleep(mipi->reset_delay);
	}

	regulator_bulk_disable(ARRAY_SIZE(mipi->supplies), mipi->supplies);

	return 0;
}

static int panel_mipi_get_modes(struct drm_panel *panel,
				struct drm_connector *connector)
{
	struct panel_mipi *mipi = to_panel_mipi(panel);
	struct drm_display_mode *mode;
	unsigned int count = 0;

	list_for_each_entry(mode, &mipi->mode_list, head) {
		struct drm_display_mode *dmode;

		dmode = drm_mode_duplicate(connector->dev, mode);
		if (!dmode)
			return -EINVAL;

		drm_mode_probed_add(connector, dmode);
		count++;
	}

	connector->display_info.bpc = 8;
	connector->display_info.width_mm = mipi->width_mm;
	connector->display_info.height_mm = mipi->height_mm;
	connector->display_info.bus_flags = mipi->bus_flags;
	drm_display_info_set_bus_formats(&connector->display_info,
					 &mipi->bus_format, 1);

	/*
	 * TODO: Remove once all drm drivers call
	 * drm_connector_set_orientation_from_panel()
	 */
	drm_connector_set_panel_orientation(connector, mipi->orientation);

	return count;
}

static enum drm_panel_orientation
panel_mipi_get_orientation(struct drm_panel *panel)
{
	struct panel_mipi *mipi = to_panel_mipi(panel);

	return mipi->orientation;
}

#ifdef CONFIG_DEBUG_FS

static int display_id_show(struct seq_file *s, void *data)
{
	struct drm_panel *panel = s->private;
	struct panel_mipi *mipi = to_panel_mipi(panel);
	int display_id;

	PANEL_MIPI_TRANSACTION(mipi, {
		display_id = panel_mipi_read_display_id(mipi);
	});

	if (display_id >= 0)
		seq_printf(s, "%06x\n", display_id);
	else
		seq_puts(s, "Unknown\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(display_id);

static int initial_display_id_show(struct seq_file *s, void *data)
{
	struct drm_panel *panel = s->private;
	struct panel_mipi *mipi = to_panel_mipi(panel);

	if (mipi->display_id >= 0)
		seq_printf(s, "%06x\n", mipi->display_id);
	else
		seq_puts(s, "Unknown\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(initial_display_id);

static int commands_open(struct inode *inode, struct file *file)
{
	struct panel_mipi *mipi = inode->i_private;

	mipi->recvlen = 0;
	file->private_data = mipi;

	return 0;
}

static ssize_t commands_write(struct file *file, const char __user *data,
			      size_t size, loff_t *pos)
{
	struct panel_mipi *mipi = file->private_data;
	ssize_t s = min_t(u64, sizeof(mipi->recvbuf) - mipi->recvlen, size);
	size_t valid_len;
	int err;

	if (!s || !mipi)
		return -EINVAL;

	if (copy_from_user(mipi->recvbuf + mipi->recvlen, data, s))
		return -EINVAL;

	mipi->recvlen += s;

	err = panel_mipi_validate_commands(mipi, mipi->recvbuf, mipi->recvlen,
					   &valid_len);
	if (err)
		return err;

	if (valid_len > 0) {
		PANEL_MIPI_TRANSACTION(mipi, {
			err = panel_mipi_write_commands(mipi, mipi->recvbuf,
							valid_len);
		});
		if (err)
			return err;

		mipi->recvlen -= valid_len;
		if (mipi->recvlen)
			memmove(mipi->recvbuf, mipi->recvbuf + valid_len,
				mipi->recvlen);
	}

	return s;
}

static int commands_release(struct inode *inode, struct file *file)
{
	struct panel_mipi *mipi = inode->i_private;

	if (mipi->recvlen) {
		dev_err(mipi->panel.dev, "Premature end of commands\n");
		return -EIO;
	}

	return 0;
}

static const struct file_operations commands_fops = {
	.owner = THIS_MODULE,
	.open = commands_open,
	.write = commands_write,
	.release = commands_release,
};

#endif

static void panel_mipi_debugfs_init(struct device *dev, struct panel_mipi *mipi)
{
#ifdef CONFIG_DEBUG_FS
	struct dentry *dir;

	mipi->debugfs = debugfs_lookup("panel-mipi", NULL);
	if (!mipi->debugfs)
		mipi->debugfs = debugfs_create_dir("panel-mipi", NULL);

	dir = debugfs_create_dir(dev_name(dev), mipi->debugfs);
	debugfs_create_file("display-id", 0600, dir, mipi, &display_id_fops);
	debugfs_create_file("initial-display-id", 0600, dir, mipi,
			    &initial_display_id_fops);
	debugfs_create_file("commands", 0600, dir, mipi, &commands_fops);
#endif
}

static void panel_mipi_debugfs_remove(struct panel_mipi *mipi)
{
#ifdef CONFIG_DEBUG_FS
	debugfs_remove_recursive(mipi->debugfs);
	mipi->debugfs = NULL;
#endif
}

static const struct drm_panel_funcs panel_mipi_funcs = {
	.prepare = panel_mipi_prepare,
	.unprepare = panel_mipi_unprepare,
	.enable = panel_mipi_enable,
	.disable = panel_mipi_disable,
	.get_modes = panel_mipi_get_modes,
	.get_orientation = panel_mipi_get_orientation,
};

static int panel_mipi_add_mode(struct device *dev, struct panel_mipi *mipi,
			       const struct drm_display_mode *src_mode)
{
	struct drm_display_mode *mode;

	mode = devm_kzalloc(dev, sizeof(*mode), GFP_KERNEL);
	if (!mode)
		return -ENOMEM;

	drm_mode_copy(mode, src_mode);

	if (!mode->clock)
		mode->clock = mode->htotal * mode->vtotal * 60 / 1000;

	mode->type |= DRM_MODE_TYPE_DRIVER;

	dev_info(dev, "Modeline " DRM_MODE_FMT " added\n", DRM_MODE_ARG(mode));

	list_add_tail(&mode->head, &mipi->mode_list);

	return 0;
}

static int panel_mipi_firmware_read_modes(const struct panel_firmware *firmware,
					  struct drm_display_mode *mode,
					  unsigned int i)
{
	const struct panel_firmware_panel_timing *timings =
		&firmware->timings[i];

	if (!firmware || i >= firmware->config->num_timings)
		return -ENOENT;

	if (!timings->hactive || !timings->vactive)
		return -ENOENT;

	memset(mode, 0, sizeof(*mode));

	mode->clock = be32_to_cpu(timings->dclk);

	mode->hdisplay = be16_to_cpu(timings->hactive);
	mode->hsync_start = mode->hdisplay + be16_to_cpu(timings->hfp);
	mode->hsync_end = mode->hsync_start + be16_to_cpu(timings->hslen);
	mode->htotal = mode->hsync_end + be16_to_cpu(timings->hbp);

	mode->vdisplay = be16_to_cpu(timings->vactive);
	mode->vsync_start = mode->vdisplay + be16_to_cpu(timings->vfp);
	mode->vsync_end = mode->vsync_start + be16_to_cpu(timings->vslen);
	mode->vtotal = mode->vsync_end + be16_to_cpu(timings->vbp);

	mode->flags = be32_to_cpu(timings->flags);

	drm_mode_set_name(mode);

	return 0;
}

static int panel_mipi_probe_modes(struct device *dev, struct panel_mipi *mipi)
{
	unsigned int i = 0;
	struct drm_display_mode mode = {};
	int err;

	INIT_LIST_HEAD(&mipi->mode_list);

	/* Read from the firmware */
	for (i = 0; i < mipi->firmware->config->num_timings; i++) {
		if (!panel_mipi_firmware_read_modes(mipi->firmware, &mode, i)) {
			if (mipi->firmware->config->preferred_timing == i)
				mode.type |= DRM_MODE_TYPE_PREFERRED;

			err = panel_mipi_add_mode(dev, mipi, &mode);
			if (err)
				return err;
		}
	}

	if (list_empty(&mipi->mode_list))
		return dev_err_probe(dev, -EINVAL, "No modes defined\n");

	return 0;
}

static void panel_mipi_cleanup(void *data)
{
	struct panel_mipi *mipi = (struct panel_mipi *)data;

	panel_mipi_debugfs_remove(mipi);

	drm_panel_remove(&mipi->panel);

	if (mipi->panel.enabled)
		drm_panel_disable(&mipi->panel);

	if (mipi->panel.prepared)
		drm_panel_unprepare(&mipi->panel);

	if (mipi->firmware)
		release_firmware(mipi->firmware->blob);
}

static int
panel_mipi_backlight_update_status(struct backlight_device *backlight)
{
	struct panel_mipi *mipi = dev_get_drvdata(&backlight->dev);
	int level = backlight_get_brightness(backlight);
	int err;

	PANEL_MIPI_TRANSACTION_LPM(mipi, false, {
		err = panel_mipi_write(mipi, MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
				       level, level);
	});

	return err;
}

static int
panel_mipi_backlight_get_brightness(struct backlight_device *backlight)
{
	return backlight_get_brightness(backlight);
}

static const struct backlight_ops panel_mipi_backlight_ops = {
	.get_brightness = panel_mipi_backlight_get_brightness,
	.update_status = panel_mipi_backlight_update_status,
};

static int panel_mipi_set_backlight(struct drm_panel *panel, struct device *dev,
				    struct panel_mipi *mipi)
{
	int err;

	err = drm_panel_of_backlight(panel);
	if (err)
		return err;

	if (!panel->backlight) {
		struct backlight_device *backlight = NULL;
		struct backlight_properties props = {
			.max_brightness = 255,
			.type = BACKLIGHT_RAW,
		};

		backlight = devm_backlight_device_register(
			dev, dev_name(dev), dev, mipi,
			&panel_mipi_backlight_ops, &props);
		if (IS_ERR(backlight))
			return PTR_ERR(backlight);

		panel->backlight = backlight;
	}

	return 0;
}

static int panel_mipi_probe(struct device *dev, int connector_type)
{
	struct panel_mipi *mipi;
	int err;

	mipi = devm_kzalloc(dev, sizeof(*mipi), GFP_KERNEL);
	if (!mipi)
		return -ENOMEM;

	mutex_init(&mipi->lock);

	mipi->display_id = -1;

	/* Get `power-supply` and `io-supply` (if any) */
	mipi->supplies[0].supply = "power";
	mipi->supplies[1].supply = "io";
	err = devm_regulator_bulk_get(dev, ARRAY_SIZE(mipi->supplies),
				      mipi->supplies);
	if (err < 0) {
		return dev_err_probe(dev, err,
				     "%pOF: Failed to get regulators\n",
				     dev->of_node);
	}

	/* GPIO for /RESET */
	mipi->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(mipi->reset))
		return dev_err_probe(dev, PTR_ERR(mipi->reset),
				     "%pOF: Failed to get GPIO for RESET\n",
				     dev->of_node);

	/* Load the firmware */
	mipi->firmware = panel_mipi_load_firmware(dev);
	if (IS_ERR(mipi->firmware))
		return dev_err_probe(dev, PTR_ERR(mipi->firmware),
				     "Failed to load firmware\n");

	err = panel_mipi_read_firmware(dev, mipi, mipi->firmware);
	if (err)
		return err;

	err = panel_mipi_probe_modes(dev, mipi);
	if (err)
		return err;

	/* DRM panel setup */
	drm_panel_init(&mipi->panel, dev, &panel_mipi_funcs, connector_type);

	err = panel_mipi_set_backlight(&mipi->panel, dev, mipi);
	if (err)
		return dev_err_probe(dev, err, "Failed to set backlight\n");

	drm_panel_add(&mipi->panel);

	dev_set_drvdata(dev, mipi);

	panel_mipi_debugfs_init(dev, mipi);

	return devm_add_action_or_reset(dev, panel_mipi_cleanup, mipi);
}

static int panel_mipi_dsi_probe(struct mipi_dsi_device *dsi)
{
	struct panel_mipi *mipi;
	int err;

	err = panel_mipi_probe(&dsi->dev, DRM_MODE_CONNECTOR_DSI);
	if (err)
		return err;

	mipi = dev_get_drvdata(&dsi->dev);
	mipi->dsi = dsi;
	mipi->write_command = panel_mipi_dsi_write;
	mipi->read_command = panel_mipi_dsi_read;

	/* Read from the firmware */
	dsi->lanes = be16_to_cpu(mipi->firmware->config->dsi_lanes);
	dsi->format = be16_to_cpu(mipi->firmware->config->dsi_format);
	dsi->mode_flags = be32_to_cpu(mipi->firmware->config->dsi_mode_flags);

	if (!dsi->lanes)
		return dev_err_probe(&dsi->dev, -EINVAL,
				     "dsi-lanes == 0 for DSI panel\n");

	/* Adjust bus_format */
	switch (dsi->format) {
	case MIPI_DSI_FMT_RGB888:
		mipi->bus_format = MEDIA_BUS_FMT_RGB888_1X24;
		break;
	case MIPI_DSI_FMT_RGB666:
		mipi->bus_format = MEDIA_BUS_FMT_RGB666_1X24_CPADHI;
		break;
	case MIPI_DSI_FMT_RGB666_PACKED:
		mipi->bus_format = MEDIA_BUS_FMT_RGB666_1X18;
		break;
	case MIPI_DSI_FMT_RGB565:
		mipi->bus_format = MEDIA_BUS_FMT_RGB565_1X16;
		break;
	}

	err = mipi_dsi_attach(dsi);
	if (err)
		return dev_err_probe(&dsi->dev, err,
				     "Failed to attach to DSI\n");

	return 0;
}

static int panel_mipi_spi_probe(struct spi_device *spi)
{
	struct panel_mipi *mipi;
	struct gpio_desc *dc;
	int err;

	err = panel_mipi_probe(&spi->dev, DRM_MODE_CONNECTOR_DPI);
	if (err)
		return err;

	mipi = dev_get_drvdata(&spi->dev);
	mipi->write_command = panel_mipi_dbi_write;
	mipi->read_command = panel_mipi_dbi_read;

	dc = devm_gpiod_get_optional(&spi->dev, "dc", GPIOD_OUT_LOW);
	if (IS_ERR(dc))
		return dev_err_probe(&spi->dev, PTR_ERR(dc),
				     "Failed to get GPIO for D/CX\n");

	err = mipi_dbi_spi_init(spi, &mipi->dbi, dc);
	if (err)
		return dev_err_probe(&spi->dev, err, "Failed to init SPI\n");

	return 0;
}

static void panel_mipi_dsi_remove(struct mipi_dsi_device *dsi)
{
	mipi_dsi_detach(dsi);
}

static const struct of_device_id panel_mipi_dsi_of_match[] = {
	{ .compatible = "panel-mipi-dsi" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, panel_mipi_dsi_of_match);

static const struct of_device_id panel_mipi_spi_of_match[] = {
	{ .compatible = "panel-mipi-dpi-spi" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, panel_mipi_spi_of_match);

static const struct spi_device_id panel_mipi_spi_ids[] = {
	{ "panel-mipi-dpi-spi" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(spi, panel_mipi_spi_ids);

static struct mipi_dsi_driver panel_mipi_dsi_driver = {
	.probe		= panel_mipi_dsi_probe,
	.remove		= panel_mipi_dsi_remove,
	.driver = {
		.name		= "panel-mipi",
		.of_match_table	= panel_mipi_dsi_of_match,
	},
};

static struct spi_driver panel_mipi_spi_driver = {
	.probe		= panel_mipi_spi_probe,
	.id_table	= panel_mipi_spi_ids,
	.driver = {
		.name		= "panel-mipi",
		.of_match_table	= panel_mipi_spi_of_match,
	},
};

static int __init panel_mipi_driver_init(void)
{
	int err;

	if (IS_ENABLED(CONFIG_SPI)) {
		err = spi_register_driver(&panel_mipi_spi_driver);
		if (err)
			return err;
	}

	if (IS_ENABLED(CONFIG_DRM_MIPI_DSI)) {
		err = mipi_dsi_driver_register(&panel_mipi_dsi_driver);
		if (err) {
			if (IS_ENABLED(CONFIG_SPI))
				spi_unregister_driver(&panel_mipi_spi_driver);
			return err;
		}
	}

	return 0;
}
module_init(panel_mipi_driver_init);

static void __exit panel_mipi_driver_exit(void)
{
	if (IS_ENABLED(CONFIG_DRM_MIPI_DSI))
		mipi_dsi_driver_unregister(&panel_mipi_dsi_driver);

	if (IS_ENABLED(CONFIG_SPI))
		spi_unregister_driver(&panel_mipi_spi_driver);
}
module_exit(panel_mipi_driver_exit);

MODULE_DESCRIPTION("Generic MIPI-DSI/DPI(+SPI) Panel Driver");
MODULE_AUTHOR("Hironori KIKUCHI <kikuchan98@gmail.com>");
MODULE_LICENSE("GPL");
