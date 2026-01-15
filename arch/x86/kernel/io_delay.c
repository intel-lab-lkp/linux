// SPDX-License-Identifier: GPL-2.0
/*
 * I/O delay strategies for inb_p/outb_p
 *
 * Allow for a DMI based override of port 0x80, needed for certain HP laptops
 * and possibly other systems. Also allow for the gradual elimination of
 * outb_p/inb_p API uses.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/delay.h>
#include <linux/io.h>

#define IO_DELAY_TYPE_0X80	0
#define IO_DELAY_TYPE_0XED	1
#define IO_DELAY_TYPE_UDELAY	2
#define IO_DELAY_TYPE_NONE	3

int io_delay_type __read_mostly = IO_DELAY_TYPE_NONE;

/*
 * Paravirt wants native_io_delay to be a constant.
 */
void native_io_delay(void)
{
	switch (io_delay_type) {
	default:
	case IO_DELAY_TYPE_0X80:
		asm volatile ("outb %al, $0x80");
		break;
	case IO_DELAY_TYPE_0XED:
		asm volatile ("outb %al, $0xed");
		break;
	case IO_DELAY_TYPE_UDELAY:
		/*
		 * 2 usecs is an upper-bound for the outb delay but
		 * note that udelay doesn't have the bus-level
		 * side-effects that outb does, nor does udelay() have
		 * precise timings during very early bootup (the delays
		 * are shorter until calibrated):
		 */
		udelay(2);
		break;
	case IO_DELAY_TYPE_NONE:
		break;
	}
}
EXPORT_SYMBOL(native_io_delay);

static int __init io_delay_param(char *s)
{
	if (!s)
		return -EINVAL;

	if (!strcmp(s, "0x80"))
		io_delay_type = IO_DELAY_TYPE_0X80;
	else if (!strcmp(s, "0xed"))
		io_delay_type = IO_DELAY_TYPE_0XED;
	else if (!strcmp(s, "udelay"))
		io_delay_type = IO_DELAY_TYPE_UDELAY;
	else if (!strcmp(s, "none"))
		io_delay_type = IO_DELAY_TYPE_NONE;
	else
		return -EINVAL;

	return 0;
}

early_param("io_delay", io_delay_param);
