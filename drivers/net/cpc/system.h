/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#ifndef __CPC_SYSTEM_H
#define __CPC_SYSTEM_H

#define CPC_SYSTEM_ENDPOINT_NAME "system"

int cpc_system_drv_register(void);
void cpc_system_drv_unregister(void);

#endif
