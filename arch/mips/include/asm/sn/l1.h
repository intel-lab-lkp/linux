/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Entry points into the SGI L1 system controller driver.
 *
 * Copyright (C) 2026 Imre Kaloz <kaloz@kernel.org>
 */
#ifndef __ASM_SN_L1_H
#define __ASM_SN_L1_H

#include <linux/types.h>

int l1_exec_command(const char *cmd);

#endif /* __ASM_SN_L1_H */
