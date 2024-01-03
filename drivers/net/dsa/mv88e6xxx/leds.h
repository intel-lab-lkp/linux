/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Marvell 88E6xxx Switch LED support. */

#ifndef _MV88E6XXX_LEDS_H
#define _MV88E6XXX_LEDS_H

#include "chip.h"

int mv88e6xxx_port_setup_leds(struct dsa_switch *ds, int port);

#endif /* _MV88E6XXX_LEDS_H */
