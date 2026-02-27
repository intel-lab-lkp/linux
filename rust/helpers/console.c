// SPDX-License-Identifier: GPL-2.0

/*
 * Rust helpers for console.
 */

#include <linux/console.h>

void rust_helper_register_console(struct console *console)
{
	register_console(console);
}

int rust_helper_unregister_console(struct console *console)
{
	return unregister_console(console);
}

bool rust_helper_console_is_registered(struct console *console)
{
	return console_is_registered(console);
}
