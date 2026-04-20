// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for J1939 socket utility functions
 *
 * Copyright (c) 2026 Pengutronix,
 *                    Oleksij Rempel <kernel@pengutronix.de>
 */

#include <kunit/test.h>
#include <linux/can/j1939.h>
#include "../j1939-priv.h"
#include "../j1939-test.h"

/*
 * Priority conversion: J1939 prio (0=high, 7=low) <-> sk_priority
 * (7=high, 0=low)
 */
static void j1939_test_prio_conversion_roundtrip(struct kunit *test)
{
	int i;

	for (i = 0; i <= 7; i++) {
		u32 sk_prio = j1939_to_sk_priority_wrapper(i);
		priority_t j1939_p = j1939_prio_wrapper(sk_prio);

		KUNIT_EXPECT_EQ(test, j1939_p, i);
	}
}

/*
 * Out-of-range sk_priority values must clamp to 0 (highest) to prevent
 * 3-bit wraparound. High UNIX priority (>7) = high importance = J1939 prio 0.
 */
static void j1939_test_prio_clamping(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, j1939_prio_wrapper(8), 0);
	KUNIT_EXPECT_EQ(test, j1939_prio_wrapper(100), 0);
	KUNIT_EXPECT_EQ(test, j1939_prio_wrapper(UINT_MAX), 0);
}

/* PGN (Parameter Group Number) validation: 18-bit identifier (0x00000-0x3FFFF) */
static void j1939_test_pgn_valid_range(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, j1939_pgn_is_valid_wrapper(0));
	KUNIT_EXPECT_TRUE(test, j1939_pgn_is_valid_wrapper(J1939_PGN_MAX));
	KUNIT_EXPECT_FALSE(test, j1939_pgn_is_valid_wrapper(J1939_PGN_MAX + 1));
	KUNIT_EXPECT_FALSE(test, j1939_pgn_is_valid_wrapper(0xFFFFFFFF));
}

static void j1939_test_pgn_valid_common(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, j1939_pgn_is_valid_wrapper(0xEF00));
	KUNIT_EXPECT_TRUE(test, j1939_pgn_is_valid_wrapper(0xFECA)); /* DM1 */
	KUNIT_EXPECT_TRUE(test, j1939_pgn_is_valid_wrapper(0xFEEC));
	KUNIT_EXPECT_TRUE(test, j1939_pgn_is_valid_wrapper(0xF004));
}

/*
 * Clean PDU tests: PDU1 (<0xF000) requires DA bits clear;
 * PDU2 (>=0xF000) always clean.
 * "Dirty" PDU1 (non-zero DA) causes addressing ambiguity.
 */
static void j1939_test_clean_pdu_pdu1_format(struct kunit *test)
{
	/* PDU1 with DA bits clear - clean */
	KUNIT_EXPECT_TRUE(test, j1939_pgn_is_clean_pdu_wrapper(0xEF00));
	KUNIT_EXPECT_TRUE(test, j1939_pgn_is_clean_pdu_wrapper(0xEA00));
	KUNIT_EXPECT_TRUE(test, j1939_pgn_is_clean_pdu_wrapper(0x0000));

	/* PDU1 with DA bits set - dirty */
	KUNIT_EXPECT_FALSE(test, j1939_pgn_is_clean_pdu_wrapper(0xEF01));
	KUNIT_EXPECT_FALSE(test, j1939_pgn_is_clean_pdu_wrapper(0xEF12));
	KUNIT_EXPECT_FALSE(test, j1939_pgn_is_clean_pdu_wrapper(0xEAFF));
	KUNIT_EXPECT_FALSE(test, j1939_pgn_is_clean_pdu_wrapper(0x00FF));
}

static void j1939_test_clean_pdu_pdu2_format(struct kunit *test)
{
	/* PDU2 - always clean, lower 8 bits are Group Extension, not DA */
	KUNIT_EXPECT_TRUE(test, j1939_pgn_is_clean_pdu_wrapper(0xF000));
	KUNIT_EXPECT_TRUE(test, j1939_pgn_is_clean_pdu_wrapper(0xF0FF));
	KUNIT_EXPECT_TRUE(test, j1939_pgn_is_clean_pdu_wrapper(0xFECA)); /* DM1 */
	KUNIT_EXPECT_TRUE(test, j1939_pgn_is_clean_pdu_wrapper(0xFEEC));
	KUNIT_EXPECT_TRUE(test, j1939_pgn_is_clean_pdu_wrapper(0xFFFF));
}

/*
 * Socket address sanity checks catch errors early: NULL, short length, wrong
 * family, etc.
 */
static void j1939_test_sanity_check_null_addr(struct kunit *test)
{
	int ret;

	ret = j1939_sk_sanity_check(NULL, 0);
	KUNIT_EXPECT_EQ(test, ret, -EDESTADDRREQ);
}

static void j1939_test_sanity_check_short_length(struct kunit *test)
{
	struct sockaddr_can addr = {};
	int ret;

	ret = j1939_sk_sanity_check(&addr, J1939_MIN_NAMELEN - 1);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static void j1939_test_sanity_check_wrong_family(struct kunit *test)
{
	struct sockaddr_can addr = {
		.can_family = AF_INET,
		.can_ifindex = 1,
	};
	int ret;

	ret = j1939_sk_sanity_check(&addr, J1939_MIN_NAMELEN);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static void j1939_test_sanity_check_no_ifindex(struct kunit *test)
{
	struct sockaddr_can addr = {
		.can_family = AF_CAN,
		.can_ifindex = 0,
	};
	int ret;

	ret = j1939_sk_sanity_check(&addr, J1939_MIN_NAMELEN);
	KUNIT_EXPECT_EQ(test, ret, -ENODEV);
}

static void j1939_test_sanity_check_dirty_pdu1_pgn(struct kunit *test)
{
	struct sockaddr_can addr = {
		.can_family = AF_CAN,
		.can_ifindex = 1,
		.can_addr.j1939.pgn = 0xEF12, /* PDU1 with DA bits set */
	};
	int ret;

	ret = j1939_sk_sanity_check(&addr, J1939_MIN_NAMELEN);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static void j1939_test_sanity_check_valid(struct kunit *test)
{
	struct sockaddr_can addr;
	int ret;

	/* Valid PDU2 with full fields */
	addr = (struct sockaddr_can){
		.can_family = AF_CAN,
		.can_ifindex = 1,
		.can_addr.j1939.pgn = 0xFECA,
		.can_addr.j1939.name = 0x1234567890ABCDEF,
		.can_addr.j1939.addr = 0x80,
	};
	ret = j1939_sk_sanity_check(&addr, J1939_MIN_NAMELEN);
	KUNIT_EXPECT_EQ(test, ret, 0);

	/* Valid clean PDU1 */
	addr.can_addr.j1939.pgn = 0xEF00;
	ret = j1939_sk_sanity_check(&addr, J1939_MIN_NAMELEN);
	KUNIT_EXPECT_EQ(test, ret, 0);

	/* Valid J1939_NO_PGN wildcard */
	addr.can_addr.j1939.pgn = J1939_NO_PGN;
	ret = j1939_sk_sanity_check(&addr, J1939_MIN_NAMELEN);
	KUNIT_EXPECT_EQ(test, ret, 0);
}

/* Socket-to-sockaddr conversion for getname(): peer=0 (local), peer=1 (remote) */
static void j1939_test_sock2sockaddr_local(struct kunit *test)
{
	struct sockaddr_can addr;
	struct j1939_sock jsk = {
		.ifindex = 5,
		.addr = {
			.src_name = 0x1234567890ABCDEF,
			.dst_name = 0xFEDCBA0987654321,
			.pgn = 0xFECA,
			.sa = 0x25,
			.da = 0x30,
		},
	};

	memset(&addr, 0xFF, sizeof(addr));
	j1939_sk_sock2sockaddr_can(&addr, &jsk, 0);

	KUNIT_EXPECT_EQ(test, addr.can_family, AF_CAN);
	KUNIT_EXPECT_EQ(test, addr.can_ifindex, 5);
	KUNIT_EXPECT_EQ(test, addr.can_addr.j1939.name, 0x1234567890ABCDEF);
	KUNIT_EXPECT_EQ(test, addr.can_addr.j1939.addr, 0x25);
	KUNIT_EXPECT_EQ(test, addr.can_addr.j1939.pgn, 0xFECA);
}

static void j1939_test_sock2sockaddr_peer(struct kunit *test)
{
	struct sockaddr_can addr;
	struct j1939_sock jsk = {
		.ifindex = 5,
		.addr = {
			.src_name = 0x1234567890ABCDEF,
			.dst_name = 0xFEDCBA0987654321,
			.pgn = 0xFECA,
			.sa = 0x25,
			.da = 0x30,
		},
	};

	memset(&addr, 0xFF, sizeof(addr));
	j1939_sk_sock2sockaddr_can(&addr, &jsk, 1);

	KUNIT_EXPECT_EQ(test, addr.can_family, AF_CAN);
	KUNIT_EXPECT_EQ(test, addr.can_ifindex, 5);
	KUNIT_EXPECT_EQ(test, addr.can_addr.j1939.name, 0xFEDCBA0987654321);
	KUNIT_EXPECT_EQ(test, addr.can_addr.j1939.addr, 0x30);
	KUNIT_EXPECT_EQ(test, addr.can_addr.j1939.pgn, 0xFECA);
}

/* Verify padding is zeroed to prevent kernel info leak */
static void j1939_test_sock2sockaddr_zeroes_padding(struct kunit *test)
{
	struct sockaddr_can addr;
	struct j1939_sock jsk = {
		.ifindex = 1,
		.addr = {
			.src_name = 0,
			.dst_name = 0,
			.pgn = 0,
			.sa = 0,
			.da = 0,
		},
	};

	memset(&addr, 0xAA, sizeof(addr));
	j1939_sk_sock2sockaddr_can(&addr, &jsk, 0);

	KUNIT_EXPECT_EQ(test, addr.can_addr.j1939.name, 0ULL);
	KUNIT_EXPECT_EQ(test, addr.can_addr.j1939.addr, 0);
	KUNIT_EXPECT_EQ(test, addr.can_addr.j1939.pgn, 0U);
}

/* Error queue buffer size for transport statistics (MSG_ERRQUEUE) */

static void j1939_test_errqueue_size_consistency(struct kunit *test)
{
	size_t rx_rts = j1939_sk_opt_stats_get_size(J1939_ERRQUEUE_RX_RTS);
	size_t tx_ack = j1939_sk_opt_stats_get_size(J1939_ERRQUEUE_TX_ACK);

	/* RX_RTS should be larger (more fields) */
	KUNIT_EXPECT_GT(test, rx_rts, tx_ack);
	KUNIT_EXPECT_GT(test, tx_ack, 0);

	/* All non-RX_RTS types should have same size (default case) */
	KUNIT_EXPECT_EQ(test, tx_ack,
			j1939_sk_opt_stats_get_size(J1939_ERRQUEUE_TX_SCHED));
	KUNIT_EXPECT_EQ(test, tx_ack,
			j1939_sk_opt_stats_get_size(J1939_ERRQUEUE_TX_ABORT));
	KUNIT_EXPECT_EQ(test, tx_ack,
			j1939_sk_opt_stats_get_size(J1939_ERRQUEUE_RX_DPO));
	KUNIT_EXPECT_EQ(test, tx_ack,
			j1939_sk_opt_stats_get_size(J1939_ERRQUEUE_RX_ABORT));
}

static struct kunit_case j1939_socket_test_cases[] = {
	KUNIT_CASE(j1939_test_prio_conversion_roundtrip),
	KUNIT_CASE(j1939_test_prio_clamping),

	KUNIT_CASE(j1939_test_pgn_valid_range),
	KUNIT_CASE(j1939_test_pgn_valid_common),

	KUNIT_CASE(j1939_test_clean_pdu_pdu1_format),
	KUNIT_CASE(j1939_test_clean_pdu_pdu2_format),

	KUNIT_CASE(j1939_test_sanity_check_null_addr),
	KUNIT_CASE(j1939_test_sanity_check_short_length),
	KUNIT_CASE(j1939_test_sanity_check_wrong_family),
	KUNIT_CASE(j1939_test_sanity_check_no_ifindex),
	KUNIT_CASE(j1939_test_sanity_check_dirty_pdu1_pgn),
	KUNIT_CASE(j1939_test_sanity_check_valid),

	KUNIT_CASE(j1939_test_sock2sockaddr_local),
	KUNIT_CASE(j1939_test_sock2sockaddr_peer),
	KUNIT_CASE(j1939_test_sock2sockaddr_zeroes_padding),

	KUNIT_CASE(j1939_test_errqueue_size_consistency),
	{}
};

static struct kunit_suite j1939_socket_test_suite = {
	.name = "j1939-socket",
	.test_cases = j1939_socket_test_cases,
};

kunit_test_suite(j1939_socket_test_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit tests for J1939 socket utilities");
MODULE_AUTHOR("Oleksij Rempel <kernel@pengutronix.de>");
