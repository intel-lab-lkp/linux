/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/types.h>

struct primes {
	struct rcu_head rcu;
	unsigned long last, sz;
	unsigned long primes[];
};

#ifdef CONFIG_PRIME_NUMBERS_KUNIT_TEST
bool slow_is_prime_number(unsigned long x);
#endif
typedef void (*primes_fn)(void *, const struct primes *);

// Calls the callback under RCU lock. The callback must not retain the primes pointer.
void with_primes(void *ctx, primes_fn fn);
