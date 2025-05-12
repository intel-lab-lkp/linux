/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#ifndef __CPC_BLE_H
#define __CPC_BLE_H

#define CPC_BLUETOOTH_ENDPOINT_NAME "silabs,cpc-ble"

int cpc_ble_drv_register(void);
void cpc_ble_drv_unregister(void);

#endif
