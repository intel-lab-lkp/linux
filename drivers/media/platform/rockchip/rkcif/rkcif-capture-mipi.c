// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip Camera Interface (CIF) Driver
 *
 * Copyright (C) 2018 Rockchip Electronics Co., Ltd.
 * Copyright (C) 2025 Michael Riesch <michael.riesch@wolfvision.net>
 */

#include "rkcif-capture-mipi.h"
#include "rkcif-common.h"
#include "rkcif-stream.h"

irqreturn_t rkcif_mipi_isr(int irq, void *ctx)
{
	irqreturn_t ret = IRQ_NONE;

	return ret;
}

int rkcif_mipi_register(struct rkcif_device *rkcif)
{
	return 0;
}

void rkcif_mipi_unregister(struct rkcif_device *rkcif)
{
}
