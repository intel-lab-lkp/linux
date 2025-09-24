// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/rv.h>

#define MODULE_NAME "deadline"

#include "deadline.h"

struct rv_monitor rv_deadline = {
	.name = "deadline",
	.description = "container for several deadline scheduler specifications.",
	.enable = NULL,
	.disable = NULL,
	.reset = NULL,
	.enabled = 0,
};

static int __init register_deadline(void)
{
	return rv_register_monitor(&rv_deadline, NULL);
}

static void __exit unregister_deadline(void)
{
	rv_unregister_monitor(&rv_deadline);
}

module_init(register_deadline);
module_exit(unregister_deadline);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Gabriele Monaco <gmonaco@redhat.com>");
MODULE_DESCRIPTION("deadline: container for several deadline scheduler specifications.");
