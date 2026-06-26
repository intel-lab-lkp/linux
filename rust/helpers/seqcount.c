// SPDX-License-Identifier: GPL-2.0

#include <linux/seqlock.h>

__rust_helper void rust_helper_write_seqcount_begin(seqcount_spinlock_t *s)
{
	write_seqcount_begin(s);
}

__rust_helper void rust_helper_write_seqcount_end(seqcount_spinlock_t *s)
{
	write_seqcount_end(s);
}

__rust_helper unsigned int rust_helper_read_seqcount_begin(const seqcount_spinlock_t *s)
{
	return read_seqcount_begin(s);
}

__rust_helper unsigned int rust_helper_read_seqcount_retry(const seqcount_spinlock_t *s,
							   unsigned int start)
{
	return read_seqcount_retry(s, start);
}

__rust_helper void rust_helper_seqcount_spinlock_init(
	seqcount_spinlock_t *s, spinlock_t *lock)
{
	seqcount_spinlock_init(s, lock);
}
