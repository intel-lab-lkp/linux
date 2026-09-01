// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Yuya Kusakabe (yuya.kusakabe@gmail.com)
 *
 * Helper for SRv6 Mobile (RFC 9433) selftests.
 *
 * Usage:
 *   srv6_mobile_send -m end-map -s <src> -d <dst> [--rh-type N] [--bad-srh]
 */

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <netinet/ip6.h>
#include <netinet/icmp6.h>
#include <stdbool.h>
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
struct srh_one_seg {
	struct ip6_rthdr rthdr;
	uint32_t type_data;
	struct in6_addr segment;
};

/* Built as a struct so the headers are written through typed, aligned
 * members rather than casts of a byte array; static_assert keeps it
 * exactly the on-wire layout.
 */
struct end_map_frame {
	struct ip6_hdr		ip6;
	struct srh_one_seg	srh;
	struct icmp6_hdr	icmp6;
};

static_assert(sizeof(struct end_map_frame) ==
	      sizeof(struct ip6_hdr) + sizeof(struct srh_one_seg) +
	      sizeof(struct icmp6_hdr),
	      "end_map_frame must not contain padding");

enum mode {
	MODE_NONE,
	MODE_END_MAP,
};

struct cfg {
	enum mode	mode;
	struct in6_addr	src6;
	struct in6_addr	dst6;
	uint8_t		rh_type;
	bool		bad_srh;
};

static void usage(const char *bin)
{
	fprintf(stderr,
"Usage: %s -m <mode> -s <src> -d <dst> [opts]\n"
"\n"
"Modes:\n"
"  end-map    Send IPv6 + SRH + ICMPv6 echo for End.MAP testing\n"
"\n"
"Mode end-map options:\n"
"  --rh-type <n>       Routing Header type (default 4, the SRH)\n"
"  --bad-srh           emit an SRH whose Last Entry exceeds its length (drop test)\n"
"\n"
"Exit: 0 sent, 1 failure, 3 usage error.\n",
		bin);
}

static int parse_u32(const char *s, uint32_t *out)
{
	unsigned long v;
	char *end;

	errno = 0;
	v = strtoul(s, &end, 0);
	if (errno || !*s || *end || v > 0xffffffffUL)
		return -1;
	*out = (uint32_t)v;
	return 0;
}

static int parse_u8(const char *s, uint8_t *out)
{
	uint32_t v;

	if (parse_u32(s, &v) || v > 0xff)
		return -1;
	*out = (uint8_t)v;
	return 0;
}

static enum mode parse_mode(const char *s)
{
	if (!strcmp(s, "end-map"))
		return MODE_END_MAP;
	return MODE_NONE;
}

static int parse_args(int argc, char **argv, struct cfg *cfg)
{
	enum { OPT_RH_TYPE = 256, OPT_BAD_SRH };
	static const struct option longopts[] = {
		{ "rh-type",         required_argument, NULL, OPT_RH_TYPE },
		{ "bad-srh",         no_argument, NULL, OPT_BAD_SRH },
		{ NULL, 0, NULL, 0 },
	};
	int c;

	cfg->rh_type = 4;	/* RFC 8754: the SRH is Routing Header type 4 */
	while ((c = getopt_long(argc, argv, "m:s:d:", longopts, NULL))
	       != -1) {
		switch (c) {
		case 'm':
			cfg->mode = parse_mode(optarg);
			break;
		case 's':
			if (inet_pton(AF_INET6, optarg, &cfg->src6) != 1)
				return -1;
			break;
		case 'd':
			if (inet_pton(AF_INET6, optarg, &cfg->dst6) != 1)
				return -1;
			break;
		case OPT_RH_TYPE:
			if (parse_u8(optarg, &cfg->rh_type))
				return -1;
			break;
		case OPT_BAD_SRH:
			cfg->bad_srh = true;
			break;
		default:
			return -1;
		}
	}
	if (cfg->mode == MODE_NONE)
		return -1;
	return 0;
}

static uint16_t csum_fold(uint32_t sum)
{
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return ~sum;
}

static uint32_t csum_partial(const void *buf, size_t len, uint32_t sum)
{
	const uint8_t *p = buf;
	uint16_t word;

	while (len > 1) {
		memcpy(&word, p, sizeof(word));
		sum += word;
		p += sizeof(word);
		len -= sizeof(word);
	}
	if (len)
		sum += *p;
	return sum;
}

static uint16_t pseudo_csum(const struct in6_addr *src,
			    const struct in6_addr *dst,
			    uint32_t plen, uint8_t nexthdr,
			    const void *payload, size_t len)
{
	uint32_t nh = htonl(nexthdr);
	uint32_t pl = htonl(plen);
	uint32_t sum;

	sum = csum_partial(src, sizeof(*src), 0);
	sum = csum_partial(dst, sizeof(*dst), sum);
	sum = csum_partial(&pl, sizeof(pl), sum);
	sum = csum_partial(&nh, sizeof(nh), sum);
	sum = csum_partial(payload, len, sum);
	return csum_fold(sum);
}

static int send_end_map(const struct cfg *cfg)
{
	struct sockaddr_in6 dst_addr = { .sin6_family = AF_INET6 };
	struct end_map_frame frame = {};
	ssize_t res;
	int fd;

	frame.ip6.ip6_flow = htonl(6u << 28);
	frame.ip6.ip6_plen = htons(sizeof(frame.srh) + sizeof(frame.icmp6));
	frame.ip6.ip6_nxt = IPPROTO_ROUTING;
	frame.ip6.ip6_hops = 64;
	frame.ip6.ip6_src = cfg->src6;
	frame.ip6.ip6_dst = cfg->dst6;

	frame.srh.rthdr.ip6r_nxt = IPPROTO_ICMPV6;
	frame.srh.rthdr.ip6r_len = 2;		/* (1 + ip6r_len) * 8 = 24 */
	frame.srh.rthdr.ip6r_type = cfg->rh_type;
	frame.srh.rthdr.ip6r_segleft = 0;
	/* SRH Last Entry is the high byte of the type-specific word.  A
	 * single segment makes it 0; --bad-srh claims a second segment the
	 * header has no room for, which seg6_validate_srh() rejects.
	 */
	frame.srh.type_data = htonl((uint32_t)(cfg->bad_srh ? 1 : 0) << 24);
	frame.srh.segment = frame.ip6.ip6_dst;

	frame.icmp6.icmp6_type = ICMP6_ECHO_REQUEST;
	frame.icmp6.icmp6_code = 0;
	frame.icmp6.icmp6_dataun.icmp6_un_data16[0] = htons(0x1234);
	frame.icmp6.icmp6_dataun.icmp6_un_data16[1] = htons(1);
	frame.icmp6.icmp6_cksum = pseudo_csum(&frame.ip6.ip6_src,
					      &frame.ip6.ip6_dst,
					      sizeof(frame.icmp6),
					      IPPROTO_ICMPV6, &frame.icmp6,
					      sizeof(frame.icmp6));

	fd = socket(AF_INET6, SOCK_RAW, IPPROTO_RAW);
	if (fd < 0) {
		perror("socket");
		return 1;
	}
	dst_addr.sin6_addr = frame.ip6.ip6_dst;

	res = sendto(fd, &frame, sizeof(frame), 0,
		     (struct sockaddr *)&dst_addr, sizeof(dst_addr));
	close(fd);
	if (res != (ssize_t)sizeof(frame)) {
		perror("sendto");
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct cfg cfg = {};

	if (parse_args(argc, argv, &cfg)) {
		usage(argv[0]);
		return 3;
	}

	switch (cfg.mode) {
	case MODE_END_MAP:
		return send_end_map(&cfg);
	default:
		usage(argv[0]);
		return 3;
	}
}
