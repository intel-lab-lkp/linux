// SPDX-License-Identifier: GPL-2.0
/*  IPXLAT - Stateless IP/ICMP Translation (SIIT) virtual device driver
 *
 *  Copyright (C) 2026- Mandelbit SRL
 *  Copyright (C) 2026- Daniel Gröber <dxld@darkboxed.org>
 *
 *  Author:	Antonio Quartulli <antonio@mandelbit.com>
 *		Daniel Gröber <dxld@darkboxed.org>
 *		Ralf Lici <ralf@mandelbit.com>
 */

#include <arpa/inet.h>
#include <errno.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static uint16_t iphdr_csum(const void *buf, size_t len)
{
	const uint16_t *p = buf;
	uint32_t sum = 0;

	while (len > 1) {
		sum += *p++;
		len -= 2;
	}
	if (len)
		sum += *(const uint8_t *)p;

	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);

	return (uint16_t)~sum;
}

int main(int argc, char **argv)
{
	static const char payload[] = "ipxlat-zero-udp-csum";
	struct sockaddr_in dst = {};
	struct {
		struct iphdr ip;
		struct udphdr udp;
		char payload[sizeof(payload)];
	} pkt = {};
	in_addr_t saddr, daddr;
	unsigned long dport_ul;
	socklen_t dst_len;
	ssize_t n;
	int one = 1;
	int fd;

	if (argc != 4) {
		fprintf(stderr, "usage: %s <src4> <dst4> <dport>\n", argv[0]);
		return 2;
	}

	if (!inet_pton(AF_INET, argv[1], &saddr) ||
	    !inet_pton(AF_INET, argv[2], &daddr)) {
		fprintf(stderr, "invalid IPv4 address\n");
		return 2;
	}

	errno = 0;
	dport_ul = strtoul(argv[3], NULL, 10);
	if (errno || dport_ul > 65535) {
		fprintf(stderr, "invalid UDP port\n");
		return 2;
	}

	fd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
	if (fd < 0) {
		perror("socket");
		return 1;
	}

	if (setsockopt(fd, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one)) < 0) {
		perror("setsockopt(IP_HDRINCL)");
		close(fd);
		return 1;
	}

	pkt.ip.version = 4;
	pkt.ip.ihl = 5;
	pkt.ip.ttl = 64;
	pkt.ip.protocol = IPPROTO_UDP;
	pkt.ip.tot_len = htons(sizeof(pkt));
	pkt.ip.id = htons(1);
	pkt.ip.frag_off = 0;
	pkt.ip.saddr = saddr;
	pkt.ip.daddr = daddr;
	pkt.ip.check = iphdr_csum(&pkt.ip, sizeof(pkt.ip));

	pkt.udp.source = htons(4242);
	pkt.udp.dest = htons((uint16_t)dport_ul);
	pkt.udp.len = htons(sizeof(pkt.udp) + sizeof(payload));
	pkt.udp.check = 0;

	memcpy(pkt.payload, payload, sizeof(payload));

	dst.sin_family = AF_INET;
	dst.sin_port = pkt.udp.dest;
	dst.sin_addr.s_addr = daddr;
	dst_len = sizeof(dst);

	n = sendto(fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&dst, dst_len);
	if (n != (ssize_t)sizeof(pkt)) {
		perror("sendto");
		close(fd);
		return 1;
	}

	close(fd);
	return 0;
}
