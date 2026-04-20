// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for J1939 transport layer functions
 *
 * Copyright (c) 2026 Pengutronix,
 *                    Oleksij Rempel <kernel@pengutronix.de>
 */

#include <kunit/test.h>
#include <linux/can/j1939.h>
#include "../j1939-priv.h"
#include "../j1939-test.h"

/* Abort code to errno conversion for userspace error reporting */

struct abort_errno_map {
	u8 abort_code;
	int expected_errno;
};

static void j1939_test_abort_to_errno(struct kunit *test)
{
	static const struct abort_errno_map tests[] = {
		{ J1939_XTP_ABORT_BUSY, EALREADY },
		{ J1939_XTP_ABORT_RESOURCE, EMSGSIZE },
		{ J1939_XTP_ABORT_TIMEOUT, EHOSTUNREACH },
		{ J1939_XTP_ABORT_GENERIC, EBADMSG },
		{ J1939_XTP_ABORT_FAULT, ENOTRECOVERABLE },
		{ J1939_XTP_ABORT_UNEXPECTED_DATA, ENOTCONN },
		{ J1939_XTP_ABORT_BAD_SEQ, EILSEQ },
		{ J1939_XTP_ABORT_DUP_SEQ, EPROTO },
		{ J1939_XTP_ABORT_EDPO_UNEXPECTED, EPROTO },
		{ J1939_XTP_ABORT_BAD_EDPO_PGN, EPROTO },
		{ J1939_XTP_ABORT_EDPO_OUTOF_CTS, EPROTO },
		{ J1939_XTP_ABORT_BAD_EDPO_OFFSET, EPROTO },
		{ J1939_XTP_ABORT_ECTS_UNXPECTED_PGN, EPROTO },
		{ J1939_XTP_ABORT_ECTS_TOO_BIG, EPROTO },
		{ J1939_XTP_ABORT_OTHER, EPROTO },
		{ 99, EPROTO },  /* unknown */
		{ 200, EPROTO }, /* unknown */
	};
	struct j1939_priv *priv;
	struct net_device *ndev;
	int i;

	/* Zero-init safe: netdev_warn only accesses dev->name inline array */
	ndev = kunit_kzalloc(test, sizeof(*ndev), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ndev);

	priv = kunit_kzalloc(test, sizeof(*priv), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, priv);
	priv->ndev = ndev;

	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		int err = j1939_xtp_abort_to_errno(priv, tests[i].abort_code);

		KUNIT_EXPECT_EQ_MSG(test, err, tests[i].expected_errno,
				    "abort_code=0x%02x", tests[i].abort_code);
	}
}

/* Abort code to string conversion for debug logging */

static void j1939_test_abort_to_str_all_codes(struct kunit *test)
{
	const char *str;
	const char *expected;

	str = j1939_xtp_abort_to_str(J1939_XTP_ABORT_BUSY);
	expected = "Already in one or more connection managed sessions and cannot support another.";
	KUNIT_EXPECT_STREQ(test, str, expected);

	str = j1939_xtp_abort_to_str(J1939_XTP_ABORT_RESOURCE);
	expected = "System resources were needed for another task so this connection managed session was terminated.";
	KUNIT_EXPECT_STREQ(test, str, expected);

	str = j1939_xtp_abort_to_str(J1939_XTP_ABORT_TIMEOUT);
	expected = "A timeout occurred and this is the connection abort to close the session.";
	KUNIT_EXPECT_STREQ(test, str, expected);

	str = j1939_xtp_abort_to_str(J1939_XTP_ABORT_GENERIC);
	expected = "CTS messages received when data transfer is in progress";
	KUNIT_EXPECT_STREQ(test, str, expected);

	str = j1939_xtp_abort_to_str(J1939_XTP_ABORT_FAULT);
	expected = "Maximal retransmit request limit reached";
	KUNIT_EXPECT_STREQ(test, str, expected);

	str = j1939_xtp_abort_to_str(J1939_XTP_ABORT_UNEXPECTED_DATA);
	expected = "Unexpected data transfer packet";
	KUNIT_EXPECT_STREQ(test, str, expected);

	str = j1939_xtp_abort_to_str(J1939_XTP_ABORT_BAD_SEQ);
	expected = "Bad sequence number (and software is not able to recover)";
	KUNIT_EXPECT_STREQ(test, str, expected);

	str = j1939_xtp_abort_to_str(J1939_XTP_ABORT_DUP_SEQ);
	expected = "Duplicate sequence number (and software is not able to recover)";
	KUNIT_EXPECT_STREQ(test, str, expected);

	str = j1939_xtp_abort_to_str(J1939_XTP_ABORT_OTHER);
	expected = "Any other reason (if a Connection Abort reason is identified that is not listed in the table use code 250)";
	KUNIT_EXPECT_STREQ(test, str, expected);
}

static void j1939_test_abort_to_str_unknown(struct kunit *test)
{
	const char *str;

	str = j1939_xtp_abort_to_str(99);
	KUNIT_EXPECT_STREQ(test, str, "<unknown>");

	str = j1939_xtp_abort_to_str(255);
	KUNIT_EXPECT_STREQ(test, str, "<unknown>");
}

/* Control message field extraction from 8-byte transport protocol messages */

static void j1939_test_tp_ctl_to_size(struct kunit *test)
{
	u8 dat[8];

	memset(dat, 0, sizeof(dat));
	dat[1] = 0x00;
	dat[2] = 0x00;
	KUNIT_EXPECT_EQ(test, j1939_tp_ctl_to_size_wrapper(dat), 0);

	dat[1] = 0x01;
	dat[2] = 0x00;
	KUNIT_EXPECT_EQ(test, j1939_tp_ctl_to_size_wrapper(dat), 1);

	dat[1] = 0xFF;
	dat[2] = 0x00;
	KUNIT_EXPECT_EQ(test, j1939_tp_ctl_to_size_wrapper(dat), 255);

	dat[1] = 0x00;
	dat[2] = 0x01;
	KUNIT_EXPECT_EQ(test, j1939_tp_ctl_to_size_wrapper(dat), 256);

	dat[1] = 0xF9;
	dat[2] = 0x06;
	KUNIT_EXPECT_EQ(test, j1939_tp_ctl_to_size_wrapper(dat), 1785); /* max TP */

	dat[1] = 0xFF;
	dat[2] = 0xFF;
	KUNIT_EXPECT_EQ(test, j1939_tp_ctl_to_size_wrapper(dat), 65535);
}

static void j1939_test_etp_ctl_to_size(struct kunit *test)
{
	u8 dat[8];

	memset(dat, 0, sizeof(dat));
	KUNIT_EXPECT_EQ(test, j1939_etp_ctl_to_size_wrapper(dat), 0);

	dat[1] = 0xFA;
	dat[2] = 0x06;
	dat[3] = 0x00;
	dat[4] = 0x00;
	KUNIT_EXPECT_EQ(test, j1939_etp_ctl_to_size_wrapper(dat), 1786); /* min ETP */

	dat[1] = 0x00;
	dat[2] = 0x00;
	dat[3] = 0x01;
	dat[4] = 0x00;
	KUNIT_EXPECT_EQ(test, j1939_etp_ctl_to_size_wrapper(dat), 65536);

	dat[1] = 0xF9;
	dat[2] = 0xFF;
	dat[3] = 0xFF;
	dat[4] = 0x06;
	KUNIT_EXPECT_EQ(test, j1939_etp_ctl_to_size_wrapper(dat), 117440505); /* max ETP */

	dat[1] = 0xFF;
	dat[2] = 0xFF;
	dat[3] = 0xFF;
	dat[4] = 0xFF;
	KUNIT_EXPECT_EQ(test, j1939_etp_ctl_to_size_wrapper(dat), 0xFFFFFFFF);
}

static void j1939_test_etp_ctl_to_packet(struct kunit *test)
{
	u8 dat[8];

	memset(dat, 0, sizeof(dat));
	KUNIT_EXPECT_EQ(test, j1939_etp_ctl_to_packet_wrapper(dat), 0);

	dat[2] = 0x01;
	dat[3] = 0x00;
	dat[4] = 0x00;
	KUNIT_EXPECT_EQ(test, j1939_etp_ctl_to_packet_wrapper(dat), 1);

	dat[2] = 0x00;
	dat[3] = 0x01;
	dat[4] = 0x00;
	KUNIT_EXPECT_EQ(test, j1939_etp_ctl_to_packet_wrapper(dat), 256);

	dat[2] = 0x00;
	dat[3] = 0x00;
	dat[4] = 0x01;
	KUNIT_EXPECT_EQ(test, j1939_etp_ctl_to_packet_wrapper(dat), 65536);

	dat[2] = 0xFF;
	dat[3] = 0xFF;
	dat[4] = 0xFF;
	KUNIT_EXPECT_EQ(test, j1939_etp_ctl_to_packet_wrapper(dat), 0xFFFFFF);
}

/*
 * PGN extraction: PDU1 (<0xF000) masks DA bits, PDU2 (>=0xF000) preserves
 * all bits
 */
static void j1939_test_xtp_ctl_to_pgn_pdu1(struct kunit *test)
{
	u8 dat[8];

	memset(dat, 0, sizeof(dat));
	dat[5] = 0x00;
	dat[6] = 0xEF;
	dat[7] = 0x00;
	KUNIT_EXPECT_EQ(test, j1939_xtp_ctl_to_pgn_wrapper(dat), 0xEF00);

	dat[5] = 0x25; /* DA should be masked */
	dat[6] = 0xEF;
	dat[7] = 0x00;
	KUNIT_EXPECT_EQ(test, j1939_xtp_ctl_to_pgn_wrapper(dat), 0xEF00);

	dat[5] = 0xFF;
	dat[6] = 0xEA;
	dat[7] = 0x00;
	KUNIT_EXPECT_EQ(test, j1939_xtp_ctl_to_pgn_wrapper(dat), 0xEA00);

	dat[5] = 0x00;
	dat[6] = 0x00;
	dat[7] = 0x00;
	KUNIT_EXPECT_EQ(test, j1939_xtp_ctl_to_pgn_wrapper(dat), 0x0000);
}

static void j1939_test_xtp_ctl_to_pgn_pdu2(struct kunit *test)
{
	u8 dat[8];

	memset(dat, 0, sizeof(dat));
	dat[5] = 0x00;
	dat[6] = 0xF0;
	dat[7] = 0x00;
	KUNIT_EXPECT_EQ(test, j1939_xtp_ctl_to_pgn_wrapper(dat), 0xF000);

	dat[5] = 0xCA; /* GE preserved */
	dat[6] = 0xFE;
	dat[7] = 0x00;
	KUNIT_EXPECT_EQ(test, j1939_xtp_ctl_to_pgn_wrapper(dat), 0xFECA);

	dat[5] = 0xEC;
	dat[6] = 0xFE;
	dat[7] = 0x00;
	KUNIT_EXPECT_EQ(test, j1939_xtp_ctl_to_pgn_wrapper(dat), 0xFEEC);

	dat[5] = 0xFF;
	dat[6] = 0xFF;
	dat[7] = 0x03;
	KUNIT_EXPECT_EQ(test, j1939_xtp_ctl_to_pgn_wrapper(dat), 0x3FFFF);
}

/* Broadcast detection: dst_name=0 and da=0xFF */

static void j1939_test_cb_is_broadcast_true(struct kunit *test)
{
	struct j1939_sk_buff_cb skcb = {
		.addr = {
			.dst_name = 0,
			.da = 0xFF,
		},
	};

	KUNIT_EXPECT_TRUE(test, j1939_cb_is_broadcast_wrapper(&skcb));
}

static void j1939_test_cb_is_broadcast_unicast_addr(struct kunit *test)
{
	struct j1939_sk_buff_cb skcb = {
		.addr = {
			.dst_name = 0,
			.da = 0x25,
		},
	};

	KUNIT_EXPECT_FALSE(test, j1939_cb_is_broadcast_wrapper(&skcb));
}

static void j1939_test_cb_is_broadcast_unicast_name(struct kunit *test)
{
	struct j1939_sk_buff_cb skcb = {
		.addr = {
			.dst_name = 0x1234567890ABCDEF,
			.da = 0xFF,
		},
	};

	KUNIT_EXPECT_FALSE(test, j1939_cb_is_broadcast_wrapper(&skcb));
}

/*
 * SKB control buffer swap for reply messages: swaps src<->dst addresses and
 * flags
 */
static void j1939_test_skbcb_swap_addresses(struct kunit *test)
{
	struct j1939_sk_buff_cb skcb = {
		.addr = {
			.src_name = 0x1111111111111111,
			.dst_name = 0x2222222222222222,
			.sa = 0x10,
			.da = 0x20,
		},
	};

	j1939_skbcb_swap(&skcb);

	KUNIT_EXPECT_EQ(test, skcb.addr.src_name, 0x2222222222222222ULL);
	KUNIT_EXPECT_EQ(test, skcb.addr.dst_name, 0x1111111111111111ULL);
	KUNIT_EXPECT_EQ(test, skcb.addr.sa, 0x20);
	KUNIT_EXPECT_EQ(test, skcb.addr.da, 0x10);
}

static void j1939_test_skbcb_swap_flags(struct kunit *test)
{
	struct j1939_sk_buff_cb skcb;

	skcb.flags = J1939_ECU_LOCAL_SRC;
	j1939_skbcb_swap(&skcb);
	KUNIT_EXPECT_EQ(test, skcb.flags & J1939_ECU_LOCAL_DST,
			J1939_ECU_LOCAL_DST);
	KUNIT_EXPECT_EQ(test, skcb.flags & J1939_ECU_LOCAL_SRC, 0);

	skcb.flags = J1939_ECU_LOCAL_DST;
	j1939_skbcb_swap(&skcb);
	KUNIT_EXPECT_EQ(test, skcb.flags & J1939_ECU_LOCAL_SRC,
			J1939_ECU_LOCAL_SRC);
	KUNIT_EXPECT_EQ(test, skcb.flags & J1939_ECU_LOCAL_DST, 0);

	skcb.flags = J1939_ECU_LOCAL_SRC | J1939_ECU_LOCAL_DST;
	j1939_skbcb_swap(&skcb);
	KUNIT_EXPECT_EQ(test, skcb.flags & (J1939_ECU_LOCAL_SRC |
					    J1939_ECU_LOCAL_DST),
			J1939_ECU_LOCAL_SRC | J1939_ECU_LOCAL_DST);

	skcb.flags = 0;
	j1939_skbcb_swap(&skcb);
	KUNIT_EXPECT_EQ(test,
			skcb.flags & (J1939_ECU_LOCAL_SRC |
				      J1939_ECU_LOCAL_DST), 0);
}

static void j1939_test_skbcb_swap_preserves_other_flags(struct kunit *test)
{
	struct j1939_sk_buff_cb skcb;

	skcb.flags = J1939_ECU_LOCAL_SRC | 0xF0;
	j1939_skbcb_swap(&skcb);

	KUNIT_EXPECT_EQ(test, skcb.flags & 0xF0, 0xF0);
	KUNIT_EXPECT_EQ(test, skcb.flags & J1939_ECU_LOCAL_DST,
			J1939_ECU_LOCAL_DST);
	KUNIT_EXPECT_EQ(test, skcb.flags & J1939_ECU_LOCAL_SRC, 0);
}

/* Session matching for finding active sessions: by type, name, or address */

static void j1939_test_session_match_exact(struct kunit *test)
{
	struct j1939_addr se_addr = {
		.type = J1939_TP,
		.src_name = 0x1111111111111111,
		.dst_name = 0x2222222222222222,
		.sa = 0x10,
		.da = 0x20,
	};
	struct j1939_addr sk_addr = {
		.type = J1939_TP,
		.src_name = 0x1111111111111111,
		.dst_name = 0x2222222222222222,
		.sa = 0x10,
		.da = 0x20,
	};

	KUNIT_EXPECT_TRUE(test, j1939_session_match(&se_addr, &sk_addr, false));
}

static void j1939_test_session_match_reverse(struct kunit *test)
{
	struct j1939_addr se_addr = {
		.type = J1939_TP,
		.src_name = 0x1111111111111111,
		.dst_name = 0x2222222222222222,
		.sa = 0x10,
		.da = 0x20,
	};
	struct j1939_addr sk_addr = {
		.type = J1939_TP,
		.src_name = 0x2222222222222222,
		.dst_name = 0x1111111111111111,
		.sa = 0x20,
		.da = 0x10,
	};

	KUNIT_EXPECT_TRUE(test, j1939_session_match(&se_addr, &sk_addr, true));
	KUNIT_EXPECT_FALSE(test,
			   j1939_session_match(&se_addr, &sk_addr, false));
}

static void j1939_test_session_match_type_mismatch(struct kunit *test)
{
	struct j1939_addr se_addr = {
		.type = J1939_TP,
		.src_name = 0x1111111111111111,
		.dst_name = 0x2222222222222222,
		.sa = 0x10,
		.da = 0x20,
	};
	struct j1939_addr sk_addr = {
		.type = J1939_ETP,
		.src_name = 0x1111111111111111,
		.dst_name = 0x2222222222222222,
		.sa = 0x10,
		.da = 0x20,
	};

	KUNIT_EXPECT_FALSE(test, j1939_session_match(&se_addr, &sk_addr, false));
}

/* NAME matching takes priority over address (NAME is stable, address can change) */
static void j1939_test_session_match_name_priority(struct kunit *test)
{
	struct j1939_addr se_addr = {
		.type = J1939_TP,
		.src_name = 0x1111111111111111,
		.dst_name = 0x2222222222222222,
		.sa = 0x10,
		.da = 0x20,
	};
	struct j1939_addr sk_addr = {
		.type = J1939_TP,
		.src_name = 0x1111111111111111,
		.dst_name = 0x2222222222222222,
		.sa = 0x99,
		.da = 0x88,
	};

	KUNIT_EXPECT_TRUE(test, j1939_session_match(&se_addr, &sk_addr, false));
}

static void j1939_test_session_match_addr_fallback(struct kunit *test)
{
	struct j1939_addr se_addr = {
		.type = J1939_TP,
		.src_name = 0,
		.dst_name = 0,
		.sa = 0x10,
		.da = 0x20,
	};
	struct j1939_addr sk_addr = {
		.type = J1939_TP,
		.src_name = 0,
		.dst_name = 0,
		.sa = 0x10,
		.da = 0x20,
	};

	KUNIT_EXPECT_TRUE(test, j1939_session_match(&se_addr, &sk_addr, false));
}

static void j1939_test_session_match_addr_mismatch(struct kunit *test)
{
	struct j1939_addr se_addr = {
		.type = J1939_TP,
		.src_name = 0,
		.dst_name = 0,
		.sa = 0x10,
		.da = 0x20,
	};
	struct j1939_addr sk_addr = {
		.type = J1939_TP,
		.src_name = 0,
		.dst_name = 0,
		.sa = 0x11,
		.da = 0x20,
	};

	KUNIT_EXPECT_FALSE(test, j1939_session_match(&se_addr, &sk_addr, false));
}

static struct kunit_case j1939_transport_test_cases[] = {
	KUNIT_CASE(j1939_test_abort_to_errno),

	KUNIT_CASE(j1939_test_abort_to_str_all_codes),
	KUNIT_CASE(j1939_test_abort_to_str_unknown),

	KUNIT_CASE(j1939_test_tp_ctl_to_size),
	KUNIT_CASE(j1939_test_etp_ctl_to_size),
	KUNIT_CASE(j1939_test_etp_ctl_to_packet),
	KUNIT_CASE(j1939_test_xtp_ctl_to_pgn_pdu1),
	KUNIT_CASE(j1939_test_xtp_ctl_to_pgn_pdu2),

	KUNIT_CASE(j1939_test_cb_is_broadcast_true),
	KUNIT_CASE(j1939_test_cb_is_broadcast_unicast_addr),
	KUNIT_CASE(j1939_test_cb_is_broadcast_unicast_name),

	KUNIT_CASE(j1939_test_skbcb_swap_addresses),
	KUNIT_CASE(j1939_test_skbcb_swap_flags),
	KUNIT_CASE(j1939_test_skbcb_swap_preserves_other_flags),

	KUNIT_CASE(j1939_test_session_match_exact),
	KUNIT_CASE(j1939_test_session_match_reverse),
	KUNIT_CASE(j1939_test_session_match_type_mismatch),
	KUNIT_CASE(j1939_test_session_match_name_priority),
	KUNIT_CASE(j1939_test_session_match_addr_fallback),
	KUNIT_CASE(j1939_test_session_match_addr_mismatch),

	{}
};

static struct kunit_suite j1939_transport_test_suite = {
	.name = "j1939-transport",
	.test_cases = j1939_transport_test_cases,
};

kunit_test_suite(j1939_transport_test_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit tests for J1939 transport layer");
MODULE_AUTHOR("Oleksij Rempel <kernel@pengutronix.de>");
