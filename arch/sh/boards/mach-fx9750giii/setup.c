// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Artem Novak
/*
 * Casio fx-9750GIII platform support
 *
 */

#include <linux/init.h>

#include <asm/machvec.h>

static struct sh_machine_vector mv_fx9750giii __initmv = {
	.mv_name	= "Casio fx-9750GIII",
};
