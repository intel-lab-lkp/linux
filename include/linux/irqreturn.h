/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_IRQRETURN_H
#define _LINUX_IRQRETURN_H

/**
 * enum irqreturn - irqreturn type values
 * @IRQ_NONE:		interrupt was not from this device or was not handled
 * @IRQ_HANDLED:	interrupt was handled by this device
 * @IRQ_WAKE_THREAD:	handler requests to wake the handler thread
 * @IRQ_HANDLED_MANY:	interrupt was handled by this device multiple times
 *			should be the only bit set in the first 3 bits, and
 *			carry a count > 1 in the next bits.
 */
enum irqreturn {
	IRQ_NONE		= (0 << 0),
	IRQ_HANDLED		= (1 << 0),
	IRQ_WAKE_THREAD		= (1 << 1),
	IRQ_HANDLED_MANY	= (1 << 2),
	IRQ_RETMASK		= IRQ_HANDLED |	IRQ_WAKE_THREAD | IRQ_HANDLED_MANY,
};

#define IRQ_HANDLED_MANY_SHIFT (3)

typedef int irqreturn_t;
#define IRQ_RETVAL(x)	((x) ? IRQ_HANDLED : IRQ_NONE)
#define	IRQ_RETVAL_MANY(x)							\
({										\
	__typeof__(x) __x = (x);						\
	irqreturn_t __ret;							\
	if (__x == 0)								\
		__ret = IRQ_NONE;						\
	else if (__x == 1)							\
		__ret = IRQ_HANDLED;						\
	else									\
		__ret = IRQ_HANDLED_MANY | (__x << IRQ_HANDLED_MANY_SHIFT);	\
	__ret;									\
})

#define IRQ_HANDLED_MANY_GET(x)	((x) >> IRQ_HANDLED_MANY_SHIFT)

#endif
