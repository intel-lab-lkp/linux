// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
/*
 * Copyright (c) 2011 Volkswagen Group Electronic Research
 * All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <net/if.h>
#include <linux/if.h>

#include <errno.h>
#include <stddef.h>

#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/types.h>

#include "kselftest_harness.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

#define ID 0x123

char CANIF[IFNAMSIZ];

static int send_can_frames(int sock, int testcase)
{
	struct can_frame frame;

	frame.can_dlc = 1;
	frame.data[0] = testcase;

	frame.can_id = ID;
	if (write(sock, &frame, sizeof(frame)) < 0)
		goto write_err;

	frame.can_id = (ID | CAN_RTR_FLAG);
	if (write(sock, &frame, sizeof(frame)) < 0)
		goto write_err;

	frame.can_id = (ID | CAN_EFF_FLAG);
	if (write(sock, &frame, sizeof(frame)) < 0)
		goto write_err;

	frame.can_id = (ID | CAN_EFF_FLAG | CAN_RTR_FLAG);
	if (write(sock, &frame, sizeof(frame)) < 0)
		goto write_err;

	return 0;

write_err:
	perror("write");
	return 1;
}

FIXTURE(can_filters) {
	int sock;
};

FIXTURE_SETUP(can_filters)
{
	struct sockaddr_can addr;
	struct ifreq ifr;
	int recv_own_msgs = 1;
	int s, ret;

	s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
	ASSERT_GE(s, 0)
		TH_LOG("failed to create CAN_RAW socket: %d", errno);

	strncpy(ifr.ifr_name, CANIF, sizeof(ifr.ifr_name));
	ret = ioctl(s, SIOCGIFINDEX, &ifr);
	ASSERT_GE(ret, 0)
		TH_LOG("failed SIOCGIFINDEX: %d", errno);

	addr.can_family = AF_CAN;
	addr.can_ifindex = ifr.ifr_ifindex;

	setsockopt(s, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS,
		   &recv_own_msgs, sizeof(recv_own_msgs));

	ret = bind(s, (struct sockaddr *)&addr, sizeof(addr));
	ASSERT_EQ(ret, 0)
		TH_LOG("failed bind socket: %d", errno);

	self->sock = s;
}

FIXTURE_TEARDOWN(can_filters)
{
	close(self->sock);
}

FIXTURE_VARIANT(can_filters) {
	int testcase;
	canid_t id;
	canid_t mask;
	int exp_num_rx;
	canid_t exp_flags[];
};

/* Receive all frames when filtering for the ID in standard frame format */
FIXTURE_VARIANT_ADD(can_filters, base) {
	.testcase = 1,
	.id = ID,
	.mask = CAN_SFF_MASK,
	.exp_num_rx = 4,
	.exp_flags = {
		0,
		CAN_RTR_FLAG,
		CAN_EFF_FLAG,
		CAN_EFF_FLAG | CAN_RTR_FLAG,
	},
};

/* Ignore EFF flag in filter ID if not covered by filter mask */
FIXTURE_VARIANT_ADD(can_filters, base_eff) {
	.testcase = 2,
	.id = ID | CAN_EFF_FLAG,
	.mask = CAN_SFF_MASK,
	.exp_num_rx = 4,
	.exp_flags = {
		0,
		CAN_RTR_FLAG,
		CAN_EFF_FLAG,
		CAN_EFF_FLAG | CAN_RTR_FLAG,
	},
};

/* Ignore RTR flag in filter ID if not covered by filter mask */
FIXTURE_VARIANT_ADD(can_filters, base_rtr) {
	.testcase = 3,
	.id = ID | CAN_RTR_FLAG,
	.mask = CAN_SFF_MASK,
	.exp_num_rx = 4,
	.exp_flags = {
		0,
		CAN_RTR_FLAG,
		CAN_EFF_FLAG,
		CAN_EFF_FLAG | CAN_RTR_FLAG,
	},
};

/* Ignore EFF and RTR flags in filter ID if not covered by filter mask */
FIXTURE_VARIANT_ADD(can_filters, base_effrtr) {
	.testcase = 4,
	.id = ID | CAN_EFF_FLAG | CAN_RTR_FLAG,
	.mask = CAN_SFF_MASK,
	.exp_num_rx = 4,
	.exp_flags = {
		0,
		CAN_RTR_FLAG,
		CAN_EFF_FLAG,
		CAN_EFF_FLAG | CAN_RTR_FLAG,
	},
};

/* Receive only SFF frames when expecting no EFF flag */
FIXTURE_VARIANT_ADD(can_filters, filter_eff) {
	.testcase = 5,
	.id = ID,
	.mask = CAN_SFF_MASK | CAN_EFF_FLAG,
	.exp_num_rx = 2,
	.exp_flags = {
		0,
		CAN_RTR_FLAG,
	},
};

/* Receive only EFF frames when filter id and filter mask include EFF flag */
FIXTURE_VARIANT_ADD(can_filters, filter_eff_eff) {
	.testcase = 6,
	.id = ID | CAN_EFF_FLAG,
	.mask = CAN_SFF_MASK | CAN_EFF_FLAG,
	.exp_num_rx = 2,
	.exp_flags = {
		CAN_EFF_FLAG,
		CAN_EFF_FLAG | CAN_RTR_FLAG,
	},
};

/* Receive only SFF frames when expecting no EFF flag, ignoring RTR flag */
FIXTURE_VARIANT_ADD(can_filters, filter_eff_rtr) {
	.testcase = 7,
	.id = ID | CAN_RTR_FLAG,
	.mask = CAN_SFF_MASK | CAN_EFF_FLAG,
	.exp_num_rx = 2,
	.exp_flags = {
		0,
		CAN_RTR_FLAG,
	},
};

/* Receive only EFF frames when filter id and filter mask include EFF flag,
 * ignoring RTR flag
 */
FIXTURE_VARIANT_ADD(can_filters, filter_eff_effrtr) {
	.testcase = 8,
	.id = ID | CAN_EFF_FLAG | CAN_RTR_FLAG,
	.mask = CAN_SFF_MASK | CAN_EFF_FLAG,
	.exp_num_rx = 2,
	.exp_flags = {
		CAN_EFF_FLAG,
		CAN_EFF_FLAG | CAN_RTR_FLAG,
	},
};

/* Receive no remote frames when filtering for no RTR flag */
FIXTURE_VARIANT_ADD(can_filters, filter_rtr) {
	.testcase = 9,
	.id = ID,
	.mask = CAN_SFF_MASK | CAN_RTR_FLAG,
	.exp_num_rx = 2,
	.exp_flags = {
		0,
		CAN_EFF_FLAG,
	},
};

/* Receive no remote frames when filtering for no RTR flag, ignoring EFF flag */
FIXTURE_VARIANT_ADD(can_filters, filter_rtr_eff) {
	.testcase = 10,
	.id = ID | CAN_EFF_FLAG,
	.mask = CAN_SFF_MASK | CAN_RTR_FLAG,
	.exp_num_rx = 2,
	.exp_flags = {
		0,
		CAN_EFF_FLAG,
	},
};

/* Receive only remote frames when filter includes RTR flag */
FIXTURE_VARIANT_ADD(can_filters, filter_rtr_rtr) {
	.testcase = 11,
	.id = ID | CAN_RTR_FLAG,
	.mask = CAN_SFF_MASK | CAN_RTR_FLAG,
	.exp_num_rx = 2,
	.exp_flags = {
		CAN_RTR_FLAG,
		CAN_EFF_FLAG | CAN_RTR_FLAG,
	},
};

/* Receive only remote frames when filter includes RTR flag, ignoring EFF
 * flag
 */
FIXTURE_VARIANT_ADD(can_filters, filter_rtr_effrtr) {
	.testcase = 12,
	.id = ID | CAN_EFF_FLAG | CAN_RTR_FLAG,
	.mask = CAN_SFF_MASK | CAN_RTR_FLAG,
	.exp_num_rx = 2,
	.exp_flags = {
		CAN_RTR_FLAG,
		CAN_EFF_FLAG | CAN_RTR_FLAG,
	},
};

/* Receive only SFF data frame when filtering for no flags */
FIXTURE_VARIANT_ADD(can_filters, filter_effrtr) {
	.testcase = 13,
	.id = ID,
	.mask = CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG,
	.exp_num_rx = 1,
	.exp_flags = {
		0,
	},
};

/* Receive only EFF data frame when filtering for EFF but no RTR flag */
FIXTURE_VARIANT_ADD(can_filters, filter_effrtr_eff) {
	.testcase = 14,
	.id = ID | CAN_EFF_FLAG,
	.mask = CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG,
	.exp_num_rx = 1,
	.exp_flags = {
		CAN_EFF_FLAG,
	},
};

/* Receive only SFF remote frame when filtering for RTR but no EFF flag */
FIXTURE_VARIANT_ADD(can_filters, filter_effrtr_rtr) {
	.testcase = 15,
	.id = ID | CAN_RTR_FLAG,
	.mask = CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG,
	.exp_num_rx = 1,
	.exp_flags = {
		CAN_RTR_FLAG,
	},
};

/* Receive only EFF remote frame when filtering for EFF and RTR flag */
FIXTURE_VARIANT_ADD(can_filters, filter_effrtr_effrtr) {
	.testcase = 16,
	.id = ID | CAN_EFF_FLAG | CAN_RTR_FLAG,
	.mask = CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG,
	.exp_num_rx = 1,
	.exp_flags = {
		CAN_EFF_FLAG | CAN_RTR_FLAG,
	},
};

/* Receive only SFF data frame when filtering for no EFF flag and no RTR flag
 * but based on EFF mask
 */
FIXTURE_VARIANT_ADD(can_filters, eff) {
	.testcase = 17,
	.id = ID,
	.mask = CAN_EFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG,
	.exp_num_rx = 1,
	.exp_flags = {
		0,
	},
};

/* Receive only EFF data frame when filtering for EFF flag and no RTR flag but
 * based on EFF mask
 */
FIXTURE_VARIANT_ADD(can_filters, eff_eff) {
	.testcase = 18,
	.id = ID | CAN_EFF_FLAG,
	.mask = CAN_EFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG,
	.exp_num_rx = 1,
	.exp_flags = {
		CAN_EFF_FLAG,
	},
};

/* This test verifies that the raw CAN filters work, by checking if only frames
 * with the expected set of flags are received. For each test case, the given
 * filter (id and mask) is added and four CAN frames are sent with every
 * combination of set/unset EFF/RTR flags.
 */
TEST_F(can_filters, test_filter)
{
	struct can_filter rfilter;
	int ret;

	rfilter.can_id = variant->id;
	rfilter.can_mask = variant->mask;
	setsockopt(self->sock, SOL_CAN_RAW, CAN_RAW_FILTER,
		   &rfilter, sizeof(rfilter));

	TH_LOG("filters: can_id = 0x%08X can_mask = 0x%08X",
		rfilter.can_id, rfilter.can_mask);

	ret = send_can_frames(self->sock, variant->testcase);
	ASSERT_EQ(ret, 0)
		TH_LOG("failed to send CAN frames");

	for (int i = 0; i <= variant->exp_num_rx; i++) {
		struct can_frame frame;
		struct timeval tv = {
			.tv_sec = 0,
			.tv_usec = 50000, /* 50ms timeout */
		};
		fd_set rdfs;

		FD_ZERO(&rdfs);
		FD_SET(self->sock, &rdfs);

		ret = select(self->sock + 1, &rdfs, NULL, NULL, &tv);
		ASSERT_GE(ret, 0)
			TH_LOG("failed select for frame %d, err: %d)", i, errno);

		ret = FD_ISSET(self->sock, &rdfs);
		if (i == variant->exp_num_rx) {
			ASSERT_EQ(ret, 0)
				TH_LOG("too many frames received");
		} else {
			ASSERT_NE(ret, 0)
				TH_LOG("too few frames received");

			ret = read(self->sock, &frame, sizeof(frame));
			ASSERT_GE(ret, 0)
				TH_LOG("failed to read frame %d, err: %d", i, errno);

			TH_LOG("rx: can_id = 0x%08X rx = %d", frame.can_id, i);

			ASSERT_EQ(ID, frame.can_id & CAN_SFF_MASK)
				TH_LOG("received wrong can_id");
			ASSERT_EQ(variant->testcase, frame.data[0])
				TH_LOG("received wrong test case");

			ASSERT_EQ(frame.can_id & ~CAN_ERR_MASK,
				  variant->exp_flags[i])
				TH_LOG("received unexpected flags");
		}
	}
}

/* --- CAN XL --- */

#define XLF 0x80

/* CAN XL requires an interface whose MTU is large enough; a classic CAN or
 * CAN FD only interface cannot carry CAN XL frames.
 */
static bool can_iface_supports_xl(void)
{
	struct ifreq ifr;
	int s, ret;

	s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
	if (s < 0)
		return false;

	strncpy(ifr.ifr_name, CANIF, sizeof(ifr.ifr_name));
	ret = ioctl(s, SIOCGIFMTU, &ifr);
	close(s);

	if (ret < 0)
		return false;

	return ifr.ifr_mtu >= CANXL_MIN_MTU;
}

static int open_xl_socket(int send_xl, const struct can_raw_vcid_options *vcid)
{
	struct sockaddr_can addr;
	struct ifreq ifr;
	int one = 1;
	int s, ret;

	s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
	if (s < 0)
		return -1;

	strncpy(ifr.ifr_name, CANIF, sizeof(ifr.ifr_name));
	ret = ioctl(s, SIOCGIFINDEX, &ifr);
	if (ret < 0)
		goto close_sock;

	addr.can_family = AF_CAN;
	addr.can_ifindex = ifr.ifr_ifindex;

	ret = bind(s, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0)
		goto close_sock;

	ret = setsockopt(s, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &one, sizeof(one));
	if (ret < 0)
		goto close_sock;

	if (vcid) {
		ret = setsockopt(s, SOL_CAN_RAW, CAN_RAW_XL_VCID_OPTS, vcid,
				 sizeof(*vcid));
		if (ret < 0)
			goto close_sock;
	}

	ret = setsockopt(s, SOL_CAN_RAW, CAN_RAW_XL_FRAMES, &send_xl,
			 sizeof(send_xl));
	if (ret < 0)
		goto close_sock;

	return s;

close_sock:
	close(s);
	return -1;
}

static void fill_canxl(struct canxl_frame *cxl, unsigned int len, __u8 sdt)
{
	int i;

	memset(cxl, 0, sizeof(*cxl));
	cxl->prio = ID;
	cxl->flags = XLF;
	cxl->sdt = sdt;
	cxl->len = len;
	cxl->af = 0xdeadbeef;

	for (i = 0; i < len; i++)
		cxl->data[i] = i & 0xff;
}

/* Wait up to 200ms for a frame. Returns the frame size, or -1 if nothing
 * arrived. A blocking read() cannot be used to assert that a frame was
 * filtered out, as it would simply wait forever.
 */
static int recv_timeout(int sock, void *buf, size_t len)
{
	struct timeval tv = {
		.tv_sec = 0,
		.tv_usec = 200000,
	};
	fd_set rdfs;
	int ret;

	FD_ZERO(&rdfs);
	FD_SET(sock, &rdfs);

	ret = select(sock + 1, &rdfs, NULL, NULL, &tv);
	if (ret <= 0)
		return -1;

	return read(sock, buf, len);
}

FIXTURE(canxl) {
	int sock;
};

FIXTURE_SETUP(canxl)
{
	self->sock = open_xl_socket(1, NULL);
}

FIXTURE_TEARDOWN(canxl)
{
	if (self->sock >= 0)
		close(self->sock);
}

/* Round-trip payload lengths, including the maximum CAN XL payload */
TEST_F(canxl, payload_roundtrip)
{
	const unsigned int lens[] = { 1, 8, 64, 2048 };
	struct canxl_frame tx;

	if (!can_iface_supports_xl())
		SKIP(return, "%s does not support CAN XL", CANIF);

	ASSERT_GE(self->sock, 0)
		TH_LOG("failed to open CAN XL socket on %s", CANIF);

	for (unsigned int i = 0; i < ARRAY_SIZE(lens); i++) {
		unsigned int len = lens[i];
		struct canxl_frame rx;
		int ret;

		fill_canxl(&tx, len, 0x11);

		ret = write(self->sock, &tx, CANXL_HDR_SIZE + len);
		ASSERT_EQ(ret, CANXL_HDR_SIZE + len)
			TH_LOG("failed to send CAN XL frame of len %u", len);

		ret = read(self->sock, &rx, sizeof(rx));
		ASSERT_EQ(ret, CANXL_HDR_SIZE + len)
			TH_LOG("failed to receive CAN XL frame of len %u", len);

		ASSERT_EQ(rx.len, len)
			TH_LOG("wrong payload length in received frame");
		ASSERT_EQ(rx.sdt, tx.sdt)
			TH_LOG("wrong SDU type in received frame");
		ASSERT_EQ(rx.af, tx.af)
			TH_LOG("wrong acceptance field in received frame");
		ASSERT_EQ(rx.prio, tx.prio)
			TH_LOG("wrong priority in received frame");
		ASSERT_EQ(memcmp(rx.data, tx.data, len), 0)
			TH_LOG("payload corrupted");
	}
}

/* A CAN XL frame must not be accepted when CAN_RAW_XL_FRAMES is off */
TEST(canxl_send_requires_xl_frames)
{
	struct canxl_frame tx;
	int s, ret;

	/* a plain socket that did not enable CAN XL */
	s = open_xl_socket(0, NULL);
	ASSERT_GE(s, 0)
		TH_LOG("failed to open CAN socket on %s", CANIF);

	fill_canxl(&tx, 8, 0x11);

	ret = write(s, &tx, CANXL_HDR_SIZE + tx.len);
	ASSERT_LT(ret, 0)
		TH_LOG("CAN XL frame accepted without CAN_RAW_XL_FRAMES");
	ASSERT_EQ(errno, EINVAL);

	close(s);
}

/* A frame without the mandatory CANXL_XLF flag must be rejected */
TEST(canxl_requires_xlf_flag)
{
	struct canxl_frame tx;
	int s, ret;

	s = open_xl_socket(1, NULL);
	if (!can_iface_supports_xl())
		SKIP(return, "%s does not support CAN XL", CANIF);

	ASSERT_GE(s, 0)
		TH_LOG("failed to open CAN XL socket on %s", CANIF);

	fill_canxl(&tx, 8, 0x11);
	tx.flags = 0;

	ret = write(s, &tx, CANXL_HDR_SIZE + tx.len);
	ASSERT_LT(ret, 0)
		TH_LOG("frame without CANXL_XLF accepted");
	ASSERT_EQ(errno, EINVAL);

	close(s);
}

/* The declared length must match the payload actually written */
TEST(canxl_declared_len_must_match_payload)
{
	struct canxl_frame tx;
	int s, ret;

	s = open_xl_socket(1, NULL);
	if (!can_iface_supports_xl())
		SKIP(return, "%s does not support CAN XL", CANIF);

	ASSERT_GE(s, 0)
		TH_LOG("failed to open CAN XL socket on %s", CANIF);

	fill_canxl(&tx, 8, 0x11);
	tx.len = 100;

	ret = write(s, &tx, CANXL_HDR_SIZE + 8);
	ASSERT_LT(ret, 0)
		TH_LOG("frame with mismatching length accepted");
	ASSERT_EQ(errno, EINVAL);

	close(s);
}

/* Enabling CAN XL must implicitly enable CAN FD, and it must not be turnable
 * off again while CAN XL stays enabled
 */
TEST(canxl_enables_fd_and_cannot_be_disabled)
{
	struct canfd_frame tx;
	struct canxl_frame rx;
	int s, ret;
	int zero = 0;

	s = open_xl_socket(1, NULL);
	if (!can_iface_supports_xl())
		SKIP(return, "%s does not support CAN XL", CANIF);

	ASSERT_GE(s, 0)
		TH_LOG("failed to open CAN XL socket on %s", CANIF);

	ret = setsockopt(s, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &zero, sizeof(zero));
	ASSERT_LT(ret, 0)
		TH_LOG("CAN FD could be disabled while CAN XL was enabled");
	ASSERT_EQ(errno, EINVAL);

	memset(&tx, 0, sizeof(tx));
	tx.can_id = ID;
	tx.len = 8;

	ret = write(s, &tx, CANFD_MTU);
	ASSERT_EQ(ret, CANFD_MTU)
		TH_LOG("CAN FD not implicitly enabled by CAN XL");

	ret = read(s, &rx, sizeof(rx));
	ASSERT_EQ(ret, CANFD_MTU)
		TH_LOG("CAN FD frame not delivered to a CAN XL socket");

	close(s);
}

/* Without VCID options the VCID is stripped on TX and a VCID-tagged frame is
 * not forwarded on RX
 */
TEST(canxl_vcid_stripped_by_default)
{
	struct can_raw_vcid_options pass = {
		.flags = CAN_RAW_XL_VCID_TX_PASS,
	};
	struct canxl_frame tx, rx;
	int tx_sock, rx_sock, ret;

	tx_sock = open_xl_socket(1, &pass);
	ASSERT_GE(tx_sock, 0)
		TH_LOG("CAN XL not supported on %s", CANIF);

	rx_sock = open_xl_socket(1, NULL);
	ASSERT_GE(rx_sock, 0);

	fill_canxl(&tx, 8, 0x11);
	tx.prio |= 0x42 << CANXL_VCID_OFFSET;

	ret = write(tx_sock, &tx, CANXL_HDR_SIZE + tx.len);
	ASSERT_EQ(ret, CANXL_HDR_SIZE + tx.len);

	/* the RX socket has no VCID filter, so it drops VCID tagged frames */
	ret = recv_timeout(rx_sock, &rx, sizeof(rx));
	ASSERT_LT(ret, 0)
		TH_LOG("VCID tagged frame forwarded without CAN_RAW_XL_VCID_RX_FILTER");

	close(tx_sock);
	close(rx_sock);
}

/* CAN_RAW_XL_VCID_TX_SET overwrites the VCID, and the RX side can select it */
TEST(canxl_vcid_set_and_filter)
{
	struct can_raw_vcid_options opts = {
		.flags = CAN_RAW_XL_VCID_TX_SET,
		.tx_vcid = 0x77,
	};
	struct can_raw_vcid_options filter = {
		.flags = CAN_RAW_XL_VCID_RX_FILTER,
		.rx_vcid = 0x77,
		.rx_vcid_mask = 0xff,
	};
	struct canxl_frame tx, rx;
	int tx_sock, rx_sock, ret;

	tx_sock = open_xl_socket(1, &opts);
	if (!can_iface_supports_xl())
		SKIP(return, "%s does not support CAN XL", CANIF);

	ASSERT_GE(tx_sock, 0)
		TH_LOG("failed to open CAN XL VCID socket on %s", CANIF);

	rx_sock = open_xl_socket(1, &filter);
	ASSERT_GE(rx_sock, 0);

	fill_canxl(&tx, 8, 0x11);
	tx.prio |= 0x42 << CANXL_VCID_OFFSET;

	ret = write(tx_sock, &tx, CANXL_HDR_SIZE + tx.len);
	ASSERT_EQ(ret, CANXL_HDR_SIZE + tx.len);

	ret = read(rx_sock, &rx, sizeof(rx));
	ASSERT_EQ(ret, CANXL_HDR_SIZE + 8);

	ASSERT_EQ((rx.prio & CANXL_VCID_MASK) >> CANXL_VCID_OFFSET, 0x77)
		TH_LOG("CAN_RAW_XL_VCID_TX_SET did not overwrite the VCID");
	ASSERT_EQ(rx.prio & CANXL_PRIO_MASK, ID)
		TH_LOG("priority clobbered by the VCID option");

	close(tx_sock);
	close(rx_sock);
}

/* A non-matching VCID filter must not forward the frame */
TEST(canxl_vcid_filter_mismatch)
{
	struct can_raw_vcid_options pass = {
		.flags = CAN_RAW_XL_VCID_TX_PASS,
	};
	struct can_raw_vcid_options filter = {
		.flags = CAN_RAW_XL_VCID_RX_FILTER,
		.rx_vcid = 0x99,
		.rx_vcid_mask = 0xff,
	};
	struct canxl_frame tx, rx;
	int tx_sock, rx_sock, ret;

	tx_sock = open_xl_socket(1, &pass);
	if (!can_iface_supports_xl())
		SKIP(return, "%s does not support CAN XL", CANIF);

	ASSERT_GE(tx_sock, 0)
		TH_LOG("failed to open CAN XL VCID socket on %s", CANIF);

	rx_sock = open_xl_socket(1, &filter);
	ASSERT_GE(rx_sock, 0);

	fill_canxl(&tx, 8, 0x11);
	tx.prio |= 0x42 << CANXL_VCID_OFFSET;

	ret = write(tx_sock, &tx, CANXL_HDR_SIZE + tx.len);
	ASSERT_EQ(ret, CANXL_HDR_SIZE + tx.len);

	ret = recv_timeout(rx_sock, &rx, sizeof(rx));
	ASSERT_LT(ret, 0)
		TH_LOG("frame with non-matching VCID was forwarded");

	close(tx_sock);
	close(rx_sock);
}

int main(int argc, char **argv)
{
	char *ifname = getenv("CANIF");

	if (!ifname) {
		printf("CANIF environment variable must contain the test interface\n");
		return KSFT_FAIL;
	}

	strncpy(CANIF, ifname, sizeof(CANIF) - 1);

	return test_harness_run(argc, argv);
}
