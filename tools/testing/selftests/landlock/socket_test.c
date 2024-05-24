// SPDX-License-Identifier: GPL-2.0-only
/*
 * Landlock tests - Socket
 *
 * Copyright © 2024 Huawei Tech. Co., Ltd.
 * Copyright © 2024 Microsoft Corporation
 */

#define _GNU_SOURCE

#include <errno.h>
#include <linux/net.h>
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

/* clang-format off */
FIXTURE(mini) {};
/* clang-format on */

FIXTURE_SETUP(mini)
{
	disable_caps(_metadata);

	setup_namespace(_metadata);
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

		EXPECT_EQ(-1, landlock_create_ruleset(&ruleset_attr,
						      sizeof(ruleset_attr), 0));
		EXPECT_EQ(EINVAL, errno);
	}
}

TEST_F(mini, socket_overflow)
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = LANDLOCK_ACCESS_SOCKET_CREATE,
	};
	/*
	 * Assuming that AF_MCTP == AF_MAX - 1 uses MCTP as protocol
	 * with maximum family value. Appropriate сheck for this is given below.
	 */
	const struct landlock_socket_attr create_socket_max_family = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = AF_MCTP,
		.type = SOCK_DGRAM,
	};
	/*
	 * Assuming that SOCK_PACKET == SOCK_MAX - 1 uses PACKET socket as
	 * socket with maximum type value. Since SOCK_MAX cannot be accessed
	 * from selftests, this assumption is not verified.
	 */
	const struct landlock_socket_attr create_socket_max_type = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = AF_PACKET,
		.type = SOCK_PACKET,
	};
	struct landlock_socket_attr create_socket_overflow = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
	};
	const struct protocol_variant protocol_max_family = {
		.family = create_socket_max_family.family,
		.type = create_socket_max_family.type,
	};
	const struct protocol_variant protocol_max_type = {
		.family = create_socket_max_type.family,
		.type = create_socket_max_type.type,
	};
	const struct protocol_variant ipv4_tcp = {
		.family = AF_INET,
		.type = SOCK_STREAM,
	};
	struct service_fixture srv_max_allowed_family, srv_max_allowed_type,
		srv_denied;
	int ruleset_fd;

	/* Checks protocol_max_family correctness. */
	ASSERT_EQ(AF_MCTP + 1, AF_MAX);

	srv_max_allowed_family.protocol = protocol_max_family;
	srv_max_allowed_type.protocol = protocol_max_type;
	srv_denied.protocol = ipv4_tcp;

	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);

	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
				       &create_socket_max_family, 0));
	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
				       &create_socket_max_type, 0));

	/* Checks the overflow variants for family, type values. */
#define CHECK_RULE_OVERFLOW(family_val, type_val)                             \
	do {                                                                  \
		create_socket_overflow.family = family_val;                   \
		create_socket_overflow.type = type_val;                       \
		EXPECT_EQ(-1,                                                 \
			  landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET, \
					    &create_socket_overflow, 0));     \
		EXPECT_EQ(EINVAL, errno);                                     \
	} while (0)

	CHECK_RULE_OVERFLOW(AF_MAX, SOCK_STREAM);
	CHECK_RULE_OVERFLOW(AF_INET, (SOCK_PACKET + 1));
	CHECK_RULE_OVERFLOW(AF_MAX, (SOCK_PACKET + 1));
	CHECK_RULE_OVERFLOW(-1, SOCK_STREAM);
	CHECK_RULE_OVERFLOW(AF_INET, -1);
	CHECK_RULE_OVERFLOW(-1, -1);
	CHECK_RULE_OVERFLOW(INT16_MAX + 1, INT16_MAX + 1);

#undef CHECK_RULE_OVERFLOW

	enforce_ruleset(_metadata, ruleset_fd);

	EXPECT_EQ(0, test_socket(&srv_max_allowed_family));

	/* PACKET sockets can be used only with CAP_NET_RAW. */
	set_cap(_metadata, CAP_NET_RAW);
	EXPECT_EQ(0, test_socket(&srv_max_allowed_type));
	clear_cap(_metadata, CAP_NET_RAW);

	EXPECT_EQ(EACCES, test_socket(&srv_denied));
}

TEST_F(mini, socket_invalid_type)
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = LANDLOCK_ACCESS_SOCKET_CREATE,
	};
	/*
	 * SOCK_PACKET is invalid type for UNIX socket
	 * (see net/unix/af_unix.c:unix_create()).
	 */
	const struct landlock_socket_attr create_unix_invalid = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = AF_UNIX,
		.type = SOCK_PACKET,
	};
	const struct protocol_variant protocol_invalid = {
		.family = create_unix_invalid.family,
		.type = create_unix_invalid.type,
	};
	struct service_fixture srv_unix_invalid;
	int ruleset_fd;

	srv_unix_invalid.protocol = protocol_invalid;

	/* Allowed created */
	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);

	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
				       &create_unix_invalid, 0));
	enforce_ruleset(_metadata, ruleset_fd);
	EXPECT_EQ(0, close(ruleset_fd));

	EXPECT_EQ(ESOCKTNOSUPPORT, test_socket(&srv_unix_invalid));

	/* Denied create */
	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);

	enforce_ruleset(_metadata, ruleset_fd);
	EXPECT_EQ(0, close(ruleset_fd));

	EXPECT_EQ(ESOCKTNOSUPPORT, test_socket(&srv_unix_invalid));
}

TEST_HARNESS_MAIN
