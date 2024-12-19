// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2024 Bootlin
 *
 * Author: Mathieu Dubois-Briand <mathieu.dubois-briand@bootlin.com>
 */

#include <linux/init.h>
#include <linux/input.h>
#include <linux/input/matrix_keypad.h>
#include <linux/interrupt.h>
#include <linux/mfd/max7360.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeirq.h>
#include <linux/regmap.h>
#include <linux/slab.h>

struct max7360_keypad {
	struct input_dev *input;
	unsigned int rows;
	unsigned int cols;
	unsigned int debounce_ms;
	int irq;
	bool no_autorepeat;
	struct regmap *regmap;
	unsigned short keycodes[MAX7360_MAX_KEY_ROWS * MAX7360_MAX_KEY_COLS];
};

static irqreturn_t max7360_keypad_irq(int irq, void *data)
{
	struct max7360_keypad *max7360_keypad = data;
	unsigned int val;
	unsigned int row, col;
	unsigned int release;
	unsigned int code;
	int ret;

	ret = regmap_read(max7360_keypad->regmap, MAX7360_REG_KEYFIFO, &val);
	if (!ret && val == MAX7360_FIFO_OVERFLOW) {
		/* FIFO overflow: ignore it and get next event. */
		dev_err(&max7360_keypad->input->dev, "max7360 FIFO overflow");
		ret = regmap_read(max7360_keypad->regmap, MAX7360_REG_KEYFIFO,
				  &val);
	}
	if (ret) {
		dev_err(&max7360_keypad->input->dev,
			"Failed to read max7360 FIFO");
		return ret;
	}

	if (val == MAX7360_FIFO_EMPTY) {
		dev_dbg(&max7360_keypad->input->dev,
			"Got a spurious interrupt");

		return IRQ_NONE;
	}

	row = FIELD_GET(MAX7360_FIFO_ROW, val);
	col = FIELD_GET(MAX7360_FIFO_COL, val);
	release = val & MAX7360_FIFO_RELEASE;

	code = MATRIX_SCAN_CODE(row, col, MAX7360_ROW_SHIFT);

	dev_dbg(&max7360_keypad->input->dev,
		"key[%d:%d] %s\n", row, col, release ? "release" : "press");

	input_event(max7360_keypad->input, EV_MSC, MSC_SCAN, code);
	input_report_key(max7360_keypad->input, max7360_keypad->keycodes[code],
			 !release);
	input_sync(max7360_keypad->input);

	return IRQ_HANDLED;
}

static int max7360_keypad_open(struct input_dev *pdev)
{
	struct max7360_keypad *max7360_keypad = input_get_drvdata(pdev);
	int ret;

	/*
	 * Somebody is using the device: get out of sleep.
	 */
	ret = regmap_write_bits(max7360_keypad->regmap, MAX7360_REG_CONFIG,
				MAX7360_CFG_SLEEP, MAX7360_CFG_SLEEP);
	if (ret) {
		dev_err(&max7360_keypad->input->dev,
			"Failed to write max7360 configuration");
		return ret;
	}

	return 0;
}

static void max7360_keypad_close(struct input_dev *pdev)
{
	struct max7360_keypad *max7360_keypad = input_get_drvdata(pdev);
	int ret;

	/*
	 * Nobody is using the device anymore: go to sleep.
	 */
	ret = regmap_write_bits(max7360_keypad->regmap, MAX7360_REG_CONFIG,
				MAX7360_CFG_SLEEP, ~MAX7360_CFG_SLEEP);
	if (ret) {
		dev_err(&max7360_keypad->input->dev,
			"Failed to write max7360 configuration");
	}
}

static int max7360_keypad_hw_init(struct max7360_keypad *max7360_keypad)
{
	unsigned int val;
	int ret;

	val = max7360_keypad->debounce_ms - MAX7360_DEBOUNCE_MIN;
	ret = regmap_write_bits(max7360_keypad->regmap, MAX7360_REG_DEBOUNCE,
				MAX7360_DEBOUNCE,
				FIELD_PREP(MAX7360_DEBOUNCE, val));
	if (ret) {
		dev_err(&max7360_keypad->input->dev,
			"Failed to write max7360 debounce configuration");
		return ret;
	}

	ret = regmap_write_bits(max7360_keypad->regmap, MAX7360_REG_INTERRUPT,
				MAX7360_INTERRUPT_TIME_MASK,
				FIELD_PREP(MAX7360_INTERRUPT_TIME_MASK, 1));
	if (ret) {
		dev_err(&max7360_keypad->input->dev,
			"Failed to write max7360 keypad interrupt configuration");
		return ret;
	}

	return 0;
}

static int max7360_keypad_parse_dt(struct platform_device *pdev,
				   struct max7360_keypad *max7360_keypad)
{
	bool no_autorepeat;
	int ret;

	ret = matrix_keypad_parse_properties(&pdev->dev, &max7360_keypad->rows,
					     &max7360_keypad->cols);
	if (ret)
		return ret;

	if (!max7360_keypad->rows || !max7360_keypad->cols ||
	    max7360_keypad->rows > MAX7360_MAX_KEY_ROWS ||
	    max7360_keypad->cols > MAX7360_MAX_KEY_COLS) {
		dev_err(&pdev->dev,
			"Invalid number of columns or rows (%ux%u)\n",
			max7360_keypad->cols, max7360_keypad->rows);
		return -EINVAL;
	}

	max7360_keypad->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!max7360_keypad->regmap) {
		dev_err(&pdev->dev, "Could not get parent regmap\n");
		return -ENODEV;
	}

	no_autorepeat = of_property_read_bool(pdev->dev.of_node,
					      "linux,input-no-autorepeat");
	max7360_keypad->no_autorepeat = no_autorepeat;

	max7360_keypad->debounce_ms = MAX7360_DEBOUNCE_MIN;
	ret = of_property_read_u32(pdev->dev.of_node, "debounce-delay-ms",
				   &max7360_keypad->debounce_ms);
	if (ret == -EINVAL) {
		dev_info(&pdev->dev, "Using default debounce-delay-ms: %u\n",
			 max7360_keypad->debounce_ms);
	} else if (ret < 0) {
		dev_err(&pdev->dev, "Failed to read debounce-delay-ms property\n");
		return ret;
	} else if (max7360_keypad->debounce_ms < MAX7360_DEBOUNCE_MIN ||
		   max7360_keypad->debounce_ms > MAX7360_DEBOUNCE_MAX) {
		dev_err(&pdev->dev,
			"Invalid debounce-delay-ms: %u, should be between %u and %u.\n",
			max7360_keypad->debounce_ms, MAX7360_DEBOUNCE_MIN,
			MAX7360_DEBOUNCE_MAX);
		return -EINVAL;
	}

	return 0;
}

static int max7360_keypad_probe(struct platform_device *pdev)
{
	struct max7360_keypad *max7360_keypad;
	struct input_dev *input;
	unsigned long flags;
	int irq;
	int ret;

	if (!pdev->dev.parent)
		return dev_err_probe(&pdev->dev, -ENODEV, "No parent device\n");

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	max7360_keypad = devm_kzalloc(&pdev->dev, sizeof(*max7360_keypad),
				      GFP_KERNEL);
	if (!max7360_keypad)
		return -ENOMEM;

	ret = max7360_keypad_parse_dt(pdev, max7360_keypad);
	if (ret)
		return ret;

	input = devm_input_allocate_device(&pdev->dev);
	if (!input)
		return dev_err_probe(&pdev->dev, -ENOMEM,
				     "Failed to allocate input device\n");

	max7360_keypad->input = input;

	input->id.bustype = BUS_I2C;
	input->name = pdev->name;
	input->dev.parent = &pdev->dev;

	input->open = max7360_keypad_open;
	input->close = max7360_keypad_close;

	ret = matrix_keypad_build_keymap(NULL, NULL,
					 MAX7360_MAX_KEY_ROWS,
					 MAX7360_MAX_KEY_COLS,
					 max7360_keypad->keycodes, input);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Failed to build keymap\n");

	input_set_capability(input, EV_MSC, MSC_SCAN);
	if (!max7360_keypad->no_autorepeat)
		__set_bit(EV_REP, input->evbit);

	input_set_drvdata(input, max7360_keypad);

	flags = IRQF_TRIGGER_LOW | IRQF_ONESHOT | IRQF_SHARED;
	ret = devm_request_threaded_irq(&pdev->dev, irq, NULL,
					max7360_keypad_irq, flags,
					"max7360-keypad", max7360_keypad);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Failed to register interrupt: %d\n", ret);

	ret = input_register_device(input);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Could not register input device: %d\n",
				     ret);

	platform_set_drvdata(pdev, max7360_keypad);

	device_init_wakeup(&pdev->dev, true);
	ret = dev_pm_set_wake_irq(&pdev->dev, irq);
	if (ret)
		dev_warn(&pdev->dev, "Failed to set up wakeup irq: %d\n", ret);

	ret = max7360_keypad_hw_init(max7360_keypad);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Failed to initialize max7360 keypad\n");

	return 0;
}

static void max7360_keypad_remove(struct platform_device *pdev)
{
	dev_pm_clear_wake_irq(&pdev->dev);
}

#ifdef CONFIG_OF
static const struct of_device_id max7360_keypad_of_match[] = {
	{ .compatible = "maxim,max7360-keypad", },
	{ }
};
MODULE_DEVICE_TABLE(of, max7360_keypad_of_match);
#endif

static struct platform_driver max7360_keypad_driver = {
	.driver = {
		.name	= "max7360-keypad",
		.of_match_table = of_match_ptr(max7360_keypad_of_match),
	},
	.probe		= max7360_keypad_probe,
	.remove		= max7360_keypad_remove,
};
module_platform_driver(max7360_keypad_driver);

MODULE_DESCRIPTION("MAX7360 Keypad driver");
MODULE_AUTHOR("Mathieu Dubois-Briand <mathieu.dubois-briand@bootlin.com>");
MODULE_ALIAS("platform:max7360-keypad");
MODULE_LICENSE("GPL");
