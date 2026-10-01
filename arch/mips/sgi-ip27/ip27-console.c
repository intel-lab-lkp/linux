/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2001, 2002 Ralf Baechle
 */

#include <asm/page.h>
#include <asm/setup.h>
#include <asm/sn/addrs.h>
#include <asm/sn/agent.h>
#include <asm/sn/klconfig.h>
#include <asm/sn/ioc3.h>
#include <asm/sn/ioc4.h>

#include <linux/serial.h>
#include <linux/serial_core.h>

#include "ip27-common.h"

#define IOC3_CLK	(22000000 / 3)
#define IOC3_FLAGS	(0)

static inline struct ioc3_uartregs *console_uart(void)
{
	struct ioc3 *ioc3;
	nasid_t nasid;

	nasid = (master_nasid == INVALID_NASID) ? get_nasid() : master_nasid;
	ioc3 = (struct ioc3 *)KL_CONFIG_CH_CONS_INFO(nasid)->memory_base;

	return &ioc3->sregs.uarta;
}

/*
 * On IP35 a Tezro drives its console off IOC4, a Fuel off IOC3.
 * console_t.type cannot tell them apart -- a Tezro reads it back as
 * 0x0300fafa, which is neither chip's PCI id -- while uart_base is
 * populated on both, so dispatch on whether that lands on IOC3's own UART.
 */
static inline bool console_is_ioc4(nasid_t nasid)
{
	unsigned long uart_base;

	if (!system_is_ip35)
		return false;

	uart_base = KL_CONFIG_CH_CONS_INFO(nasid)->uart_base;
	if (!uart_base)
		return false;

	/* NODE_SWIN_ADDR() aliases widget 0 away; the console sits on 0xf. */
	if (uart_base < RAW_NODE_SWIN_BASE(nasid, 0) ||
	    uart_base >= RAW_NODE_SWIN_BASE(nasid, SWIN_WIDGET_MASK) +
			 SWIN_SIZE)
		return false;

	return uart_base != (unsigned long)console_uart();
}

static inline struct ioc4_uartregs *console_uart_ioc4(nasid_t nasid)
{
	return (struct ioc4_uartregs *)KL_CONFIG_CH_CONS_INFO(nasid)->uart_base;
}

void prom_putchar(char c)
{
	struct ioc3_uartregs *uart;
	nasid_t nasid;

	nasid = (master_nasid == INVALID_NASID) ? get_nasid() : master_nasid;
	if (console_is_ioc4(nasid)) {
		struct ioc4_uartregs *ioc4_uart = console_uart_ioc4(nasid);

		while ((readb(&ioc4_uart->i4u_lsr) & 0x20) == 0)
			;
		writeb(c, &ioc4_uart->i4u_thr);
		return;
	}

	uart = console_uart();

	while ((readb(&uart->iu_lsr) & 0x20) == 0)
		;
	writeb(c, &uart->iu_thr);
}
