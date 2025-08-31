// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025, Sebastian Reichel
 */

#define DEBUG

#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/leds.h>
#include <linux/lockdep.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#define THINKPAD_T14S_EC_CMD_ECRD 0x02
#define THINKPAD_T14S_EC_CMD_ECWR 0x03
#define THINKPAD_T14S_EC_CMD_EVT 0xf0

#define THINKPAD_T14S_EC_REG_LED	0x0c
#define THINKPAD_T14S_EC_REG_KBD_BL1	0x0d
#define THINKPAD_T14S_EC_REG_KBD_BL2	0xe1
#define THINKPAD_T14S_EC_KBD_BL1_MASK	GENMASK_U8(7, 6)
#define THINKPAD_T14S_EC_KBD_BL1_SHIFT	6
#define THINKPAD_T14S_EC_KBD_BL2_MASK	GENMASK_U8(3, 2)
#define THINKPAD_T14S_EC_KBD_BL2_SHIFT	2
#define THINKPAD_T14S_EC_REG_AUD	0x30
#define THINKPAD_T14S_EC_MIC_MUTE_LED	BIT(5)
#define THINKPAD_T14S_EC_SPK_MUTE_LED	BIT(6)

#define THINKPAD_T14S_EC_EVT_NONE			0x00
#define THINKPAD_T14S_EC_EVT_KEY_FN_4			0x13
#define THINKPAD_T14S_EC_EVT_KEY_FN_F7			0x16
#define THINKPAD_T14S_EC_EVT_KEY_FN_SPACE		0x1F
#define THINKPAD_T14S_EC_EVT_KEY_TP_DOUBLE_TAP		0x20
#define THINKPAD_T14S_EC_EVT_AC_CONNECTED		0x26
#define THINKPAD_T14S_EC_EVT_AC_DISCONNECTED		0x27
#define THINKPAD_T14S_EC_EVT_KEY_POWER			0x28
#define THINKPAD_T14S_EC_EVT_LID_OPEN			0x2A
#define THINKPAD_T14S_EC_EVT_LID_CLOSED			0x2B
#define THINKPAD_T14S_EC_EVT_KEY_FN_F12			0x62
#define THINKPAD_T14S_EC_EVT_KEY_FN_TAB			0x63
#define THINKPAD_T14S_EC_EVT_KEY_FN_F8			0x64
#define THINKPAD_T14S_EC_EVT_KEY_FN_F10			0x65
#define THINKPAD_T14S_EC_EVT_KEY_FN_F4			0x6A
#define THINKPAD_T14S_EC_EVT_KEY_FN_D			0x6B
#define THINKPAD_T14S_EC_EVT_KEY_FN_T			0x6C
#define THINKPAD_T14S_EC_EVT_KEY_FN_H			0x6D
#define THINKPAD_T14S_EC_EVT_KEY_FN_M			0x6E
#define THINKPAD_T14S_EC_EVT_KEY_FN_L			0x6F
#define THINKPAD_T14S_EC_EVT_KEY_FN_RIGHT_SHIFT		0x71
#define THINKPAD_T14S_EC_EVT_KEY_FN_ESC			0x74
#define THINKPAD_T14S_EC_EVT_KEY_FN_N			0x79
#define THINKPAD_T14S_EC_EVT_KEY_FN_F11			0x7A
#define THINKPAD_T14S_EC_EVT_KEY_FN_G			0x7E

enum thinkpad_t14s_ec_led_status_t {
	THINKPAD_EC_LED_OFF = 0x00,
	THINKPAD_EC_LED_ON = 0x80,
	THINKPAD_EC_LED_BLINK = 0xc0,
};

struct thinkpad_t14s_ec_led_classdev {
	struct led_classdev led_classdev;
	int led;
	enum thinkpad_t14s_ec_led_status_t cache;
	struct thinkpad_t14s_ec *ec;
};

struct thinkpad_t14s_ec {
	struct regmap *regmap;
	struct device *dev;
	struct thinkpad_t14s_ec_led_classdev led_pwr_btn;
	struct thinkpad_t14s_ec_led_classdev led_chrg_orange;
	struct thinkpad_t14s_ec_led_classdev led_chrg_white;
	struct thinkpad_t14s_ec_led_classdev led_lid_logo_dot;
	struct led_classdev kbd_backlight;
	struct led_classdev led_mic_mute;
	struct led_classdev led_spk_mute;
	struct input_dev *inputdev;
};

static const struct regmap_config thinkpad_t14s_ec_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
};

static int thinkpad_t14s_ec_write(void *context, unsigned int reg,
				  unsigned int val)
{
	char buf[5] = {THINKPAD_T14S_EC_CMD_ECWR, reg, 0x00, 0x01, val};
	struct thinkpad_t14s_ec *ec = context;
	struct i2c_client *client = to_i2c_client(ec->dev);
	int ret;

	ret = i2c_master_send(client, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	return 0;
}

static int thinkpad_t14s_ec_read(void *context, unsigned int reg,
				 unsigned int *val)
{
	char buf[4] = {THINKPAD_T14S_EC_CMD_ECRD, reg, 0x00, 0x01};
	struct thinkpad_t14s_ec *ec = context;
	struct i2c_client *client = to_i2c_client(ec->dev);
	struct i2c_msg request, response;
	u8 result;
	int ret;

	request.addr = client->addr;
	request.flags = I2C_M_STOP;
	request.len = sizeof(buf);
	request.buf = buf;
	response.addr = client->addr;
	response.flags = I2C_M_RD;
	response.len = 1;
	response.buf = &result;

	i2c_lock_bus(client->adapter, I2C_LOCK_SEGMENT);

	ret = __i2c_transfer(client->adapter, &request, 1);
	if (ret < 0)
		goto out;
	ret = __i2c_transfer(client->adapter, &response, 1);
	if (ret < 0)
		goto out;

	*val = result;
	ret = 0;

out:
	i2c_unlock_bus(client->adapter, I2C_LOCK_SEGMENT);
	return ret;
}

static const struct regmap_bus thinkpad_t14s_ec_regmap_bus = {
	.reg_write = thinkpad_t14s_ec_write,
	.reg_read = thinkpad_t14s_ec_read,
};

static int thinkpad_t14s_ec_read_evt(struct thinkpad_t14s_ec *ec, u8 *val)
{
	char buf[4] = {THINKPAD_T14S_EC_CMD_EVT, 0x00, 0x00, 0x01};
	struct i2c_client *client = to_i2c_client(ec->dev);
	struct i2c_msg request, response;
	int ret;

	request.addr = client->addr;
	request.flags = I2C_M_STOP;
	request.len = sizeof(buf);
	request.buf = buf;
	response.addr = client->addr;
	response.flags = I2C_M_RD;
	response.len = 1;
	response.buf = val;

	i2c_lock_bus(client->adapter, I2C_LOCK_SEGMENT);

	ret = __i2c_transfer(client->adapter, &request, 1);
	if (ret < 0)
		goto out;
	ret = __i2c_transfer(client->adapter, &response, 1);
	if (ret < 0)
		goto out;

	ret = 0;

out:
	i2c_unlock_bus(client->adapter, I2C_LOCK_SEGMENT);
	return ret;
}

static int thinkpad_t14s_led_set_status(struct thinkpad_t14s_ec *ec,
			struct thinkpad_t14s_ec_led_classdev *led,
			const enum thinkpad_t14s_ec_led_status_t ledstatus)
{
	int ret;

	ret = regmap_write(ec->regmap, THINKPAD_T14S_EC_REG_LED,
			   led->led | ledstatus);
	if (ret < 0)
		return ret;

	led->cache = ledstatus;
	return 0;
}

static int thinkpad_t14s_led_set(struct led_classdev *led_cdev,
				 enum led_brightness brightness)
{
	struct thinkpad_t14s_ec_led_classdev *led = container_of(led_cdev,
			struct thinkpad_t14s_ec_led_classdev, led_classdev);
	enum thinkpad_t14s_ec_led_status_t new_state;

	if (brightness == LED_OFF)
		new_state = THINKPAD_EC_LED_OFF;
	else if (led->cache != THINKPAD_EC_LED_BLINK)
		new_state = THINKPAD_EC_LED_ON;
	else
		new_state = THINKPAD_EC_LED_BLINK;

	return thinkpad_t14s_led_set_status(led->ec, led, new_state);
}

static int thinkpad_t14s_led_blink_set(struct led_classdev *led_cdev,
				       unsigned long *delay_on,
				       unsigned long *delay_off)
{
	struct thinkpad_t14s_ec_led_classdev *led = container_of(led_cdev,
			struct thinkpad_t14s_ec_led_classdev, led_classdev);

	/* Can we choose the flash rate? */
	if (*delay_on == 0 && *delay_off == 0) {
		/* yes. set them to the hardware blink rate (1 Hz) */
		*delay_on = 500; /* ms */
		*delay_off = 500; /* ms */
	} else if ((*delay_on != 500) || (*delay_off != 500))
		return -EINVAL;

	return thinkpad_t14s_led_set_status(led->ec, led, THINKPAD_EC_LED_BLINK);
}

static int thinkpad_t14s_init_led(struct thinkpad_t14s_ec *ec,
				  struct thinkpad_t14s_ec_led_classdev *led,
				  u8 id, const char *name)
{
	led->led_classdev.name = name;
	led->led_classdev.flags = LED_RETAIN_AT_SHUTDOWN;
	led->led_classdev.max_brightness = 1;
	led->led_classdev.brightness_set_blocking = thinkpad_t14s_led_set;
	led->led_classdev.blink_set = thinkpad_t14s_led_blink_set;
	led->ec = ec;
	led->led = id;

	return devm_led_classdev_register(ec->dev, &led->led_classdev);
}

static int thinkpad_t14s_leds_probe(struct thinkpad_t14s_ec *ec)
{
	int ret;

	ret = thinkpad_t14s_init_led(ec, &ec->led_pwr_btn, 0,
				     "platform::power");
	if (ret)
		return ret;
	ret = thinkpad_t14s_init_led(ec, &ec->led_chrg_orange, 1,
				     "platform:amber:battery-charging");
	if (ret)
		return ret;
	ret = thinkpad_t14s_init_led(ec, &ec->led_chrg_white, 2,
				     "platform:white:battery-full");
	if (ret)
		return ret;
	ret = thinkpad_t14s_init_led(ec, &ec->led_lid_logo_dot, 10,
				     "platform::lid_logo_dot");
	if (ret)
		return ret;

	return 0;
}

static int thinkpad_t14s_kbd_bl_set(struct led_classdev *led_cdev,
				    enum led_brightness brightness)
{
	struct thinkpad_t14s_ec *ec = container_of(led_cdev,
					struct thinkpad_t14s_ec, kbd_backlight);
	int ret;

	ret = regmap_update_bits(ec->regmap, THINKPAD_T14S_EC_REG_KBD_BL1,
				 THINKPAD_T14S_EC_KBD_BL1_MASK,
				 brightness << THINKPAD_T14S_EC_KBD_BL1_SHIFT);
	if (ret < 0)
		return ret;

	ret = regmap_update_bits(ec->regmap, THINKPAD_T14S_EC_REG_KBD_BL2,
				 THINKPAD_T14S_EC_KBD_BL2_MASK,
				 brightness << THINKPAD_T14S_EC_KBD_BL2_SHIFT);
	if (ret < 0)
		return ret;

	return 0;
}

static enum led_brightness thinkpad_t14s_kbd_bl_get(struct led_classdev *led_cdev)
{
	struct thinkpad_t14s_ec *ec = container_of(led_cdev,
					struct thinkpad_t14s_ec, kbd_backlight);
	int ret, val;

	ret = regmap_read(ec->regmap, THINKPAD_T14S_EC_REG_KBD_BL1, &val);
	if (ret < 0)
		return ret;

	val &= THINKPAD_T14S_EC_KBD_BL1_MASK;

	return val >> THINKPAD_T14S_EC_KBD_BL1_SHIFT;
}

static void thinkpad_t14s_kbd_bl_update(struct thinkpad_t14s_ec *ec)
{
	enum led_brightness brightness = thinkpad_t14s_kbd_bl_get(&ec->kbd_backlight);

	led_classdev_notify_brightness_hw_changed(&ec->kbd_backlight, brightness);
}

static int thinkpad_t14s_kbd_backlight_probe(struct thinkpad_t14s_ec *ec)
{
	ec->kbd_backlight.name = "platform::kbd_backlight";
	ec->kbd_backlight.flags = LED_BRIGHT_HW_CHANGED;
	ec->kbd_backlight.max_brightness = 2;
	ec->kbd_backlight.brightness_set_blocking = thinkpad_t14s_kbd_bl_set;
	ec->kbd_backlight.brightness_get = thinkpad_t14s_kbd_bl_get;

	return devm_led_classdev_register(ec->dev, &ec->kbd_backlight);
}

static enum led_brightness thinkpad_t14s_audio_led_get(struct thinkpad_t14s_ec *ec,
						       u8 led_bit)
{
	int ret, val;

	ret = regmap_read(ec->regmap, THINKPAD_T14S_EC_REG_AUD, &val);
	if (ret < 0)
		return ret;

	return !!(val && led_bit);
}

static enum led_brightness thinkpad_t14s_audio_led_set(struct thinkpad_t14s_ec *ec,
						       u8 led_bit,
						       enum led_brightness brightness)
{
	u8 val = brightness ? led_bit : 0;

	return regmap_update_bits(ec->regmap, THINKPAD_T14S_EC_REG_AUD, led_bit, val);
}

static enum led_brightness thinkpad_t14s_mic_mute_led_get(struct led_classdev *led_cdev)
{
	struct thinkpad_t14s_ec *ec = container_of(led_cdev,
					struct thinkpad_t14s_ec, led_mic_mute);

	return thinkpad_t14s_audio_led_get(ec, THINKPAD_T14S_EC_MIC_MUTE_LED);
}

static int thinkpad_t14s_mic_mute_led_set(struct led_classdev *led_cdev,
					  enum led_brightness brightness)
{
	struct thinkpad_t14s_ec *ec = container_of(led_cdev,
					struct thinkpad_t14s_ec, led_mic_mute);

	return thinkpad_t14s_audio_led_set(ec, THINKPAD_T14S_EC_MIC_MUTE_LED, brightness);
}

static enum led_brightness thinkpad_t14s_spk_mute_led_get(struct led_classdev *led_cdev)
{
	struct thinkpad_t14s_ec *ec = container_of(led_cdev,
					struct thinkpad_t14s_ec, led_spk_mute);

	return thinkpad_t14s_audio_led_get(ec, THINKPAD_T14S_EC_SPK_MUTE_LED);
}

static int thinkpad_t14s_spk_mute_led_set(struct led_classdev *led_cdev,
					  enum led_brightness brightness)
{
	struct thinkpad_t14s_ec *ec = container_of(led_cdev,
					struct thinkpad_t14s_ec, led_spk_mute);

	return thinkpad_t14s_audio_led_set(ec, THINKPAD_T14S_EC_SPK_MUTE_LED, brightness);
}

static int thinkpad_t14s_kbd_audio_led_probe(struct thinkpad_t14s_ec *ec)
{
	int ret;

	ec->led_mic_mute.name = "platform::micmute";
	ec->led_mic_mute.max_brightness = 1,
	ec->led_mic_mute.default_trigger = "audio-micmute",
	ec->led_mic_mute.brightness_set_blocking = thinkpad_t14s_mic_mute_led_set;
	ec->led_mic_mute.brightness_get = thinkpad_t14s_mic_mute_led_get;

	ec->led_spk_mute.name = "platform::mute";
	ec->led_spk_mute.max_brightness = 1,
	ec->led_spk_mute.default_trigger = "audio-mute",
	ec->led_spk_mute.brightness_set_blocking = thinkpad_t14s_spk_mute_led_set;
	ec->led_spk_mute.brightness_get = thinkpad_t14s_spk_mute_led_get;

	ret = devm_led_classdev_register(ec->dev, &ec->led_mic_mute);
	if (ret)
		return ret;

	return devm_led_classdev_register(ec->dev, &ec->led_spk_mute);
}

/*
 * using code 0x16 ignores the provided KEY code and use KEY_MUTE,
 * so all codes have a 0x1000 offset
 */
static const struct key_entry thinkpad_t14s_keymap[] = {
	{ KE_KEY, 0x1000 + THINKPAD_T14S_EC_EVT_KEY_FN_4, { KEY_SLEEP } },
	{ KE_KEY, 0x1000 + THINKPAD_T14S_EC_EVT_KEY_FN_N, { KEY_VENDOR } },
	{ KE_KEY, 0x1000 + THINKPAD_T14S_EC_EVT_KEY_FN_F4, { KEY_MICMUTE } },
	{ KE_KEY, 0x1000 + THINKPAD_T14S_EC_EVT_KEY_FN_F7, { KEY_SWITCHVIDEOMODE } },
	{ KE_KEY, 0x1000 + THINKPAD_T14S_EC_EVT_KEY_FN_F8, { KEY_MODE } },
	{ KE_KEY, 0x1000 + THINKPAD_T14S_EC_EVT_KEY_FN_F10, { KEY_SELECTIVE_SCREENSHOT } },
	{ KE_KEY, 0x1000 + THINKPAD_T14S_EC_EVT_KEY_FN_F11, { KEY_LINK_PHONE } },
	{ KE_KEY, 0x1000 + THINKPAD_T14S_EC_EVT_KEY_FN_F12, { KEY_BOOKMARKS } },
	{ KE_KEY, 0x1000 + THINKPAD_T14S_EC_EVT_KEY_FN_SPACE, { KEY_KBDILLUMTOGGLE } },
	{ KE_KEY, 0x1000 + THINKPAD_T14S_EC_EVT_KEY_FN_ESC, { KEY_FN_ESC } },
	{ KE_KEY, 0x1000 + THINKPAD_T14S_EC_EVT_KEY_FN_TAB, { KEY_ZOOM } },
	{ KE_KEY, 0x1000 + THINKPAD_T14S_EC_EVT_KEY_FN_RIGHT_SHIFT, { KEY_FN_RIGHT_SHIFT } },
	{ KE_KEY, 0x1000 + THINKPAD_T14S_EC_EVT_KEY_TP_DOUBLE_TAP, { KEY_PROG4 } },
	{ KE_END }
};

static int thinkpad_t14s_input_probe(struct thinkpad_t14s_ec *ec)
{
	int ret;

	ec->inputdev = devm_input_allocate_device(ec->dev);
	if (!ec->inputdev)
		return -ENOMEM;

	ec->inputdev->name = "ThinkPad Extra Buttons";
	ec->inputdev->phys = "thinkpad/input0";
	ec->inputdev->id.bustype = BUS_HOST;
	ec->inputdev->dev.parent = ec->dev;

	ret = sparse_keymap_setup(ec->inputdev, thinkpad_t14s_keymap, NULL);
	if (ret)
		return ret;

	ret = input_register_device(ec->inputdev);
	if (ret < 0)
		return ret;

	return 0;
}

static irqreturn_t thinkpad_t14s_ec_irq_handler(int irq, void *data)
{
	struct thinkpad_t14s_ec *ec = data;
	int ret;
	u8 val;

	ret = thinkpad_t14s_ec_read_evt(ec, &val);
	if (ret < 0) {
		dev_err(ec->dev, "Failed to read event\n");
		return IRQ_HANDLED;
	}

	switch (val) {
	case THINKPAD_T14S_EC_EVT_NONE:
		break;
	case THINKPAD_T14S_EC_EVT_KEY_FN_SPACE:
		thinkpad_t14s_kbd_bl_update(ec);
		fallthrough;
	case THINKPAD_T14S_EC_EVT_KEY_FN_F4:
	case THINKPAD_T14S_EC_EVT_KEY_FN_F7:
	case THINKPAD_T14S_EC_EVT_KEY_FN_4:
	case THINKPAD_T14S_EC_EVT_KEY_FN_F8:
	case THINKPAD_T14S_EC_EVT_KEY_FN_F12:
	case THINKPAD_T14S_EC_EVT_KEY_FN_TAB:
	case THINKPAD_T14S_EC_EVT_KEY_FN_F10:
	case THINKPAD_T14S_EC_EVT_KEY_FN_N:
	case THINKPAD_T14S_EC_EVT_KEY_FN_F11:
	case THINKPAD_T14S_EC_EVT_KEY_FN_ESC:
	case THINKPAD_T14S_EC_EVT_KEY_FN_RIGHT_SHIFT:
	case THINKPAD_T14S_EC_EVT_KEY_TP_DOUBLE_TAP:
		sparse_keymap_report_event(ec->inputdev, 0x1000 + val, 1, true);
		break;
	case THINKPAD_T14S_EC_EVT_AC_CONNECTED:
		dev_dbg(ec->dev, "AC connected\n");
		break;
	case THINKPAD_T14S_EC_EVT_AC_DISCONNECTED:
		dev_dbg(ec->dev, "AC disconnected\n");
		break;
	case THINKPAD_T14S_EC_EVT_KEY_POWER:
		dev_dbg(ec->dev, "power button\n");
		break;
	case THINKPAD_T14S_EC_EVT_LID_OPEN:
		dev_dbg(ec->dev, "LID open\n");
		break;
	case THINKPAD_T14S_EC_EVT_LID_CLOSED:
		dev_dbg(ec->dev, "LID closed\n");
		break;
	case THINKPAD_T14S_EC_EVT_KEY_FN_G:
		dev_dbg(ec->dev, "FN + G - toggle double-tapping\n");
		break;
	case THINKPAD_T14S_EC_EVT_KEY_FN_L:
		dev_dbg(ec->dev, "FN + L - low performance mode\n");
		break;
	case THINKPAD_T14S_EC_EVT_KEY_FN_M:
		dev_dbg(ec->dev, "FN + M - medium performance mode\n");
		break;
	case THINKPAD_T14S_EC_EVT_KEY_FN_H:
		dev_dbg(ec->dev, "FN + H - high performance mode\n");
		break;
	case THINKPAD_T14S_EC_EVT_KEY_FN_T:
		dev_dbg(ec->dev, "FN + T - toggle intelligent cooling mode\n");
		break;
	case THINKPAD_T14S_EC_EVT_KEY_FN_D:
		dev_dbg(ec->dev, "FN + D - toggle privacy guard mode\n");
		break;
	default:
		dev_info(ec->dev, "Unknown EC event: 0x%02x\n", val);
		break;
	}

	return IRQ_HANDLED;
}

static int thinkpad_t14s_ec_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct thinkpad_t14s_ec *ec;
	int ret;

	ec = devm_kzalloc(dev, sizeof(*ec), GFP_KERNEL);
	if (!ec)
		return -ENOMEM;

	ec->dev = dev;

	ec->regmap = devm_regmap_init(dev, &thinkpad_t14s_ec_regmap_bus,
				      ec, &thinkpad_t14s_ec_regmap_config);
	if (IS_ERR(ec->regmap))
		return dev_err_probe(dev, PTR_ERR(ec->regmap),
				     "Failed to init regmap\n");

	ret = devm_request_threaded_irq(dev, client->irq, NULL,
					thinkpad_t14s_ec_irq_handler,
					IRQF_ONESHOT, dev_name(dev), ec);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get IRQ\n");

	ret = thinkpad_t14s_leds_probe(ec);
	if (ret < 0)
		return ret;

	ret = thinkpad_t14s_kbd_backlight_probe(ec);
	if (ret < 0)
		return ret;

	ret = thinkpad_t14s_kbd_audio_led_probe(ec);
	if (ret < 0)
		return ret;

	ret = thinkpad_t14s_input_probe(ec);
	if (ret < 0)
		return ret;

	/*
	 * Disable wakeup support by default, because the driver currently does
	 * not support masking any events and the laptop should not wake up when
	 * the LID is closed.
	 */
	device_wakeup_disable(dev);

	return 0;
}

static const struct of_device_id thinkpad_t14s_ec_of_match[] = {
	{ .compatible = "lenovo,thinkpad-t14s-ec" },
	{}
};
MODULE_DEVICE_TABLE(of, thinkpad_t14s_ec_of_match);

static const struct i2c_device_id thinkpad_t14s_ec_i2c_id_table[] = {
	{ "thinkpad-t14s-ec", },
	{}
};
MODULE_DEVICE_TABLE(i2c, thinkpad_t14s_ec_i2c_id_table);

static struct i2c_driver thinkpad_t14s_ec_i2c_driver = {
	.driver = {
		.name = "thinkpad-t14s-ec",
		.of_match_table = thinkpad_t14s_ec_of_match
	},
	.probe = thinkpad_t14s_ec_probe,
	.id_table = thinkpad_t14s_ec_i2c_id_table,
};
module_i2c_driver(thinkpad_t14s_ec_i2c_driver);

MODULE_AUTHOR("Sebastian Reichel <sre@kernel.org>");
MODULE_DESCRIPTION("Lenovo Thinkpad T14s Embedded Controller");
MODULE_LICENSE("GPL");
