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

TEST_HARNESS_MAIN
