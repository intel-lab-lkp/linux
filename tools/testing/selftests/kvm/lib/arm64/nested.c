// SPDX-License-Identifier: GPL-2.0
/*
 * ARM64 Nested virtualization helpers
 */

#include "nested.h"
#include "test_util.h"

void __hyp_exception(u64 type)
{
	GUEST_FAIL("Unexpected hyp exception! type: %lx\n", type);
}
