/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/bits.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/types.h>

struct primes {
	struct rcu_head rcu;
	unsigned long last, sz;
	unsigned long primes[];
};

#if BITS_PER_LONG == 64
static const struct primes small_primes = {
	.last = 61,
	.sz = 64,
	.primes = {
		BIT(2) |
		BIT(3) |
		BIT(5) |
		BIT(7) |
		BIT(11) |
		BIT(13) |
		BIT(17) |
		BIT(19) |
		BIT(23) |
		BIT(29) |
		BIT(31) |
		BIT(37) |
		BIT(41) |
		BIT(43) |
		BIT(47) |
		BIT(53) |
		BIT(59) |
		BIT(61)
	}
};
#elif BITS_PER_LONG == 32
static const struct primes small_primes = {
	.last = 31,
	.sz = 32,
	.primes = {
		BIT(2) |
		BIT(3) |
		BIT(5) |
		BIT(7) |
		BIT(11) |
		BIT(13) |
		BIT(17) |
		BIT(19) |
		BIT(23) |
		BIT(29) |
		BIT(31)
	}
};
#else
#error "unhandled BITS_PER_LONG"
#endif

static const struct primes __rcu *primes = RCU_INITIALIZER(&small_primes);
static DEFINE_MUTEX(lock);

bool slow_is_prime_number(unsigned long x);
