/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * This header provides constants for Renesas RZ/G2L family pinctrl bindings.
 *
 * Copyright (C) 2021 Renesas Electronics Corp.
 *
 */

#ifndef __DT_BINDINGS_RZG2L_PINCTRL_H
#define __DT_BINDINGS_RZG2L_PINCTRL_H

#define RZG2L_PINS_PER_PORT	8

/* PORT_Px = Offset address of PFC_P_mn  - 0x20 */
#define PORT_P0		0
#define PORT_P1		1
#define PORT_P2		2
#define PORT_P3		3
#define PORT_P4		4
#define PORT_P5		5
#define PORT_P6		6
#define PORT_P7		7
#define PORT_P8		8
#define PORT_P9		9
#define PORT_PA		10
#define PORT_PB		11
#define PORT_PC		12
#define PORT_PD		13
#define PORT_PE		14
#define PORT_PF		15
#define PORT_PG		16
#define PORT_PH		17
#define PORT_PI		18
#define PORT_PJ		19
#define PORT_PK		20
#define PORT_PL		21
#define PORT_PM		22
#define PORT_PN		23
#define PORT_PO		24
#define PORT_PP		25
#define PORT_PQ		26
#define PORT_PR		27
#define PORT_PS		28

/*
 * Create the pin index from its bank and position numbers and store in
 * the upper 16 bits the alternate function identifier
 */
#define RZG2L_PORT_PINMUX(b, p, f)	((b) * RZG2L_PINS_PER_PORT + (p) | ((f) << 16))
#define RZG3E_PORT_PINMUX(b, p, f)	RZG2L_PORT_PINMUX(PORT_P##b, p, f)
#define RZV2H_PORT_PINMUX(b, p, f)	RZG2L_PORT_PINMUX(PORT_P##b, p, f)

/* Convert a port and pin label to its global pin index */
#define RZG2L_GPIO(port, pin)	((port) * RZG2L_PINS_PER_PORT + (pin))
#define RZG3E_GPIO(port, pin)	RZG2L_GPIO(PORT_P##port, pin)
#define RZV2H_GPIO(port, pin)	RZG2L_GPIO(PORT_P##port, pin)

#endif /* __DT_BINDINGS_RZG2L_PINCTRL_H */
