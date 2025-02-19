/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NET_SELFTESTS
#define _NET_SELFTESTS

#include <linux/ethtool.h>

/**
 * enum net_selftest_set - selftest set ID
 * @NET_SELFTEST_CARRIER: Loopback tests based on carrier speed
 * @NET_SELFTEST_100: Loopback tests with 100 Mbps
 * @NET_SELFTEST_1000: Loopback tests with 1000 Mbps
 */
enum net_selftest_set {
	NET_TEST_LOOPBACK_CARRIER = 0,
	NET_TEST_LOOPBACK_100,
	NET_TEST_LOOPBACK_1000,
};

#if IS_ENABLED(CONFIG_NET_SELFTESTS)

void net_selftest(struct net_device *ndev, struct ethtool_test *etest,
		  u64 *buf);
int net_selftest_get_count(void);
void net_selftest_get_strings(u8 *data);

void net_selftest_set(int set, struct net_device *ndev,
		      struct ethtool_test *etest, u64 *buf);
int net_selftest_set_get_count(int set);
void net_selftest_set_get_strings(int set, u8 **data);

#else

static inline void net_selftest(struct net_device *ndev, struct ethtool_test *etest,
				u64 *buf)
{
}

static inline int net_selftest_get_count(void)
{
	return 0;
}

static inline void net_selftest_get_strings(u8 *data)
{
}

static inline void net_selftest_set(int set, struct net_device *ndev,
				    struct ethtool_test *etest, u64 *buf)
{
}

static inline int net_selftest_set_get_count(int set, void)
{
	return 0;
}

static inline void net_selftest_set_get_strings(int set, u8 *data)
{
}

#endif
#endif /* _NET_SELFTESTS */
