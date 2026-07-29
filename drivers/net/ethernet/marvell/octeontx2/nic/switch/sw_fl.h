/* SPDX-License-Identifier: GPL-2.0 */
/* Marvell switch driver
 *
 * Copyright (C) 2026 Marvell.
 *
 */
#ifndef SW_FL_H_
#define SW_FL_H_

#include <linux/kconfig.h>

int sw_fl_setup_ft_block_ingress_cb(enum tc_setup_type type,
				    void *type_data, void *cb_priv);

#if IS_ENABLED(CONFIG_OCTEONTX_SWITCH)
#include <net/pkt_cls.h>

void sw_fl_deinit(void);
int sw_fl_init(void);
#else
static inline void sw_fl_deinit(void) {}
static inline int sw_fl_init(void) { return 0; }

#endif

#endif /* SW_FL_H_ */
