// SPDX-License-Identifier: GPL-2.0

#include <asm/barrier.h>

s8 rust_helper_atomic_i8_load_acquire(s8 *ptr)
{
	return smp_load_acquire(ptr);
}

s16 rust_helper_atomic_i16_load_acquire(s16 *ptr)
{
	return smp_load_acquire(ptr);
}

void rust_helper_atomic_i8_store_release(s8 *ptr, s8 val)
{
	smp_store_release(ptr, val);
}

void rust_helper_atomic_i16_store_release(s16 *ptr, s16 val)
{
	smp_store_release(ptr, val);
}
