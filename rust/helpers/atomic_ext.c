// SPDX-License-Identifier: GPL-2.0

#include <asm/barrier.h>
#include <asm/rwonce.h>
#include <linux/atomic.h>

__rust_helper s8 rust_helper_atomic_i8_load(s8 *ptr)
{
	return READ_ONCE(*ptr);
}

__rust_helper s8 rust_helper_atomic_i8_load_acquire(s8 *ptr)
{
	return smp_load_acquire(ptr);
}

__rust_helper s16 rust_helper_atomic_i16_load(s16 *ptr)
{
	return READ_ONCE(*ptr);
}

__rust_helper s16 rust_helper_atomic_i16_load_acquire(s16 *ptr)
{
	return smp_load_acquire(ptr);
}

__rust_helper void rust_helper_atomic_i8_store(s8 *ptr, s8 val)
{
	WRITE_ONCE(*ptr, val);
}

__rust_helper void rust_helper_atomic_i8_store_release(s8 *ptr, s8 val)
{
	smp_store_release(ptr, val);
}

__rust_helper void rust_helper_atomic_i16_store(s16 *ptr, s16 val)
{
	WRITE_ONCE(*ptr, val);
}

__rust_helper void rust_helper_atomic_i16_store_release(s16 *ptr, s16 val)
{
	smp_store_release(ptr, val);
}

__rust_helper s8 rust_helper_atomic_i8_xchg(s8 *ptr, s8 new)
{
	return raw_xchg(ptr, new);
}

__rust_helper s16 rust_helper_atomic_i16_xchg(s16 *ptr, s16 new)
{
	return raw_xchg(ptr, new);
}

__rust_helper s8 rust_helper_atomic_i8_xchg_acquire(s8 *ptr, s8 new)
{
	return raw_xchg_acquire(ptr, new);
}

__rust_helper s16 rust_helper_atomic_i16_xchg_acquire(s16 *ptr, s16 new)
{
	return raw_xchg_acquire(ptr, new);
}
