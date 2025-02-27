/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NET_SELFTESTS
#define _NET_SELFTESTS

#include <linux/ethtool.h>

/**
 * enum net_selftest - selftest set ID
 * @NET_SELFTEST_LOOPBACK_CARRIER: Loopback tests based on carrier speed
 */
enum net_selftest {
	NET_SELFTEST_LOOPBACK_CARRIER = 0,
};

#if IS_ENABLED(CONFIG_NET_SELFTESTS)

void net_selftest(struct net_device *ndev, struct ethtool_test *etest,
		  u64 *buf);
int net_selftest_get_count(void);
void net_selftest_get_strings(u8 *data);

void net_selftest_set(int set, int speed, struct net_device *ndev,
		      struct ethtool_test *etest, u64 *buf);
int net_selftest_set_get_count(int set);
void net_selftest_set_get_strings(int set, int speed, u8 **data);

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

static inline void net_selftest_set(int set, int speed, struct net_device *ndev,
				    struct ethtool_test *etest, u64 *buf)
{
}

static inline int net_selftest_set_get_count(int set)
{
	return 0;
}

static inline void net_selftest_set_get_strings(int set, int speed, u8 **data)
{
}

#endif
#endif /* _NET_SELFTESTS */
