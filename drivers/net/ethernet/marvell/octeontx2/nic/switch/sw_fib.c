// SPDX-License-Identifier: GPL-2.0
/* Marvell RVU switch driver
 *
 * Copyright (C) 2026 Marvell.
 *
 */
#include "sw_fib.h"

#if IS_ENABLED(CONFIG_OCTEONTX_SWITCH)

int otx2_sw_fib_init(void)
{
	return 0;
}

void otx2_sw_fib_deinit(void)
{
}

#endif
