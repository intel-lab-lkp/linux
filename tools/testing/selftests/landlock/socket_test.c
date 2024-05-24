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

TEST_F(protocol, rule_with_unknown_access)
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

	for (access = 1ULL << 63; access != ACCESS_LAST; access >>= 1) {
		protocol.allowed_access = access;
		EXPECT_EQ(-1,
			  landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
					    &protocol, 0));
		EXPECT_EQ(EINVAL, errno);
	}
	EXPECT_EQ(0, close(ruleset_fd));
}

TEST_F(protocol, rule_with_unhandled_access)
{
	struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = LANDLOCK_ACCESS_SOCKET_CREATE,
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

	for (access = 1; access > 0; access <<= 1) {
		int err;

		protocol.allowed_access = access;
		err = landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
					&protocol, 0);
		if (access == ruleset_attr.handled_access_socket) {
			EXPECT_EQ(0, err);
		} else {
			EXPECT_EQ(-1, err);
			EXPECT_EQ(EINVAL, errno);
		}
	}

	EXPECT_EQ(0, close(ruleset_fd));
}

TEST_F(protocol, inval)
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = LANDLOCK_ACCESS_SOCKET_CREATE
	};

	struct landlock_socket_attr protocol = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = self->srv0.protocol.family,
		.type = self->srv0.protocol.type,
	};

	struct landlock_socket_attr protocol_denied = {
		.allowed_access = 0,
		.family = self->srv0.protocol.family,
		.type = self->srv0.protocol.type,
	};

	int ruleset_fd;

	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);

	/* Checks zero access value. */
	EXPECT_EQ(-1, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
					&protocol_denied, 0));
	EXPECT_EQ(ENOMSG, errno);

	/* Adds with legitimate values. */
	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
				       &protocol, 0));
}

FIXTURE(tcp_layers)
{
	struct service_fixture srv0;
};

FIXTURE_VARIANT(tcp_layers)
{
	const size_t num_layers;
};

FIXTURE_SETUP(tcp_layers)
{
	const struct protocol_variant prot = {
		.family = AF_INET,
		.type = SOCK_STREAM,
	};

	disable_caps(_metadata);
	self->srv0.protocol = prot;
	setup_namespace(_metadata);
};

FIXTURE_TEARDOWN(tcp_layers)
{
}

/* clang-format off */
FIXTURE_VARIANT_ADD(tcp_layers, no_sandbox_with_ipv4) {
	/* clang-format on */
	.num_layers = 0,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(tcp_layers, one_sandbox_with_ipv4) {
	/* clang-format on */
	.num_layers = 1,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(tcp_layers, two_sandboxes_with_ipv4) {
	/* clang-format on */
	.num_layers = 2,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(tcp_layers, three_sandboxes_with_ipv4) {
	/* clang-format on */
	.num_layers = 3,
};

TEST_F(tcp_layers, ruleset_overlap)
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = LANDLOCK_ACCESS_SOCKET_CREATE,
	};
	const struct landlock_socket_attr tcp_create = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = self->srv0.protocol.family,
		.type = self->srv0.protocol.type,
	};

	if (variant->num_layers >= 1) {
		int ruleset_fd;

		ruleset_fd = landlock_create_ruleset(&ruleset_attr,
						     sizeof(ruleset_attr), 0);
		ASSERT_LE(0, ruleset_fd);

		/* Allows create. */
		ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
					       &tcp_create, 0));
		enforce_ruleset(_metadata, ruleset_fd);
		EXPECT_EQ(0, close(ruleset_fd));
	}

	if (variant->num_layers >= 2) {
		int ruleset_fd;

		/* Creates another ruleset layer with denied create. */
		ruleset_fd = landlock_create_ruleset(&ruleset_attr,
						     sizeof(ruleset_attr), 0);
		ASSERT_LE(0, ruleset_fd);

		enforce_ruleset(_metadata, ruleset_fd);
		EXPECT_EQ(0, close(ruleset_fd));
	}

	if (variant->num_layers >= 3) {
		int ruleset_fd;

		/* Creates another ruleset layer. */
		ruleset_fd = landlock_create_ruleset(&ruleset_attr,
						     sizeof(ruleset_attr), 0);
		ASSERT_LE(0, ruleset_fd);

		/* Try to allow create second time. */
		ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
					       &tcp_create, 0));
		enforce_ruleset(_metadata, ruleset_fd);
		EXPECT_EQ(0, close(ruleset_fd));
	}

	if (variant->num_layers < 2) {
		ASSERT_EQ(0, test_socket(&self->srv0));
	} else {
		ASSERT_EQ(EACCES, test_socket(&self->srv0));
	}
}

TEST_HARNESS_MAIN
