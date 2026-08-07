/* SPDX-License-Identifier: GPL-2.0 or BSD-3-Clause */
/*
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */

#ifndef _CPORT_TRAPS_H
#define _CPORT_TRAPS_H

#include "cport.h"

/*
 * Facility to handle CPORT "TRAPS".
 */

/*
 * Handler prototype for callbacks.
 *
 * @traps indicates which traps are being reported, and is not restricted
 * to the trap(s) registered to this callback. At least one of the traps
 * that was requested will be set.
 */
typedef void (*cport_trap_handler)(struct hfi2_devdata *dd,
				   struct cport_trap_status traps);

/*
 * Register for a callback when certain CPORT TRAPs occur.
 *
 * @traps indicates the TRAPs to be detected.
 * @func will be called when at least one of @traps is set.
 */
int hfi2_register_cport_trap(struct hfi2_devdata *dd, struct cport_trap_status traps,
			cport_trap_handler func);

/*
 * Deregister for a callback on CPORT TRAPs.
 *
 * @func All registered callbacks using this function will be removed.
 *
 * On return, @func will not be called for any traps.
 */
int hfi2_deregister_cport_trap(struct hfi2_devdata *dd, cport_trap_handler func);

#endif /* _CPORT_TRAPS_H */
