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

#include <linux/can.h>
#include <linux/can/raw.h>

#define TH_LOG_ENABLED 0
#include "../../kselftest_harness.h"

#define ID 0x123

#define VCANIF "vcan0"

int send_can_frames(int sock, int testcase)
{
	struct can_frame frame;

	frame.can_dlc = 1;
	frame.data[0] = testcase;

	frame.can_id = ID;
	if (write(sock, &frame, sizeof(frame)) < 0) {
		perror("write");
		return 1;
	}
	frame.can_id = (ID | CAN_RTR_FLAG);
	if (write(sock, &frame, sizeof(frame)) < 0) {
		perror("write");
		return 1;
	}
	frame.can_id = (ID | CAN_EFF_FLAG);
	if (write(sock, &frame, sizeof(frame)) < 0) {
		perror("write");
		return 1;
	}
	frame.can_id = (ID | CAN_EFF_FLAG | CAN_RTR_FLAG);
	if (write(sock, &frame, sizeof(frame)) < 0) {
		perror("write");
		return 1;
	}

	return 0;
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
	ASSERT_LT(0, s)
		TH_LOG("failed to create CAN_RAW socket (%d)", errno);

	strcpy(ifr.ifr_name, VCANIF);
	ret = ioctl(s, SIOCGIFINDEX, &ifr);
	ASSERT_LE(0, ret)
		TH_LOG("failed SIOCGIFINDEX (%d)", errno);

	addr.can_family = AF_CAN;
	addr.can_ifindex = ifr.ifr_ifindex;

	setsockopt(s, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS,
		   &recv_own_msgs, sizeof(recv_own_msgs));

	ret = bind(s, (struct sockaddr *)&addr, sizeof(addr));
	ASSERT_EQ(0, ret)
		TH_LOG("failed bind socket (%d)", errno);

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
	int exp_rxbits;
};

FIXTURE_VARIANT_ADD(can_filters, base) {
	.testcase = 1,
	.id = ID,
	.mask = CAN_SFF_MASK,
	.exp_num_rx = 4,
	.exp_rxbits = 4369,
};
FIXTURE_VARIANT_ADD(can_filters, base_eff) {
	.testcase = 2,
	.id = ID | CAN_EFF_FLAG,
	.mask = CAN_SFF_MASK,
	.exp_num_rx = 4,
	.exp_rxbits = 4369,
};
FIXTURE_VARIANT_ADD(can_filters, base_rtr) {
	.testcase = 3,
	.id = ID | CAN_RTR_FLAG,
	.mask = CAN_SFF_MASK,
	.exp_num_rx = 4,
	.exp_rxbits = 4369,
};
FIXTURE_VARIANT_ADD(can_filters, base_effrtr) {
	.testcase = 4,
	.id = ID | CAN_EFF_FLAG | CAN_RTR_FLAG,
	.mask = CAN_SFF_MASK,
	.exp_num_rx = 4,
	.exp_rxbits = 4369,
};

FIXTURE_VARIANT_ADD(can_filters, filter_eff) {
	.testcase = 5,
	.id = ID,
	.mask = CAN_SFF_MASK | CAN_EFF_FLAG,
	.exp_num_rx = 2,
	.exp_rxbits = 17,
};
FIXTURE_VARIANT_ADD(can_filters, filter_eff_eff) {
	.testcase = 6,
	.id = ID | CAN_EFF_FLAG,
	.mask = CAN_SFF_MASK | CAN_EFF_FLAG,
	.exp_num_rx = 2,
	.exp_rxbits = 4352,
};
FIXTURE_VARIANT_ADD(can_filters, filter_eff_rtr) {
	.testcase = 7,
	.id = ID | CAN_RTR_FLAG,
	.mask = CAN_SFF_MASK | CAN_EFF_FLAG,
	.exp_num_rx = 2,
	.exp_rxbits = 17,
};
FIXTURE_VARIANT_ADD(can_filters, filter_eff_effrtr) {
	.testcase = 8,
	.id = ID | CAN_EFF_FLAG | CAN_RTR_FLAG,
	.mask = CAN_SFF_MASK | CAN_EFF_FLAG,
	.exp_num_rx = 2,
	.exp_rxbits = 4352,
};

FIXTURE_VARIANT_ADD(can_filters, filter_rtr) {
	.testcase = 9,
	.id = ID,
	.mask = CAN_SFF_MASK | CAN_RTR_FLAG,
	.exp_num_rx = 2,
	.exp_rxbits = 257,
};
FIXTURE_VARIANT_ADD(can_filters, filter_rtr_eff) {
	.testcase = 10,
	.id = ID | CAN_EFF_FLAG,
	.mask = CAN_SFF_MASK | CAN_RTR_FLAG,
	.exp_num_rx = 2,
	.exp_rxbits = 257,
};
FIXTURE_VARIANT_ADD(can_filters, filter_rtr_rtr) {
	.testcase = 11,
	.id = ID | CAN_RTR_FLAG,
	.mask = CAN_SFF_MASK | CAN_RTR_FLAG,
	.exp_num_rx = 2,
	.exp_rxbits = 4112,
};
FIXTURE_VARIANT_ADD(can_filters, filter_rtr_effrtr) {
	.testcase = 12,
	.id = ID | CAN_EFF_FLAG | CAN_RTR_FLAG,
	.mask = CAN_SFF_MASK | CAN_RTR_FLAG,
	.exp_num_rx = 2,
	.exp_rxbits = 4112,
};

FIXTURE_VARIANT_ADD(can_filters, filter_effrtr) {
	.testcase = 13,
	.id = ID,
	.mask = CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG,
	.exp_num_rx = 1,
	.exp_rxbits = 1,
};
FIXTURE_VARIANT_ADD(can_filters, filter_effrtr_eff) {
	.testcase = 14,
	.id = ID | CAN_EFF_FLAG,
	.mask = CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG,
	.exp_num_rx = 1,
	.exp_rxbits = 256,
};
FIXTURE_VARIANT_ADD(can_filters, filter_effrtr_rtr) {
	.testcase = 15,
	.id = ID | CAN_RTR_FLAG,
	.mask = CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG,
	.exp_num_rx = 1,
	.exp_rxbits = 16,
};
FIXTURE_VARIANT_ADD(can_filters, filter_effrtr_effrtr) {
	.testcase = 16,
	.id = ID | CAN_EFF_FLAG | CAN_RTR_FLAG,
	.mask = CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG,
	.exp_num_rx = 1,
	.exp_rxbits = 4096,
};

FIXTURE_VARIANT_ADD(can_filters, eff) {
	.testcase = 17,
	.id = ID,
	.mask = CAN_EFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG,
	.exp_num_rx = 1,
	.exp_rxbits = 1,
};
FIXTURE_VARIANT_ADD(can_filters, eff_eff) {
	.testcase = 18,
	.id = ID | CAN_EFF_FLAG,
	.mask = CAN_EFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG,
	.exp_num_rx = 1,
	.exp_rxbits = 256,
};

TEST_F(can_filters, test_filter)
{
	fd_set rdfs;
	struct timeval tv;
	struct can_filter rfilter;
	struct can_frame frame;
	int have_rx;
	int rx;
	int rxbits, rxbitval;
	int ret;

	rfilter.can_id = variant->id;
	rfilter.can_mask = variant->mask;
	setsockopt(self->sock, SOL_CAN_RAW, CAN_RAW_FILTER,
		   &rfilter, sizeof(rfilter));

	TH_LOG("filters: can_id = 0x%08X can_mask = 0x%08X",
		rfilter.can_id, rfilter.can_mask);

	ret = send_can_frames(self->sock, variant->testcase);
	ASSERT_EQ(0, ret)
		TH_LOG("failed to send CAN frames");

	rx = 0;
	rxbits = 0;

	do {
		have_rx = 0;
		FD_ZERO(&rdfs);
		FD_SET(self->sock, &rdfs);
		tv.tv_sec = 0;
		tv.tv_usec = 50000; /* 50ms timeout */

		ret = select(self->sock + 1, &rdfs, NULL, NULL, &tv);
		ASSERT_LE(0, ret)
			TH_LOG("failed select for frame %d (%d)", rx, errno);

		if (FD_ISSET(self->sock, &rdfs)) {
			have_rx = 1;
			ret = read(self->sock, &frame, sizeof(struct can_frame));
			ASSERT_LE(0, ret)
				TH_LOG("failed to read frame %d (%d)", rx, errno);

			ASSERT_EQ(ID, frame.can_id & CAN_SFF_MASK)
				TH_LOG("received wrong can_id");
			ASSERT_EQ(variant->testcase, frame.data[0])
				TH_LOG("received wrong test case");

			/* test & calc rxbits */
			rxbitval = 1 << ((frame.can_id & (CAN_EFF_FLAG|CAN_RTR_FLAG|CAN_ERR_FLAG)) >> 28);

			/* only receive a rxbitval once */
			ASSERT_NE(rxbitval, rxbits & rxbitval)
				TH_LOG("received rxbitval %d twice", rxbitval);
			rxbits |= rxbitval;
			rx++;

			TH_LOG("rx: can_id = 0x%08X rx = %d rxbits = %d",
			       frame.can_id, rx, rxbits);
		}
	} while (have_rx);

	/* rx timed out -> check the received results */
	ASSERT_EQ(variant->exp_num_rx, rx)
		TH_LOG("wrong number of received frames");
	ASSERT_EQ(variant->exp_rxbits, rxbits)
		TH_LOG("wrong rxbits value");
}

TEST_HARNESS_MAIN
