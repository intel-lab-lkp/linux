// SPDX-License-Identifier: GPL-2.0

/* XSK basic regression test
 *
 * Exercise AF_XDP (XSK) sockets in all modes
 * - skb copy
 * - drv copy
 * - drv zerocopy
 *
 * Run:
 *
 * server: ./xsk_hw -i $DEV -[46] -D $CLIENT_ADDR -S $SERVER_ADDR -m $LOCAL_MAC -M $GW_MAC -h $ARGS
 * client: ./xsk_hw -i $DEV -[46] -D $SERVER_ADDR -S $CLIENT_ADDR -m $LOCAL_MAC -M $GW_MAC $ARGS
 *
 * Args:
 *
 * - ``: no args: minimal connectivity sanity test using PF_PACKET
 *
 * - `-T -s -c`: test transmit, skb copy mode
 * - `-T -d -c`: test transmit, driver copy mode
 * - `-T -d -z`: test transmit, driver zerocopy mode
 *
 * - `-R -s -c`: receive, skb copy mode
 * - `-R -d -c`: receive, driver copy mode
 * - `-R -d -z`: receive, driver zerocopy mode
 */

#include <arpa/inet.h>
#include <errno.h>
#include <error.h>
#include <linux/errqueue.h>
#include <linux/ethtool.h>
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/if_packet.h>
#include <linux/in.h>
#include <linux/in6.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <linux/udp.h>
#include <limits.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <uapi/linux/filter.h>
#include <unistd.h>

#include <network_helpers.h>

#include "xsk.h"
#include "xsk_hw.skel.h"

static int cfg_addr_len;
static void *cfg_daddr, *cfg_saddr;
static struct in_addr cfg_daddr4, cfg_saddr4;
static struct in6_addr cfg_daddr6, cfg_saddr6;
static uint16_t cfg_eth_proto;
static int cfg_family = PF_UNSPEC;

static bool cfg_host_run;
static char *cfg_ifname = "eth0";
static int cfg_ifindex;
static bool cfg_fill_after_bind;
static char *cfg_mac_dst, *cfg_mac_src;
static int cfg_num_rxq;
static uint16_t cfg_port = __constant_htons(8000);
static int cfg_pkt_len;
static const char cfg_payload[] = "aaaaaaaa";
static int cfg_rcvtimeo = 10;
static int cfg_send_queue_id;
static __u32 cfg_xdp_flags = XDP_FLAGS_REPLACE;
static __u16 cfg_xdp_bind_flags;
static bool cfg_xdp_rx;
static bool cfg_xdp_tx;
static bool cfg_xdp_tx_force_attach;

/* constants that can be used in static array allocation
 * const int is not sufficient: a const qualified variable
 */
enum {
	pkt_len_l4 = sizeof(struct udphdr) + sizeof(cfg_payload),
	pkt_len_v4 = ETH_HLEN + sizeof(struct iphdr) + pkt_len_l4,
	pkt_len_v6 = ETH_HLEN + sizeof(struct ipv6hdr) + pkt_len_l4,
};

static char pkt[pkt_len_v6];

#define UMEM_NUM 8192
#define UMEM_QLEN (UMEM_NUM / 2)
#define UMEM_FRAME_SIZE XSK_UMEM__DEFAULT_FRAME_SIZE
#define UMEM_SIZE (UMEM_FRAME_SIZE * UMEM_NUM)

struct xsk {
	void *umem_area;
	struct xsk_umem *umem;
	struct xsk_ring_prod fill;
	struct xsk_ring_cons comp;
	struct xsk_ring_prod tx;
	struct xsk_ring_cons rx;
	struct xsk_socket *socket;
	__u32 tx_head;
};

static struct xsk_hw *bpf_obj;
static struct xsk *xsks;

static int pfpacket_fd;
static int udp_fd;

static void init_pkt_ipv4(struct iphdr *iph)
{
	struct udphdr *uh;

	iph->version = 4;
	iph->ihl = 5;
	iph->protocol = IPPROTO_UDP;
	iph->tot_len = htons(sizeof(*iph) + sizeof(*uh) + sizeof(cfg_payload));
	iph->ttl = 64;
	iph->daddr = cfg_daddr4.s_addr;
	iph->saddr = cfg_saddr4.s_addr;
	iph->check = csum_fold(checksum_nofold(iph, sizeof(*iph), 0));
}

static void init_pkt_ipv6(struct ipv6hdr *ip6h)
{
	struct udphdr *uh;

	ip6h->version = 6;
	ip6h->payload_len = htons(sizeof(*uh) + sizeof(cfg_payload));
	ip6h->nexthdr = IPPROTO_UDP;
	ip6h->hop_limit = 64;
	ip6h->daddr = cfg_daddr6;
	ip6h->saddr = cfg_saddr6;
}

static void init_pkt(void)
{
	struct ipv6hdr *ip6h = NULL;
	struct iphdr *iph = NULL;
	struct ethhdr *eth;
	struct udphdr *uh;
	__wsum check;

	/* init mac header */
	eth = (void *)&pkt;
	if (sscanf(cfg_mac_dst, "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
		   &eth->h_dest[0], &eth->h_dest[1], &eth->h_dest[2],
		   &eth->h_dest[3], &eth->h_dest[4], &eth->h_dest[5]) != 6)
		error(1, 0, "sscanf mac dst ('-M')\n");
	if (sscanf(cfg_mac_src, "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
		   &eth->h_source[0], &eth->h_source[1], &eth->h_source[2],
		   &eth->h_source[3], &eth->h_source[4], &eth->h_source[5]) != 6)
		error(1, 0, "sscanf mac src ('-m')\n");
	eth->h_proto = htons(cfg_eth_proto);

	if (cfg_family == PF_INET) {
		iph = (void *)(eth + 1);
		uh = (void *)(iph + 1);
		init_pkt_ipv4(iph);
	} else {
		ip6h = (void *)(eth + 1);
		uh = (void *)(ip6h + 1);
		init_pkt_ipv6(ip6h);
	}

	/* init udp header */
	uh->source = cfg_port;
	uh->dest = cfg_port;
	uh->len = htons(sizeof(*uh) + sizeof(cfg_payload));
	uh->check = 0;

	/* init payload */
	memcpy(uh + 1, cfg_payload, sizeof(cfg_payload));

	/* udp checksum */
	check = checksum_nofold(uh, sizeof(*uh) + sizeof(cfg_payload), 0);
	if (ip6h)
		uh->check = csum_ipv6_magic(&ip6h->saddr, &ip6h->daddr,
					    ntohs(uh->len), IPPROTO_UDP,
					    check);
	else
		uh->check = csum_tcpudp_magic(iph->saddr, iph->daddr,
					      ntohs(uh->len), IPPROTO_UDP,
					      check);
}

static void *verify_pkt_ipv4(void *data, void *data_end)
{
	struct iphdr *iph = data;

	data = iph + 1;
	if (data > data_end)
		return NULL;

	if (iph->protocol != IPPROTO_UDP)
		return NULL;

	return data;
}

static void *verify_pkt_ipv6(void *data, void *data_end)
{
	struct ipv6hdr *ip6h = data;

	data = ip6h + 1;
	if (data > data_end)
		return NULL;

	if (ip6h->nexthdr != IPPROTO_UDP)
		return NULL;

	return data;
}

static void verify_pkt(void *data, size_t len)
{
	void *data_end = data + len;
	struct ethhdr *eth;
	struct udphdr *uh;

	eth = data;
	data = eth + 1;
	if (data > data_end)
		goto bad;
	if (eth->h_proto != htons(cfg_eth_proto))
		goto bad;

	if (cfg_family == PF_INET)
		data = verify_pkt_ipv4(data, data_end);
	else
		data = verify_pkt_ipv6(data, data_end);
	if (!data)
		goto bad;

	uh = data;
	data = uh + 1;
	if (data > data_end)
		goto bad;
	if (uh->dest != cfg_port)
		goto bad;

	if (data_end - data != sizeof(cfg_payload))
		goto bad;
	if (memcmp(data, cfg_payload, sizeof(cfg_payload)))
		goto bad;

	return;
bad:
	error(1, 0, "bad packet content");
}

static void udp_bind(void)
{
	/* Dual-stack, as not enabling IPV6_V6ONLY */
	struct sockaddr_in6 ip6addr = {
		.sin6_family = AF_INET6,
		.sin6_port = cfg_port,
		.sin6_addr = in6addr_any,
	};

	if (bind(udp_fd, (void *)&ip6addr, sizeof(ip6addr)) == -1)
		error(1, 0, "udp bind");
}

static void pfpacket_setfilter_ipproto(void)
{
	int off_proto, off_port;

	if (cfg_family == PF_INET) {
		off_proto = ETH_HLEN + offsetof(struct iphdr, protocol);
		off_port = ETH_HLEN + sizeof(struct iphdr) + offsetof(struct udphdr, dest);
	} else {
		off_proto = ETH_HLEN + offsetof(struct ipv6hdr, nexthdr);
		off_port = ETH_HLEN + sizeof(struct ipv6hdr) + offsetof(struct udphdr, dest);
	}

	struct sock_filter filter[] = {
		BPF_STMT(BPF_LD  + BPF_B   + BPF_ABS, SKF_AD_OFF + SKF_AD_PKTTYPE),
		BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, PACKET_HOST, 0, 5),
		BPF_STMT(BPF_LD  + BPF_B   + BPF_ABS, off_proto),
		BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, IPPROTO_UDP, 0, 3),
		BPF_STMT(BPF_LD  + BPF_H   + BPF_ABS, off_port),
		BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, ntohs(cfg_port), 0, 1),
		BPF_STMT(BPF_RET + BPF_K, 0xFFFF),
		BPF_STMT(BPF_RET + BPF_K, 0),
	};
	struct sock_fprog prog = {};

	prog.filter = filter;
	prog.len = sizeof(filter) / sizeof(struct sock_filter);
	if (setsockopt(pfpacket_fd, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof(prog)))
		error(1, errno, "setsockopt filter");
}

static void pfpacket_bind(void)
{
	struct sockaddr_ll laddr = {
		.sll_family = AF_PACKET,
		.sll_protocol = cfg_xdp_rx ? 0 : htons(cfg_eth_proto),
		.sll_ifindex = cfg_ifindex,
	};

	if (bind(pfpacket_fd, (void *)&laddr, sizeof(laddr)) == -1)
		error(1, 0, "pfpacket bind");
}

static int fill_xsk(struct xsk *xsk)
{
	__u64 addr;
	__u32 idx;
	int i;

	/* returns either 0 on failure or second arg, UMEM_QLEN */
	if (!xsk_ring_prod__reserve(&xsk->fill, UMEM_QLEN, &idx))
		return -ENOMEM;

	for (i = 0; i < UMEM_QLEN; i++) {
		addr = (UMEM_QLEN + i) * UMEM_FRAME_SIZE;
		*xsk_ring_prod__fill_addr(&xsk->fill, i) = addr;
	}
	xsk_ring_prod__submit(&xsk->fill, UMEM_QLEN);

	return 0;
}

static int open_xsk(struct xsk *xsk, __u32 queue_id)
{
	const int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
	const struct xsk_socket_config socket_config = {
		.rx_size = UMEM_QLEN,
		.tx_size = UMEM_QLEN,
		.bind_flags = cfg_xdp_bind_flags,
	};
	const struct xsk_umem_config umem_config = {
		.fill_size = UMEM_QLEN,
		.comp_size = UMEM_QLEN,
		.frame_size = XSK_UMEM__DEFAULT_FRAME_SIZE,
	};
	__u64 addr;
	int ret;
	int i;

	xsk->umem_area = mmap(NULL, UMEM_SIZE, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);
	if (xsk->umem_area == MAP_FAILED)
		return -ENOMEM;

	ret = xsk_umem__create(&xsk->umem,
			       xsk->umem_area, UMEM_SIZE,
			       &xsk->fill,
			       &xsk->comp,
			       &umem_config);
	if (ret)
		return ret;

	ret = xsk_socket__create(&xsk->socket, cfg_ifindex, queue_id,
				 xsk->umem,
				 &xsk->rx,
				 &xsk->tx,
				 &socket_config);
	if (ret)
		return ret;

	/* First half of umem is for TX. This way address matches 1-to-1
	 * to the completion queue index.
	 */

	for (i = 0; i < UMEM_QLEN; i++) {
		addr = i * UMEM_FRAME_SIZE;
		memcpy(xsk_umem__get_data(xsk->umem_area, addr),
		       pkt, cfg_pkt_len);
	}

	/* Second half of umem is for RX. */
	if (!cfg_fill_after_bind) {
		if (fill_xsk(xsk))
			return -ENOMEM;
	}

	return 0;
}

static void release_tx(struct xsk *xsk)
{
	__u32 idx = 0;
	unsigned int n;

	n = xsk_ring_cons__peek(&xsk->comp, XSK_RING_CONS__DEFAULT_NUM_DESCS, &idx);
	if (n)
		xsk_ring_cons__release(&xsk->comp, n);
}

static void send_xsk(void)
{
	struct xsk *xsk = &xsks[cfg_send_queue_id];
	struct xdp_desc *desc;
	__u32 idx;

	release_tx(xsk);
	if (xsk_ring_prod__reserve(&xsk->tx, 1, &idx) != 1)
		error(1, 0, "TX ring is full");

	desc = xsk_ring_prod__tx_desc(&xsk->tx, idx);
	desc->addr = (xsk->tx_head++ % UMEM_QLEN) * UMEM_FRAME_SIZE;
	desc->len = cfg_pkt_len;

	xsk_ring_prod__submit(&xsk->tx, 1);
	sendto(xsk_socket__fd(xsk->socket), NULL, 0, MSG_DONTWAIT, NULL, 0);
}

static void refill_rx(struct xsk *xsk, __u64 addr)
{
	__u32 idx;

	if (xsk_ring_prod__reserve(&xsk->fill, 1, &idx) == 1) {
		*xsk_ring_prod__fill_addr(&xsk->fill, idx) = addr;
		xsk_ring_prod__submit(&xsk->fill, 1);
	}
}

static void recv_xsk(void)
{
	const struct xdp_desc *desc;
	struct pollfd fds[cfg_num_rxq];
	__u64 comp_addr;
	__u64 addr;
	__u32 idx;
	int ret;
	int i;

	for (i = 0; i < cfg_num_rxq; i++) {
		fds[i].fd = xsk_socket__fd(xsks[i].socket);
		fds[i].events = POLLIN;
		fds[i].revents = 0;
	}

	ret = poll(fds, cfg_num_rxq, cfg_rcvtimeo * 1000);
	if (ret < 0)
		error(1, -ret, "poll");
	if (ret == 0)
		error(1, 0, "%s: Timeout", __func__);

	for (i = 0; i < cfg_num_rxq; i++) {
		if (fds[i].revents == 0)
			continue;

		struct xsk *xsk = &xsks[i];

		/* Reading one packet at a time, because we expect only one
		 * packet outstanding per queue at a time due to test doing
		 * single connection request/response
		 */
		ret = xsk_ring_cons__peek(&xsk->rx, 1, &idx);
		if (ret != 1)
			continue;

		desc = xsk_ring_cons__rx_desc(&xsk->rx, idx);
		comp_addr = xsk_umem__extract_addr(desc->addr);
		addr = xsk_umem__add_offset_to_addr(desc->addr);
		verify_pkt(xsk_umem__get_data(xsk->umem_area, addr), desc->len);
		xsk_ring_cons__release(&xsk->rx, 1);
		refill_rx(xsk, comp_addr);
	}
}

static void send_pfpacket(void)
{
	int ret;

	ret = write(pfpacket_fd, pkt, cfg_pkt_len);
	if (ret == -1)
		error(1, errno, "write");
	if (ret != cfg_pkt_len)
		error(1, 0, "write pkt: %uB != %uB", ret, cfg_pkt_len);
}

static void recv_pfpacket(void)
{
	static char recv_pkt[sizeof(pkt)];
	struct pollfd fds = {
		.fd = pfpacket_fd,
		.events = POLLIN,
	};
	int ret, pkt_len;

	ret = poll(&fds, 1, cfg_rcvtimeo * 1000);
	if (ret < 0)
		error(1, -ret, "poll");
	if (ret == 0)
		error(1, 0, "%s: Timeout", __func__);

	pkt_len = cfg_family == PF_INET ? pkt_len_v4 : pkt_len_v6;

	ret = recv(pfpacket_fd, recv_pkt, sizeof(pkt), MSG_TRUNC);
	if (ret == -1)
		error(1, errno, "recv");
	if (ret != pkt_len)
		error(1, 0, "recv pkt: %uB != %uB\n", ret, pkt_len);

	verify_pkt(recv_pkt, ret);
}

static void do_send(void)
{
	if (cfg_xdp_tx)
		send_xsk();
	else
		send_pfpacket();
}

static void do_recv(void)
{
	if (cfg_xdp_rx)
		recv_xsk();
	else
		recv_pfpacket();
}

static bool link_is_down(void)
{
	char path[PATH_MAX];
	FILE *file;
	char status;

	snprintf(path, PATH_MAX, "/sys/class/net/%s/carrier", cfg_ifname);
	file = fopen(path, "r");
	if (!file)
		error(1, errno, "%s", path);

	if (fread(&status, 1, 1, file) != 1)
		error(1, errno, "fread");

	fclose(file);

	return status == '0';
}

static void do_sync_client(void)
{
	struct sockaddr_in ip4addr = {
		.sin_family = AF_INET,
		.sin_port = cfg_port,
		.sin_addr = cfg_daddr4,
	};
	struct sockaddr_in6 ip6addr = {
		.sin6_family = AF_INET6,
		.sin6_port = cfg_port,
		.sin6_addr = cfg_daddr6,
	};
	const int retry_sleep_ms = 200;
	const int retries_per_sec = 1000 / retry_sleep_ms;
	const int max_retries = cfg_rcvtimeo * retries_per_sec;
	int fd, ret, retries = 0;

	fd = socket(cfg_family, SOCK_STREAM, 0);
	if (fd == -1)
		error(1, errno, "socket sync client");

	/* If the client calls connect before the server listens,
	 * the connection will fail immediately and the call returns
	 * with ECONNREFUSED. Retry up to cfg_rcvtimeo.
	 */
	while (true) {
		if (cfg_family == PF_INET)
			ret = connect(fd, (void *)&ip4addr, sizeof(ip4addr));
		else
			ret = connect(fd, (void *)&ip6addr, sizeof(ip6addr));

		if (ret == -1 && errno != ECONNREFUSED)
			error(1, errno, "connect sync client");
		if (ret == 0)
			break;
		retries++;
		usleep(retry_sleep_ms * 1000);
		if (retries == max_retries)
			error(1, 0, "connect sync client: max_retries");
	}

	if (close(fd))
		error(1, errno, "close sync client");
}

static void do_sync_server(void)
{
	int fdl, fdc;
	int *fds;

	/* Dual-stack, as not enabling IPV6_V6ONLY */
	fds = start_reuseport_server(AF_INET6, SOCK_STREAM, "::",
				     ntohs(cfg_port), cfg_rcvtimeo * 1000, 1);
	if (!fds)
		error(1, errno, "start_server");
	fdl = *fds;

	fdc = accept(fdl, NULL, NULL);
	if (fdc == -1)
		error(1, errno, "accept sync");

	if (close(fdc))
		error(1, errno, "close sync child");
	if (close(fdl))
		error(1, errno, "close sync listener");
}

static void sync_barrier(void)
{
	if (cfg_host_run)
		do_sync_server();
	else
		do_sync_client();
}

static void cleanup(void)
{
	LIBBPF_OPTS(bpf_xdp_attach_opts, opts);

	if (bpf_obj) {
		opts.old_prog_fd = bpf_program__fd(bpf_obj->progs.rx);
		if (opts.old_prog_fd >= 0)
			bpf_xdp_detach(cfg_ifindex, cfg_xdp_flags, &opts);
	}
}

static void setup_for_ipv4(void)
{
	cfg_family = PF_INET;
	cfg_eth_proto = ETH_P_IP;
	cfg_addr_len = sizeof(struct in_addr);
	cfg_pkt_len = pkt_len_v4;
	cfg_daddr = &cfg_daddr4;
	cfg_saddr = &cfg_saddr4;
}

static void setup_for_ipv6(void)
{
	cfg_family = PF_INET6;
	cfg_eth_proto = ETH_P_IPV6;
	cfg_addr_len = sizeof(struct in6_addr);
	cfg_pkt_len = pkt_len_v6;
	cfg_daddr = &cfg_daddr6;
	cfg_saddr = &cfg_saddr6;
}

static void parse_opts(int argc, char **argv)
{
	char *daddr = NULL, *saddr = NULL;
	int c;

	while ((c = getopt(argc, argv, "46cD:dfhi:m:M:p:q:RS:sTz")) != -1) {
		switch (c) {
		case '4':
			setup_for_ipv4();
			break;
		case '6':
			setup_for_ipv6();
			break;
		case 'c':
			cfg_xdp_bind_flags |= XDP_COPY;
			break;
		case 'D':
			daddr = optarg;
			break;
		case 'd':
			cfg_xdp_flags |= XDP_FLAGS_DRV_MODE;
			break;
		case 'f':
			cfg_fill_after_bind = true;
			break;
		case 'h':
			cfg_host_run = true;
			break;
		case 'i':
			cfg_ifname = optarg;
			break;
		case 'm':
			cfg_mac_src = optarg;
			break;
		case 'M':
			cfg_mac_dst = optarg;
			break;
		case 'p':
			cfg_port = htons(atoi(optarg));
			break;
		case 'q':
			cfg_send_queue_id = atoi(optarg);
			break;
		case 'R':
			cfg_xdp_rx = true;
			break;
		case 'S':
			saddr = optarg;
			break;
		case 's':
			cfg_xdp_flags |= XDP_FLAGS_SKB_MODE;
			break;
		case 'T':
			if (cfg_xdp_tx)
				cfg_xdp_tx_force_attach = true;
			cfg_xdp_tx = true;
			break;
		case 'z':
			cfg_xdp_bind_flags |= XDP_ZEROCOPY;
			break;
		default:
			error(1, 0, "%s: parse error", argv[0]);
		}
	}

	if (cfg_family == PF_UNSPEC)
		error(1, 0, "select one of -4 or -6");

	if (!cfg_mac_src || !cfg_mac_dst || !saddr || !daddr)
		error(1, 0, "all MAC and IP addresses must be set");
	if (cfg_fill_after_bind && !cfg_xdp_rx && cfg_xdp_tx)
		error(1, 0, "'-f' is meaningless without '-R' or '-T'");

	if (inet_pton(cfg_family, daddr, cfg_daddr) != 1)
		error(1, 0, "dst addr parse error: dst ('-D')");
	if (inet_pton(cfg_family, saddr, cfg_saddr) != 1)
		error(1, 0, "src addr parse error: src ('-S')");

	cfg_ifindex = if_nametoindex(cfg_ifname);
	if (!cfg_ifindex)
		error(1, 0, "ifname invalid");
}

static void handle_signal(int sig)
{
	/* Signal handler (rather than default termination) needed to
	 * make sure the atexit cleanup is invoked and XDP is detached
	 */
	exit(1);
}

int main(int argc, char *argv[])
{
	int ret;
	int i;

	parse_opts(argc, argv);
	init_pkt();

	/* A UDP socket to silence kernel-generated ICMP unreachable
	 * without needing an iptables rule.
	 */
	udp_fd = socket(PF_INET6, SOCK_DGRAM, 0);
	if (udp_fd == -1)
		error(1, errno, "socket");

	pfpacket_fd = socket(PF_PACKET, SOCK_RAW, 0);
	if (pfpacket_fd == -1)
		error(1, errno, "socket");

	udp_bind();

	pfpacket_setfilter_ipproto();
	pfpacket_bind();

	cfg_num_rxq = rxq_num(cfg_ifname);
	if (cfg_num_rxq < 0)
		error(1, -cfg_num_rxq, "rxq_num");

	if (cfg_xdp_rx || cfg_xdp_tx_force_attach) {
		bpf_obj = xsk_hw__open();
		if (libbpf_get_error(bpf_obj))
			error(1, libbpf_get_error(bpf_obj), "xsk_hw__open");

		/* Not doing bpf_program__set_ifindex because it requests offload */

		ret = xsk_hw__load(bpf_obj);
		if (ret)
			error(1, -ret, "xsk_hw__load");

		bpf_obj->bss->port = cfg_port;
		bpf_obj->bss->should_rx = cfg_xdp_rx;
	}

	if (cfg_xdp_rx || cfg_xdp_tx) {
		xsks = calloc(cfg_num_rxq, sizeof(struct xsk));
		if (!xsks)
			error(1, ENOMEM, "malloc");

		for (i = 0; i < cfg_num_rxq; i++) {
			ret = open_xsk(&xsks[i], i);
			if (ret)
				error(1, -ret, "open_xsk");
		}
	}

	if (cfg_xdp_rx) {
		for (i = 0; i < cfg_num_rxq; i++) {
			int sock_fd = xsk_socket__fd(xsks[i].socket);
			__u32 queue_id = i;

			ret = bpf_map__update_elem(bpf_obj->maps.xsk,
						   &queue_id, sizeof(queue_id),
						   &sock_fd, sizeof(sock_fd), 0);
			if (ret)
				error(1, -ret, "bpf_map__update_elem");
		}
	}

	if (cfg_xdp_rx || cfg_xdp_tx_force_attach) {
		ret = bpf_xdp_attach(cfg_ifindex,
				     bpf_program__fd(bpf_obj->progs.rx),
				     cfg_xdp_flags, NULL);
		if (ret)
			error(1, -ret, "bpf_xdp_attach");
	}

	/* Optionally exercise an AF_XDP API use edge case:
	 * Bind the socket before making buffers available in the fillq.
	 *
	 * Peculiar behavior, but seen in practice and seen it tripping
	 * up at least one driver that would leave networking disabled
	 * on failing to allocate during init.
	 */
	if (cfg_fill_after_bind) {
		for (i = 0; i < cfg_num_rxq; i++)
			fill_xsk(&xsks[i]);
	}

	atexit(cleanup);
	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	/* XDP may need a delay for device reinitialization */
	do {
		usleep(100 * 1000);
	} while (link_is_down());

	sync_barrier();

	if (cfg_host_run) {
		usleep(100 * 1000);
		do_recv();
	} else {
		do_send();
	}
}
