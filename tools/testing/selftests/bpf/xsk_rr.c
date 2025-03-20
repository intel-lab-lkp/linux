// SPDX-License-Identifier: GPL-2.0

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
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "xsk.h"

#include "xsk_rr_progs.skel.h"

#define MIN(x, y) (((x)<(y))?(x):(y))

#define MAX_OUTSTANDING 1024
#define OUTSTANDING_MASK(x) (x & (MAX_OUTSTANDING - 1))

#define MAX_PAYLOAD_SIZE 1024

static int cfg_addr_len;
static void *cfg_daddr, *cfg_saddr;
static struct in_addr cfg_daddr4, cfg_saddr4;
static struct in6_addr cfg_daddr6, cfg_saddr6;
static uint16_t cfg_eth_proto;
static int cfg_family = PF_UNSPEC;

static bool cfg_server_run;
static char *cfg_ifname = "eth0";
static int cfg_ifindex;
static char *cfg_mac_dst, *cfg_mac_src;
static uint16_t cfg_port = __constant_htons(8000);
static int cfg_pkt_len;
static char cfg_payload[MAX_PAYLOAD_SIZE];
static __u32 cfg_payload_size = 200;
static __u32 cfg_xdp_flags = XDP_FLAGS_DRV_MODE;//XDP_FLAGS_REPLACE;
static __u16 cfg_xdp_bind_flags = XDP_COPY| XDP_USE_NEED_WAKEUP;
static int cfg_outstanding_pkts = 1;
static uint64_t cfg_server_delay = 0;
static uint64_t cfg_client_send_period = 0;
static bool cfg_client_send_threaded = false;
static bool cfg_rx_polling = false;
static bool cfg_skip_kick = false;
static bool cfg_verify = false;
static uint64_t cfg_log_f = 1UL;
static pthread_t rx_polling_thread;

/* constants that can be used in static array allocation
 * const int is not sufficient: a const qualified variable
 */
enum {
	pkt_len_l4 = sizeof(struct udphdr),
	pkt_len_v4 = ETH_HLEN + sizeof(struct iphdr) + pkt_len_l4,
	pkt_len_v6 = ETH_HLEN + sizeof(struct ipv6hdr) + pkt_len_l4,
};

static char pkt[2048];

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

static struct xsk_rr_progs *bpf_obj;
static struct xsk *xsk_sock;

uint64_t min_rtt = ULONG_MAX;
uint64_t max_rtt = 0;
uint64_t avg_rtt = 0;
static int first_idx = 0;
static int next_idx = 0;
static uint64_t outstanding[MAX_OUTSTANDING] = {0};
static uint64_t empty_rx_tries = 0;
static uint64_t rx_tries = 0;

static uint64_t prev_print = 0;

#define BUCKETS_NUM 20000
#define BUCKET_SIZE_NM 100

/*
 * Last bucket for outliers
 */
#define BUCKET_MAX_VALUE ((BUCKETS_NUM - 1) *  BUCKET_SIZE_NM)

static uint64_t buckets[BUCKETS_NUM] = {0};
static uint64_t total_pkts_intvl = 0;

static uint64_t pending_sends = 0;
static uint64_t done_sends = 0;

static inline uint64_t get_current_time()
{
	uint64_t now;
	struct timespec tm;

	clock_gettime(CLOCK_MONOTONIC, &tm);
	now = tm.tv_nsec + tm.tv_sec * 1000000000ul;
	return now;
}

static void clean_buckets()
{
	memset(buckets, 0, sizeof(buckets));
}

static void print_percentile(int percentile)
{
	int i=0;
	uint64_t rtt = 0;
	uint64_t count = 0;
	uint64_t total_count = 0;

	while (i < BUCKETS_NUM) {
		total_count += buckets[i];
		++i;
	}

	total_count = (total_count*percentile)/100;

	i = 0;
	while (count < total_count) {
		buckets[i] = MIN(buckets[i], total_count - count);
		if (count != 0 || buckets[i] != 0)
			rtt = (buckets[i]*(i) + count*rtt) / (count + buckets[i]);
		count += buckets[i];
		++i;
	}

	printf("count: %lu p%d: %lu\n", count, percentile, rtt*BUCKET_SIZE_NM);
}

static void show_rate()
{
        printf("rate: %lu\n", total_pkts_intvl/cfg_log_f);
	total_pkts_intvl = 0;
}

static inline int record_send()
{
	outstanding[OUTSTANDING_MASK(next_idx)] = get_current_time();
	++next_idx;

	if (OUTSTANDING_MASK(next_idx) == OUTSTANDING_MASK(first_idx)) {
		printf("\n%d %d\n", next_idx, first_idx);
		error(1, 0, "oustanding array full");
		return EAGAIN;
	}

	return 0;
}

static inline void record_recv()
{
	uint64_t now = get_current_time();
	uint64_t rtt = now - outstanding[OUTSTANDING_MASK(first_idx)];

	++first_idx;

	if (min_rtt > rtt)
		min_rtt = rtt;

	if (max_rtt < rtt)
		max_rtt = rtt;

	avg_rtt = (avg_rtt*total_pkts_intvl + rtt) /
				(total_pkts_intvl + 1);

	if (rtt < BUCKET_MAX_VALUE)
		buckets[(rtt/BUCKET_SIZE_NM)] += 1;
	else
		buckets[BUCKETS_NUM - 1] += 1;

	++total_pkts_intvl;

	if (now >= (prev_print + (cfg_log_f*1000000000UL))) {
		printf("\n");
		printf("min: %lu max: %lu avg: %lu empty: %lu\n",
			min_rtt,
			max_rtt, avg_rtt, (empty_rx_tries*100)/rx_tries);

		/*
		 * Just to avoid divide by zero
		 */
		empty_rx_tries = 1;
		rx_tries = 1;
		print_percentile(5);
		print_percentile(50);
		print_percentile(95);
		print_percentile(99);
		show_rate();
		printf("oustanding packets: %lu\n", (pending_sends - done_sends));
		printf("\n");

		clean_buckets();

		prev_print = now;
	}
}

static uint32_t checksum_nofold(void *data, size_t len, uint32_t sum)
{
	uint16_t *words = (uint16_t *)data;
	int i;

	for (i = 0; i < len / 2; i++)
		sum += words[i];

	if (len & 1)
		sum += ((unsigned char *)data)[len - 1];

	return sum;
}

static uint16_t checksum_fold(void *data, size_t len, uint32_t sum)
{
	sum = checksum_nofold(data, len, sum);

	while (sum > 0xFFFF)
		sum = (sum & 0xFFFF) + (sum >> 16);

	return ~sum;
}

static void *init_pkt_ipv4(void *data)
{
	struct iphdr *iph = data;
	struct udphdr *uh;

	iph->version = 4;
	iph->ihl = 5;
	iph->protocol = IPPROTO_UDP;
	iph->tot_len = htons(sizeof(*iph) + sizeof(*uh) + cfg_payload_size);
	iph->ttl = 64;
	iph->daddr = cfg_daddr4.s_addr;
	iph->saddr = cfg_saddr4.s_addr;
	iph->check = checksum_fold(iph, sizeof(*iph), 0);

	return iph + 1;
}

static void *init_pkt_ipv6(void *data)
{
	struct ipv6hdr *ip6h = data;
	struct udphdr *uh;

	ip6h->version = 6;
	ip6h->payload_len = htons(sizeof(*uh) + cfg_payload_size);
	ip6h->nexthdr = IPPROTO_UDP;
	ip6h->hop_limit = 64;
	ip6h->daddr = cfg_daddr6;
	ip6h->saddr = cfg_saddr6;

	return ip6h + 1;
}

static void init_payload(void)
{
	memset(cfg_payload, 'a', cfg_payload_size);
}

static void init_pkt(void)
{
	struct ethhdr *eth;
	struct udphdr *uh;
	uint32_t sum;

	init_payload();

	/* init mac header */
	eth = (void *)&pkt;
	if (sscanf(cfg_mac_dst, "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
		   &eth->h_dest[0], &eth->h_dest[1], &eth->h_dest[2],
		   &eth->h_dest[3], &eth->h_dest[4], &eth->h_dest[5]) != 6)
		error(1, 0, "sscanf mac dst ('-M') \n");
	if (sscanf(cfg_mac_src, "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
		   &eth->h_source[0], &eth->h_source[1], &eth->h_source[2],
		   &eth->h_source[3], &eth->h_source[4], &eth->h_source[5]) != 6)
		error(1, 0, "sscanf mac src ('-m')\n");
	eth->h_proto = htons(cfg_eth_proto);

	if (cfg_family == PF_INET)
		uh = init_pkt_ipv4(eth + 1);
	else
		uh = init_pkt_ipv6(eth + 1);

	/* init udp header */
	uh->source = cfg_port;
	uh->dest = cfg_port;
	uh->len = htons(sizeof(*uh) + cfg_payload_size);
	uh->check = 0;

	/* init payload */
	memcpy(uh + 1, cfg_payload, cfg_payload_size);

	/* udp checksum */
	sum = checksum_nofold(((char *)uh) - (cfg_addr_len * 2),
			      (cfg_addr_len * 2) + sizeof(*uh) + cfg_payload_size, 0);
	sum += htons(IPPROTO_UDP);
	sum += uh->len;

	uh->check = checksum_fold(NULL, 0, sum);
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

	if (data_end - data != cfg_payload_size)
		goto bad;
	if (memcmp(data, cfg_payload, cfg_payload_size))
		goto bad;

	return;
bad:
	error(1, 0, "bad packet content");
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

static int open_xsk(struct xsk *xsk)
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

	ret = xsk_socket__create(&xsk->socket, cfg_ifindex, 0,
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

	if (fill_xsk(xsk))
		return -ENOMEM;

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
	struct xsk *xsk = xsk_sock;
	struct xdp_desc *desc;
	__u32 idx;

	release_tx(xsk);

	if (xsk_ring_prod__reserve(&xsk->tx, 1, &idx) != 1)
		error(1, 0, "TX ring is full");

	desc = xsk_ring_prod__tx_desc(&xsk->tx, idx);
	desc->addr = (xsk->tx_head++ % UMEM_QLEN) * UMEM_FRAME_SIZE;
	desc->len = cfg_pkt_len;

	xsk_ring_prod__submit(&xsk->tx, 1);

	if (xsk_ring_prod__needs_wakeup(&xsk->tx))
		sendto(xsk_socket__fd(xsk->socket),
		       NULL,
		       0,
		       MSG_DONTWAIT,
		       NULL,
		       0);
}

static void refill_rx(struct xsk *xsk, __u64 addr)
{
	__u32 idx;

	if (xsk_ring_prod__reserve(&xsk->fill, 1, &idx) == 1) {
		*xsk_ring_prod__fill_addr(&xsk->fill, idx) = addr;
		xsk_ring_prod__submit(&xsk->fill, 1);
	}
}

static bool recv_xsk(bool block)
{
	const struct xdp_desc *desc;
	__u64 comp_addr;
	static int max_descs = 0;
	int pkt_cnt;
	int xsk_fd;
	__u64 addr;
	__u32 idx;

	xsk_fd = xsk_socket__fd(xsk_sock->socket);

	while (1) {
		struct xsk *xsk = xsk_sock;

		++rx_tries;
		pkt_cnt = xsk_ring_cons__peek(&xsk->rx, 1, &idx);
		if (!pkt_cnt) {
			if (!cfg_rx_polling && !cfg_skip_kick)
				recvfrom(xsk_fd, NULL, 0, MSG_DONTWAIT, NULL, NULL);

			++empty_rx_tries;

			if (!block)
				return false;

			continue;
		}

		if (cfg_server_run && max_descs < pkt_cnt) {
			max_descs = pkt_cnt;
			printf("max_desc: %d\n", max_descs);
		}

		pkt_cnt = 1;
		pkt_cnt = idx + pkt_cnt;
		while (idx < pkt_cnt) {
			desc = xsk_ring_cons__rx_desc(&xsk->rx, idx);
			comp_addr = xsk_umem__extract_addr(desc->addr);
			addr = xsk_umem__add_offset_to_addr(desc->addr);

			if (cfg_verify)
				verify_pkt(xsk_umem__get_data(xsk->umem_area, addr), desc->len);

			xsk_ring_cons__release(&xsk->rx, 1);
			refill_rx(xsk, comp_addr);
			++idx;
		}
		return true;
	}
}

static int do_send(void)
{
	if (!cfg_server_run) {
		int ret = record_send();
		if (ret) {
			printf("returning\n");
			return ret;
		}
	}

	send_xsk();

	return 0;
}

static void do_recv(bool block)
{
	bool read;
	static uint64_t until = 0;

	if (cfg_server_delay)
		if (get_current_time() < until)
			return;

	read = recv_xsk(block);
	if (read && !cfg_server_run) {
		record_recv();

		until = cfg_server_delay + get_current_time();
		++done_sends;
	}
}


static void *rx_polling_run(void *arg)
{
	int xsk_fd;

	xsk_fd = xsk_socket__fd(xsk_sock->socket);
	printf("Doing rx polling in separate thread\n");

	while (cfg_rx_polling) {
		if (recvfrom(xsk_fd, NULL, 0, MSG_DONTWAIT, NULL, NULL) < 0)
			error(1, errno, "kick_rx");
	}

	return NULL;
}

static void setup_rx_polling()
{
	printf("Do rx polling in separate thread\n");

	pthread_create(&rx_polling_thread,
		       NULL,
		       rx_polling_run,
		       NULL);
}

static void *do_client_period_send_run(void *arg) {
	uint64_t now, prev = 0;

	while(cfg_client_send_threaded) {
		now = get_current_time();
		if ((now - prev) >= cfg_client_send_period) {
			if (do_send())
				continue;

			++pending_sends;
			prev = now;
		}
	}

	return NULL;
}

static void do_client_send_period()
{
	pthread_t send_thread;
	uint64_t now, prev = 0;

	if (cfg_client_send_threaded) {
		printf("send in separate thread\n");
		pthread_create(&send_thread,
			       NULL,
			       do_client_period_send_run,
			       NULL);
	}

	while (true) {
		if (!cfg_client_send_threaded) {
			now = get_current_time();
			if ((now - prev) >= cfg_client_send_period) {
				do_send();
				++pending_sends;
				prev = now;
			}
		}

		do_recv(false);
	}

	if (cfg_client_send_threaded) {
		void *th_ret;
		cfg_client_send_threaded = false;
		pthread_join(send_thread, &th_ret);
	}
}

static void do_client() {
	int i;

	if (cfg_client_send_period) {
		do_client_send_period();
		return;
	}

	i = 0;
	while (i < cfg_outstanding_pkts) {
		do_send();
		++i;
	}

	while (true) {
		do_send();
		do_recv(true);
	}
}

static void do_server() {
	while (true) {
		do_recv(true);
		if (cfg_server_delay)
			usleep(cfg_server_delay);

		do_send();
	}
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

static void cleanup(void)
{
	cfg_rx_polling = false;
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
	cfg_pkt_len = pkt_len_v4 + cfg_payload_size;
	cfg_daddr = &cfg_daddr4;
	cfg_saddr = &cfg_saddr4;
}

static void setup_for_ipv6(void)
{
	cfg_family = PF_INET6;
	cfg_eth_proto = ETH_P_IPV6;
	cfg_addr_len = sizeof(struct in6_addr);
	cfg_pkt_len = pkt_len_v6 + cfg_payload_size;
	cfg_daddr = &cfg_daddr6;
	cfg_saddr = &cfg_saddr6;
}

static void parse_opts(int argc, char **argv)
{
	char *daddr = NULL, *saddr = NULL;
	int c;

	while ((c = getopt(argc, argv, "46B:D:d:hi:l:m:M:no:p:P:S:tTv")) != -1) {
		switch (c) {
		case '4':
			setup_for_ipv4();
			break;
		case '6':
			setup_for_ipv6();
			break;
		case 'B':
			cfg_payload_size = atoi(optarg);
			cfg_payload_size = MIN(cfg_payload_size,
					       MAX_PAYLOAD_SIZE);
			break;
		case 'D':
			daddr = optarg;
			break;
		case 'd':
			cfg_server_delay = atol(optarg);
			break;
		case 'h':
			cfg_server_run = true;
			break;
		case 'i':
			cfg_ifname = optarg;
			break;
		case 'l':
			cfg_log_f = atol(optarg);
			break;
		case 'm':
			cfg_mac_src = optarg;
			break;
		case 'M':
			cfg_mac_dst = optarg;
			break;
		case 'n':
			cfg_skip_kick = true;
			break;
		case 'o':
			cfg_outstanding_pkts = atoi(optarg);
			break;
		case 'p':
			cfg_port = htons(atoi(optarg));
			break;
		case 'P':
			cfg_client_send_period = atol(optarg);
			break;
		case 'S':
			saddr = optarg;
			break;
		case 't':
			cfg_rx_polling = true;
			break;
		case 'T':
			cfg_client_send_threaded = true;
			break;
		case 'v':
			cfg_verify = true;
			break;
		default:
			error(1, 0, "%s: parse error", argv[0]);
		}
	}

	if (cfg_family == PF_UNSPEC)
		error(1, 0, "select one of -4 or -6");

	if (!cfg_mac_src || !cfg_mac_dst || !saddr || !daddr)
		error(1, 0, "all MAC and IP addresses must be set");

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
	exit(1);
}

int main(int argc, char *argv[])
{
	__u32 queue_id = 0;
	int sock_fd;
	int ret;

	parse_opts(argc, argv);
	init_pkt();

	bpf_obj = xsk_rr_progs__open();
	if (libbpf_get_error(bpf_obj))
		error(1, libbpf_get_error(bpf_obj), "xsk_rr_progs__open");

	ret = xsk_rr_progs__load(bpf_obj);
	if (ret)
		error(1, -ret, "xsk_rr_progs__load");

	xsk_sock = calloc(1, sizeof(struct xsk));

	bpf_obj->bss->port = cfg_port;

	ret = open_xsk(xsk_sock);
	if (ret)
		error(1, -ret, "open_xsk");

	sock_fd = xsk_socket__fd(xsk_sock->socket);

	ret = bpf_map__update_elem(bpf_obj->maps.xsk, &queue_id,
				   sizeof(queue_id), &sock_fd,
				   sizeof(sock_fd), 0);
	if (ret)
		error(1, -ret, "bpf_map__update_elem");

	ret = bpf_xdp_attach(cfg_ifindex,
			     bpf_program__fd(bpf_obj->progs.rx),
			     cfg_xdp_flags, NULL);
	if (ret)
		error(1, -ret, "bpf_xdp_attach");

	atexit(cleanup);
	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	/* XDP may need a delay for device reinitialization */
	do {
		usleep(100 * 1000);
	} while (link_is_down());

	if(cfg_rx_polling)
		setup_rx_polling();

	if (cfg_server_run)
		do_server();
	else
		do_client();
}
