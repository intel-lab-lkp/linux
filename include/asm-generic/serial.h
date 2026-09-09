/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_GENERIC_SERIAL_H
#define __ASM_GENERIC_SERIAL_H

/*
 * This should not be an architecture-specific definition.
 *
 * Traditionally, it describes i8250 and related serial ports using
 * a 1.8432 MHz UART clock.
 */
#define BASE_BAUD (1843200 / 16)

#endif /* __ASM_GENERIC_SERIAL_H */
