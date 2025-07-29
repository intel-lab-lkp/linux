/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2025, Intel Corporation. */

#ifndef _ICE_TX_CLK_H_
#define _ICE_TX_CLK_H_

#include "ice.h"

#if IS_ENABLED(CONFIG_NET_TX_CLK)
void ice_tx_clk_init(struct ice_pf *pf);
void ice_tx_clk_deinit(struct ice_pf *pf);
#else
static inline void ice_tx_clk_init(struct ice_pf *pf) { }
static inline void ice_tx_clk_deinit(struct ice_pf *pf) { }
#endif

#endif /* _ICE_TX_CLK_H_ */
