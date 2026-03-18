/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef _XE_SYSCTRL_TYPES_H_
#define _XE_SYSCTRL_TYPES_H_

#include <linux/mutex.h>
#include <linux/types.h>

struct xe_mmio;

/**
 * struct xe_sysctrl - System Controller driver context
 */
struct xe_sysctrl {
	/** @mmio: MMIO region for system control registers */
	struct xe_mmio *mmio;

	/** @cmd_lock: Mutex protecting mailbox command operations */
	struct mutex cmd_lock;

	/**
	 * @phase_bit: MKHI message boundary phase toggle bit
	 *
	 * Phase bit alternates between 0 and 1 for consecutive
	 * messages to help distinguish message boundaries.
	 */
	bool phase_bit;
};

#endif /* _XE_SYSCTRL_TYPES_H_ */
