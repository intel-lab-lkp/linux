// SPDX-License-Identifier: GPL-2.0-only

#include <linux/types.h>
#include <linux/mm.h>

#include "../../../../lib/vdso/getrandom.c"

ssize_t __kernel_getrandom(void *buffer, size_t len, unsigned int flags, void *opaque_state, size_t opaque_len);

ssize_t __kernel_getrandom(void *buffer, size_t len, unsigned int flags, void *opaque_state, size_t opaque_len)
{
	return __cvdso_getrandom(buffer, len, flags, opaque_state, opaque_len);
}
