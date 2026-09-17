// SPDX-License-Identifier: GPL-2.0
/*
 * Second translation unit of the lineinfo KUnit test module.
 *
 * Copyright (c) 2026 Sasha Levin <sashal@kernel.org>
 *
 * A module built from one object file cannot tell whether per-compile-unit
 * DWARF references were relocated: its only compile unit sits at offset 0,
 * which is exactly what an unrelocated reference reads.  Keeping a target
 * function in a second object makes the test cover the general case.
 */

#include <linux/compiler.h>

#include "lineinfo_kunit.h"

noinline int lineinfo_target_second_file(int x)
{
	barrier();
	return x + 1;
}
