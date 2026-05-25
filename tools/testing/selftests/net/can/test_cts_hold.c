// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/if.h>

#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/can/j1939.h>


#include "kselftest_harness.h"


#define SENDER_ADDR		0x10
#define RECEIVER_ADDR	0x20

#define TEST_PGN 0xAB00
#define SENDER_TP_CM_ID		(0x18EC2010 | CAN_EFF_FLAG)
#define RECEIVER_TP_CM_ID	(0x18EC1020 | CAN_EFF_FLAG)

#define DEFAULT_RECV_TIMEOUT_MS 2000

/* Segemented payload sent by the J1939 socket*/
const uint8_t J1939_PAYLOAD[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};

/* Expected RTS payload */
const uint8_t RTS_PAYLOAD[] = {0x10, 0x0A, 0x00, 0x02, 0x02, 0x00, 0xAB, 0x00};
/* Hold payload to be sent by raw socket */
const uint8_t HOLD_PAYLOAD[] = {0x11, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0xAB, 0x00};
/* CTS to send to only allow for the transmission of one data frame */
const uint8_t CTS_1_FRAME_PAYLOAD[] = {0x11, 0x01, 0x01, 0xFF, 0xFF, 0x00, 0xAB, 0x00};
/* Resume payload to resume from connection which has been held directly after RTS*/
const uint8_t RESUME_IMMEDIATE_PAYLOAD[] = {0x11, 0x02, 0x01, 0xFF, 0xFF, 0x00, 0xAB, 0x00};
/* Resume payload to resume session which has been held after first data frame */
const uint8_t RESUME_PAYLOAD[] = {0x11, 0x01, 0x02, 0xFF, 0xFF, 0x00, 0xAB, 0x00};
/* Data payloads */
const uint8_t DATA_1_PAYLOAD[] = {0x01, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
const uint8_t DATA_2_PAYLOAD[] = {0x02, 0x07, 0x08, 0x09, 0xFF, 0xFF, 0xFF, 0xFF};

/* EOMA payload to cleanup session */
const uint8_t EOMA_PAYLOAD[] = {0x13, 0x0A, 0x00, 0x02, 0xFF, 0x00, 0xAB, 0x00};

/* Timeout payload sent on connection timeout */
const uint8_t ABORT_TIMEOUT_PAYLOAD[] = {0xFF, 0x03, 0xFF, 0xFF, 0xFF, 0x00, 0xAB, 0x00};
char CANIF[IFNAMSIZ];

static int recv_payload_timeout(int sock, const uint8_t *payload, size_t len, int timeout_ms)
{
	struct can_frame rx_frame = {};
	struct pollfd pfd = {
		.fd = sock,
		.events = POLLIN,
	};
	int ret;

	/* Wait for data to be ready to read, up to timeout_ms */
	ret = poll(&pfd, 1, timeout_ms);
	if (ret < 0) {
		perror("poll failed");
		return 1;
	}

	if (ret == 0) {
		fprintf(stderr, "timeout waiting for can raw frame\n");
		return 1;
	}

	/* Socket is readable, recv will not block */
	if (recv(sock, &rx_frame, sizeof(rx_frame), 0) < 0) {
		perror("failed to recv can raw frame");
		return 1;
	}

	if (rx_frame.len != len) {
		fprintf(stderr, "received data length does not match expected value\n");
		return 1;
	}

	if (memcmp(rx_frame.data, payload, len)) {
		fprintf(stderr, "received data does not match expected value\n");
		return 1;
	}

	return 0;
}

static int recv_payload(int sock, const uint8_t *payload, size_t len)
{
	return recv_payload_timeout(sock, payload, len, DEFAULT_RECV_TIMEOUT_MS);
}


FIXTURE(can_env)
{
	int j1939_sock;
	int raw_sock;
};

FIXTURE_SETUP(can_env)
{
	struct sockaddr_can addr = {};
	struct ifreq ifr = {};
	int ret;

	self->raw_sock = -1;
	self->j1939_sock = -1;

	self->raw_sock = socket(PF_CAN, SOCK_RAW, CAN_RAW);
	ASSERT_GE(self->raw_sock, 0)
		TH_LOG("failed to create CAN_RAW socket: %d", errno);

	strncpy(ifr.ifr_name, CANIF, sizeof(ifr.ifr_name));
	ret = ioctl(self->raw_sock, SIOCGIFINDEX, &ifr);
	ASSERT_GE(ret, 0)
		TH_LOG("failed SIOCGIFINDEX: %d", errno);


	addr.can_family = AF_CAN;
	addr.can_ifindex = ifr.ifr_ifindex;

	ret = bind(self->raw_sock, (struct sockaddr *)&addr, sizeof(addr));
	ASSERT_EQ(ret, 0)
		TH_LOG("failed bind CAN_RAW socket: %d", errno);

	self->j1939_sock = socket(PF_CAN, SOCK_DGRAM, CAN_J1939);
	ASSERT_GE(self->j1939_sock, 0)
		TH_LOG("failed to create CAN_J1939 socket: %d", errno);

	addr.can_addr.j1939.addr = SENDER_ADDR;
	addr.can_addr.j1939.name = J1939_NO_NAME;
	addr.can_addr.j1939.pgn = J1939_NO_PGN;

	ret = bind(self->j1939_sock, (struct sockaddr *)&addr, sizeof(addr));
	ASSERT_EQ(ret, 0)
		TH_LOG("failed bind CAN_J1939 socket: %d", errno);

	addr.can_addr.j1939.addr = RECEIVER_ADDR;
	addr.can_addr.j1939.pgn = TEST_PGN;
	ret = connect(self->j1939_sock, (struct sockaddr *)&addr, sizeof(addr));
	ASSERT_EQ(ret, 0)
		TH_LOG("failed connect CAN_J1939 socket: %d", errno);
}

FIXTURE_TEARDOWN(can_env)
{
	if (self->j1939_sock != -1)
		close(self->j1939_sock);

	if (self->raw_sock != -1)
		close(self->raw_sock);
}

/* Test RTS/CTS transport without hold as baseline */
TEST_F(can_env, test_no_hold)
{
	struct can_frame tx_frame = {
		.can_id = RECEIVER_TP_CM_ID,
		.len = 8,
	};

	memcpy(tx_frame.data, RESUME_IMMEDIATE_PAYLOAD, sizeof(RESUME_IMMEDIATE_PAYLOAD));

	int res = send(self->j1939_sock, J1939_PAYLOAD, sizeof(J1939_PAYLOAD), 0);

	ASSERT_GT(res, 0)
		TH_LOG("failed to send j1939 payload: %d", errno);


	res = recv_payload(self->raw_sock, RTS_PAYLOAD, sizeof(RTS_PAYLOAD));
	ASSERT_EQ(res, 0)
		TH_LOG("Failed to receive RTS as expeceted");

	res = send(self->raw_sock, &tx_frame, sizeof(tx_frame), 0);
	ASSERT_GT(res, 0)
		TH_LOG("failed to send hold with raw sock: %d", errno);

	res = recv_payload(self->raw_sock, DATA_1_PAYLOAD, sizeof(DATA_1_PAYLOAD));
	ASSERT_EQ(res, 0)
		TH_LOG("Failed to receive DATA 1 as expeceted");

	res = recv_payload(self->raw_sock, DATA_2_PAYLOAD, sizeof(DATA_2_PAYLOAD));
	ASSERT_EQ(res, 0)
		TH_LOG("Failed to receive DATA 2 as expeceted");

	memcpy(tx_frame.data, EOMA_PAYLOAD, sizeof(EOMA_PAYLOAD));
	res = send(self->raw_sock, &tx_frame, sizeof(tx_frame), 0);
	ASSERT_GT(res, 0)
		TH_LOG("failed to send EOMA with raw sock: %d", errno);
}

/* Test holding RTS/CTS transport on first frame and resuming immediatley */
TEST_F(can_env, test_hold_resume_immediate)
{
	struct can_frame tx_frame = {
		.can_id = RECEIVER_TP_CM_ID,
		.len = 8,
	};

	memcpy(tx_frame.data, HOLD_PAYLOAD, sizeof(HOLD_PAYLOAD));

	int res = send(self->j1939_sock, J1939_PAYLOAD, sizeof(J1939_PAYLOAD), 0);

	ASSERT_GT(res, 0)
		TH_LOG("failed to send j1939 payload: %d", errno);


	res = recv_payload(self->raw_sock, RTS_PAYLOAD, sizeof(RTS_PAYLOAD));
	ASSERT_EQ(res, 0)
		TH_LOG("Failed to receive RTS as expeceted");

	res = send(self->raw_sock, &tx_frame, sizeof(tx_frame), 0);
	ASSERT_GT(res, 0)
		TH_LOG("failed to send hold with raw sock: %d", errno);

	/* Wait for 300ms before sending the resume */
	usleep(300000);

	memcpy(tx_frame.data, RESUME_IMMEDIATE_PAYLOAD, sizeof(RESUME_IMMEDIATE_PAYLOAD));
	res = send(self->raw_sock, &tx_frame, sizeof(tx_frame), 0);
	ASSERT_GT(res, 0)
		TH_LOG("failed to send resume with raw sock: %d", errno);

	res = recv_payload(self->raw_sock, DATA_1_PAYLOAD, sizeof(DATA_1_PAYLOAD));
	ASSERT_EQ(res, 0)
		TH_LOG("Failed to receive DATA 1 as expeceted");

	res = recv_payload(self->raw_sock, DATA_2_PAYLOAD, sizeof(DATA_2_PAYLOAD));
	ASSERT_EQ(res, 0)
		TH_LOG("Failed to receive DATA 2 as expeceted");

	memcpy(tx_frame.data, EOMA_PAYLOAD, sizeof(EOMA_PAYLOAD));
	res = send(self->raw_sock, &tx_frame, sizeof(tx_frame), 0);
	ASSERT_GT(res, 0)
		TH_LOG("failed to send EOMA with raw sock: %d", errno);
}

/* Test send hold in transport session and resuming */
TEST_F(can_env, test_hold_resume)
{
	struct can_frame tx_frame = {
		.can_id = RECEIVER_TP_CM_ID,
		.len = 8,
	};

	memcpy(tx_frame.data, CTS_1_FRAME_PAYLOAD, sizeof(CTS_1_FRAME_PAYLOAD));

	int res = send(self->j1939_sock, J1939_PAYLOAD, sizeof(J1939_PAYLOAD), 0);

	ASSERT_GT(res, 0)
		TH_LOG("failed to send j1939 payload: %d", errno);

	res = recv_payload(self->raw_sock, RTS_PAYLOAD, sizeof(RTS_PAYLOAD));
	ASSERT_EQ(res, 0)
		TH_LOG("Failed to receive RTS as expeceted");

	res = send(self->raw_sock, &tx_frame, sizeof(tx_frame), 0);
	ASSERT_GT(res, 0)
		TH_LOG("failed to send cts(1) with raw sock: %d", errno);

	res = recv_payload(self->raw_sock, DATA_1_PAYLOAD, sizeof(DATA_1_PAYLOAD));
	ASSERT_EQ(res, 0)
		TH_LOG("Failed to receive RTS as expeceted");

	memcpy(tx_frame.data, HOLD_PAYLOAD, sizeof(HOLD_PAYLOAD));
	res = send(self->raw_sock, &tx_frame, sizeof(tx_frame), 0);
	ASSERT_GT(res, 0)
		TH_LOG("failed to send hold with raw sock: %d", errno);

	/* Wait for 300ms before sending the resume */
	usleep(300000);

	memcpy(tx_frame.data, RESUME_PAYLOAD, sizeof(RESUME_PAYLOAD));
	res = send(self->raw_sock, &tx_frame, sizeof(tx_frame), 0);
	ASSERT_GT(res, 0)
		TH_LOG("failed to send resume with raw sock: %d", errno);

	res = recv_payload(self->raw_sock, DATA_2_PAYLOAD, sizeof(DATA_2_PAYLOAD));
	ASSERT_EQ(res, 0)
		TH_LOG("Failed to receive DATA 2 as expeceted");

	memcpy(tx_frame.data, EOMA_PAYLOAD, sizeof(EOMA_PAYLOAD));
	res = send(self->raw_sock, &tx_frame, sizeof(tx_frame), 0);
	ASSERT_GT(res, 0)
		TH_LOG("failed to send EOMA with raw sock: %d", errno);
}

/* Test timeout after not resuming hold */
TEST_F(can_env, test_hold_timeout)
{
	struct can_frame tx_frame = {
		.can_id = RECEIVER_TP_CM_ID,
		.len = 8,
	};
	struct timespec start, end;
	long elapsed_ms;
	int res;

	memcpy(tx_frame.data, HOLD_PAYLOAD, sizeof(HOLD_PAYLOAD));
	res = send(self->j1939_sock, J1939_PAYLOAD, sizeof(J1939_PAYLOAD), 0);
	ASSERT_GT(res, 0)
		TH_LOG("failed to send j1939 payload: %d", errno);

	res = recv_payload(self->raw_sock, RTS_PAYLOAD, sizeof(RTS_PAYLOAD));
	ASSERT_EQ(res, 0)
		TH_LOG("Failed to receive RTS as expected");

	res = send(self->raw_sock, &tx_frame, sizeof(tx_frame), 0);
	ASSERT_GT(res, 0)
		TH_LOG("failed to send hold with raw sock: %d", errno);

	/* Record start time */
	clock_gettime(CLOCK_MONOTONIC, &start);

	/*
	 * Receive with a timeout larger than the expected 1050ms J1939 timeout.
	 * 2000ms provides plenty of headroom for CI without hanging indefinitely.
	 */
	res = recv_payload_timeout(self->raw_sock, ABORT_TIMEOUT_PAYLOAD,
				   sizeof(ABORT_TIMEOUT_PAYLOAD), 2000);

	ASSERT_EQ(res, 0)
		TH_LOG("Failed to receive abort as expected");

	/* Record end time and calculate elapsed milliseconds */
	clock_gettime(CLOCK_MONOTONIC, &end);
	elapsed_ms = (end.tv_sec - start.tv_sec) * 1000 +
		     (end.tv_nsec - start.tv_nsec) / 1000000;

	/*
	 * The actual timeout is 1050ms. We define an acceptable window
	 * to account for CI scheduling variations.
	 */
	ASSERT_GE(elapsed_ms, 1000)
		TH_LOG("Abort received too early: %ld ms", elapsed_ms);
	ASSERT_LE(elapsed_ms, 1500)
		TH_LOG("Abort received too late: %ld ms", elapsed_ms);
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
