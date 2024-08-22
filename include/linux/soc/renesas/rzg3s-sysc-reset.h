/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __SOC_RENESAS_SYSC_RESET_RZG3S_H
#define __SOC_RENESAS_SYSC_RESET_RZG3S_H

#include <linux/auxiliary_bus.h>
#include <linux/spinlock_types.h>
#include <linux/container_of.h>

/**
 * struct rzg3s_sysc_reset_adev - SYSC reset auxiliary device
 * @base: base address
 * @lock: lock
 * @adev: auxiliary device
 */
struct rzg3s_sysc_reset_adev {
	void __iomem *base;
	spinlock_t *lock;
	struct auxiliary_device adev;
};

#define to_rzg3s_sysc_reset_adev(a)	container_of(a, struct rzg3s_sysc_reset_adev, adev)

#endif
