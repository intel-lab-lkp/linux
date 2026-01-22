// SPDX-License-Identifier: GPL-2.0
/*
 * TouchNetix aXiom Touchscreen Driver
 *
 * Copyright (C) 2020-2026 TouchNetix Ltd.
 *
 * Author(s): Mark Satterthwaite <mark.satterthwaite@touchnetix.com>
 *            Pedro Torruella <pedro.torruella@touchnetix.com>
 *            Bart Prescott <bartp@baasheep.co.uk>
 *            Hannah Rossiter <hannah.rossiter@touchnetix.com>
 *            Andrew Thomas <andrew.thomas@touchnetix.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */

// #define DEBUG // Enable debug messages

#include <linux/device.h>
#include <linux/input/mt.h>
#include <linux/crc16.h>
#include <linux/property.h>
#include <linux/interrupt.h>
#include <linux/unaligned.h>
#include "axiom_core.h"

/* u31 device info masks */
#define AX_DEV_ID_MASK GENMASK(14, 0)
#define AX_MODE BIT(15)
#define AX_FW_REV_MINOR_MASK GENMASK(7, 0)
#define AX_FW_REV_MAJOR_MASK GENMASK(15, 8)
#define AX_VARIANT_MASK GENMASK(5, 0)
#define AX_FW_STATUS BIT(7)
#define AX_TCP_REV_MASK GENMASK(15, 8)
#define AX_BOOT_REV_MINOR_MASK GENMASK(7, 0)
#define AX_BOOT_REV_MAJOR_MASK GENMASK(15, 8)
#define AX_NUM_USAGES_MASK GENMASK(7, 0)
#define AX_SILICON_REV_MASK GENMASK(11, 8)
#define AX_RUNTIME_FW_PATCH_MASK GENMASK(15, 12)

/* u31 usage table entry masks */
#define AX_U31_USAGE_NUM_MASK GENMASK(7, 0)
#define AX_U31_START_PAGE_MASK GENMASK(15, 8)
#define AX_U31_NUM_PAGES_MASK GENMASK(7, 0)
#define AX_U31_MAX_OFFSET_MASK GENMASK(14, 8)
#define AX_U31_OFFSET_TYPE_BIT BIT(15)
#define AX_U31_UIF_REV_MASK GENMASK(7, 0)
#define AX_U31_USAGE_TYPE_MASK GENMASK(15, 8)

/* u34 report masks */
#define AX_U34_LEN_MASK GENMASK(6, 0)
#define AX_U34_OVERFLOW BIT(7)
#define AX_U34_USAGE_MASK GENMASK(15, 8)
#define AX_U34_PAYLOAD_BUFFER 2

/* u41 report masks */
#define AX_U41_PRESENT_MASK GENMASK(9, 0)
#define U41_X_Y_OFFSET (2)
#define U41_COORD_SIZE (4)
#define U41_Z_OFFSET (42)

static const char *const fw_variants[] = { "3D", "2D", "FORCE",
					   "0D", "XL", "TOUCHPAD" };

static int axiom_set_capabilities(struct input_dev *input_dev)
{
	input_dev->name = "TouchNetix aXiom Touchscreen";
	input_dev->phys = "input/axiom_ts";

	// Single Touch
	input_set_abs_params(input_dev, ABS_X, 0, 65535, 0, 0);
	input_set_abs_params(input_dev, ABS_Y, 0, 65535, 0, 0);

	// Multi Touch
	// Min, Max, Fuzz (expected noise in px, try 4?) and Flat
	input_set_abs_params(input_dev, ABS_MT_POSITION_X, 0, 65535, 0, 0);
	// Min, Max, Fuzz (expected noise in px, try 4?) and Flat
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y, 0, 65535, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOOL_TYPE, 0, MT_TOOL_MAX, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_DISTANCE, 0, 127, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_PRESSURE, 0, 127, 0, 0);

	input_mt_init_slots(input_dev, U41_MAX_TARGETS, INPUT_MT_DIRECT);

	return 0;
}

static struct u31_usage_entry *usage_find_entry(struct axiom *ax, u16 usage)
{
	u16 i;

	for (i = 0; i < ax->dev_info.num_usages; i++) {
		if (ax->usage_table[i].usage_num == usage)
			return &ax->usage_table[i];
	}

	pr_err("aXiom-core: Usage u%02x not found in usage table\n", usage);
	return ERR_PTR(-EINVAL);
}

static void axiom_unpack_device_info(const u8 *buf,
				     struct axiom_device_info *info)
{
	u16 w;

	w = get_unaligned_le16(buf);
	info->device_id = FIELD_GET(AX_DEV_ID_MASK, w);
	info->mode = !!(w & AX_MODE);

	w = get_unaligned_le16(buf + 2);
	info->runtime_fw_rev_minor = FIELD_GET(AX_FW_REV_MINOR_MASK, w);
	info->runtime_fw_rev_major = FIELD_GET(AX_FW_REV_MAJOR_MASK, w);

	w = get_unaligned_le16(buf + 4);
	info->device_build_variant = FIELD_GET(AX_VARIANT_MASK, w);
	info->runtime_fw_status = !!(w & AX_FW_STATUS);
	info->tcp_revision = FIELD_GET(AX_TCP_REV_MASK, w);

	w = get_unaligned_le16(buf + 6);
	info->bootloader_fw_rev_minor = FIELD_GET(AX_BOOT_REV_MINOR_MASK, w);
	info->bootloader_fw_rev_major = FIELD_GET(AX_BOOT_REV_MAJOR_MASK, w);

	info->jedec_id = get_unaligned_le16(buf + 8);

	w = get_unaligned_le16(buf + 10);
	info->num_usages = FIELD_GET(AX_NUM_USAGES_MASK, w);
	info->silicon_revision = FIELD_GET(AX_SILICON_REV_MASK, w);
	info->runtime_fw_rev_patch = FIELD_GET(AX_RUNTIME_FW_PATCH_MASK, w);
}

static void axiom_unpack_usage_table(u8 *buf, struct axiom *ax)
{
	u8 *ptr;
	struct u31_usage_entry *entry;
	int i;
	u16 w;
	u16 report_len;

	for (i = 0; i < ax->dev_info.num_usages && i < U31_MAX_USAGES; i++) {
		entry = &ax->usage_table[i];
		/* Calculate offset for this specific entry */
		ptr = buf + (i * SIZE_U31_USAGE_ENTRY);

		w = get_unaligned_le16(ptr);
		entry->usage_num = FIELD_GET(AX_U31_USAGE_NUM_MASK, w);
		entry->start_page = FIELD_GET(AX_U31_START_PAGE_MASK, w);

		w = get_unaligned_le16(ptr + 2);
		entry->num_pages = FIELD_GET(AX_U31_NUM_PAGES_MASK, w);
		entry->max_offset = FIELD_GET(AX_U31_MAX_OFFSET_MASK, w);
		entry->offset_type = !!(w & AX_U31_OFFSET_TYPE_BIT);

		w = get_unaligned_le16(ptr + 4);
		entry->uifrevision = FIELD_GET(AX_U31_UIF_REV_MASK, w);
		entry->usage_type = FIELD_GET(AX_U31_USAGE_TYPE_MASK, w);

		// Convert words to bytes
		report_len = (entry->max_offset + 1) * 2;
		if ((entry->usage_type == REPORT) &&
		    (report_len > ax->max_report_len)) {
			ax->max_report_len = report_len;
		}
	}
}

static int axiom_init_dev_info(struct axiom *ax)
{
	int i;
	struct u31_usage_entry *u;
	int err;
	const char *variant_str;

	/* Read page 0 of u31 */
	err = ax->bus_ops->read(ax->dev, 0x0, SIZE_U31_DEVICE_INFO,
				ax->read_buf);
	if (err)
		return -EIO;

	axiom_unpack_device_info(ax->read_buf, &ax->dev_info);

	if (ax->dev_info.device_build_variant < ARRAY_SIZE(fw_variants)) {
		variant_str = fw_variants[ax->dev_info.device_build_variant];
	} else {
		variant_str = "UNKNOWN";
	}
	char silicon_rev = (char)(0x41 + ax->dev_info.silicon_revision);

	dev_info(ax->dev, "Firmware Info:\n");
	dev_info(ax->dev, "  BL Mode     : %u\n", ax->dev_info.mode);
	dev_info(ax->dev, "  Device ID   : %04x\n", ax->dev_info.device_id);
	dev_info(ax->dev, "  FW Revision : %u.%u.%u-%s %s\n",
		 ax->dev_info.runtime_fw_rev_major,
		 ax->dev_info.runtime_fw_rev_minor,
		 ax->dev_info.runtime_fw_rev_patch,
		 (ax->dev_info.runtime_fw_status == 0) ? "eng" : "prod",
		 variant_str);
	dev_info(ax->dev, "  BL Revision : %02x.%02x\n",
		 ax->dev_info.bootloader_fw_rev_major,
		 ax->dev_info.bootloader_fw_rev_minor);
	dev_info(ax->dev, "  Silicon     : 0x%04X (Rev %c)\n",
		 ax->dev_info.jedec_id, silicon_rev);
	dev_info(ax->dev, "  Num Usages  : %u\n", ax->dev_info.num_usages);

	if (ax->dev_info.num_usages > U31_MAX_USAGES) {
		dev_err(ax->dev,
			"Num usages (%u) exceeds maximum supported (%u)\n",
			ax->dev_info.num_usages, U31_MAX_USAGES);
		return -EINVAL;
	}

	/* Read the second page of u31 to get the usage table */
	err = ax->bus_ops->read(ax->dev, 0x100,
				sizeof(ax->usage_table[0]) *
					ax->dev_info.num_usages,
				ax->read_buf);
	if (err)
		return -EIO;

	axiom_unpack_usage_table(ax->read_buf, ax);

	dev_info(ax->dev, "Usage Table:\n");
	for (i = 0; i < ax->dev_info.num_usages; i++) {
		u = &ax->usage_table[i];

		dev_info(
			ax->dev,
			"  Usage: u%02x  Rev: %3u  Page: 0x%02x00  Num Pages: %3u\n",
			u->usage_num, u->uifrevision, u->start_page,
			u->num_pages);
	}
	dev_info(ax->dev, "Max Report Length: %u\n", ax->max_report_len);

	if (ax->max_report_len > AXIOM_MAX_READ_SIZE) {
		dev_err(ax->dev,
			"aXiom maximum report length (%u) greater than allocated buffer size (%u).",
			ax->max_report_len, AXIOM_MAX_READ_SIZE);
		return -EINVAL;
	}

	/* Set u34 address to allow direct access to report reading address */
	u = usage_find_entry(ax, 0x34);
	if (IS_ERR(u))
		return PTR_ERR(u);
	ax->u34_address = u->start_page << 8;

	return 0;
}

static int axiom_process_u41_report(struct axiom *ax, u8 *report)
{
	int i;
	u16 target_present;
	bool active;
	u8 offset;
	enum u41_target_state_e state;
	u16 x;
	u16 y;
	s8 z;

	target_present =
		FIELD_GET(AX_U41_PRESENT_MASK, get_unaligned_le16(&report[0]));

	for (i = 0; i < U41_MAX_TARGETS; i++) {
		active = !!((target_present >> i) & 1);

		offset = U41_X_Y_OFFSET + (i * U41_COORD_SIZE);
		x = get_unaligned_le16(&report[offset]);
		y = get_unaligned_le16(&report[offset + 2]);
		z = report[U41_Z_OFFSET + i];

		if (!active)
			state = Target_State_Not_Present;
		else if (z >= 0)
			state = Target_State_Touching;
		else if ((z > U41_PROX_LEVEL) && (z < 0))
			state = Target_State_Hover;
		else if (z == U41_PROX_LEVEL)
			state = Target_State_Prox;
		else
			state = Target_State_Not_Present;

		dev_dbg(ax->dev, "Target %d: x=%u y=%u z=%d present=%d\n", i, x,
			y, z, active);

		switch (state) {
		case Target_State_Not_Present:
		case Target_State_Prox:

			input_mt_slot(ax->input, i);
			input_mt_report_slot_inactive(ax->input);
			break;

		case Target_State_Hover:
		case Target_State_Touching:

			input_mt_slot(ax->input, i);
			input_report_abs(ax->input, ABS_MT_TRACKING_ID, i);
			input_report_abs(ax->input, ABS_MT_POSITION_X, x);
			input_report_abs(ax->input, ABS_MT_POSITION_Y, y);

			if (state == Target_State_Touching) {
				input_report_abs(ax->input, ABS_MT_DISTANCE, 0);
				input_report_abs(ax->input, ABS_MT_PRESSURE, z);
			} else { /* Hover */
				input_report_abs(ax->input, ABS_MT_DISTANCE, -z);
				input_report_abs(ax->input, ABS_MT_PRESSURE, 0);
			}
			break;

		default:
			break;
		}
	}

	input_mt_sync_frame(ax->input);
	input_sync(ax->input);

	return 0;
}

static int axiom_process_report(struct axiom *ax, u8 *report)
{
	int err;
	struct u34_report_header hdr;
	u16 crc_calc;
	u16 crc_report;
	u8 len;
	u16 hdr_buf = get_unaligned_le16(&report[0]);

	dev_dbg(ax->dev, "Payload Data %*ph\n", ax->max_report_len, report);

	hdr.report_length = FIELD_GET(AX_U34_LEN_MASK, hdr_buf);
	hdr.overflow = !!(hdr_buf & AX_U34_OVERFLOW);
	hdr.report_usage = FIELD_GET(AX_U34_USAGE_MASK, hdr_buf);

	len = hdr.report_length << 1;
	if (hdr.report_length == 0) {
		dev_err(ax->dev, "Zero length report discarded.\n");
		return -EIO;
	}

	// Length is 16 bit words and remove the size of the CRC16 itself
	crc_report = (report[len - 1] << 8) | (report[len - 2]);
	crc_calc = crc16(0, report, (len - 2));

	if (crc_calc != crc_report) {
		dev_err(ax->dev,
			"CRC mismatch! Expected: %04X, Calculated CRC: %04X. Report discarded.\n",
			crc_report, crc_calc);
		return -EIO;
	}

	switch (hdr.report_usage) {
	case AX_2DCTS_REPORT_ID:
		err = axiom_process_u41_report(ax,
					       &report[AX_U34_PAYLOAD_BUFFER]);
		break;

	default:
		break;
	}

	return err;
}

static void axiom_poll(struct input_dev *input_dev)
{
	struct axiom *ax = input_get_drvdata(input_dev);
	int err;

	/* Read touch reports from u34 */
	err = ax->bus_ops->read(ax->dev, ax->u34_address, ax->max_report_len,
				ax->read_buf);
	if (err)
		return;

	err = axiom_process_report(ax, ax->read_buf);
	if (err)
		dev_err(ax->dev, "Failed to process report: %d\n", err);
}

static irqreturn_t axiom_irq(int irq, void *handle)
{
	struct axiom *ax = handle;
	int err;

	/* Read touch reports from u34 */
	err = ax->bus_ops->read(ax->dev, ax->u34_address, ax->max_report_len,
				ax->read_buf);
	if (err)
		goto out;

	err = axiom_process_report(ax, ax->read_buf);
	if (err)
		dev_err(ax->dev, "Failed to process report: %d\n", err);

out:
	return IRQ_HANDLED;
}

struct axiom *axiom_probe(const struct axiom_bus_ops *bus_ops,
			  struct device *dev, int irq)
{
	struct axiom *ax;
	struct input_dev *input_dev;
	int err;
	bool poll_enable = false;
	u8 poll_period = 0;

	ax = devm_kzalloc(dev, sizeof(*ax), GFP_KERNEL);
	if (!ax)
		return ERR_PTR(-ENOMEM);

	input_dev = devm_input_allocate_device(dev);
	if (!input_dev) {
		pr_err("ERROR: aXiom-core: Failed to allocate memory for input device!\n");
		return ERR_PTR(-ENOMEM);
	}

	poll_enable = device_property_read_bool(dev, "axiom,poll-enable");

	device_property_read_u8(dev, "axiom,poll-period", &poll_period);
	if (!poll_period)
		poll_period = AX_POLLING_PERIOD_MS;

	ax->dev = dev;
	ax->input = input_dev;
	ax->bus_ops = bus_ops;
	ax->irq = irq;

	dev_info(dev, "aXiom Probe\n");
	if (poll_enable)
		dev_info(dev, "Polling Period : %u\n", poll_period);
	else
		dev_info(dev, "Device IRQ : %u\n", ax->irq);

	axiom_set_capabilities(input_dev);

	err = axiom_init_dev_info(ax);
	if (err) {
		dev_err(ax->dev, "Failed to read device info, err: %d\n", err);
		return ERR_PTR(err);
	}

	if (poll_enable) {
		err = input_setup_polling(input_dev, axiom_poll);
		if (err) {
			dev_err(ax->dev, "could not set up polling mode, %d\n",
				err);
			return ERR_PTR(err);
		}

		input_set_poll_interval(input_dev, poll_period);
	} else {
		err = devm_request_threaded_irq(ax->dev, ax->irq, NULL,
						axiom_irq,
						IRQF_TRIGGER_LOW | IRQF_ONESHOT,
						"axiom_irq", ax);
		if (err)
			return ERR_PTR(err);
	}

	err = input_register_device(input_dev);
	if (err) {
		dev_err(ax->dev, "Failed to register input device: %d\n", err);
		return ERR_PTR(err);
	}

	input_set_drvdata(input_dev, ax);

	return ax;
}
EXPORT_SYMBOL_GPL(axiom_probe);

MODULE_AUTHOR("TouchNetix <support@touchnetix.com>");
MODULE_DESCRIPTION("aXiom touchscreen core logic");
MODULE_LICENSE("GPL");
MODULE_ALIAS("axiom");
MODULE_VERSION("1.0.0");
