/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SiTime SiT9531x DPLL subsystem interface
 *
 * Copyright (C) 2026 SiTime Corp.
 * Author: Ali Rouhi <arouhi@sitime.com>
 * Author: Oleg Zadorozhnyi <Oleg.Zadorozhnyi@devoxsoftware.com>
 *
 * DPLL device and pin structures, and function declarations for
 * the DPLL registration and callback layer.
 */

#ifndef _SIT9531X_DPLL_H
#define _SIT9531X_DPLL_H

#include <linux/dpll.h>
#include <linux/list.h>
#include <linux/types.h>
#include <linux/workqueue.h>

struct sit9531x_dev;

/**
 * struct sit9531x_dpll_pin - per-pin DPLL state
 * @list:		linked list entry within dpll->pins
 * @dpll:		back-pointer to owning DPLL
 * @dpll_pin:		registered dpll_pin object
 * @tracker:		reference count tracker for dpll_pin_get/put
 * @fwnode:		firmware node handle (from DT)
 * @label:		package label string (e.g. "IN0", "OUT3")
 * @dir:		pin direction (INPUT or OUTPUT)
 * @id:			hardware pin index (input 0-N or output 0-M)
 * @prio:		current priority for automatic input selection
 * @pin_state:		last saved pin state
 * @phase_adjust:	current phase adjustment in picoseconds
 * @phase_offset:	last measured phase offset
 * @esync_control:	esync/sysref control allowed for this output pin
 * @esync_freq:		last requested esync frequency (0 means disabled)
 */
struct sit9531x_dpll_pin {
	struct list_head		list;
	struct sit9531x_dpll		*dpll;
	struct dpll_pin			*dpll_pin;
	dpll_tracker			tracker;
	struct fwnode_handle		*fwnode;
	char				label[8];
	enum dpll_pin_direction		dir;
	u8				id;
	u8				prio;
	enum dpll_pin_state		pin_state;
	s32				phase_adjust;
	s64				phase_offset;
	bool				esync_control;
	u64				esync_freq;
};

/**
 * struct sit9531x_dpll - per-PLL DPLL device state
 * @list:		linked list entry within sitdev->dplls
 * @dev:		back-pointer to parent sit9531x_dev
 * @dpll_dev:		registered dpll_device object
 * @tracker:		reference count tracker for dpll_device_get/put
 * @ops:		copy of dpll_device_ops (per-instance)
 * @pins:		list of registered pins
 * @id:			PLL channel number (0 = PLLA, 3 = PLLD)
 * @lock_status:	cached DPLL lock status
 * @change_work:	work for sending device change notifications
 */
struct sit9531x_dpll {
	struct list_head		list;
	struct sit9531x_dev		*dev;
	struct dpll_device		*dpll_dev;
	dpll_tracker			tracker;
	struct dpll_device_ops		ops;
	struct list_head		pins;
	u8				id;
	enum dpll_lock_status		lock_status;
	struct work_struct		change_work;
};

/* ---- DPLL allocation and registration ---- */
struct sit9531x_dpll *sit9531x_dpll_alloc(struct sit9531x_dev *sitdev, u8 ch);
void sit9531x_dpll_free(struct sit9531x_dpll *sitdpll);
int  sit9531x_dpll_register(struct sit9531x_dpll *sitdpll);
void sit9531x_dpll_unregister(struct sit9531x_dpll *sitdpll);

/* ---- Periodic change detection ---- */
void sit9531x_dpll_changes_check(struct sit9531x_dpll *sitdpll);

#endif /* _SIT9531X_DPLL_H */
