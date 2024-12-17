/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Rockchip Camera Interface (CIF) Driver
 *
 * Copyright (C) 2018 Rockchip Electronics Co., Ltd.
 * Copyright (C) 2023 Mehdi Djait <mehdi.djait@bootlin.com>
 * Copyright (C) 2024 Michael Riesch <michael.riesch@wolfvision.net>
 */

#ifndef _CIF_CAPTURE_DVP_H
#define _CIF_CAPTURE_DVP_H

#include "cif-common.h"

extern const struct cif_dvp_match_data px30_vip_dvp_match_data;
extern const struct cif_dvp_match_data rk3568_vicap_dvp_match_data;

int cif_dvp_register(struct cif_device *dev);

void cif_dvp_unregister(struct cif_device *dev);

irqreturn_t cif_dvp_isr(int irq, void *ctx);

#endif
