// SPDX-License-Identifier: GPL-2.0
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <linux/in6.h>
#include <linux/if_link.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>

#include "nk_forward.skel.h"

static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args)
{
	return vfprintf(stderr, format, args);
}

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s -n <netkit_ifindex> -e <eth0_ifindex> -i <ipv6_prefix>\n", prog);
	fprintf(stderr, "  -n  netkit interface index\n");
	fprintf(stderr, "  -e  eth0 interface index\n");
	fprintf(stderr, "  -i  IPv6 prefix to match\n");
	fprintf(stderr, "  -h  show this help\n");
}

int main(int argc, char **argv)
{
	unsigned int netkit_ifindex = 0;
	const char *ipv6_prefix = NULL;
	unsigned int eth0_ifindex = 0;
	struct nk_forward *skel;
	struct in6_addr ip6_addr;
	struct bpf_link *link;
	int opt, err, i;

	while ((opt = getopt(argc, argv, "n:e:i:h")) != -1) {
		switch (opt) {
		case 'n':
			netkit_ifindex = atoi(optarg);
			break;
		case 'e':
			eth0_ifindex = atoi(optarg);
			break;
		case 'i':
			ipv6_prefix = optarg;
			break;
		case 'h':
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : 1;
		}
	}

	if (!netkit_ifindex || !eth0_ifindex || !ipv6_prefix) {
		fprintf(stderr, "Error: All options -n, -e, and -i are required\n\n");
		usage(argv[0]);
		return 1;
	}

	if (inet_pton(AF_INET6, ipv6_prefix, &ip6_addr) != 1) {
		fprintf(stderr, "Error: Invalid IPv6 address: %s\n", ipv6_prefix);
		return 1;
	}

	libbpf_set_print(libbpf_print_fn);
	skel = nk_forward__open();
	if (!skel) {
		fprintf(stderr, "Error: Failed to open BPF skeleton\n");
		return 1;
	}

	skel->bss->netkit_ifindex = netkit_ifindex;
	memcpy((void *)&skel->bss->ipv6_prefix, &ip6_addr, sizeof(struct in6_addr));

	err = nk_forward__load(skel);
	if (err) {
		fprintf(stderr, "Error: Failed to load BPF skeleton: %d\n", err);
		goto cleanup;
	}

	LIBBPF_OPTS(bpf_tcx_opts, opts);
	link = bpf_program__attach_tcx(skel->progs.tc_redirect_peer, eth0_ifindex, &opts);
	if (!link) {
		err = -errno;
		fprintf(stderr, "Error: Failed to attach TC program to ifindex %u: %s\n",
			eth0_ifindex, strerror(errno));
		goto cleanup;
	}

	while (1)
		sleep(1);

cleanup:
	bpf_link__destroy(link);
	nk_forward__destroy(skel);
	return err != 0;
}
