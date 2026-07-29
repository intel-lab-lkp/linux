/* SPDX-License-Identifier: GPL-2.0 */
/* Marvell switch driver
 *
 * Copyright (C) 2026 Marvell.
 *
 */
#ifndef SW_FDB_H_
#define SW_FDB_H_

#include <linux/kconfig.h>

#if IS_ENABLED(CONFIG_OCTEONTX_SWITCH)
void sw_fdb_deinit(void);
int sw_fdb_init(void);
#else
static inline void sw_fdb_deinit(void) {}
static inline int sw_fdb_init(void) { return 0; }
#endif

#endif /* SW_FDB_H_ */
