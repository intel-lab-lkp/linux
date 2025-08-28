// SPDX-License-Identifier: GPL-2.0

#include <linux/percpu.h>

void __percpu *rust_helper_alloc_percpu(size_t sz, size_t align)
{
	return __alloc_percpu(sz, align);
}

