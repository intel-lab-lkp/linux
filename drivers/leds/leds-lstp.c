// SPDX-License-Identifier: GPL-2.0-only
/*
 * LED driver for LSTP USB interface.
 *
 * Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES.  All rights reserved.
 */

#include <linux/auxiliary_bus.h>
#include <linux/build_bug.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/slab.h>

#include <linux/usb/lstp.h>

#define LSTP_LED_REPEAT_FOREVER 0 /* wire: repeats == 0 plays forever */
#define LSTP_LED_LINUX_REPEAT_FOREVER (-1) /* LED-class pattern_set: repeat == -1 plays forever */
#define LSTP_LED_DEFAULT_HOLD_TIME_MS 1 /* delta_t for single-step holds */
#define LSTP_LED_DEFAULT_BLINK_MS 500 /* LED-class default when delay_on/off are both 0 */

/* A GPIO channel command; the others are in gpio-lstp.c. */
#define LSTP_GPIO_CMD_SET_LED_PATTERN 0x05

struct lstp_led_pattern {
	u8 brightness;
	__le32 time_ms;
} __packed;

struct lstp_gpio_set_led_pattern_req {
	__le16 pin;
	__le32 repeats;
	u8 num_steps;
	struct lstp_led_pattern pattern[];
} __packed;

struct lstp_led {
	struct led_classdev cdev;
	struct lstp_channel *ch;
	u16 pin;
};

/* Send SET_LED_PATTERN. @num_steps 0 clears the pattern, 1 holds forever. */
static int lstp_led_set_pattern(struct lstp_led *led, u32 repeats,
				const struct led_pattern *pattern, u8 num_steps)
{
	struct lstp_channel *ch = led->ch;
	struct lstp_gpio_set_led_pattern_req *req;
	size_t req_len = struct_size(req, pattern, num_steps);
	/* The LED core applies a firmware-node max-brightness; the wire is 8-bit */
	unsigned int max_brightness = min_t(unsigned int, led->cdev.max_brightness, U8_MAX);

	/* Probe enforces bulk_tx_size >= LSTP_USB_EP_MIN_SIZE; anchor base here. */
	BUILD_BUG_ON(sizeof(struct lstp_header) + sizeof(*req) > LSTP_USB_EP_MIN_SIZE);

	for (u8 i = 0; i < num_steps; i++) {
		if (pattern[i].brightness > max_brightness)
			return -EINVAL;
	}

	if (req_len > ch->max_tx_payload) {
		dev_err_ratelimited(ch->dev,
				    "%s: ch_%d: SET_LED_PATTERN too long for led '%s' (%zu bytes, max %zu)\n",
				    __func__, ch->ch_id, led->cdev.name, req_len,
				    ch->max_tx_payload);
		return -EMSGSIZE;
	}

	req = kmalloc(req_len, GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	req->pin = cpu_to_le16(led->pin);
	req->repeats = cpu_to_le32(repeats);
	req->num_steps = num_steps;
	for (u8 i = 0; i < num_steps; i++) {
		req->pattern[i].brightness = (u8)pattern[i].brightness;
		req->pattern[i].time_ms = cpu_to_le32(pattern[i].delta_t);
	}

	int ret =
		lstp_request(ch, LSTP_GPIO_CMD_SET_LED_PATTERN, req, req_len, NULL, 0, NULL, NULL);
	if (ret && ret != -ENODEV)
		dev_err_ratelimited(ch->dev,
				    "%s: ch_%d: SET_LED_PATTERN failed for led '%s' (repeats=%u, num_steps=%u) (%pe)\n",
				    __func__, ch->ch_id, led->cdev.name, repeats, num_steps,
				    ERR_PTR(ret));

	kfree(req);
	return ret;
}

/*
 * blink_set: @delay_on 0 holds OFF, @delay_off 0 holds ON, both 0 selects the
 * LED-class 500 ms default. On-brightness is cdev->brightness, falling back to
 * max_brightness when the LED is currently off.
 */
static int lstp_led_blink_set(struct led_classdev *cdev, unsigned long *delay_on,
			      unsigned long *delay_off)
{
	struct lstp_led *led = container_of(cdev, struct lstp_led, cdev);
	u8 on_value = cdev->brightness ? (u8)cdev->brightness : (u8)cdev->max_brightness;
	struct led_pattern steps[2];
	u8 len;

	if (*delay_on == 0 && *delay_off == 0) {
		*delay_on = LSTP_LED_DEFAULT_BLINK_MS;
		*delay_off = LSTP_LED_DEFAULT_BLINK_MS;
	}

	if (*delay_on == 0) {
		steps[0].delta_t = LSTP_LED_DEFAULT_HOLD_TIME_MS;
		steps[0].brightness = LED_OFF;
		len = 1;
	} else if (*delay_off == 0) {
		steps[0].delta_t = LSTP_LED_DEFAULT_HOLD_TIME_MS;
		steps[0].brightness = on_value;
		len = 1;
	} else {
		steps[0].delta_t = *delay_on;
		steps[0].brightness = on_value;
		steps[1].delta_t = *delay_off;
		steps[1].brightness = LED_OFF;
		len = 2;
	}
	return lstp_led_set_pattern(led, LSTP_LED_REPEAT_FOREVER, steps, len);
}

/*
 * pattern_set: LED-class repeat -1 maps to the wire's repeats == 0 ("forever").
 * repeat == 0 and anything below -1 are invalid.
 */
static int lstp_led_pattern_set(struct led_classdev *cdev, struct led_pattern *pattern, u32 len,
				int repeat)
{
	struct lstp_led *led = container_of(cdev, struct lstp_led, cdev);

	if (len > U8_MAX)
		return -EMSGSIZE;
	if (repeat == 0 || repeat < LSTP_LED_LINUX_REPEAT_FOREVER)
		return -EINVAL;

	u32 mapped_repeat = (repeat == LSTP_LED_LINUX_REPEAT_FOREVER) ? LSTP_LED_REPEAT_FOREVER :
									(u32)repeat;
	return lstp_led_set_pattern(led, mapped_repeat, pattern, (u8)len);
}

static int lstp_led_pattern_clear(struct led_classdev *cdev)
{
	struct lstp_led *led = container_of(cdev, struct lstp_led, cdev);

	return lstp_led_set_pattern(led, LSTP_LED_REPEAT_FOREVER, NULL, 0);
}

/* brightness_set_blocking: a one-step pattern; @brightness is core-clamped. */
static int lstp_led_brightness_set_blocking(struct led_classdev *cdev,
					    enum led_brightness brightness)
{
	struct lstp_led *led = container_of(cdev, struct lstp_led, cdev);
	struct led_pattern step = { .delta_t = LSTP_LED_DEFAULT_HOLD_TIME_MS,
				    .brightness = brightness };

	return lstp_led_set_pattern(led, LSTP_LED_REPEAT_FOREVER, &step, 1);
}

/*
 * Send @fwnode's led-pattern once, repeating forever; the device then plays it
 * without host involvement. A pattern that does not fit in one request is not
 * sent at all.
 */
static void lstp_led_send_fwnode_pattern(struct lstp_led *led, struct fwnode_handle *fwnode)
{
	struct lstp_channel *ch = led->ch;
	struct lstp_gpio_set_led_pattern_req *req;
	size_t max_steps = min_t(size_t, U8_MAX,
				 (ch->max_tx_payload - sizeof(*req)) / sizeof(req->pattern[0]));
	int count = fwnode_property_count_u32(fwnode, "led-pattern");
	struct led_pattern *steps = NULL;
	unsigned int num_steps;
	u32 *raw = NULL;

	if (count <= 0 || count % 2) {
		dev_err(ch->dev,
			"%s: ch_%d: led-pattern of LED '%s' is not a list of <brightness duration> pairs\n",
			__func__, ch->ch_id, led->cdev.name);
		return;
	}

	num_steps = count / 2;
	if (num_steps > max_steps) {
		dev_err(ch->dev,
			"%s: ch_%d: led-pattern of LED '%s' has %u steps, the device takes at most %zu\n",
			__func__, ch->ch_id, led->cdev.name, num_steps, max_steps);
		return;
	}

	raw = kcalloc(count, sizeof(*raw), GFP_KERNEL);
	steps = kzalloc_objs(*steps, num_steps);
	if (!raw || !steps || fwnode_property_read_u32_array(fwnode, "led-pattern", raw, count))
		goto out;

	for (unsigned int i = 0; i < num_steps; i++) {
		if (raw[2 * i] > led->cdev.max_brightness) {
			dev_err(ch->dev,
				"%s: ch_%d: led-pattern of LED '%s': step %u brightness %u exceeds %u\n",
				__func__, ch->ch_id, led->cdev.name, i, raw[2 * i],
				led->cdev.max_brightness);
			goto out;
		}
		steps[i].brightness = raw[2 * i];
		steps[i].delta_t = raw[2 * i + 1];
	}

	lstp_led_set_pattern(led, LSTP_LED_REPEAT_FOREVER, steps, num_steps);
out:
	kfree(steps);
	kfree(raw);
}

/*
 * default-state is written once, and a missing one means "off". "keep" leaves
 * the LED as the device firmware set it: the device cannot report its current
 * state, so there is nothing to read back.
 */
static void lstp_led_apply_default_state(struct lstp_led *led, struct fwnode_handle *fwnode)
{
	switch (led_init_default_state_get(fwnode)) {
	case LEDS_DEFSTATE_KEEP:
		return;
	case LEDS_DEFSTATE_ON:
		led->cdev.brightness = led->cdev.max_brightness;
		break;
	default:
		led->cdev.brightness = LED_OFF;
		break;
	}

	lstp_led_brightness_set_blocking(&led->cdev, led->cdev.brightness);
}

static void lstp_led_put_fwnode(void *fwnode)
{
	fwnode_handle_put(fwnode);
}

/*
 * led_classdev_unregister() flushes set_brightness_work before it removes the
 * sysfs files, and the LED core does not refuse brightness or trigger writes in
 * between: such a write would queue work or attach a trigger that outlives
 * @data. Disable sysfs first, under led_access, so they fail with -EBUSY.
 */
static void lstp_led_unregister(void *data)
{
	struct led_classdev *cdev = data;

	mutex_lock(&cdev->led_access);
	led_sysfs_disable(cdev);
	mutex_unlock(&cdev->led_access);
	led_classdev_unregister(cdev);
}

static int lstp_led_classdev_register(struct device *dev, struct led_classdev *cdev,
				      struct led_init_data *init_data)
{
	int ret = led_classdev_register_ext(dev, cdev, init_data);

	if (ret)
		return ret;
	return devm_add_action_or_reset(dev, lstp_led_unregister, cdev);
}

/* Register @led, initialised from @fwnode if any. A failure leaves the other LEDs up. */
static int lstp_led_register(struct device *dev, struct lstp_led *led, struct fwnode_handle *fwnode)
{
	int ret;

	if (!fwnode) {
		ret = lstp_led_classdev_register(dev, &led->cdev, NULL);
	} else {
		struct led_init_data init_data = { .fwnode = fwnode };

		/*
		 * The class device keeps @fwnode as its node, and default_trigger
		 * points into it. Queued first, the put runs after unregistration.
		 */
		ret = devm_add_action_or_reset(dev, lstp_led_put_fwnode, fwnode_handle_get(fwnode));
		if (!ret) {
			bool pattern = fwnode_property_present(fwnode, "led-pattern");
			bool trigger = fwnode_property_present(fwnode, "linux,default-trigger");

			/* A trigger takes precedence; the LED core applies it from @fwnode. */
			if (pattern && trigger)
				dev_err(dev,
					"%s: ch_%d: Ignoring led-pattern of LED '%s': %pfw also sets linux,default-trigger\n",
					__func__, led->ch->ch_id, led->cdev.name, fwnode);
			if (pattern && !trigger)
				lstp_led_send_fwnode_pattern(led, fwnode);
			else
				lstp_led_apply_default_state(led, fwnode);
			ret = lstp_led_classdev_register(dev, &led->cdev, &init_data);
		}
	}

	if (ret)
		dev_warn(dev, "%s: ch_%d: Could not register LED '%s', pin %u (%pe)\n", __func__,
			 led->ch->ch_id, led->cdev.name, led->pin, ERR_PTR(ret));
	return ret;
}

static struct lstp_led *lstp_led_by_pin(struct lstp_led *leds, u16 nleds, u32 pin)
{
	for (u16 i = 0; i < nleds; i++) {
		if (leds[i].pin == pin)
			return &leds[i];
	}
	return NULL;
}

/*
 * Register the LEDs described under the channel's "leds" node, then every other
 * LED pin under its firmware pin name. A node that does not describe a
 * firmware-reported LED pin, or that names no LED, is ignored.
 */
static int lstp_leds_probe(struct auxiliary_device *auxdev, const struct auxiliary_device_id *id)
{
	struct lstp_leds *pins = container_of(auxdev, struct lstp_leds, auxdev);
	struct lstp_channel *ch = pins->ch;
	struct device *dev = &auxdev->dev;
	struct fwnode_handle *node;
	struct fwnode_handle *child;
	struct lstp_led *leds;

	leds = devm_kcalloc(dev, pins->npins, sizeof(*leds), GFP_KERNEL);
	if (!leds)
		return -ENOMEM;

	for (u16 i = 0; i < pins->npins; i++) {
		leds[i].ch = ch;
		leds[i].pin = pins->pins[i].pin;
		leds[i].cdev.name = pins->pins[i].name;
		leds[i].cdev.max_brightness = U8_MAX;
		leds[i].cdev.flags = LED_HW_PLUGGABLE;
		leds[i].cdev.brightness_set_blocking = lstp_led_brightness_set_blocking;
		leds[i].cdev.blink_set = lstp_led_blink_set;
		leds[i].cdev.pattern_set = lstp_led_pattern_set;
		leds[i].cdev.pattern_clear = lstp_led_pattern_clear;
	}

	node = fwnode_get_named_child_node(ch->fwnode, "leds");
	/*
	 * Not the available-child iterator; see Documentation/driver-api/usb/lstp.rst.
	 * On OF, fwnode_for_each_child_node() already skips disabled nodes.
	 */
	fwnode_for_each_child_node(node, child) {
		struct lstp_led *led = NULL;
		u32 pin;

		if (!fwnode_property_read_u32(child, "reg", &pin))
			led = lstp_led_by_pin(leds, pins->npins, pin);
		if (!led) {
			dev_warn(dev, "%s: ch_%d: Ignoring %pfw: not an LED pin\n", __func__,
				 ch->ch_id, child);
			continue;
		}
		if (led->cdev.dev) {
			dev_warn(dev, "%s: ch_%d: Ignoring %pfw: pin %u is already described\n",
				 __func__, ch->ch_id, child, pin);
			continue;
		}
		if (!fwnode_property_present(child, "label") &&
		    !fwnode_property_present(child, "function")) {
			dev_warn(dev, "%s: ch_%d: Ignoring %pfw: neither label nor function\n",
				 __func__, ch->ch_id, child);
			continue;
		}
		lstp_led_register(dev, led, child);
	}
	fwnode_handle_put(node);

	for (u16 i = 0; i < pins->npins; i++) {
		if (!leds[i].cdev.dev)
			lstp_led_register(dev, &leds[i], NULL);
	}

	return 0;
}

static const struct auxiliary_device_id lstp_leds_id_table[] = { { .name = "lstp.led" }, {} };
MODULE_DEVICE_TABLE(auxiliary, lstp_leds_id_table);

static struct auxiliary_driver lstp_leds_drv = {
	.name = "led",
	.probe = lstp_leds_probe,
	.id_table = lstp_leds_id_table,
};
module_auxiliary_driver(lstp_leds_drv);

MODULE_IMPORT_NS("LSTP");
MODULE_DESCRIPTION("LSTP LED driver");
MODULE_LICENSE("GPL");
