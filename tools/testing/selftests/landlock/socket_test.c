// SPDX-License-Identifier: GPL-2.0-only
/*
 * Landlock tests - Socket
 *
 * Copyright © 2025 Huawei Tech. Co., Ltd.
 */

#define _GNU_SOURCE

#include <linux/landlock.h>
#include <sys/prctl.h>

#include "common.h"

#define ACCESS_LAST LANDLOCK_ACCESS_SOCKET_CREATE
#define ACCESS_ALL LANDLOCK_ACCESS_SOCKET_CREATE

/* clang-format off */
FIXTURE(mini) {};
/* clang-format on */

FIXTURE_SETUP(mini)
{
	disable_caps(_metadata);
};

FIXTURE_TEARDOWN(mini)
{
}

TEST_F(mini, ruleset_with_unknown_access)
{
	__u64 access_mask;

	for (access_mask = 1ULL << 63; access_mask != ACCESS_LAST;
	     access_mask >>= 1) {
		const struct landlock_ruleset_attr ruleset_attr = {
			.handled_access_socket = access_mask,
		};

		ASSERT_EQ(-1, landlock_create_ruleset(&ruleset_attr,
						      sizeof(ruleset_attr), 0));
		ASSERT_EQ(EINVAL, errno);
	}
}

TEST_HARNESS_MAIN
