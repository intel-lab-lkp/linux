// SPDX-License-Identifier: GPL-2.0
/*
 *
 * APEI GHES CPER helper translation unit - staging file for helper moves
 *
 * Copyright (C) 2026 ARM Ltd.
 * Author: Ahmed Tiba <ahmed.tiba@arm.com>
 * Based on ACPI APEI GHES driver.
 *
 */

#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/ratelimit.h>
#include <linux/slab.h>

#include <acpi/apei.h>

#include <asm/fixmap.h>
#include <asm/tlbflush.h>

#include "apei-internal.h"

/* Helper bodies will be moved here in follow-up commits. */
