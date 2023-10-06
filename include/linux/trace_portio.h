/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/types.h>

extern void do_trace_portio_read(u32 value, u16 port, char width);
extern void do_trace_portio_write(u32 value, u16 port, char width);
