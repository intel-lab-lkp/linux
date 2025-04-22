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

int main(int argc, char **argv)
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
	int err = 0;
	struct ifreq ifr;

	if ((s = socket(PF_CAN, SOCK_RAW, CAN_RAW)) < 0) {
		perror("socket");
		err = 1;
		goto out;
	}

	strcpy(ifr.ifr_name, VCANIF);
	if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
		perror("SIOCGIFINDEX");
		err = 1;
		goto out_socket;
	}
	addr.can_family = AF_CAN;
	addr.can_ifindex = ifr.ifr_ifindex;

	setsockopt(s, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS,
		   &recv_own_msgs, sizeof(recv_own_msgs));

	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		err = 1;
		goto out_socket;
	}

	printf("---\n");

	for (testcase = 0; testcase < TC; testcase++) {

		rfilter.can_id   = calc_id(testcase);
		rfilter.can_mask = calc_mask(testcase);
		setsockopt(s, SOL_CAN_RAW, CAN_RAW_FILTER,
			   &rfilter, sizeof(rfilter));

		printf("testcase %2d filters : can_id = 0x%08X can_mask = 0x%08X\n",
		       testcase, rfilter.can_id, rfilter.can_mask);

		printf("testcase %2d sending patterns ... ", testcase);

		frame.can_dlc = 1;
		frame.data[0] = testcase;

		frame.can_id = ID;
		if (write(s, &frame, sizeof(frame)) < 0) {
			perror("write");
			exit(1);
		}
		frame.can_id = (ID | CAN_RTR_FLAG);
		if (write(s, &frame, sizeof(frame)) < 0) {
			perror("write");
			exit(1);
		}
		frame.can_id = (ID | CAN_EFF_FLAG);
		if (write(s, &frame, sizeof(frame)) < 0) {
			perror("write");
			exit(1);
		}
		frame.can_id = (ID | CAN_EFF_FLAG | CAN_RTR_FLAG);
		if (write(s, &frame, sizeof(frame)) < 0) {
			perror("write");
			exit(1);
		}

		printf("ok\n");

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
			if (ret < 0) {
				perror("select");
				exit(1);
			}

			if (FD_ISSET(s, &rdfs)) {
				have_rx = 1;
				ret = read(s, &frame, sizeof(struct can_frame));
				if (ret < 0) {
					perror("read");
					exit(1);
				}
				if ((frame.can_id & CAN_SFF_MASK) != ID) {
					fprintf(stderr, "received wrong can_id!\n");
					exit(1);
				}
				if (frame.data[0] != testcase) {
					fprintf(stderr, "received wrong testcase!\n");
					exit(1);
				}

				/* test & calc rxbits */
				rxbitval = 1 << ((frame.can_id & (CAN_EFF_FLAG|CAN_RTR_FLAG|CAN_ERR_FLAG)) >> 28);

				/* only receive a rxbitval once */
				if ((rxbits & rxbitval) == rxbitval) {
					fprintf(stderr, "received rxbitval %d twice!\n", rxbitval);
					exit(1);
				}
				rxbits |= rxbitval;
				rx++;

				printf("testcase %2d rx : can_id = 0x%08X rx = %d rxbits = %d\n",
				       testcase, frame.can_id, rx, rxbits);
			}
		}
		/* rx timed out -> check the received results */
		if (rx_res[testcase] != rx) {
			fprintf(stderr, "wrong rx value in testcase %d : %d (expected %d)\n",
				testcase, rx, rx_res[testcase]);
			exit(1);
		}
		if (rxbits_res[testcase] != rxbits) {
			fprintf(stderr, "wrong rxbits value in testcase %d : %d (expected %d)\n",
				testcase, rxbits, rxbits_res[testcase]);
			exit(1);
		}
		printf("testcase %2d ok\n---\n", testcase);
	}

out_socket:
	close(s);
out:
	return err;
}
