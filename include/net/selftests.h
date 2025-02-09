/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NET_SELFTESTS
#define _NET_SELFTESTS

#include <linux/ethtool.h>

#if IS_ENABLED(CONFIG_NET_SELFTESTS)

int net_test_phy_loopback_udp(struct net_device *ndev);
int net_test_phy_loopback_udp_mtu(struct net_device *ndev);
int net_test_phy_loopback_tcp(struct net_device *ndev);

void net_selftest(struct net_device *ndev, struct ethtool_test *etest,
		  u64 *buf);
int net_selftest_get_count(void);
void net_selftest_get_strings(u8 *data);

#else

static inline int net_test_phy_loopback_udp(struct net_device *ndev)
{
	return 0;
}

static inline int net_test_phy_loopback_udp_mtu(struct net_device *ndev)
{
	return 0;
}

static inline int net_test_phy_loopback_tcp(struct net_device *ndev)
{
	return 0;
}

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

#endif
#endif /* _NET_SELFTESTS */
