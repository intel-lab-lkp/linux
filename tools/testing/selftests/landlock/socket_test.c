// SPDX-License-Identifier: GPL-2.0-only
/*
 * Landlock tests - Socket
 *
 * Copyright © 2024 Huawei Tech. Co., Ltd.
 * Copyright © 2024 Microsoft Corporation
 */

#define _GNU_SOURCE

#include <errno.h>
#include <linux/landlock.h>
#include <sched.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>

#include "common.h"

/* clang-format off */

#define ACCESS_LAST LANDLOCK_ACCESS_SOCKET_CREATE

#define ACCESS_ALL ( \
	LANDLOCK_ACCESS_SOCKET_CREATE)

/* clang-format on */

struct protocol_variant {
	int family;
	int type;
};

struct service_fixture {
	struct protocol_variant protocol;
};

static void setup_namespace(struct __test_metadata *const _metadata)
{
	set_cap(_metadata, CAP_SYS_ADMIN);
	ASSERT_EQ(0, unshare(CLONE_NEWNET));
	clear_cap(_metadata, CAP_SYS_ADMIN);
}

static int test_socket(const struct service_fixture *const srv)
{
	int fd;

	fd = socket(srv->protocol.family, srv->protocol.type | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return errno;
	/*
	 * Mixing error codes from close(2) and socket(2) should not lead to any
	 * (access type) confusion for this test.
	 */
	if (close(fd) != 0)
		return errno;
	return 0;
}

FIXTURE(protocol)
{
	struct service_fixture srv0, unspec_srv0;
};

FIXTURE_VARIANT(protocol)
{
	const struct protocol_variant protocol;
};

FIXTURE_SETUP(protocol)
{
	const struct protocol_variant prot_unspec = {
		.family = AF_UNSPEC,
		.type = SOCK_STREAM,
	};

	disable_caps(_metadata);
	self->srv0.protocol = variant->protocol;
	self->unspec_srv0.protocol = prot_unspec;
	setup_namespace(_metadata);
};

FIXTURE_TEARDOWN(protocol)
{
}

/* clang-format off */
FIXTURE_VARIANT_ADD(protocol, unix_stream) {
	/* clang-format on */
	.protocol = {
		.family = AF_UNIX,
		.type = SOCK_STREAM,
	},
};

/* clang-format off */
FIXTURE_VARIANT_ADD(protocol, unix_dgram) {
	/* clang-format on */
	.protocol = {
		.family = AF_UNIX,
		.type = SOCK_DGRAM,
	},
};

/* clang-format off */
FIXTURE_VARIANT_ADD(protocol, ipv4_tcp) {
	/* clang-format on */
	.protocol = {
		.family = AF_INET,
		.type = SOCK_STREAM,
	},
};

/* clang-format off */
FIXTURE_VARIANT_ADD(protocol, ipv4_udp) {
	/* clang-format on */
	.protocol = {
		.family = AF_INET,
		.type = SOCK_DGRAM,
	},
};

/* clang-format off */
FIXTURE_VARIANT_ADD(protocol, ipv6_tcp) {
	/* clang-format on */
	.protocol = {
		.family = AF_INET6,
		.type = SOCK_STREAM,
	},
};

/* clang-format off */
FIXTURE_VARIANT_ADD(protocol, ipv6_udp) {
	/* clang-format on */
	.protocol = {
		.family = AF_INET6,
		.type = SOCK_DGRAM,
	},
};

TEST_F(protocol, create)
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = LANDLOCK_ACCESS_SOCKET_CREATE,
	};
	const struct landlock_socket_attr create_socket_attr = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = self->srv0.protocol.family,
		.type = self->srv0.protocol.type,
	};

	int ruleset_fd;

	/* Allowed create */
	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);

	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
				       &create_socket_attr, 0));

	enforce_ruleset(_metadata, ruleset_fd);
	EXPECT_EQ(0, close(ruleset_fd));

	ASSERT_EQ(0, test_socket(&self->srv0));
	ASSERT_EQ(EAFNOSUPPORT, test_socket(&self->unspec_srv0));

	/* Denied create */
	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);

	enforce_ruleset(_metadata, ruleset_fd);
	EXPECT_EQ(0, close(ruleset_fd));

	ASSERT_EQ(EACCES, test_socket(&self->srv0));
	ASSERT_EQ(EAFNOSUPPORT, test_socket(&self->unspec_srv0));
}

TEST_F(protocol, socket_access_rights)
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = ACCESS_ALL,
	};
	struct landlock_socket_attr protocol = {
		.family = self->srv0.protocol.family,
		.type = self->srv0.protocol.type,
	};
	int ruleset_fd;
	__u64 access;

	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);

	for (access = 1; access <= ACCESS_LAST; access <<= 1) {
		protocol.allowed_access = access;
		EXPECT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
					       &protocol, 0))
		{
			TH_LOG("Failed to add rule with access 0x%llx: %s",
			       access, strerror(errno));
		}
	}
	EXPECT_EQ(0, close(ruleset_fd));
}

TEST_HARNESS_MAIN
