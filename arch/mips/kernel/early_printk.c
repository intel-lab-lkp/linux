/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2002, 2003, 06, 07 Ralf Baechle (ralf@linux-mips.org)
 * Copyright (C) 2007 MIPS Technologies, Inc.
 *   written by Ralf Baechle (ralf@linux-mips.org)
 */
#include <linux/kernel.h>
#include <linux/console.h>
#include <linux/printk.h>
#include <linux/init.h>

#include <asm/setup.h>

#ifdef CONFIG_DEBUG_LL
extern void printascii(const char *);

static void early_console_write(struct console *con, const char *s, unsigned n)
{
	char buf[128];

	while (n) {
		unsigned int l = min(n, sizeof(buf)-1);

		memcpy(buf, s, l);
		buf[l] = 0;
		s += l;
		n -= l;
		printascii(buf);
	}
}
#else
static void early_console_write(struct console *con, const char *s, unsigned n)
{
	while (n-- && *s) {
		if (*s == '\n')
			prom_putchar('\r');
		prom_putchar(*s);
		s++;
	}
}
#endif

static struct console early_console_prom = {
	.name	= "early",
	.write	= early_console_write,
	.flags	= CON_PRINTBUFFER | CON_BOOT,
	.index	= -1
};

void __init setup_early_printk(void)
{
	if (early_console)
		return;
	early_console = &early_console_prom;

	register_console(&early_console_prom);
}
