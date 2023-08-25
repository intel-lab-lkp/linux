/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * This header provides constants for most thermal bindings.
 *
 * Copyright (C) 2013 Texas Instruments
 *	Eduardo Valentin <eduardo.valentin@ti.com>
 */

#ifndef _DT_BINDINGS_THERMAL_THERMAL_H
#define _DT_BINDINGS_THERMAL_THERMAL_H

/* On cooling devices upper and lower limits */
#define THERMAL_NO_LIMIT		(~0)

/* Possible values for the 'critical-action' property */
#define THERMAL_CRITICAL_ACTION_SHUTDOWN	0
#define THERMAL_CRITICAL_ACTION_REBOOT		1

#endif

