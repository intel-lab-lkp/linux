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

#include "../../kselftest_harness.h"

#define ID 0x123
#define TC 18 /* # of testcases */

const int rx_res[TC] = {4, 4, 4, 4, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1};
const int rxbits_res[TC] = {4369, 4369, 4369, 4369, 17, 4352, 17, 4352, 257, 257, 4112, 4112, 1, 256, 16, 4096, 1, 256};

#define VCANIF "vcan0"

canid_t calc_id(int testcase)
{
	canid_t id = ID;

	if (testcase & 1)
		id |= CAN_EFF_FLAG;
	if (testcase & 2)
		id |= CAN_RTR_FLAG;

	return id;
}

canid_t calc_mask(int testcase)
{
	canid_t mask = CAN_SFF_MASK;

	if (testcase > 15)
		return (CAN_EFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG);

	if (testcase & 4)
		mask |= CAN_EFF_FLAG;
	if (testcase & 8)
		mask |= CAN_RTR_FLAG;

	return mask;
}

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

TEST(can_filter)
{
	fd_set rdfs;
	struct timeval tv;
	int s;
	struct sockaddr_can addr;
	struct can_filter rfilter;
	struct can_frame frame;
	int testcase;
	int have_rx;
	int rx;
	int rxbits, rxbitval;
	int ret;
	int recv_own_msgs = 1;
	struct ifreq ifr;

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

	for (testcase = 0; testcase < TC; testcase++) {

		rfilter.can_id   = calc_id(testcase);
		rfilter.can_mask = calc_mask(testcase);
		setsockopt(s, SOL_CAN_RAW, CAN_RAW_FILTER,
			   &rfilter, sizeof(rfilter));

		TH_LOG("testcase %2d filters : can_id = 0x%08X can_mask = 0x%08X",
		       testcase, rfilter.can_id, rfilter.can_mask);

		TH_LOG("testcase %2d sending patterns...", testcase);

		ret = send_can_frames(s, testcase);
		ASSERT_EQ(0, ret)
			TH_LOG("failed to send CAN frames");

		have_rx = 1;
		rx = 0;
		rxbits = 0;

		while (have_rx) {

			have_rx = 0;
			FD_ZERO(&rdfs);
			FD_SET(s, &rdfs);
			tv.tv_sec = 0;
			tv.tv_usec = 50000; /* 50ms timeout */

			ret = select(s+1, &rdfs, NULL, NULL, &tv);
			ASSERT_LE(0, ret)
				TH_LOG("failed select for frame %d (%d)", rx, errno);

			if (FD_ISSET(s, &rdfs)) {
				have_rx = 1;
				ret = read(s, &frame, sizeof(struct can_frame));
				ASSERT_LE(0, ret)
					TH_LOG("failed to read frame %d (%d)", rx, errno);

				ASSERT_EQ(ID, frame.can_id & CAN_SFF_MASK)
					TH_LOG("received wrong can_id");
				ASSERT_EQ(testcase, frame.data[0])
					TH_LOG("received wrong test case");

				/* test & calc rxbits */
				rxbitval = 1 << ((frame.can_id & (CAN_EFF_FLAG|CAN_RTR_FLAG|CAN_ERR_FLAG)) >> 28);

				/* only receive a rxbitval once */
				ASSERT_NE(rxbitval, rxbits & rxbitval)
					TH_LOG("received rxbitval %d twice", rxbitval);
				rxbits |= rxbitval;
				rx++;

				TH_LOG("testcase %2d rx : can_id = 0x%08X rx = %d rxbits = %d",
				       testcase, frame.can_id, rx, rxbits);
			}
		}
		/* rx timed out -> check the received results */
		ASSERT_EQ(rx_res[testcase], rx)
			TH_LOG("wrong number of received frames %d", testcase);
		ASSERT_EQ(rxbits_res[testcase], rxbits)
			TH_LOG("wrong rxbits value in testcase %d", testcase);

		TH_LOG("testcase %2d ok", testcase);
		TH_LOG("---");
	}

	close(s);
	return;
}

TEST_HARNESS_MAIN
