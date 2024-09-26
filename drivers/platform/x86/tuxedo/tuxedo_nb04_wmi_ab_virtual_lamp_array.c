// SPDX-License-Identifier: GPL-2.0
/*
 * This code gives the built in RGB lighting of the TUXEDO NB04 devices a
 * standardised interface, namely HID LampArray.
 *
 * Copyright (C) 2024 Werner Sembach wse@tuxedocomputers.com
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "tuxedo_nb04_wmi_util.h"

#include "tuxedo_nb04_wmi_ab_virtual_lamp_array.h"

#define dev_to_wdev(__dev)	container_of(__dev, struct wmi_device, dev)

enum report_ids {
	LAMP_ARRAY_ATTRIBUTES_REPORT_ID		= 0x01,
	LAMP_ATTRIBUTES_REQUEST_REPORT_ID	= 0x02,
	LAMP_ATTRIBUTES_RESPONSE_REPORT_ID	= 0x03,
	LAMP_MULTI_UPDATE_REPORT_ID		= 0x04,
	LAMP_RANGE_UPDATE_REPORT_ID		= 0x05,
	LAMP_ARRAY_CONTROL_REPORT_ID		= 0x06,
};

static const uint8_t sirius_16_ansii_kbl_mapping[] = {
	0x29, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42,
	0x43, 0x44, 0x45, 0xf1, 0x46, 0x4c,   0x4a, 0x4d, 0x4b, 0x4e,
	0x35, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
	0x27, 0x2d, 0x2e, 0x2a,               0x53, 0x55, 0x54, 0x56,
	0x2b, 0x14, 0x1a, 0x08, 0x15, 0x17, 0x1c, 0x18, 0x0c, 0x12,
	0x13, 0x2f, 0x30, 0x31,               0x5f, 0x60, 0x61,
	0x39, 0x04, 0x16, 0x07, 0x09, 0x0a, 0x0b, 0x0d, 0x0e, 0x0f,
	0x33, 0x34, 0x28,                     0x5c, 0x5d, 0x5e, 0x57,
	0xe1, 0x1d, 0x1b, 0x06, 0x19, 0x05, 0x11, 0x10, 0x36, 0x37,
	0x38, 0xe5, 0x52,                     0x59, 0x5a, 0x5b,
	0xe0, 0xfe, 0xe3, 0xe2, 0x2c, 0xe6, 0x65, 0xe4, 0x50, 0x51,
	0x4f,                                 0x62, 0x63, 0x58
};

static const uint32_t sirius_16_ansii_kbl_mapping_pos_x[] = {
	 25000,  41700,  58400,  75100,  91800, 108500, 125200, 141900, 158600, 175300,
	192000, 208700, 225400, 242100, 258800, 275500,   294500, 311200, 327900, 344600,
	 24500,  42500,  61000,  79500,  98000, 116500, 135000, 153500, 172000, 190500,
	209000, 227500, 246000, 269500,                   294500, 311200, 327900, 344600,
	 31000,  51500,  70000,  88500, 107000, 125500, 144000, 162500, 181000, 199500,
	218000, 236500, 255000, 273500,                   294500, 311200, 327900,
	 33000,  57000,  75500,  94000, 112500, 131000, 149500, 168000, 186500, 205000,
	223500, 242000, 267500,                           294500, 311200, 327900, 344600,
	 37000,  66000,  84500, 103000, 121500, 140000, 158500, 177000, 195500, 214000,
	232500, 251500, 273500,                           294500, 311200, 327900,
	 28000,  47500,  66000,  84500, 140000, 195500, 214000, 234000, 255000, 273500,
	292000,                                           311200, 327900, 344600
};

static const uint32_t sirius_16_ansii_kbl_mapping_pos_y[] = {
	 53000,  53000,  53000,  53000,  53000,  53000,  53000,  53000,  53000,  53000,
	 53000,  53000,  53000,  53000,  53000,  53000,    53000,  53000,  53000,  53000,
	 67500,  67500,  67500,  67500,  67500,  67500,  67500,  67500,  67500,  67500,
	 67500,  67500,  67500,  67500,                    67500,  67500,  67500,  67500,
	 85500,  85500,  85500,  85500,  85500,  85500,  85500,  85500,  85500,  85500,
	 85500,  85500,  85500,  85500,                    85500,  85500,  85500,
	103500, 103500, 103500, 103500, 103500, 103500, 103500, 103500, 103500, 103500,
	103500, 103500, 103500,                           103500, 103500, 103500,  94500,
	121500, 121500, 121500, 121500, 121500, 121500, 121500, 121500, 121500, 121500,
	121500, 121500, 129000,                           121500, 121500, 121500,
	139500, 139500, 139500, 139500, 139500, 139500, 139500, 139500, 147000, 147000,
	147000,                                           139500, 139500, 130500
};

static const uint32_t sirius_16_ansii_kbl_mapping_pos_z[] = {
	  5000,   5000,   5000,   5000,   5000,   5000,   5000,   5000,   5000,   5000,
	  5000,   5000,   5000,   5000,   5000,   5000,     5000,   5000,   5000,   5000,
	  5250,   5250,   5250,   5250,   5250,   5250,   5250,   5250,   5250,   5250,
	  5250,   5250,   5250,   5250,                     5250,   5250,   5250,   5250,
	  5500,   5500,   5500,   5500,   5500,   5500,   5500,   5500,   5500,   5500,
	  5500,   5500,   5500,   5500,                     5500,   5500,   5500,
	  5750,   5750,   5750,   5750,   5750,   5750,   5750,   5750,   5750,   5750,
	  5750,   5750,   5750,                             5750,   5750,   5750,   5625,
	  6000,   6000,   6000,   6000,   6000,   6000,   6000,   6000,   6000,   6000,
	  6000,   6000,   6125,                             6000,   6000,   6000,
	  6250,   6250,   6250,   6250,   6250,   6250,   6250,   6250,   6375,   6375,
	  6375,                                             6250,   6250,   6125
};

static const uint8_t sirius_16_iso_kbl_mapping[] = {
	0x29, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42,
	0x43, 0x44, 0x45, 0xf1, 0x46, 0x4c,   0x4a, 0x4d, 0x4b, 0x4e,
	0x35, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
	0x27, 0x2d, 0x2e, 0x2a,               0x53, 0x55, 0x54, 0x56,
	0x2b, 0x14, 0x1a, 0x08, 0x15, 0x17, 0x1c, 0x18, 0x0c, 0x12,
	0x13, 0x2f, 0x30,                     0x5f, 0x60, 0x61,
	0x39, 0x04, 0x16, 0x07, 0x09, 0x0a, 0x0b, 0x0d, 0x0e, 0x0f,
	0x33, 0x34, 0x32, 0x28,               0x5c, 0x5d, 0x5e, 0x57,
	0xe1, 0x64, 0x1d, 0x1b, 0x06, 0x19, 0x05, 0x11, 0x10, 0x36,
	0x37, 0x38, 0xe5, 0x52,               0x59, 0x5a, 0x5b,
	0xe0, 0xfe, 0xe3, 0xe2, 0x2c, 0xe6, 0x65, 0xe4, 0x50, 0x51,
	0x4f,                                 0x62, 0x63, 0x58
};

static const uint32_t sirius_16_iso_kbl_mapping_pos_x[] = {
	 25000,  41700,  58400,  75100,  91800, 108500, 125200, 141900, 158600, 175300,
	192000, 208700, 225400, 242100, 258800, 275500,   294500, 311200, 327900, 344600,
	 24500,  42500,  61000,  79500,  98000, 116500, 135000, 153500, 172000, 190500,
	209000, 227500, 246000, 269500,                   294500, 311200, 327900, 344600,
	 31000,  51500,  70000,  88500, 107000, 125500, 144000, 162500, 181000, 199500,
	218000, 234500, 251000,                           294500, 311200, 327900,
	 33000,  57000,  75500,  94000, 112500, 131000, 149500, 168000, 186500, 205000,
	223500, 240000, 256500, 271500,                   294500, 311200, 327900, 344600,
	 28000,  47500,  66000,  84500, 103000, 121500, 140000, 158500, 177000, 195500,
	214000, 232500, 251500, 273500,                   294500, 311200, 327900,
	 28000,  47500,  66000,  84500, 140000, 195500, 214000, 234000, 255000, 273500,
	292000,                                           311200, 327900, 344600
};

static const uint32_t sirius_16_iso_kbl_mapping_pos_y[] = {
	 53000,  53000,  53000,  53000,  53000,  53000,  53000,  53000,  53000,  53000,
	 53000,  53000,  53000,  53000,  53000,  53000,    53000,  53000,  53000,  53000,
	 67500,  67500,  67500,  67500,  67500,  67500,  67500,  67500,  67500,  67500,
	 67500,  67500,  67500,  67500,                    67500,  67500,  67500,  67500,
	 85500,  85500,  85500,  85500,  85500,  85500,  85500,  85500,  85500,  85500,
	 85500,  85500,  85500,                            85500,  85500,  85500,
	103500, 103500, 103500, 103500, 103500, 103500, 103500, 103500, 103500, 103500,
	103500, 103500, 103500,  94500,                   103500, 103500, 103500,  94500,
	121500, 121500, 121500, 121500, 121500, 121500, 121500, 121500, 121500, 121500,
	121500, 121500, 121500, 129000,                   121500, 121500, 121500,
	139500, 139500, 139500, 139500, 139500, 139500, 139500, 139500, 147000, 147000,
	147000,                                           139500, 139500, 130500
};

static const uint32_t sirius_16_iso_kbl_mapping_pos_z[] = {
	  5000,   5000,   5000,   5000,   5000,   5000,   5000,   5000,   5000,   5000,
	  5000,   5000,   5000,   5000, 5000, 5000,         5000,   5000,   5000,   5000,
	  5250,   5250,   5250,   5250,   5250,   5250,   5250,   5250,   5250,   5250,
	  5250,   5250,   5250,   5250,                     5250,   5250,   5250,   5250,
	  5500,   5500,   5500,   5500,   5500,   5500,   5500,   5500,   5500,   5500,
	  5500,   5500,   5500,                             5500,   5500,   5500,
	  5750,   5750,   5750,   5750,   5750,   5750,   5750,   5750,   5750,   5750,
	  5750,   5750,   5750,   5750,                     5750,   5750,   5750,   5625,
	  6000,   6000,   6000,   6000,   6000,   6000,   6000,   6000,   6000,   6000,
	  6000,   6000,   6000,   6125,                     6000,   6000,   6000,
	  6250,   6250,   6250,   6250,   6250,   6250,   6250,   6250,   6375,   6375,
	  6375,                                             6250,   6250,   6125
};

struct driver_data_t {
	uint8_t keyboard_type;
	uint8_t lamp_count;
	uint8_t next_lamp_id;
	union tuxedo_nb04_wmi_496_b_in_80_b_out_input next_kbl_set_multiple_keys_input;
};


static int ll_start(struct hid_device *hdev)
{
	int ret;
	struct driver_data_t *driver_data;
	struct wmi_device *wdev = dev_to_wdev(hdev->dev.parent);
	union tuxedo_nb04_wmi_8_b_in_80_b_out_input input;
	union tuxedo_nb04_wmi_8_b_in_80_b_out_output output;

	driver_data = devm_kzalloc(&hdev->dev, sizeof(struct driver_data_t), GFP_KERNEL);
	if (!driver_data)
		return -ENOMEM;

	input.get_device_status_input.device_type = WMI_AB_GET_DEVICE_STATUS_DEVICE_ID_KEYBOARD;
	ret = tuxedo_nb04_wmi_8_b_in_80_b_out(wdev, WMI_AB_GET_DEVICE_STATUS, &input, &output);
	if (ret)
		return ret;

	driver_data->keyboard_type = output.get_device_status_output.keyboard_physical_layout;
	driver_data->lamp_count = sizeof(sirius_16_ansii_kbl_mapping);
	driver_data->next_lamp_id = 0;

	hdev->driver_data = driver_data;

	return ret;
}


static void ll_stop(struct hid_device __always_unused *hdev)
{
}


static int ll_open(struct hid_device __always_unused *hdev)
{
	return 0;
}


static void ll_close(struct hid_device __always_unused *hdev)
{
}


static uint8_t report_descriptor[327] = {
	0x05, 0x59,			// Usage Page (Lighting and Illumination)
	0x09, 0x01,			// Usage (Lamp Array)
	0xa1, 0x01,			// Collection (Application)
	0x85, LAMP_ARRAY_ATTRIBUTES_REPORT_ID, //  Report ID (1)
	0x09, 0x02,			//  Usage (Lamp Array Attributes Report)
	0xa1, 0x02,			//  Collection (Logical)
	0x09, 0x03,			//   Usage (Lamp Count)
	0x15, 0x00,			//   Logical Minimum (0)
	0x27, 0xff, 0xff, 0x00, 0x00,	//   Logical Maximum (65535)
	0x75, 0x10,			//   Report Size (16)
	0x95, 0x01,			//   Report Count (1)
	0xb1, 0x03,			//   Feature (Cnst,Var,Abs)
	0x09, 0x04,			//   Usage (Bounding Box Width In Micrometers)
	0x09, 0x05,			//   Usage (Bounding Box Height In Micrometers)
	0x09, 0x06,			//   Usage (Bounding Box Depth In Micrometers)
	0x09, 0x07,			//   Usage (Lamp Array Kind)
	0x09, 0x08,			//   Usage (Min Update Interval In Microseconds)
	0x15, 0x00,			//   Logical Minimum (0)
	0x27, 0xff, 0xff, 0xff, 0x7f,	//   Logical Maximum (2147483647)
	0x75, 0x20,			//   Report Size (32)
	0x95, 0x05,			//   Report Count (5)
	0xb1, 0x03,			//   Feature (Cnst,Var,Abs)
	0xc0,				//  End Collection
	0x85, LAMP_ATTRIBUTES_REQUEST_REPORT_ID, //  Report ID (2)
	0x09, 0x20,			//  Usage (Lamp Attributes Request Report)
	0xa1, 0x02,			//  Collection (Logical)
	0x09, 0x21,			//   Usage (Lamp Id)
	0x15, 0x00,			//   Logical Minimum (0)
	0x27, 0xff, 0xff, 0x00, 0x00,	//   Logical Maximum (65535)
	0x75, 0x10,			//   Report Size (16)
	0x95, 0x01,			//   Report Count (1)
	0xb1, 0x02,			//   Feature (Data,Var,Abs)
	0xc0,				//  End Collection
	0x85, LAMP_ATTRIBUTES_RESPONSE_REPORT_ID, //  Report ID (3)
	0x09, 0x22,			//  Usage (Lamp Attributes Response Report)
	0xa1, 0x02,			//  Collection (Logical)
	0x09, 0x21,			//   Usage (Lamp Id)
	0x15, 0x00,			//   Logical Minimum (0)
	0x27, 0xff, 0xff, 0x00, 0x00,	//   Logical Maximum (65535)
	0x75, 0x10,			//   Report Size (16)
	0x95, 0x01,			//   Report Count (1)
	0xb1, 0x02,			//   Feature (Data,Var,Abs)
	0x09, 0x23,			//   Usage (Position X In Micrometers)
	0x09, 0x24,			//   Usage (Position Y In Micrometers)
	0x09, 0x25,			//   Usage (Position Z In Micrometers)
	0x09, 0x27,			//   Usage (Update Latency In Microseconds)
	0x09, 0x26,			//   Usage (Lamp Purposes)
	0x15, 0x00,			//   Logical Minimum (0)
	0x27, 0xff, 0xff, 0xff, 0x7f,	//   Logical Maximum (2147483647)
	0x75, 0x20,			//   Report Size (32)
	0x95, 0x05,			//   Report Count (5)
	0xb1, 0x02,			//   Feature (Data,Var,Abs)
	0x09, 0x28,			//   Usage (Red Level Count)
	0x09, 0x29,			//   Usage (Green Level Count)
	0x09, 0x2a,			//   Usage (Blue Level Count)
	0x09, 0x2b,			//   Usage (Intensity Level Count)
	0x09, 0x2c,			//   Usage (Is Programmable)
	0x09, 0x2d,			//   Usage (Input Binding)
	0x15, 0x00,			//   Logical Minimum (0)
	0x26, 0xff, 0x00,		//   Logical Maximum (255)
	0x75, 0x08,			//   Report Size (8)
	0x95, 0x06,			//   Report Count (6)
	0xb1, 0x02,			//   Feature (Data,Var,Abs)
	0xc0,				//  End Collection
	0x85, LAMP_MULTI_UPDATE_REPORT_ID, //  Report ID (4)
	0x09, 0x50,			//  Usage (Lamp Multi Update Report)
	0xa1, 0x02,			//  Collection (Logical)
	0x09, 0x03,			//   Usage (Lamp Count)
	0x09, 0x55,			//   Usage (Lamp Update Flags)
	0x15, 0x00,			//   Logical Minimum (0)
	0x25, 0x08,			//   Logical Maximum (8)
	0x75, 0x08,			//   Report Size (8)
	0x95, 0x02,			//   Report Count (2)
	0xb1, 0x02,			//   Feature (Data,Var,Abs)
	0x09, 0x21,			//   Usage (Lamp Id)
	0x15, 0x00,			//   Logical Minimum (0)
	0x27, 0xff, 0xff, 0x00, 0x00,	//   Logical Maximum (65535)
	0x75, 0x10,			//   Report Size (16)
	0x95, 0x08,			//   Report Count (8)
	0xb1, 0x02,			//   Feature (Data,Var,Abs)
	0x09, 0x51,			//   Usage (Red Update Channel)
	0x09, 0x52,			//   Usage (Green Update Channel)
	0x09, 0x53,			//   Usage (Blue Update Channel)
	0x09, 0x54,			//   Usage (Intensity Update Channel)
	0x09, 0x51,			//   Usage (Red Update Channel)
	0x09, 0x52,			//   Usage (Green Update Channel)
	0x09, 0x53,			//   Usage (Blue Update Channel)
	0x09, 0x54,			//   Usage (Intensity Update Channel)
	0x09, 0x51,			//   Usage (Red Update Channel)
	0x09, 0x52,			//   Usage (Green Update Channel)
	0x09, 0x53,			//   Usage (Blue Update Channel)
	0x09, 0x54,			//   Usage (Intensity Update Channel)
	0x09, 0x51,			//   Usage (Red Update Channel)
	0x09, 0x52,			//   Usage (Green Update Channel)
	0x09, 0x53,			//   Usage (Blue Update Channel)
	0x09, 0x54,			//   Usage (Intensity Update Channel)
	0x09, 0x51,			//   Usage (Red Update Channel)
	0x09, 0x52,			//   Usage (Green Update Channel)
	0x09, 0x53,			//   Usage (Blue Update Channel)
	0x09, 0x54,			//   Usage (Intensity Update Channel)
	0x09, 0x51,			//   Usage (Red Update Channel)
	0x09, 0x52,			//   Usage (Green Update Channel)
	0x09, 0x53,			//   Usage (Blue Update Channel)
	0x09, 0x54,			//   Usage (Intensity Update Channel)
	0x09, 0x51,			//   Usage (Red Update Channel)
	0x09, 0x52,			//   Usage (Green Update Channel)
	0x09, 0x53,			//   Usage (Blue Update Channel)
	0x09, 0x54,			//   Usage (Intensity Update Channel)
	0x09, 0x51,			//   Usage (Red Update Channel)
	0x09, 0x52,			//   Usage (Green Update Channel)
	0x09, 0x53,			//   Usage (Blue Update Channel)
	0x09, 0x54,			//   Usage (Intensity Update Channel)
	0x15, 0x00,			//   Logical Minimum (0)
	0x26, 0xff, 0x00,		//   Logical Maximum (255)
	0x75, 0x08,			//   Report Size (8)
	0x95, 0x20,			//   Report Count (32)
	0xb1, 0x02,			//   Feature (Data,Var,Abs)
	0xc0,				//  End Collection
	0x85, LAMP_RANGE_UPDATE_REPORT_ID, //  Report ID (5)
	0x09, 0x60,			//  Usage (Lamp Range Update Report)
	0xa1, 0x02,			//  Collection (Logical)
	0x09, 0x55,			//   Usage (Lamp Update Flags)
	0x15, 0x00,			//   Logical Minimum (0)
	0x25, 0x08,			//   Logical Maximum (8)
	0x75, 0x08,			//   Report Size (8)
	0x95, 0x01,			//   Report Count (1)
	0xb1, 0x02,			//   Feature (Data,Var,Abs)
	0x09, 0x61,			//   Usage (Lamp Id Start)
	0x09, 0x62,			//   Usage (Lamp Id End)
	0x15, 0x00,			//   Logical Minimum (0)
	0x27, 0xff, 0xff, 0x00, 0x00,	//   Logical Maximum (65535)
	0x75, 0x10,			//   Report Size (16)
	0x95, 0x02,			//   Report Count (2)
	0xb1, 0x02,			//   Feature (Data,Var,Abs)
	0x09, 0x51,			//   Usage (Red Update Channel)
	0x09, 0x52,			//   Usage (Green Update Channel)
	0x09, 0x53,			//   Usage (Blue Update Channel)
	0x09, 0x54,			//   Usage (Intensity Update Channel)
	0x15, 0x00,			//   Logical Minimum (0)
	0x26, 0xff, 0x00,		//   Logical Maximum (255)
	0x75, 0x08,			//   Report Size (8)
	0x95, 0x04,			//   Report Count (4)
	0xb1, 0x02,			//   Feature (Data,Var,Abs)
	0xc0,				//  End Collection
	0x85, LAMP_ARRAY_CONTROL_REPORT_ID, //  Report ID (6)
	0x09, 0x70,			//  Usage (Lamp Array Control Report)
	0xa1, 0x02,			//  Collection (Logical)
	0x09, 0x71,			//   Usage (Autonomous Mode)
	0x15, 0x00,			//   Logical Minimum (0)
	0x25, 0x01,			//   Logical Maximum (1)
	0x75, 0x08,			//   Report Size (8)
	0x95, 0x01,			//   Report Count (1)
	0xb1, 0x02,			//   Feature (Data,Var,Abs)
	0xc0,				//  End Collection
	0xc0				// End Collection
};

static int ll_parse(struct hid_device *hdev)
{
	return hid_parse_report(hdev, report_descriptor, sizeof(report_descriptor));
}


struct __packed lamp_array_attributes_report_t {
	const uint8_t report_id;
	uint16_t lamp_count;
	uint32_t bounding_box_width_in_micrometers;
	uint32_t bounding_box_height_in_micrometers;
	uint32_t bounding_box_depth_in_micrometers;
	uint32_t lamp_array_kind;
	uint32_t min_update_interval_in_microseconds;
};

static int handle_lamp_array_attributes_report(struct hid_device *hdev,
					       struct lamp_array_attributes_report_t *rep)
{
	struct driver_data_t *driver_data = hdev->driver_data;

	rep->lamp_count = driver_data->lamp_count;
	rep->bounding_box_width_in_micrometers = 368000;
	rep->bounding_box_height_in_micrometers = 266000;
	rep->bounding_box_depth_in_micrometers = 30000;
	// LampArrayKindKeyboard, see "26.2.1 LampArrayKind Values" of "HID Usage Tables v1.5"
	rep->lamp_array_kind = 1;
	// Some guessed value for interval microseconds
	rep->min_update_interval_in_microseconds = 500;

	return sizeof(struct lamp_array_attributes_report_t);
}


struct __packed lamp_attributes_request_report_t {
	const uint8_t report_id;
	uint16_t lamp_id;
};

static int handle_lamp_attributes_request_report(struct hid_device *hdev,
						 struct lamp_attributes_request_report_t *rep)
{
	struct driver_data_t *driver_data = hdev->driver_data;

	if (rep->lamp_id < driver_data->lamp_count)
		driver_data->next_lamp_id = rep->lamp_id;
	else
		driver_data->next_lamp_id = 0;

	return sizeof(struct lamp_attributes_request_report_t);
}


struct __packed lamp_attributes_response_report_t {
	const uint8_t report_id;
	uint16_t lamp_id;
	uint32_t position_x_in_micrometers;
	uint32_t position_y_in_micrometers;
	uint32_t position_z_in_micrometers;
	uint32_t update_latency_in_microseconds;
	uint32_t lamp_purpose;
	uint8_t red_level_count;
	uint8_t green_level_count;
	uint8_t blue_level_count;
	uint8_t intensity_level_count;
	uint8_t is_programmable;
	uint8_t input_binding;
};

static int handle_lamp_attributes_response_report(struct hid_device *hdev,
						  struct lamp_attributes_response_report_t *rep)
{
	struct driver_data_t *driver_data = hdev->driver_data;
	uint16_t lamp_id = driver_data->next_lamp_id;
	const uint8_t *kbl_mapping;
	const uint32_t *kbl_mapping_pos_x, *kbl_mapping_pos_y, *kbl_mapping_pos_z;

	rep->lamp_id = lamp_id;
	// Some guessed value for latency microseconds
	rep->update_latency_in_microseconds = 100;
	 // LampPurposeControl, see "26.3.1 LampPurposes Flags" of "HID Usage Tables v1.5"
	rep->lamp_purpose = 1;
	rep->red_level_count = 0xff;
	rep->green_level_count = 0xff;
	rep->blue_level_count = 0xff;
	rep->intensity_level_count = 0xff;
	rep->is_programmable = 1;

	if (driver_data->keyboard_type == WMI_AB_GET_DEVICE_STATUS_KEYBOARD_LAYOUT_ANSII) {
		kbl_mapping = &sirius_16_ansii_kbl_mapping[0];
		kbl_mapping_pos_x = &sirius_16_ansii_kbl_mapping_pos_x[0];
		kbl_mapping_pos_y = &sirius_16_ansii_kbl_mapping_pos_y[0];
		kbl_mapping_pos_z = &sirius_16_ansii_kbl_mapping_pos_z[0];
	} else if (driver_data->keyboard_type == WMI_AB_GET_DEVICE_STATUS_KEYBOARD_LAYOUT_ISO) {
		kbl_mapping = &sirius_16_iso_kbl_mapping[0];
		kbl_mapping_pos_x = &sirius_16_iso_kbl_mapping_pos_x[0];
		kbl_mapping_pos_y = &sirius_16_iso_kbl_mapping_pos_y[0];
		kbl_mapping_pos_z = &sirius_16_iso_kbl_mapping_pos_z[0];
	} else
		return -EINVAL;

	if (kbl_mapping[lamp_id] <= 0xe8)
		rep->input_binding = kbl_mapping[lamp_id];
	else
		// Everything bigger is reserved/undefined, see "10 Keyboard/Keypad Page (0x07)" of
		// "HID Usage Tables v1.5" and should return 0, see "26.8.3 Lamp Attributes" of the
		// same document.
		rep->input_binding = 0;
	rep->position_x_in_micrometers = kbl_mapping_pos_x[lamp_id];
	rep->position_y_in_micrometers = kbl_mapping_pos_y[lamp_id];
	rep->position_z_in_micrometers = kbl_mapping_pos_z[lamp_id];

	driver_data->next_lamp_id = (driver_data->next_lamp_id + 1) % driver_data->lamp_count;

	return sizeof(struct lamp_attributes_response_report_t);
}


#define LAMP_UPDATE_FLAGS_LAMP_UPDATE_COMPLETE	BIT(0)

struct __packed lamp_multi_update_report_t {
	const uint8_t report_id;
	uint8_t lamp_count;
	uint8_t lamp_update_flags;
	uint16_t lamp_id[8];
	struct {
		uint8_t red;
		uint8_t green;
		uint8_t blue;
		uint8_t intensity;
	} update_channels[8];
};

static int handle_lamp_multi_update_report(struct hid_device *hdev,
					   struct lamp_multi_update_report_t *rep)
{
	int ret;
	struct driver_data_t *driver_data = hdev->driver_data;
	struct wmi_device *wdev = dev_to_wdev(hdev->dev.parent);
	uint8_t lamp_count, key_id, key_id_j;
	union tuxedo_nb04_wmi_496_b_in_80_b_out_input *next =
		&driver_data->next_kbl_set_multiple_keys_input;
	union tuxedo_nb04_wmi_496_b_in_80_b_out_output output;

	// Catching missformated lamp_multi_update_report and fail silently according to
	// "HID Usage Tables v1.5"
	for (int i = 0; i < rep->lamp_count; ++i) {
		if (driver_data->keyboard_type == WMI_AB_GET_DEVICE_STATUS_KEYBOARD_LAYOUT_ANSII)
			lamp_count = sizeof(sirius_16_ansii_kbl_mapping);
		else if (driver_data->keyboard_type == WMI_AB_GET_DEVICE_STATUS_KEYBOARD_LAYOUT_ISO)
			lamp_count = sizeof(sirius_16_ansii_kbl_mapping);

		if (rep->lamp_id[i] > lamp_count) {
			pr_debug("Out of bounds lamp_id in lamp_multi_update_report. Skippng whole report!\n");
			return sizeof(struct lamp_multi_update_report_t);
		}

		for (int j = i + 1; j < rep->lamp_count; ++j) {
			if (rep->lamp_id[i] == rep->lamp_id[j]) {
				pr_debug("Duplicate lamp_id in lamp_multi_update_report. Skippng whole report!\n");
				return sizeof(struct lamp_multi_update_report_t);
			}
		}
	}

	for (int i = 0; i < rep->lamp_count; ++i) {
		if (driver_data->keyboard_type == WMI_AB_GET_DEVICE_STATUS_KEYBOARD_LAYOUT_ANSII)
			key_id = sirius_16_ansii_kbl_mapping[rep->lamp_id[i]];
		else if (driver_data->keyboard_type == WMI_AB_GET_DEVICE_STATUS_KEYBOARD_LAYOUT_ISO)
			key_id = sirius_16_iso_kbl_mapping[rep->lamp_id[i]];

		for (int j = 0; j < WMI_AB_KBL_SET_MULTIPLE_KEYS_LIGHTING_SETTINGS_COUNT_MAX; ++j) {
			key_id_j = next->kbl_set_multiple_keys_input.lighting_settings[j].key_id;
			if (key_id_j == 0x00 || key_id_j == key_id) {
				if (key_id_j == 0x00)
					next->kbl_set_multiple_keys_input.lighting_setting_count =
						j + 1;
				next->kbl_set_multiple_keys_input.lighting_settings[j].key_id =
					key_id;
				// While this driver respects
				// intensity_update_channel according to "HID
				// Usage Tables v1.5" also on RGB leds, the
				// Microsoft MacroPad reference implementation
				// (https://github.com/microsoft/RP2040MacropadHidSample
				// 1d6c3ad) does not and ignores it. If it turns
				// out that Windows writes intensity = 0 for RGB
				// leds instead of intensity = 255, this driver
				// should also irgnore the
				// intensity_update_channel.
				next->kbl_set_multiple_keys_input.lighting_settings[j].red =
					rep->update_channels[i].red
						* rep->update_channels[i].intensity / 0xff;
				next->kbl_set_multiple_keys_input.lighting_settings[j].green =
					rep->update_channels[i].green
						* rep->update_channels[i].intensity / 0xff;
				next->kbl_set_multiple_keys_input.lighting_settings[j].blue =
					rep->update_channels[i].blue
						* rep->update_channels[i].intensity / 0xff;

				break;
			}
		}
	}

	if (rep->lamp_update_flags & LAMP_UPDATE_FLAGS_LAMP_UPDATE_COMPLETE) {
		ret = tuxedo_nb04_wmi_496_b_in_80_b_out(wdev, WMI_AB_KBL_SET_MULTIPLE_KEYS, next,
							&output);
		memset(next, 0, sizeof(union tuxedo_nb04_wmi_496_b_in_80_b_out_input));
		if (ret)
			return ret;
	}

	return sizeof(struct lamp_multi_update_report_t);
}


struct __packed lamp_range_update_report_t {
	const uint8_t report_id;
	uint8_t lamp_update_flags;
	uint16_t lamp_id_start;
	uint16_t lamp_id_end;
	uint8_t red_update_channel;
	uint8_t green_update_channel;
	uint8_t blue_update_channel;
	uint8_t intensity_update_channel;
};

static int handle_lamp_range_update_report(struct hid_device *hdev,
					   struct lamp_range_update_report_t *report)
{
	int ret;
	struct driver_data_t *driver_data = hdev->driver_data;
	uint8_t lamp_count;
	struct lamp_multi_update_report_t lamp_multi_update_report = {
		.report_id = LAMP_MULTI_UPDATE_REPORT_ID
	};

	// Catching missformated lamp_range_update_report and fail silently according to
	// "HID Usage Tables v1.5"
	if (report->lamp_id_start > report->lamp_id_end) {
		pr_debug("lamp_id_start > lamp_id_end in lamp_range_update_report. Skippng whole report!\n");
		return sizeof(struct lamp_range_update_report_t);
	}

	if (driver_data->keyboard_type == WMI_AB_GET_DEVICE_STATUS_KEYBOARD_LAYOUT_ANSII)
		lamp_count = sizeof(sirius_16_ansii_kbl_mapping);
	else if (driver_data->keyboard_type == WMI_AB_GET_DEVICE_STATUS_KEYBOARD_LAYOUT_ISO)
		lamp_count = sizeof(sirius_16_ansii_kbl_mapping);

	if (report->lamp_id_end > lamp_count - 1) {
		pr_debug("Out of bounds lamp_id_* in lamp_range_update_report. Skippng whole report!\n");
		return sizeof(struct lamp_range_update_report_t);
	}

	// Break handle_lamp_range_update_report call down to multiple
	// handle_lamp_multi_update_report calls to easily ensure that mixing
	// handle_lamp_range_update_report and handle_lamp_multi_update_report
	// does not break things.
	for (int i = report->lamp_id_start; i < report->lamp_id_end + 1; i = i + 8) {
		lamp_multi_update_report.lamp_count = MIN(report->lamp_id_end + 1 - i, 8);
		if (i + lamp_multi_update_report.lamp_count == report->lamp_id_end + 1)
			lamp_multi_update_report.lamp_update_flags |=
				LAMP_UPDATE_FLAGS_LAMP_UPDATE_COMPLETE;

		for (int j = 0; j < lamp_multi_update_report.lamp_count; ++j) {
			lamp_multi_update_report.lamp_id[j] = i + j;
			lamp_multi_update_report.update_channels[j].red =
				report->red_update_channel;
			lamp_multi_update_report.update_channels[j].green =
				report->green_update_channel;
			lamp_multi_update_report.update_channels[j].blue =
				report->blue_update_channel;
			lamp_multi_update_report.update_channels[j].intensity =
				report->intensity_update_channel;
		}

		ret = handle_lamp_multi_update_report(hdev, &lamp_multi_update_report);
		if (ret < 0)
			return ret;
		else if (ret != sizeof(struct lamp_multi_update_report_t))
			return -EIO;
	}

	return sizeof(struct lamp_range_update_report_t);
}


struct __packed lamp_array_control_report_t {
	const uint8_t report_id;
	uint8_t autonomous_mode;
};

static int handle_lamp_array_control_report(struct hid_device __always_unused *hdev,
					    struct lamp_array_control_report_t __always_unused *rep)
{
	// The keyboard firmware doesn't have any built in effects or controls
	// so this is a NOOP.
	// According to the HID Documentation (HID Usage Tables v1.5) this
	// function is optional and can be removed from the HID Report
	// Descriptor, but it should first be confirmed that userspace respects
	// this possibility too. The Microsoft MacroPad reference implementation
	// (https://github.com/microsoft/RP2040MacropadHidSample 1d6c3ad)
	// already deviates from the spec at another point, see
	// handle_lamp_*_update_report.

	return sizeof(struct lamp_array_control_report_t);
}


static int ll_raw_request(struct hid_device *hdev, unsigned char reportnum, __u8 *buf, size_t len,
			   unsigned char rtype, int reqtype)
{
	int ret;

	pr_debug("Recived report: rtype: %u, reqtype: %u, reportnum: %u, len: %lu buf:\n", rtype,
		 reqtype, reportnum, len);
	print_hex_dump_bytes("", DUMP_PREFIX_OFFSET, buf, len);

	ret = -EINVAL;
	if (rtype == HID_FEATURE_REPORT) {
		if (reqtype == HID_REQ_GET_REPORT) {
			if (reportnum == LAMP_ARRAY_ATTRIBUTES_REPORT_ID
			    && len == sizeof(struct lamp_array_attributes_report_t))
				ret = handle_lamp_array_attributes_report(
					hdev, (struct lamp_array_attributes_report_t *)buf);
			else if (reportnum == LAMP_ATTRIBUTES_RESPONSE_REPORT_ID
			    && len == sizeof(struct lamp_attributes_response_report_t))
				ret = handle_lamp_attributes_response_report(
					hdev, (struct lamp_attributes_response_report_t *)buf);
		} else if (reqtype == HID_REQ_SET_REPORT) {
			if (reportnum == LAMP_ATTRIBUTES_REQUEST_REPORT_ID
			    && len == sizeof(struct lamp_attributes_request_report_t))
				ret = handle_lamp_attributes_request_report(
					hdev, (struct lamp_attributes_request_report_t *)buf);
			else if (reportnum == LAMP_MULTI_UPDATE_REPORT_ID
			    && len == sizeof(struct lamp_multi_update_report_t))
				ret = handle_lamp_multi_update_report(
					hdev, (struct lamp_multi_update_report_t *)buf);
			else if (reportnum == LAMP_RANGE_UPDATE_REPORT_ID
			    && len == sizeof(struct lamp_range_update_report_t))
				ret = handle_lamp_range_update_report(
					hdev, (struct lamp_range_update_report_t *)buf);
			else if (reportnum == LAMP_ARRAY_CONTROL_REPORT_ID
			    && len == sizeof(struct lamp_array_control_report_t))
				ret = handle_lamp_array_control_report(
					hdev, (struct lamp_array_control_report_t *)buf);
		}
	}

	return ret;
}

static const struct hid_ll_driver ll_driver = {
	.start = &ll_start,
	.stop = &ll_stop,
	.open = &ll_open,
	.close = &ll_close,
	.parse = &ll_parse,
	.raw_request = &ll_raw_request,
};

int tuxedo_nb04_virtual_lamp_array_add_device(struct wmi_device *wdev, struct hid_device **hdev_out)
{
	int ret;
	struct hid_device *hdev;

	pr_debug("Adding TUXEDO NB04 Virtual LampArray device.\n");

	hdev = hid_allocate_device();
	if (IS_ERR(hdev))
		return PTR_ERR(hdev);
	*hdev_out = hdev;

	strscpy(hdev->name, "TUXEDO NB04 RGB Lighting", sizeof(hdev->name));

	hdev->ll_driver = &ll_driver;
	hdev->bus = BUS_VIRTUAL;
	hdev->vendor = 0x21ba;
	hdev->product = 0x0400;
	hdev->dev.parent = &wdev->dev;

	ret = hid_add_device(hdev);
	if (ret)
		hid_destroy_device(hdev);
	return ret;
}
EXPORT_SYMBOL(tuxedo_nb04_virtual_lamp_array_add_device);
