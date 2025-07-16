// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2012-2025 Synaptics Incorporated
 */

#include <linux/kernel.h>
#include <linux/rmi.h>
#include <linux/input.h>
#include <linux/slab.h>
#include "rmi_driver.h"

#define RMI_f21_FORCE_CLICK			BIT(0)
#define RMI_f21_DATA_REGS_MAX_SIZE	255
#define RMI_f21_FORCEPAD_BUTTON_COUNT	1

struct f21_data {
	/* Query Data */
	u8 data_regs[RMI_f21_DATA_REGS_MAX_SIZE];
	struct input_dev *input;
	u16 key_code;

	unsigned int attn_data_size;
	unsigned int attn_data_index_for_button;
	unsigned int sensor_count;
	unsigned int max_number_Of_finger;
	unsigned int data_reg_size;
	unsigned int data_reg_index_for_button;
	unsigned int query15_offset;
};

static irqreturn_t rmi_f21_attention(int irq, void *ctx)
{
	struct rmi_function *fn = ctx;
	struct f21_data *f21 = dev_get_drvdata(&fn->dev);
	struct rmi_driver_data *drvdata = dev_get_drvdata(&fn->rmi_dev->dev);
	int error;
	bool pressed = false;

	if (drvdata->attn_data.data) {
		if (drvdata->attn_data.size < f21->attn_data_size) {
			dev_warn(&fn->dev, "f21 interrupted, but data is missing\n");
			return IRQ_HANDLED;
		}
		pressed = (((u8 *)drvdata->attn_data.data)[f21->attn_data_index_for_button] &
					RMI_f21_FORCE_CLICK);
		drvdata->attn_data.data += f21->attn_data_size;
		drvdata->attn_data.size -= f21->attn_data_size;
	} else {
		error = rmi_read_block(fn->rmi_dev, fn->fd.data_base_addr,
					f21->data_regs, f21->data_reg_size);
		if (error) {
			dev_err(&fn->dev, "%s: Failed to read f21 data registers: %d\n",
				__func__, error);
			return IRQ_RETVAL(error);
		}
		pressed = (f21->data_regs[f21->data_reg_index_for_button] &
					RMI_f21_FORCE_CLICK);
	}

	input_report_key(f21->input, f21->key_code, pressed);

	return IRQ_HANDLED;
}

static int rmi_f21_config(struct rmi_function *fn)
{
	struct rmi_driver *drv = fn->rmi_dev->driver;

	drv->set_irq_bits(fn->rmi_dev, fn->irq_mask);

	return 0;
}

static int rmi_f21_initialize(struct rmi_function *fn, struct f21_data *f21)
{
	struct input_dev *input = f21->input;

	f21->key_code = BTN_LEFT;
	input_set_capability(input, EV_KEY, f21->key_code);
	input->keycode = &f21->key_code;
	input->keycodesize = sizeof(f21->key_code);
	input->keycodemax = RMI_f21_FORCEPAD_BUTTON_COUNT;

	__set_bit(INPUT_PROP_BUTTONPAD, input->propbit);

	return 0;
}

static int rmi_f21_probe(struct rmi_function *fn)
{
	struct rmi_device *rmi_dev = fn->rmi_dev;
	struct rmi_driver_data *drv_data = dev_get_drvdata(&rmi_dev->dev);
	struct f21_data *f21;
	int error;

	if (!drv_data->input) {
		dev_info(&fn->dev, "f21: no input device found, ignoring\n");
		return -ENXIO;
	}

	f21 = devm_kzalloc(&fn->dev, sizeof(*f21), GFP_KERNEL);
	if (!f21)
		return -ENOMEM;

	f21->input = drv_data->input;

	error = rmi_f21_initialize(fn, f21);
	if (error)
		return error;

	dev_set_drvdata(&fn->dev, f21);

	f21->sensor_count = fn->fd.query_base_addr & (BIT(0) | BIT(1) | BIT(2) | BIT(3));

	if (fn->fd.query_base_addr & BIT(5)) {
		if (fn->fd.query_base_addr & BIT(6))
			f21->query15_offset = 2;
		else
			f21->query15_offset = 1;

		rmi_read_block(fn->rmi_dev, fn->fd.query_base_addr + f21->query15_offset,
					f21->data_regs, 1);
		f21->max_number_Of_finger = f21->data_regs[0] & 0x0F;
	} else {
		dev_info(&fn->dev, "f21_query15 doesn't support.\n");
		f21->query15_offset = 0;
		f21->max_number_Of_finger = 5;
	}

	if (fn->fd.query_base_addr & BIT(6)) {
		dev_info(&fn->dev, "Support new F21 feature.\n");
		/*Each finger uses one byte, and the button state uses one byte.*/
		f21->attn_data_size = f21->max_number_Of_finger + 1;
		f21->attn_data_index_for_button = f21->attn_data_size - 1;
		/*
		 * Each sensor uses two bytes, the button state uses one byte,
		 * and each finger uses two bytes.
		 */
		f21->data_reg_size = f21->sensor_count * 2 + 1 +
								f21->max_number_Of_finger * 2;
		f21->data_reg_index_for_button = f21->sensor_count * 2;
	} else {
		dev_info(&fn->dev, "Support old F21 feature.\n");
		/*Each finger uses two bytes, and the button state uses one byte.*/
		f21->attn_data_size = f21->sensor_count * 2 + 1;
		f21->attn_data_index_for_button = f21->attn_data_size - 1;
		/*Each finger uses two bytes, and the button state uses one byte.*/
		f21->data_reg_size = f21->sensor_count * 2 + 1;
		f21->data_reg_index_for_button = f21->data_reg_size - 1;
	}
	return 0;
}

struct rmi_function_handler rmi_f21_handler = {
	.driver = {
		.name = "rmi4_f21",
	},
	.func = 0x21,
	.probe = rmi_f21_probe,
	.config = rmi_f21_config,
	.attention = rmi_f21_attention,
};
