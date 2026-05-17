// SPDX-License-Identifier: GPL-2.0
/*
 * UDP loopback round-trip prober.  Wrapped by killswitch_test.sh with
 * an IPsec ESP SA + policy pair on loopback, this demonstrates that
 * engaging a killswitch on esp_input drops inbound ESP packets before
 * decapsulation, mitigating CVE-2026-43284 ("Dirty Frag", upstream fix
 * xfrm: esp: avoid in-place decrypt on shared skb frags).
 *
 * The binary itself knows nothing about ESP -- it sends one UDP
 * datagram to itself and waits up to a second for delivery.
 *
 * Exit codes:
 *   0  UDP round-trip succeeded (no mitigation in effect)
 *   1  UDP recv timed out (mitigation engaged)
 *   2  setup error -> SKIP
 *
 * Copyright (C) 2026 Sasha Levin <sashal@kernel.org>
 */

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define UDP_PORT 53435
#define PROBE    "ks-43284-probe"

int main(void)
{
	struct sockaddr_in addr = {
		.sin_family      = AF_INET,
		.sin_port        = htons(UDP_PORT),
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
	char buf[64];
	int sk;
	ssize_t n;

	sk = socket(AF_INET, SOCK_DGRAM, 0);
	if (sk < 0) {
		fprintf(stderr, "socket: %s -- skip\n", strerror(errno));
		return 2;
	}
	if (bind(sk, (struct sockaddr *)&addr, sizeof(addr))) {
		fprintf(stderr, "bind: %s -- skip\n", strerror(errno));
		close(sk);
		return 2;
	}
	if (setsockopt(sk, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv))) {
		fprintf(stderr, "SO_RCVTIMEO: %s -- skip\n", strerror(errno));
		close(sk);
		return 2;
	}

	if (sendto(sk, PROBE, sizeof(PROBE) - 1, 0,
		   (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "sendto: %s -- skip\n", strerror(errno));
		close(sk);
		return 2;
	}

	memset(buf, 0, sizeof(buf));
	n = recvfrom(sk, buf, sizeof(buf), 0, NULL, NULL);
	close(sk);

	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			fprintf(stderr,
				"recvfrom: timeout -- mitigation engaged?\n");
			return 1;
		}
		fprintf(stderr, "recvfrom: %s\n", strerror(errno));
		return 2;
	}
	if (n != (ssize_t)(sizeof(PROBE) - 1) ||
	    memcmp(buf, PROBE, sizeof(PROBE) - 1)) {
		fprintf(stderr, "recvfrom: bad payload (%zd bytes)\n", n);
		return 2;
	}

	printf("UDP round-trip OK (%zd bytes)\n", n);
	return 0;
}
