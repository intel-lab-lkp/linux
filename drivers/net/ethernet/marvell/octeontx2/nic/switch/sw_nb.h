/* SPDX-License-Identifier: GPL-2.0 */
/* Marvell switch driver
 *
 * Copyright (C) 2026 Marvell.
 *
 */
#ifndef SW_NB_H_
#define SW_NB_H_

#include <linux/kconfig.h>

#if IS_ENABLED(CONFIG_OCTEONTX_SWITCH)
int sw_nb_register(void);
int sw_nb_unregister(void);
#else
static inline int sw_nb_register(void) { return 0; }
static inline int sw_nb_unregister(void) { return 0; }
#endif

#endif /* SW_NB_H_ */
