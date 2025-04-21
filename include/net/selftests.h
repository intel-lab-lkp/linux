/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NET_SELFTESTS
#define _NET_SELFTESTS

#include <linux/ethtool.h>

#define NET_TEST_NETIF_CARRIER		BIT(0)
#define NET_TEST_FULL_DUPLEX		BIT(1)
#define NET_TEST_TCP			BIT(2)
#define NET_TEST_UDP			BIT(3)
#define NET_TEST_UDP_MAX_MTU		BIT(4)

#define NET_EXTRA_CARRIER_TEST		BIT(0)
#define NET_EXTRA_FULL_DUPLEX_TEST	BIT(1)
#define NET_EXTRA_PHY_TEST		BIT(2)

struct net_test_entry {
	char name[ETH_GSTRING_LEN];

	/* can set to NULL */
	int (*enable)(struct net_device *ndev, bool enable);

	/* can set to NULL */
	int (*fn)(struct net_device *ndev);

	/* if flag is set, fn() will be ignored,
	 * and will do test according to the flag,
	 * such as NET_TEST_UDP...
	 */
	unsigned long flags;
};

#define NET_TEST_E(_name, _enable, _flags) { \
	.name = _name, \
	.enable = _enable, \
	.fn = NULL, \
	.flags = _flags }

#define NET_TEST_ENTRY_MAX_COUNT	10
struct net_test {
	/* extra tests will be added based on this flag */
	unsigned long extra_flags;

	struct net_test_entry entries[NET_TEST_ENTRY_MAX_COUNT];
	/* the count of entries, must <= NET_TEST_ENTRY_MAX_COUNT */
	u32 count;
};

#if IS_ENABLED(CONFIG_NET_SELFTESTS)

void net_selftest(struct net_device *ndev, struct ethtool_test *etest,
		  u64 *buf);
int net_selftest_get_count(void);
void net_selftest_get_strings(u8 *data);

void net_selftest_custom(struct net_device *ndev, const struct net_test *test,
			 struct ethtool_test *etest, u64 *buf);
int net_selftest_get_count_custom(const struct net_test *test);
void net_selftest_get_strings_custom(const struct net_test *test, u8 *data);

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

void net_selftest_custom(struct net_device *ndev, struct net_test *test,
			 struct ethtool_test *etest, u64 *buf)
{
}

int net_selftest_get_count_custom(struct net_test *test)
{
	return 0;
}

void net_selftest_get_strings_custom(struct net_test *test, u8 *data)
{
}

#endif
#endif /* _NET_SELFTESTS */
