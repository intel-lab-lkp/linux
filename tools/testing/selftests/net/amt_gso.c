// SPDX-License-Identifier: GPL-2.0
/*
 * Helper for amt_gso.sh.
 *
 * send:  send datagrams to a multicast group, optionally as one UDP_SEGMENT
 *        burst per datagram, with a self-describing payload pattern.
 * recv:  receive datagrams on a UDP port and check count, length and payload
 *        of every one of them against the pattern used by "send".
 * sniff: capture on an interface with AF_PACKET and report how large the
 *        frames handed to the device were.
 *
 * Every datagram consists of cnt chunks of seg bytes followed by an optional
 * tail chunk of tail bytes. A chunk starts with a struct chunk_hdr and the
 * rest of it is a function of (datagram number, chunk number, offset).
 * With UDP_SEGMENT set to seg, every chunk is one segment on the wire.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <error.h>
#include <limits.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef UDP_SEGMENT
#define UDP_SEGMENT	103
#endif

#define MAX_DGRAM	65507
#define AMT_MSG_MCAST_DATA	6

struct chunk_hdr {
	uint32_t seq;
	uint16_t idx;
	uint16_t len;
};

struct cfg {
	bool ipv6;
	bool gso;
	bool amt;		/* sniff: count AMT multicast data messages */
	const char *ifname;
	const char *group;
	const char *bind_addr;
	unsigned int port;
	unsigned int seg;
	unsigned int cnt;
	unsigned int tail;
	unsigned int num;
	unsigned int delay_us;
	unsigned int idle;
	unsigned int grace;	/* recv: extra wait for the first datagram */
	unsigned int mtu;
};

static uint8_t fill_byte(uint32_t seq, unsigned int idx, unsigned int off)
{
	return (uint8_t)(seq * 131 + idx * 17 + off * 7 + (off >> 8));
}

static unsigned int chunk_len(const struct cfg *c, unsigned int idx)
{
	return idx < c->cnt ? c->seg : c->tail;
}

static unsigned int chunks_per_dgram(const struct cfg *c)
{
	return c->cnt + (c->tail ? 1 : 0);
}

static void fill_dgram(const struct cfg *c, uint32_t seq, uint8_t *buf)
{
	unsigned int i, off, n = chunks_per_dgram(c);

	for (i = 0; i < n; i++) {
		unsigned int len = chunk_len(c, i);
		struct chunk_hdr h = { .seq = seq, .idx = i, .len = len };

		memcpy(buf, &h, sizeof(h));
		for (off = sizeof(h); off < len; off++)
			buf[off] = fill_byte(seq, i, off);
		buf += len;
	}
}

static int open_udp(const struct cfg *c)
{
	int fd = socket(c->ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, 0);

	if (fd < 0)
		error(2, errno, "socket");
	return fd;
}

static void fill_addr(const struct cfg *c, const char *str, unsigned int port,
		      struct sockaddr_storage *ss, socklen_t *len)
{
	memset(ss, 0, sizeof(*ss));
	if (c->ipv6) {
		struct sockaddr_in6 *a = (void *)ss;

		a->sin6_family = AF_INET6;
		a->sin6_port = htons(port);
		if (inet_pton(AF_INET6, str, &a->sin6_addr) != 1)
			error(2, errno, "inet_pton");
		*len = sizeof(*a);
	} else {
		struct sockaddr_in *a = (void *)ss;

		a->sin_family = AF_INET;
		a->sin_port = htons(port);
		if (inet_pton(AF_INET, str, &a->sin_addr) != 1)
			error(2, errno, "inet_pton");
		*len = sizeof(*a);
	}
}

static int do_send(const struct cfg *c)
{
	static uint8_t buf[MAX_DGRAM];
	struct sockaddr_storage dst, src;
	socklen_t dlen, slen;
	unsigned int total, ifindex, i;
	int fd, ttl = 8, zero = 0;

	total = c->cnt * c->seg + c->tail;
	if (total > MAX_DGRAM ||
	    (c->tail && c->tail < sizeof(struct chunk_hdr)) ||
	    c->seg < sizeof(struct chunk_hdr))
		error(2, 0, "bad sizes");
	ifindex = if_nametoindex(c->ifname);
	if (!ifindex)
		error(2, errno, "if_nametoindex");

	fd = open_udp(c);
	if (c->bind_addr) {
		fill_addr(c, c->bind_addr, 0, &src, &slen);
		if (bind(fd, (void *)&src, slen))
			error(2, errno, "bind");
	}
	if (c->ipv6) {
		if (setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &ifindex,
			       sizeof(ifindex)))
			error(2, errno, "IPV6_MULTICAST_IF");
		if (setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &ttl,
			       sizeof(ttl)))
			error(2, errno, "IPV6_MULTICAST_HOPS");
		if (setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &zero,
			       sizeof(zero)))
			error(2, errno, "IPV6_MULTICAST_LOOP");
	} else {
		struct ip_mreqn mr = { .imr_ifindex = ifindex };

		if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &mr,
			       sizeof(mr)))
			error(2, errno, "IP_MULTICAST_IF");
		if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl,
			       sizeof(ttl)))
			error(2, errno, "IP_MULTICAST_TTL");
		if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &zero,
			       sizeof(zero)))
			error(2, errno, "IP_MULTICAST_LOOP");
	}
	if (c->gso) {
		int gso = c->seg;

		if (setsockopt(fd, IPPROTO_UDP, UDP_SEGMENT, &gso, sizeof(gso)))
			error(2, errno, "UDP_SEGMENT");
	}
	fill_addr(c, c->group, c->port, &dst, &dlen);

	for (i = 0; i < c->num; i++) {
		fill_dgram(c, i, buf);
		if (sendto(fd, buf, total, 0, (void *)&dst, dlen) !=
		    (ssize_t)total)
			error(2, errno, "sendto");
		if (c->delay_us)
			usleep(c->delay_us);
	}
	close(fd);
	return 0;
}

static int do_recv(const struct cfg *c)
{
	unsigned int per = chunks_per_dgram(c), expected = c->num * per;
	unsigned int good = 0, bad_len = 0, bad_payload = 0, dup = 0, unk = 0;
	static uint8_t buf[MAX_DGRAM + 1];
	struct sockaddr_storage any;
	socklen_t alen;
	uint8_t *seen;
	int fd, rcvbuf = 8 << 20, idle_ms = 0;
	bool got_any = false;

	seen = calloc(expected ? expected : 1, 1);
	if (!seen)
		error(2, errno, "calloc");
	fd = open_udp(c);
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
	fill_addr(c, c->ipv6 ? "::" : "0.0.0.0", c->port, &any, &alen);
	if (bind(fd, (void *)&any, alen))
		error(2, errno, "bind");
	printf("READY\n");
	fflush(stdout);

	while (good < expected) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		struct chunk_hdr h;
		unsigned int off;
		ssize_t n;
		int r = poll(&pfd, 1, 100);

		if (r < 0)
			error(2, errno, "poll");
		if (!r) {
			idle_ms += 100;
			if (idle_ms >=
			    (int)(c->idle + (got_any ? 0 : c->grace)) * 1000)
				break;
			continue;
		}
		idle_ms = 0;
		got_any = true;
		n = recv(fd, buf, sizeof(buf), 0);
		if (n < 0)
			error(2, errno, "recv");
		if (n < (ssize_t)sizeof(h)) {
			bad_len++;
			continue;
		}
		memcpy(&h, buf, sizeof(h));
		if (h.seq >= c->num || h.idx >= per) {
			unk++;
			continue;
		}
		if ((unsigned int)n != chunk_len(c, h.idx) || h.len != n) {
			bad_len++;
			continue;
		}
		for (off = sizeof(h); off < (unsigned int)n; off++)
			if (buf[off] != fill_byte(h.seq, h.idx, off))
				break;
		if (off != (unsigned int)n) {
			bad_payload++;
			continue;
		}
		if (seen[h.seq * per + h.idx]++) {
			dup++;
			continue;
		}
		good++;
	}
	printf("RECV expected=%u good=%u bad_len=%u ", expected, good, bad_len);
	printf("bad_payload=%u dup=%u unknown=%u\n", bad_payload, dup, unk);
	free(seen);
	return good == expected && !bad_len && !bad_payload && !dup &&
	       !unk ? 0 : 1;
}

static int sniff_stop;

static void sniff_sig(int sig)
{
	__atomic_store_n(&sniff_stop, 1, __ATOMIC_RELAXED);
}

static int do_sniff(const struct cfg *c)
{
	static uint8_t buf[MAX_DGRAM + 256];
	unsigned int n = 0, max_len = 0, over = 0, amt = 0;
	struct sockaddr_ll sll = { .sll_family = AF_PACKET };
	struct timespec t0, last, now;
	struct tpacket_stats st = { 0 };
	socklen_t stlen = sizeof(st);
	int fd, rcvbuf = 32 << 20;

	fd = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_ALL));
	if (fd < 0)
		error(2, errno, "socket(AF_PACKET)");
	sll.sll_protocol = htons(ETH_P_ALL);
	sll.sll_ifindex = if_nametoindex(c->ifname);
	if (!sll.sll_ifindex)
		error(2, errno, "if_nametoindex");
	if (bind(fd, (void *)&sll, sizeof(sll)))
		error(2, errno, "bind(AF_PACKET)");
	/* Do not lose frames to a full receive queue: they are counted */
	if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf, sizeof(rcvbuf)))
		setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
	signal(SIGTERM, sniff_sig);
	printf("READY\n");
	fflush(stdout);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	last = t0;

	for (;;) {
		/* Once asked to stop, read what is still queued, then report */
		int stopping = __atomic_load_n(&sniff_stop, __ATOMIC_RELAXED);
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		struct sockaddr_ll from;
		socklen_t flen = sizeof(from);
		unsigned int hl, dport;
		const uint8_t *p = buf;
		ssize_t len, got;
		int r;

		clock_gettime(CLOCK_MONOTONIC, &now);
		if (!stopping && (now.tv_sec - t0.tv_sec > 120 ||
				  now.tv_sec - last.tv_sec >= (long)c->idle))
			break;
		r = poll(&pfd, 1, stopping ? 0 : 100);
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0)
			error(2, errno, "poll");
		if (!r) {
			if (stopping)
				break;
			continue;
		}
		got = recvfrom(fd, buf, sizeof(buf), MSG_TRUNC,
			       (void *)&from, &flen);
		if (got < 0)
			error(2, errno, "recvfrom");
		len = got;
		if (got > (ssize_t)sizeof(buf))
			got = sizeof(buf);

		if (ntohs(from.sll_protocol) == ETH_P_IP && got >= 28 &&
		    (p[0] >> 4) == 4 && p[9] == IPPROTO_UDP) {
			hl = (p[0] & 0xf) * 4;
		} else if (ntohs(from.sll_protocol) == ETH_P_IPV6 &&
			   got >= 48 && (p[0] >> 4) == 6 &&
			   p[6] == IPPROTO_UDP) {
			hl = 40;
		} else {
			continue;
		}
		if (got < (ssize_t)(hl + sizeof(struct udphdr)))
			continue;
		dport = (p[hl + 2] << 8) | p[hl + 3];
		if (dport != c->port)
			continue;
		if (c->amt) {
			const uint8_t *a = p + hl + sizeof(struct udphdr);

			/* version 0, type 6, a reserved byte, the IP packet */
			if (got < (ssize_t)(hl + sizeof(struct udphdr) + 3) ||
			    a[0] != AMT_MSG_MCAST_DATA || a[1] != 0)
				continue;
			if ((a[2] >> 4) != (c->ipv6 ? 6 : 4))
				continue;
			amt++;
		}
		last = now;
		n++;
		if ((unsigned int)len > max_len)
			max_len = len;
		if ((unsigned int)len > c->mtu)
			over++;
	}
	getsockopt(fd, SOL_PACKET, PACKET_STATISTICS, &st, &stlen);
	printf("SNIFF frames=%u max_len=%u over_mtu=%u amt_data=%u lost=%u\n",
	       n, max_len, over, amt, st.tp_drops);
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"amt_gso send|recv|sniff [-6] [-I ifname] [-g group] [-b srcaddr]\n"
		"           [-p port] [-s seg] [-c chunks] [-t tail] [-n datagrams]\n"
		"           [-G] [-d delay_us] [-T idle_s] [-S grace_s] [-M mtu] [-a]\n");
	exit(2);
}

int main(int argc, char **argv)
{
	struct cfg c = { .port = 4000, .seg = 1200, .cnt = 8, .tail = 100,
			 .num = 1, .idle = 2, .mtu = 1500 };
	int opt;

	if (argc < 2)
		usage();
	optind = 2;
	while ((opt = getopt(argc, argv,
			     "6I:g:b:p:s:c:t:n:Gd:T:S:M:a")) != -1) {
		switch (opt) {
		case '6':
			c.ipv6 = true;
			break;
		case 'I':
			c.ifname = optarg;
			break;
		case 'g':
			c.group = optarg;
			break;
		case 'b':
			c.bind_addr = optarg;
			break;
		case 'p':
			c.port = atoi(optarg);
			break;
		case 's':
			c.seg = atoi(optarg);
			break;
		case 'c':
			c.cnt = atoi(optarg);
			break;
		case 't':
			c.tail = atoi(optarg);
			break;
		case 'n':
			c.num = atoi(optarg);
			break;
		case 'G':
			c.gso = true;
			break;
		case 'd':
			c.delay_us = atoi(optarg);
			break;
		case 'S':
			c.grace = atoi(optarg);
			break;
		case 'T':
			c.idle = atoi(optarg);
			break;
		case 'M':
			c.mtu = atoi(optarg);
			break;
		case 'a':
			c.amt = true;
			break;
		default:
			usage();
		}
	}
	if (c.seg > MAX_DGRAM || c.tail > MAX_DGRAM ||
	    (c.seg && c.cnt > (MAX_DGRAM - c.tail) / c.seg) ||
	    (c.num && chunks_per_dgram(&c) > UINT_MAX / c.num))
		error(2, 0, "bad sizes");
	if (!strcmp(argv[1], "send") && c.ifname && c.group)
		return do_send(&c);
	if (!strcmp(argv[1], "recv"))
		return do_recv(&c);
	if (!strcmp(argv[1], "sniff") && c.ifname)
		return do_sniff(&c);
	usage();
	return 2;
}
