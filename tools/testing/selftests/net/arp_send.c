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

#ifndef __packed
#define __packed __attribute__((packed))
#endif

struct arp_pkt {
	struct ethhdr eth;
	struct {
		struct arphdr hdr;

		/* Variable part for Ethernet IP ARP */
		unsigned char ar_sha[ETH_ALEN]; /* sender hardware address */
		__be32 ar_sip;                  /* sender IP address       */
		unsigned char ar_tha[ETH_ALEN]; /* target hardware address */
		__be32 ar_tip;                  /* target IP address       */
	} __packed arp;
} __packed;

int parse_opts(int argc, char **argv, int *ifindex, struct arp_pkt *pkt)
{
	int rc;
	struct ether_addr *mac;
	uint16_t op_code;

	if (argc != 9) {
		fprintf(stderr, "Usage: %s <iface> <mac_dst> <mac_src> <op_code> <target-ip> <target-hwaddr> <sender-ip> <sender-hwaddr>\n",
			argv[0]);
		return -1;
	}

	*ifindex = atoi(argv[1]);
	mac = ether_aton(argv[2]);
	if (!mac) {
		fprintf(stderr, "Unable to parse mac_dst from '%s'\n", argv[2]);
		return -1;
	}

	/* Ethernet */
	memcpy(pkt->eth.h_dest, mac, ETH_ALEN);
	mac = ether_aton(argv[3]);
	if (!mac) {
		fprintf(stderr, "Unable to parse mac_src from '%s'\n", argv[3]);
		return -1;
	}
	memcpy(pkt->eth.h_source, mac, ETH_ALEN);
	pkt->eth.h_proto = htons(ETH_P_ARP);

	/* ARP */
	op_code = atol(argv[4]);
	if (op_code != ARPOP_REQUEST && op_code != ARPOP_REPLY) {
		fprintf(stderr, "Invalid ARP op %s\n", argv[4]);
		return -1;
	}
	pkt->arp.hdr.ar_op = htons(op_code);

	pkt->arp.hdr.ar_hrd = htons(0x1); /* Ethernet */
	pkt->arp.hdr.ar_pro = htons(ETH_P_IP);
	pkt->arp.hdr.ar_hln = ETH_ALEN;
	pkt->arp.hdr.ar_pln = 4;

	rc = inet_pton(AF_INET, argv[5], &pkt->arp.ar_tip);
	if (rc != 1) {
		fprintf(stderr, "Invalid IPv4 address %s\n", argv[5]);
		return -1;
	}
	rc = inet_pton(AF_INET, argv[7], &pkt->arp.ar_sip);
	if (rc != 1) {
		fprintf(stderr, "Invalid IPv4 address %s\n", argv[7]);
		return -1;
	}

	mac = ether_aton(argv[6]);
	if (!mac) {
		fprintf(stderr, "Unable to parse target-hwaddr from '%s'\n",
			argv[6]);
		return -1;
	}
	memcpy(pkt->arp.ar_tha, mac, ETH_ALEN);
	mac = ether_aton(argv[8]);
	if (!mac) {
		fprintf(stderr, "Unable to parse sender-hwaddr from '%s'\n",
			argv[8]);
		return -1;
	}
	memcpy(pkt->arp.ar_sha, mac, ETH_ALEN);

	return 0;
}

int main(int argc, char **argv)
{
	int rc, fd;
	struct sockaddr_ll bind_addr = {0};
	int ifindex;
	struct arp_pkt pkt = {0};

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
