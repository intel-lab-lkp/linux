// SPDX-License-Identifier: GPL-2.0+
/*
 * Serial base console options handling
 *
 * Copyright (C) 2023 Texas Instruments Incorporated - https://www.ti.com/
 * Author: Tony Lindgren <tony@atomide.com>
 */

#include <linux/init.h>
#include <linux/list.h>
#include <linux/kernel.h>
#include <linux/serial_core.h>
#include <linux/slab.h>

#include "serial_base.h"

static LIST_HEAD(serial_base_consoles);

struct serial_base_console {
	struct list_head node;
	char *name;
	char *opt;
};

/*
 * Adds a preferred console for a serial port if console=DEVNAME:0.0
 * style addressing is used for the kernel command line. Translates
 * from DEVNAME:0.0 to port->dev_name such as ttyS. Duplicates are
 * ignored by add_preferred_console().
 */
int serial_base_add_preferred_console(struct uart_driver *drv,
				      struct uart_port *port)
{
	struct serial_base_console *entry;
	char *port_match;

	port_match = kasprintf(GFP_KERNEL, "%s:%i.%i", dev_name(port->dev),
			       port->ctrl_id, port->port_id);
	if (!port_match)
		return -ENOMEM;

	list_for_each_entry(entry, &serial_base_consoles, node) {
		if (!strcmp(port_match, entry->name)) {
			add_preferred_console(drv->dev_name, port->line,
					      entry->opt);
			break;
		}
	}

	kfree(port_match);

	return 0;
}
EXPORT_SYMBOL_GPL(serial_base_add_preferred_console);

/* Adds a command line console to the list of consoles for driver probe time */
static int __init serial_base_add_con(char *name, char *opt)
{
	struct serial_base_console *con;

	con = kzalloc(sizeof(*con), GFP_KERNEL);
	if (!con)
		return -ENOMEM;

	con->name = kstrdup(name, GFP_KERNEL);
	if (!con->name)
		goto free_con;

	if (opt) {
		con->opt = kstrdup(opt, GFP_KERNEL);
		if (!con->name)
			goto free_name;
	}

	list_add_tail(&con->node, &serial_base_consoles);

	return 0;

free_name:
	kfree(con->name);

free_con:
	kfree(con);

	return -ENOMEM;
}

/* Parse console name and options */
static int __init serial_base_parse_one(char *param, char *val,
					const char *unused, void *arg)
{
	char *opt;

	if (strcmp(param, "console"))
		return 0;

	if (!val)
		return 0;

	opt = strchr(val, ',');
	if (opt) {
		opt[0] = '\0';
		opt++;
	}

	if (!strlen(val))
		return 0;

	return serial_base_add_con(val, opt);
}

/*
 * The "console=" option is handled by console_setup() in printk. We can't use
 * early_param() as do_early_param() checks for "console" and "earlycon" options
 * so console_setup() potentially handles console also early. Use parse_args().
 */
static int __init serial_base_opts_init(void)
{
	char *command_line;

	command_line = kstrdup(boot_command_line, GFP_KERNEL);
	if (!command_line)
		return -ENOMEM;

	parse_args("Setting serial core console", command_line,
		   NULL, 0, -1, -1, NULL, serial_base_parse_one);

	kfree(command_line);

	return 0;
}

arch_initcall(serial_base_opts_init);
