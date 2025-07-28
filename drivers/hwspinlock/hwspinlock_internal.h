/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Hardware spinlocks internal header
 *
 * Copyright (C) 2010 Texas Instruments Incorporated - http://www.ti.com
 *
 * Contact: Ohad Ben-Cohen <ohad@wizery.com>
 */

#ifndef __HWSPINLOCK_HWSPINLOCK_H
#define __HWSPINLOCK_HWSPINLOCK_H

static inline int hwlock_to_id(struct hwspinlock *hwlock)
{
	int local_id = hwlock - &hwlock->bank->lock[0];

	return hwlock->bank->base_id + local_id;
}

#endif /* __HWSPINLOCK_HWSPINLOCK_H */
