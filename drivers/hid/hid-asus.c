// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for Asus notebook built-in keyboard.
 *  Fixes small logical maximum to match usage maximum.
 *
 *  Currently supported devices are:
 *    EeeBook X205TA
 *    VivoBook E200HA
 *
 *  Copyright (c) 2016 Yusuke Fujimaki <usk.fujimaki@gmail.com>
 *
 *  This module based on hid-ortek by
 *  Copyright (c) 2010 Johnathon Harris <jmharris@gmail.com>
 *  Copyright (c) 2011 Jiri Kosina
 *
 *  This module has been updated to add support for Asus i2c touchpad.
 *
 *  Copyright (c) 2016 Brendan McGrath <redmcg@redmandi.dyndns.org>
 *  Copyright (c) 2016 Victor Vlasenko <victor.vlasenko@sysgears.com>
 *  Copyright (c) 2016 Frederik Wenigwieser <frederik.wenigwieser@gmail.com>
 */

#include <linux/acpi.h>
#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/hid.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_data/x86/asus-wmi.h>
#include <linux/types.h>
#include <linux/input/mt.h>
#include <linux/usb.h> /* For to_usb_interface for T100 touchpad intf check */
#include <linux/power_supply.h>
#include <linux/stddef.h>
#include <linux/sysfs.h>
#include <linux/leds.h>
#include <linux/unaligned.h>

#include "hid-ids.h"

MODULE_AUTHOR("Yusuke Fujimaki <usk.fujimaki@gmail.com>");
MODULE_AUTHOR("Brendan McGrath <redmcg@redmandi.dyndns.org>");
MODULE_AUTHOR("Victor Vlasenko <victor.vlasenko@sysgears.com>");
MODULE_AUTHOR("Frederik Wenigwieser <frederik.wenigwieser@gmail.com>");
MODULE_AUTHOR("Denis Benato <denis.benato@linux.dev>");
MODULE_AUTHOR("Luke Jones <luke@ljones.dev>");
MODULE_AUTHOR("Khamunetri Clark <khamunetriclark@gmail.com>");
MODULE_DESCRIPTION("Asus HID Keyboard and TouchPad");

#define T100_TPAD_INTF 2
#define MEDION_E1239T_TPAD_INTF 1

#define E1239T_TP_TOGGLE_REPORT_ID 0x05
#define T100CHI_MOUSE_REPORT_ID 0x06
#define FEATURE_REPORT_ID 0x0d
#define INPUT_REPORT_ID 0x5d
#define FEATURE_KBD_REPORT_ID 0x5a
#define FEATURE_KBD_REPORT_SIZE 64
#define FEATURE_KBD_LED_REPORT_ID1 0x5d
#define FEATURE_KBD_LED_REPORT_ID2 0x5e

#define ROG_ALLY_REPORT_SIZE 64
#define ROG_ALLY_X_MIN_MCU 313
#define ROG_ALLY_MIN_MCU 319

#define HID_ALLY_INTF_KEYBOARD_IN 0x81
#define HID_ALLY_INTF_CFG_IN 0x83
#define HID_ALLY_X_INTF_IN 0x87

#define HID_ALLY_GET_REPORT_ID 0x0D
#define HID_ALLY_SET_REPORT_ID 0x5A
#define HID_ALLY_FEATURE_CODE_PAGE 0xD1

#define HID_ALLY_X_INPUT_REPORT_SIZE 16
#define HID_ALLY_X_INPUT_REPORT 0x0B

#define HID_ALLY_READY_MAX_TRIES 6

/* Spurious HID codes sent by QUIRK_ROG_NKEY_KEYBOARD devices */
#define ASUS_SPURIOUS_CODE_0XEA 0xea
#define ASUS_SPURIOUS_CODE_0XEC 0xec
#define ASUS_SPURIOUS_CODE_0X02 0x02
#define ASUS_SPURIOUS_CODE_0X8A 0x8a
#define ASUS_SPURIOUS_CODE_0X9E 0x9e

/* Special key codes */
#define ASUS_FAN_CTRL_KEY_CODE 0xae

#define SUPPORT_KBD_BACKLIGHT BIT(0)

#define MAX_TOUCH_MAJOR 8
#define MAX_PRESSURE 128

#define BTN_LEFT_MASK 0x01
#define CONTACT_TOOL_TYPE_MASK 0x80
#define CONTACT_X_MSB_MASK 0xf0
#define CONTACT_Y_MSB_MASK 0x0f
#define CONTACT_TOUCH_MAJOR_MASK 0x07
#define CONTACT_PRESSURE_MASK 0x7f

#define	BATTERY_REPORT_ID	(0x03)
#define	BATTERY_REPORT_SIZE	(1 + 8)
#define	BATTERY_LEVEL_MAX	((u8)255)
#define	BATTERY_STAT_DISCONNECT	(0)
#define	BATTERY_STAT_CHARGING	(1)
#define	BATTERY_STAT_FULL	(2)

#define QUIRK_FIX_NOTEBOOK_REPORT	BIT(0)
#define QUIRK_NO_INIT_REPORTS		BIT(1)
#define QUIRK_SKIP_INPUT_MAPPING	BIT(2)
#define QUIRK_IS_MULTITOUCH		BIT(3)
#define QUIRK_NO_CONSUMER_USAGES	BIT(4)
#define QUIRK_USE_KBD_BACKLIGHT		BIT(5)
#define QUIRK_T100_KEYBOARD		BIT(6)
#define QUIRK_T100CHI			BIT(7)
#define QUIRK_G752_KEYBOARD		BIT(8)
#define QUIRK_T90CHI			BIT(9)
#define QUIRK_MEDION_E1239T		BIT(10)
#define QUIRK_ROG_NKEY_KEYBOARD		BIT(11)
#define QUIRK_ROG_CLAYMORE_II_KEYBOARD	BIT(12)
#define QUIRK_ROG_ALLY_XPAD		BIT(13)
#define QUIRK_HID_FN_LOCK		BIT(14)
#define QUIRK_FILTER_CAMERA_COMPANION	BIT(15)

#define I2C_KEYBOARD_QUIRKS			(QUIRK_FIX_NOTEBOOK_REPORT | \
						 QUIRK_NO_INIT_REPORTS | \
						 QUIRK_NO_CONSUMER_USAGES)
#define I2C_TOUCHPAD_QUIRKS			(QUIRK_NO_INIT_REPORTS | \
						 QUIRK_SKIP_INPUT_MAPPING | \
						 QUIRK_IS_MULTITOUCH)

#define TRKID_SGN       ((TRKID_MAX + 1) >> 1)

enum asus_work_action_type {
	FN_LOCK_SYNC,
	BRIGHTNESS_SET,
	WMI_FAN,
};

struct hid_raw_event_data {
	u8 report_data[FEATURE_KBD_REPORT_SIZE];
	size_t report_size;
};

struct asus_work_action {
	struct list_head node;
	enum asus_work_action_type type;
	union {
		/* Data for BRIGHTNESS_SET */
		unsigned int brightness;

		/* Data for FN_LOCK_SYNC */
		bool fn_lock;

		/* Data for WMI_FAN */
		struct hid_raw_event_data fan_hid_data;
	} data;
};

struct asus_worker {
	struct hid_device *hdev;
	struct work_struct work;
	struct list_head actions;
	spinlock_t lock;
	bool removed;
};

struct asus_touchpad_info {
	int max_x;
	int max_y;
	int res_x;
	int res_y;
	int contact_size;
	int max_contacts;
	int report_size;
};

struct ally_config {
	/* Must be locked if the data is being changed */
	struct mutex config_mutex;
	bool initialized;

	/* Device capabilities flags */
	bool is_ally_x;
	bool xbox_controller_support;
	bool user_cal_support;
	bool turbo_support;
	bool resp_curve_support;
	bool dir_to_btn_support;
	bool gyro_support;
	bool anti_deadzone_support;

	/* Current settings */
	bool xbox_controller_enabled;
	u8 gamepad_mode;
	u8 left_deadzone;
	u8 left_outer_threshold;
	u8 right_deadzone;
	u8 right_outer_threshold;
	u8 left_anti_deadzone;
	u8 right_anti_deadzone;
	u8 left_trigger_min;
	u8 left_trigger_max;
	u8 right_trigger_min;
	u8 right_trigger_max;

	/* Vibration settings */
	u8 vibration_intensity_left;
	u8 vibration_intensity_right;
};

struct ally_handheld {
	/* All read/write to IN interfaces must lock */
	struct mutex intf_mutex;
	/*
	 * USB device of the connected controller, held with a reference:
	 * at most one ROG Ally can exist, so probes from any other USB
	 * device are rejected to protect the shared state.
	 */
	struct usb_device *udev;
	struct hid_device *cfg_hdev;

	struct input_dev *ally_x_input;
	struct hid_device *ally_x_hdev;

	struct hid_device *keyboard_hdev;
	struct input_dev *keyboard_input;

	u8 cad_sequence_state;
	unsigned long cad_last_event_time;

	struct delayed_work resume_work;

	struct ally_config *config;
};

struct asus_drvdata {
	unsigned long quirks;
	struct hid_device *hdev;
	struct input_dev *input;
	struct input_dev *tp_kbd_input;
	struct asus_worker *worker;
	unsigned int kbd_backlight_brightness;
	struct ally_handheld *rog_ally;
	const struct asus_touchpad_info *tp;
	struct power_supply *battery;
	struct power_supply_desc battery_desc;
	int battery_capacity;
	int battery_stat;
	bool battery_in_query;
	unsigned long battery_next_query;
	struct asus_hid_listener listener;
	bool fn_lock;
};

static int asus_report_battery(struct asus_drvdata *, u8 *, int);

static const struct asus_touchpad_info asus_i2c_tp = {
	.max_x = 2794,
	.max_y = 1758,
	.contact_size = 5,
	.max_contacts = 5,
	.report_size = 28 /* 2 byte header + 5 * 5 + 1 byte footer */,
};

static const struct asus_touchpad_info asus_t100ta_tp = {
	.max_x = 2240,
	.max_y = 1120,
	.res_x = 30, /* units/mm */
	.res_y = 27, /* units/mm */
	.contact_size = 5,
	.max_contacts = 5,
	.report_size = 28 /* 2 byte header + 5 * 5 + 1 byte footer */,
};

static const struct asus_touchpad_info asus_t100ha_tp = {
	.max_x = 2640,
	.max_y = 1320,
	.res_x = 30, /* units/mm */
	.res_y = 29, /* units/mm */
	.contact_size = 5,
	.max_contacts = 5,
	.report_size = 28 /* 2 byte header + 5 * 5 + 1 byte footer */,
};

static const struct asus_touchpad_info asus_t200ta_tp = {
	.max_x = 3120,
	.max_y = 1716,
	.res_x = 30, /* units/mm */
	.res_y = 28, /* units/mm */
	.contact_size = 5,
	.max_contacts = 5,
	.report_size = 28 /* 2 byte header + 5 * 5 + 1 byte footer */,
};

static const struct asus_touchpad_info asus_t100chi_tp = {
	.max_x = 2640,
	.max_y = 1320,
	.res_x = 31, /* units/mm */
	.res_y = 29, /* units/mm */
	.contact_size = 3,
	.max_contacts = 4,
	.report_size = 15 /* 2 byte header + 3 * 4 + 1 byte footer */,
};

static const struct asus_touchpad_info medion_e1239t_tp = {
	.max_x = 2640,
	.max_y = 1380,
	.res_x = 29, /* units/mm */
	.res_y = 28, /* units/mm */
	.contact_size = 5,
	.max_contacts = 5,
	.report_size = 32 /* 2 byte header + 5 * 5 + 5 byte footer */,
};

enum ally_command_codes {
	CMD_SET_GAMEPAD_MODE            = 0x01,
	CMD_SET_MAPPING                 = 0x02,
	CMD_SET_JOYSTICK_MAPPING        = 0x03,
	CMD_SET_JOYSTICK_DEADZONE       = 0x04,
	CMD_SET_TRIGGER_RANGE           = 0x05,
	CMD_SET_VIBRATION_INTENSITY     = 0x06,
	CMD_LED_CONTROL                 = 0x08,
	CMD_CHECK_READY                 = 0x0A,
	CMD_SET_XBOX_CONTROLLER         = 0x0B,
	CMD_CHECK_XBOX_SUPPORT          = 0x0C,
	CMD_USER_CAL_DATA               = 0x0D,
	CMD_CHECK_USER_CAL_SUPPORT      = 0x0E,
	CMD_SET_TURBO_PARAMS            = 0x0F,
	CMD_CHECK_TURBO_SUPPORT         = 0x10,
	CMD_CHECK_RESP_CURVE_SUPPORT    = 0x12,
	CMD_SET_RESP_CURVE              = 0x13,
	CMD_CHECK_DIR_TO_BTN_SUPPORT    = 0x14,
	CMD_SET_GYRO_PARAMS             = 0x15,
	CMD_CHECK_GYRO_TO_JOYSTICK      = 0x16,
	CMD_CHECK_ANTI_DEADZONE         = 0x17,
	CMD_SET_ANTI_DEADZONE           = 0x18,
};

static const u8 ALLY_FORCE_FEEDBACK_OFF[] = {
	0x0D, 0x0F, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xEB
};

/*
 * The ROG Ally device presents multiple USB interfaces (keyboard, mouse, gamepad,
 * and custom configuration interface) that bind to the same module. Since only
 * one ROG Ally device can be connected at a time, we use a single global static
 * ally_handheld structure to share state across these separate HID interfaces.
 */
static void ally_resume_work_fn(struct work_struct *work);

/*
 * Changes to ally_drvdata must lock: the raw_event callbacks, which may
 * run in atomic (URB completion) context, also take this lock, so it must
 * be a spinlock.
 */
static DEFINE_SPINLOCK(ally_data_lock);
static struct ally_handheld ally_drvdata = {
	.intf_mutex = __MUTEX_INITIALIZER(ally_drvdata.intf_mutex),
	/*
	 * Initialised statically so it is always safe to cancel, whichever
	 * of the interfaces probed or failed to probe.
	 */
	.resume_work = __DELAYED_WORK_INITIALIZER(ally_drvdata.resume_work,
						  ally_resume_work_fn, 0),
};

/*
 * Drop the recorded USB device if no Ally interface is bound anymore:
 * probe failures never run remove(), so without this cleanup a failed
 * probe would leave a stale record behind and the driver would then
 * reject the real controller when it reconnects.
 */
static void ally_put_udev_if_orphaned(void)
{
	struct usb_device *udev;
	unsigned long flags;

	spin_lock_irqsave(&ally_data_lock, flags);
	if (!ally_drvdata.udev || ally_drvdata.keyboard_hdev ||
	    ally_drvdata.cfg_hdev || ally_drvdata.ally_x_hdev) {
		spin_unlock_irqrestore(&ally_data_lock, flags);
		return;
	}

	udev = ally_drvdata.udev;
	ally_drvdata.udev = NULL;
	spin_unlock_irqrestore(&ally_data_lock, flags);

	usb_put_dev(udev);
}

static const u8 asus_report_id_init[] = {
	FEATURE_KBD_REPORT_ID,
	FEATURE_KBD_LED_REPORT_ID1,
	FEATURE_KBD_LED_REPORT_ID2
};

static inline int ally_dev_set_report(struct hid_device *hdev, const u8 *buf, size_t len)
{
	u8 *dmabuf __free(kfree) = kmemdup(buf, len, GFP_KERNEL);
	if (!dmabuf)
		return -ENOMEM;

	return hid_hw_raw_request(hdev, buf[0], dmabuf, len,
					HID_FEATURE_REPORT, HID_REQ_SET_REPORT);
}

static inline int ally_dev_get_report(struct hid_device *hdev, u8 *out, size_t len)
{
	return hid_hw_raw_request(hdev, HID_ALLY_GET_REPORT_ID, out, len,
		HID_FEATURE_REPORT, HID_REQ_GET_REPORT);
}

static void ally_resume_work_fn(struct work_struct *work)
{
	struct ally_handheld *ally = container_of(work, struct ally_handheld,
						  resume_work.work);
	struct input_dev *keyboard_input, *x_input;
	unsigned long flags;

	/*
	 * Snapshot the very pointers that get dereferenced under the lock
	 * protecting them, and take a reference on each input_dev: probe
	 * sets keyboard_hdev even when the interface exposes no input_dev,
	 * removal clears the two fields one after the other, and the input
	 * devices are freed with their own interface.
	 */
	spin_lock_irqsave(&ally_data_lock, flags);
	keyboard_input = input_get_device(ally->keyboard_input);
	x_input = input_get_device(ally->ally_x_input);
	spin_unlock_irqrestore(&ally_data_lock, flags);

	/*
	 * Force release all vendor buttons to prevent "stuck" ghosting on
	 * resume (workaround for Ally X USB re-probing during suspend/resume).
	 */
	if (keyboard_input) {
		input_report_key(keyboard_input, KEY_F16, 0);
		input_report_key(keyboard_input, KEY_F17, 0);
		input_report_key(keyboard_input, KEY_F18, 0);
		input_report_key(keyboard_input, KEY_F19, 0);
		input_report_key(keyboard_input, KEY_F20, 0);
		input_report_key(keyboard_input, KEY_PROG1, 0);
		input_sync(keyboard_input);
		input_put_device(keyboard_input);
	}

	if (x_input) {
		input_report_key(x_input, KEY_F16, 0);
		input_report_key(x_input, KEY_F17, 0);
		input_report_key(x_input, KEY_F18, 0);
		input_report_key(x_input, KEY_PROG1, 0);
		input_sync(x_input);
		input_put_device(x_input);
	}
}

/**
 * handle_ctrl_alt_del() - detect a left button long press
 * @hdev: HID device the report arrived on
 * @ally: ally handheld structure holding the sequence state
 * @data: raw report buffer, rewritten in place when the sequence matches
 * @size: length of @data in bytes
 *
 * The Ally left button emits a sequence of ctrl+alt+del events. Capture that
 * and emit only a single code for that single event.
 *
 * Return: true iff the event has been managed
 */
static bool handle_ctrl_alt_del(struct hid_device *hdev,
				struct ally_handheld *ally, u8 *data, int size)
{
	bool time_is_past = time_after(jiffies, ally->cad_last_event_time + msecs_to_jiffies(100));

	if (size < 16 || data[0] != 0x01)
		return false;

	if (ally->cad_sequence_state > 0 && time_is_past)
		ally->cad_sequence_state = 0;

	ally->cad_last_event_time = jiffies;

	switch (ally->cad_sequence_state) {
	case 0:
		if (data[1] == 0x01 && data[2] == 0x00 && data[3] == 0x00) {
			ally->cad_sequence_state = 1;
			data[1] = 0x00;
			return true;
		}
		break;
	case 1:
		if (data[1] == 0x05 && data[2] == 0x00 && data[3] == 0x00) {
			ally->cad_sequence_state = 2;
			data[1] = 0x00;
			return true;
		}
		break;
	case 2:
		if (data[1] == 0x05 && data[2] == 0x00 && data[3] == 0x4c) {
			ally->cad_sequence_state = 3;
			data[1] = 0x00;
			data[3] = 0x6F; // F20;
			return true;
		}
		break;
	case 3:
		if (data[1] == 0x04 && data[2] == 0x00 && data[3] == 0x4c) {
			ally->cad_sequence_state = 4;
			data[1] = data[3] = 0x00;
			return true;
		}
		break;
	case 4:
		if (data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x4c) {
			ally->cad_sequence_state = 5;
			data[3] = 0x00;
			return true;
		}
		break;
	}
	ally->cad_sequence_state = 0;
	return false;
}

static bool handle_ally_event(struct hid_device *hdev, struct ally_handheld *ally,
			      u8 *data, int size)
{
	struct input_dev *keyboard_input;
	unsigned long flags;
	int keycode = 0;

	if (size < 2)
		return false;

	if (data[0] == 0x5A) {
		switch (data[1]) {
		case 0x38:
			keycode = KEY_F19;
			break;
		case 0xA6:
			keycode = KEY_F16;
			break;
		case 0xA7:
			keycode = KEY_F17;
			break;
		case 0xA8:
			keycode = KEY_F18;
			break;
		default:
			return false;
		}

		/*
		 * Take a reference on the input_dev before dropping the lock:
		 * the keyboard interface can be unbound concurrently, and its
		 * input_dev is freed with it.
		 */
		spin_lock_irqsave(&ally_data_lock, flags);
		keyboard_input = input_get_device(ally->keyboard_input);
		spin_unlock_irqrestore(&ally_data_lock, flags);

		if (!keyboard_input)
			return false;

		input_report_key(keyboard_input, keycode, 1);
		input_sync(keyboard_input);
		input_report_key(keyboard_input, keycode, 0);
		input_sync(keyboard_input);
		input_put_device(keyboard_input);
		return true;
	}
	return false;
}

/**
 * ally_gamepad_send_packet() - Send a raw packet to the gamepad device
 * @ally: ally handheld structure
 * @hdev: HID device
 * @buf: buffer containing the packet data
 * @len: length of data to send
 *
 * Return: count of data transferred, negative if error
 */
static int ally_gamepad_send_packet(struct ally_handheld *ally,
				    struct hid_device *hdev, const u8 *buf, size_t len)
{
	scoped_guard(mutex, &ally->intf_mutex)
		return ally_dev_set_report(hdev, buf, len);
}

/**
 * ally_gamepad_send_receive_packet() - Send a packet and receive the response
 * @ally: ally handheld structure
 * @hdev: HID device
 * @buf: buffer containing the packet data to send and receive response in
 * @len: length of buffer
 *
 * Return: count of data transferred, negative if error
 */
static int ally_gamepad_send_receive_packet(struct ally_handheld *ally,
					    struct hid_device *hdev,
					    u8 *buf, size_t len)
{
	int ret;

	scoped_guard(mutex, &ally->intf_mutex) {
		ret = ally_dev_set_report(hdev, buf, len);
		if (ret >= 0) {
			memset(buf, 0, len);
			ret = ally_dev_get_report(hdev, buf, len);
		}
	}

	return ret;
}

/**
 * ally_alloc_cmd() - Construct a command buffer for the gamepad
 * @cmd: Command code to send
 * @payload: Optional payload data to include in the command
 * @payload_size: Size of the payload data
 *
 * The constructed buffer is 64 bytes long, and it is the caller
 * responsibility to free the buffer using kfree().
 *
 * Return: the newly allocated buffer containing the command, or NULL on
 * allocation failure
 */
static u8 *ally_alloc_cmd(u8 cmd, const u8 *payload, u8 payload_size)
{
	u8 *hidbuf = kzalloc(ROG_ALLY_REPORT_SIZE, GFP_KERNEL);

	if (!hidbuf)
		return NULL;

	hidbuf[0] = HID_ALLY_SET_REPORT_ID;
	hidbuf[1] = HID_ALLY_FEATURE_CODE_PAGE;
	hidbuf[2] = cmd;
	hidbuf[3] = payload_size;

	if (payload_size > 0 && payload)
		memcpy(&hidbuf[4], payload, payload_size);

	return hidbuf;
}

/**
 * ally_check_capability() - Check if a specific capability is supported
 * @hdev: HID device
 * @ally: ally handheld structure
 * @check_cmd: capability command code to query
 *
 * Return: true if the capability is supported, false otherwise
 */
static bool ally_check_capability(struct hid_device *hdev, struct ally_handheld *ally,
				  enum ally_command_codes check_cmd)
{
	u8 payload[] = { 0x00 };
	int ret;

	u8 *buf __free(kfree) = ally_alloc_cmd(check_cmd, payload, sizeof(payload));
	if (!buf) {
		hid_err(hdev, "Failed to allocate buffer for capability check.\n");
		return false;
	}

	ret = ally_gamepad_send_receive_packet(ally, hdev, buf, ROG_ALLY_REPORT_SIZE);
	if (ret < 0) {
		hid_err(hdev, "Failed to check capability 0x%02x: %d\n", check_cmd, ret);
		return false;
	}

	return buf[1] == HID_ALLY_FEATURE_CODE_PAGE && buf[2] == check_cmd &&
	       buf[4] == 0x01;
}

static int ally_detect_capabilities(struct hid_device *hdev, struct ally_handheld *ally,
				    struct ally_config *cfg)
{
	if (!hdev || !cfg || !ally)
		return -EINVAL;

	scoped_guard(mutex, &cfg->config_mutex) {
		cfg->is_ally_x = (hdev->product == USB_DEVICE_ID_ASUSTEK_ROG_NKEY_ALLY_X);

		cfg->xbox_controller_support =
			ally_check_capability(hdev, ally, CMD_CHECK_XBOX_SUPPORT);
		cfg->user_cal_support =
			ally_check_capability(hdev, ally, CMD_CHECK_USER_CAL_SUPPORT);
		cfg->turbo_support =
			ally_check_capability(hdev, ally, CMD_CHECK_TURBO_SUPPORT);
		cfg->resp_curve_support =
			ally_check_capability(hdev, ally, CMD_CHECK_RESP_CURVE_SUPPORT);
		cfg->dir_to_btn_support =
			ally_check_capability(hdev, ally, CMD_CHECK_DIR_TO_BTN_SUPPORT);
		cfg->gyro_support =
			ally_check_capability(hdev, ally, CMD_CHECK_GYRO_TO_JOYSTICK);
		cfg->anti_deadzone_support =
			ally_check_capability(hdev, ally, CMD_CHECK_ANTI_DEADZONE);
	}

	return 0;
}

static int ally_set_xbox_controller(struct hid_device *hdev,
				    struct ally_handheld *ally,
				    struct ally_config *cfg, bool enabled)
{
	u8 payload[] = { enabled ? 0x01 : 0x00 };
	int ret;

	if (!cfg || !cfg->xbox_controller_support)
		return -ENODEV;

	u8 *buf __free(kfree) = ally_alloc_cmd(CMD_SET_XBOX_CONTROLLER, payload, sizeof(payload));
	if (!buf)
		return -ENOMEM;

	ret = ally_gamepad_send_packet(ally, hdev, buf, ROG_ALLY_REPORT_SIZE);
	if (ret < 0) {
		hid_err(hdev, "Failed to set Xbox controller mode: %d\n", ret);
		return ret;
	}

	cfg->xbox_controller_enabled = enabled;
	return 0;
}

/**
 * ally_get_config() - Get the configuration of the Ally device
 * @ally: ally handheld structure
 *
 * Fetch the configuration published by hid_asus_ally_probe() under
 * ally_data_lock: the pointer is also cleared by hid_asus_ally_remove(),
 * so reading it without the lock would race with interface removal.
 *
 * The returned configuration outlives the sysfs callbacks using it: it is
 * devm-managed on the same device as the sysfs attributes, which are
 * removed before the memory is released.
 *
 * Return: the ally config, or NULL if no configuration is published
 */
static struct ally_config *ally_get_config(struct ally_handheld *ally)
{
	struct ally_config *cfg;
	unsigned long flags;

	spin_lock_irqsave(&ally_data_lock, flags);
	cfg = ally->config;
	spin_unlock_irqrestore(&ally_data_lock, flags);

	return cfg;
}

static ssize_t xbox_controller_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	struct ally_handheld *ally = drvdata->rog_ally;
	struct ally_config *cfg;

	if (!ally)
		return -ENODEV;

	cfg = ally_get_config(ally);
	if (!cfg)
		return -ENODEV;

	guard(mutex)(&cfg->config_mutex);

	if (!cfg->xbox_controller_support)
		return -ENODEV;

	return sysfs_emit(buf, "%d\n", cfg->xbox_controller_enabled ? 1 : 0);
}

static ssize_t xbox_controller_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	struct ally_handheld *ally = drvdata->rog_ally;
	struct ally_config *cfg;
	bool enabled;
	int ret;

	if (!ally)
		return -ENODEV;

	cfg = ally_get_config(ally);
	if (!cfg)
		return -ENODEV;

	ret = kstrtobool(buf, &enabled);
	if (ret)
		return ret;

	guard(mutex)(&cfg->config_mutex);

	if (!cfg->xbox_controller_support)
		return -ENODEV;

	ret = ally_set_xbox_controller(hdev, ally, cfg, enabled);
	if (ret < 0)
		return ret;

	return count;
}

static DEVICE_ATTR_RW(xbox_controller);

/**
 * ally_set_vibration_intensity() - Set vibration intensity values
 * @ally: ally handheld structure
 * @hdev: HID device
 * @left: Left motor intensity (0-100)
 * @right: Right motor intensity (0-100)
 *
 * Return: 0 on success, negative errno on failure
 */
static int ally_set_vibration_intensity(struct ally_handheld *ally,
					struct hid_device *hdev, u8 left, u8 right)
{
	const u8 data[] = { left, right };
	int ret;

	u8 *buf __free(kfree) = ally_alloc_cmd(CMD_SET_VIBRATION_INTENSITY, data, sizeof(data));
	if (!buf)
		return -ENOMEM;

	ret = ally_gamepad_send_packet(ally, hdev, buf, ROG_ALLY_REPORT_SIZE);
	if (ret < 0) {
		hid_err(hdev, "Failed to set vibration intensity: %d\n", ret);
		return ret;
	}

	return 0;
}

static ssize_t left_vibration_intensity_show(struct device *dev, struct device_attribute *attr,
					     char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	struct ally_handheld *ally = drvdata->rog_ally;
	struct ally_config *cfg;

	if (!ally)
		return -ENODEV;

	cfg = ally_get_config(ally);
	if (!cfg)
		return -ENODEV;

	guard(mutex)(&cfg->config_mutex);

	return sysfs_emit(buf, "%u\n", cfg->vibration_intensity_left);
}

static ssize_t left_vibration_intensity_store(struct device *dev, struct device_attribute *attr,
					      const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	struct ally_handheld *ally = drvdata->rog_ally;
	struct ally_config *cfg;
	u8 value;
	int ret;

	if (!ally)
		return -ENODEV;

	cfg = ally_get_config(ally);
	if (!cfg)
		return -ENODEV;

	ret = kstrtou8(buf, 10, &value);
	if (ret || value > 100)
		return -EINVAL;

	guard(mutex)(&cfg->config_mutex);

	ret = ally_set_vibration_intensity(ally, hdev, value,
					   cfg->vibration_intensity_right);
	if (ret < 0)
		return ret;

	cfg->vibration_intensity_left = value;

	return count;
}

static ssize_t left_vibration_intensity_range_show(struct device *dev,
						   struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "0 100\n");
}

static ssize_t right_vibration_intensity_show(struct device *dev, struct device_attribute *attr,
					      char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	struct ally_handheld *ally = drvdata->rog_ally;
	struct ally_config *cfg;

	if (!ally)
		return -ENODEV;

	cfg = ally_get_config(ally);
	if (!cfg)
		return -ENODEV;

	guard(mutex)(&cfg->config_mutex);

	return sysfs_emit(buf, "%u\n", cfg->vibration_intensity_right);
}

static ssize_t right_vibration_intensity_store(struct device *dev, struct device_attribute *attr,
					       const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	struct ally_handheld *ally = drvdata->rog_ally;
	struct ally_config *cfg;
	u8 value;
	int ret;

	if (!ally)
		return -ENODEV;

	cfg = ally_get_config(ally);
	if (!cfg)
		return -ENODEV;

	ret = kstrtou8(buf, 10, &value);
	if (ret || value > 100)
		return -EINVAL;

	guard(mutex)(&cfg->config_mutex);

	ret = ally_set_vibration_intensity(ally, hdev,
					   cfg->vibration_intensity_left, value);
	if (ret < 0)
		return ret;

	cfg->vibration_intensity_right = value;

	return count;
}

static ssize_t right_vibration_intensity_range_show(struct device *dev,
						    struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "0 100\n");
}

static struct device_attribute dev_attr_left_vibration_intensity =
	__ATTR(intensity, 0644, left_vibration_intensity_show, left_vibration_intensity_store);

static struct device_attribute dev_attr_left_vibration_intensity_range =
	__ATTR(intensity_range, 0444, left_vibration_intensity_range_show, NULL);

static struct device_attribute dev_attr_right_vibration_intensity =
	__ATTR(intensity, 0644, right_vibration_intensity_show, right_vibration_intensity_store);

static struct device_attribute dev_attr_right_vibration_intensity_range =
	__ATTR(intensity_range, 0444, right_vibration_intensity_range_show, NULL);

/**
 * ally_set_joystick_thresholds() - Generic function to set joystick ranges
 * @ally: ally handheld structure
 * @hdev: HID device
 * @cfg: ally config
 * @left_it: inner threshold (deadzone) of the left stick (0-50)
 * @left_ot: outer threshold of the left stick (70-100)
 * @right_it: inner threshold (deadzone) of the right stick (0-50)
 * @right_ot: outer threshold of the right stick (70-100)
 *
 * This function sends the command to set both inner and outer threshold
 * for the left and right joysticks.
 *
 * Return: 0 on success, negative errno on failure
 */
static int ally_set_joystick_thresholds(struct ally_handheld *ally,
					struct hid_device *hdev, struct ally_config *cfg,
					u8 left_it, u8 left_ot, u8 right_it, u8 right_ot)
{
	const u8 payload[] = { left_it, left_ot, right_it, right_ot };
	int ret;

	u8 *buf __free(kfree) = ally_alloc_cmd(CMD_SET_JOYSTICK_DEADZONE, payload, sizeof(payload));
	if (!buf)
		return -ENOMEM;

	ret = ally_gamepad_send_packet(ally, hdev, buf, ROG_ALLY_REPORT_SIZE);
	if (ret < 0) {
		hid_err(hdev, "Failed to set joystick ranges: %d\n", ret);
		return ret;
	}

	return 0;
}

static ssize_t left_joystick_inner_threshold_show(struct device *dev, struct device_attribute *attr,
						  char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	struct ally_handheld *const ally = drvdata->rog_ally;
	struct ally_config *cfg;

	if (!ally)
		return -ENODEV;

	cfg = ally_get_config(ally);
	if (!cfg)
		return -ENODEV;

	guard(mutex)(&cfg->config_mutex);

	if (!cfg->user_cal_support)
		return -EOPNOTSUPP;

	return sysfs_emit(buf, "%u\n", cfg->left_deadzone);
}

static ssize_t left_joystick_inner_threshold_store(struct device *dev,
						   struct device_attribute *attr,
						   const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	struct ally_handheld *const ally = drvdata->rog_ally;
	struct ally_config *cfg;
	u8 value;
	int ret;

	if (!ally)
		return -ENODEV;

	cfg = ally_get_config(ally);
	if (!cfg)
		return -ENODEV;

	ret = kstrtou8(buf, 10, &value);
	if (ret || value > 50)
		return -EINVAL;

	guard(mutex)(&cfg->config_mutex);

	if (!cfg->user_cal_support)
		return -EOPNOTSUPP;

	ret = ally_set_joystick_thresholds(ally, hdev, cfg,
					   value,
					   cfg->left_outer_threshold,
					   cfg->right_deadzone,
					   cfg->right_outer_threshold);
	if (ret)
		return ret;

	cfg->left_deadzone = value;

	return count;
}

static ssize_t left_joystick_inner_threshold_range_show(struct device *dev,
							struct device_attribute *attr,
							char *buf)
{
	return sysfs_emit(buf, "0 50\n");
}

static ssize_t left_joystick_outer_threshold_show(struct device *dev, struct device_attribute *attr,
						  char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	struct ally_handheld *const ally = drvdata->rog_ally;
	struct ally_config *cfg;

	if (!ally)
		return -ENODEV;

	cfg = ally_get_config(ally);
	if (!cfg)
		return -ENODEV;

	guard(mutex)(&cfg->config_mutex);

	if (!cfg->user_cal_support)
		return -EOPNOTSUPP;

	return sysfs_emit(buf, "%u\n", cfg->left_outer_threshold);
}

static ssize_t left_joystick_outer_threshold_store(struct device *dev,
						   struct device_attribute *attr,
						   const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	struct ally_handheld *const ally = drvdata->rog_ally;
	struct ally_config *cfg;
	u8 value;
	int ret;

	if (!ally)
		return -ENODEV;

	cfg = ally_get_config(ally);
	if (!cfg)
		return -ENODEV;

	ret = kstrtou8(buf, 10, &value);
	if (ret || value < 70 || value > 100)
		return -EINVAL;

	guard(mutex)(&cfg->config_mutex);

	if (!cfg->user_cal_support)
		return -EOPNOTSUPP;

	ret = ally_set_joystick_thresholds(ally, hdev, cfg,
					   cfg->left_deadzone,
					   value,
					   cfg->right_deadzone,
					   cfg->right_outer_threshold);
	if (ret)
		return ret;

	cfg->left_outer_threshold = value;

	return count;
}

static ssize_t left_joystick_outer_threshold_range_show(struct device *dev,
							struct device_attribute *attr,
							char *buf)
{
	return sysfs_emit(buf, "70 100\n");
}

static ssize_t right_joystick_inner_threshold_show(struct device *dev,
						   struct device_attribute *attr,
						   char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	struct ally_handheld *const ally = drvdata->rog_ally;
	struct ally_config *cfg;

	if (!ally)
		return -ENODEV;

	cfg = ally_get_config(ally);
	if (!cfg)
		return -ENODEV;

	guard(mutex)(&cfg->config_mutex);

	if (!cfg->user_cal_support)
		return -EOPNOTSUPP;

	return sysfs_emit(buf, "%u\n", cfg->right_deadzone);
}

static ssize_t right_joystick_inner_threshold_store(struct device *dev,
						    struct device_attribute *attr,
						    const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	struct ally_handheld *const ally = drvdata->rog_ally;
	struct ally_config *cfg;
	u8 value;
	int ret;

	if (!ally)
		return -ENODEV;

	cfg = ally_get_config(ally);
	if (!cfg)
		return -ENODEV;

	ret = kstrtou8(buf, 10, &value);
	if (ret || value > 50)
		return -EINVAL;

	guard(mutex)(&cfg->config_mutex);

	if (!cfg->user_cal_support)
		return -EOPNOTSUPP;

	ret = ally_set_joystick_thresholds(ally, hdev, cfg,
					   cfg->left_deadzone,
					   cfg->left_outer_threshold,
					   value,
					   cfg->right_outer_threshold);
	if (ret)
		return ret;

	cfg->right_deadzone = value;

	return count;
}

static ssize_t right_joystick_inner_threshold_range_show(struct device *dev,
							 struct device_attribute *attr,
							 char *buf)
{
	return sysfs_emit(buf, "0 50\n");
}

static ssize_t right_joystick_outer_threshold_show(struct device *dev,
						   struct device_attribute *attr,
						   char *buf)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	struct ally_handheld *const ally = drvdata->rog_ally;
	struct ally_config *cfg;

	if (!ally)
		return -ENODEV;

	cfg = ally_get_config(ally);
	if (!cfg)
		return -ENODEV;

	guard(mutex)(&cfg->config_mutex);

	if (!cfg->user_cal_support)
		return -EOPNOTSUPP;

	return sysfs_emit(buf, "%u\n", cfg->right_outer_threshold);
}

static ssize_t right_joystick_outer_threshold_store(struct device *dev,
						    struct device_attribute *attr,
						    const char *buf, size_t count)
{
	struct hid_device *hdev = to_hid_device(dev);
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	struct ally_handheld *const ally = drvdata->rog_ally;
	struct ally_config *cfg;
	u8 value;
	int ret;

	if (!ally)
		return -ENODEV;

	cfg = ally_get_config(ally);
	if (!cfg)
		return -ENODEV;

	ret = kstrtou8(buf, 10, &value);
	if (ret || value < 70 || value > 100)
		return -EINVAL;

	guard(mutex)(&cfg->config_mutex);

	if (!cfg->user_cal_support)
		return -EOPNOTSUPP;

	ret = ally_set_joystick_thresholds(ally, hdev, cfg,
					   cfg->left_deadzone,
					   cfg->left_outer_threshold,
					   cfg->right_deadzone,
					   value);
	if (ret)
		return ret;

	cfg->right_outer_threshold = value;

	return count;
}

static ssize_t right_joystick_outer_threshold_range_show(struct device *dev,
							 struct device_attribute *attr,
							 char *buf)
{
	return sysfs_emit(buf, "70 100\n");
}

static struct device_attribute dev_attr_left_joystick_inner_threshold =
	__ATTR(inner_threshold, 0644, left_joystick_inner_threshold_show,
	       left_joystick_inner_threshold_store);

static struct device_attribute dev_attr_left_joystick_inner_threshold_range =
	__ATTR(inner_threshold_range, 0444, left_joystick_inner_threshold_range_show, NULL);

static struct device_attribute dev_attr_left_joystick_outer_threshold =
	__ATTR(outer_threshold, 0644, left_joystick_outer_threshold_show,
	       left_joystick_outer_threshold_store);

static struct device_attribute dev_attr_left_joystick_outer_threshold_range =
	__ATTR(outer_threshold_range, 0444, left_joystick_outer_threshold_range_show, NULL);

static struct device_attribute dev_attr_right_joystick_inner_threshold =
	__ATTR(inner_threshold, 0644, right_joystick_inner_threshold_show,
	       right_joystick_inner_threshold_store);

static struct device_attribute dev_attr_right_joystick_inner_threshold_range =
	__ATTR(inner_threshold_range, 0444, right_joystick_inner_threshold_range_show, NULL);

static struct device_attribute dev_attr_right_joystick_outer_threshold =
	__ATTR(outer_threshold, 0644, right_joystick_outer_threshold_show,
	       right_joystick_outer_threshold_store);

static struct device_attribute dev_attr_right_joystick_outer_threshold_range =
	__ATTR(outer_threshold_range, 0444, right_joystick_outer_threshold_range_show, NULL);

static struct attribute *ally_config_attrs[] = {
	&dev_attr_xbox_controller.attr,
	NULL
};

static struct attribute *ally_left_vibration_attrs[] = {
	&dev_attr_left_vibration_intensity.attr,
	&dev_attr_left_vibration_intensity_range.attr,
	NULL
};

static struct attribute *ally_right_vibration_attrs[] = {
	&dev_attr_right_vibration_intensity.attr,
	&dev_attr_right_vibration_intensity_range.attr,
	NULL
};

static struct attribute *left_joystick_axis_attrs[] = {
	&dev_attr_left_joystick_inner_threshold.attr,
	&dev_attr_left_joystick_outer_threshold.attr,
	&dev_attr_left_joystick_inner_threshold_range.attr,
	&dev_attr_left_joystick_outer_threshold_range.attr,
	NULL
};

static struct attribute *right_joystick_axis_attrs[] = {
	&dev_attr_right_joystick_inner_threshold.attr,
	&dev_attr_right_joystick_outer_threshold.attr,
	&dev_attr_right_joystick_inner_threshold_range.attr,
	&dev_attr_right_joystick_outer_threshold_range.attr,
	NULL
};

static const struct attribute_group ally_attr_groups[] = {
	{
		.attrs = ally_config_attrs,
	},
	{
		.name = "left_vibration",
		.attrs = ally_left_vibration_attrs,
	},
	{
		.name = "right_vibration",
		.attrs = ally_right_vibration_attrs,
	},
};

/*
 * The joystick range calibration attributes are tied to the
 * user-calibration capability: their sysfs groups are registered only
 * when the device supports configuring those parameters, and the show
 * and store callbacks reject accesses with -EOPNOTSUPP regardless, in
 * case the groups are registered for another supported feature.
 */
static const struct attribute_group ally_left_joystick_axis_group = {
	.name = "left_joystick_axis",
	.attrs = left_joystick_axis_attrs,
};

static const struct attribute_group ally_right_joystick_axis_group = {
	.name = "right_joystick_axis",
	.attrs = right_joystick_axis_attrs,
};

static const struct attribute_group *const ally_cal_attr_groups[] = {
	&ally_left_joystick_axis_group,
	&ally_right_joystick_axis_group,
};

/**
 * ally_config_create() - Initialize configuration and create sysfs entries
 * @hdev: HID device
 * @ally: non-NULL ally device data with uninitialized config pointer
 *
 * Return: valid pointer on success, error pointer on failure
 */
static struct ally_config *ally_config_create(struct hid_device *hdev, struct ally_handheld *ally)
{
	struct ally_config *cfg;
	int ret, sysfs_i;

	cfg = devm_kzalloc(&hdev->dev, sizeof(*cfg), GFP_KERNEL);
	if (!cfg)
		return ERR_PTR(-ENOMEM);

	mutex_init(&cfg->config_mutex);

	ret = ally_detect_capabilities(hdev, ally, cfg);
	if (ret < 0) {
		hid_err(hdev, "Failed to detect Ally capabilities: %d\n", ret);
		goto ally_config_create_err;
	}

	for (sysfs_i = 0; sysfs_i < ARRAY_SIZE(ally_attr_groups); sysfs_i++) {
		ret = devm_device_add_group(&hdev->dev, &ally_attr_groups[sysfs_i]);
		if (ret < 0) {
			hid_err(hdev, "Failed to create sysfs group '%s': %d\n",
				ally_attr_groups[sysfs_i].name ?: "", ret);
			goto ally_config_create_sysfs_err;
		}
	}

	/* Skip the calibration groups when the capability is missing. */
	if (cfg->user_cal_support) {
		for (sysfs_i = 0; sysfs_i < ARRAY_SIZE(ally_cal_attr_groups); sysfs_i++) {
			ret = devm_device_add_group(&hdev->dev,
						    ally_cal_attr_groups[sysfs_i]);
			if (ret < 0) {
				hid_err(hdev, "Failed to create sysfs group '%s': %d\n",
					ally_cal_attr_groups[sysfs_i]->name, ret);
				goto ally_config_create_sysfs_err;
			}
		}
	}

	cfg->gamepad_mode = 0x01;
	cfg->left_deadzone = 10;
	cfg->left_outer_threshold = 90;
	cfg->right_deadzone = 10;
	cfg->right_outer_threshold = 90;
	cfg->vibration_intensity_left = 100;
	cfg->vibration_intensity_right = 100;

	/* So far the only hardware this is supported is the Ally 1 */
	if (cfg->xbox_controller_support) {
		ret = ally_set_xbox_controller(hdev, ally, cfg, true);
		if (ret < 0)
			hid_warn(hdev, "Failed to set default Xbox controller mode: %d\n",
				ret);
	}

	cfg->initialized = true;

	return cfg;
ally_config_create_sysfs_err:
ally_config_create_err:
	devm_kfree(&hdev->dev, cfg);
	return ERR_PTR(ret);
}

/**
 * ally_config_remove() - Clean up configuration resources
 * @hdev: HID device
 * @ally: Non-NULL Ally device data
 */
static void ally_config_remove(struct hid_device *hdev, struct ally_handheld *ally)
{
	struct ally_config *cfg = ally->config;

	if (!cfg || !cfg->initialized)
		return;
}

/**
 * ally_gamepad_check_ready() - Wait for the gamepad MCU to report ready
 * @ally: ally handheld structure
 * @hdev: HID device
 *
 * This should be called before any remapping attempts, and on driver
 * init/resume, after the asus handshake has been performed on the
 * configuration endpoint.
 *
 * Return: 0 on success, negative errno on failure
 */
static int ally_gamepad_check_ready(struct ally_handheld *ally, struct hid_device *hdev)
{
	u8 payload[] = { 0x00 };
	int ret;

	for (int i = 0; i < HID_ALLY_READY_MAX_TRIES; i++) {
		u8 *buf __free(kfree) = ally_alloc_cmd(CMD_CHECK_READY, payload, sizeof(payload));
		if (!buf)
			return -ENOMEM;

		ret = ally_gamepad_send_receive_packet(ally, hdev, buf, ROG_ALLY_REPORT_SIZE);
		if (ret < 0) {
			hid_dbg(hdev, "ROG Ally check %d/%d failed: %d\n", i,
				HID_ALLY_READY_MAX_TRIES, ret);
			continue;
		}

		if (buf[2] == CMD_CHECK_READY)
			return 0;

		usleep_range(1000, 2000);
	}

	hid_err(hdev, "ROG Ally never responded with a ready\n");
	return -ENODEV;
}

static int ally_get_endpoint_address(struct hid_device *hdev)
{
	struct usb_host_endpoint *ep;
	struct usb_interface *intf;

	if (!hid_is_usb(hdev))
		return -ENODEV;

	intf = to_usb_interface(hdev->dev.parent);
	if (!intf || !intf->cur_altsetting)
		return -ENODEV;

	ep = intf->cur_altsetting->endpoint;
	if (!ep)
		return -ENODEV;

	return ep->desc.bEndpointAddress;
}

struct ally_x_input_report {
	__le16 x, y;
	__le16 rx, ry;
	__le16 z, rz;
	u8 buttons[3];
} __packed;

/* The hatswitch outputs integers, we use them to index this X|Y pair */
static const int hat_values[][2] = {
	{ 0, 0 }, { 0, -1 }, { 1, -1 }, { 1, 0 },   { 1, 1 },
	{ 0, 1 }, { -1, 1 }, { -1, 0 }, { -1, -1 },
};

/**
 * ally_x_raw_event() - Parse and report an Ally X gamepad input report
 * @input: input device to report the events on
 * @hdev: HID device
 * @report: HID report the raw data arrived in
 * @data: raw report buffer
 * @size: length of @data in bytes
 *
 * Return: true if the event was handled, otherwise false
 */
static bool ally_x_raw_event(struct input_dev *input, struct hid_device *hdev,
			     struct hid_report *report, u8 *data, int size)
{
	struct ally_x_input_report *in_report;
	u16 x, y, rx, ry, z, rz;
	u8 byte;

	if (!input)
		return false;

	if (size < 1)
		return false;

	/*
	 * The size check above only guarantees one valid byte: do not
	 * read the code of a truncated report.
	 */
	if (data[0] == 0x5A) {
		if (size < 2)
			return false;

		input_report_key(input, KEY_PROG1, data[1] == 0x38);
		input_report_key(input, KEY_F16, data[1] == 0xA6);
		input_report_key(input, KEY_F17, data[1] == 0xA7);
		input_report_key(input, KEY_F18, data[1] == 0xA8);
		input_sync(input);

		return data[1] == 0xA6 || data[1] == 0xA7 || data[1] == 0xA8 || data[1] == 0x38;
	}

	if (data[0] != HID_ALLY_X_INPUT_REPORT)
		return false;

	/*
	 * hid-core only guarantees size >= 1 and does not zero-pad short
	 * reports before ->raw_event, so a truncated transfer would leave the
	 * payload below pointing at stale DMA buffer contents.
	 */
	if (size < 1 + sizeof(*in_report))
		return false;

	in_report = (struct ally_x_input_report *)&data[1];

	/* USB HID payloads are little-endian: convert them explicitly. */
	x = get_unaligned_le16(&in_report->x);
	y = get_unaligned_le16(&in_report->y);
	rx = get_unaligned_le16(&in_report->rx);
	ry = get_unaligned_le16(&in_report->ry);
	z = get_unaligned_le16(&in_report->z);
	rz = get_unaligned_le16(&in_report->rz);

	input_report_abs(input, ABS_X, x - 32768);
	input_report_abs(input, ABS_Y, y - 32768);
	input_report_abs(input, ABS_RX, rx - 32768);
	input_report_abs(input, ABS_RY, ry - 32768);
	input_report_abs(input, ABS_Z, z);
	input_report_abs(input, ABS_RZ, rz);

	byte = in_report->buttons[0];
	input_report_key(input, BTN_A, byte & BIT(0));
	input_report_key(input, BTN_B, byte & BIT(1));
	input_report_key(input, BTN_X, byte & BIT(2));
	input_report_key(input, BTN_Y, byte & BIT(3));
	input_report_key(input, BTN_TL, byte & BIT(4));
	input_report_key(input, BTN_TR, byte & BIT(5));
	input_report_key(input, BTN_SELECT, byte & BIT(6));
	input_report_key(input, BTN_START, byte & BIT(7));

	byte = in_report->buttons[1];
	input_report_key(input, BTN_THUMBL, byte & BIT(0));
	input_report_key(input, BTN_THUMBR, byte & BIT(1));
	input_report_key(input, BTN_MODE, byte & BIT(2));

	/* The hatswitch byte is device-controlled; treat anything the table
	 * does not cover as centred rather than indexing out of bounds.
	 */
	byte = in_report->buttons[2];
	if (byte >= ARRAY_SIZE(hat_values))
		byte = 0;
	input_report_abs(input, ABS_HAT0X, hat_values[byte][0]);
	input_report_abs(input, ABS_HAT0Y, hat_values[byte][1]);

	input_sync(input);

	return true;
}

static int ally_x_input_open(struct input_dev *dev)
{
	struct hid_device *hdev = input_get_drvdata(dev);

	return hid_hw_open(hdev);
}

static void ally_x_input_close(struct input_dev *dev)
{
	hid_hw_close(input_get_drvdata(dev));
}

static struct input_dev *ally_x_alloc_input_dev(struct hid_device *hdev)
{
	struct input_dev *input_dev = devm_input_allocate_device(&hdev->dev);

	if (!input_dev)
		return ERR_PTR(-ENOMEM);

	input_dev->id.bustype = hdev->bus;
	input_dev->id.vendor = hdev->vendor;
	input_dev->id.product = hdev->product;
	input_dev->id.version = hdev->version;
	input_dev->uniq = hdev->uniq;
	input_dev->name = "ASUS ROG Ally X Gamepad";

	input_set_drvdata(input_dev, hdev);
	/*
	 * Let the input core hold the hardware open while the device node
	 * is in use: without these the interface could be runtime-suspended
	 * and drop events.
	 */
	input_dev->open = ally_x_input_open;
	input_dev->close = ally_x_input_close;

	return input_dev;
}

static int ally_x_setup_input(struct hid_device *hdev, struct ally_handheld *ally)
{
	struct input_dev *input = ally_x_alloc_input_dev(hdev);
	unsigned long flags;
	int ret;

	if (IS_ERR(input))
		return PTR_ERR(input);

	input_set_abs_params(input, ABS_X, -32768, 32767, 0, 0);
	input_set_abs_params(input, ABS_Y, -32768, 32767, 0, 0);
	input_set_abs_params(input, ABS_RX, -32768, 32767, 0, 0);
	input_set_abs_params(input, ABS_RY, -32768, 32767, 0, 0);
	input_set_abs_params(input, ABS_Z, 0, 1023, 0, 0);
	input_set_abs_params(input, ABS_RZ, 0, 1023, 0, 0);
	input_set_abs_params(input, ABS_HAT0X, -1, 1, 0, 0);
	input_set_abs_params(input, ABS_HAT0Y, -1, 1, 0, 0);
	input_set_capability(input, EV_KEY, BTN_A);
	input_set_capability(input, EV_KEY, BTN_B);
	input_set_capability(input, EV_KEY, BTN_X);
	input_set_capability(input, EV_KEY, BTN_Y);
	input_set_capability(input, EV_KEY, BTN_TL);
	input_set_capability(input, EV_KEY, BTN_TR);
	input_set_capability(input, EV_KEY, BTN_SELECT);
	input_set_capability(input, EV_KEY, BTN_START);
	input_set_capability(input, EV_KEY, BTN_MODE);
	input_set_capability(input, EV_KEY, BTN_THUMBL);
	input_set_capability(input, EV_KEY, BTN_THUMBR);

	input_set_capability(input, EV_KEY, KEY_PROG1);
	input_set_capability(input, EV_KEY, KEY_F16);
	input_set_capability(input, EV_KEY, KEY_F17);
	input_set_capability(input, EV_KEY, KEY_F18);
	input_set_capability(input, EV_KEY, BTN_TRIGGER_HAPPY);
	input_set_capability(input, EV_KEY, BTN_TRIGGER_HAPPY1);

	ret = input_register_device(input);
	if (ret) {
		hid_err(hdev, "Failed to register Ally X gamepad device: %d\n", ret);
		goto ally_x_setup_input_err;
	}

	/* Publish the input_dev only when it is fully set up. */
	spin_lock_irqsave(&ally_data_lock, flags);
	ally->ally_x_input = input;
	spin_unlock_irqrestore(&ally_data_lock, flags);

	return 0;
ally_x_setup_input_err:
	return ret;
}

static int hid_asus_ally_init(struct hid_device *hdev, struct ally_handheld *ally)
{
	int ret;
	struct ally_config *cfg;

	/* Failure at this point is non-critical */
	ret = ally_gamepad_send_packet(ally, hdev, ALLY_FORCE_FEEDBACK_OFF,
				       sizeof(ALLY_FORCE_FEEDBACK_OFF));
	if (ret < 0)
		hid_err(hdev, "Ally failed to init force-feedback off: %d\n", ret);

	cfg = ally_get_config(ally);
	if (!cfg)
		return 0;

	/*
	 * The MCU may have just been reset: restore the cached Xbox
	 * controller mode, or the hardware would silently revert to its
	 * default while the sysfs state still reports the user choice.
	 */
	guard(mutex)(&cfg->config_mutex);

	if (cfg->xbox_controller_enabled) {
		ret = ally_set_xbox_controller(hdev, ally, cfg, true);
		if (ret < 0)
			hid_warn(hdev, "Failed to restore Xbox controller mode: %d\n",
				 ret);
	}

	return 0;
}

/**
 * hid_asus_ally_raw_event() - Route raw reports from the Ally interfaces
 * @hdev: HID device
 * @ally: ally handheld structure
 * @report: HID report the raw data arrived in
 * @data: raw report buffer
 * @size: length of @data in bytes
 *
 * Called from the raw_event callback, which may run in atomic (URB
 * completion) context: only spinlock-protected accesses to the shared
 * state are allowed here.
 *
 * Return: true if the event was handled, otherwise false
 */
static bool hid_asus_ally_raw_event(struct hid_device *hdev, struct ally_handheld *ally,
			    struct hid_report *report, u8 *data, int size)
{
	struct input_dev *x_input;
	struct hid_device *x_hdev;
	unsigned long flags;
	bool handled;

	if (!ally)
		return false;

	switch (ally_get_endpoint_address(hdev)) {
	case HID_ALLY_X_INTF_IN:
		/*
		 * Take a reference on the input_dev while using it: the
		 * gamepad interface can be unbound concurrently, and its
		 * input_dev is freed with it.
		 */
		spin_lock_irqsave(&ally_data_lock, flags);
		x_input = input_get_device(ally->ally_x_input);
		x_hdev = ally->ally_x_hdev;
		spin_unlock_irqrestore(&ally_data_lock, flags);

		handled = ally_x_raw_event(x_input, x_hdev, report, data, size);
		input_put_device(x_input);
		if (handled)
			return true;
		break;
	case HID_ALLY_INTF_CFG_IN:
		if (handle_ally_event(hdev, ally, data, size))
			return true;
		break;
	case HID_ALLY_INTF_KEYBOARD_IN:
		if (handle_ctrl_alt_del(hdev, ally, data, size))
			return false;
		break;
	default:
		break;
	}

	return false;
}

/**
 * hid_asus_ally_probe() - Initialize the ROG Ally HID extension
 * @hdev: HID device
 *
 * This module works alongside the main Asus HID driver to handle
 * Ally-specific features and quirks.
 *
 * Return: an ally_handheld struct pointer on success, an ERR_PTR on
 * failure. The caller is not expected to use the returned pointer, but it
 * should check for errors by using IS_ERR and PTR_ERR and pass NULL to
 * other functions if there was an error.
 */
static struct ally_handheld *hid_asus_ally_probe(struct hid_device *hdev)
{
	unsigned long flags;
	int ret, ep = ally_get_endpoint_address(hdev);
	struct usb_device *udev;
	struct ally_config *ally_cfg;
	struct hid_input *hidinput;

	if (ep < 0)
		return ERR_PTR(ep);

	/*
	 * The ROG Ally controller is integrated into a handheld PC, so at
	 * most one device can exist and the shared global state relies on
	 * that: reject a probe from a different USB device, or a spoofed
	 * peripheral could overwrite the state of the real controller. The
	 * recorded device is dropped when the last interface of the
	 * controller is unbound, so the re-enumeration the embedded
	 * controller performs after a suspend cycle (it cuts the controller
	 * power when powersave is enabled) is still accepted.
	 */
	udev = interface_to_usbdev(to_usb_interface(hdev->dev.parent));

	spin_lock_irqsave(&ally_data_lock, flags);
	if (ally_drvdata.udev && ally_drvdata.udev != udev) {
		spin_unlock_irqrestore(&ally_data_lock, flags);
		hid_err(hdev, "A ROG Ally controller is already connected\n");
		return ERR_PTR(-ENODEV);
	}
	if (!ally_drvdata.udev) {
		usb_get_dev(udev);
		ally_drvdata.udev = udev;
	}
	spin_unlock_irqrestore(&ally_data_lock, flags);

	/*
	 * The interface initialization sleeps (it performs USB transfers),
	 * so it must run before taking the spinlock guarding the shared
	 * state; pointers are published only once it succeeded.
	 */
	switch (ep) {
	case HID_ALLY_INTF_CFG_IN:
		/*
		 * This function assumes the asus-specific initialization
		 * to have been performed already at this point.
		 */
		ret = ally_gamepad_check_ready(&ally_drvdata, hdev);
		if (ret < 0) {
			hid_err(hdev, "ROG Ally device is not ready: %d\n", ret);
			ally_put_udev_if_orphaned();
			return ERR_PTR(ret);
		}

		ally_cfg = ally_config_create(hdev, &ally_drvdata);
		if (IS_ERR(ally_cfg)) {
			hid_err(hdev, "Failed to create Ally cfg: %ld\n",
				PTR_ERR(ally_cfg));
			ally_put_udev_if_orphaned();
			return ERR_PTR(PTR_ERR(ally_cfg));
		}

		ret = hid_asus_ally_init(hdev, &ally_drvdata);
		if (ret < 0) {
			ally_config_remove(hdev, &ally_drvdata);
			ally_put_udev_if_orphaned();
			return ERR_PTR(ret);
		}

		spin_lock_irqsave(&ally_data_lock, flags);
		ally_drvdata.config = ally_cfg;
		ally_drvdata.cfg_hdev = hdev;
		spin_unlock_irqrestore(&ally_data_lock, flags);
		break;
	case HID_ALLY_X_INTF_IN:
		/* This will create and populate ally_x_input */
		ret = ally_x_setup_input(hdev, &ally_drvdata);
		if (ret) {
			hid_err(hdev, "Failed to create Ally X gamepad device.\n");
			ally_put_udev_if_orphaned();
			return ERR_PTR(ret);
		}

		spin_lock_irqsave(&ally_data_lock, flags);
		ally_drvdata.ally_x_hdev = hdev;
		spin_unlock_irqrestore(&ally_data_lock, flags);
		break;
	case HID_ALLY_INTF_KEYBOARD_IN:
		spin_lock_irqsave(&ally_data_lock, flags);
		ally_drvdata.keyboard_hdev = hdev;
		if (!list_empty(&hdev->inputs)) {
			hidinput = list_first_entry(&hdev->inputs, struct hid_input, list);
			ally_drvdata.keyboard_input = hidinput->input;
		}
		spin_unlock_irqrestore(&ally_data_lock, flags);
		break;
	default:
		/* This is normally supposed to happen */
		break;
	}

	return &ally_drvdata;
}

static void hid_asus_ally_remove(struct hid_device *hdev, struct ally_handheld *ally)
{
	unsigned long flags;

	if (!ally)
		return;

	/*
	 * Any of the three interfaces can own an input_dev the resume work
	 * reports through, and they are torn down in an arbitrary order, so
	 * drain it before clearing anything. Cancel outside ally_data_lock so
	 * a handler that wants the lock cannot deadlock against us.
	 */
	cancel_delayed_work_sync(&ally->resume_work);

	spin_lock_irqsave(&ally_data_lock, flags);
	if (ally->ally_x_hdev == hdev) {
		ally->ally_x_input = NULL;
		ally->ally_x_hdev = NULL;
	}

	/*
	 * The keyboard interface is torn down before the config one, and
	 * its input_dev is freed with it. handle_ally_event() and
	 * ally_resume_work_fn() both report keys through it from the
	 * config endpoint, so drop the references here or they dangle.
	 */
	if (ally->keyboard_hdev == hdev) {
		ally->keyboard_input = NULL;
		ally->keyboard_hdev = NULL;
	}

	if (ally->cfg_hdev == hdev) {
		ally_config_remove(hdev, ally);
		ally->cfg_hdev = NULL;
		ally->config = NULL;
	}
	spin_unlock_irqrestore(&ally_data_lock, flags);

	/*
	 * Drop the recorded USB device when the last interface of the
	 * controller has been unbound: the driver is then ready to accept
	 * the controller again when it reconnects.
	 */
	ally_put_udev_if_orphaned();
}

static int hid_asus_ally_reset_resume(struct hid_device *hdev, struct ally_handheld *ally)
{
	int ep, ret;

	/*
	 * The extensions failed to probe and the device is operating as a
	 * generic HID device: do not fail the resume because of that.
	 */
	if (!ally)
		return 0;

	ep = ally_get_endpoint_address(hdev);
	if (ep != HID_ALLY_INTF_CFG_IN)
		return 0;

	/*
	 * This function assumes the asus-specific initialization
	 * to have been performed already at this point.
	 */
	ret = ally_gamepad_check_ready(ally, hdev);
	if (ret < 0) {
		hid_err(hdev, "ROG Ally device is not ready: %d\n", ret);
		return ret;
	}

	ret = hid_asus_ally_init(hdev, ally);
	if (ret < 0)
		return ret;

	return 0;
}

/*
 * Send events to asus-wmi driver for handling special keys
 */
static int asus_wmi_send_event(struct asus_drvdata *drvdata, u8 code)
{
	int err;
	u32 retval;

	err = asus_wmi_evaluate_method(ASUS_WMI_METHODID_DEVS,
				       ASUS_WMI_METHODID_NOTIF, code, &retval);
	if (err) {
		pr_warn("Failed to notify asus-wmi: %d\n", err);
		return err;
	}

	if (retval != 0) {
		pr_warn("Failed to notify asus-wmi (retval): 0x%x\n", retval);
		return -EIO;
	}

	return 0;
}

static void asus_report_contact_down(struct asus_drvdata *drvdat,
		int toolType, u8 *data)
{
	struct input_dev *input = drvdat->input;
	int touch_major, pressure, x, y;

	x = (data[0] & CONTACT_X_MSB_MASK) << 4 | data[1];
	y = drvdat->tp->max_y - ((data[0] & CONTACT_Y_MSB_MASK) << 8 | data[2]);

	input_report_abs(input, ABS_MT_POSITION_X, x);
	input_report_abs(input, ABS_MT_POSITION_Y, y);

	if (drvdat->tp->contact_size < 5)
		return;

	if (toolType == MT_TOOL_PALM) {
		touch_major = MAX_TOUCH_MAJOR;
		pressure = MAX_PRESSURE;
	} else {
		touch_major = (data[3] >> 4) & CONTACT_TOUCH_MAJOR_MASK;
		pressure = data[4] & CONTACT_PRESSURE_MASK;
	}

	input_report_abs(input, ABS_MT_TOUCH_MAJOR, touch_major);
	input_report_abs(input, ABS_MT_PRESSURE, pressure);
}

/* Required for Synaptics Palm Detection */
static void asus_report_tool_width(struct asus_drvdata *drvdat)
{
	struct input_mt *mt = drvdat->input->mt;
	struct input_mt_slot *oldest;
	int oldid, i;

	if (drvdat->tp->contact_size < 5)
		return;

	oldest = NULL;
	oldid = mt->trkid;

	for (i = 0; i < mt->num_slots; ++i) {
		struct input_mt_slot *ps = &mt->slots[i];
		int id = input_mt_get_value(ps, ABS_MT_TRACKING_ID);

		if (id < 0)
			continue;
		if ((id - oldid) & TRKID_SGN) {
			oldest = ps;
			oldid = id;
		}
	}

	if (oldest) {
		input_report_abs(drvdat->input, ABS_TOOL_WIDTH,
			input_mt_get_value(oldest, ABS_MT_TOUCH_MAJOR));
	}
}

static int asus_report_input(struct asus_drvdata *drvdat, u8 *data, int size)
{
	int i, toolType = MT_TOOL_FINGER;
	u8 *contactData = data + 2;

	if (size != drvdat->tp->report_size)
		return 0;

	for (i = 0; i < drvdat->tp->max_contacts; i++) {
		bool down = !!(data[1] & BIT(i+3));

		if (drvdat->tp->contact_size >= 5)
			toolType = contactData[3] & CONTACT_TOOL_TYPE_MASK ?
						MT_TOOL_PALM : MT_TOOL_FINGER;

		input_mt_slot(drvdat->input, i);
		input_mt_report_slot_state(drvdat->input, toolType, down);

		if (down) {
			asus_report_contact_down(drvdat, toolType, contactData);
			contactData += drvdat->tp->contact_size;
		}
	}

	input_report_key(drvdat->input, BTN_LEFT, data[1] & BTN_LEFT_MASK);
	asus_report_tool_width(drvdat);

	input_mt_sync_frame(drvdat->input);
	input_sync(drvdat->input);

	return 1;
}

static int asus_e1239t_event(struct asus_drvdata *drvdat, u8 *data, int size)
{
	if (size != 3)
		return 0;

	/* Handle broken mute key which only sends press events */
	if (!drvdat->tp &&
	    data[0] == 0x02 && data[1] == 0xe2 && data[2] == 0x00) {
		input_report_key(drvdat->input, KEY_MUTE, 1);
		input_sync(drvdat->input);
		input_report_key(drvdat->input, KEY_MUTE, 0);
		input_sync(drvdat->input);
		return 1;
	}

	/* Handle custom touchpad toggle key which only sends press events */
	if (drvdat->tp_kbd_input &&
	    data[0] == 0x05 && data[1] == 0x02 && data[2] == 0x28) {
		input_report_key(drvdat->tp_kbd_input, KEY_F21, 1);
		input_sync(drvdat->tp_kbd_input);
		input_report_key(drvdat->tp_kbd_input, KEY_F21, 0);
		input_sync(drvdat->tp_kbd_input);
		return 1;
	}

	return 0;
}

/*
 * Used in atomic contexts to schedule work involving sleeps operations or
 * asus-wmi interactions.
 *
 * Caller is responsible to store relevant data in the structure to carry out
 * the required action.
 *
 * This function must be called while the spin lock protecting the workqueue
 * is already being held.
 */
static void asus_worker_schedule(struct asus_worker *worker, struct asus_work_action *action)
{
	if (worker->removed) {
		kfree(action);
		return;
	}

	list_add_tail(&action->node, &worker->actions);
	schedule_work(&worker->work);
}

static int asus_kbd_fn_lock_set(struct asus_drvdata *drvdata, bool enabled)
{
	struct asus_work_action *action;
	unsigned long flags;

	action = kzalloc(sizeof(struct asus_work_action), GFP_ATOMIC);
	if (!action)
		return -ENOMEM;

	drvdata->fn_lock = enabled;
	action->type = FN_LOCK_SYNC;
	action->data.fn_lock = drvdata->fn_lock;
	INIT_LIST_HEAD(&action->node);

	spin_lock_irqsave(&drvdata->worker->lock, flags);
	asus_worker_schedule(drvdata->worker, action);
	spin_unlock_irqrestore(&drvdata->worker->lock, flags);

	return 0;
}

static int asus_kbd_wmi_fan_send(struct asus_drvdata *drvdata, u8 *report_data,
				 size_t report_size)
{
	struct asus_work_action *action;
	unsigned long flags;

	if (report_size > FEATURE_KBD_REPORT_SIZE) {
		hid_err(drvdata->hdev, "Invalid report size for fan event: %zu\n", report_size);
		return -EINVAL;
	}

	action = kzalloc(sizeof(struct asus_work_action), GFP_NOWAIT);
	if (!action)
		return -ENOMEM;

	action->type = WMI_FAN;
	action->data.fan_hid_data.report_size = report_size;
	memcpy(action->data.fan_hid_data.report_data, report_data, report_size);
	INIT_LIST_HEAD(&action->node);

	spin_lock_irqsave(&drvdata->worker->lock, flags);
	asus_worker_schedule(drvdata->worker, action);
	spin_unlock_irqrestore(&drvdata->worker->lock, flags);

	return 0;
}

static int asus_event(struct hid_device *hdev, struct hid_field *field,
		      struct hid_usage *usage, __s32 value)
{
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	int ret;

	if ((usage->hid & HID_USAGE_PAGE) == HID_UP_ASUSVENDOR &&
	    (usage->hid & HID_USAGE) != 0x00 &&
	    (usage->hid & HID_USAGE) != 0xff && !usage->type) {
		hid_warn(hdev, "Unmapped Asus vendor usagepage code 0x%02x\n",
			 usage->hid & HID_USAGE);
	}

	if (usage->type == EV_KEY && value) {
		switch (usage->code) {
		case KEY_KBDILLUMUP:
			return !asus_hid_event(ASUS_EV_BRTUP);
		case KEY_KBDILLUMDOWN:
			return !asus_hid_event(ASUS_EV_BRTDOWN);
		case KEY_KBDILLUMTOGGLE:
			return !asus_hid_event(ASUS_EV_BRTTOGGLE);
		case KEY_FN_ESC:
			if (drvdata->quirks & QUIRK_HID_FN_LOCK) {
				ret = asus_kbd_fn_lock_set(drvdata, !drvdata->fn_lock);
				if (ret) {
					hid_err(hdev, "Error while toggling FN lock: %d\n", ret);
					return ret;
				}
			}
			break;
		}
	}

	return 0;
}

static int asus_raw_event(struct hid_device *hdev,
		struct hid_report *report, u8 *data, int size)
{
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	int ret;

	if (size < 2) {
		hid_dbg(hdev, "Unexpected keyboard report size %d\n", size);
		return 0;
	}

	if (drvdata->battery && data[0] == BATTERY_REPORT_ID)
		return asus_report_battery(drvdata, data, size);

	if (drvdata->tp && data[0] == INPUT_REPORT_ID)
		return asus_report_input(drvdata, data, size);

	if (drvdata->quirks & QUIRK_MEDION_E1239T)
		return asus_e1239t_event(drvdata, data, size);

	if (drvdata->quirks & QUIRK_ROG_ALLY_XPAD) {
		/*
		 * Return -1 to suppress further processing by the generic HID
		 * input parser for reports we fully handle for the Gamepad (0x0B):
		 * letting 0x0B reach the default parser creates a generic gamepad
		 * causing Steam Input overlaps (i.e. L1 stuck on screenshot).
		 */
		if (hid_asus_ally_raw_event(hdev, drvdata->rog_ally, report, data, size))
			return -1;
	}

	/*
	 * Skip these report ID, the device emits a continuous stream associated
	 * with the AURA mode it is in which looks like an 'echo'.
	 */
	if (report->id == FEATURE_KBD_LED_REPORT_ID1 || report->id == FEATURE_KBD_LED_REPORT_ID2)
		return -1;
	if (drvdata->quirks & QUIRK_ROG_NKEY_KEYBOARD) {
		if (report->id == FEATURE_KBD_REPORT_ID) {
			/*
			 * Fn+F5 fan control key - try to send WMI event to toggle fan mode.
			 * If successful, block the event from reaching userspace.
			 * If asus-wmi is unavailable or the call fails, let the event
			 * pass to userspace so it can implement its own fan control.
			 */
			if (data[1] == ASUS_FAN_CTRL_KEY_CODE) {
				ret = asus_kbd_wmi_fan_send(drvdata, data, size);

				/* if execution deferred successfully block event */
				if (ret == 0)
					return -1;

				return ret;
			}

			/*
			 * ASUS ROG laptops send these codes during normal operation
			 * with no discernable reason. Filter them out to avoid
			 * unmapped warning messages.
			 */
			if (data[1] == ASUS_SPURIOUS_CODE_0XEA ||
			    data[1] == ASUS_SPURIOUS_CODE_0XEC ||
			    data[1] == ASUS_SPURIOUS_CODE_0X02 ||
			    data[1] == ASUS_SPURIOUS_CODE_0X8A ||
			    data[1] == ASUS_SPURIOUS_CODE_0X9E) {
				return -1;
			}
		}

		/*
		 * G713 and G733 send these codes on some keypresses, depending on
		 * the key pressed it can trigger a shutdown event if not caught.
		 */
		if (data[0] == 0x02 && data[1] == 0x30)
			return -1;
	}

	if (drvdata->quirks & QUIRK_ROG_CLAYMORE_II_KEYBOARD) {
		/*
		 * CLAYMORE II keyboard sends this packet when it goes to sleep
		 * this causes the whole system to go into suspend.
		 */
		if (size == 2 && data[0] == 0x02 && data[1] == 0x00)
			return -1;
	}

	/*
	 * The camera-toggle key reports its vendor usage (0x85) together with a
	 * companion state byte in the same array report, e.g. "5a 85 01" and
	 * "5a 85 10" for the two toggle positions. The 0x10 companion aliases the
	 * brightness-down vendor usage and would spuriously dim the panel, so drop
	 * the companion slots and leave only the camera usage for input mapping.
	 */
	if (drvdata->quirks & QUIRK_FILTER_CAMERA_COMPANION &&
	    report->id == FEATURE_KBD_REPORT_ID && size >= 3 && data[1] == 0x85)
		memset(&data[2], 0, size - 2);

	return 0;
}

static int asus_kbd_set_report(struct hid_device *hdev, const u8 *buf, size_t buf_size)
{
	u8 *dmabuf __free(kfree) = kmemdup(buf, buf_size, GFP_KERNEL);
	if (!dmabuf)
		return -ENOMEM;

	/*
	 * The report ID should be set from the incoming buffer due to LED and key
	 * interfaces having different pages
	 */
	return hid_hw_raw_request(hdev, buf[0], dmabuf, buf_size, HID_FEATURE_REPORT,
				  HID_REQ_SET_REPORT);
}

static int asus_kbd_init(struct hid_device *hdev, u8 report_id)
{
	/*
	 * The handshake is first sent as a set_report, then retrieved
	 * from a get_report. They should be equal.
	 */
	const u8 buf[] = { report_id, 0x41, 0x53, 0x55, 0x53, 0x20, 0x54,
		     0x65, 0x63, 0x68, 0x2e, 0x49, 0x6e, 0x63, 0x2e, 0x00 };
	int ret;

	ret = asus_kbd_set_report(hdev, buf, sizeof(buf));
	if (ret < 0) {
		hid_err(hdev, "Asus handshake %02x failed to send: %d\n",
			report_id, ret);
		return ret;
	}

	u8 *readbuf __free(kfree) = kzalloc(FEATURE_KBD_REPORT_SIZE, GFP_KERNEL);
	if (!readbuf)
		return -ENOMEM;

	ret = hid_hw_raw_request(hdev, report_id, readbuf,
				 FEATURE_KBD_REPORT_SIZE, HID_FEATURE_REPORT,
				 HID_REQ_GET_REPORT);
	if (ret < 0) {
		hid_warn(hdev, "Asus handshake %02x failed to receive ack: %d\n",
			 report_id, ret);
	} else if (memcmp(readbuf, buf, sizeof(buf)) != 0) {
		hid_warn(hdev, "Asus handshake %02x returned invalid response: %*ph\n",
			 report_id, FEATURE_KBD_REPORT_SIZE, readbuf);
	}

	/*
	 * Do not return error if handshake is wrong until this is
	 * verified to work for all devices.
	 */
	return 0;
}

static int asus_kbd_get_functions(struct hid_device *hdev,
				  unsigned char *kbd_func,
				  u8 report_id)
{
	const u8 buf[] = { report_id, 0x05, 0x20, 0x31, 0x00, 0x08 };
	u8 *readbuf;
	int ret;

	ret = asus_kbd_set_report(hdev, buf, sizeof(buf));
	if (ret < 0) {
		hid_err(hdev, "Asus failed to send configuration command: %d\n", ret);
		return ret;
	}

	readbuf = kzalloc(FEATURE_KBD_REPORT_SIZE, GFP_KERNEL);
	if (!readbuf)
		return -ENOMEM;

	ret = hid_hw_raw_request(hdev, report_id, readbuf,
				 FEATURE_KBD_REPORT_SIZE, HID_FEATURE_REPORT,
				 HID_REQ_GET_REPORT);
	if (ret < 0) {
		hid_err(hdev, "Asus failed to request functions: %d\n", ret);
		kfree(readbuf);
		return ret;
	}

	*kbd_func = readbuf[6];

	kfree(readbuf);
	return ret;
}

static int asus_kbd_disable_oobe(struct hid_device *hdev)
{
	const u8 init[][6] = {
		{ FEATURE_KBD_REPORT_ID, 0x05, 0x20, 0x31, 0x00, 0x08 },
		{ FEATURE_KBD_REPORT_ID, 0xBA, 0xC5, 0xC4 },
		{ FEATURE_KBD_REPORT_ID, 0xD0, 0x8F, 0x01 },
		{ FEATURE_KBD_REPORT_ID, 0xD0, 0x85, 0xFF }
	};
	int ret;

	for (size_t i = 0; i < ARRAY_SIZE(init); i++) {
		ret = asus_kbd_set_report(hdev, init[i], sizeof(init[i]));
		if (ret < 0)
			return ret;
	}

	hid_info(hdev, "Disabled OOBE for keyboard\n");
	return 0;
}

static void asus_kbd_set_fn_lock(struct hid_device *hdev, bool enabled)
{
	const u8 buf[FEATURE_KBD_REPORT_SIZE] = { FEATURE_KBD_REPORT_ID, 0xd0, 0x4e, !!enabled };
	int ret;

	ret = asus_kbd_set_report(hdev, buf, sizeof(buf));
	if (ret < 0)
		hid_err(hdev, "Asus failed to set fn lock: %d\n", ret);
}

static void asus_kbd_set_brightness(struct hid_device *hdev, u8 brightness)
{
	const u8 buf[FEATURE_KBD_REPORT_SIZE] = {
		FEATURE_KBD_REPORT_ID, 0xba, 0xc5, 0xc4, brightness
	};
	int ret;

	ret = asus_kbd_set_report(hdev, buf, sizeof(buf));
	if (ret < 0)
		hid_err(hdev, "Asus failed to set keyboard backlight: %d\n", ret);
}

static void asus_kbd_wmi_fan(struct hid_device *hdev, struct hid_raw_event_data *data)
{
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	int ret;

	ret = asus_wmi_send_event(drvdata, ASUS_FAN_CTRL_KEY_CODE);

	/*
	 * Warn if asus-wmi failed (but not if it's unavailable).
	 * Let the event reach userspace in all failure cases.
	 */
	switch (ret) {
	case -ENODEV:
		break;
	case 0:
		return;
	default:
		hid_warn(hdev, "Failed to notify asus-wmi: %d\n", ret);
		break;
	}

	/*
	 * Fallback: pass the raw event to the HID core; to avoid
	 * racing against the hid_report_raw_event() that generated
	 * this event use the same locking mechanism and wait for
	 * that function to terminate and signal the deferred execution
	 * before raising the stored event.
	 */
	down(&hdev->driver_input_lock);
	hid_report_raw_event(hdev, HID_INPUT_REPORT,
			     data->report_data, data->report_size,
			     data->report_size, 1);
	up(&hdev->driver_input_lock);
}

static void asus_kbd_backlight_set(struct asus_hid_listener *listener, int brightness)
{
	struct asus_drvdata *drvdata = container_of(listener, struct asus_drvdata, listener);
	struct asus_worker *worker = drvdata->worker;
	struct asus_work_action *action;
	unsigned long flags;

	drvdata->kbd_backlight_brightness = brightness;

	action = kzalloc(sizeof(struct asus_work_action), GFP_NOWAIT);
	if (!action)
		return;

	action->type = BRIGHTNESS_SET;
	action->data.brightness = brightness;
	INIT_LIST_HEAD(&action->node);

	spin_lock_irqsave(&worker->lock, flags);
	asus_worker_schedule(worker, action);
	spin_unlock_irqrestore(&worker->lock, flags);
}

static void asus_work(struct work_struct *work)
{
	struct asus_worker *worker = container_of(work, struct asus_worker, work);
	struct asus_work_action *action = NULL;
	unsigned long flags;

	/* Save the action to be performed and clear the flag */
	spin_lock_irqsave(&worker->lock, flags);
	if (!list_empty(&worker->actions)) {
		action = list_first_entry(&worker->actions,
					  struct asus_work_action, node);
		list_del(&action->node);
	}
	spin_unlock_irqrestore(&worker->lock, flags);

	if (!action)
		return;

	switch (action->type) {
	case BRIGHTNESS_SET:
		asus_kbd_set_brightness(worker->hdev, action->data.brightness);
		break;
	case FN_LOCK_SYNC:
		asus_kbd_set_fn_lock(worker->hdev, action->data.fn_lock);
		break;
	case WMI_FAN:
		asus_kbd_wmi_fan(worker->hdev, &action->data.fan_hid_data);
		break;
	default:
		hid_err(worker->hdev, "Invalid action type: %d\n", action->type);
		break;
	}

	kfree(action);

	/* Re-schedule if there are more pending actions */
	spin_lock_irqsave(&worker->lock, flags);
	if (!list_empty(&worker->actions))
		schedule_work(&worker->work);
	spin_unlock_irqrestore(&worker->lock, flags);
}

static int asus_worker_create(struct hid_device *hdev, struct asus_drvdata *drvdata)
{
	drvdata->worker = devm_kzalloc(&hdev->dev, sizeof(struct asus_worker), GFP_KERNEL);
	if (!drvdata->worker)
		return -ENOMEM;

	drvdata->worker->removed = false;
	drvdata->worker->hdev = hdev;
	INIT_LIST_HEAD(&drvdata->worker->actions);

	INIT_WORK(&drvdata->worker->work, asus_work);
	spin_lock_init(&drvdata->worker->lock);

	return 0;
}

static void asus_worker_stop(struct asus_worker *worker)
{
	struct asus_work_action *action, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&worker->lock, flags);
	worker->removed = true;
	list_for_each_entry_safe(action, tmp, &worker->actions, node) {
		list_del(&action->node);
		kfree(action);
	}
	spin_unlock_irqrestore(&worker->lock, flags);

	cancel_work_sync(&worker->work);
}

/*
 * We don't care about any other part of the string except the version section.
 * Example strings: FGA80100.RC72LA.312_T01, FGA80100.RC71LS.318_T01
 * The bytes "5a 05 03 31 00 1a 13" and possibly more come before the version
 * string, and there may be additional bytes after the version string such as
 * "75 00 74 00 65 00" or a postfix such as "_T01"
 */
static int mcu_parse_version_string(const u8 *response, size_t response_size)
{
	const u8 *end = response + response_size;
	const u8 *p = response;
	int dots, err, version;
	char buf[4];

	dots = 0;
	while (p < end && dots < 2) {
		if (*p++ == '.')
			dots++;
	}

	if (dots != 2 || end - p < 3)
		return -EINVAL;

	memcpy(buf, p, 3);
	buf[3] = '\0';

	err = kstrtoint(buf, 10, &version);
	if (err || version < 0)
		return -EINVAL;

	return version;
}

static int mcu_request_version(struct hid_device *hdev)
{
	u8 *response __free(kfree) = kzalloc(ROG_ALLY_REPORT_SIZE, GFP_KERNEL);
	const u8 request[] = { 0x5a, 0x05, 0x03, 0x31, 0x00, 0x20 };
	int ret;

	if (!response)
		return -ENOMEM;

	ret = asus_kbd_set_report(hdev, request, sizeof(request));
	if (ret < 0)
		return ret;

	ret = hid_hw_raw_request(hdev, FEATURE_REPORT_ID, response,
				ROG_ALLY_REPORT_SIZE, HID_FEATURE_REPORT,
				HID_REQ_GET_REPORT);
	if (ret < 0)
		return ret;

	ret = mcu_parse_version_string(response, ROG_ALLY_REPORT_SIZE);
	if (ret < 0) {
		pr_err("Failed to parse MCU version: %d\n", ret);
		print_hex_dump(KERN_ERR, "MCU: ", DUMP_PREFIX_NONE,
			      16, 1, response, ROG_ALLY_REPORT_SIZE, false);
	}

	return ret;
}

static void validate_mcu_fw_version(struct hid_device *hdev, int idProduct)
{
	int min_version, version;

	version = mcu_request_version(hdev);
	if (version < 0)
		return;

	switch (idProduct) {
	case USB_DEVICE_ID_ASUSTEK_ROG_NKEY_ALLY:
		min_version = ROG_ALLY_MIN_MCU;
		break;
	case USB_DEVICE_ID_ASUSTEK_ROG_NKEY_ALLY_X:
		min_version = ROG_ALLY_X_MIN_MCU;
		break;
	default:
		min_version = 0;
	}

	if (version < min_version) {
		hid_warn(hdev,
			"The MCU firmware version must be %d or greater to avoid issues with suspend.\n",
			min_version);
	} else {
		set_ally_mcu_hack(ASUS_WMI_ALLY_MCU_HACK_DISABLED);
		set_ally_mcu_powersave(true);
	}
}

static bool asus_has_report_id(struct hid_device *hdev, u16 report_id)
{
	struct hid_report *report;
	int t;

	for (t = HID_INPUT_REPORT; t <= HID_FEATURE_REPORT; t++) {
		list_for_each_entry(report, &hdev->report_enum[t].report_list, list) {
			if (report->id == report_id)
				return true;
		}
	}

	return false;
}

static int asus_kbd_register_leds(struct hid_device *hdev)
{
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	struct usb_interface *intf;
	struct usb_device *udev;
	unsigned char kbd_func;
	int ret;

	/* Get keyboard functions */
	ret = asus_kbd_get_functions(hdev, &kbd_func, FEATURE_KBD_REPORT_ID);
	if (ret < 0)
		return ret;

	/* Check for backlight support */
	if (!(kbd_func & SUPPORT_KBD_BACKLIGHT))
		return -ENODEV;

	if (dmi_match(DMI_PRODUCT_FAMILY, "ProArt P16")) {
		ret = asus_kbd_disable_oobe(hdev);
		if (ret < 0)
			return ret;
	}

	if ((drvdata->quirks & QUIRK_ROG_ALLY_XPAD) && hid_is_usb(hdev)) {
		intf = to_usb_interface(hdev->dev.parent);
		udev = interface_to_usbdev(intf);
		validate_mcu_fw_version(hdev,
			le16_to_cpu(udev->descriptor.idProduct));
	}

	drvdata->listener.brightness_set = asus_kbd_backlight_set;
	ret = asus_hid_register_listener(&drvdata->listener);
	if (ret < 0) {
		hid_err(hdev, "Unable to register kbd brightness listener: %d\n", ret);
		drvdata->listener.brightness_set = NULL;
	}

	return ret;
}

/*
 * [0]       REPORT_ID (same value defined in report descriptor)
 * [1]	     rest battery level. range [0..255]
 * [2]..[7]  Bluetooth hardware address (MAC address)
 * [8]       charging status
 *            = 0 : AC offline / discharging
 *            = 1 : AC online  / charging
 *            = 2 : AC online  / fully charged
 */
static int asus_parse_battery(struct asus_drvdata *drvdata, u8 *data, int size)
{
	u8 sts;
	u8 lvl;
	int val;

	lvl = data[1];
	sts = data[8];

	drvdata->battery_capacity = ((int)lvl * 100) / (int)BATTERY_LEVEL_MAX;

	switch (sts) {
	case BATTERY_STAT_CHARGING:
		val = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case BATTERY_STAT_FULL:
		val = POWER_SUPPLY_STATUS_FULL;
		break;
	case BATTERY_STAT_DISCONNECT:
	default:
		val = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	}
	drvdata->battery_stat = val;

	return 0;
}

static int asus_report_battery(struct asus_drvdata *drvdata, u8 *data, int size)
{
	/* notify only the autonomous event by device */
	if ((drvdata->battery_in_query == false) &&
			 (size == BATTERY_REPORT_SIZE))
		power_supply_changed(drvdata->battery);

	return 0;
}

static int asus_battery_query(struct asus_drvdata *drvdata)
{
	u8 *buf;
	int ret = 0;

	buf = kmalloc(BATTERY_REPORT_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	drvdata->battery_in_query = true;
	ret = hid_hw_raw_request(drvdata->hdev, BATTERY_REPORT_ID,
				buf, BATTERY_REPORT_SIZE,
				HID_INPUT_REPORT, HID_REQ_GET_REPORT);
	drvdata->battery_in_query = false;
	if (ret == BATTERY_REPORT_SIZE)
		ret = asus_parse_battery(drvdata, buf, BATTERY_REPORT_SIZE);
	else
		ret = -ENODATA;

	kfree(buf);

	return ret;
}

static enum power_supply_property asus_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_MODEL_NAME,
};

#define	QUERY_MIN_INTERVAL	(60 * HZ)	/* 60[sec] */

static int asus_battery_get_property(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct asus_drvdata *drvdata = power_supply_get_drvdata(psy);
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
	case POWER_SUPPLY_PROP_CAPACITY:
		if (time_before(drvdata->battery_next_query, jiffies)) {
			drvdata->battery_next_query =
					 jiffies + QUERY_MIN_INTERVAL;
			ret = asus_battery_query(drvdata);
			if (ret)
				return ret;
		}
		if (psp == POWER_SUPPLY_PROP_STATUS)
			val->intval = drvdata->battery_stat;
		else
			val->intval = drvdata->battery_capacity;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_DEVICE;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = drvdata->hdev->name;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int asus_battery_probe(struct hid_device *hdev)
{
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	struct power_supply_config pscfg = { .drv_data = drvdata };
	int ret = 0;

	drvdata->battery_capacity = 0;
	drvdata->battery_stat = POWER_SUPPLY_STATUS_UNKNOWN;
	drvdata->battery_in_query = false;

	drvdata->battery_desc.properties = asus_battery_props;
	drvdata->battery_desc.num_properties = ARRAY_SIZE(asus_battery_props);
	drvdata->battery_desc.get_property = asus_battery_get_property;
	drvdata->battery_desc.type = POWER_SUPPLY_TYPE_BATTERY;
	drvdata->battery_desc.use_for_apm = 0;
	drvdata->battery_desc.name = devm_kasprintf(&hdev->dev, GFP_KERNEL,
					"asus-keyboard-%s-battery",
					strlen(hdev->uniq) ?
					hdev->uniq : dev_name(&hdev->dev));
	if (!drvdata->battery_desc.name)
		return -ENOMEM;

	drvdata->battery_next_query = jiffies;

	drvdata->battery = devm_power_supply_register(&hdev->dev,
				&(drvdata->battery_desc), &pscfg);
	if (IS_ERR(drvdata->battery)) {
		ret = PTR_ERR(drvdata->battery);
		drvdata->battery = NULL;
		hid_err(hdev, "Unable to register battery device\n");
		return ret;
	}

	power_supply_powers(drvdata->battery, &hdev->dev);

	return ret;
}

static int asus_input_configured(struct hid_device *hdev, struct hid_input *hi)
{
	struct input_dev *input = hi->input;
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);

	/* T100CHI uses MULTI_INPUT, bind the touchpad to the mouse hid_input */
	if (drvdata->quirks & QUIRK_T100CHI &&
	    hi->report->id != T100CHI_MOUSE_REPORT_ID)
		return 0;

	/* Handle MULTI_INPUT on E1239T mouse/touchpad USB interface */
	if (drvdata->tp && (drvdata->quirks & QUIRK_MEDION_E1239T)) {
		switch (hi->report->id) {
		case E1239T_TP_TOGGLE_REPORT_ID:
			input_set_capability(input, EV_KEY, KEY_F21);
			input->name = "Asus Touchpad Keys";
			drvdata->tp_kbd_input = input;
			return 0;
		case INPUT_REPORT_ID:
			break; /* Touchpad report, handled below */
		default:
			return 0; /* Ignore other reports */
		}
	}

	if (drvdata->tp) {
		int ret;

		input_set_abs_params(input, ABS_MT_POSITION_X, 0,
				     drvdata->tp->max_x, 0, 0);
		input_set_abs_params(input, ABS_MT_POSITION_Y, 0,
				     drvdata->tp->max_y, 0, 0);
		input_abs_set_res(input, ABS_MT_POSITION_X, drvdata->tp->res_x);
		input_abs_set_res(input, ABS_MT_POSITION_Y, drvdata->tp->res_y);

		if (drvdata->tp->contact_size >= 5) {
			input_set_abs_params(input, ABS_TOOL_WIDTH, 0,
					     MAX_TOUCH_MAJOR, 0, 0);
			input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0,
					     MAX_TOUCH_MAJOR, 0, 0);
			input_set_abs_params(input, ABS_MT_PRESSURE, 0,
					      MAX_PRESSURE, 0, 0);
		}

		__set_bit(BTN_LEFT, input->keybit);
		__set_bit(INPUT_PROP_BUTTONPAD, input->propbit);

		ret = input_mt_init_slots(input, drvdata->tp->max_contacts,
					  INPUT_MT_POINTER);

		if (ret) {
			hid_err(hdev, "Asus input mt init slots failed: %d\n", ret);
			return ret;
		}
	}

	drvdata->input = input;

	if ((drvdata->quirks & QUIRK_HID_FN_LOCK) &&
	    (asus_kbd_fn_lock_set(drvdata, true)))
		hid_warn(hdev, "Error while setting FN lock to ON\n");

	return 0;
}

#define asus_map_key_clear(c)	hid_map_usage_clear(hi, usage, bit, \
						    max, EV_KEY, (c))
static int asus_input_mapping(struct hid_device *hdev,
		struct hid_input *hi, struct hid_field *field,
		struct hid_usage *usage, unsigned long **bit,
		int *max)
{
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);

	if (drvdata->quirks & QUIRK_SKIP_INPUT_MAPPING) {
		/* Don't map anything from the HID report.
		 * We do it all manually in asus_input_configured
		 */
		return -1;
	}

	/*
	 * Ignore a bunch of bogus collections in the T100CHI descriptor.
	 * This avoids a bunch of non-functional hid_input devices getting
	 * created because of the T100CHI using HID_QUIRK_MULTI_INPUT.
	 */
	if ((drvdata->quirks & (QUIRK_T100CHI | QUIRK_T90CHI)) &&
	    (field->application == (HID_UP_GENDESK | 0x0080) ||
	     field->application == HID_GD_MOUSE ||
	     usage->hid == (HID_UP_GENDEVCTRLS | 0x0024) ||
	     usage->hid == (HID_UP_GENDEVCTRLS | 0x0025) ||
	     usage->hid == (HID_UP_GENDEVCTRLS | 0x0026)))
		return -1;

	/* ASUS-specific keyboard hotkeys and led backlight */
	if ((usage->hid & HID_USAGE_PAGE) == HID_UP_ASUSVENDOR) {
		switch (usage->hid & HID_USAGE) {
		case 0x10: asus_map_key_clear(KEY_BRIGHTNESSDOWN);	break;
		case 0x20: asus_map_key_clear(KEY_BRIGHTNESSUP);		break;
		case 0x35: asus_map_key_clear(KEY_DISPLAY_OFF);		break;
		case 0x6c: asus_map_key_clear(KEY_SLEEP);		break;
		case 0x7c: asus_map_key_clear(KEY_MICMUTE);		break;
		case 0x82: asus_map_key_clear(KEY_CAMERA);		break;
		case 0x85: asus_map_key_clear(KEY_CAMERA);		break;
		case 0x86: asus_map_key_clear(KEY_PROG1);	break; /* MyASUS key */
		case 0x88: asus_map_key_clear(KEY_RFKILL);			break;
		case 0xb5: asus_map_key_clear(KEY_CALC);			break;
		case 0xc4: asus_map_key_clear(KEY_KBDILLUMUP);		break;
		case 0xc5: asus_map_key_clear(KEY_KBDILLUMDOWN);		break;
		case 0xc7: asus_map_key_clear(KEY_KBDILLUMTOGGLE);	break;
		case 0x4e: asus_map_key_clear(KEY_FN_ESC);		break;
		case 0x7e: asus_map_key_clear(KEY_EMOJI_PICKER);	break;

		case 0x8b: asus_map_key_clear(KEY_PROG1);	break; /* ProArt Creator Hub key */
		case 0x5f: asus_map_key_clear(KEY_PROG2);	break; /* S-shaped programmable key */
		case 0x6b: asus_map_key_clear(KEY_F21);		break; /* ASUS touchpad toggle */
		case 0x38: asus_map_key_clear(KEY_PROG1);	break; /* ROG key */
		case 0x93: asus_map_key_clear(KEY_PROG1);	break; /* ROG Ally X AC button */
		case 0xba: asus_map_key_clear(KEY_PROG2);	break; /* Fn+C ASUS Splendid */
		case 0x5c: asus_map_key_clear(KEY_PROG3);	break; /* Fn+Space Power4Gear */
		case 0x99: asus_map_key_clear(KEY_PROG4);	break; /* Fn+F5 "fan" symbol */
		case 0xae: asus_map_key_clear(KEY_PROG4);	break; /* Fn+F5 "fan" symbol */
		case 0x92: asus_map_key_clear(KEY_CALC);	break; /* Fn+Ret "Calc" symbol */
		case 0xb2: asus_map_key_clear(KEY_PROG2);	break; /* Fn+Left previous aura */
		case 0xb3: asus_map_key_clear(KEY_PROG3);	break; /* Fn+Left next aura */
		case 0x6a: asus_map_key_clear(KEY_F13);		break; /* Screenpad toggle */
		case 0x4b: asus_map_key_clear(KEY_F14);		break; /* Arrows/Pg-Up/Dn toggle */
		case 0xa5: asus_map_key_clear(KEY_F15);		break; /* ROG Ally left back */
		case 0xa6: asus_map_key_clear(KEY_F16);		break; /* ROG Ally QAM button */
		case 0xa7: asus_map_key_clear(KEY_F17);		break; /* ROG Ally ROG long-press */
		case 0xa8: asus_map_key_clear(KEY_F18);		break;

		default:
			/* ASUS lazily declares 256 usages, ignore the rest,
			 * as some make the keyboard appear as a pointer device. */
			return -1;
		}

		set_bit(EV_REP, hi->input->evbit);
		return 1;
	}

	if ((usage->hid & HID_USAGE_PAGE) == HID_UP_MSVENDOR) {
		switch (usage->hid & HID_USAGE) {
		case 0xff01: asus_map_key_clear(BTN_1);	break;
		case 0xff02: asus_map_key_clear(BTN_2);	break;
		case 0xff03: asus_map_key_clear(BTN_3);	break;
		case 0xff04: asus_map_key_clear(BTN_4);	break;
		case 0xff05: asus_map_key_clear(BTN_5);	break;
		case 0xff06: asus_map_key_clear(BTN_6);	break;
		case 0xff07: asus_map_key_clear(BTN_7);	break;
		case 0xff08: asus_map_key_clear(BTN_8);	break;
		case 0xff09: asus_map_key_clear(BTN_9);	break;
		case 0xff0a: asus_map_key_clear(BTN_A);	break;
		case 0xff0b: asus_map_key_clear(BTN_B);	break;
		case 0x00f1: asus_map_key_clear(KEY_WLAN);	break;
		case 0x00f2: asus_map_key_clear(KEY_BRIGHTNESSDOWN);	break;
		case 0x00f3: asus_map_key_clear(KEY_BRIGHTNESSUP);	break;
		case 0x00f4: asus_map_key_clear(KEY_DISPLAY_OFF);	break;
		case 0x00f7: asus_map_key_clear(KEY_CAMERA);	break;
		case 0x00f8: asus_map_key_clear(KEY_PROG1);	break;
		default:
			return 0;
		}

		set_bit(EV_REP, hi->input->evbit);
		return 1;
	}

	if (drvdata->quirks & QUIRK_NO_CONSUMER_USAGES &&
		(usage->hid & HID_USAGE_PAGE) == HID_UP_CONSUMER) {
		switch (usage->hid & HID_USAGE) {
		case 0xe2: /* Mute */
		case 0xe9: /* Volume up */
		case 0xea: /* Volume down */
			return 0;
		default:
			/* Ignore dummy Consumer usages which make the
			 * keyboard incorrectly appear as a pointer device.
			 */
			return -1;
		}
	}

	/*
	 * The mute button is broken and only sends press events, we
	 * deal with this in our raw_event handler, so do not map it.
	 */
	if ((drvdata->quirks & QUIRK_MEDION_E1239T) &&
	    usage->hid == (HID_UP_CONSUMER | 0xe2)) {
		input_set_capability(hi->input, EV_KEY, KEY_MUTE);
		return -1;
	}

	return 0;
}

static int asus_start_multitouch(struct hid_device *hdev)
{
	int ret;
	static const unsigned char buf[] = {
		FEATURE_REPORT_ID, 0x00, 0x03, 0x01, 0x00
	};
	unsigned char *dmabuf = kmemdup(buf, sizeof(buf), GFP_KERNEL);

	if (!dmabuf) {
		ret = -ENOMEM;
		hid_err(hdev, "Asus failed to alloc dma buf: %d\n", ret);
		return ret;
	}

	ret = hid_hw_raw_request(hdev, dmabuf[0], dmabuf, sizeof(buf),
					HID_FEATURE_REPORT, HID_REQ_SET_REPORT);

	kfree(dmabuf);

	if (ret != sizeof(buf)) {
		hid_err(hdev, "Asus failed to start multitouch: %d\n", ret);
		return ret;
	}

	return 0;
}

/*
 * Initialize the reports of the device.
 *
 * Failures are intentionally not fatal: asus_kbd_init() tolerates a wrong
 * handshake until this is verified to work for all devices, so a failure
 * is only reported and the initialization of the remaining reports is
 * still attempted.
 */
static void asus_initialize_reports(struct hid_device *hdev)
{
	int ret;

	for (int r = 0; r < ARRAY_SIZE(asus_report_id_init); r++) {
		if (asus_has_report_id(hdev, asus_report_id_init[r])) {
			ret = asus_kbd_init(hdev, asus_report_id_init[r]);
			if (ret < 0)
				hid_warn(hdev, "Failed to initialize 0x%x: %d.\n",
					 asus_report_id_init[r], ret);
		}
	}
}

static int __maybe_unused asus_resume(struct hid_device *hdev)
{
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	struct ally_handheld *ally = drvdata->rog_ally;
	int ep;

	/*
	 * If we have a backlight listener registered, restore the previous state,
	 * in case of error do not fail: most models restore the backlight
	 * automatically, and the error is non-fatal.
	 */
	if (drvdata->listener.brightness_set)
		asus_kbd_backlight_set(&drvdata->listener, drvdata->kbd_backlight_brightness);

	if (ally && (drvdata->quirks & QUIRK_ROG_ALLY_XPAD)) {
		ep = ally_get_endpoint_address(hdev);
		if (ep == HID_ALLY_INTF_CFG_IN)
			schedule_delayed_work(&ally->resume_work, msecs_to_jiffies(500));
	}

	return 0;
}

static int __maybe_unused asus_reset_resume(struct hid_device *hdev)
{
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	int ret;

	asus_initialize_reports(hdev);

	if (drvdata->tp)
		return asus_start_multitouch(hdev);

	if (drvdata->quirks & QUIRK_ROG_ALLY_XPAD) {
		ret = hid_asus_ally_reset_resume(hdev, drvdata->rog_ally);
		if (ret) {
			hid_err(hdev, "Failed to resume ROG Ally HID extensions: %d\n", ret);
			return ret;
		}
	}

	return 0;
}

static int asus_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct hid_report_enum *rep_enum;
	struct asus_drvdata *drvdata;
	struct ally_handheld *ally;
	struct hid_report *rep;
	bool is_vendor = false;
	int ret;

	drvdata = devm_kzalloc(&hdev->dev, sizeof(*drvdata), GFP_KERNEL);
	if (drvdata == NULL)
		return -ENOMEM;

	hid_set_drvdata(hdev, drvdata);

	drvdata->quirks = id->driver_data;

	/*
	 * T90CHI's keyboard dock returns same ID values as T100CHI's dock.
	 * Thus, identify T90CHI dock with product name string.
	 */
	if (strstr(hdev->name, "T90CHI")) {
		drvdata->quirks &= ~QUIRK_T100CHI;
		drvdata->quirks |= QUIRK_T90CHI;
	}

	if (drvdata->quirks & QUIRK_IS_MULTITOUCH)
		drvdata->tp = &asus_i2c_tp;

	if ((drvdata->quirks & QUIRK_T100_KEYBOARD) && hid_is_usb(hdev)) {
		struct usb_interface *intf = to_usb_interface(hdev->dev.parent);

		if (intf->altsetting->desc.bInterfaceNumber == T100_TPAD_INTF) {
			drvdata->quirks = QUIRK_SKIP_INPUT_MAPPING;
			/*
			 * The T100HA uses the same USB-ids as the T100TAF and
			 * the T200TA uses the same USB-ids as the T100TA, while
			 * both have different max x/y values as the T100TA[F].
			 */
			if (dmi_match(DMI_PRODUCT_NAME, "T100HAN"))
				drvdata->tp = &asus_t100ha_tp;
			else if (dmi_match(DMI_PRODUCT_NAME, "T200TA"))
				drvdata->tp = &asus_t200ta_tp;
			else
				drvdata->tp = &asus_t100ta_tp;
		}
	}

	if (drvdata->quirks & QUIRK_T100CHI) {
		/*
		 * All functionality is on a single HID interface and for
		 * userspace the touchpad must be a separate input_dev.
		 */
		hdev->quirks |= HID_QUIRK_MULTI_INPUT;
		drvdata->tp = &asus_t100chi_tp;
	}

	if ((drvdata->quirks & QUIRK_MEDION_E1239T) && hid_is_usb(hdev)) {
		struct usb_host_interface *alt =
			to_usb_interface(hdev->dev.parent)->altsetting;

		if (alt->desc.bInterfaceNumber == MEDION_E1239T_TPAD_INTF) {
			/* For separate input-devs for tp and tp toggle key */
			hdev->quirks |= HID_QUIRK_MULTI_INPUT;
			drvdata->quirks |= QUIRK_SKIP_INPUT_MAPPING;
			drvdata->tp = &medion_e1239t_tp;
		}
	}

	if (drvdata->quirks & QUIRK_NO_INIT_REPORTS)
		hdev->quirks |= HID_QUIRK_NO_INIT_REPORTS;

	drvdata->hdev = hdev;

	if (drvdata->quirks & (QUIRK_T100CHI | QUIRK_T90CHI)) {
		ret = asus_battery_probe(hdev);
		if (ret) {
			hid_err(hdev,
			    "Asus hid battery_probe failed: %d\n", ret);
			return ret;
		}
	}

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "Asus hid parse failed: %d\n", ret);
		return ret;
	}

	/* Check for vendor for RGB init and handle generic devices properly. */
	rep_enum = &hdev->report_enum[HID_INPUT_REPORT];
	list_for_each_entry(rep, &rep_enum->report_list, list) {
		if ((rep->application & HID_USAGE_PAGE) == HID_UP_ASUSVENDOR)
			is_vendor = true;
	}

	ret = asus_worker_create(hdev, drvdata);
	if (ret) {
		hid_warn(hdev, "Failed to initialize worker: %d\n", ret);
		return ret;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret) {
		asus_worker_stop(drvdata->worker);
		hid_err(hdev, "Asus hw start failed: %d\n", ret);
		return ret;
	}

	asus_initialize_reports(hdev);

	/* Laptops keyboard backlight is always at 0x5a */
	if (is_vendor && (drvdata->quirks & QUIRK_USE_KBD_BACKLIGHT) &&
	    (asus_has_report_id(hdev, FEATURE_KBD_REPORT_ID)) &&
		(asus_kbd_register_leds(hdev)))
		hid_warn(hdev, "Failed to initialize backlight.\n");

	if (drvdata->quirks & QUIRK_ROG_ALLY_XPAD) {
		ally = hid_asus_ally_probe(hdev);
		if (IS_ERR(ally))
			hid_err(hdev, "Failed to initialize ROG Ally HID extensions: %ld\n",
				PTR_ERR(ally));
		else
			drvdata->rog_ally = ally;
	}

	/*
	 * For ROG keyboards, skip rename for consistency and ->input check as
	 * some devices do not have inputs.
	 */
	if (drvdata->quirks & QUIRK_ROG_NKEY_KEYBOARD)
		return 0;

	/*
	 * Check that input registration succeeded. Checking that
	 * HID_CLAIMED_INPUT is set prevents a UAF when all input devices
	 * were freed during registration due to no usages being mapped,
	 * leaving drvdata->input pointing to freed memory.
	 */
	if (drvdata->input && (hdev->claimed & HID_CLAIMED_INPUT)) {
		if (drvdata->tp)
			drvdata->input->name = "Asus TouchPad";
		else
			drvdata->input->name = "Asus Keyboard";

		if (drvdata->tp) {
			ret = asus_start_multitouch(hdev);
			if (ret)
				goto err_stop_hw;
		}
	}

	return 0;
err_stop_hw:
	if (drvdata->listener.brightness_set)
		asus_hid_unregister_listener(&drvdata->listener);

	/*
	 * Roll back the state published in the global ally_drvdata by
	 * hid_asus_ally_probe(), or it would keep dangling pointers to
	 * this hdev and its devres-managed input_dev.
	 */
	if (drvdata->quirks & QUIRK_ROG_ALLY_XPAD)
		hid_asus_ally_remove(hdev, drvdata->rog_ally);

	asus_worker_stop(drvdata->worker);
	hid_hw_stop(hdev);
	return ret;
}

static void asus_remove(struct hid_device *hdev)
{
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);

	if (drvdata->listener.brightness_set)
		asus_hid_unregister_listener(&drvdata->listener);

	if (drvdata->quirks & QUIRK_ROG_ALLY_XPAD)
		hid_asus_ally_remove(hdev, drvdata->rog_ally);

	asus_worker_stop(drvdata->worker);
	hid_hw_stop(hdev);
}

static const __u8 asus_g752_fixed_rdesc[] = {
        0x19, 0x00,			/*   Usage Minimum (0x00)       */
        0x2A, 0xFF, 0x00,		/*   Usage Maximum (0xFF)       */
};

static const __u8 *asus_report_fixup(struct hid_device *hdev, __u8 *rdesc,
		unsigned int *rsize)
{
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);

	if (drvdata->quirks & QUIRK_FIX_NOTEBOOK_REPORT &&
			*rsize >= 56 && rdesc[54] == 0x25 && rdesc[55] == 0x65) {
		hid_info(hdev, "Fixing up Asus notebook report descriptor\n");
		rdesc[55] = 0xdd;
	}
	/* For the T100TA/T200TA keyboard dock */
	if (drvdata->quirks & QUIRK_T100_KEYBOARD &&
		 (*rsize == 76 || *rsize == 101) &&
		 rdesc[73] == 0x81 && rdesc[74] == 0x01) {
		hid_info(hdev, "Fixing up Asus T100 keyb report descriptor\n");
		rdesc[74] &= ~HID_MAIN_ITEM_CONSTANT;
	}
	/* For the T100CHI/T90CHI keyboard dock */
	if (drvdata->quirks & (QUIRK_T100CHI | QUIRK_T90CHI)) {
		int rsize_orig;
		int offs;

		if (drvdata->quirks & QUIRK_T100CHI) {
			rsize_orig = 403;
			offs = 388;
		} else {
			rsize_orig = 306;
			offs = 291;
		}

		/*
		 * Change Usage (76h) to Usage Minimum (00h), Usage Maximum
		 * (FFh) and clear the flags in the Input() byte.
		 * Note the descriptor has a bogus 0 byte at the end so we
		 * only need 1 extra byte.
		 */
		if (*rsize == rsize_orig &&
			rdesc[offs] == 0x09 && rdesc[offs + 1] == 0x76) {
			__u8 *new_rdesc;

			new_rdesc = devm_kzalloc(&hdev->dev, rsize_orig + 1,
						 GFP_KERNEL);
			if (!new_rdesc)
				return rdesc;

			hid_info(hdev, "Fixing up %s keyb report descriptor\n",
				drvdata->quirks & QUIRK_T100CHI ?
				"T100CHI" : "T90CHI");

			memcpy(new_rdesc, rdesc, rsize_orig);
			*rsize = rsize_orig + 1;
			rdesc = new_rdesc;

			memmove(rdesc + offs + 4, rdesc + offs + 2, 12);
			rdesc[offs] = 0x19;
			rdesc[offs + 1] = 0x00;
			rdesc[offs + 2] = 0x29;
			rdesc[offs + 3] = 0xff;
			rdesc[offs + 14] = 0x00;
		}
	}

	if (drvdata->quirks & QUIRK_G752_KEYBOARD &&
		 *rsize == 75 && rdesc[61] == 0x15 && rdesc[62] == 0x00) {
		/* report is missing usage minimum and maximum */
		__u8 *new_rdesc;
		size_t new_size = *rsize + sizeof(asus_g752_fixed_rdesc);

		new_rdesc = devm_kzalloc(&hdev->dev, new_size, GFP_KERNEL);
		if (new_rdesc == NULL)
			return rdesc;

		hid_info(hdev, "Fixing up Asus G752 keyb report descriptor\n");
		/* copy the valid part */
		memcpy(new_rdesc, rdesc, 61);
		/* insert missing part */
		memcpy(new_rdesc + 61, asus_g752_fixed_rdesc, sizeof(asus_g752_fixed_rdesc));
		/* copy remaining data */
		memcpy(new_rdesc + 61 + sizeof(asus_g752_fixed_rdesc), rdesc + 61, *rsize - 61);

		*rsize = new_size;
		rdesc = new_rdesc;
	}

	if (drvdata->quirks & QUIRK_ROG_NKEY_KEYBOARD &&
			*rsize == 331 && rdesc[190] == 0x85 && rdesc[191] == 0x5a &&
			rdesc[204] == 0x95 && rdesc[205] == 0x05) {
		hid_info(hdev, "Fixing up Asus N-KEY keyb report descriptor\n");
		rdesc[205] = 0x01;
	}

	/* match many more n-key devices */
	if (drvdata->quirks & QUIRK_ROG_NKEY_KEYBOARD && *rsize > 15) {
		for (int i = 0; i < *rsize - 15; i++) {
			/* offset to the count from 0x5a report part always 14 */
			if (rdesc[i] == 0x85 && rdesc[i + 1] == 0x5a &&
			    rdesc[i + 14] == 0x95 && rdesc[i + 15] == 0x05) {
				hid_info(hdev, "Fixing up Asus N-Key report descriptor\n");
				rdesc[i + 15] = 0x01;
				break;
			}
		}
	}

	return rdesc;
}

static const struct hid_device_id asus_devices[] = {
	{ HID_I2C_DEVICE(USB_VENDOR_ID_ASUSTEK,
		USB_DEVICE_ID_ASUSTEK_I2C_KEYBOARD), I2C_KEYBOARD_QUIRKS},
	{ HID_I2C_DEVICE(USB_VENDOR_ID_ASUSTEK,
		USB_DEVICE_ID_ASUSTEK_I2C_ZENBOOK_KEYBOARD),
	  I2C_KEYBOARD_QUIRKS | QUIRK_FILTER_CAMERA_COMPANION },
	{ HID_I2C_DEVICE(USB_VENDOR_ID_ASUSTEK,
		USB_DEVICE_ID_ASUSTEK_I2C_TOUCHPAD), I2C_TOUCHPAD_QUIRKS },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
		USB_DEVICE_ID_ASUSTEK_ROG_KEYBOARD1), QUIRK_USE_KBD_BACKLIGHT },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
		USB_DEVICE_ID_ASUSTEK_ROG_KEYBOARD2), QUIRK_USE_KBD_BACKLIGHT },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
		USB_DEVICE_ID_ASUSTEK_ROG_KEYBOARD3), QUIRK_G752_KEYBOARD },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
		USB_DEVICE_ID_ASUSTEK_FX503VD_KEYBOARD),
	  QUIRK_USE_KBD_BACKLIGHT },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
	    USB_DEVICE_ID_ASUSTEK_ROG_NKEY_KEYBOARD),
	  QUIRK_USE_KBD_BACKLIGHT | QUIRK_ROG_NKEY_KEYBOARD },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
	    USB_DEVICE_ID_ASUSTEK_ROG_NKEY_KEYBOARD2),
	  QUIRK_USE_KBD_BACKLIGHT | QUIRK_ROG_NKEY_KEYBOARD | QUIRK_HID_FN_LOCK },
	{ HID_I2C_DEVICE(USB_VENDOR_ID_ASUSTEK,
	    USB_DEVICE_ID_ASUSTEK_ROG_NKEY_KEYBOARD2),
	  QUIRK_USE_KBD_BACKLIGHT | QUIRK_ROG_NKEY_KEYBOARD | QUIRK_HID_FN_LOCK },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
	    USB_DEVICE_ID_ASUSTEK_ROG_Z13_LIGHTBAR),
	  QUIRK_USE_KBD_BACKLIGHT | QUIRK_ROG_NKEY_KEYBOARD },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
	    USB_DEVICE_ID_ASUSTEK_ROG_NKEY_ALLY),
	  QUIRK_USE_KBD_BACKLIGHT | QUIRK_ROG_NKEY_KEYBOARD | QUIRK_ROG_ALLY_XPAD},
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
	    USB_DEVICE_ID_ASUSTEK_ROG_NKEY_ALLY_X),
	  QUIRK_USE_KBD_BACKLIGHT | QUIRK_ROG_NKEY_KEYBOARD | QUIRK_ROG_ALLY_XPAD },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
	    USB_DEVICE_ID_ASUSTEK_XGM_2022),
	},
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
	    USB_DEVICE_ID_ASUSTEK_XGM_2023),
	},
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
	    USB_DEVICE_ID_ASUSTEK_ROG_CLAYMORE_II_KEYBOARD),
	  QUIRK_ROG_CLAYMORE_II_KEYBOARD },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
		USB_DEVICE_ID_ASUSTEK_T100TA_KEYBOARD),
	  QUIRK_T100_KEYBOARD | QUIRK_NO_CONSUMER_USAGES },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
		USB_DEVICE_ID_ASUSTEK_T100TAF_KEYBOARD),
	  QUIRK_T100_KEYBOARD | QUIRK_NO_CONSUMER_USAGES },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CHICONY, USB_DEVICE_ID_ASUS_AK1D) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_TURBOX, USB_DEVICE_ID_ASUS_MD_5110) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_JESS, USB_DEVICE_ID_ASUS_MD_5112) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_ASUSTEK,
		USB_DEVICE_ID_ASUSTEK_T100CHI_KEYBOARD), QUIRK_T100CHI },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ITE, USB_DEVICE_ID_ITE_MEDION_E1239T),
		QUIRK_MEDION_E1239T },
	/*
	 * Note bind to the HID_GROUP_GENERIC group, so that we only bind to the keyboard
	 * part, while letting hid-multitouch.c handle the touchpad.
	 */
	{ HID_DEVICE(BUS_USB, HID_GROUP_GENERIC,
		USB_VENDOR_ID_ASUSTEK, USB_DEVICE_ID_ASUSTEK_ROG_Z13_FOLIO),
	  QUIRK_USE_KBD_BACKLIGHT | QUIRK_ROG_NKEY_KEYBOARD },
	{ HID_DEVICE(BUS_USB, HID_GROUP_GENERIC,
		USB_VENDOR_ID_ASUSTEK, USB_DEVICE_ID_ASUSTEK_T101HA_KEYBOARD) },
	{ }
};
MODULE_DEVICE_TABLE(hid, asus_devices);

static struct hid_driver asus_driver = {
	.name			= "asus",
	.id_table		= asus_devices,
	.report_fixup		= asus_report_fixup,
	.probe                  = asus_probe,
	.remove			= asus_remove,
	.input_mapping          = asus_input_mapping,
	.input_configured       = asus_input_configured,
	.reset_resume           = pm_ptr(asus_reset_resume),
	.resume			= pm_ptr(asus_resume),
	.event			= asus_event,
	.raw_event		= asus_raw_event
};
module_hid_driver(asus_driver);

MODULE_IMPORT_NS("ASUS_WMI");
MODULE_LICENSE("GPL");
