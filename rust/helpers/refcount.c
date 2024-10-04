// SPDX-License-Identifier: GPL-2.0

#include <linux/export.h>
#include <linux/refcount.h>

refcount_t rust_helper_REFCOUNT_INIT(int n)
{
	return (refcount_t)REFCOUNT_INIT(n);
}

unsigned int rust_helper_refcount_read(refcount_t *r)
{
	return refcount_read(r);
}

void rust_helper_refcount_set(refcount_t *r, int n)
{
	refcount_set(r, n);
}

void rust_helper_refcount_inc(refcount_t *r)
{
	refcount_inc(r);
}

void rust_helper_refcount_dec(refcount_t *r)
{
	refcount_dec(r);
}

bool rust_helper_refcount_dec_and_test(refcount_t *r)
{
	return refcount_dec_and_test(r);
}
