// SPDX-License-Identifier: GPL-2.0
/*
 * Powerpc userspace implementations of getrandom()
 */
#include <linux/time.h>
#include <linux/types.h>

ssize_t __c_kernel_getrandom(void *buffer, size_t len, unsigned int flags, void *opaque_state,
			     size_t opaque_len, const struct vdso_rng_data *vd)
{
       return __cvdso_getrandom_data(vd, buffer, len, flags, opaque_state, opaque_len);
}
