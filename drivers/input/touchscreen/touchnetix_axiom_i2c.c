// SPDX-License-Identifier: GPL-2.0-only
/*
 * TouchNetix aXiom Touchscreen Driver
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

#include <linux/crc16.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/string.h>

/*
 * Runtime TCP mode: device is executing normal code and is
 * accessible via the Touch Controller Mode
 */
#define BOOT_TCP			0
/*
 * Bootloader BLP mode: device is executing bootloader and is
 * accessible via the Boot Loader Protocol.
 */
#define BOOT_BLP			1
#define AXIOM_PROX_LEVEL		-128
/*
 * Register group u31 has 2 pages for usage table entries.
 * (2 * AXIOM_COMMS_PAGE_SIZE) / AXIOM_U31_BYTES_PER_USAGE = 85
 */
#define AXIOM_U31_MAX_USAGES		85
#define AXIOM_U31_BYTES_PER_USAGE	6
#define AXIOM_U31_PAGE0_LENGTH		0x0C
#define AXIOM_U31_BOOTMODE_MASK		BIT(7)
#define AXIOM_U31_FW_INFO_VARIANT_MASK	GENMASK(6, 0)
#define AXIOM_U31_FW_INFO_STATUS_MASK	BIT(7)

#define AXIOM_U41_MAX_TARGETS		10

#define AXIOM_U46_AUX_CHANNELS		4
#define AXIOM_U46_AUX_MASK		GENMASK(11, 0)

#define AXIOM_COMMS_MAX_USAGE_PAGES	3
#define AXIOM_COMMS_PAGE_SIZE		256
#define AXIOM_COMMS_OVERFLOW_MASK	BIT(7)
#define AXIOM_COMMS_REPORT_LEN_MASK	GENMASK(7, 0)

#define AXIOM_REBASELINE_CMD		0x03

#define AXIOM_REPORT_USAGE_ID		0x34
#define AXIOM_DEVINFO_USAGE_ID		0x31
#define AXIOM_USAGE_2HB_REPORT_ID	0x01
#define AXIOM_REBASELINE_USAGE_ID	0x02
#define AXIOM_USAGE_2AUX_REPORT_ID	0x46
#define AXIOM_USAGE_2DCTS_REPORT_ID	0x41

#define AXIOM_PAGE_MASK			GENMASK(15, 8)
#define AXIOM_PAGE_OFFSET_MASK		GENMASK(7, 0)

struct axiom_devinfo {
	char bootloader_fw_major;
	char bootloader_fw_minor;
	char bootmode;
	u16 device_id;
	char fw_major;
	char fw_minor;
	char fw_info_extra;
	char tcp_revision;
	u16 jedec_id;
	char num_usages;
	char silicon_revision;
};

/*
 * Describes parameters of a specific usage, essenstially a single element of
 * the "Usage Table"
 */
struct usage_entry {
	char id;
	char is_report;
	char start_page;
	char num_pages;
};

/*
 * Holds state of a touch or target when detected prior a touch (eg.
 * hover or proximity events).
 */
enum axiom_target_state {
	TARGET_STATE_NOT_PRESENT = 0,
	TARGET_STATE_PROX = 1,
	TARGET_STATE_HOVER = 2,
	TARGET_STATE_TOUCHING = 3,
	TARGET_STATE_MIN = TARGET_STATE_NOT_PRESENT,
	TARGET_STATE_MAX = TARGET_STATE_TOUCHING,
};

struct u41_target {
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
	u16 target_address;
	u16 length:15;
	u16 read:1;
};

struct axiom_data {
	struct axiom_devinfo devinfo;
	struct device *dev;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *irq_gpio;
	struct i2c_client *client;
	struct input_dev *input_dev;
	u32 max_report_len;
	u32 report_overflow_counter;
	u32 report_counter;
	char rx_buf[AXIOM_COMMS_MAX_USAGE_PAGES * AXIOM_COMMS_PAGE_SIZE];
	struct u41_target targets[AXIOM_U41_MAX_TARGETS];
	struct usage_entry usage_table[AXIOM_U31_MAX_USAGES];
	bool usage_table_populated;
};

/*
 * aXiom devices are typically configured to report
 * touches at a rate of 100Hz (10ms). For systems
 * that require polling for reports, 100ms seems like
 * an acceptable polling rate.
 * When reports are polled, it will be expected to
 * occasionally observe the overflow bit being set
 * in the reports. This indicates that reports are not
 * being read fast enough.
 */
#define POLL_INTERVAL_DEFAULT_MS 100

/**
 * Translate usage/page/offset triplet into physical address.
 *
 * @usage: groups of registers
 * @page: page to which the usage belongs to offset
 * @offset: offset of the usage
 * @return: target address on success, 0xFFFF on error
 */
static u16
usage_to_target_address(struct axiom_data *ts, char usage, char page,
			char offset)
{
	struct axiom_devinfo *device_info;
	struct usage_entry *usage_table;
	u32 i;

	device_info = &ts->devinfo;
	usage_table = ts->usage_table;

	/* At the moment the convention is that u31 is always at physical address 0x0 */
	if (!ts->usage_table_populated) {
		if (usage == AXIOM_DEVINFO_USAGE_ID)
			return ((page << 8) + offset);
		else
			return 0xffff;
	}

	for (i = 0; i < device_info->num_usages; i++) {
		if (usage_table[i].id != usage)
			continue;

		if (page >= usage_table[i].num_pages) {
			dev_err(ts->dev, "Invalid usage table! usage: %u, page: %u, offset: %u\n",
				usage, page, offset);
			return 0xffff;
		}
	}

	return ((usage_table[i].start_page + page) << 8) + offset;
}

static int
axiom_i2c_read(struct i2c_client *client, u8 usage, u8 page, u8 *buf, u16 len)
{
	struct axiom_data *ts = i2c_get_clientdata(client);
	struct axiom_cmd_header cmd_header;
	struct i2c_msg msg[2];
	int ret;

	cmd_header.target_address = cpu_to_le16(usage_to_target_address(ts, usage, page, 0));
	cmd_header.length = cpu_to_le16(len);
	cmd_header.read = cpu_to_le16(1);

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].len = sizeof(cmd_header);
	msg[0].buf = (u8 *)&cmd_header;

	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len = len;
	msg[1].buf = (char *)buf;

	ret = i2c_transfer(client->adapter, msg, 2);
	if (ret != ARRAY_SIZE(msg)) {
		dev_err(&client->dev,
			"Failed reading usage %#x page %#x, error=%d\n",
			usage, page, ret);
		return -EIO;
	}

	return 0;
}

static int
axiom_i2c_write(struct i2c_client *client, u8 usage, u8 page, u8 *buf, u16 len)
{
	struct axiom_data *ts = i2c_get_clientdata(client);
	struct axiom_cmd_header cmd_header;
	struct i2c_msg msg[2];
	int ret;

	cmd_header.target_address = cpu_to_le16(usage_to_target_address(ts, usage, page, 0));
	cmd_header.length = cpu_to_le16(len);
	cmd_header.read = 0;

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].len = sizeof(cmd_header);
	msg[0].buf = (u8 *)&cmd_header;

	msg[1].addr = client->addr;
	msg[1].flags = 0;
	msg[1].len = len;
	msg[1].buf = (char *)buf;

	ret = i2c_transfer(client->adapter, msg, 2);
	if (ret < 0) {
		dev_err(&client->dev,
			"Failed to write usage %#x page %#x, error=%d\n", usage,
			page, ret);
		return ret;
	}

	return 0;
}

/*
 * Decodes and populates the local Usage Table.
 * Given a buffer of data read from page 1 onwards of u31 from an aXiom
 * device.
 */
static u32 axiom_populate_usage_table(struct axiom_data *ts, char *rx_data)
{
	u32 usage_id = 0;
	u32 max_report_len = 0;
	struct axiom_devinfo *device_info;
	struct usage_entry *usage_table;

	device_info = &ts->devinfo;
	usage_table = ts->usage_table;

	for (usage_id = 0; usage_id < device_info->num_usages; usage_id++) {
		u16 offset = (usage_id * AXIOM_U31_BYTES_PER_USAGE);
		char id = rx_data[offset + 0];
		char start_page = rx_data[offset + 1];
		char num_pages = rx_data[offset + 2];
		u32 max_offset = ((rx_data[offset + 3] & AXIOM_PAGE_OFFSET_MASK) + 1) * 2;

		if (!num_pages)
			usage_table[usage_id].is_report = true;

		/* Store the entry into the usage table */
		usage_table[usage_id].id = id;
		usage_table[usage_id].start_page = start_page;
		usage_table[usage_id].num_pages = num_pages;

		dev_dbg(ts->dev, "Usage %2u Info: %*ph\n", usage_id,
			AXIOM_U31_BYTES_PER_USAGE,
			&rx_data[offset]);

		/* Identify the max report length the module will receive */
		if (usage_table[usage_id].is_report && max_offset > max_report_len)
			max_report_len = max_offset;
	}
	ts->usage_table_populated = true;

	return max_report_len;
}

/* Retrieve, store and print the axiom device information */
static int axiom_discover(struct axiom_data *ts)
{
	struct axiom_devinfo *devinfo = &ts->devinfo;
	struct device *dev = ts->dev;
	char *rx_data = ts->rx_buf;
	int ret;

	/*
	 * Fetch the first page of usage u31 to get the
	 * device information and the number of usages
	 */
	ret = axiom_i2c_read(ts->client, AXIOM_DEVINFO_USAGE_ID, 0, rx_data,
			     AXIOM_U31_PAGE0_LENGTH);
	if (ret)
		return ret;

	devinfo->bootmode = (rx_data[0] & AXIOM_U31_BOOTMODE_MASK);
	devinfo->device_id = ((rx_data[1] & AXIOM_PAGE_OFFSET_MASK) << 8) | rx_data[0];
	devinfo->fw_minor = rx_data[2];
	devinfo->fw_major = rx_data[3];
	devinfo->fw_info_extra = rx_data[4];
	devinfo->bootloader_fw_minor = rx_data[6];
	devinfo->bootloader_fw_major = rx_data[7];
	devinfo->jedec_id = (rx_data[8]) | (rx_data[9] << 8);
	devinfo->num_usages = rx_data[10];
	devinfo->silicon_revision = rx_data[11];

	dev_dbg(dev, "  Boot Mode: %s\n", ts->devinfo.bootmode ? "BLP" : "TCP");
	dev_dbg(dev, "  Device ID      : %04x\n", ts->devinfo.device_id);
	dev_dbg(dev, "  Firmware Rev   : %02x.%02x\n", ts->devinfo.fw_major,
		ts->devinfo.fw_minor);
	dev_dbg(dev, "  Bootloader Rev : %02x.%02x\n",
		ts->devinfo.bootloader_fw_major,
		ts->devinfo.bootloader_fw_minor);
	dev_dbg(dev, "  FW Extra Info  : %04x\n", ts->devinfo.fw_info_extra);
	dev_dbg(dev, "  Silicon        : %02x\n", ts->devinfo.jedec_id);
	dev_dbg(dev, "  Num Usages     : %04x\n", ts->devinfo.num_usages);

	/* Read the second page of usage u31 to get the usage table */
	ret = axiom_i2c_read(ts->client, AXIOM_DEVINFO_USAGE_ID, 1, rx_data,
			     (AXIOM_U31_BYTES_PER_USAGE * ts->devinfo.num_usages));
	if (ret)
		return ret;

	ts->max_report_len = axiom_populate_usage_table(ts, rx_data);
	dev_dbg(dev, "Max Report Length: %u\n", ts->max_report_len);

	return 0;
}

/*
 * Support function to axiom_process_u41_report.
 * Generates input-subsystem events for every target.
 * After calling this function the caller shall issue
 * a Sync to the input sub-system.
 */
static bool
axiom_process_u41_report_target(struct axiom_data *ts,
				struct axiom_target_report *target)
{
	struct input_dev *input_dev = ts->input_dev;
	enum axiom_target_state current_state;
	struct u41_target *target_prev_state;
	struct device *dev = ts->dev;
	bool update = false;
	int slot;

	/* Verify the target index */
	if (target->index >= AXIOM_U41_MAX_TARGETS) {
		dev_dbg(dev, "Invalid target index! %u\n", target->index);
		return false;
	}

	target_prev_state = &ts->targets[target->index];

	current_state = TARGET_STATE_NOT_PRESENT;

	if (target->present) {
		if (target->z >= 0)
			current_state = TARGET_STATE_TOUCHING;
		else if (target->z > AXIOM_PROX_LEVEL && target->z < 0)
			current_state = TARGET_STATE_HOVER;
		else if (target->z AXIOM_PROX_LEVEL)
			current_state = TARGET_STATE_PROX;
	}

	if (target_prev_state->state == current_state &&
	    target_prev_state->x == target->x &&
	    target_prev_state->y == target->y &&
	    target_prev_state->z == target->z) {
		return false;
	}

	slot = target->index;

	dev_dbg(dev, "U41 Target T%u, slot:%u present:%u, x:%u, y:%u, z:%d\n",
		target->index, slot, target->present,
		target->x, target->y, target->z);

	switch (current_state) {
	case TARGET_STATE_NOT_PRESENT:
	case TARGET_STATE_PROX:
		if (target_prev_state->insert)
			break;
		update = true;
		target_prev_state->insert = false;
		input_mt_slot(input_dev, slot);

		if (!slot)
			input_report_key(input_dev, BTN_LEFT, 0);

		input_mt_report_slot_inactive(input_dev);
		/*
		 * make sure the previous coordinates are
		 * all off screen when the finger comes back
		 */
		target->x = 65535;
		target->y = 65535;
		target->z = AXIOM_PROX_LEVEL;
		break;
	case TARGET_STATE_HOVER:
	case TARGET_STATE_TOUCHING:
		target_prev_state->insert = true;
		update = true;
		input_mt_slot(input_dev, slot);
		input_report_abs(input_dev, ABS_MT_TRACKING_ID, slot);
		input_report_abs(input_dev, ABS_MT_POSITION_X, target->x);
		input_report_abs(input_dev, ABS_X, target->x);
		input_report_abs(input_dev, ABS_MT_POSITION_Y, target->y);
		input_report_abs(input_dev, ABS_Y, target->y);

		if (current_state == TARGET_STATE_TOUCHING) {
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

		if (!slot)
			input_report_key(input_dev, BTN_LEFT, (current_state ==
					 TARGET_STATE_TOUCHING));
		break;
	}

	target_prev_state->state = current_state;
	target_prev_state->x = target->x;
	target_prev_state->y = target->y;
	target_prev_state->z = target->z;

	if (update)
		input_mt_sync_frame(input_dev);

	return update;
}

/*
 * Take a raw buffer with u41 report data and decode it.
 * Also generate input events if needed.
 * rx_buf: ptr to a byte array [0]: Usage number [1]: Status LSB [2]: Status MSB
 */
static void axiom_process_u41_report(struct axiom_data *ts, char *rx_buf)
{
	struct input_dev *input_dev = ts->input_dev;
	struct axiom_target_report target;
	bool update_done = false;
	u16 target_status;
	u32 i;

	target_status = ((rx_buf[1]) | (rx_buf[2] << 8));

	for (i = 0; i < AXIOM_U41_MAX_TARGETS; i++) {
		char target_step = rx_buf[(i * 4)];

		target.index = i;
		target.present = ((target_status & (1 << i)) != 0) ? 1 : 0;
		target.x = ((target_step + 3) | ((target_step + 4) << 8));
		target.y = ((target_step + 5) | ((target_step + 6) << 8));
		target.z = (s8)(rx_buf[i + 43]);
		update_done |= axiom_process_u41_report_target(ts, &target);
	}

	if (update_done)
		input_sync(input_dev);
}

static void axiom_process_u46_report(struct axiom_data *ts, char *rx_buf)
{
	struct input_dev *input_dev = ts->input_dev;
	u32 event_value;
	u16 aux_value;
	u32 i = 0;

	for (i = 0; i < AXIOM_U46_AUX_CHANNELS; i++) {
		char target_step = rx_buf[(i * 2)];

		aux_value = (((target_step + 2) << 8) | (target_step + 1)) & AXIOM_U46_AUX_MASK;
		event_value = (i << 16) | (aux_value);
		input_event(input_dev, EV_MSC, MSC_RAW, event_value);
	}

	input_mt_sync(input_dev);
	input_sync(input_dev);
}

/*
 * Validates the crc and demultiplexes the axiom reports to the appropriate
 * report handler
 */
void axiom_handle_events(struct axiom_data *ts)
{
	char *report_data = ts->rx_buf;
	struct device *dev = ts->dev;
	char usage = report_data[1];
	u16 crc_report;
	u16 crc_calc;
	char len;

	axiom_i2c_read(ts->client, AXIOM_REPORT_USAGE_ID, 0, report_data, ts->max_report_len);

	if ((report_data[0] & AXIOM_COMMS_OVERFLOW_MASK) != 0)
		ts->report_overflow_counter++;

	len = (report_data[0] & AXIOM_COMMS_REPORT_LEN_MASK) * 2;
	if (!len) {
		dev_err(dev, "Zero length report discarded.\n");
		return;
	}

	dev_dbg(dev, "Payload Data %*ph\n", len, report_data);

	/* Validate the report CRC */
	crc_report = (report_data[len - 1] << 8) | (report_data[len - 2]);
	/* Length is in 16 bit words and remove the size of the CRC16 itself */
	crc_calc = crc16(0, report_data, (len - 2));

	if (crc_calc != crc_report) {
		dev_err(dev,
			"CRC mismatch! Expected: %#x, Calculated CRC: %#x.\n",
			crc_report, crc_calc);
		return;
	}

	switch (usage) {
	case AXIOM_USAGE_2DCTS_REPORT_ID:
		axiom_process_u41_report(ts, &report_data[1]);
		break;

	case AXIOM_USAGE_2AUX_REPORT_ID:
		/* This is an aux report (force) */
		axiom_process_u46_report(ts, &report_data[1]);
		break;

	case AXIOM_USAGE_2HB_REPORT_ID:
		/* This is a heartbeat report */
		break;
	}

	ts->report_counter++;
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
	msleep(100);
}

/* Rebaseline the touchscreen, effectively zero-ing it */
static int axiom_rebaseline(struct axiom_data *ts)
{
	char buffer[8] = {};

	buffer[0] = AXIOM_REBASELINE_CMD;

	return axiom_i2c_write(ts->client, AXIOM_REPORT_USAGE_ID, 0, buffer, sizeof(buffer));
}

static int axiom_i2c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct input_dev *input_dev;
	struct axiom_data *ts;
	int ret;
	int target;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->client = client;
	i2c_set_clientdata(client, ts);
	ts->dev = dev;

	ts->irq_gpio = devm_gpiod_get_optional(dev, "irq", GPIOD_IN);
	if (IS_ERR(ts->irq_gpio))
		return dev_err_probe(dev, PTR_ERR(ts->irq_gpio), "failed to get irq GPIO");

	ts->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ts->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ts->reset_gpio), "failed to get reset GPIO\n");

	axiom_reset(ts->reset_gpio);

	if (ts->irq_gpio) {
		ret = devm_request_threaded_irq(dev, client->irq, NULL,
						axiom_irq, 0, dev_name(dev), ts);
		if (ret < 0)
			return dev_err_probe(dev, ret, "Failed to request threaded IRQ\n");
	}

	ret = axiom_discover(ts);
	if (ret)
		return dev_err_probe(dev, ret, "Failed touchscreen discover\n");

	ret = axiom_rebaseline(ts);
	if (ret)
		return dev_err_probe(dev, ret, "Failed touchscreen re-baselining\n");

	input_dev = devm_input_allocate_device(ts->dev);
	if (!input_dev)
		return -ENOMEM;

	input_dev->name = "TouchNetix aXiom Touchscreen";
	input_dev->phys = "input/axiom_ts";

	/* Single Touch */
	input_set_abs_params(input_dev, ABS_X, 0, 65535, 0, 0);
	input_set_abs_params(input_dev, ABS_Y, 0, 65535, 0, 0);

	/* Multi Touch */
	/* Min, Max, Fuzz (expected noise in px, try 4?) and Flat */
	input_set_abs_params(input_dev, ABS_MT_POSITION_X, 0, 65535, 0, 0);
	/* Min, Max, Fuzz (expected noise in px, try 4?) and Flat */
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y, 0, 65535, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOOL_TYPE, 0, MT_TOOL_MAX, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_DISTANCE, 0, 127, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_PRESSURE, 0, 127, 0, 0);

	/* Registers the axiom device as a touchscreen instead of as a mouse pointer */
	input_mt_init_slots(input_dev, AXIOM_U41_MAX_TARGETS, INPUT_MT_DIRECT);

	input_set_capability(input_dev, EV_KEY, BTN_LEFT);

	/* Enables the raw data for up to 4 force channels to be sent to the input subsystem */
	set_bit(EV_REL, input_dev->evbit);
	set_bit(EV_MSC, input_dev->evbit);
	/* Declare that we support "RAW" Miscellaneous events */
	set_bit(MSC_RAW, input_dev->mscbit);

	if (!ts->irq_gpio) {
		ret = input_setup_polling(input_dev, axiom_i2c_poll);
		if (ret)
			return	dev_err_probe(ts->dev, ret, "Unable to set up polling mode\n");
		input_set_poll_interval(input_dev, POLL_INTERVAL_DEFAULT_MS);
	}

	ts->input_dev = input_dev;
	input_set_drvdata(ts->input_dev, ts);

	/* Ensure that all reports are initialised to not be present. */
	for (target = 0; target < AXIOM_U41_MAX_TARGETS; target++)
		ts->targets[target].state = TARGET_STATE_NOT_PRESENT;

	ret = input_register_device(input_dev);

	if (ret)
		return dev_err_probe(ts->dev, ret,
					"Could not register with Input Sub-system.\n");

	return 0;
}

static void axiom_i2c_remove(struct i2c_client *client)
{
	struct axiom_data *ts = i2c_get_clientdata(client);

	input_unregister_device(ts->input_dev);
}

static const struct i2c_device_id axiom_i2c_id_table[] = {
	{ "axiom-ax54a" },
	{},
};

MODULE_DEVICE_TABLE(i2c, axiom_i2c_id_table);

static const struct of_device_id axiom_i2c_of_match[] = {
	{ .compatible = "touchnetix,axiom-ax54a", },
	{}
};

MODULE_DEVICE_TABLE(of, axiom_i2c_of_match);

static struct i2c_driver axiom_i2c_driver = {
	.driver = {
		   .name = "axiom",
		   .of_match_table = axiom_i2c_of_match,
	},
	.id_table = axiom_i2c_id_table,
	.probe = axiom_i2c_probe,
	.remove = axiom_i2c_remove,
};

module_i2c_driver(axiom_i2c_driver);

MODULE_AUTHOR("Bart Prescott <bartp@baasheep.co.uk>");
MODULE_AUTHOR("Pedro Torruella <pedro.torruella@touchnetix.com>");
MODULE_AUTHOR("Mark Satterthwaite <mark.satterthwaite@touchnetix.com>");
MODULE_AUTHOR("Hannah Rossiter <hannah.rossiter@touchnetix.com>");
MODULE_AUTHOR("Kamel Bouhara <kamel.bouhara@bootlin.com>");
MODULE_DESCRIPTION("TouchNetix aXiom touchscreen I2C bus driver");
MODULE_LICENSE("GPL");
