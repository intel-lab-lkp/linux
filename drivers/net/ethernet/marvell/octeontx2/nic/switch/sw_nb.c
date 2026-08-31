// SPDX-License-Identifier: GPL-2.0
/* Marvell RVU switch driver
 *
 * Copyright (C) 2026 Marvell.
 *
 */
#include "sw_nb.h"

#if IS_ENABLED(CONFIG_OCTEONTX_SWITCH)

int otx2_sw_nb_unregister(void)
{
	return 0;
}

int otx2_sw_nb_register(void)
{
	return 0;
}

#endif
