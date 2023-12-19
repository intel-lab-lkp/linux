/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Dummy definitions for the Auxiliary I/O register.
 *
 * Copyright (C) 1995 David S. Miller (davem@caip.rutgers.edu)
 */
#ifndef _SPARC_AUXIO_H
#define _SPARC_AUXIO_H

/*
 * The following routines are provided for driver-compatibility
 * with sparc64 (primarily sunlance.c)
 */

#define AUXIO_LTE_ON    1
#define AUXIO_LTE_OFF   0

#define auxio_set_lte(on)

#endif /* !(_SPARC_AUXIO_H) */
