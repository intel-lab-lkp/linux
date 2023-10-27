// SPDX-License-Identifier: GPL-2.0
/*
 * MIPS UHI semihosting based earlycon
 *
 * Copyright (C) 2023 Jiaxun Yang <jiaxun.yang@flygoat.com>
 */

#include <linux/kernel.h>
#include <linux/console.h>
#include <linux/init.h>
#include <linux/serial_core.h>
#include <asm/uhi.h>

static int stdin_fd = -1;
static int stdout_fd = -1;

static void uhi_plog_write(struct console *con, const char *s, unsigned int n)
{
	uhi_plog(s, 0);
}

static void uhi_stdout_write(struct console *con, const char *s, unsigned int n)
{
	if (stdout_fd < 0)
		return;

	uhi_write(stdout_fd, s, n);
}

#ifdef CONFIG_CONSOLE_POLL
static int uhi_stdin_read(struct console *con, char *s, unsigned int n)
{
	if (stdin_fd < 0)
		return 0;

	return uhi_read(stdin_fd, s, n);
}
#endif

static int uhi_stdio_fd_open(struct console *co, char *options)
{
	/*
	 * You have to open both stdin and stdout to get console work
	 * properly on some old CodeScape debugger.
	 */
	stdin_fd = uhi_open("/dev/stdin", UHI_O_RDONLY, 0);
	stdout_fd = uhi_open("/dev/stdout", UHI_O_WRONLY, 0);

	return (stdin_fd < 0 || stdout_fd < 0) ? -ENODEV : 0;
}

static int uhi_stdio_fd_close(struct console *co)
{
	int ret1 = 0, ret2 = 0;

	if (stdin_fd >= 0)
		ret1 = uhi_close(stdin_fd);
	if (stdout_fd >= 0)
		ret2 = uhi_close(stdout_fd);

	return (ret1 < 0 || ret2 < 0) ? -ENODEV : 0;
}

static int
__init early_uhi_setup(struct earlycon_device *device, const char *opt)
{
	device->con->write = uhi_plog_write;
	return 0;
}

static int
__init early_uhi_stdio_setup(struct earlycon_device *device, const char *opt)
{

	device->con->setup = uhi_stdio_fd_open;
	device->con->exit = uhi_stdio_fd_close;
	device->con->write = uhi_stdout_write;
#ifdef CONFIG_CONSOLE_POLL
	device->con->read = uhi_stdin_read;
#endif
	return 0;
}

EARLYCON_DECLARE(uhi, early_uhi_setup);
EARLYCON_DECLARE(uhi_stdio, early_uhi_stdio_setup);
