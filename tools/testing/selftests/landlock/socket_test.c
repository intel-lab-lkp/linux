// SPDX-License-Identifier: GPL-2.0-only
/*
 * Landlock tests - Socket
 *
 * Copyright © 2024 Huawei Tech. Co., Ltd.
 */

#define _GNU_SOURCE

#include <linux/landlock.h>
#include <linux/pfkeyv2.h>
#include <linux/kcm.h>
#include <linux/can.h>
#include <linux/in.h>
#include <sys/prctl.h>

#include "common.h"

#define ACCESS_LAST LANDLOCK_ACCESS_SOCKET_CREATE
#define ACCESS_ALL LANDLOCK_ACCESS_SOCKET_CREATE

struct protocol_variant {
	int family;
	int type;
	int protocol;
};

static int test_socket(int family, int type, int protocol)
{
	int fd;

	fd = socket(family, type | SOCK_CLOEXEC, protocol);
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

static int test_socket_variant(const struct protocol_variant *const prot)
{
	return test_socket(prot->family, prot->type, prot->protocol);
}

FIXTURE(protocol)
{
	struct protocol_variant prot;
};

FIXTURE_VARIANT(protocol)
{
	const struct protocol_variant prot;
};

FIXTURE_SETUP(protocol)
{
	disable_caps(_metadata);
	self->prot = variant->prot;

	/*
	 * Some address families require this caps to be set
	 * (e.g. AF_CAIF, AF_KEY).
	 */
	set_cap(_metadata, CAP_SYS_ADMIN);
	set_cap(_metadata, CAP_NET_ADMIN);
	set_cap(_metadata, CAP_NET_RAW);
};

FIXTURE_TEARDOWN(protocol)
{
	clear_cap(_metadata, CAP_SYS_ADMIN);
	clear_cap(_metadata, CAP_NET_ADMIN);
	clear_cap(_metadata, CAP_NET_RAW);
}

#define PROTOCOL_VARIANT_EXT_ADD(family_, type_, protocol_) \
	FIXTURE_VARIANT_ADD(protocol, family_##_##type_)    \
	{                                                   \
		.prot = {                                   \
			.family = AF_##family_,             \
			.type = SOCK_##type_,               \
			.protocol = protocol_,              \
		},                                          \
	}

#define PROTOCOL_VARIANT_ADD(family, type) \
	PROTOCOL_VARIANT_EXT_ADD(family, type, 0)

/*
 * Every protocol that can be used to create socket using create() method
 * of net_proto_family structure is tested (e.g. this method is used to
 * create socket with socket(2)).
 *
 * List of address families that are not tested:
 * - AF_ASH, AF_SNA, AF_WANPIPE, AF_NETBEUI, AF_IPX, AF_DECNET, AF_ECONET
 *   and AF_IRDA are not implemented in kernel.
 * - AF_BRIDGE, AF_MPLS can't be used for creating sockets.
 * - AF_SECURITY - pseudo AF (Cf. socket.h).
 * - AF_IB is reserved by infiniband.
 */

/* Cf. unix_create */
PROTOCOL_VARIANT_ADD(UNIX, STREAM);
PROTOCOL_VARIANT_ADD(UNIX, RAW);
PROTOCOL_VARIANT_ADD(UNIX, DGRAM);
PROTOCOL_VARIANT_ADD(UNIX, SEQPACKET);

/* Cf. inet_create */
PROTOCOL_VARIANT_ADD(INET, STREAM);
PROTOCOL_VARIANT_ADD(INET, DGRAM);
PROTOCOL_VARIANT_EXT_ADD(INET, RAW, IPPROTO_TCP);
PROTOCOL_VARIANT_EXT_ADD(INET, SEQPACKET, IPPROTO_SCTP);

/* Cf. ax25_create */
PROTOCOL_VARIANT_ADD(AX25, DGRAM);
PROTOCOL_VARIANT_ADD(AX25, SEQPACKET);
PROTOCOL_VARIANT_ADD(AX25, RAW);

/* Cf. atalk_create */
PROTOCOL_VARIANT_ADD(APPLETALK, RAW);
PROTOCOL_VARIANT_ADD(APPLETALK, DGRAM);

/* Cf. nr_create */
PROTOCOL_VARIANT_ADD(NETROM, SEQPACKET);

/* Cf. pvc_create */
PROTOCOL_VARIANT_ADD(ATMPVC, DGRAM);
PROTOCOL_VARIANT_ADD(ATMPVC, RAW);
PROTOCOL_VARIANT_ADD(ATMPVC, RDM);
PROTOCOL_VARIANT_ADD(ATMPVC, SEQPACKET);
PROTOCOL_VARIANT_ADD(ATMPVC, DCCP);
PROTOCOL_VARIANT_ADD(ATMPVC, PACKET);

/* Cf. x25_create */
PROTOCOL_VARIANT_ADD(X25, SEQPACKET);

/* Cf. inet6_create */
PROTOCOL_VARIANT_ADD(INET6, STREAM);
PROTOCOL_VARIANT_ADD(INET6, DGRAM);
PROTOCOL_VARIANT_EXT_ADD(INET6, RAW, IPPROTO_TCP);

/* Cf. rose_create */
PROTOCOL_VARIANT_ADD(ROSE, SEQPACKET);

/* Cf. pfkey_create */
PROTOCOL_VARIANT_EXT_ADD(KEY, RAW, PF_KEY_V2);

/* Cf. netlink_create */
PROTOCOL_VARIANT_ADD(NETLINK, RAW);
PROTOCOL_VARIANT_ADD(NETLINK, DGRAM);

/* Cf. packet_create */
PROTOCOL_VARIANT_ADD(PACKET, DGRAM);
PROTOCOL_VARIANT_ADD(PACKET, RAW);
PROTOCOL_VARIANT_ADD(PACKET, PACKET);

/* Cf. svc_create */
PROTOCOL_VARIANT_ADD(ATMSVC, DGRAM);
PROTOCOL_VARIANT_ADD(ATMSVC, RAW);
PROTOCOL_VARIANT_ADD(ATMSVC, RDM);
PROTOCOL_VARIANT_ADD(ATMSVC, SEQPACKET);
PROTOCOL_VARIANT_ADD(ATMSVC, DCCP);
PROTOCOL_VARIANT_ADD(ATMSVC, PACKET);

/* Cf. rds_create */
PROTOCOL_VARIANT_ADD(RDS, SEQPACKET);

/* Cf. pppox_create + pppoe_create */
PROTOCOL_VARIANT_ADD(PPPOX, STREAM);
PROTOCOL_VARIANT_ADD(PPPOX, DGRAM);
PROTOCOL_VARIANT_ADD(PPPOX, RAW);
PROTOCOL_VARIANT_ADD(PPPOX, RDM);
PROTOCOL_VARIANT_ADD(PPPOX, SEQPACKET);
PROTOCOL_VARIANT_ADD(PPPOX, DCCP);
PROTOCOL_VARIANT_ADD(PPPOX, PACKET);

/* Cf. llc_ui_create */
PROTOCOL_VARIANT_ADD(LLC, DGRAM);
PROTOCOL_VARIANT_ADD(LLC, STREAM);

/* Cf. can_create */
PROTOCOL_VARIANT_EXT_ADD(CAN, DGRAM, CAN_BCM);

/* Cf. tipc_sk_create */
PROTOCOL_VARIANT_ADD(TIPC, STREAM);
PROTOCOL_VARIANT_ADD(TIPC, SEQPACKET);
PROTOCOL_VARIANT_ADD(TIPC, DGRAM);
PROTOCOL_VARIANT_ADD(TIPC, RDM);

/* Cf. l2cap_sock_create */
#ifndef __s390x__
PROTOCOL_VARIANT_ADD(BLUETOOTH, SEQPACKET);
PROTOCOL_VARIANT_ADD(BLUETOOTH, STREAM);
PROTOCOL_VARIANT_ADD(BLUETOOTH, DGRAM);
PROTOCOL_VARIANT_ADD(BLUETOOTH, RAW);
#endif

/* Cf. iucv_sock_create */
#ifdef __s390x__
PROTOCOL_VARIANT_ADD(IUCV, STREAM);
PROTOCOL_VARIANT_ADD(IUCV, SEQPACKET);
#endif

/* Cf. rxrpc_create */
PROTOCOL_VARIANT_EXT_ADD(RXRPC, DGRAM, PF_INET);

/* Cf. mISDN_sock_create */
#define ISDN_P_BASE 0 /* Cf. linux/mISDNif.h */
#define ISDN_P_TE_S0 0x01 /* Cf. linux/mISDNif.h */
PROTOCOL_VARIANT_EXT_ADD(ISDN, RAW, ISDN_P_BASE);
PROTOCOL_VARIANT_EXT_ADD(ISDN, DGRAM, ISDN_P_TE_S0);

/* Cf. pn_socket_create */
PROTOCOL_VARIANT_ADD(PHONET, DGRAM);
PROTOCOL_VARIANT_ADD(PHONET, SEQPACKET);

/* Cf. ieee802154_create */
PROTOCOL_VARIANT_ADD(IEEE802154, RAW);
PROTOCOL_VARIANT_ADD(IEEE802154, DGRAM);

/* Cf. caif_create */
PROTOCOL_VARIANT_ADD(CAIF, SEQPACKET);
PROTOCOL_VARIANT_ADD(CAIF, STREAM);

/* Cf. alg_create */
PROTOCOL_VARIANT_ADD(ALG, SEQPACKET);

/* Cf. nfc_sock_create + rawsock_create */
PROTOCOL_VARIANT_ADD(NFC, SEQPACKET);

/* Cf. vsock_create */
#if defined(__x86_64__) || defined(__aarch64__)
PROTOCOL_VARIANT_ADD(VSOCK, DGRAM);
PROTOCOL_VARIANT_ADD(VSOCK, STREAM);
PROTOCOL_VARIANT_ADD(VSOCK, SEQPACKET);
#endif

/* Cf. kcm_create */
PROTOCOL_VARIANT_EXT_ADD(KCM, DGRAM, KCMPROTO_CONNECTED);
PROTOCOL_VARIANT_EXT_ADD(KCM, SEQPACKET, KCMPROTO_CONNECTED);

/* Cf. qrtr_create */
PROTOCOL_VARIANT_ADD(QIPCRTR, DGRAM);

/* Cf. smc_create */
#ifndef __alpha__
PROTOCOL_VARIANT_ADD(SMC, STREAM);
#endif

/* Cf. xsk_create */
PROTOCOL_VARIANT_ADD(XDP, RAW);

/* Cf. mctp_pf_create */
PROTOCOL_VARIANT_ADD(MCTP, DGRAM);

TEST_F(protocol, create)
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = LANDLOCK_ACCESS_SOCKET_CREATE,
	};
	const struct landlock_socket_attr create_socket_attr = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = self->prot.family,
		.type = self->prot.type,
	};
	int ruleset_fd;

	/* Tries to create a socket when ruleset is not established. */
	ASSERT_EQ(0, test_socket_variant(&self->prot));

	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);

	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
				       &create_socket_attr, 0));

	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Tries to create a socket when protocol is allowed. */
	EXPECT_EQ(0, test_socket_variant(&self->prot));

	/* Denied create. */
	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);

	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Tries to create a socket when protocol is restricted. */
	EXPECT_EQ(EACCES, test_socket_variant(&self->prot));
}

TEST_F(protocol, socket_access_rights)
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = ACCESS_ALL,
	};
	struct landlock_socket_attr protocol = {
		.family = self->prot.family,
		.type = self->prot.type,
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

TEST_F(protocol, rule_with_unknown_access)
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = ACCESS_ALL,
	};
	struct landlock_socket_attr protocol = {
		.family = self->prot.family,
		.type = self->prot.type,
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
	ASSERT_EQ(0, close(ruleset_fd));
}

TEST_F(protocol, rule_with_unhandled_access)
{
	struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = LANDLOCK_ACCESS_SOCKET_CREATE,
	};
	struct landlock_socket_attr protocol = {
		.family = self->prot.family,
		.type = self->prot.type,
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

	ASSERT_EQ(0, close(ruleset_fd));
}

TEST_F(protocol, rule_with_empty_access)
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = LANDLOCK_ACCESS_SOCKET_CREATE
	};
	struct landlock_socket_attr protocol_allowed = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = self->prot.family,
		.type = self->prot.type,
	};
	struct landlock_socket_attr protocol_denied = {
		.allowed_access = 0,
		.family = self->prot.family,
		.type = self->prot.type,
	};
	int ruleset_fd;

	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);

	/* Checks zero access value. */
	EXPECT_EQ(-1, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
					&protocol_denied, 0));
	EXPECT_EQ(ENOMSG, errno);

	/* Adds with legitimate value. */
	EXPECT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
				       &protocol_allowed, 0));

	ASSERT_EQ(0, close(ruleset_fd));
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

TEST_F(protocol, ruleset_overlap)
{
	const struct landlock_socket_attr create_socket_attr = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = self->prot.family,
		.type = self->prot.type,
	};

	/* socket(2) is allowed if there are no restrictions. */
	ASSERT_EQ(0, test_socket_variant(&self->prot));

	/* Creates ruleset with socket(2) allowed. */
	add_ruleset_layer(_metadata, &create_socket_attr);
	EXPECT_EQ(0, test_socket_variant(&self->prot));

	/* Adds ruleset layer with socket(2) restricted. */
	add_ruleset_layer(_metadata, NULL);
	EXPECT_EQ(EACCES, test_socket_variant(&self->prot));

	/*
	 * Adds ruleset layer with socket(2) allowed. socket(2) is restricted
	 * by second layer of the ruleset.
	 */
	add_ruleset_layer(_metadata, &create_socket_attr);
	EXPECT_EQ(EACCES, test_socket_variant(&self->prot));
}

TEST(ruleset_with_unknown_access)
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

FIXTURE(prot_outside_range)
{
	struct protocol_variant prot;
};

FIXTURE_VARIANT(prot_outside_range)
{
	struct protocol_variant prot;
};

FIXTURE_SETUP(prot_outside_range)
{
	self->prot = variant->prot;
};

FIXTURE_TEARDOWN(prot_outside_range)
{
}

/* Cf. include/linux/net.h */
#define SOCK_MAX (SOCK_PACKET + 1)
#define NEGATIVE_MAX (-1)
/* Cf. linux/net.h */
#define SOCK_TYPE_MASK 0xf

#define SOCK_STREAM_FLAG1 (SOCK_STREAM | SOCK_NONBLOCK)
#define SOCK_STREAM_FLAG2 (SOCK_STREAM | SOCK_CLOEXEC)

#define INVAL_PROTOCOL_VARIANT_ADD(family_, type_)                 \
	FIXTURE_VARIANT_ADD(prot_outside_range, family_##_##type_) \
	{                                                          \
		.prot = {                                          \
			.family = family_,                         \
			.type = type_,                             \
		},                                                 \
	}

INVAL_PROTOCOL_VARIANT_ADD(INT32_MIN, INT32_MIN);
INVAL_PROTOCOL_VARIANT_ADD(INT32_MIN, NEGATIVE_MAX);
INVAL_PROTOCOL_VARIANT_ADD(INT32_MIN, SOCK_STREAM);
INVAL_PROTOCOL_VARIANT_ADD(INT32_MIN, SOCK_MAX);
INVAL_PROTOCOL_VARIANT_ADD(INT32_MIN, INT32_MAX);

INVAL_PROTOCOL_VARIANT_ADD(NEGATIVE_MAX, INT32_MIN);
INVAL_PROTOCOL_VARIANT_ADD(NEGATIVE_MAX, NEGATIVE_MAX);
INVAL_PROTOCOL_VARIANT_ADD(NEGATIVE_MAX, SOCK_STREAM);
INVAL_PROTOCOL_VARIANT_ADD(NEGATIVE_MAX, SOCK_MAX);
INVAL_PROTOCOL_VARIANT_ADD(NEGATIVE_MAX, INT32_MAX);

INVAL_PROTOCOL_VARIANT_ADD(AF_INET, INT32_MIN);
INVAL_PROTOCOL_VARIANT_ADD(AF_INET, NEGATIVE_MAX);
INVAL_PROTOCOL_VARIANT_ADD(AF_INET, SOCK_MAX);
INVAL_PROTOCOL_VARIANT_ADD(AF_INET, INT32_MAX);

INVAL_PROTOCOL_VARIANT_ADD(AF_MAX, INT32_MIN);
INVAL_PROTOCOL_VARIANT_ADD(AF_MAX, NEGATIVE_MAX);
INVAL_PROTOCOL_VARIANT_ADD(AF_MAX, SOCK_STREAM);
INVAL_PROTOCOL_VARIANT_ADD(AF_MAX, SOCK_MAX);
INVAL_PROTOCOL_VARIANT_ADD(AF_MAX, INT32_MAX);

INVAL_PROTOCOL_VARIANT_ADD(INT32_MAX, INT32_MIN);
INVAL_PROTOCOL_VARIANT_ADD(INT32_MAX, NEGATIVE_MAX);
INVAL_PROTOCOL_VARIANT_ADD(INT32_MAX, SOCK_STREAM);
INVAL_PROTOCOL_VARIANT_ADD(INT32_MAX, SOCK_MAX);
INVAL_PROTOCOL_VARIANT_ADD(INT32_MAX, INT32_MAX);

TEST_F(prot_outside_range, add_rule)
{
	int family = self->prot.family;
	int type = self->prot.type;
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = LANDLOCK_ACCESS_SOCKET_CREATE,
	};
	struct landlock_socket_attr create_socket_overflow = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = family,
		.type = type,
	};
	int ruleset_fd;

	/* Checks type flags using __sys_socket_create. */
	if ((type & ~SOCK_TYPE_MASK) & ~(SOCK_CLOEXEC | SOCK_NONBLOCK)) {
		ASSERT_EQ(EINVAL, test_socket_variant(&self->prot));
	}
	/* Checks range using __sock_create. */
	else if (family >= AF_MAX || family < 0) {
		ASSERT_EQ(EAFNOSUPPORT, test_socket_variant(&self->prot));
	} else {
		ASSERT_EQ(EINVAL, test_socket_variant(&self->prot));
	}

	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);

	EXPECT_EQ(-1, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
					&create_socket_overflow, 0));
	EXPECT_EQ(EINVAL, errno);

	ASSERT_EQ(0, close(ruleset_fd));
}

TEST(unsupported_af_and_prot)
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = LANDLOCK_ACCESS_SOCKET_CREATE,
	};
	struct landlock_socket_attr socket_af_unsupported = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = AF_UNSPEC,
		.type = SOCK_STREAM,
	};
	struct landlock_socket_attr socket_prot_unsupported = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = AF_UNIX,
		.type = SOCK_PACKET,
	};
	int ruleset_fd;

	/* Tries to create a socket when ruleset is not established. */
	ASSERT_EQ(EAFNOSUPPORT, test_socket(AF_UNSPEC, SOCK_STREAM, 0));
	ASSERT_EQ(ESOCKTNOSUPPORT, test_socket(AF_UNIX, SOCK_PACKET, 0));

	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);

	EXPECT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
				       &socket_af_unsupported, 0));
	EXPECT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
				       &socket_prot_unsupported, 0));
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Tries to create a socket when protocols are allowed. */
	EXPECT_EQ(EAFNOSUPPORT, test_socket(AF_UNSPEC, SOCK_STREAM, 0));
	EXPECT_EQ(ESOCKTNOSUPPORT, test_socket(AF_UNIX, SOCK_PACKET, 0));

	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Tries to create a socket when protocols are restricted. */
	EXPECT_EQ(EAFNOSUPPORT, test_socket(AF_UNSPEC, SOCK_STREAM, 0));
	EXPECT_EQ(ESOCKTNOSUPPORT, test_socket(AF_UNIX, SOCK_PACKET, 0));
}

TEST(kernel_socket)
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = LANDLOCK_ACCESS_SOCKET_CREATE,
	};
	struct landlock_socket_attr smc_socket_create = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = AF_SMC,
		.type = SOCK_STREAM,
	};
	int ruleset_fd;

	/*
	 * Checks that SMC socket is created sucessfuly without
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

FIXTURE(packet_protocol)
{
	struct protocol_variant prot_allowed, prot_tested;
};

FIXTURE_VARIANT(packet_protocol)
{
	bool packet;
};

FIXTURE_SETUP(packet_protocol)
{
	self->prot_allowed.type = self->prot_tested.type = SOCK_PACKET;

	self->prot_allowed.family = variant->packet ? AF_PACKET : AF_INET;
	self->prot_tested.family = variant->packet ? AF_INET : AF_PACKET;

	/* Packet protocol requires NET_RAW to be set (Cf. packet_create). */
	set_cap(_metadata, CAP_NET_RAW);
};

FIXTURE_TEARDOWN(packet_protocol)
{
	clear_cap(_metadata, CAP_NET_RAW);
}

/* clang-format off */
FIXTURE_VARIANT_ADD(packet_protocol, packet_allows_inet) {
	/* clang-format on */
	.packet = true,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(packet_protocol, inet_allows_packet) {
	/* clang-format on */
	.packet = false,
};

TEST_F(packet_protocol, alias_restriction)
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = LANDLOCK_ACCESS_SOCKET_CREATE,
	};
	struct landlock_socket_attr packet_socket_create = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = self->prot_allowed.family,
		.type = self->prot_allowed.type,
	};
	int ruleset_fd;

	/*
	 * Checks that packet socket is created sucessfuly without
	 * landlock restrictions.
	 */
	ASSERT_EQ(0, test_socket_variant(&self->prot_tested));

	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);

	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_SOCKET,
				       &packet_socket_create, 0));
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/*
	 * (AF_INET, SOCK_PACKET) is an alias for the (AF_PACKET, SOCK_PACKET)
	 * (Cf. __sock_create). Checks that Landlock does not restrict one pair
	 * if the other was allowed.
	 */
	EXPECT_EQ(0, test_socket_variant(&self->prot_tested));
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

FIXTURE(socket_creation)
{
	bool sandboxed;
	bool allowed;
};

FIXTURE_VARIANT(socket_creation)
{
	bool sandboxed;
	bool allowed;
};

FIXTURE_SETUP(socket_creation)
{
	self->sandboxed = variant->sandboxed;
	self->allowed = variant->allowed;

	setup_loopback(_metadata);
};

FIXTURE_TEARDOWN(socket_creation)
{
}

/* clang-format off */
FIXTURE_VARIANT_ADD(socket_creation, no_sandbox) {
	/* clang-format on */
	.sandboxed = false,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(socket_creation, sandbox_allow) {
	/* clang-format on */
	.sandboxed = true,
	.allowed = true,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(socket_creation, sandbox_deny) {
	/* clang-format on */
	.sandboxed = true,
	.allowed = false,
};

TEST_F(socket_creation, socketpair)
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_socket = LANDLOCK_ACCESS_SOCKET_CREATE,
	};
	struct landlock_socket_attr unix_socket_create = {
		.allowed_access = LANDLOCK_ACCESS_SOCKET_CREATE,
		.family = AF_UNIX,
		.type = SOCK_STREAM,
	};
	int ruleset_fd;

	if (self->sandboxed) {
		ruleset_fd = landlock_create_ruleset(&ruleset_attr,
						     sizeof(ruleset_attr), 0);
		ASSERT_LE(0, ruleset_fd);

		if (self->allowed) {
			ASSERT_EQ(0, landlock_add_rule(ruleset_fd,
						       LANDLOCK_RULE_SOCKET,
						       &unix_socket_create, 0));
		}
		enforce_ruleset(_metadata, ruleset_fd);
		ASSERT_EQ(0, close(ruleset_fd));
	}

	if (!self->sandboxed || self->allowed) {
		/*
		 * Tries to create sockets when ruleset is not established
		 * or protocol is allowed.
		 */
		EXPECT_EQ(0, test_socketpair(AF_UNIX, SOCK_STREAM, 0));
	} else {
		/* Tries to create sockets when protocol is restricted. */
		EXPECT_EQ(EACCES, test_socketpair(AF_UNIX, SOCK_STREAM, 0));
	}
}

TEST_HARNESS_MAIN
