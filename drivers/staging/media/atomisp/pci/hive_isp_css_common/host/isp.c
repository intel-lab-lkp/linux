// SPDX-License-Identifier: GPL-2.0
/*
 * Support for Intel Camera Imaging ISP subsystem.
 * Copyright (c) 2010-2015, Intel Corporation.
 */

#include <system_global.h>
#include "isp.h"

#ifndef __INLINE_ISP__
#include "isp_private.h"
#endif /* __INLINE_ISP__ */

#include "assert_support.h"

void cnd_isp_irq_enable(
    const isp_ID_t		ID,
    const bool		cnd)
{
	if (cnd) {
		isp_ctrl_setbit(ID, ISP_IRQ_READY_REG, ISP_IRQ_READY_BIT);
		/* Enabling the IRQ immediately triggers an interrupt, clear it */
		isp_ctrl_setbit(ID, ISP_IRQ_CLEAR_REG, ISP_IRQ_CLEAR_BIT);
	} else {
		isp_ctrl_clearbit(ID, ISP_IRQ_READY_REG,
				  ISP_IRQ_READY_BIT);
	}
	return;
}
