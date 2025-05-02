// SPDX-License-Identifier: GPL-2.0
/*
 * Serial port routines for use during early boot reporting. This code is
 * included from both the compressed kernel and the regular kernel.
 */
#include "boot.h"

#define DEFAULT_SERIAL_PORT 0x3f8 /* ttyS0 */

#define DLAB		0x80

#define TXR             0       /*  Transmit register (WRITE) */
#define RXR             0       /*  Receive register  (READ)  */
#define IER             1       /*  Interrupt Enable          */
#define IIR             2       /*  Interrupt ID              */
#define FCR             2       /*  FIFO control              */
#define LCR             3       /*  Line control              */
#define MCR             4       /*  Modem control             */
#define LSR             5       /*  Line Status               */
#define MSR             6       /*  Modem Status              */
#define DLL             0       /*  Divisor Latch Low         */
#define DLH             1       /*  Divisor latch High        */

#define DEFAULT_BAUD 9600

static unsigned int io_serial_in(unsigned long addr, int offset)
{
	return inb(addr + offset);
}

static void io_serial_out(unsigned long addr, int offset, int value)
{
	outb(value, addr + offset);
}

static void mem8_serial_out(unsigned long addr, int offset, int value)
{
	u8 __iomem *vaddr = (u8 __iomem *)addr;
	/* shift implied by pointer type */
	writeb(value, vaddr + offset);
}

static unsigned int mem8_serial_in(unsigned long addr, int offset)
{
	u8 __iomem *vaddr = (u8 __iomem *)addr;
	/* shift implied by pointer type */
	return readb(vaddr + offset);
}

static void early_serial_configure(unsigned long port, int baud)
{
	unsigned char c;
	unsigned divisor;

	serial_out(port, LCR, 0x3);	/* 8n1 */
	serial_out(port, IER, 0);	/* no interrupt */
	serial_out(port, FCR, 0);	/* no fifo */
	serial_out(port, MCR, 0x3);	/* DTR + RTS */

	divisor	= 115200 / baud;
	c = serial_in(port, LCR);
	serial_out(port, LCR, c | DLAB);
	serial_out(port, DLL, divisor & 0xff);
	serial_out(port, DLH, (divisor >> 8) & 0xff);
	serial_out(port, LCR, c & ~DLAB);
}

/* Assign serial I/O accessors */
static void early_serial_use_io_accessors(void)
{
	/* These will always be IO based ports */
	serial_in = io_serial_in;
	serial_out = io_serial_out;
}

static void early_serial_use_mmio_accessors(void)
{
	/* It is memory mapped - assume 8-bit alignment */
	serial_in = mem8_serial_in;
	serial_out = mem8_serial_out;
}

static void early_serial_init(unsigned long port, int baud)
{
	early_serial_configure(port, baud);

	early_serial_base = port;
}

static unsigned long parse_serial_port(const char *arg, int off, int *pos)
{
	unsigned long port;
	char *e;

	port = simple_strtoull(arg + off, &e, 16);
	if (port == 0 || arg + off == e)
		port = DEFAULT_SERIAL_PORT;
	else
		*pos = e - arg;

	return port;
}

static void parse_earlyprintk(void)
{
	int baud = DEFAULT_BAUD;
	unsigned long port = 0;
	char arg[64];
	int pos = 0;

	if (cmdline_find_option("earlyprintk", arg, sizeof(arg)) > 0) {
		char *e;

		if (!strncmp(arg, "serial", 6)) {
			port = DEFAULT_SERIAL_PORT;
			pos += 6;
		}

		if (arg[pos] == ',')
			pos++;

		/*
		 * make sure we have
		 *	"serial,0x3f8,115200"
		 *	"serial,mmio,0xff010180,115200"
		 *	"serial,ttyS0,115200"
		 *	"ttyS0,115200"
		 */
		if (pos == 7 && !strncmp(arg + pos, "0x", 2)) {
			port = parse_serial_port(arg, pos + 0, &pos);
			early_serial_use_io_accessors();
		} else if (pos == 7 && !strncmp(arg + pos, "mmio,0x", 7)) {
			port = parse_serial_port(arg, pos + 5, &pos);
			early_serial_use_mmio_accessors();
		} else if (!strncmp(arg + pos, "ttyS", 4)) {
			static const int bases[] = { 0x3f8, 0x2f8 };
			int idx = 0;

			/* += strlen("ttyS"); */
			pos += 4;

			if (arg[pos++] == '1')
				idx = 1;

			port = bases[idx];
			early_serial_use_io_accessors();
		}

		if (arg[pos] == ',')
			pos++;

		baud = simple_strtoull(arg + pos, &e, 0);
		if (baud == 0 || arg + pos == e)
			baud = DEFAULT_BAUD;
	}

	if (port)
		early_serial_init(port, baud);
}

#define BASE_BAUD (1843200/16)
static unsigned int probe_baud(int port)
{
	unsigned char lcr, dll, dlh;
	unsigned int quot;

	lcr = serial_in(port, LCR);
	serial_out(port, LCR, lcr | DLAB);
	dll = serial_in(port, DLL);
	dlh = serial_in(port, DLH);
	serial_out(port, LCR, lcr);
	quot = (dlh << 8) | dll;

	return BASE_BAUD / quot;
}

static void parse_console_uart8250(void)
{
	char optstr[64], *options;
	int baud = DEFAULT_BAUD;
	unsigned long port = 0;

	/*
	 * console=uart8250,io,0x3f8,115200n8
	 * need to make sure it is last one console !
	 */
	if (cmdline_find_option("console", optstr, sizeof(optstr)) <= 0)
		return;

	options = optstr;

	if (!strncmp(options, "uart8250,io,", 12)) {
		port = simple_strtoull(options + 12, &options, 0);
		early_serial_use_io_accessors();
	} else if (!strncmp(options, "uart,io,", 8)) {
		port = simple_strtoull(options + 8, &options, 0);
		early_serial_use_io_accessors();
	} else
		return;

	if (options && (options[0] == ','))
		baud = simple_strtoull(options + 1, &options, 0);
	else
		baud = probe_baud(port);

	if (port)
		early_serial_init(port, baud);
}

void console_init(void)
{
	parse_earlyprintk();

	if (!early_serial_base)
		parse_console_uart8250();
}
