// SPDX-License-Identifier: GPL-2.0

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <inttypes.h>
#include <netinet/ether.h>
#include <arpa/inet.h>

#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/ipv6.h>
#include <linux/icmpv6.h>

#define ICMPV6_ND_NS 135
#define ICMPV6_ND_NA 136
#define ICMPV6_ND_SLLADR 1
#define ICMPV6_ND_TLLADR 2

#ifndef __noinline
#define __noinline __attribute__((noinline))
#endif
#ifndef __packed
#define __packed __attribute__((packed))
#endif

struct icmp6_pseudohdr {
	struct in6_addr saddr;
	struct in6_addr daddr;
	uint32_t plen;
	uint8_t zero[3];
	uint8_t next;
};

struct ndisc_pkt {
	struct ethhdr eth;
	struct ipv6hdr ip6;
	struct ndp_hdrs {
		struct icmp6hdr hdr;
		struct in6_addr target;

		uint8_t opt_type;
		uint8_t opt_len;
		uint8_t opt_mac[ETH_ALEN];
	} __packed ndp;
} __packed;

__noinline uint32_t csum_add(void *buf, int len, uint32_t sum)
{
	uint16_t *p = (uint16_t *)buf;

	while (len > 1) {
		sum += *p++;
		len -= 2;
	}

	if (len)
		sum += *(uint8_t *)p;

	return sum;
}

static uint16_t csum_fold(uint32_t sum)
{
	return ~((sum & 0xffff) + (sum >> 16)) ? : 0xffff;
}

int parse_opts(int argc, char **argv, int *ifindex, struct ndisc_pkt *pkt)
{
	struct ether_addr *mac;
	uint16_t op;
	struct icmp6_pseudohdr ph = {0};
	uint32_t sum = 0;

	if (argc != 9) {
		fprintf(stderr, "Usage: %s <iface> <mac_dst> <mac_src> <dst_ip> <src_ip> <target_ip> <op> <lladr>\n",
			argv[0]);
		return -1;
	}

	*ifindex = atoi(argv[1]);
	mac = ether_aton(argv[2]);
	if (!mac) {
		fprintf(stderr, "Unable to parse mac_dst from '%s'\n", argv[1]);
		return -1;
	}

	/* Ethernet */
	memcpy(pkt->eth.h_dest, mac, ETH_ALEN);
	mac = ether_aton(argv[3]);
	if (!mac) {
		fprintf(stderr, "Unable to parse mac_src from '%s'\n", argv[2]);
		return -1;
	}
	memcpy(pkt->eth.h_source, mac, ETH_ALEN);
	pkt->eth.h_proto = htons(ETH_P_IPV6);

	/* IPv6 */
	pkt->ip6.version = 6;
	pkt->ip6.nexthdr = IPPROTO_ICMPV6;
	pkt->ip6.hop_limit = 255;

	if (inet_pton(AF_INET6, argv[4], &pkt->ip6.daddr) != 1) {
		fprintf(stderr, "Unable to parse src_ip from '%s'\n", argv[4]);
		return -1;
	}
	if (inet_pton(AF_INET6, argv[5], &pkt->ip6.saddr) != 1) {
		fprintf(stderr, "Unable to parse src_ip from '%s'\n", argv[5]);
		return -1;
	}

	/* ICMPv6 */
	op = atoi(argv[7]);
	if (op != ICMPV6_ND_NS && op != ICMPV6_ND_NA) {
		fprintf(stderr, "Invalid ICMPv6 op %d\n", op);
		return -1;
	}

	pkt->ndp.hdr.icmp6_type = op;
	pkt->ndp.hdr.icmp6_code = 0;

	if (inet_pton(AF_INET6, argv[6], &pkt->ndp.target) != 1) {
		fprintf(stderr, "Unable to parse target_ip from '%s'\n",
			argv[6]);
		return -1;
	}

	/* Target/Source Link-Layer Address */
	if (op == ICMPV6_ND_NS) {
		pkt->ndp.opt_type = ICMPV6_ND_SLLADR;
	} else {
		pkt->ndp.opt_type = ICMPV6_ND_TLLADR;
		pkt->ndp.hdr.icmp6_override = 1;
	}
	pkt->ndp.opt_len = 1;

	mac = ether_aton(argv[8]);
	if (!mac) {
		fprintf(stderr, "Invalid lladdr %s\n", argv[8]);
		return -1;
	}

	memcpy(pkt->ndp.opt_mac, mac, ETH_ALEN);

	pkt->ip6.payload_len = htons(sizeof(pkt->ndp));

	/* Pseudoheader */
	ph.saddr = pkt->ip6.saddr;
	ph.daddr = pkt->ip6.daddr;
	ph.plen = htonl(sizeof(pkt->ndp));
	ph.next = IPPROTO_ICMPV6;

	sum = csum_add(&ph, sizeof(ph), 0);
	sum = csum_add(&pkt->ndp, sizeof(pkt->ndp), sum);

	pkt->ndp.hdr.icmp6_cksum = csum_fold(sum);

	return 0;
}

int main(int argc, char **argv)
{
	int rc, fd;
	struct sockaddr_ll bind_addr = {0};
	int ifindex;
	struct ndisc_pkt pkt = {0};

	if (parse_opts(argc, argv, &ifindex, &pkt) < 0)
		return -1;

	fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (fd < 0) {
		fprintf(stderr, "Unable to open raw socket(%d). Need root privileges?\n",
			fd);
		return 1;
	}

	bind_addr.sll_family   = AF_PACKET;
	bind_addr.sll_protocol = htons(ETH_P_ALL);
	bind_addr.sll_ifindex  = ifindex;

	rc = bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
	if (rc < 0) {
		fprintf(stderr, "Unable to bind raw socket(%d). Invalid iface '%d'?\n",
			rc, ifindex);
		return 1;
	}

	rc = send(fd, &pkt, sizeof(pkt), 0);
	if (rc < 0) {
		fprintf(stderr, "Unable to send packet: %d\n", rc);
		return 1;
	}

	return 0;
}
