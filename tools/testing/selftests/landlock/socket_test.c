// SPDX-License-Identifier: GPL-2.0-only
/*
 * Landlock tests - Socket
 *
 * Copyright © 2025 Huawei Tech. Co., Ltd.
 */

#define _GNU_SOURCE

#include <linux/landlock.h>
#include <sys/prctl.h>
#include <linux/pfkeyv2.h>
#include <linux/kcm.h>
#include <linux/can.h>

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

TEST_F(mini, rule_with_supported_access)
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = ACCESS_ALL,
	};
	struct landlock_socket_attr protocol = {
		.family = AF_INET,
		.type = SOCK_STREAM,
		.protocol = 0,
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
	ASSERT_EQ(0, close(ruleset_fd));
}

TEST_F(mini, rule_with_unknown_access)
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = ACCESS_ALL,
	};
	struct landlock_socket_attr protocol = { .family = AF_INET,
						 .type = SOCK_STREAM,
						 .protocol = 0 };
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
	ASSERT_EQ(0, close(ruleset_fd));
}

TEST_F(mini, rule_with_unhandled_access)
{
	/* Prepares ruleset that handles network access instead of socket access. */
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_net = LANDLOCK_ACCESS_NET_BIND_TCP,
	};
	struct landlock_socket_attr protocol = { .family = AF_UNIX,
						 .type = SOCK_STREAM,
						 .protocol = 0 };
	int ruleset_fd;
	__u64 access;

	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);

	for (access = ACCESS_LAST; access > 0; access <<= 1) {
		int err;

		protocol.allowed_access = access;
		err = landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
					&protocol, 0);
		EXPECT_EQ(-1, err);
		EXPECT_EQ(EINVAL, errno);
	}

	ASSERT_EQ(0, close(ruleset_fd));
}

TEST_F(mini, rule_with_empty_access)
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = LANDLOCK_ACCESS_SOCKET_CREATE
	};
	const struct landlock_socket_attr protocol_denied = {
		.allowed_access = 0,
		.family = AF_UNIX,
		.type = SOCK_STREAM,
		.protocol = 0,
	};
	int ruleset_fd;

	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);

	/* Checks zero access value. */
	EXPECT_EQ(-1, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
					&protocol_denied, 0));
	EXPECT_EQ(ENOMSG, errno);
	ASSERT_EQ(0, close(ruleset_fd));
}

/* Validates landlock behaviour when using wildcard (-1) values in socket rules. */
TEST_F(mini, rule_with_wildcard)
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = LANDLOCK_ACCESS_SOCKET_CREATE,
	};
	const struct landlock_socket_attr create_family_attr = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = AF_INET,
		.type = -1,
		.protocol = -1,
	};
	const struct landlock_socket_attr create_family_type_attr = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = AF_INET,
		.type = SOCK_STREAM,
		.protocol = -1,
	};
	const struct landlock_socket_attr create_family_protocol_attr = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = AF_INET,
		.type = -1,
		.protocol = 0,
	};
	int ruleset_fd;

	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);

	EXPECT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
				       &create_family_attr, 0));
	EXPECT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
				       &create_family_type_attr, 0));
	EXPECT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
				       &create_family_protocol_attr, 0));
	ASSERT_EQ(0, close(ruleset_fd));
}

/* clang-format off */
FIXTURE(prot_inside_range) {};
/* clang-format on */

FIXTURE_VARIANT(prot_inside_range)
{
	int family, type, protocol;
};

FIXTURE_SETUP(prot_inside_range)
{
	disable_caps(_metadata);
};

FIXTURE_TEARDOWN(prot_inside_range)
{
}

/* clang-format off */
FIXTURE_VARIANT_ADD(prot_inside_range, family_upper) {
	/* clang-format on */
	.family = UINT8_MAX - 1,
	.type = SOCK_STREAM,
	.protocol = 0,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(prot_inside_range, type_upper) {
	/* clang-format on */
	.family = AF_INET,
	.type = UINT8_MAX - 1,
	.protocol = 0,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(prot_inside_range, protocol_upper) {
	/* clang-format on */
	.family = AF_INET,
	.type = SOCK_STREAM,
	.protocol = UINT16_MAX - 1,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(prot_inside_range, family_lower) {
	/* clang-format on */
	.family = 0,
	.type = SOCK_STREAM,
	.protocol = 0,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(prot_inside_range, type_lower) {
	/* clang-format on */
	.family = AF_INET,
	.type = 0,
	.protocol = 0,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(prot_inside_range, protocol_lower) {
	/* clang-format on */
	.family = AF_INET,
	.type = SOCK_STREAM,
	.protocol = 0,
};

/*
 * Verifies acceptable range of family, type and protocol values. Specific
 * case of adding rule with masked fields checked in "rule_with_wildcard"
 * test.
 *
 * Acceptable ranges are [0, UINT8_MAX) for family and type,
 * [0, UINT16_MAX) for protocol field.
 */
TEST_F(prot_inside_range, add_rule)
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = LANDLOCK_ACCESS_SOCKET_CREATE,
	};
	const struct landlock_socket_attr create_socket_attr = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = variant->family,
		.type = variant->type,
		.protocol = variant->protocol,
	};
	int ruleset_fd;

	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);

	EXPECT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
				       &create_socket_attr, 0));
	ASSERT_EQ(0, close(ruleset_fd));
}

/* clang-format off */
FIXTURE(prot_outside_range) {};
/* clang-format on */

FIXTURE_VARIANT(prot_outside_range)
{
	int family, type, protocol;
};

FIXTURE_SETUP(prot_outside_range)
{
	disable_caps(_metadata);
};

FIXTURE_TEARDOWN(prot_outside_range)
{
}

/* clang-format off */
FIXTURE_VARIANT_ADD(prot_outside_range, family_upper) {
	/* clang-format on */
	.family = UINT8_MAX,
	.type = SOCK_STREAM,
	.protocol = 0,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(prot_outside_range, type_upper) {
	/* clang-format on */
	.family = AF_INET,
	.type = UINT8_MAX,
	.protocol = 0,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(prot_outside_range, protocol_upper) {
	/* clang-format on */
	.family = AF_INET,
	.type = SOCK_STREAM,
	.protocol = UINT16_MAX,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(prot_outside_range, family_lower) {
	/* clang-format on */
	.family = -1,
	.type = SOCK_STREAM,
	.protocol = 0,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(prot_outside_range, type_lower) {
	/* clang-format on */
	.family = AF_INET,
	.type = -2,
	.protocol = 0,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(prot_outside_range, protocol_lower) {
	/* clang-format on */
	.family = AF_INET,
	.type = SOCK_STREAM,
	.protocol = -2,
};

/*
 * Acceptable ranges are [0, UINT8_MAX) for family and type,
 * [0, UINT16_MAX) for protocol field. Also type and protocol
 * can be set with specific -1 wildcard value.
 */
TEST_F(prot_outside_range, add_rule)
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = LANDLOCK_ACCESS_SOCKET_CREATE,
	};
	const struct landlock_socket_attr create_socket_attr = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = variant->family,
		.type = variant->type,
		.protocol = variant->protocol,
	};
	int ruleset_fd;

	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);

	EXPECT_EQ(-1, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
					&create_socket_attr, 0));
	ASSERT_EQ(0, close(ruleset_fd));
}

FIXTURE(protocol)
{
	struct protocol_variant prot;
	bool requires_caps;
};

FIXTURE_VARIANT(protocol)
{
	struct protocol_variant prot;
	bool requires_caps;
};

FIXTURE_SETUP(protocol)
{
	disable_caps(_metadata);

	self->prot = variant->prot;
	self->requires_caps = variant->requires_caps;
};

FIXTURE_TEARDOWN(protocol)
{
}

#define _PROTOCOL_VARIANT_ADD(family_, type_, protocol_, caps_)          \
	FIXTURE_VARIANT_ADD(protocol, family_##_##type_##_##protocol_)   \
	{                                                                \
		.prot = {                                              \
			.domain = AF_##family_,                             \
			.type = SOCK_##type_,                                 \
			.protocol = protocol_,                         \
		},                                                     \
		.requires_caps = caps_, \
	}

#define PROTOCOL_VARIANT_ADD(family, type, protocol) \
	_PROTOCOL_VARIANT_ADD(family, type, protocol, false)

#define PROTOCOL_VARIANT_ADD_CAPS(family, type, protocol) \
	_PROTOCOL_VARIANT_ADD(family, type, protocol, true)

#include "protocols_define.h"

#undef _PROTOCOL_VARIANT_ADD
#undef PROTOCOL_VARIANT_ADD
#undef PROTOCOL_VARIANT_ADD_CAPS

static int test_socket(int family, int type, int protocol)
{
	int fd;

	fd = socket(family, type | SOCK_CLOEXEC, protocol);
	if (fd < 0)
		return errno;
	/*
	 * Mixing error codes from close(2) and socket(2) should not lead to
	 * any (access type) confusion for this tests.
	 */
	if (close(fd) != 0)
		return errno;
	return 0;
}

static int test_socket_variant(struct __test_metadata *const _metadata,
			       const struct protocol_variant *const prot,
			       bool requires_caps)
{
	int err;

	if (requires_caps) {
		set_cap(_metadata, CAP_NET_RAW);
		set_cap(_metadata, CAP_SYS_ADMIN);
		set_cap(_metadata, CAP_NET_ADMIN);
	}

	err = test_socket(prot->domain, prot->type, prot->protocol);

	if (requires_caps) {
		clear_cap(_metadata, CAP_NET_RAW);
		clear_cap(_metadata, CAP_SYS_ADMIN);
		clear_cap(_metadata, CAP_NET_ADMIN);
	}

	return err;
}

TEST_F(protocol, restrict_socket)
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = LANDLOCK_ACCESS_SOCKET_CREATE,
	};
	int ruleset_fd;
	const struct landlock_socket_attr create_socket_attr = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = self->prot.domain,
		.type = self->prot.type,
		.protocol = self->prot.protocol,
	};

	/* Verifies default socket creation. */
	ASSERT_EQ(0, test_socket_variant(_metadata, &self->prot,
					 self->requires_caps));

	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);

	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
				       &create_socket_attr, 0));
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Tries to create socket when protocol is allowed. */
	EXPECT_EQ(0, test_socket_variant(_metadata, &self->prot,
					 self->requires_caps));

	/* Denies creation. */
	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);

	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Tries to create a socket when protocol is restricted. */
	EXPECT_EQ(EACCES, test_socket_variant(_metadata, &self->prot,
					      self->requires_caps));
}

/*
 * Errors related to AF internal validation of supported protocol attributes
 * are not consistent in sandboxed mode.
 */
TEST_F(mini, unsupported_af_and_prot)
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = LANDLOCK_ACCESS_SOCKET_CREATE,
	};
	const struct landlock_socket_attr socket_af_unsupported = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = AF_UNSPEC, /* cf __sock_create */
		.type = SOCK_STREAM,
		.protocol = 0,
	};
	const struct landlock_socket_attr socket_type_unsupported = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = AF_UNIX,
		.type = SOCK_PACKET, /* cf. unix_create */
		.protocol = 0,
	};
	const struct landlock_socket_attr socket_proto_unsupported = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = AF_UNIX,
		.type = SOCK_STREAM,
		.protocol = PF_UNIX + 1, /* cf. unix_create */
	};
	int ruleset_fd;

	/* Tries to create a socket when ruleset is not established. */
	ASSERT_EQ(EAFNOSUPPORT, test_socket(AF_UNSPEC, SOCK_STREAM, 0));
	ASSERT_EQ(ESOCKTNOSUPPORT, test_socket(AF_UNIX, SOCK_PACKET, 0));
	ASSERT_EQ(EPROTONOSUPPORT,
		  test_socket(AF_UNIX, SOCK_STREAM, PF_UNIX + 1));

	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);

	/* Landlock allows creating rules for meaningless protocols. */
	EXPECT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
				       &socket_af_unsupported, 0));
	EXPECT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
				       &socket_type_unsupported, 0));
	EXPECT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
				       &socket_proto_unsupported, 0));
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Tries to create a socket when protocols are allowed. */
	EXPECT_EQ(EAFNOSUPPORT, test_socket(AF_UNSPEC, SOCK_STREAM, 0));
	EXPECT_EQ(ESOCKTNOSUPPORT, test_socket(AF_UNIX, SOCK_PACKET, 0));
	EXPECT_EQ(EPROTONOSUPPORT,
		  test_socket(AF_UNIX, SOCK_STREAM, PF_UNIX + 1));

	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Tries to create a socket when protocols are restricted. */
	EXPECT_EQ(EACCES, test_socket(AF_UNSPEC, SOCK_STREAM, 0));
	EXPECT_EQ(EACCES, test_socket(AF_UNIX, SOCK_PACKET, 0));
	EXPECT_EQ(EACCES, test_socket(AF_UNIX, SOCK_STREAM, PF_UNIX + 1));
}

TEST_HARNESS_MAIN
