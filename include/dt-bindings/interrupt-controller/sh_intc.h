/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause)
 *
 * SH3/4 INTC EVT - IRQ conversion
 */

#define evt2irq(evt)		((evt) >> 5)
#define irq2evt(irq)		((irq) << 5)

#define IPRDEF(e, o, b)		< e o b >
#define IPRA			0
#define IPRB			4
#define IPRC			8
#define IPRD			12
#define INTPRI00		256
#define IPR_B12			12
#define IPR_B8			8
#define IPR_B4			4
#define IPR_B0			0
