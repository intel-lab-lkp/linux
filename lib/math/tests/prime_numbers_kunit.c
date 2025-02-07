// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/module.h>
#include <linux/prime_numbers.h>
#include <linux/slab.h>

#include "../prime_numbers_private.h"

static void free_primes(struct kunit_suite *suite)
{
	const struct primes *p;

	mutex_lock(&lock);
	p = rcu_dereference_protected(primes, lockdep_is_held(&lock));
	if (p != &small_primes) {
		rcu_assign_pointer(primes, &small_primes);
		kfree_rcu((struct primes *)p, rcu);
	}
	mutex_unlock(&lock);
}

static void dump_primes(struct kunit *test)
{
	const struct primes *p;
	char *buf;

	buf = kmalloc(PAGE_SIZE, GFP_KERNEL);

	rcu_read_lock();
	p = rcu_dereference(primes);

	if (buf)
		bitmap_print_to_pagebuf(true, buf, p->primes, p->sz);
	kunit_info(test, "primes.{last=%lu, .sz=%lu, .primes[]=...x%lx} = %s\n",
		   p->last, p->sz, p->primes[BITS_TO_LONGS(p->sz) - 1], buf);

	rcu_read_unlock();

	kfree(buf);
}

static void prime_numbers_test(struct kunit *test)
{
	const unsigned long max = 65536;
	unsigned long x, last;

	for (last = 0, x = 2; x < max; x++) {
		bool slow = slow_is_prime_number(x);
		bool fast = is_prime_number(x);

		if (slow != fast) {
			KUNIT_FAIL(test,
				   "inconsistent result for is-prime(%lu): slow=%s, fast=%s!\n",
				   x, slow ? "yes" : "no", fast ? "yes" : "no");
			goto err;
		}

		if (!slow)
			continue;

		if (next_prime_number(last) != x) {
			KUNIT_FAIL(test,
				   "incorrect result for next-prime(%lu): expected %lu, got %lu\n",
				   last, x, next_prime_number(last));
			goto err;
		}
		last = x;
	}

	kunit_info(test, "%s(%lu) passed, last prime was %lu\n", __func__, x, last);

err:
	dump_primes(test);
}

static struct kunit_case prime_numbers_cases[] = {
	KUNIT_CASE(prime_numbers_test),
	{},
};

static struct kunit_suite prime_numbers_suite = {
	.name = "math-prime_numbers",
	.suite_exit = free_primes,
	.test_cases = prime_numbers_cases,
};

kunit_test_suite(prime_numbers_suite);

MODULE_AUTHOR("Intel Corporation");
MODULE_DESCRIPTION("Prime number library");
MODULE_LICENSE("GPL");
