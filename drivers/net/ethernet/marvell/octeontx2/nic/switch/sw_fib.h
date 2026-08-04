/* SPDX-License-Identifier: GPL-2.0 */
/* Marvell switch driver
 *
 * Copyright (C) 2026 Marvell.
 *
 */
#ifndef SW_FIB_H_
#define SW_FIB_H_

#include <linux/kconfig.h>

#if IS_ENABLED(CONFIG_OCTEONTX_SWITCH)
void sw_fib_deinit(void);
int sw_fib_init(void);
#else
static inline void sw_fib_deinit(void) {}
static inline int sw_fib_init(void) { return 0; }
#endif

#endif /* SW_FIB_H_ */
