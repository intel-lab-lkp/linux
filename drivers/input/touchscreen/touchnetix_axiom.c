// SPDX-License-Identifier: GPL-2.0-only
/*
 * TouchNetix axiom Touchscreen Driver
 *
 * Copyright (C) 2020-2023 TouchNetix Ltd.
 *
 * Author(s): Bart Prescott <bartp@baasheep.co.uk>
 *            Pedro Torruella <pedro.torruella@touchnetix.com>
 *            Mark Satterthwaite <mark.satterthwaite@touchnetix.com>
 *            Hannah Rossiter <hannah.rossiter@touchnetix.com>
 *            Kamel Bouhara <kamel.bouhara@bootlin.com>
 *
 */
#include <linux/bitfield.h>
#include <linux/crc16.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/regmap.h>

#include <asm/unaligned.h>
#define AXIOM_PROX_LEVEL		-128
#define AXIOM_DMA_OPS_DELAY_USEC	250
#define AXIOM_STARTUP_TIME_MS		110
/*
 * Register group u31 has 2 pages for usage table entries.
 */
#define AXIOM_U31_MAX_USAGES		0xff
#define AXIOM_U31_BYTES_PER_USAGE	6
#define AXIOM_U31_PAGE0_LENGTH		0x0C
#define AXIOM_U31_BOOTMODE_MASK		BIT(7)
#define AXIOM_U31_DEVID_MASK		GENMASK(14, 0)

#define AXIOM_MAX_REPORT_LEN		0x7f

#define AXIOM_CMD_HEADER_READ_MASK	BIT(15)
#define AXIOM_U41_MAX_TARGETS		10

#define AXIOM_U46_AUX_CHANNELS		4
#define AXIOM_U46_AUX_MASK		GENMASK(11, 0)

#define AXIOM_COMMS_MAX_USAGE_PAGES	3
#define AXIOM_COMMS_PAGE_SIZE		256
#define AXIOM_COMMS_REPORT_LEN_MASK	GENMASK(6, 0)

#define AXIOM_REPORT_USAGE_ID		0x34
#define AXIOM_DEVINFO_USAGE_ID		0x31
#define AXIOM_USAGE_2HB_REPORT_ID	0x01
#define AXIOM_USAGE_2AUX_REPORT_ID	0x46
#define AXIOM_USAGE_2DCTS_REPORT_ID	0x41

#define AXIOM_PAGE_OFFSET_MASK		GENMASK(6, 0)

struct axiom_devinfo {
	__le16 device_id;
	u8 fw_minor;
	u8 fw_major;
	u8 fw_info_extra;
	u8 tcp_revision;
	u8 bootloader_fw_minor;
	u8 bootloader_fw_major;
	__le16 jedec_id;
	u8 num_usages;
} __packed;

/*
 * Describes parameters of a specific usage, essentially a single element of
 * the "Usage Table"
 */
struct axiom_usage_entry {
	u8 id;
	u8 is_report;
	u8 start_page;
	u8 num_pages;
};

/*
 * Represents state of a touch or target when detected prior to a touch (eg.
 * hover or proximity events).
 */
enum axiom_target_state {
	AXIOM_TARGET_STATE_NOT_PRESENT = 0,
	AXIOM_TARGET_STATE_PROX = 1,
	AXIOM_TARGET_STATE_HOVER = 2,
	AXIOM_TARGET_STATE_TOUCHING = 3,
};

struct axiom_u41_target {
	enum axiom_target_state state;
	u16 x;
	u16 y;
	s8 z;
	bool insert;
	bool touch;
};

struct axiom_target_report {
	u8 index;
	u8 present;
	u16 x;
	u16 y;
	s8 z;
};

struct axiom_cmd_header {
	__le16 target_address;
	__le16 length;
} __packed;

struct axiom_data {
	struct axiom_devinfo devinfo;
	struct device *dev;
	struct gpio_desc *reset_gpio;
	struct i2c_client *client;
	struct input_dev *input_dev;
	u32 max_report_len;
	u8 rx_buf[AXIOM_COMMS_MAX_USAGE_PAGES * AXIOM_COMMS_PAGE_SIZE];
	struct axiom_u41_target targets[AXIOM_U41_MAX_TARGETS];
	struct axiom_usage_entry usage_table[AXIOM_U31_MAX_USAGES];
	bool usage_table_populated;
	struct regulator *vdda;
	struct regulator *vddi;
	struct regmap *regmap;
	struct touchscreen_properties	prop;
};

static const struct regmap_config axiom_i2c_regmap_config = {
	.reg_bits = 32,
	.reg_format_endian = REGMAP_ENDIAN_LITTLE,
	.val_bits = 8,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
};

/*
 * axiom devices are typically configured to report touches at a rate
 * of 100Hz (10ms) for systems that require polling for reports.
 * When reports are polled, it will be expected to occasionally
 * observe the overflow bit being set in the reports.
 * This indicates that reports are not being read fast enough.
 */
#define POLL_INTERVAL_DEFAULT_MS 10

/* Translate usage/page/offset triplet into physical address. */
static u16 axiom_usage_to_target_address(struct axiom_data *ts, u8 usage, u8 page,
					 char offset)
{
	/* At the moment the convention is that u31 is always at physical address 0x0 */
	if (!ts->usage_table_populated) {
		if (usage == AXIOM_DEVINFO_USAGE_ID)
			return ((page << 8) + offset);
		else
			return 0xffff;
	}

	if (page >= ts->usage_table[usage].num_pages) {
		dev_err(ts->dev, "Invalid usage table! usage: u%02x, page: %02x, offset: %02x\n",
			usage, page, offset);
		return 0xffff;
	}

	return ((ts->usage_table[usage].start_page + page) << 8) + offset;
}

static int axiom_read(struct axiom_data *ts, u8 usage, u8 page, void *buf, u16 len)
{
	struct axiom_cmd_header cmd_header;
	u32 preamble;
	int ret;

	cmd_header.target_address = cpu_to_le16(axiom_usage_to_target_address(ts, usage, page, 0));
	cmd_header.length = cpu_to_le16(len | AXIOM_CMD_HEADER_READ_MASK);

	preamble = get_unaligned_le32(&cmd_header);

	ret = regmap_write(ts->regmap, preamble, 0);
	if (ret) {
		dev_err(ts->dev, "failed to write preamble, error %d\n", ret);
		return ret;
	}

	ret = regmap_raw_read(ts->regmap, 0, buf, len);
	if (ret) {
		dev_err(ts->dev, "failed to read target address %04x, error %d\n",
			cmd_header.target_address, ret);
		return ret;
	}

	/* Wait device's DMA operations */
	usleep_range(AXIOM_DMA_OPS_DELAY_USEC, AXIOM_DMA_OPS_DELAY_USEC + 50);

	return 0;
}

/*
 * One of the main purposes for reading the usage table is to identify
 * which usages reside at which target address.
 * When performing subsequent reads or writes to AXIOM, the target address
 * is used to specify which usage is being accessed.
 * Consider the following discovery code which will build up the usage table.
 */
static u32 axiom_populate_usage_table(struct axiom_data *ts)
{
	struct axiom_usage_entry *usage_table;
	u8 *rx_data = ts->rx_buf;
	u32 max_report_len = 0;
	u32 usage_id;
	int error;

	usage_table = ts->usage_table;

	/* Read the second page of usage u31 to get the usage table */
	error = axiom_read(ts, AXIOM_DEVINFO_USAGE_ID, 1, rx_data,
			   (AXIOM_U31_BYTES_PER_USAGE * ts->devinfo.num_usages));

	if (error)
		return error;

	for (usage_id = 0; usage_id < ts->devinfo.num_usages; usage_id++) {
		u16 offset = (usage_id * AXIOM_U31_BYTES_PER_USAGE);
		u8 id = rx_data[offset + 0];
		u8 start_page = rx_data[offset + 1];
		u8 num_pages = rx_data[offset + 2];
		u32 max_offset = ((rx_data[offset + 3] & AXIOM_PAGE_OFFSET_MASK) + 1) * 2;

		usage_table[id].is_report = !num_pages;

		/* Store the entry into the usage table */
		usage_table[id].id = id;
		usage_table[id].start_page = start_page;
		usage_table[id].num_pages = num_pages;

		dev_dbg(ts->dev, "Usage u%02x Info: %*ph\n", id, AXIOM_U31_BYTES_PER_USAGE,
			&rx_data[offset]);

		/* Identify the max report length the module will receive */
		if (usage_table[id].is_report && max_offset > max_report_len)
			max_report_len = max_offset;
	}

	ts->usage_table_populated = true;

	return max_report_len;
}

static int axiom_discover(struct axiom_data *ts)
{
	int error;

	/*
	 * Fetch the first page of usage u31 to get the
	 * device information and the number of usages
	 */
	error = axiom_read(ts, AXIOM_DEVINFO_USAGE_ID, 0, &ts->devinfo, AXIOM_U31_PAGE0_LENGTH);
	if (error)
		return error;

	dev_dbg(ts->dev, "  Boot Mode      : %s\n",
		FIELD_GET(AXIOM_U31_BOOTMODE_MASK,
			  le16_to_cpu(ts->devinfo.device_id)) ? "BLP" : "TCP");
	dev_dbg(ts->dev, "  Device ID      : %04lx\n",
		FIELD_GET(AXIOM_U31_DEVID_MASK, le16_to_cpu(ts->devinfo.device_id)));
	dev_dbg(ts->dev, "  Firmware Rev   : %02x.%02x\n", ts->devinfo.fw_major,
		ts->devinfo.fw_minor);
	dev_dbg(ts->dev, "  Bootloader Rev : %02x.%02x\n", ts->devinfo.bootloader_fw_major,
		ts->devinfo.bootloader_fw_minor);
	dev_dbg(ts->dev, "  FW Extra Info  : %04x\n", ts->devinfo.fw_info_extra);
	dev_dbg(ts->dev, "  Silicon        : %04x\n", le16_to_cpu(ts->devinfo.jedec_id));
	dev_dbg(ts->dev, "  Number usages        : %04x\n", ts->devinfo.num_usages);

	ts->max_report_len = axiom_populate_usage_table(ts);
	if (!ts->max_report_len || !ts->devinfo.num_usages ||
	    ts->max_report_len > AXIOM_MAX_REPORT_LEN) {
		dev_err(ts->dev, "Invalid report length or usages number");
		return -EINVAL;
	}

	dev_dbg(ts->dev, "Max Report Length: %u\n", ts->max_report_len);

	return 0;
}

/*
 * Support function to axiom_process_u41_report.
 * Generates input-subsystem events for every target.
 * After calling this function the caller shall issue
 * a Sync to the input sub-system.
 */
static bool axiom_process_u41_report_target(struct axiom_data *ts,
					    struct axiom_target_report *target)
{
	struct input_dev *input_dev = ts->input_dev;
	struct axiom_u41_target *target_prev_state;
	enum axiom_target_state current_state;
	int id;

	/* Verify the target index */
	if (target->index >= AXIOM_U41_MAX_TARGETS) {
		dev_err(ts->dev, "Invalid target index! %u\n", target->index);
		return false;
	}

	target_prev_state = &ts->targets[target->index];

	current_state = AXIOM_TARGET_STATE_NOT_PRESENT;

	if (target->present) {
		if (target->z >= 0)
			current_state = AXIOM_TARGET_STATE_TOUCHING;
		else if (target->z > AXIOM_PROX_LEVEL && target->z < 0)
			current_state = AXIOM_TARGET_STATE_HOVER;
		else if (target->z == AXIOM_PROX_LEVEL)
			current_state = AXIOM_TARGET_STATE_PROX;
	}

	if (target_prev_state->state == current_state &&
	    target_prev_state->x == target->x &&
	    target_prev_state->y == target->y &&
	    target_prev_state->z == target->z)
		return false;

	id = target->index;

	dev_dbg(ts->dev, "U41 Target T%u, slot:%u present:%u, x:%u, y:%u, z:%d\n",
		target->index, slot, target->present,
		target->x, target->y, target->z);

	switch (current_state) {
	case AXIOM_TARGET_STATE_NOT_PRESENT:
	case AXIOM_TARGET_STATE_PROX:
		if (!target_prev_state->insert)
			break;
		target_prev_state->insert = false;

		if (!id)
			input_report_key(input_dev, BTN_TOUCH, 0);

		input_mt_report_slot_inactive(input_dev);
		/*
		 * make sure the previous coordinates are
		 * all off screen when the finger comes back
		 */
		target->x = 65535;
		target->y = 65535;
		target->z = AXIOM_PROX_LEVEL;
		break;
	case AXIOM_TARGET_STATE_HOVER:
	case AXIOM_TARGET_STATE_TOUCHING:
		target_prev_state->insert = true;
		input_report_abs(input_dev, ABS_MT_TRACKING_ID, id);
		input_report_abs(input_dev, ABS_MT_POSITION_X, target->x);
		input_report_abs(input_dev, ABS_MT_POSITION_Y, target->y);

		if (current_state == AXIOM_TARGET_STATE_TOUCHING) {
			input_report_abs(input_dev, ABS_MT_DISTANCE, 0);
			input_report_abs(input_dev, ABS_DISTANCE, 0);
			input_report_abs(input_dev, ABS_MT_PRESSURE, target->z);
			input_report_abs(input_dev, ABS_PRESSURE, target->z);
		} else {
			input_report_abs(input_dev, ABS_MT_DISTANCE, -target->z);
			input_report_abs(input_dev, ABS_DISTANCE, -target->z);
			input_report_abs(input_dev, ABS_MT_PRESSURE, 0);
			input_report_abs(input_dev, ABS_PRESSURE, 0);
		}

		if (!id)
			input_report_key(input_dev, BTN_TOUCH, (current_state ==
					 AXIOM_TARGET_STATE_TOUCHING));
		break;
	default:
		break;
	}

	target_prev_state->state = current_state;
	target_prev_state->x = target->x;
	target_prev_state->y = target->y;
	target_prev_state->z = target->z;

	return true;
}

/*
 * U41 is the output report of the 2D CTS and contains the status of targets
 * (including contacts and pre-contacts) along with their X,Y,Z values.
 * When a target has been removed (no longer detected),
 * the corresponding X,Y,Z values will be zeroed.
 */
static bool axiom_process_u41_report(struct axiom_data *ts, u8 *rx_buf)
{
	struct axiom_target_report target;
	bool update_done = false;
	u16 target_status;
	int i;

	target_status = get_unaligned_le16(rx_buf + 1);

	for (i = 0; i < AXIOM_U41_MAX_TARGETS; i++) {
		u8 *target_step = &rx_buf[i * 4];

		target.index = i;
		input_mt_slot(ts->input_dev, i);
		input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, true);
		target.present = ((target_status & (1 << i)) != 0) ? 1 : 0;
		target.x = get_unaligned_le16(target_step + 3);
		target.y = get_unaligned_le16(target_step + 5);
		target.z = (s8)(rx_buf[i + 43]);
		touchscreen_report_pos(ts->input_dev, &ts->prop, target.x, target.y, true);
		update_done |= axiom_process_u41_report_target(ts, &target);
	}

	return update_done;
}

/*
 * U46 report contains a low level measurement data generated by the capacitive
 * displacement sensor (CDS) algorithms from the auxiliary channels.
 * This information is useful when tuning multi-press to assess mechanical
 * consistency in the unit's construction.
 */
static void axiom_process_u46_report(struct axiom_data *ts, u8 *rx_buf)
{
	struct input_dev *input_dev = ts->input_dev;
	u32 event_value;
	u16 aux_value;
	int i;

	for (i = 0; i < AXIOM_U46_AUX_CHANNELS; i++) {
		u8 *target_step = &rx_buf[i * 2];

		input_mt_slot(input_dev, i);
		input_mt_report_slot_state(input_dev, MT_TOOL_FINGER, true);
		aux_value = get_unaligned_le16(target_step + 1) & AXIOM_U46_AUX_MASK;
		event_value = (i << 16) | (aux_value);
		input_event(input_dev, EV_MSC, MSC_RAW, event_value);
	}
}

/*
 * Validates the crc and demultiplexes the axiom reports to the appropriate
 * report handler
 */
static int axiom_handle_events(struct axiom_data *ts)
{
	struct input_dev *input_dev = ts->input_dev;
	u8 *report_data = ts->rx_buf;
	struct device *dev = ts->dev;
	u16 crc_report;
	u8 *crc_bytes;
	u16 crc_calc;
	int error;
	u8 len;

	error = axiom_read(ts, AXIOM_REPORT_USAGE_ID, 0, report_data, ts->max_report_len);
	if (error)
		return error;

	len = (report_data[0] & AXIOM_COMMS_REPORT_LEN_MASK) << 1;
	if (len <= 2) {
		dev_err(dev, "Zero length report discarded.\n");
		return -ENODATA;
	}

	/* Validate the report CRC */
	crc_bytes = &report_data[len];

	crc_report = get_unaligned_le16(crc_bytes - 2);
	/* Length is in 16 bit words and remove the size of the CRC16 itself */
	crc_calc = crc16(0, report_data, (len - 2));

	if (crc_calc != crc_report) {
		dev_err(dev,
			"CRC mismatch! Expected: %#x, Calculated CRC: %#x.\n",
			crc_report, crc_calc);
		return -EINVAL;
	}

	switch (report_data[1]) {
	case AXIOM_USAGE_2DCTS_REPORT_ID:
		if (axiom_process_u41_report(ts, &report_data[1])) {
			input_mt_sync_frame(input_dev);
			input_sync(input_dev);
		}
		break;

	case AXIOM_USAGE_2AUX_REPORT_ID:
		/* This is an aux report (force) */
		axiom_process_u46_report(ts, &report_data[1]);
		input_mt_sync(input_dev);
		input_sync(input_dev);
		break;

	case AXIOM_USAGE_2HB_REPORT_ID:
		/* This is a heartbeat report */
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void axiom_i2c_poll(struct input_dev *input_dev)
{
	struct axiom_data *ts = input_get_drvdata(input_dev);

	axiom_handle_events(ts);
}

static irqreturn_t axiom_irq(int irq, void *dev_id)
{
	struct axiom_data *ts = dev_id;

	axiom_handle_events(ts);

	return IRQ_HANDLED;
}

static void axiom_reset(struct gpio_desc *reset_gpio)
{
	gpiod_set_value_cansleep(reset_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(reset_gpio, 0);
	msleep(AXIOM_STARTUP_TIME_MS);
}

static int axiom_i2c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct input_dev *input_dev;
	struct axiom_data *ts;
	u32 poll_interval;
	int target;
	int error;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	i2c_set_clientdata(client, ts);
	ts->client = client;
	ts->dev = dev;

	ts->regmap = devm_regmap_init_i2c(client, &axiom_i2c_regmap_config);
	error = PTR_ERR_OR_ZERO(ts->regmap);
	if (error) {
		dev_err(dev, "Failed to initialize regmap: %d\n", error);
		return error;
	}

	ts->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ts->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ts->reset_gpio), "failed to get reset GPIO\n");

	if (ts->reset_gpio)
		axiom_reset(ts->reset_gpio);

	ts->vddi = devm_regulator_get_optional(dev, "vddi");
	if (!IS_ERR(ts->vddi)) {
		error = devm_regulator_get_enable(dev, "vddi");
		if (error)
			return dev_err_probe(&client->dev, error,
					     "Failed to enable vddi regulator\n");
	}

	ts->vdda = devm_regulator_get_optional(dev, "vdda");
	if (!IS_ERR(ts->vdda)) {
		error = devm_regulator_get_enable(dev, "vdda");
		if (error)
			return dev_err_probe(&client->dev, error,
					     "Failed to enable vdda regulator\n");
		msleep(AXIOM_STARTUP_TIME_MS);
	}

	error = axiom_discover(ts);
	if (error)
		return dev_err_probe(dev, error, "Failed touchscreen discover\n");

	input_dev = devm_input_allocate_device(ts->dev);
	if (!input_dev)
		return -ENOMEM;

	input_dev->name = "TouchNetix axiom Touchscreen";
	input_dev->phys = "input/axiom_ts";

	input_set_abs_params(input_dev, ABS_MT_POSITION_X, 0, 65535, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y, 0, 65535, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOOL_TYPE, 0, MT_TOOL_MAX, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_DISTANCE, 0, 127, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_PRESSURE, 0, 127, 0, 0);

	touchscreen_parse_properties(input_dev, true, &ts->prop);

	/* Registers the axiom device as a touchscreen instead of a mouse pointer */
	error = input_mt_init_slots(input_dev, AXIOM_U41_MAX_TARGETS, INPUT_MT_DIRECT);
	if (error)
		return error;

	/* Enables the raw data for up to 4 force channels to be sent to the input subsystem */
	set_bit(EV_REL, input_dev->evbit);
	set_bit(EV_MSC, input_dev->evbit);
	/* Declare that we support "RAW" Miscellaneous events */
	set_bit(MSC_RAW, input_dev->mscbit);

	ts->input_dev = input_dev;
	input_set_drvdata(ts->input_dev, ts);

	/* Ensure that all reports are initialised to not be present. */
	for (target = 0; target < AXIOM_U41_MAX_TARGETS; target++)
		ts->targets[target].state = AXIOM_TARGET_STATE_NOT_PRESENT;

	error = devm_request_threaded_irq(dev, client->irq, NULL,
					  axiom_irq, IRQF_ONESHOT, dev_name(dev), ts);
	if (error) {
		dev_info(dev, "Request irq failed, falling back to polling mode");

		error = input_setup_polling(input_dev, axiom_i2c_poll);
		if (error)
			return dev_err_probe(ts->dev, error, "Unable to set up polling mode\n");

		if (!device_property_read_u32(ts->dev, "poll-interval", &poll_interval))
			input_set_poll_interval(input_dev, poll_interval);
		else
			input_set_poll_interval(input_dev, POLL_INTERVAL_DEFAULT_MS);
	}

	error = input_register_device(input_dev);
	if (error)
		return dev_err_probe(ts->dev, error,
				     "Could not register with Input Sub-system.\n");

	return 0;
}

static const struct i2c_device_id axiom_i2c_id_table[] = {
	{ "ax54a" },
	{ },
};
MODULE_DEVICE_TABLE(i2c, axiom_i2c_id_table);

static const struct of_device_id axiom_i2c_of_match[] = {
	{ .compatible = "touchnetix,ax54a", },
	{ }
};
MODULE_DEVICE_TABLE(of, axiom_i2c_of_match);

static struct i2c_driver axiom_i2c_driver = {
	.driver = {
		   .name = "axiom",
		   .of_match_table = axiom_i2c_of_match,
	},
	.id_table = axiom_i2c_id_table,
	.probe = axiom_i2c_probe,
};
module_i2c_driver(axiom_i2c_driver);

MODULE_AUTHOR("Bart Prescott <bartp@baasheep.co.uk>");
MODULE_AUTHOR("Pedro Torruella <pedro.torruella@touchnetix.com>");
MODULE_AUTHOR("Mark Satterthwaite <mark.satterthwaite@touchnetix.com>");
MODULE_AUTHOR("Hannah Rossiter <hannah.rossiter@touchnetix.com>");
MODULE_AUTHOR("Kamel Bouhara <kamel.bouhara@bootlin.com>");
MODULE_DESCRIPTION("TouchNetix axiom touchscreen I2C bus driver");
MODULE_LICENSE("GPL");
