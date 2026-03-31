/* SPDX-License-Identifier: GPL-2.0 */
/******************************************************************************
 *
 * Copyright(c) 2007 - 2011 Realtek Corporation. All rights reserved.
 *
 ******************************************************************************/
#ifndef __SDIO_HAL_H__
#define __SDIO_HAL_H__

u8 sd_int_isr(struct adapter *padapter);
void sd_int_dpc(struct adapter *padapter);

#endif /* __SDIO_HAL_H__ */
