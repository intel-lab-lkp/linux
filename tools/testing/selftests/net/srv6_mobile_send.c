// SPDX-License-Identifier: GPL-2.0
/*
 * Helper for SRv6 Mobile (RFC 9433) selftests.
 *
 * Usage: srv6_mobile_send <src-addr> <dst-addr>
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/ip6.h>
#include <netinet/icmp6.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* RFC 8200 Routing header common fields are 4 bytes; an additional
 * 4 bytes of type-specific data follow (the Reserved field for the
 * deprecated type 0, or first_segment/flags/tag for SRH type 4).  The
 * segment list then runs in 16-byte units, giving a total of 24 bytes
 * for one segment -- which is what ip6r_len = 2 advertises.
 */
struct srh {
	struct ip6_rthdr rthdr;
	uint32_t type_data;
	struct in6_addr segments[];
};

#define SRH_ONE_SEG_LEN (sizeof(struct srh) + sizeof(struct in6_addr))

static uint16_t csum_fold(uint32_t sum)
{
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return ~sum;
}

static uint32_t csum_partial(const void *buf, size_t len, uint32_t sum)
{
	const uint16_t *p = buf;

	while (len > 1) {
		sum += *p++;
		len -= 2;
	}
	if (len)
		sum += *(const uint8_t *)p;
	return sum;
}

static uint16_t icmpv6_checksum(const struct in6_addr *src,
				const struct in6_addr *dst,
				const void *payload, size_t len)
{
	uint32_t nexthdr = htonl(IPPROTO_ICMPV6);
	uint32_t plen = htonl(len);
	uint32_t sum;

	sum = csum_partial(src, sizeof(*src), 0);
	sum = csum_partial(dst, sizeof(*dst), sum);
	sum = csum_partial(&plen, sizeof(plen), sum);
	sum = csum_partial(&nexthdr, sizeof(nexthdr), sum);
	sum = csum_partial(payload, len, sum);
	return csum_fold(sum);
}

int main(int argc, char **argv)
{
	uint8_t frame[sizeof(struct ip6_hdr) + SRH_ONE_SEG_LEN +
		      sizeof(struct icmp6_hdr)];
	struct sockaddr_in6 dst_addr = {};
	struct icmp6_hdr *icmp6;
	struct ip6_hdr *ip6;
	struct srh *srh;
	ssize_t res;
	int fd;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <src-addr> <dst-addr>\n", argv[0]);
		return 1;
	}

	memset(frame, 0, sizeof(frame));
	ip6 = (struct ip6_hdr *)frame;
	srh = (struct srh *)(frame + sizeof(*ip6));
	icmp6 = (struct icmp6_hdr *)(frame + sizeof(*ip6) + SRH_ONE_SEG_LEN);

	ip6->ip6_flow = htonl(6u << 28);
	ip6->ip6_plen = htons(SRH_ONE_SEG_LEN + sizeof(*icmp6));
	ip6->ip6_nxt = IPPROTO_ROUTING;
	ip6->ip6_hops = 64;
	if (inet_pton(AF_INET6, argv[1], &ip6->ip6_src) != 1) {
		fprintf(stderr, "invalid src %s\n", argv[1]);
		return 1;
	}
	if (inet_pton(AF_INET6, argv[2], &ip6->ip6_dst) != 1) {
		fprintf(stderr, "invalid dst %s\n", argv[2]);
		return 1;
	}

	srh->rthdr.ip6r_nxt = IPPROTO_ICMPV6;
	srh->rthdr.ip6r_len = 2;		/* (1 + ip6r_len) * 8 = 24 */
	srh->rthdr.ip6r_type = 0;		/* RFC 8754: SRH is type 4 */
	srh->rthdr.ip6r_segleft = 0;
	srh->segments[0] = ip6->ip6_dst;

	icmp6->icmp6_type = ICMP6_ECHO_REQUEST;
	icmp6->icmp6_code = 0;
	icmp6->icmp6_cksum = 0;
	icmp6->icmp6_dataun.icmp6_un_data16[0] = htons(0x1234);
	icmp6->icmp6_dataun.icmp6_un_data16[1] = htons(1);
	icmp6->icmp6_cksum =
		icmpv6_checksum(&ip6->ip6_src, &ip6->ip6_dst,
				icmp6, sizeof(*icmp6));

	fd = socket(AF_INET6, SOCK_RAW, IPPROTO_RAW);
	if (fd < 0) {
		perror("socket");
		return 1;
	}

	dst_addr.sin6_family = AF_INET6;
	dst_addr.sin6_addr = ip6->ip6_dst;

	res = sendto(fd, frame, sizeof(frame), 0,
		     (struct sockaddr *)&dst_addr, sizeof(dst_addr));
	if (res != (ssize_t)sizeof(frame)) {
		perror("sendto");
		close(fd);
		return 1;
	}

	close(fd);
	return 0;
}
