// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for OneXPlayer gamepad configuration devices.
 *
 *  Copyright (c) 2026 Valve Corporation
 */

#include <linux/array_size.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/hid.h>
#include <linux/jiffies.h>
#include <linux/kstrtox.h>
#include <linux/led-class-multicolor.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/usb.h>
#include <linux/workqueue.h>

#include "hid-ids.h"

#define OXP_PACKET_SIZE 64
#define OXP_GEN2_EVENT_HEADER_SIZE 6
#define OXP_GEN2_RGB_ACK 0x20

#define GEN1_MESSAGE_ID	0xff
#define GEN2_MESSAGE_ID	0x3f

#define GEN1_USAGE_PAGE	0xff01
#define GEN2_USAGE_PAGE	0xff00

#define OXP_MAPPING_GAMEPAD	0x01
#define OXP_MAPPING_KEYBOARD	0x02
#define OXP_BMAP_FORMAT_DEFAULT	0x20
#define OXP_BMAP_FORMAT_X2	0x02
#define OXP_FILL_PAGE_SLOT(page, btn)            \
	{ .button_idx = (page)->btn.button_idx,  \
	  .mapping_idx = (page)->btn.mapping_idx }

#define OXP_GET_PROPERTY 0xfc
#define OXP_SET_PROPERTY 0xfd
#define OXP_EFFECT_MONO_TRUE 0xfe /* actual index for monocolor */
#define OXP_EFFECT_BREATHING_TRUE 0xf0

#define OXP_DEVICE_ATTR_RW(_name, _group)                                     \
	static ssize_t _name##_store(struct device *dev,                      \
				     struct device_attribute *attr,           \
				     const char *buf, size_t count)           \
	{                                                                     \
		return _group##_store(dev, attr, buf, count, _name.index);    \
	}                                                                     \
	static ssize_t _name##_show(struct device *dev,                       \
				    struct device_attribute *attr, char *buf) \
	{                                                                     \
		return _group##_show(dev, attr, buf, _name.index);            \
	}                                                                     \
	static DEVICE_ATTR_RW(_name)

enum oxp_function_index {
	OXP_FID_GEN1_RGB_SET =		0x07,
	OXP_FID_GEN1_RGB_REPLY =	0x0f,
	OXP_FID_GEN2_TOGGLE_MODE =	0xb2,
	OXP_FID_GEN2_RUMBLE_SET =	0xb3,
	OXP_FID_GEN2_KEY_STATE =	0xb4,
	OXP_FID_GEN2_STATUS_EVENT =	0xb8,
};

struct oxp_button_data {
	u8 mode;
	u8 index;
	u8 key_id;
	u8 padding[2];
} __packed;

struct oxp_button_entry {
	struct oxp_button_data data;
	const char *name;
};

static const struct oxp_button_entry oxp_button_table[] = {
	/* Gamepad Buttons */
	{ { OXP_MAPPING_GAMEPAD, 0x01 }, "BTN_A" },
	{ { OXP_MAPPING_GAMEPAD, 0x02 }, "BTN_B" },
	{ { OXP_MAPPING_GAMEPAD, 0x03 }, "BTN_X" },
	{ { OXP_MAPPING_GAMEPAD, 0x04 }, "BTN_Y" },
	{ { OXP_MAPPING_GAMEPAD, 0x05 }, "BTN_LB" },
	{ { OXP_MAPPING_GAMEPAD, 0x06 }, "BTN_RB" },
	{ { OXP_MAPPING_GAMEPAD, 0x07 }, "BTN_LT" },
	{ { OXP_MAPPING_GAMEPAD, 0x08 }, "BTN_RT" },
	{ { OXP_MAPPING_GAMEPAD, 0x09 }, "BTN_START" },
	{ { OXP_MAPPING_GAMEPAD, 0x0a }, "BTN_SELECT" },
	{ { OXP_MAPPING_GAMEPAD, 0x0b }, "BTN_L3" },
	{ { OXP_MAPPING_GAMEPAD, 0x0c }, "BTN_R3" },
	{ { OXP_MAPPING_GAMEPAD, 0x0d }, "DPAD_UP" },
	{ { OXP_MAPPING_GAMEPAD, 0x0e }, "DPAD_DOWN" },
	{ { OXP_MAPPING_GAMEPAD, 0x0f }, "DPAD_LEFT" },
	{ { OXP_MAPPING_GAMEPAD, 0x10 }, "DPAD_RIGHT" },
	{ { OXP_MAPPING_GAMEPAD, 0x11 }, "JOY_L_UP" },
	{ { OXP_MAPPING_GAMEPAD, 0x12 }, "JOY_L_UP_RIGHT" },
	{ { OXP_MAPPING_GAMEPAD, 0x13 }, "JOY_L_RIGHT" },
	{ { OXP_MAPPING_GAMEPAD, 0x14 }, "JOY_L_DOWN_RIGHT" },
	{ { OXP_MAPPING_GAMEPAD, 0x15 }, "JOY_L_DOWN" },
	{ { OXP_MAPPING_GAMEPAD, 0x16 }, "JOY_L_DOWN_LEFT" },
	{ { OXP_MAPPING_GAMEPAD, 0x17 }, "JOY_L_LEFT" },
	{ { OXP_MAPPING_GAMEPAD, 0x18 }, "JOY_L_UP_LEFT" },
	{ { OXP_MAPPING_GAMEPAD, 0x19 }, "JOY_R_UP" },
	{ { OXP_MAPPING_GAMEPAD, 0x1a }, "JOY_R_UP_RIGHT" },
	{ { OXP_MAPPING_GAMEPAD, 0x1b }, "JOY_R_RIGHT" },
	{ { OXP_MAPPING_GAMEPAD, 0x1c }, "JOY_R_DOWN_RIGHT" },
	{ { OXP_MAPPING_GAMEPAD, 0x1d }, "JOY_R_DOWN" },
	{ { OXP_MAPPING_GAMEPAD, 0x1e }, "JOY_R_DOWN_LEFT" },
	{ { OXP_MAPPING_GAMEPAD, 0x1f }, "JOY_R_LEFT" },
	{ { OXP_MAPPING_GAMEPAD, 0x20 }, "JOY_R_UP_LEFT" },
	{ { OXP_MAPPING_GAMEPAD, 0x22 }, "BTN_GUIDE" },
	/* Keyboard Keys */
	{ { OXP_MAPPING_KEYBOARD, 0x01, 0x5a }, "KEY_F1" },
	{ { OXP_MAPPING_KEYBOARD, 0x01, 0x5b }, "KEY_F2" },
	{ { OXP_MAPPING_KEYBOARD, 0x01, 0x5c }, "KEY_F3" },
	{ { OXP_MAPPING_KEYBOARD, 0x01, 0x5d }, "KEY_F4" },
	{ { OXP_MAPPING_KEYBOARD, 0x01, 0x5e }, "KEY_F5" },
	{ { OXP_MAPPING_KEYBOARD, 0x01, 0x5f }, "KEY_F6" },
	{ { OXP_MAPPING_KEYBOARD, 0x01, 0x60 }, "KEY_F7" },
	{ { OXP_MAPPING_KEYBOARD, 0x01, 0x61 }, "KEY_F8" },
	{ { OXP_MAPPING_KEYBOARD, 0x01, 0x62 }, "KEY_F9" },
	{ { OXP_MAPPING_KEYBOARD, 0x01, 0x63 }, "KEY_F10" },
	{ { OXP_MAPPING_KEYBOARD, 0x01, 0x64 }, "KEY_F11" },
	{ { OXP_MAPPING_KEYBOARD, 0x01, 0x65 }, "KEY_F12" },
	{ { OXP_MAPPING_KEYBOARD, 0x01, 0x66 }, "KEY_F13" },
	{ { OXP_MAPPING_KEYBOARD, 0x01, 0x67 }, "KEY_F14" },
	{ { OXP_MAPPING_KEYBOARD, 0x01, 0x68 }, "KEY_F15" },
	{ { OXP_MAPPING_KEYBOARD, 0x01, 0x69 }, "KEY_F16" },
	{ { OXP_MAPPING_KEYBOARD, 0x01, 0x6a }, "KEY_F17" },
	{ { OXP_MAPPING_KEYBOARD, 0x01, 0x6b }, "KEY_F18" },
	{ { OXP_MAPPING_KEYBOARD, 0x01, 0x6c }, "KEY_F19" },
	{ { OXP_MAPPING_KEYBOARD, 0x01, 0x6d }, "KEY_F20" },
	{ { OXP_MAPPING_KEYBOARD, 0x01, 0x6e }, "KEY_F21" },
	{ { OXP_MAPPING_KEYBOARD, 0x01, 0x6f }, "KEY_F22" },
	{ { OXP_MAPPING_KEYBOARD, 0x01, 0x70 }, "KEY_F23" },
	{ { OXP_MAPPING_KEYBOARD, 0x01, 0x71 }, "KEY_F24" },
};

enum oxp_joybutton_index {
	BUTTON_A =	0x01,
	BUTTON_B,
	BUTTON_X,
	BUTTON_Y,
	BUTTON_LB,
	BUTTON_RB,
	BUTTON_LT,
	BUTTON_RT,
	BUTTON_START,
	BUTTON_SELECT,
	BUTTON_L3,
	BUTTON_R3,
	BUTTON_DUP,
	BUTTON_DDOWN,
	BUTTON_DLEFT,
	BUTTON_DRIGHT,
	BUTTON_M1 =	0x22,
	BUTTON_M2,
	BUTTON_M3,
	BUTTON_M4,
	/* These are unused currently, reserved for future devices */
	BUTTON_M5,
	BUTTON_M6,
};

struct oxp_button_idx {
	enum oxp_joybutton_index button_idx;
	u8 mapping_idx;
} __packed;

struct oxp_bmap_page_1 {
	struct oxp_button_idx btn_a;
	struct oxp_button_idx btn_b;
	struct oxp_button_idx btn_x;
	struct oxp_button_idx btn_y;
	struct oxp_button_idx btn_lb;
	struct oxp_button_idx btn_rb;
	struct oxp_button_idx btn_lt;
	struct oxp_button_idx btn_rt;
	struct oxp_button_idx btn_start;
} __packed;

struct oxp_bmap_page_2 {
	struct oxp_button_idx btn_select;
	struct oxp_button_idx btn_l3;
	struct oxp_button_idx btn_r3;
	struct oxp_button_idx btn_dup;
	struct oxp_button_idx btn_ddown;
	struct oxp_button_idx btn_dleft;
	struct oxp_button_idx btn_dright;
	struct oxp_button_idx btn_m1;
	struct oxp_button_idx btn_m2;
} __packed;

struct oxp_rgb_led;

/* Hybrid devices expose RGB and controller configuration on separate HIDs. */
struct oxp_hid_cfg {
	/* General HID state */
	struct hid_device *hdev;
	struct mutex cfg_mutex; /*ensure single synchronous output report*/
	bool suspended;
	bool removing;

	/* Gamepad state */
	struct delayed_work oxp_btn_queue;
	struct delayed_work oxp_mcu_init;
	struct oxp_bmap_page_1 *bmap_1;
	struct oxp_bmap_page_2 *bmap_2;
	bool gen2_work_initialized;
	u8 rumble_intensity;
	bool bmap_page_3;
	u8 gamepad_mode;
	u8 bmap_format;

	/* RGB state */
	struct oxp_rgb_led *rgb_leds;
	spinlock_t rgb_reply_lock; /* Protect pending RGB acknowledgment state. */
	struct mutex rgb_mutex; /*serialize complete RGB transactions*/
	bool rgb_reply_pending;
	u8 rgb_reply_command;
	u8 rgb_reply_zone;
	u8 rgb_led_count;
	bool x2_rgb;
};

enum oxp_gamepad_mode_index {
	OXP_GP_MODE_XINPUT = 0x00,
	OXP_GP_MODE_DEBUG = 0x03,
};

static const char *const oxp_gamepad_mode_text[] = {
	[OXP_GP_MODE_XINPUT] = "xinput",
	[OXP_GP_MODE_DEBUG] = "debug",
};

enum oxp_feature_en_index {
	OXP_FEAT_DISABLED,
	OXP_FEAT_ENABLED,
};

static const char *const oxp_feature_en_text[] = {
	[OXP_FEAT_DISABLED] = "false",
	[OXP_FEAT_ENABLED] = "true",
};

enum oxp_rgb_effect_index {
	OXP_UNKNOWN,
	OXP_EFFECT_AURORA,
	OXP_EFFECT_BIRTHDAY,
	OXP_EFFECT_FLOWING,
	OXP_EFFECT_CHROMA_1,
	OXP_EFFECT_NEON,
	OXP_EFFECT_CHROMA_2,
	OXP_EFFECT_DREAMY,
	OXP_EFFECT_WARM,
	OXP_EFFECT_CYBERPUNK,
	OXP_EFFECT_SEA,
	OXP_EFFECT_SUNSET,
	OXP_EFFECT_COLORFUL,
	OXP_EFFECT_MONSTER,
	OXP_EFFECT_GREEN,
	OXP_EFFECT_BLUE,
	OXP_EFFECT_YELLOW,
	OXP_EFFECT_TEAL,
	OXP_EFFECT_PURPLE,
	OXP_EFFECT_FOGGY,
	OXP_EFFECT_MONO_LIST, /* placeholder for effect_index_show */
};

static const char *const oxp_rgb_effect_text[] = {
	[OXP_UNKNOWN] = "unknown",
	[OXP_EFFECT_AURORA] = "aurora",
	[OXP_EFFECT_BIRTHDAY] = "birthday_cake",
	[OXP_EFFECT_FLOWING] = "flowing_light",
	[OXP_EFFECT_CHROMA_1] = "chroma_popping",
	[OXP_EFFECT_NEON] = "neon",
	[OXP_EFFECT_CHROMA_2] = "chroma_breathing",
	[OXP_EFFECT_DREAMY] = "dreamy",
	[OXP_EFFECT_WARM] = "warm_sun",
	[OXP_EFFECT_CYBERPUNK] = "cyberpunk",
	[OXP_EFFECT_SEA] = "sea_foam",
	[OXP_EFFECT_SUNSET] = "sunset_afterglow",
	[OXP_EFFECT_COLORFUL] = "colorful",
	[OXP_EFFECT_MONSTER] = "monster_woke",
	[OXP_EFFECT_GREEN] = "green_breathing",
	[OXP_EFFECT_BLUE] = "blue_breathing",
	[OXP_EFFECT_YELLOW] = "yellow_breathing",
	[OXP_EFFECT_TEAL] = "teal_breathing",
	[OXP_EFFECT_PURPLE] = "purple_breathing",
	[OXP_EFFECT_FOGGY] = "foggy_haze",
	[OXP_EFFECT_MONO_LIST] = "monocolor",
};

enum oxp_rumble_side_index {
	OXP_RUMBLE_LEFT = 0x00,
	OXP_RUMBLE_RIGHT,
};

struct oxp_gen_1_rgb_report {
	u8 report_id;
	u8 message_id;
	u8 padding_2[2];
	u8 effect;
	u8 enabled;
	u8 speed;
	u8 brightness;
	u8 red;
	u8 green;
	u8 blue;
} __packed;

struct oxp_gen_2_event_header {
	u8 report_id;
	u8 message_id;
	u8 padding;
	u8 command;
	u8 zone;
	u8 status;
} __packed;

struct oxp_gen_2_rgb_report {
	u8 report_id;
	u8 header_id;
	u8 padding_2;
	u8 message_id;
	u8 padding_4[2];
	u8 enabled;
	u8 speed;
	u8 brightness;
	u8 red;
	u8 green;
	u8 blue;
	u8 padding_12[3];
	u8 effect;
} __packed;

struct oxp_rgb_color {
	u8 red;
	u8 green;
	u8 blue;
} __packed;

struct oxp_gen_3_rgb_color_report {
	u8 effect;
	u8 zone;
	u8 mode;
	struct oxp_rgb_color colors[18];
	u8 final_red;
	u8 final_green;
} __packed;

enum oxp_rgb_type {
	OXP_RGB_FULL,
	OXP_RGB_AUX,
};

enum oxp_rgb_aux_effect {
	OXP_RGB_AUX_EFFECT_MONOCOLOR,
	OXP_RGB_AUX_EFFECT_BREATHING,
};

static const char *const oxp_rgb_aux_effect_text[] = {
	[OXP_RGB_AUX_EFFECT_MONOCOLOR] = "monocolor",
	[OXP_RGB_AUX_EFFECT_BREATHING] = "breathing",
};

struct oxp_rgb_full_state {
	u8 brightness;
	u8 enabled;
	u8 effect;
	u8 speed;
};

struct oxp_rgb_aux_state {
	enum led_brightness brightness;
	bool valid;
	u8 effect;
	u8 green;
	u8 blue;
	u8 red;
};

struct oxp_rgb_led {
	struct mc_subled subled_info[3];
	struct led_classdev_mc mc_cdev;
	struct delayed_work work;
	struct oxp_hid_cfg *cfg;
	spinlock_t state_lock; /* protects auxiliary RGB state */
	void *state;
	u8 type;
	u8 zone;
};

struct oxp_rgb_led_desc {
	const char *name;
	u8 type;
	u8 zone;
};

struct oxp_attr {
	u8 index;
};

struct quirk_entry {
	u8 cfg_interface_num;
	bool bmap_page_3;
	bool hybrid_mcu;
	u8 bmap_format;
	bool x2_rgb;
};

static u16 get_usage_page(struct hid_device *hdev)
{
	return hdev->collection[0].usage >> 16;
}

static struct oxp_rgb_led *oxp_rgb_led_by_type(struct oxp_hid_cfg *cfg, u8 type)
{
	int i;

	for (i = 0; i < cfg->rgb_led_count; i++)
		if (cfg->rgb_leds[i].type == type)
			return &cfg->rgb_leds[i];

	return NULL;
}

static struct oxp_rgb_full_state *oxp_rgb_full_state(struct oxp_rgb_led *led)
{
	if (!led)
		return NULL;

	switch (led->type) {
	case OXP_RGB_FULL:
		return led->state;
	case OXP_RGB_AUX:
		return NULL;
	}

	return NULL;
}

static struct oxp_rgb_aux_state *oxp_rgb_aux_state(struct oxp_rgb_led *led)
{
	if (!led)
		return NULL;

	switch (led->type) {
	case OXP_RGB_FULL:
		return NULL;
	case OXP_RGB_AUX:
		return led->state;
	}

	return NULL;
}

static int oxp_hid_raw_event_gen_1(struct hid_device *hdev,
				   struct hid_report *report, u8 *data,
				   int size)
{
	struct oxp_hid_cfg *cfg = hid_get_drvdata(hdev);
	struct oxp_rgb_led *led = oxp_rgb_led_by_type(cfg, OXP_RGB_FULL);
	struct oxp_rgb_full_state *state = oxp_rgb_full_state(led);
	struct oxp_gen_1_rgb_report *rgb_rep;
	struct led_classdev_mc *led_mc;

	if (size < sizeof(*rgb_rep) || !state)
		return 0;

	if (data[1] != OXP_FID_GEN1_RGB_REPLY)
		return 0;

	led_mc = &led->mc_cdev;
	rgb_rep = (struct oxp_gen_1_rgb_report *)data;
	/* Ensure we save monocolor as the list value */
	state->effect = rgb_rep->effect == OXP_EFFECT_MONO_TRUE ?
			OXP_EFFECT_MONO_LIST : rgb_rep->effect;
	state->speed = rgb_rep->speed;
	state->enabled = rgb_rep->enabled == 0 ? OXP_FEAT_DISABLED :
					       OXP_FEAT_ENABLED;
	state->brightness = rgb_rep->brightness;
	led_mc->led_cdev.brightness = rgb_rep->brightness *
				      led_mc->led_cdev.max_brightness / 4;
	/* If monocolor had less than 100% brightness on the previous boot,
	 * there will be no reliable way to determine the real intensity.
	 * Since intensity scaling is used with a hardware brightness set at max,
	 * our brightness will always look like 100%. Use the last set value to
	 * prevent successive boots from lowering the brightness further.
	 * Brightness will be "wrong" but the effect will remain the same visually.
	 */
	led_mc->subled_info[0].intensity = rgb_rep->red;
	led_mc->subled_info[1].intensity = rgb_rep->green;
	led_mc->subled_info[2].intensity = rgb_rep->blue;

	return 0;
}

static int oxp_gen_2_property_out(struct oxp_hid_cfg *cfg,
				  enum oxp_function_index fid, u8 *data,
				  u8 data_size);
static int oxp_set_buttons(struct oxp_hid_cfg *cfg);
static int oxp_rumble_intensity_set(struct oxp_hid_cfg *cfg, u8 intensity);

static int oxp_rgb_status_store(struct oxp_rgb_led *led, u8 enabled, u8 speed,
				u8 brightness);
static int oxp_rgb_effect_set(struct oxp_rgb_led *led, u8 effect);

static void oxp_rgb_aux_restore_locked(struct oxp_rgb_led *led);

static void oxp_rgb_restore(struct oxp_hid_cfg *cfg)
{
	struct oxp_rgb_full_state *state;
	struct oxp_rgb_led *led;
	int ret;
	int i;

	if (!cfg->x2_rgb)
		return;

	guard(mutex)(&cfg->rgb_mutex);
	for (i = 0; i < cfg->rgb_led_count; i++) {
		led = &cfg->rgb_leds[i];
		switch (led->type) {
		case OXP_RGB_FULL:
			state = led->state;
			if (state->effect == OXP_UNKNOWN)
				break;

			ret = oxp_rgb_status_store(led, state->enabled,
						   state->speed,
						   state->brightness);
			if (ret) {
				dev_err(&cfg->hdev->dev,
					"Error: Failed to restore RGB status: %i\n",
					ret);
				break;
			}
			if (state->enabled == OXP_FEAT_DISABLED)
				break;

			ret = oxp_rgb_effect_set(led, state->effect);
			if (ret)
				dev_err(&cfg->hdev->dev,
					"Error: Failed to restore RGB effect: %i\n",
					ret);
			break;
		case OXP_RGB_AUX:
			oxp_rgb_aux_restore_locked(led);
			break;
		}
	}
}

static void oxp_mcu_init_fn(struct work_struct *work)
{
	struct oxp_hid_cfg *cfg = container_of(to_delayed_work(work),
					      struct oxp_hid_cfg, oxp_mcu_init);
	u8 gp_mode_data[3] = { OXP_GP_MODE_DEBUG, 0x01, 0x02 };
	int ret;

	if (READ_ONCE(cfg->suspended) || READ_ONCE(cfg->removing))
		return;

	/* Re-apply the button mapping */
	ret = oxp_set_buttons(cfg);
	if (ret)
		dev_err(&cfg->hdev->dev,
			"Error: Failed to set button mapping: %i\n", ret);

	/* Cycle the gamepad mode before restoring lighting. */
	ret = oxp_gen_2_property_out(cfg, OXP_FID_GEN2_TOGGLE_MODE,
				     gp_mode_data, sizeof(gp_mode_data));
	if (ret)
		dev_err(&cfg->hdev->dev,
			"Error: Failed to set gamepad mode: %i\n", ret);

	if (cfg->gamepad_mode != OXP_GP_MODE_DEBUG) {
		gp_mode_data[0] = OXP_GP_MODE_XINPUT;
		ret = oxp_gen_2_property_out(cfg, OXP_FID_GEN2_TOGGLE_MODE,
					     gp_mode_data, sizeof(gp_mode_data));
		if (ret)
			dev_err(&cfg->hdev->dev,
				"Error: Failed to set gamepad mode: %i\n", ret);
	}

	/* The final mode change must precede the single RGB restore. */
	oxp_rgb_restore(cfg);
	if (cfg->gamepad_mode == OXP_GP_MODE_DEBUG)
		return;

	ret = oxp_rumble_intensity_set(cfg, cfg->rumble_intensity);
	if (ret)
		dev_err(&cfg->hdev->dev,
			"Error: Failed to set rumble intensity: %i\n", ret);
}

static bool oxp_gen_2_rgb_event(struct oxp_hid_cfg *cfg,
				const struct oxp_gen_2_event_header *header)
{
	bool solicited = false;

	if (header->status == OXP_GEN2_RGB_ACK) {
		scoped_guard(spinlock_irqsave, &cfg->rgb_reply_lock) {
			if (cfg->rgb_reply_pending &&
			    header->command == cfg->rgb_reply_command &&
			    header->zone == cfg->rgb_reply_zone) {
				cfg->rgb_reply_pending = false;
				solicited = true;
			}
		}
		if (solicited)
			return true;
	}

	/* A color-write acknowledgment must not be treated as an MCU reset. */
	if (header->command != OXP_EFFECT_MONO_TRUE)
		return false;

	/* An unsolicited event is sent after the MCU resets on resume. */
	if (READ_ONCE(cfg->gen2_work_initialized) &&
	    !READ_ONCE(cfg->suspended) && !READ_ONCE(cfg->removing))
		mod_delayed_work(system_dfl_wq, &cfg->oxp_mcu_init,
				 msecs_to_jiffies(50));

	return true;
}

static int oxp_hid_raw_event_gen_2(struct hid_device *hdev,
				   struct hid_report *report, u8 *data,
				   int size)
{
	struct oxp_hid_cfg *cfg = hid_get_drvdata(hdev);
	struct oxp_rgb_led *led = oxp_rgb_led_by_type(cfg, OXP_RGB_FULL);
	struct oxp_rgb_full_state *state = oxp_rgb_full_state(led);
	struct oxp_gen_2_event_header *header = (void *)data;
	struct oxp_gen_2_rgb_report *rgb_rep;

	if (size < OXP_GEN2_EVENT_HEADER_SIZE)
		return 0;
	if (header->report_id != OXP_FID_GEN2_STATUS_EVENT)
		return 0;
	if (oxp_gen_2_rgb_event(cfg, header))
		return 0;
	if (header->command != OXP_GET_PROPERTY)
		return 0;
	if (size < sizeof(*rgb_rep) || !state)
		return 0;

	rgb_rep = (struct oxp_gen_2_rgb_report *)data;
	if (rgb_rep->enabled > OXP_FEAT_ENABLED || rgb_rep->speed > 9 ||
	    rgb_rep->brightness > 4)
		return 0;

	/* Ensure we save monocolor as the list value. */
	state->effect = rgb_rep->effect == OXP_EFFECT_MONO_TRUE ?
			OXP_EFFECT_MONO_LIST : rgb_rep->effect;
	state->speed = rgb_rep->speed;
	state->enabled = rgb_rep->enabled == 0 ? OXP_FEAT_DISABLED :
						  OXP_FEAT_ENABLED;
	state->brightness = rgb_rep->brightness;
	led->mc_cdev.led_cdev.brightness = rgb_rep->brightness *
						led->mc_cdev.led_cdev.max_brightness;
	led->mc_cdev.led_cdev.brightness /= 4;
	/* If monocolor had less than 100% brightness on the previous boot,
	 * there will be no reliable way to determine the real intensity.
	 * Since intensity scaling is used with a hardware brightness set at max,
	 * our brightness will always look like 100%. Use the last set value to
	 * prevent successive boots from lowering the brightness further.
	 * Brightness will be "wrong" but the effect will remain the same visually.
	 */
	led->mc_cdev.subled_info[0].intensity = rgb_rep->red;
	led->mc_cdev.subled_info[1].intensity = rgb_rep->green;
	led->mc_cdev.subled_info[2].intensity = rgb_rep->blue;

	return 0;
}

static int oxp_hid_raw_event_gen_3(struct oxp_hid_cfg *cfg,
				   struct hid_report *report, u8 *data,
				   int size)
{
	struct oxp_rgb_led *led = oxp_rgb_led_by_type(cfg, OXP_RGB_FULL);
	struct oxp_rgb_full_state *state = oxp_rgb_full_state(led);
	struct oxp_gen_2_event_header *header = (void *)data;
	struct oxp_gen_2_rgb_report *rgb_rep;

	if (size < OXP_GEN2_EVENT_HEADER_SIZE)
		return 0;
	if (header->report_id != OXP_FID_GEN2_STATUS_EVENT)
		return 0;
	if (oxp_gen_2_rgb_event(cfg, header))
		return 0;
	if (header->command != OXP_GET_PROPERTY)
		return 0;
	if (size < sizeof(*rgb_rep) || !state)
		return 0;

	rgb_rep = (struct oxp_gen_2_rgb_report *)data;
	if (rgb_rep->enabled > OXP_FEAT_ENABLED || rgb_rep->speed > 9 ||
	    rgb_rep->brightness > 4)
		return 0;

	/* Gen3 status replies do not report the current effect or RGB color. */
	state->speed = rgb_rep->speed;
	state->enabled = rgb_rep->enabled == 0 ? OXP_FEAT_DISABLED :
						  OXP_FEAT_ENABLED;
	/*
	 * Monocolor uses scaled channels at maximum hardware brightness.
	 * Retain the requested brightness rather than replacing it with 100%.
	 */
	if (state->effect != OXP_EFFECT_MONO_LIST) {
		state->brightness = rgb_rep->brightness;
		led->mc_cdev.led_cdev.brightness = rgb_rep->brightness *
						led->mc_cdev.led_cdev.max_brightness;
		led->mc_cdev.led_cdev.brightness /= 4;
	}

	return 0;
}

static int oxp_hid_raw_event(struct hid_device *hdev, struct hid_report *report,
			     u8 *data, int size)
{
	struct oxp_hid_cfg *cfg = hid_get_drvdata(hdev);
	u16 up = get_usage_page(hdev);

	if (!cfg || READ_ONCE(cfg->removing))
		return 0;

	dev_dbg(&hdev->dev, "raw event data: [%*ph]\n", size, data);

	switch (up) {
	case GEN1_USAGE_PAGE:
		return oxp_hid_raw_event_gen_1(hdev, report, data, size);
	case GEN2_USAGE_PAGE:
		/* X2/Gen3 controllers share the Gen2 HID usage page. */
		if (cfg->x2_rgb)
			return oxp_hid_raw_event_gen_3(cfg, report, data, size);
		return oxp_hid_raw_event_gen_2(hdev, report, data, size);
	default:
		break;
	}

	return 0;
}

static int mcu_property_out(struct oxp_hid_cfg *cfg, u8 *header,
			    size_t header_size, u8 *data, size_t data_size,
			    u8 *footer, size_t footer_size, bool expect_rgb_ack)
{
	unsigned char *dmabuf __free(kfree) = kzalloc(OXP_PACKET_SIZE, GFP_KERNEL);
	u8 rgb_command = 0;
	u8 rgb_zone = 0;
	bool rgb_write;
	int ret;

	if (!dmabuf)
		return -ENOMEM;

	if (header_size + data_size + footer_size > OXP_PACKET_SIZE)
		return -EINVAL;

	guard(mutex)(&cfg->cfg_mutex);
	if (READ_ONCE(cfg->removing))
		return -ENODEV;
	if (READ_ONCE(cfg->suspended))
		return -EHOSTDOWN;

	/*
	 * Track legacy monocolor replies to distinguish them from MCU resets.
	 * Only Gen3's known acknowledgment protocol makes a missing reply an
	 * error; retain transport-only success semantics for older controllers.
	 */
	rgb_write = header_size && data_size > 1 &&
		    header[0] == OXP_FID_GEN2_STATUS_EVENT &&
		    data[0] != OXP_GET_PROPERTY &&
		    (expect_rgb_ack || data[0] == OXP_EFFECT_MONO_TRUE);
	if (rgb_write) {
		rgb_command = data[0];
		rgb_zone = data[1];
		scoped_guard(spinlock_irqsave, &cfg->rgb_reply_lock) {
			cfg->rgb_reply_command = rgb_command;
			cfg->rgb_reply_zone = rgb_zone;
			cfg->rgb_reply_pending = true;
		}
	}

	memcpy(dmabuf, header, header_size);
	memcpy(dmabuf + header_size, data, data_size);
	if (footer_size)
		memcpy(dmabuf + OXP_PACKET_SIZE - footer_size, footer, footer_size);

	dev_dbg(&cfg->hdev->dev, "raw data: [%*ph]\n", OXP_PACKET_SIZE, dmabuf);

	ret = hid_hw_output_report(cfg->hdev, dmabuf, OXP_PACKET_SIZE);
	/* MCU takes 200ms to be ready for another command. */
	msleep(200);
	if (ret >= 0)
		ret = ret == OXP_PACKET_SIZE ? 0 : -EIO;

	if (rgb_write) {
		scoped_guard(spinlock_irqsave, &cfg->rgb_reply_lock) {
			if (cfg->rgb_reply_pending &&
			    cfg->rgb_reply_command == rgb_command &&
			    cfg->rgb_reply_zone == rgb_zone) {
				if (!ret && expect_rgb_ack)
					ret = -ETIMEDOUT;
				cfg->rgb_reply_pending = false;
			}
		}
	}

	return ret;
}

static int oxp_gen_1_property_out(struct oxp_hid_cfg *cfg,
				  enum oxp_function_index fid, u8 *data,
				  u8 data_size)
{
	u8 header[] = { fid, GEN1_MESSAGE_ID };
	size_t header_size = ARRAY_SIZE(header);

	return mcu_property_out(cfg, header, header_size, data, data_size,
				NULL, 0, false);
}

static int oxp_gen_2_property_out(struct oxp_hid_cfg *cfg,
				  enum oxp_function_index fid, u8 *data,
				  u8 data_size)
{
	u8 header[] = { fid, GEN2_MESSAGE_ID, 0x01 };
	u8 footer[] = { GEN2_MESSAGE_ID, fid };
	size_t header_size = ARRAY_SIZE(header);
	size_t footer_size = ARRAY_SIZE(footer);

	return mcu_property_out(cfg, header, header_size, data, data_size, footer,
				footer_size, false);
}

static const u8 oxp_x2_rgb_zones[] = { 0x01, 0x02, 0x07 };

static int oxp_gen_3_property_out(struct oxp_hid_cfg *cfg, u8 *data,
				  u8 data_size)
{
	u8 header[] = { OXP_FID_GEN2_STATUS_EVENT, GEN2_MESSAGE_ID, 0x01 };
	u8 footer[] = { GEN2_MESSAGE_ID, OXP_FID_GEN2_STATUS_EVENT };
	size_t header_size = ARRAY_SIZE(header);
	size_t footer_size = ARRAY_SIZE(footer);

	return mcu_property_out(cfg, header, header_size, data, data_size,
				footer, footer_size, true);
}

static int oxp_gen_3_rgb_property_out(struct oxp_hid_cfg *cfg, u8 *data,
				      u8 data_size)
{
	int first_err;
	int attempt;
	int ret;
	int i;

	/*
	 * Each zone is addressed independently. Complete the pass even if one
	 * write fails so the remaining ring zones still receive the setting.
	 * Retry the full pass once to keep their requested settings identical.
	 */
	for (attempt = 0; attempt < 2; attempt++) {
		first_err = 0;
		for (i = 0; i < ARRAY_SIZE(oxp_x2_rgb_zones); i++) {
			data[1] = oxp_x2_rgb_zones[i];
			ret = oxp_gen_3_property_out(cfg, data, data_size);
			if (ret && !first_err)
				first_err = ret;
		}
		if (!first_err)
			return 0;
	}

	return first_err;
}

static int oxp_gen_3_rgb_zone_property_out(struct oxp_hid_cfg *cfg,
					   u8 *data, u8 data_size)
{
	int ret;

	ret = oxp_gen_3_property_out(cfg, data, data_size);
	/* Retry once if the write or its acknowledgment failed. */
	if (ret)
		ret = oxp_gen_3_property_out(cfg, data, data_size);

	return ret;
}

static void oxp_gen_3_rgb_fill_color(struct oxp_gen_3_rgb_color_report *report,
				     u8 command, u8 zone, u8 red, u8 green,
				     u8 blue)
{
	int i;

	memset(report, 0, sizeof(*report));
	report->effect = command;
	report->zone = zone;
	report->mode = 0x02;
	for (i = 0; i < ARRAY_SIZE(report->colors); i++)
		report->colors[i] = (struct oxp_rgb_color) { red, green, blue };
	report->final_red = red;
	report->final_green = green;
}

static bool oxp_rgb_aux_snapshot(struct oxp_rgb_led *led,
				 struct oxp_rgb_aux_state *state)
{
	struct oxp_rgb_aux_state *cached = oxp_rgb_aux_state(led);

	if (!cached)
		return false;

	scoped_guard(spinlock_irqsave, &led->state_lock) {
		*state = *cached;
	}

	return state->valid;
}

static int oxp_rgb_aux_apply(struct oxp_rgb_led *led,
			     const struct oxp_rgb_aux_state *state)
{
	struct oxp_hid_cfg *cfg = led->cfg;
	unsigned int max_brightness = led->mc_cdev.led_cdev.max_brightness;
	unsigned int brightness = min_t(unsigned int, state->brightness,
						 max_brightness);
	u8 status_data[6] = { OXP_SET_PROPERTY, led->zone, 0x02,
			      OXP_FEAT_DISABLED, 0x05, 0x04 };
	struct oxp_gen_3_rgb_color_report color_report;
	u8 command;
	u8 green;
	u8 blue;
	u8 red;
	int ret;

	if (brightness)
		status_data[3] = OXP_FEAT_ENABLED;

	ret = oxp_gen_3_rgb_zone_property_out(cfg, status_data,
					      sizeof(status_data));
	if (ret || !brightness)
		return ret;

	/* Scale each RGB channel by brightness, rounding to the nearest integer. */
	red = ((unsigned int)state->red * brightness + max_brightness / 2) /
	      max_brightness;
	green = ((unsigned int)state->green * brightness + max_brightness / 2) /
		max_brightness;
	blue = ((unsigned int)state->blue * brightness + max_brightness / 2) /
	       max_brightness;

	switch (state->effect) {
	case OXP_RGB_AUX_EFFECT_MONOCOLOR:
		command = OXP_EFFECT_MONO_TRUE;
		break;
	case OXP_RGB_AUX_EFFECT_BREATHING:
		command = OXP_EFFECT_BREATHING_TRUE;
		break;
	default:
		return -EINVAL;
	}

	oxp_gen_3_rgb_fill_color(&color_report, command, led->zone, red, green,
				 blue);
	return oxp_gen_3_rgb_zone_property_out(cfg, (u8 *)&color_report,
					       sizeof(color_report));
}

static void oxp_rgb_aux_restore_locked(struct oxp_rgb_led *led)
{
	struct oxp_hid_cfg *cfg = led->cfg;
	struct oxp_rgb_aux_state state;
	int ret;

	if (!oxp_rgb_aux_snapshot(led, &state))
		return;

	ret = oxp_rgb_aux_apply(led, &state);
	if (ret)
		dev_err(&cfg->hdev->dev,
			"Failed to restore RGB zone %#04x: %i\n", led->zone, ret);
}

static ssize_t gamepad_mode_store(struct device *dev,
				  struct device_attribute *attr, const char *buf,
				  size_t count)
{
	struct oxp_hid_cfg *cfg = dev_get_drvdata(dev);
	u16 up = get_usage_page(cfg->hdev);
	u8 data[3] = { 0x00, 0x01, 0x02 };
	int ret = -EINVAL;
	int i;

	if (up != GEN2_USAGE_PAGE)
		return ret;

	for (i = 0; i < ARRAY_SIZE(oxp_gamepad_mode_text); i++) {
		if (oxp_gamepad_mode_text[i] && sysfs_streq(buf, oxp_gamepad_mode_text[i])) {
			ret = i;
			break;
		}
	}
	if (ret < 0)
		return ret;

	data[0] = ret;

	ret = oxp_gen_2_property_out(cfg, OXP_FID_GEN2_TOGGLE_MODE, data, 3);
	if (ret)
		return ret;

	cfg->gamepad_mode = data[0];

	if (cfg->gamepad_mode == OXP_GP_MODE_DEBUG)
		return count;

	/* Re-apply rumble settings as switching gamepad mode will override */
	ret = oxp_rumble_intensity_set(cfg, cfg->rumble_intensity);
	if (ret)
		return ret;

	return count;
}

static ssize_t gamepad_mode_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct oxp_hid_cfg *cfg = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", oxp_gamepad_mode_text[cfg->gamepad_mode]);
}
static DEVICE_ATTR_RW(gamepad_mode);

static ssize_t gamepad_mode_index_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	ssize_t count = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(oxp_gamepad_mode_text); i++) {
		if (!oxp_gamepad_mode_text[i] ||
		    oxp_gamepad_mode_text[i][0] == '\0')
			continue;

		count += sysfs_emit_at(buf, count, "%s ", oxp_gamepad_mode_text[i]);
	}

	if (count)
		buf[count - 1] = '\n';

	return count;
}
static DEVICE_ATTR_RO(gamepad_mode_index);

static void oxp_set_defaults_bmap_1(struct oxp_bmap_page_1 *bmap)
{
	bmap->btn_a.button_idx = BUTTON_A;
	bmap->btn_a.mapping_idx = 0;
	bmap->btn_b.button_idx = BUTTON_B;
	bmap->btn_b.mapping_idx = 1;
	bmap->btn_x.button_idx = BUTTON_X;
	bmap->btn_x.mapping_idx = 2;
	bmap->btn_y.button_idx = BUTTON_Y;
	bmap->btn_y.mapping_idx = 3;
	bmap->btn_lb.button_idx = BUTTON_LB;
	bmap->btn_lb.mapping_idx = 4;
	bmap->btn_rb.button_idx = BUTTON_RB;
	bmap->btn_rb.mapping_idx = 5;
	bmap->btn_lt.button_idx = BUTTON_LT;
	bmap->btn_lt.mapping_idx = 6;
	bmap->btn_rt.button_idx = BUTTON_RT;
	bmap->btn_rt.mapping_idx = 7;
	bmap->btn_start.button_idx = BUTTON_START;
	bmap->btn_start.mapping_idx = 8;
}

static void oxp_set_defaults_bmap_2(struct oxp_bmap_page_2 *bmap)
{
	bmap->btn_select.button_idx = BUTTON_SELECT;
	bmap->btn_select.mapping_idx = 9;
	bmap->btn_l3.button_idx = BUTTON_L3;
	bmap->btn_l3.mapping_idx = 10;
	bmap->btn_r3.button_idx = BUTTON_R3;
	bmap->btn_r3.mapping_idx = 11;
	bmap->btn_dup.button_idx = BUTTON_DUP;
	bmap->btn_dup.mapping_idx = 12;
	bmap->btn_ddown.button_idx = BUTTON_DDOWN;
	bmap->btn_ddown.mapping_idx = 13;
	bmap->btn_dleft.button_idx = BUTTON_DLEFT;
	bmap->btn_dleft.mapping_idx = 14;
	bmap->btn_dright.button_idx = BUTTON_DRIGHT;
	bmap->btn_dright.mapping_idx = 15;
	bmap->btn_m1.button_idx = BUTTON_M1;
	bmap->btn_m1.mapping_idx = 47; /* KEY_F15 */
	bmap->btn_m2.button_idx = BUTTON_M2;
	bmap->btn_m2.mapping_idx = 48; /* KEY_F16 */
}

static void oxp_page_fill_data(char *buf, const struct oxp_button_idx *buttons,
			       size_t len)
{
	size_t offset_increment = sizeof(u8) + sizeof(struct oxp_button_idx);
	size_t offset = 5;
	unsigned int i;

	for (i = 0; i < len; i++, offset += offset_increment) {
		buf[offset] = (u8)buttons[i].button_idx;
		memcpy(buf + offset + 1,
		       &oxp_button_table[buttons[i].mapping_idx].data,
		       sizeof(struct oxp_button_data));
	}
}

static int oxp_set_buttons(struct oxp_hid_cfg *cfg)
{
	u8 page_1[59] = { 0x02, 0x38, cfg->bmap_format, 0x01, 0x01 };
	u8 page_2[59] = { 0x02, 0x38, cfg->bmap_format, 0x02, 0x01 };
	u8 page_3[59] = {
		0x02, 0x38, cfg->bmap_format, 0x03, 0x01,
		/*
		 * M3/M4 have no mutable sysfs mapping slots. Keep their factory
		 * encodings, which are not entries in oxp_button_table.
		 */
		BUTTON_M3, OXP_MAPPING_KEYBOARD, 0x02, 0x05, 0x00, 0x00,
		BUTTON_M4, OXP_MAPPING_GAMEPAD, 0x21, 0x00, 0x00, 0x00,
	};
	u16 up = get_usage_page(cfg->hdev);
	int ret;

	if (up != GEN2_USAGE_PAGE)
		return -EINVAL;

	const struct oxp_button_idx p1[] = {
		OXP_FILL_PAGE_SLOT(cfg->bmap_1, btn_a),
		OXP_FILL_PAGE_SLOT(cfg->bmap_1, btn_b),
		OXP_FILL_PAGE_SLOT(cfg->bmap_1, btn_x),
		OXP_FILL_PAGE_SLOT(cfg->bmap_1, btn_y),
		OXP_FILL_PAGE_SLOT(cfg->bmap_1, btn_lb),
		OXP_FILL_PAGE_SLOT(cfg->bmap_1, btn_rb),
		OXP_FILL_PAGE_SLOT(cfg->bmap_1, btn_lt),
		OXP_FILL_PAGE_SLOT(cfg->bmap_1, btn_rt),
		OXP_FILL_PAGE_SLOT(cfg->bmap_1, btn_start),
	};

	const struct oxp_button_idx p2[] = {
		OXP_FILL_PAGE_SLOT(cfg->bmap_2, btn_select),
		OXP_FILL_PAGE_SLOT(cfg->bmap_2, btn_l3),
		OXP_FILL_PAGE_SLOT(cfg->bmap_2, btn_r3),
		OXP_FILL_PAGE_SLOT(cfg->bmap_2, btn_dup),
		OXP_FILL_PAGE_SLOT(cfg->bmap_2, btn_ddown),
		OXP_FILL_PAGE_SLOT(cfg->bmap_2, btn_dleft),
		OXP_FILL_PAGE_SLOT(cfg->bmap_2, btn_dright),
		OXP_FILL_PAGE_SLOT(cfg->bmap_2, btn_m1),
		OXP_FILL_PAGE_SLOT(cfg->bmap_2, btn_m2),
	};

	oxp_page_fill_data(page_1, p1, ARRAY_SIZE(p1));
	oxp_page_fill_data(page_2, p2, ARRAY_SIZE(p2));

	ret = oxp_gen_2_property_out(cfg, OXP_FID_GEN2_KEY_STATE, page_1, ARRAY_SIZE(page_1));
	if (ret)
		return ret;

	ret = oxp_gen_2_property_out(cfg, OXP_FID_GEN2_KEY_STATE, page_2, ARRAY_SIZE(page_2));
	if (ret || !cfg->bmap_page_3)
		return ret;

	return oxp_gen_2_property_out(cfg, OXP_FID_GEN2_KEY_STATE, page_3, ARRAY_SIZE(page_3));
}

static void oxp_reset_buttons(struct oxp_hid_cfg *cfg)
{
	oxp_set_defaults_bmap_1(cfg->bmap_1);
	oxp_set_defaults_bmap_2(cfg->bmap_2);
}

static ssize_t reset_buttons_store(struct device *dev,
				   struct device_attribute *attr, const char *buf,
				   size_t count)
{
	struct oxp_hid_cfg *cfg = dev_get_drvdata(dev);
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	if (val != 1)
		return -EINVAL;

	oxp_reset_buttons(cfg);
	ret = oxp_set_buttons(cfg);
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_WO(reset_buttons);

static void oxp_btn_queue_fn(struct work_struct *work)
{
	struct oxp_hid_cfg *cfg = container_of(to_delayed_work(work),
					      struct oxp_hid_cfg, oxp_btn_queue);
	int ret;

	if (READ_ONCE(cfg->suspended) || READ_ONCE(cfg->removing))
		return;

	ret = oxp_set_buttons(cfg);
	if (ret)
		dev_err(&cfg->hdev->dev,
			"Error: Failed to write button mapping: %i\n", ret);
}

static int oxp_button_idx_from_str(const char *buf)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(oxp_button_table); i++)
		if (sysfs_streq(buf, oxp_button_table[i].name))
			return i;

	return -EINVAL;
}

static ssize_t map_button_store(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t count, u8 index)
{
	struct oxp_hid_cfg *cfg = dev_get_drvdata(dev);
	int idx;

	idx = oxp_button_idx_from_str(buf);
	if (idx < 0)
		return idx;

	switch (index) {
	case BUTTON_A:
		cfg->bmap_1->btn_a.mapping_idx = idx;
		break;
	case BUTTON_B:
		cfg->bmap_1->btn_b.mapping_idx = idx;
		break;
	case BUTTON_X:
		cfg->bmap_1->btn_x.mapping_idx = idx;
		break;
	case BUTTON_Y:
		cfg->bmap_1->btn_y.mapping_idx = idx;
		break;
	case BUTTON_LB:
		cfg->bmap_1->btn_lb.mapping_idx = idx;
		break;
	case BUTTON_RB:
		cfg->bmap_1->btn_rb.mapping_idx = idx;
		break;
	case BUTTON_LT:
		cfg->bmap_1->btn_lt.mapping_idx = idx;
		break;
	case BUTTON_RT:
		cfg->bmap_1->btn_rt.mapping_idx = idx;
		break;
	case BUTTON_START:
		cfg->bmap_1->btn_start.mapping_idx = idx;
		break;
	case BUTTON_SELECT:
		cfg->bmap_2->btn_select.mapping_idx = idx;
		break;
	case BUTTON_L3:
		cfg->bmap_2->btn_l3.mapping_idx = idx;
		break;
	case BUTTON_R3:
		cfg->bmap_2->btn_r3.mapping_idx = idx;
		break;
	case BUTTON_DUP:
		cfg->bmap_2->btn_dup.mapping_idx = idx;
		break;
	case BUTTON_DDOWN:
		cfg->bmap_2->btn_ddown.mapping_idx = idx;
		break;
	case BUTTON_DLEFT:
		cfg->bmap_2->btn_dleft.mapping_idx = idx;
		break;
	case BUTTON_DRIGHT:
		cfg->bmap_2->btn_dright.mapping_idx = idx;
		break;
	case BUTTON_M1:
		cfg->bmap_2->btn_m1.mapping_idx = idx;
		break;
	case BUTTON_M2:
		cfg->bmap_2->btn_m2.mapping_idx = idx;
		break;
	default:
		return -EINVAL;
	}
	if (!READ_ONCE(cfg->suspended) && !READ_ONCE(cfg->removing))
		mod_delayed_work(system_dfl_wq, &cfg->oxp_btn_queue,
				 msecs_to_jiffies(50));
	return count;
}

static ssize_t map_button_show(struct device *dev,
			       struct device_attribute *attr, char *buf,
			       u8 index)
{
	struct oxp_hid_cfg *cfg = dev_get_drvdata(dev);
	u8 i;

	switch (index) {
	case BUTTON_A:
		i = cfg->bmap_1->btn_a.mapping_idx;
		break;
	case BUTTON_B:
		i = cfg->bmap_1->btn_b.mapping_idx;
		break;
	case BUTTON_X:
		i = cfg->bmap_1->btn_x.mapping_idx;
		break;
	case BUTTON_Y:
		i = cfg->bmap_1->btn_y.mapping_idx;
		break;
	case BUTTON_LB:
		i = cfg->bmap_1->btn_lb.mapping_idx;
		break;
	case BUTTON_RB:
		i = cfg->bmap_1->btn_rb.mapping_idx;
		break;
	case BUTTON_LT:
		i = cfg->bmap_1->btn_lt.mapping_idx;
		break;
	case BUTTON_RT:
		i = cfg->bmap_1->btn_rt.mapping_idx;
		break;
	case BUTTON_START:
		i = cfg->bmap_1->btn_start.mapping_idx;
		break;
	case BUTTON_SELECT:
		i = cfg->bmap_2->btn_select.mapping_idx;
		break;
	case BUTTON_L3:
		i = cfg->bmap_2->btn_l3.mapping_idx;
		break;
	case BUTTON_R3:
		i = cfg->bmap_2->btn_r3.mapping_idx;
		break;
	case BUTTON_DUP:
		i = cfg->bmap_2->btn_dup.mapping_idx;
		break;
	case BUTTON_DDOWN:
		i = cfg->bmap_2->btn_ddown.mapping_idx;
		break;
	case BUTTON_DLEFT:
		i = cfg->bmap_2->btn_dleft.mapping_idx;
		break;
	case BUTTON_DRIGHT:
		i = cfg->bmap_2->btn_dright.mapping_idx;
		break;
	case BUTTON_M1:
		i = cfg->bmap_2->btn_m1.mapping_idx;
		break;
	case BUTTON_M2:
		i = cfg->bmap_2->btn_m2.mapping_idx;
		break;
	default:
		return -EINVAL;
	}

	if (i >= ARRAY_SIZE(oxp_button_table))
		return -EINVAL;

	return sysfs_emit(buf, "%s\n", oxp_button_table[i].name);
}

static ssize_t button_mapping_options_show(struct device *dev,
					   struct device_attribute *attr, char *buf)
{
	ssize_t count = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(oxp_button_table); i++)
		count += sysfs_emit_at(buf, count, "%s ", oxp_button_table[i].name);

	if (count)
		buf[count - 1] = '\n';

	return count;
}
static DEVICE_ATTR_RO(button_mapping_options);

static int oxp_rumble_intensity_set(struct oxp_hid_cfg *cfg, u8 intensity)
{
	u8 header[15] = { 0x02, 0x38, 0x02, 0xe3, 0x39, 0xe3, 0x39, 0xe3,
			  0x39, 0x01, intensity, 0x05, 0xe3, 0x39, 0xe3 };
	u8 footer[9] = { 0x39, 0xe3, 0x39, 0xe3, 0xe3, 0x02, 0x04, 0x39, 0x39 };
	size_t footer_size = ARRAY_SIZE(footer);
	size_t header_size = ARRAY_SIZE(header);
	u8 data[59] = { 0x0 };
	size_t data_size = ARRAY_SIZE(data);

	memcpy(data, header, header_size);
	memcpy(data + data_size - footer_size, footer, footer_size);

	return oxp_gen_2_property_out(cfg, OXP_FID_GEN2_RUMBLE_SET, data, data_size);
}

static ssize_t rumble_intensity_store(struct device *dev,
				      struct device_attribute *attr, const char *buf,
				      size_t count)
{
	struct oxp_hid_cfg *cfg = dev_get_drvdata(dev);
	int ret;
	u8 val;

	ret = kstrtou8(buf, 10, &val);
	if (ret)
		return ret;

	if (val < 0 || val > 5)
		return -EINVAL;

	ret = oxp_rumble_intensity_set(cfg, val);
	if (ret)
		return ret;

	cfg->rumble_intensity = val;

	return count;
}

static ssize_t rumble_intensity_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct oxp_hid_cfg *cfg = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%i\n", cfg->rumble_intensity);
}
static DEVICE_ATTR_RW(rumble_intensity);

static ssize_t rumble_intensity_range_show(struct device *dev,
					   struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "0-5\n");
}
static DEVICE_ATTR_RO(rumble_intensity_range);

static struct oxp_attr button_a = { BUTTON_A };
OXP_DEVICE_ATTR_RW(button_a, map_button);

static struct oxp_attr button_b = { BUTTON_B };
OXP_DEVICE_ATTR_RW(button_b, map_button);

static struct oxp_attr button_x = { BUTTON_X };
OXP_DEVICE_ATTR_RW(button_x, map_button);

static struct oxp_attr button_y = { BUTTON_Y };
OXP_DEVICE_ATTR_RW(button_y, map_button);

static struct oxp_attr button_lb = { BUTTON_LB };
OXP_DEVICE_ATTR_RW(button_lb, map_button);

static struct oxp_attr button_rb = { BUTTON_RB };
OXP_DEVICE_ATTR_RW(button_rb, map_button);

static struct oxp_attr button_lt = { BUTTON_LT };
OXP_DEVICE_ATTR_RW(button_lt, map_button);

static struct oxp_attr button_rt = { BUTTON_RT };
OXP_DEVICE_ATTR_RW(button_rt, map_button);

static struct oxp_attr button_start = { BUTTON_START };
OXP_DEVICE_ATTR_RW(button_start, map_button);

static struct oxp_attr button_select = { BUTTON_SELECT };
OXP_DEVICE_ATTR_RW(button_select, map_button);

static struct oxp_attr button_l3 = { BUTTON_L3 };
OXP_DEVICE_ATTR_RW(button_l3, map_button);

static struct oxp_attr button_r3 = { BUTTON_R3 };
OXP_DEVICE_ATTR_RW(button_r3, map_button);

static struct oxp_attr button_d_up = { BUTTON_DUP };
OXP_DEVICE_ATTR_RW(button_d_up, map_button);

static struct oxp_attr button_d_down = { BUTTON_DDOWN };
OXP_DEVICE_ATTR_RW(button_d_down, map_button);

static struct oxp_attr button_d_left = { BUTTON_DLEFT };
OXP_DEVICE_ATTR_RW(button_d_left, map_button);

static struct oxp_attr button_d_right = { BUTTON_DRIGHT };
OXP_DEVICE_ATTR_RW(button_d_right, map_button);

static struct oxp_attr button_m1 = { BUTTON_M1 };
OXP_DEVICE_ATTR_RW(button_m1, map_button);

static struct oxp_attr button_m2 = { BUTTON_M2 };
OXP_DEVICE_ATTR_RW(button_m2, map_button);

static struct attribute *oxp_cfg_attrs[] = {
	&dev_attr_button_a.attr,
	&dev_attr_button_b.attr,
	&dev_attr_button_d_down.attr,
	&dev_attr_button_d_left.attr,
	&dev_attr_button_d_right.attr,
	&dev_attr_button_d_up.attr,
	&dev_attr_button_l3.attr,
	&dev_attr_button_lb.attr,
	&dev_attr_button_lt.attr,
	&dev_attr_button_m1.attr,
	&dev_attr_button_m2.attr,
	&dev_attr_button_mapping_options.attr,
	&dev_attr_button_r3.attr,
	&dev_attr_button_rb.attr,
	&dev_attr_button_rt.attr,
	&dev_attr_button_select.attr,
	&dev_attr_button_start.attr,
	&dev_attr_button_x.attr,
	&dev_attr_button_y.attr,
	&dev_attr_gamepad_mode.attr,
	&dev_attr_gamepad_mode_index.attr,
	&dev_attr_reset_buttons.attr,
	&dev_attr_rumble_intensity.attr,
	&dev_attr_rumble_intensity_range.attr,
	NULL,
};

static const struct attribute_group oxp_cfg_attrs_group = {
	.attrs = oxp_cfg_attrs,
};

static int oxp_gen_3_rgb_status_store(struct oxp_rgb_led *led, u8 enabled,
				      u8 speed, u8 brightness)
{
	struct oxp_rgb_full_state *state = oxp_rgb_full_state(led);
	u8 data[6] = { OXP_SET_PROPERTY, 0x00, 0x02, enabled, speed, brightness };

	if (!state)
		return -ENODEV;
	if (state->effect == OXP_EFFECT_MONO_LIST)
		data[5] = 0x04;

	return oxp_gen_3_rgb_property_out(led->cfg, data, sizeof(data));
}

static int oxp_rgb_status_store(struct oxp_rgb_led *led, u8 enabled, u8 speed,
				u8 brightness)
{
	struct oxp_rgb_full_state *state = oxp_rgb_full_state(led);
	struct oxp_hid_cfg *cfg = led->cfg;
	u16 up = get_usage_page(cfg->hdev);
	u8 *data;

	if (!state)
		return -ENODEV;
	if (cfg->x2_rgb)
		return oxp_gen_3_rgb_status_store(led, enabled, speed, brightness);

	/* Always default to max brightness and use intensity scaling when in
	 * monocolor mode.
	 */
	switch (up) {
	case GEN1_USAGE_PAGE:
		data = (u8[4]) { OXP_SET_PROPERTY, enabled, speed, brightness };
		if (state->effect == OXP_EFFECT_MONO_LIST)
			data[3] = 0x04;
		return oxp_gen_1_property_out(cfg, OXP_FID_GEN1_RGB_SET, data, 4);
	case GEN2_USAGE_PAGE:
		data = (u8[6]) { OXP_SET_PROPERTY, 0x00, 0x02, enabled, speed, brightness };
		if (state->effect == OXP_EFFECT_MONO_LIST)
			data[5] = 0x04;
		return oxp_gen_2_property_out(cfg, OXP_FID_GEN2_STATUS_EVENT, data, 6);
	default:
		return -ENODEV;
	}
}

static int oxp_gen_3_rgb_status_show(struct oxp_hid_cfg *cfg)
{
	u8 data[3] = { OXP_GET_PROPERTY, 0x07, 0x02 };

	return oxp_gen_3_property_out(cfg, data, sizeof(data));
}

static ssize_t oxp_rgb_status_show(struct oxp_rgb_led *led)
{
	struct oxp_hid_cfg *cfg = led->cfg;
	u16 up = get_usage_page(cfg->hdev);
	u8 *data;

	if (!oxp_rgb_full_state(led))
		return -ENODEV;

	guard(mutex)(&cfg->rgb_mutex);
	if (cfg->x2_rgb)
		return oxp_gen_3_rgb_status_show(cfg);

	switch (up) {
	case GEN1_USAGE_PAGE:
		data = (u8[1]) { OXP_GET_PROPERTY };
		return oxp_gen_1_property_out(cfg, OXP_FID_GEN1_RGB_SET, data, 1);
	case GEN2_USAGE_PAGE:
		data = (u8[3]) { OXP_GET_PROPERTY, 0x00, 0x02 };
		return oxp_gen_2_property_out(cfg, OXP_FID_GEN2_STATUS_EVENT, data, 3);
	default:
		return -ENODEV;
	}
}

static int oxp_gen_3_rgb_color_set(struct oxp_rgb_led *led)
{
	struct led_classdev_mc *mc_cdev = &led->mc_cdev;
	struct oxp_gen_3_rgb_color_report color_report;
	u8 brightness = mc_cdev->led_cdev.brightness;

	led_mc_calc_color_components(mc_cdev, brightness);
	oxp_gen_3_rgb_fill_color(&color_report, OXP_EFFECT_MONO_TRUE, 0x00,
				 mc_cdev->subled_info[0].brightness,
				 mc_cdev->subled_info[1].brightness,
				 mc_cdev->subled_info[2].brightness);

	return oxp_gen_3_rgb_property_out(led->cfg, (u8 *)&color_report,
					  sizeof(color_report));
}

static int oxp_rgb_color_set(struct oxp_rgb_led *led)
{
	struct led_classdev_mc *led_mc = &led->mc_cdev;
	struct oxp_hid_cfg *cfg = led->cfg;
	u16 up = get_usage_page(cfg->hdev);
	u8 br = led_mc->led_cdev.brightness;
	u8 green, red, blue;
	size_t size;
	u8 *data;
	int i;

	if (cfg->x2_rgb)
		return oxp_gen_3_rgb_color_set(led);

	led_mc_calc_color_components(led_mc, br);
	red = led_mc->subled_info[0].brightness;
	green = led_mc->subled_info[1].brightness;
	blue = led_mc->subled_info[2].brightness;

	switch (up) {
	case GEN1_USAGE_PAGE:
		size = 55;
		data = (u8[55]) { OXP_EFFECT_MONO_TRUE };

		for (i = 0; i < (size - 1) / 3; i++) {
			data[3 * i + 1] = red;
			data[3 * i + 2] = green;
			data[3 * i + 3] = blue;
		}
		return oxp_gen_1_property_out(cfg, OXP_FID_GEN1_RGB_SET, data, size);
	case GEN2_USAGE_PAGE:
		/* Preserve the legacy payload and its two zero bytes before the footer. */
		size = 57;
		data = (u8[57]) { OXP_EFFECT_MONO_TRUE, 0x00, 0x02 };

		for (i = 1; i < size / 3; i++) {
			data[3 * i] = red;
			data[3 * i + 1] = green;
			data[3 * i + 2] = blue;
		}
		return oxp_gen_2_property_out(cfg, OXP_FID_GEN2_STATUS_EVENT, data, size);
	default:
		return -ENODEV;
	}
}

static int oxp_gen_3_rgb_effect_set(struct oxp_rgb_led *led, u8 effect)
{
	struct oxp_rgb_full_state *state = oxp_rgb_full_state(led);
	u8 data[3] = { effect, 0x00, 0x02 };
	int ret;

	if (!state)
		return -ENODEV;

	if (effect > OXP_UNKNOWN && effect < OXP_EFFECT_MONO_LIST)
		ret = oxp_gen_3_rgb_property_out(led->cfg, data, sizeof(data));
	else if (effect == OXP_EFFECT_MONO_LIST)
		ret = oxp_gen_3_rgb_color_set(led);
	else
		return -EINVAL;

	if (ret)
		return ret;

	state->effect = effect;
	return 0;
}

static int oxp_rgb_effect_set(struct oxp_rgb_led *led, u8 effect)
{
	struct oxp_rgb_full_state *state = oxp_rgb_full_state(led);
	struct oxp_hid_cfg *cfg = led->cfg;
	u16 up = get_usage_page(cfg->hdev);
	u8 *data;
	int ret;

	if (!state)
		return -ENODEV;
	if (cfg->x2_rgb)
		return oxp_gen_3_rgb_effect_set(led, effect);

	switch (effect) {
	case OXP_EFFECT_AURORA:
	case OXP_EFFECT_BIRTHDAY:
	case OXP_EFFECT_FLOWING:
	case OXP_EFFECT_CHROMA_1:
	case OXP_EFFECT_NEON:
	case OXP_EFFECT_CHROMA_2:
	case OXP_EFFECT_DREAMY:
	case OXP_EFFECT_WARM:
	case OXP_EFFECT_CYBERPUNK:
	case OXP_EFFECT_SEA:
	case OXP_EFFECT_SUNSET:
	case OXP_EFFECT_COLORFUL:
	case OXP_EFFECT_MONSTER:
	case OXP_EFFECT_GREEN:
	case OXP_EFFECT_BLUE:
	case OXP_EFFECT_YELLOW:
	case OXP_EFFECT_TEAL:
	case OXP_EFFECT_PURPLE:
	case OXP_EFFECT_FOGGY:
		switch (up) {
		case GEN1_USAGE_PAGE:
			data = (u8[1]) { effect };
			ret = oxp_gen_1_property_out(cfg, OXP_FID_GEN1_RGB_SET, data, 1);
			break;
		case GEN2_USAGE_PAGE:
			data = (u8[3]) { effect, 0x00, 0x02 };
			ret = oxp_gen_2_property_out(cfg, OXP_FID_GEN2_STATUS_EVENT, data, 3);
			break;
		default:
			ret = -ENODEV;
		}
		break;
	case OXP_EFFECT_MONO_LIST:
		ret = oxp_rgb_color_set(led);
		break;
	default:
		return -EINVAL;
	}

	if (ret)
		return ret;

	state->effect = effect;

	return 0;
}

static struct oxp_rgb_led *oxp_rgb_led_from_dev(struct device *dev)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(led_cdev);

	return container_of(mc_cdev, struct oxp_rgb_led, mc_cdev);
}

static ssize_t enabled_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct oxp_rgb_led *led = oxp_rgb_led_from_dev(dev);
	struct oxp_hid_cfg *cfg = led->cfg;
	struct oxp_rgb_full_state *state = oxp_rgb_full_state(led);
	int ret;
	u8 val;

	if (!state)
		return -ENODEV;

	ret = sysfs_match_string(oxp_feature_en_text, buf);
	if (ret < 0)
		return ret;
	val = ret;

	guard(mutex)(&cfg->rgb_mutex);

	ret = oxp_rgb_status_store(led, val, state->speed, state->brightness);
	if (ret)
		return ret;

	state->enabled = val;
	return count;
}

static ssize_t enabled_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct oxp_rgb_led *led = oxp_rgb_led_from_dev(dev);
	struct oxp_rgb_full_state *state = oxp_rgb_full_state(led);
	int ret;

	if (!state)
		return -ENODEV;

	ret = oxp_rgb_status_show(led);
	if (ret)
		return ret;

	if (state->enabled >= ARRAY_SIZE(oxp_feature_en_text))
		return -EINVAL;

	return sysfs_emit(buf, "%s\n", oxp_feature_en_text[state->enabled]);
}
static DEVICE_ATTR_RW(enabled);

static ssize_t enabled_index_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	size_t count = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(oxp_feature_en_text); i++)
		count += sysfs_emit_at(buf, count, "%s ", oxp_feature_en_text[i]);

	if (count)
		buf[count - 1] = '\n';

	return count;
}
static DEVICE_ATTR_RO(enabled_index);

static ssize_t effect_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct oxp_rgb_led *led = oxp_rgb_led_from_dev(dev);
	struct oxp_hid_cfg *cfg = led->cfg;
	struct oxp_rgb_full_state *state = oxp_rgb_full_state(led);
	u8 old_effect;
	int ret;
	u8 val;

	if (!state)
		return -ENODEV;

	ret = sysfs_match_string(oxp_rgb_effect_text, buf);
	if (ret < 0)
		return ret;

	val = ret;

	guard(mutex)(&cfg->rgb_mutex);
	old_effect = state->effect;
	state->effect = val;

	ret = oxp_rgb_status_store(led, state->enabled, state->speed,
				   state->brightness);
	if (ret) {
		state->effect = old_effect;
		return ret;
	}

	ret = oxp_rgb_effect_set(led, val);
	if (ret) {
		state->effect = old_effect;
		return ret;
	}

	return count;
}

static ssize_t effect_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct oxp_rgb_led *led = oxp_rgb_led_from_dev(dev);
	struct oxp_rgb_full_state *state = oxp_rgb_full_state(led);
	int ret;

	if (!state)
		return -ENODEV;

	ret = oxp_rgb_status_show(led);
	if (ret)
		return ret;

	if (state->effect >= ARRAY_SIZE(oxp_rgb_effect_text))
		return -EINVAL;

	return sysfs_emit(buf, "%s\n", oxp_rgb_effect_text[state->effect]);
}

static DEVICE_ATTR_RW(effect);

static ssize_t effect_index_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	size_t count = 0;
	unsigned int i;

	for (i = 1; i < ARRAY_SIZE(oxp_rgb_effect_text); i++)
		count += sysfs_emit_at(buf, count, "%s ", oxp_rgb_effect_text[i]);

	if (count)
		buf[count - 1] = '\n';

	return count;
}
static DEVICE_ATTR_RO(effect_index);

static ssize_t speed_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct oxp_rgb_led *led = oxp_rgb_led_from_dev(dev);
	struct oxp_hid_cfg *cfg = led->cfg;
	struct oxp_rgb_full_state *state = oxp_rgb_full_state(led);
	int ret;
	u8 val;

	if (!state)
		return -ENODEV;

	ret = kstrtou8(buf, 10, &val);
	if (ret)
		return ret;

	if (val > 9)
		return -EINVAL;

	guard(mutex)(&cfg->rgb_mutex);

	ret = oxp_rgb_status_store(led, state->enabled, val, state->brightness);
	if (ret)
		return ret;

	state->speed = val;
	return count;
}

static ssize_t speed_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct oxp_rgb_led *led = oxp_rgb_led_from_dev(dev);
	struct oxp_rgb_full_state *state = oxp_rgb_full_state(led);
	int ret;

	if (!state)
		return -ENODEV;

	ret = oxp_rgb_status_show(led);
	if (ret)
		return ret;

	if (state->speed > 9)
		return -EINVAL;

	return sysfs_emit(buf, "%hhu\n", state->speed);
}
static DEVICE_ATTR_RW(speed);

static ssize_t speed_range_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "0-9\n");
}
static DEVICE_ATTR_RO(speed_range);

static void oxp_rgb_full_queue(struct oxp_rgb_led *led,
			       struct oxp_rgb_full_state *state)
{
	unsigned int max_brightness = led->mc_cdev.led_cdev.max_brightness;
	unsigned int brightness = led->mc_cdev.led_cdev.brightness;
	struct oxp_hid_cfg *cfg = led->cfg;
	u8 val = 4 * brightness / max_brightness;
	int ret;

	guard(mutex)(&cfg->rgb_mutex);

	if (state->brightness != val) {
		ret = oxp_rgb_status_store(led, state->enabled, state->speed, val);
		if (ret)
			dev_err(led->mc_cdev.led_cdev.dev,
				"Error: Failed to write RGB Status: %i\n", ret);

		state->brightness = val;
	}

	if (state->effect != OXP_EFFECT_MONO_LIST)
		return;

	ret = oxp_rgb_effect_set(led, state->effect);
	if (ret)
		dev_err(led->mc_cdev.led_cdev.dev,
			"Error: Failed to write RGB color: %i\n", ret);
}

static void oxp_gen_3_rgb_queue(struct oxp_rgb_led *led,
				struct oxp_rgb_full_state *state)
{
	unsigned int max_brightness = led->mc_cdev.led_cdev.max_brightness;
	u8 old_effect;
	unsigned int brightness = led->mc_cdev.led_cdev.brightness;
	u8 enabled = brightness ? OXP_FEAT_ENABLED : OXP_FEAT_DISABLED;
	struct oxp_hid_cfg *cfg = led->cfg;
	u8 val = 4 * brightness / max_brightness;
	int ret;

	guard(mutex)(&cfg->rgb_mutex);
	old_effect = state->effect;

	if (!brightness) {
		ret = oxp_gen_3_rgb_status_store(led, OXP_FEAT_DISABLED, 5, 0);
		if (ret) {
			dev_err(led->mc_cdev.led_cdev.dev,
				"Error: Failed to disable RGB: %i\n", ret);
			return;
		}

		state->enabled = OXP_FEAT_DISABLED;
		state->brightness = 0;
		state->speed = 5;
		return;
	}

	/* Gen3 standard LED writes select solid color and follow brightness/off. */
	state->effect = OXP_EFFECT_MONO_LIST;
	ret = oxp_gen_3_rgb_status_store(led, enabled, 5, val);
	if (ret) {
		state->effect = old_effect;
		dev_err(led->mc_cdev.led_cdev.dev,
			"Error: Failed to write RGB status: %i\n", ret);
		return;
	}

	state->enabled = enabled;
	state->brightness = val;
	state->speed = 5;

	ret = oxp_gen_3_rgb_effect_set(led, OXP_EFFECT_MONO_LIST);
	if (ret)
		dev_err(led->mc_cdev.led_cdev.dev,
			"Error: Failed to write RGB color: %i\n", ret);
}

static void oxp_rgb_aux_queue(struct oxp_rgb_led *led)
{
	struct oxp_hid_cfg *cfg = led->cfg;
	struct oxp_rgb_aux_state state;
	int ret;

	if (!oxp_rgb_aux_snapshot(led, &state))
		return;

	guard(mutex)(&cfg->rgb_mutex);
	if (READ_ONCE(cfg->suspended) || READ_ONCE(cfg->removing))
		return;

	ret = oxp_rgb_aux_apply(led, &state);
	if (ret)
		dev_err(led->mc_cdev.led_cdev.dev,
			"Failed to write RGB zone %#04x: %i\n", led->zone, ret);
}

static void oxp_rgb_queue_fn(struct work_struct *work)
{
	struct oxp_rgb_led *led = container_of(to_delayed_work(work),
					       struct oxp_rgb_led, work);
	struct oxp_hid_cfg *cfg = led->cfg;

	if (READ_ONCE(cfg->suspended) || READ_ONCE(cfg->removing))
		return;

	switch (led->type) {
	case OXP_RGB_FULL:
		if (cfg->x2_rgb)
			oxp_gen_3_rgb_queue(led, led->state);
		else
			oxp_rgb_full_queue(led, led->state);
		break;
	case OXP_RGB_AUX:
		oxp_rgb_aux_queue(led);
		break;
	}
}

static void oxp_rgb_brightness_set(struct led_classdev *led_cdev,
				   enum led_brightness brightness)
{
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(led_cdev);
	struct oxp_rgb_led *led = container_of(mc_cdev, struct oxp_rgb_led,
					       mc_cdev);
	struct oxp_hid_cfg *cfg = led->cfg;
	struct oxp_rgb_aux_state *state;

	if (READ_ONCE(cfg->removing))
		return;

	switch (led->type) {
	case OXP_RGB_FULL:
		if (READ_ONCE(cfg->suspended))
			return;
		led_cdev->brightness = brightness;
		break;
	case OXP_RGB_AUX:
		state = led->state;
		led_cdev->brightness = brightness;
		scoped_guard(spinlock_irqsave, &led->state_lock) {
			state->brightness = brightness;
			state->red = led->subled_info[0].intensity;
			state->green = led->subled_info[1].intensity;
			state->blue = led->subled_info[2].intensity;
			state->valid = true;
		}
		if (READ_ONCE(cfg->suspended))
			return;
		break;
	default:
		return;
	}

	mod_delayed_work(system_dfl_wq, &led->work, msecs_to_jiffies(50));
}

static ssize_t oxp_rgb_aux_effect_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct oxp_rgb_led *led = oxp_rgb_led_from_dev(dev);
	struct oxp_hid_cfg *cfg = led->cfg;
	struct oxp_rgb_aux_state *state = oxp_rgb_aux_state(led);
	int ret;

	if (!state || READ_ONCE(cfg->removing))
		return -ENODEV;

	ret = sysfs_match_string(oxp_rgb_aux_effect_text, buf);
	if (ret < 0)
		return ret;

	scoped_guard(spinlock_irqsave, &led->state_lock) {
		state->effect = ret;
		state->valid = true;
	}

	if (!READ_ONCE(cfg->suspended) && !READ_ONCE(cfg->removing))
		mod_delayed_work(system_dfl_wq, &led->work,
				 msecs_to_jiffies(50));

	return count;
}

static ssize_t oxp_rgb_aux_effect_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct oxp_rgb_led *led = oxp_rgb_led_from_dev(dev);
	struct oxp_hid_cfg *cfg = led->cfg;
	struct oxp_rgb_aux_state state;

	if (!oxp_rgb_aux_state(led) || READ_ONCE(cfg->removing))
		return -ENODEV;

	oxp_rgb_aux_snapshot(led, &state);
	if (state.effect >= ARRAY_SIZE(oxp_rgb_aux_effect_text))
		return -EINVAL;

	return sysfs_emit(buf, "%s\n", oxp_rgb_aux_effect_text[state.effect]);
}
static DEVICE_ATTR_RW_NAMED(oxp_rgb_aux_effect, "effect");

static ssize_t oxp_rgb_aux_effect_index_show(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	int count = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(oxp_rgb_aux_effect_text); i++)
		count += sysfs_emit_at(buf, count, "%s ",
				       oxp_rgb_aux_effect_text[i]);
	if (count)
		buf[count - 1] = '\n';

	return count;
}
static DEVICE_ATTR_RO_NAMED(oxp_rgb_aux_effect_index, "effect_index");

static struct attribute *oxp_rgb_aux_attrs[] = {
	&dev_attr_oxp_rgb_aux_effect.attr,
	&dev_attr_oxp_rgb_aux_effect_index.attr,
	NULL,
};

static const struct attribute_group oxp_rgb_aux_attr_group = {
	.attrs = oxp_rgb_aux_attrs,
};

static struct attribute *oxp_rgb_attrs[] = {
	&dev_attr_effect.attr,
	&dev_attr_effect_index.attr,
	&dev_attr_enabled.attr,
	&dev_attr_enabled_index.attr,
	&dev_attr_speed.attr,
	&dev_attr_speed_range.attr,
	NULL,
};

static const struct attribute_group oxp_rgb_attr_group = {
	.attrs = oxp_rgb_attrs,
};

static const struct oxp_rgb_led_desc oxp_rgb_led_descs[] = {
	{
		.name = "oxp:rgb:joystick_rings",
		.type = OXP_RGB_FULL,
	},
	{
		.name = "oxp:rgb:guide_button",
		.type = OXP_RGB_AUX,
		.zone = 0x05,
	},
	{
		.name = "oxp:rgb:rear_logo",
		.type = OXP_RGB_AUX,
		.zone = 0x06,
	},
};

static int oxp_rgb_led_init(struct oxp_hid_cfg *cfg, struct oxp_rgb_led *led,
			    const struct oxp_rgb_led_desc *desc)
{
	struct oxp_rgb_full_state *full_state;
	struct oxp_rgb_aux_state *aux_state;
	struct hid_device *hdev = cfg->hdev;
	u8 green;
	u8 blue;
	u8 red;

	led->cfg = cfg;
	led->type = desc->type;
	led->zone = desc->zone;

	switch (led->type) {
	case OXP_RGB_FULL:
		full_state = devm_kzalloc(&hdev->dev, sizeof(*full_state),
					  GFP_KERNEL);
		if (!full_state)
			return -ENOMEM;
		led->state = full_state;
		led->mc_cdev.led_cdev.brightness = 0x64;
		red = 0x24;
		green = 0x22;
		blue = 0x99;
		break;
	case OXP_RGB_AUX:
		aux_state = devm_kzalloc(&hdev->dev, sizeof(*aux_state),
					 GFP_KERNEL);
		if (!aux_state)
			return -ENOMEM;
		aux_state->red = 0xff;
		aux_state->green = 0xff;
		aux_state->blue = 0xff;
		aux_state->effect = OXP_RGB_AUX_EFFECT_MONOCOLOR;
		led->state = aux_state;
		red = 0xff;
		green = 0xff;
		blue = 0xff;
		break;
	default:
		return -EINVAL;
	}

	led->subled_info[0] = (struct mc_subled) {
		.color_index = LED_COLOR_ID_RED,
		.intensity = red,
		.max_intensity = 0xff,
		.channel = 0x1,
	};
	led->subled_info[1] = (struct mc_subled) {
		.color_index = LED_COLOR_ID_GREEN,
		.intensity = green,
		.max_intensity = 0xff,
		.channel = 0x2,
	};
	led->subled_info[2] = (struct mc_subled) {
		.color_index = LED_COLOR_ID_BLUE,
		.intensity = blue,
		.max_intensity = 0xff,
		.channel = 0x3,
	};
	led->mc_cdev.led_cdev.name = desc->name;
	led->mc_cdev.led_cdev.color = LED_COLOR_ID_RGB;
	led->mc_cdev.led_cdev.max_brightness = 0x64;
	led->mc_cdev.led_cdev.brightness_set = oxp_rgb_brightness_set;
	led->mc_cdev.num_colors = ARRAY_SIZE(led->subled_info);
	led->mc_cdev.subled_info = led->subled_info;
	spin_lock_init(&led->state_lock);
	INIT_DELAYED_WORK(&led->work, oxp_rgb_queue_fn);

	return 0;
}

static int oxp_rgb_leds_register(struct oxp_hid_cfg *cfg)
{
	int led_count = cfg->x2_rgb ? ARRAY_SIZE(oxp_rgb_led_descs) : 1;
	const struct attribute_group *attr_group;
	struct hid_device *hdev = cfg->hdev;
	struct oxp_rgb_led *led;
	int ret;
	int i;

	cfg->rgb_leds = devm_kcalloc(&hdev->dev, led_count,
				     sizeof(*cfg->rgb_leds), GFP_KERNEL);
	if (!cfg->rgb_leds)
		return -ENOMEM;

	for (i = 0; i < led_count; i++) {
		led = &cfg->rgb_leds[i];
		ret = oxp_rgb_led_init(cfg, led, &oxp_rgb_led_descs[i]);
		if (ret)
			return ret;
		cfg->rgb_led_count++;

		ret = devm_led_classdev_multicolor_register(&hdev->dev,
							    &led->mc_cdev);
		if (ret)
			return dev_err_probe(&hdev->dev, ret,
					     "Failed to create RGB device\n");

		switch (led->type) {
		case OXP_RGB_FULL:
			attr_group = &oxp_rgb_attr_group;
			break;
		case OXP_RGB_AUX:
			attr_group = &oxp_rgb_aux_attr_group;
			break;
		default:
			return -EINVAL;
		}

		ret = devm_device_add_group(led->mc_cdev.led_cdev.dev,
					    attr_group);
		if (ret)
			return dev_err_probe(led->mc_cdev.led_cdev.dev, ret,
					     "Failed to create RGB configuration attributes\n");
	}

	return 0;
}

static void oxp_rgb_disable_works(struct oxp_hid_cfg *cfg)
{
	int i;

	for (i = 0; i < cfg->rgb_led_count; i++)
		disable_delayed_work_sync(&cfg->rgb_leds[i].work);
}

static void oxp_rgb_enable_works(struct oxp_hid_cfg *cfg)
{
	int i;

	for (i = 0; i < cfg->rgb_led_count; i++)
		enable_delayed_work(&cfg->rgb_leds[i].work);
}

static struct quirk_entry quirk_hybrid_mcu = {
	.hybrid_mcu = true,
};

static struct quirk_entry quirk_x2 = {
	.bmap_format = OXP_BMAP_FORMAT_X2,
	.bmap_page_3 = true,
	.x2_rgb = true,
	.cfg_interface_num = 2,
};

static const struct dmi_system_id oxp_quirk_list[] = {
	{
		.ident = "OneXPlayer Apex",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ONE-NETBOOK"),
			DMI_MATCH(DMI_PRODUCT_NAME, "ONEXPLAYER APEX"),
		},
		.driver_data = &quirk_hybrid_mcu,
	},
	{
		.ident = "OneXPlayer G1 AMD",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ONE-NETBOOK"),
			DMI_MATCH(DMI_PRODUCT_NAME, "ONEXPLAYER G1 A"),
		},
		.driver_data = &quirk_hybrid_mcu,
	},
	{
		.ident = "OneXPlayer G1 Intel",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ONE-NETBOOK"),
			DMI_MATCH(DMI_PRODUCT_NAME, "ONEXPLAYER G1 i"),
		},
		.driver_data = &quirk_hybrid_mcu,
	},
	{
		.ident = "OneXPlayer 3",
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "ONE-NETBOOK"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "ONEXPLAYER 3"),
		},
		.driver_data = &quirk_x2,
	},
	{
		.ident = "OneXPlayer X2 Mini Pro",
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "ONE-NETBOOK"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "ONEXPLAYER X2Mini PRO"),
		},
		.driver_data = &quirk_x2,
	},
	{},
};

static const struct quirk_entry *oxp_get_quirks(void)
{
	const struct dmi_system_id *dmi_id;

	dmi_id = dmi_first_match(oxp_quirk_list);
	if (!dmi_id)
		return NULL;

	return dmi_id->driver_data;
}

static void oxp_drain_output(struct oxp_hid_cfg *cfg)
{
	/* Wait for any in-flight sysfs output before closing the transport. */
	guard(mutex)(&cfg->cfg_mutex);
}

static void oxp_quiesce_work(struct oxp_hid_cfg *cfg)
{
	WRITE_ONCE(cfg->removing, true);
	scoped_guard(spinlock_irqsave, &cfg->rgb_reply_lock) {
		cfg->rgb_reply_pending = false;
	}
	oxp_rgb_disable_works(cfg);
	if (cfg->gen2_work_initialized) {
		disable_delayed_work_sync(&cfg->oxp_btn_queue);
		disable_delayed_work_sync(&cfg->oxp_mcu_init);
	}
	oxp_drain_output(cfg);
}

static bool oxp_is_cfg_interface(struct hid_device *hdev,
				 const struct quirk_entry *quirks)
{
	struct usb_interface *intf;

	if (!quirks || !quirks->cfg_interface_num)
		return true;
	if (hdev->bus != BUS_USB)
		return false;

	intf = to_usb_interface(hdev->dev.parent);
	return intf->cur_altsetting->desc.bInterfaceNumber ==
	       quirks->cfg_interface_num;
}

static void oxp_cfg_release(void *data)
{
	struct oxp_hid_cfg *cfg = data;

	hid_set_drvdata(cfg->hdev, NULL);
}

static int oxp_cfg_probe(struct hid_device *hdev, u16 up,
			 const struct quirk_entry *quirks)
{
	struct oxp_bmap_page_1 *bmap_1;
	struct oxp_bmap_page_2 *bmap_2;
	struct oxp_rgb_led *rgb_led;
	struct oxp_hid_cfg *cfg;
	int ret;

	cfg = devm_kzalloc(&hdev->dev, sizeof(*cfg), GFP_KERNEL);
	if (!cfg)
		return -ENOMEM;

	cfg->hdev = hdev;
	cfg->x2_rgb = quirks && quirks->x2_rgb;
	mutex_init(&cfg->cfg_mutex);
	mutex_init(&cfg->rgb_mutex);
	spin_lock_init(&cfg->rgb_reply_lock);

	/* Clear drvdata after registered callback objects have been released. */
	hid_set_drvdata(hdev, cfg);
	ret = devm_add_action_or_reset(&hdev->dev, oxp_cfg_release, cfg);
	if (ret)
		return ret;

	if (up == GEN2_USAGE_PAGE && quirks && quirks->hybrid_mcu)
		goto skip_rgb;

	ret = oxp_rgb_leds_register(cfg);
	if (ret)
		goto err_quiesce;

	rgb_led = oxp_rgb_led_by_type(cfg, OXP_RGB_FULL);
	ret = oxp_rgb_status_show(rgb_led);
	if (ret)
		dev_warn(rgb_led->mc_cdev.led_cdev.dev,
			 "Failed to query RGB initial state: %i\n", ret);

	/* Below features are only implemented in gen 2 */
	if (up != GEN2_USAGE_PAGE)
		return 0;

skip_rgb:
	bmap_1 = devm_kzalloc(&hdev->dev, sizeof(struct oxp_bmap_page_1), GFP_KERNEL);
	if (!bmap_1) {
		ret = dev_err_probe(&hdev->dev, -ENOMEM,
				    "Unable to allocate button map page 1\n");
		goto err_quiesce;
	}

	bmap_2 = devm_kzalloc(&hdev->dev, sizeof(struct oxp_bmap_page_2), GFP_KERNEL);
	if (!bmap_2) {
		ret = dev_err_probe(&hdev->dev, -ENOMEM,
				    "Unable to allocate button map page 2\n");
		goto err_quiesce;
	}

	cfg->bmap_1 = bmap_1;
	cfg->bmap_2 = bmap_2;
	cfg->bmap_format = quirks && quirks->bmap_format ?
			   quirks->bmap_format : OXP_BMAP_FORMAT_DEFAULT;
	cfg->bmap_page_3 = quirks && quirks->bmap_page_3;
	oxp_reset_buttons(cfg);
	INIT_DELAYED_WORK(&cfg->oxp_btn_queue, oxp_btn_queue_fn);

	cfg->gamepad_mode = OXP_GP_MODE_XINPUT;
	cfg->rumble_intensity = 5;

	INIT_DELAYED_WORK(&cfg->oxp_mcu_init, oxp_mcu_init_fn);
	WRITE_ONCE(cfg->gen2_work_initialized, true);
	mod_delayed_work(system_dfl_wq, &cfg->oxp_mcu_init, msecs_to_jiffies(50));

	ret = devm_device_add_group(&hdev->dev, &oxp_cfg_attrs_group);
	if (ret) {
		dev_err_probe(&hdev->dev, ret,
			      "Failed to attach configuration attributes\n");
		goto err_quiesce;
	}

	return 0;

err_quiesce:
	oxp_quiesce_work(cfg);
	return ret;
}

static int oxp_hid_probe(struct hid_device *hdev,
			 const struct hid_device_id *id)
{
	const struct quirk_entry *quirks;
	int ret;
	u16 up;

	ret = hid_parse(hdev);
	if (ret)
		return dev_err_probe(&hdev->dev, ret, "Failed to parse HID device\n");

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret)
		return dev_err_probe(&hdev->dev, ret, "Failed to start HID device\n");

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_hw_stop(hdev);
		return dev_err_probe(&hdev->dev, ret, "Failed to open HID device\n");
	}

	up = get_usage_page(hdev);
	quirks = oxp_get_quirks();
	dev_dbg(&hdev->dev, "Got usage page %04x\n", up);

	switch (up) {
	case GEN1_USAGE_PAGE:
	case GEN2_USAGE_PAGE:
		if (!oxp_is_cfg_interface(hdev, quirks))
			return 0;

		ret = oxp_cfg_probe(hdev, up, quirks);
		if (ret) {
			hid_hw_close(hdev);
			hid_hw_stop(hdev);
		}

		return ret;
	default:
		return 0;
	}
}

static void oxp_hid_remove(struct hid_device *hdev)
{
	struct oxp_hid_cfg *cfg = hid_get_drvdata(hdev);

	if (cfg)
		oxp_quiesce_work(cfg);
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static int __maybe_unused oxp_hid_suspend(struct hid_device *hdev,
					  pm_message_t message)
{
	struct oxp_hid_cfg *cfg = hid_get_drvdata(hdev);

	if (!cfg || PMSG_IS_AUTO(message))
		return 0;

	WRITE_ONCE(cfg->suspended, true);
	scoped_guard(spinlock_irqsave, &cfg->rgb_reply_lock) {
		cfg->rgb_reply_pending = false;
	}
	oxp_rgb_disable_works(cfg);
	if (cfg->gen2_work_initialized) {
		disable_delayed_work_sync(&cfg->oxp_btn_queue);
		disable_delayed_work_sync(&cfg->oxp_mcu_init);
	}
	oxp_drain_output(cfg);

	return 0;
}

static int __maybe_unused oxp_hid_resume(struct hid_device *hdev)
{
	struct oxp_hid_cfg *cfg = hid_get_drvdata(hdev);

	if (!cfg || !READ_ONCE(cfg->suspended) ||
	    READ_ONCE(cfg->removing))
		return 0;

	oxp_rgb_enable_works(cfg);
	if (cfg->gen2_work_initialized) {
		enable_delayed_work(&cfg->oxp_btn_queue);
		enable_delayed_work(&cfg->oxp_mcu_init);
	}
	WRITE_ONCE(cfg->suspended, false);
	if (!cfg->gen2_work_initialized)
		return 0;

	/* Allow the controller MCU to finish rebooting before restoring state. */
	queue_delayed_work(system_dfl_wq, &cfg->oxp_mcu_init,
			   msecs_to_jiffies(6500));

	return 0;
}

static const struct hid_device_id oxp_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_CRSC, USB_DEVICE_ID_ONEXPLAYER_GEN1) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_WCH, USB_DEVICE_ID_ONEXPLAYER_GEN2) },
	{}
};

MODULE_DEVICE_TABLE(hid, oxp_devices);
static struct hid_driver hid_oxp = {
	.name = "hid-oxp",
	.id_table = oxp_devices,
	.probe = oxp_hid_probe,
	.remove = oxp_hid_remove,
	.raw_event = oxp_hid_raw_event,
	.suspend = pm_ptr(oxp_hid_suspend),
	.resume = pm_ptr(oxp_hid_resume),
	.reset_resume = pm_ptr(oxp_hid_resume),
};
module_hid_driver(hid_oxp);

MODULE_AUTHOR("Derek J. Clark <derekjohn.clark@gmail.com>");
MODULE_DESCRIPTION("Driver for OneXPlayer HID Interfaces");
MODULE_LICENSE("GPL");
