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
#include <sys/socket.h>
#include <stdint.h>
#include <linux/sctp.h>
#include <arpa/inet.h>

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

static void add_ruleset_layer(struct __test_metadata *const _metadata,
			      const struct landlock_socket_attr *socket_attr)
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = LANDLOCK_ACCESS_SOCKET_CREATE,
	};
	int ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);

	if (socket_attr) {
		ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
					       socket_attr, 0));
	}

	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));
}

TEST_F(mini, ruleset_overlap)
{
	const struct landlock_socket_attr create_socket_attr = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = AF_INET,
		.type = SOCK_STREAM,
		.protocol = 0,
	};

	/* socket(2) is allowed if there are no restrictions. */
	ASSERT_EQ(0, test_socket(AF_INET, SOCK_STREAM, 0));

	/* Creates ruleset with socket(2) allowed. */
	add_ruleset_layer(_metadata, &create_socket_attr);
	EXPECT_EQ(0, test_socket(AF_INET, SOCK_STREAM, 0));

	/* Adds ruleset layer with socket(2) restricted. */
	add_ruleset_layer(_metadata, NULL);
	EXPECT_EQ(EACCES, test_socket(AF_INET, SOCK_STREAM, 0));

	/*
	 * Adds ruleset layer with socket(2) allowed. socket(2) is restricted
	 * by second layer of the ruleset.
	 */
	add_ruleset_layer(_metadata, &create_socket_attr);
	EXPECT_EQ(EACCES, test_socket(AF_INET, SOCK_STREAM, 0));
}

TEST_F(mini, ruleset_with_wildcards_overlap)
{
	const struct landlock_socket_attr create_socket_attr = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = AF_INET,
		.type = (-1),
		.protocol = (-1),
	};

	/* socket(2) is allowed if there are no restrictions. */
	ASSERT_EQ(0, test_socket(AF_INET, SOCK_STREAM, 0));
	ASSERT_EQ(0, test_socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP));
	ASSERT_EQ(0, test_socket(AF_INET, SOCK_DGRAM, 0));

	/* Creates ruleset with AF_INET allowed. */
	add_ruleset_layer(_metadata, &create_socket_attr);
	EXPECT_EQ(0, test_socket(AF_INET, SOCK_STREAM, 0));
	EXPECT_EQ(0, test_socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP));
	EXPECT_EQ(0, test_socket(AF_INET, SOCK_DGRAM, 0));

	const struct landlock_socket_attr create_socket_attr2 = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = AF_INET,
		.type = SOCK_STREAM,
		.protocol = (-1),
	};
	/* Creates layer with AF_INET + SOCK_STREAM allowed. */
	add_ruleset_layer(_metadata, &create_socket_attr2);
	EXPECT_EQ(0, test_socket(AF_INET, SOCK_STREAM, 0));
	EXPECT_EQ(0, test_socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP));
	EXPECT_EQ(EACCES, test_socket(AF_INET, SOCK_DGRAM, 0));

	const struct landlock_socket_attr create_socket_attr3 = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = AF_INET,
		.type = SOCK_STREAM,
		.protocol = 0,
	};
	/* Creates layer with AF_INET + SOCK_STREAM + 0 allowed. */
	add_ruleset_layer(_metadata, &create_socket_attr3);
	EXPECT_EQ(0, test_socket(AF_INET, SOCK_STREAM, 0));
	EXPECT_EQ(EACCES, test_socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP));
	EXPECT_EQ(EACCES, test_socket(AF_INET, SOCK_DGRAM, 0));
}

/* mini.kernel_socket will fail with EAFNOSUPPORT if SMC is not supported. */
TEST_F(mini, kernel_socket)
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = LANDLOCK_ACCESS_SOCKET_CREATE,
	};
	const struct landlock_socket_attr smc_socket_create = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = AF_SMC,
		.type = SOCK_STREAM,
		.protocol = 0,
	};
	int ruleset_fd;

	/*
	 * Checks that SMC socket is created successfully without
	 * landlock restrictions.
	 */
	ASSERT_EQ(0, test_socket(AF_SMC, SOCK_STREAM, 0));

	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);

	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
				       &smc_socket_create, 0));
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/*
	 * During the creation of an SMC socket, an internal service TCP socket
	 * is also created (Cf. smc_create_clcsk).
	 *
	 * Checks that Landlock does not restrict creation of the kernel space
	 * socket.
	 */
	EXPECT_EQ(0, test_socket(AF_SMC, SOCK_STREAM, 0));
}

/* clang-format off */
FIXTURE(packet_protocol) {};
/* clang-format on */

FIXTURE_VARIANT(packet_protocol)
{
	int family, type, protocol;
};

/* clang-format off */
FIXTURE_SETUP(packet_protocol) {};
/* clang-format on */

FIXTURE_TEARDOWN(packet_protocol)
{
}

/* clang-format off */
FIXTURE_VARIANT_ADD(packet_protocol, pf_inet) {
	/* clang-format on */
	.family = AF_INET,
	.type = SOCK_PACKET,
	.protocol = 0,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(packet_protocol, pf_packet) {
	/* clang-format on */
	.family = AF_PACKET,
	.type = SOCK_PACKET,
	.protocol = 0,
};

/*
 * (AF_INET, SOCK_PACKET) is an alias for the (AF_PACKET, SOCK_PACKET)
 * handled in socket layer (cf. __sock_create) due to compatibility reasons.
 *
 * Checks that Landlock does not restrict one pair if the other was allowed.
 */
TEST_F(packet_protocol, alias_restriction)
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = LANDLOCK_ACCESS_SOCKET_CREATE,
	};
	const int family = variant->family;
	const int type = variant->type;
	const int protocol = variant->protocol;
	const struct landlock_socket_attr packet_socket_create = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = family,
		.type = type,
		.protocol = protocol,
	};
	int ruleset_fd;

	/*
	 * Checks that packet socket is created successfully without
	 * landlock restrictions.
	 *
	 * Packet sockets require CAP_NET_RAW capability.
	 */
	set_cap(_metadata, CAP_NET_RAW);
	ASSERT_EQ(0, test_socket(AF_INET, SOCK_PACKET, 0));
	ASSERT_EQ(0, test_socket(AF_PACKET, SOCK_PACKET, 0));
	clear_cap(_metadata, CAP_NET_RAW);

	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);

	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
				       &packet_socket_create, 0));
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	set_cap(_metadata, CAP_NET_RAW);
	EXPECT_EQ(0, test_socket(AF_INET, SOCK_PACKET, 0));
	EXPECT_EQ(0, test_socket(AF_PACKET, SOCK_PACKET, 0));
	clear_cap(_metadata, CAP_NET_RAW);
}

/* clang-format off */
FIXTURE(tcp_protocol) {};
/* clang-format on */

FIXTURE_VARIANT(tcp_protocol)
{
	int family, type, protocol;
};

/* clang-format off */
FIXTURE_SETUP(tcp_protocol) {};
/* clang-format on */

FIXTURE_TEARDOWN(tcp_protocol)
{
}

/* clang-format off */
FIXTURE_VARIANT_ADD(tcp_protocol, variant1) {
	/* clang-format on */
	.family = AF_INET,
	.type = SOCK_STREAM,
	.protocol = 0,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(tcp_protocol, variant2) {
	/* clang-format on */
	.family = AF_INET,
	.type = SOCK_STREAM,
	.protocol = IPPROTO_TCP, /* = 6 */
};

/*
 * Landlock doesn't perform protocol mappings handled by network stack on
 * protocol family level. Test verifies that if only one definition is
 * allowed another becomes restricted.
 */
TEST_F(tcp_protocol, alias_restriction)
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = LANDLOCK_ACCESS_SOCKET_CREATE,
	};
	const int family = variant->family;
	const int type = variant->type;
	const int protocol = variant->protocol;
	const struct landlock_socket_attr tcp_socket_create = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = family,
		.type = type,
		.protocol = protocol,
	};
	int ruleset_fd;

	ASSERT_EQ(0, test_socket(AF_INET, SOCK_STREAM, 0));
	ASSERT_EQ(0, test_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));

	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);

	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
				       &tcp_socket_create, 0));
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	if (protocol == 0) {
		EXPECT_EQ(0, test_socket(AF_INET, SOCK_STREAM, 0));
		EXPECT_EQ(EACCES,
			  test_socket(AF_PACKET, SOCK_STREAM, IPPROTO_TCP));
	} else if (protocol == IPPROTO_TCP) {
		EXPECT_EQ(EACCES, test_socket(AF_INET, SOCK_STREAM, 0));
		EXPECT_EQ(0, test_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
	}
}

static int test_socketpair(int family, int type, int protocol)
{
	int fds[2];
	int err;

	err = socketpair(family, type | SOCK_CLOEXEC, protocol, fds);
	if (err)
		return errno;
	/*
	 * Mixing error codes from close(2) and socketpair(2) should not lead to
	 * any (access type) confusion for this test.
	 */
	if (close(fds[0]) != 0)
		return errno;
	if (close(fds[1]) != 0)
		return errno;
	return 0;
}

TEST_F(mini, socketpair)
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = LANDLOCK_ACCESS_SOCKET_CREATE,
	};
	const struct landlock_socket_attr unix_socket_create = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = AF_UNIX,
		.type = SOCK_STREAM,
		.protocol = 0,
	};
	int ruleset_fd;

	/* Tries to create socket when ruleset is not established. */
	ASSERT_EQ(0, test_socketpair(AF_UNIX, SOCK_STREAM, 0));
	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);

	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
				       &unix_socket_create, 0));
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Tries to create socket when protocol is allowed */
	EXPECT_EQ(0, test_socketpair(AF_UNIX, SOCK_STREAM, 0));

	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Tries to create socket when protocol is restricted. */
	EXPECT_EQ(EACCES, test_socketpair(AF_UNIX, SOCK_STREAM, 0));
}

/* clang-format off */
FIXTURE(connection_restriction) {};
/* clang-format on */

FIXTURE_VARIANT(connection_restriction)
{
	bool sandboxed;
};

FIXTURE_SETUP(connection_restriction)
{
	disable_caps(_metadata);
	setup_loopback(_metadata);
};

FIXTURE_TEARDOWN(connection_restriction)
{
}

/* clang-format off */
FIXTURE_VARIANT_ADD(connection_restriction, allowed) {
	/* clang-format on */
	.sandboxed = false,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(connection_restriction, sandboxed) {
	/* clang-format on */
	.sandboxed = true,
};

static const char loopback_ipv4[] = "127.0.0.1";
static const int backlog = 10;
static const int loopback_port = 1024;

TEST_F(connection_restriction, sctp_peeloff)
{
	int status, ret;
	pid_t child;
	struct sockaddr_in addr;
	int server_fd;

	server_fd =
		socket(AF_INET, SOCK_SEQPACKET | SOCK_CLOEXEC, IPPROTO_SCTP);
	ASSERT_LE(0, server_fd);

	addr.sin_family = AF_INET;
	addr.sin_port = htons(loopback_port);
	addr.sin_addr.s_addr = inet_addr(loopback_ipv4);

	ASSERT_EQ(0, bind(server_fd, &addr, sizeof(addr)));
	ASSERT_EQ(0, listen(server_fd, backlog));

	child = fork();
	ASSERT_LE(0, child);
	if (child == 0) {
		int client_fd;
		sctp_peeloff_flags_arg_t peeloff;
		socklen_t peeloff_size = sizeof(peeloff);

		/* Closes listening socket for the child. */
		ASSERT_EQ(0, close(server_fd));

		client_fd = socket(AF_INET, SOCK_SEQPACKET | SOCK_CLOEXEC,
				   IPPROTO_SCTP);
		ASSERT_LE(0, client_fd);

		/*
		 * Establishes connection between sockets and
		 * gets SCTP association id.
		 */
		ret = setsockopt(client_fd, IPPROTO_SCTP, SCTP_SOCKOPT_CONNECTX,
				 &addr, sizeof(addr));
		ASSERT_LE(0, ret);

		if (variant->sandboxed) {
			const struct landlock_ruleset_attr ruleset_attr = {
				.handled_access_socket =
					LANDLOCK_ACCESS_SOCKET_CREATE,
			};
			/* Denies creation of SCTP sockets. */
			int ruleset_fd = landlock_create_ruleset(
				&ruleset_attr, sizeof(ruleset_attr), 0);
			ASSERT_LE(0, ruleset_fd);

			enforce_ruleset(_metadata, ruleset_fd);
			ASSERT_EQ(0, close(ruleset_fd));
		}
		/*
		 * Branches off current SCTP association into a separate socket
		 * and returns it to user space.
		 */
		peeloff.p_arg.associd = ret;
		ret = getsockopt(client_fd, IPPROTO_SCTP, SCTP_SOCKOPT_PEELOFF,
				 &peeloff, &peeloff_size);

		/*
		 * Branching off existing SCTP association leads to creation of user space
		 * SCTP UDP socket and should be restricted by Landlock.
		 */
		if (variant->sandboxed) {
			EXPECT_EQ(-1, ret);
			EXPECT_EQ(EACCES, errno);
		} else {
			ASSERT_LE(0, ret);
		}

		/* getsockopt(2) returns 0 on success. */
		if (ret == 0) {
			/* Closes peeloff socket if such was created. */
			ASSERT_EQ(0, close(peeloff.p_arg.sd));
		}
		ASSERT_EQ(0, close(client_fd));
		_exit(_metadata->exit_code);
		return;
	}

	ASSERT_EQ(child, waitpid(child, &status, 0));
	ASSERT_EQ(1, WIFEXITED(status));
	ASSERT_EQ(EXIT_SUCCESS, WEXITSTATUS(status));

	ASSERT_EQ(0, close(server_fd));
}

TEST_HARNESS_MAIN
