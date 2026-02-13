// SPDX-License-Identifier: GPL-2.0-only
/*
 *  GPIO driven charlieplex keypad driver
 *
 *  Copyright (c) 2025 Hugo Villeneuve <hvilleneuve@dimonoff.com>
 *
 *  Based on matrix_keyboard.c
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/input/matrix_keypad.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/types.h>

struct charlieplex_keypad {
	struct input_dev *input_dev;
	struct gpio_descs *line_gpios;
	unsigned int nlines;
	unsigned int line_scan_delay_us;
	unsigned int debounce_threshold;
	unsigned int debounce_count;
	int debounce_code;
	int current_code;
};

static void charlieplex_keypad_report_key(struct input_dev *input)
{
	struct charlieplex_keypad *keypad = input_get_drvdata(input);
	const unsigned short *keycodes = input->keycode;

	if (keypad->current_code > 0) {
		input_event(input, EV_MSC, MSC_SCAN, keypad->current_code);
		input_report_key(input, keycodes[keypad->current_code], 0);
	}

	if (keypad->debounce_code) {
		input_event(input, EV_MSC, MSC_SCAN, keypad->debounce_code);
		input_report_key(input, keycodes[keypad->debounce_code], 1);
	}

	input_sync(input);
	keypad->current_code = keypad->debounce_code;
}

static void charlieplex_keypad_check_switch_change(struct input_dev *input,
						   int code)
{
	struct charlieplex_keypad *keypad = input_get_drvdata(input);

	if (code != keypad->debounce_code) {
		keypad->debounce_count = 0;
		keypad->debounce_code = code;
	} else if (keypad->debounce_count < keypad->debounce_threshold) {
		keypad->debounce_count++;

		if (keypad->debounce_count >= keypad->debounce_threshold &&
		    keypad->debounce_code != keypad->current_code)
			charlieplex_keypad_report_key(input);
	}
}

static void charlieplex_keypad_poll(struct input_dev *input)
{
	struct charlieplex_keypad *keypad = input_get_drvdata(input);
	int oline;
	int code;

	for (code = 0, oline = 0; oline < keypad->nlines; oline++) {
		DECLARE_BITMAP(values, MATRIX_MAX_ROWS);
		int iline;
		int rc;

		/* Activate only one line as output at a time. */
		gpiod_direction_output(keypad->line_gpios->desc[oline], 1);

		if (keypad->line_scan_delay_us)
			fsleep(keypad->line_scan_delay_us);

		/* Read input on all other lines. */
		rc = gpiod_get_array_value_cansleep(keypad->line_gpios->ndescs,
						    keypad->line_gpios->desc,
						    keypad->line_gpios->info, values);
		if (rc)
			return;

		for (iline = 0; iline < keypad->nlines; iline++) {
			if (iline == oline)
				continue; /* Do not read active output line. */

			/* Check if GPIO is asserted. */
			if (test_bit(iline, values)) {
				code = MATRIX_SCAN_CODE(oline, iline,
							get_count_order(keypad->nlines));
				/*
				 * Exit loop immediately since we cannot detect
				 * more than one key press at a time.
				 */
				break;
			}
		}

		gpiod_direction_input(keypad->line_gpios->desc[oline]);

		if (code)
			break;
	}

	charlieplex_keypad_check_switch_change(input, code);
}

static int charlieplex_keypad_init_gpio(struct platform_device *pdev,
					struct charlieplex_keypad *keypad)
{
	int i;

	keypad->line_gpios = devm_gpiod_get_array(&pdev->dev, "line", GPIOD_IN);
	if (IS_ERR(keypad->line_gpios))
		return PTR_ERR(keypad->line_gpios);

	keypad->nlines = keypad->line_gpios->ndescs;

	if (keypad->nlines > MATRIX_MAX_ROWS)
		return -EINVAL;

	for (i = 0; i < keypad->nlines; i++)
		gpiod_set_consumer_name(keypad->line_gpios->desc[i], "charlieplex_kbd_line");

	return 0;
}

static int charlieplex_keypad_probe(struct platform_device *pdev)
{
	struct charlieplex_keypad *keypad;
	unsigned int debounce_interval_ms;
	unsigned int poll_interval_ms;
	struct input_dev *input_dev;
	int err;

	keypad = devm_kzalloc(&pdev->dev, sizeof(*keypad), GFP_KERNEL);
	if (!keypad)
		return -ENOMEM;

	input_dev = devm_input_allocate_device(&pdev->dev);
	if (!input_dev)
		return -ENOMEM;

	keypad->input_dev = input_dev;

	device_property_read_u32(&pdev->dev, "poll-interval", &poll_interval_ms);
	device_property_read_u32(&pdev->dev, "debounce-delay-ms", &debounce_interval_ms);
	device_property_read_u32(&pdev->dev, "line-scan-delay-us", &keypad->line_scan_delay_us);

	keypad->current_code = -1;
	keypad->debounce_code = -1;
	keypad->debounce_threshold = DIV_ROUND_UP(debounce_interval_ms, poll_interval_ms);

	err = charlieplex_keypad_init_gpio(pdev, keypad);
	if (err)
		return err;

	input_dev->name		= pdev->name;
	input_dev->id.bustype	= BUS_HOST;

	err = matrix_keypad_build_keymap(NULL, NULL, keypad->nlines,
					 keypad->nlines, NULL, input_dev);
	if (err)
		dev_err_probe(&pdev->dev, -ENOMEM, "failed to build keymap\n");

	if (device_property_read_bool(&pdev->dev, "autorepeat"))
		__set_bit(EV_REP, input_dev->evbit);

	input_set_capability(input_dev, EV_MSC, MSC_SCAN);

	err = input_setup_polling(input_dev, charlieplex_keypad_poll);
	if (err)
		dev_err_probe(&pdev->dev, err, "unable to set up polling\n");

	input_set_poll_interval(input_dev, poll_interval_ms);

	input_set_drvdata(input_dev, keypad);

	err = input_register_device(keypad->input_dev);
	if (err)
		return err;

	platform_set_drvdata(pdev, keypad);

	return 0;
}

static const struct of_device_id charlieplex_keypad_dt_match[] = {
	{ .compatible = "gpio-charlieplex-keypad" },
	{ }
};
MODULE_DEVICE_TABLE(of, charlieplex_keypad_dt_match);

static struct platform_driver charlieplex_keypad_driver = {
	.probe		= charlieplex_keypad_probe,
	.driver		= {
		.name	= "charlieplex-keypad",
		.of_match_table = of_match_ptr(charlieplex_keypad_dt_match),
	},
};
module_platform_driver(charlieplex_keypad_driver);

MODULE_AUTHOR("Hugo Villeneuve <hvilleneuve@dimonoff.com>");
MODULE_DESCRIPTION("GPIO driven charlieplex keypad driver");
MODULE_LICENSE("GPL");
