/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * KUnit test for the Cirrus Logic CS2600 clock driver.
 *
 * Copyright (C) 2026 Cirrus Logic, Inc. and
 *                    Cirrus Logic International Semiconductor Ltd.
 */

#define CS2600_KUNIT_PLLOUT_RENAME	"renamed_pll"
#define CS2600_KUNIT_CLKOUT_RENAME	"renamed_clk"
#define CS2600_KUNIT_BCLK_RENAME	"renamed_bs"
#define CS2600_KUNIT_FSYNC_RENAME	"renamed_fs"

#define CS2600_KUNIT_CLKOUT_ASSIGNED	6144000
#define CS2600_KUNIT_BCLK_ASSIGNED	1536000
#define CS2600_KUNIT_FSYNC_ASSIGNED	96000
#define CS2600_KUNIT_BCLK_DIV_INDEX	3
#define CS2600_KUNIT_FSYNC_DIV_INDEX	2
