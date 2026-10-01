/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Entry points into the SGI L1 system controller driver.
 *
 * Copyright (C) 2026 Imre Kaloz <kaloz@kernel.org>
 */
#ifndef __ASM_SN_L1_H
#define __ASM_SN_L1_H

#include <linux/types.h>

/*
 * Which L1 EEPROM component id carries which board is undocumented and
 * differs from machine to machine, so a caller walks the whole range.
 */
#define L1_BOARD_COMPONENT_MAX	8

int l1_exec_command(const char *cmd);
int l1_read_board_partnum(unsigned int component, char *buf, size_t len);
int l1_read_brick_type(void);

#endif /* __ASM_SN_L1_H */
