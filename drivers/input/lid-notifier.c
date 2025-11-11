// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Lid event notifier
 *
 *  Copyright (c) 2025 Jonathan Denose <jdenose@google.com>
 */

#include <linux/device.h>
#include <linux/input.h>
#include <linux/notifier.h>

static struct input_handler lid_handler;
static struct atomic_notifier_head input_notifier_head;

int register_lid_notifier(struct notifier_block *notifier)
{
	return atomic_notifier_chain_register(&input_notifier_head, notifier);
}
EXPORT_SYMBOL(register_lid_notifier);

static int lid_handler_connect(struct input_handler *handler,
		struct input_dev *input_dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = devm_kzalloc(&input_dev->dev, sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = input_dev;
	handle->handler = handler;
	handle->name = "lid";

	error = input_register_handle(handle);
	if (error)
		goto err_free_handle;

	error = input_open_device(handle);
	if (error)
		goto err_unregister_handle;

	return 0;

 err_unregister_handle:
	input_unregister_handle(handle);
 err_free_handle:
	kfree(handle);
	return error;
}

static void lid_handler_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
}

static void lid_handler_event(struct input_handle *handle, unsigned int type,
		unsigned int code, int value)
{
	if (type == EV_SW && code == SW_LID)
		atomic_notifier_call_chain(&input_notifier_head, value, handle->dev);
}

static const struct input_device_id lid_handler_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT | INPUT_DEVICE_ID_MATCH_SWBIT
						| INPUT_DEVICE_ID_MATCH_BUS,
		.evbit = { BIT_MASK(EV_SW) },
		.swbit = { [BIT_WORD(SW_LID)] = BIT_MASK(SW_LID) },
		.bustype = 0x19
	},
	{ },
};

static struct input_handler lid_handler = {
	.connect = lid_handler_connect,
	.disconnect = lid_handler_disconnect,
	.event = lid_handler_event,
	.name = "lid",
	.id_table = lid_handler_ids
};

static int __init lid_notifier_init(void)
{
	return input_register_handler(&lid_handler);
}
module_init(lid_notifier_init);

static void __exit lid_notifier_exit(void)
{
	input_unregister_handler(&lid_handler);
}
module_exit(lid_notifier_exit);

MODULE_AUTHOR("Jonathan Denose <jdenose@google.com>");
MODULE_DESCRIPTION("Lid event notifier");
MODULE_LICENSE("GPL");
