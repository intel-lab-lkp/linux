// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2020 Intel Corporation. */

/*
 * Some functions in this program are taken from
 * Linux kernel samples/bpf/xdpsock* and modified
 * for use.
 *
 * See test_xsk.sh for detailed information on test topology
 * and prerequisite network setup.
 *
 * See README.rst for the generic and hardware test setup.
 *
 * Each instance of this test program runs one endpoint, Tx or Rx, with a
 * single socket and a unique UMEM. Two instances validate in-order packet
 * delivery and packet content by sending packets to each other.
 *
 * Tests Information:
 * ------------------
 * These selftests test AF_XDP SKB and Native/DRV modes using veth
 * Virtual Ethernet interfaces.
 *
 * For each mode, the following tests are run:
 *    a. nopoll - soft-irq processing in run-to-completion mode
 *    b. poll - using poll() syscall
 *    c. Socket Teardown
 *       Create a Tx and a Rx socket, Tx from one socket, Rx on another. Destroy
 *       both sockets, then repeat multiple times. Only nopoll mode is used
 *    d. Bi-directional sockets
 *       Configure sockets as bi-directional tx/rx sockets, sets up fill and
 *       completion rings on each socket, tx/rx in both directions. Only nopoll
 *       mode is used
 *    e. Statistics
 *       Trigger some error conditions and ensure that the appropriate statistics
 *       are incremented. Within this test, the following statistics are tested:
 *       i.   rx dropped
 *            Increase the UMEM frame headroom to a value which results in
 *            insufficient space in the rx buffer for both the packet and the headroom.
 *       ii.  tx invalid
 *            Set the 'len' field of tx descriptors to an invalid value (umem frame
 *            size + 1).
 *       iii. rx ring full
 *            Reduce the size of the RX ring to a fraction of the fill ring size.
 *       iv.  fill queue empty
 *            Do not populate the fill queue and then try to receive pkts.
 *    f. bpf_link resource persistence
 *       Configure sockets at indexes 0 and 1, run a traffic on queue ids 0,
 *       then remove xsk sockets from queue 0 on both veth interfaces and
 *       finally run a traffic on queues ids 1
 *    g. unaligned mode
 *    h. tests for invalid and corner case Tx descriptors so that the correct ones
 *       are discarded and let through, respectively.
 *    i. 2K frame size tests
 *    j. If multi-buffer is supported, send 9k packets divided into 3 frames
 *    k. If multi-buffer and huge pages are supported, send 9k packets in a single frame
 *       using unaligned mode
 *    l. If multi-buffer is supported, try various nasty combinations of descriptors to
 *       check if they pass the validation or not
 *
 * drivers/net/hw/xsk.py runs the hardware cases on a physical device in
 * zero-copy mode, with an SKB-mode peer on a remote host.
 *
 * Flow:
 * -----
 * - test_xsk.sh starts two processes: Tx and Rx
 * - Each of these two processes attach to a veth interface
 * - Each process creates one AF_XDP socket connected to a unique umem for each
 *   veth interface
 * - Tx process listens on a TCP control port, Rx process connects to it and
 *   the two synchronize each test step over that connection
 * - Tx process Transmits a number of packets from veth<xxxx> to veth<yyyy>
 * - Rx process verifies if all packets were received and delivered in-order,
 *   and have the right content
 *
 * Enable/disable packet dump mode:
 * --------------------------
 * To enable L2 - L4 headers and payload dump of each packet on STDOUT, add
 * parameter -D to params array in test_xsk.sh, i.e. params=("-S" "-D")
 */

#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/if_link.h>
#include <linux/if_ether.h>
#include <linux/mman.h>
#include <linux/netdev.h>
#include <linux/ethtool.h>
#include <linux/align.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/ether.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <libgen.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/types.h>

#include "test_xsk.h"
#include "xsk_peer.h"
#include "xsk_xdp_progs.skel.h"
#include "xsk.h"
#include "xskxceiver.h"
#include <bpf/bpf.h>
#include <linux/filter.h>
#include "kselftest.h"
#include "xsk_xdp_common.h"

enum xsk_endpoint_role {
	XSK_ENDPOINT_NONE,
	XSK_ENDPOINT_RX,
	XSK_ENDPOINT_TX,
};

static enum test_mode opt_mode = TEST_MODE_ALL;
static u32 opt_run_test = RUN_ALL_TESTS;
static enum xsk_endpoint_role opt_endpoint_role = XSK_ENDPOINT_NONE;
static bool opt_hw;
static bool opt_listen;
static const char *opt_peer_host;
static const char *opt_peer_port;
static const char *opt_ifname;
static const char *opt_udp_src;
static const char *opt_udp_dst;
static const struct ether_addr *opt_peer_mac;
static struct ether_addr peer_mac;
static u16 opt_udp_port;
static u32 opt_queue;
static u32 opt_max_frags;

enum {
	OPT_HW = 256,
	OPT_LISTEN,
	OPT_UDP_SRC,
	OPT_UDP_DST,
	OPT_UDP_PORT,
	OPT_PEER_MAC,
	OPT_QUEUE,
	OPT_MAX_FRAGS,
};

static void __exit_with_error(int error, const char *file, const char *func, int line)
{
	ksft_test_result_fail("[%s:%s:%i]: ERROR: %d/\"%s\"\n", file, func, line,
			      error, strerror(error));
	ksft_exit_xfail();
}

#define exit_with_error(error) __exit_with_error(error, __FILE__, __func__, __LINE__)

static bool ifobj_zc_avail(struct ifobject *ifobject)
{
	size_t umem_sz = DEFAULT_UMEM_BUFFERS * XSK_UMEM__DEFAULT_FRAME_SIZE;
	int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
	struct xsk_socket_info *xsk;
	struct xsk_umem_info *umem;
	bool zc_avail = false;
	void *bufs;
	int ret;

	bufs = mmap(NULL, umem_sz, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);
	if (bufs == MAP_FAILED)
		exit_with_error(errno);

	umem = calloc(1, sizeof(struct xsk_umem_info));
	if (!umem) {
		munmap(bufs, umem_sz);
		exit_with_error(ENOMEM);
	}
	umem->frame_size = XSK_UMEM__DEFAULT_FRAME_SIZE;
	ret = xsk_configure_umem(ifobject, umem, bufs, umem_sz);
	if (ret)
		exit_with_error(-ret);

	xsk = calloc(1, sizeof(struct xsk_socket_info));
	if (!xsk)
		goto out;
	ifobject->bind_flags = XDP_USE_NEED_WAKEUP | XDP_ZEROCOPY;
	ifobject->rx_on = true;
	xsk->rxqsize = XSK_RING_CONS__DEFAULT_NUM_DESCS;
	ret = xsk_configure_socket(xsk, umem, ifobject, false);
	if (!ret)
		zc_avail = true;

	xsk_socket__delete(xsk->xsk);
	free(xsk);
out:
	munmap(umem->buffer, umem_sz);
	xsk_umem__delete(umem->umem);
	free(umem);
	return zc_avail;
}

static struct option long_options[] = {
	{"interface", required_argument, 0, 'i'},
	{"busy-poll", no_argument, 0, 'b'},
	{"verbose", no_argument, 0, 'v'},
	{"mode", required_argument, 0, 'm'},
	{"list", no_argument, 0, 'l'},
	{"test", required_argument, 0, 't'},
	{"endpoint", required_argument, 0, 'e'},
	{"peer", required_argument, 0, 'p'},
	{"peer-port", required_argument, 0, 'P'},
	{"hw", no_argument, 0, OPT_HW},
	{"listen", no_argument, 0, OPT_LISTEN},
	{"udp-src", required_argument, 0, OPT_UDP_SRC},
	{"udp-dst", required_argument, 0, OPT_UDP_DST},
	{"udp-port", required_argument, 0, OPT_UDP_PORT},
	{"peer-mac", required_argument, 0, OPT_PEER_MAC},
	{"queue", required_argument, 0, OPT_QUEUE},
	{"max-frags", required_argument, 0, OPT_MAX_FRAGS},
	{"help", no_argument, 0, 'h'},
	{0, 0, 0, 0}
};

static void print_usage(char **argv)
{
	const char *str =
		"  Usage: xskxceiver -i IFACE -e rx|tx -m MODE -t TEST -p HOST -P PORT [OPTIONS]\n"
		"  Options:\n"
		"  -i, --interface      Use interface\n"
		"  -v, --verbose        Verbose output\n"
		"  -b, --busy-poll      Enable busy poll\n"
		"  -m, --mode           Run only mode skb, drv, or zc\n"
		"  -l, --list           List all available tests\n"
		"  -t, --test           Run a specific test. Enter number from -l option.\n"
		"  -e, --endpoint       Role of this endpoint: rx or tx\n"
		"  -p, --peer           Control host: IPv4/IPv6 address or hostname\n"
		"  -P, --peer-port      Control TCP port (1-65535)\n"
		"      --listen         Listen instead of connecting (hardware peer)\n"
		"      --hw             One physical-link case, controlled by xsk.py\n"
		"      --udp-src IP     IPv4 source address for test packets\n"
		"      --udp-dst IP     IPv4 destination address for test packets\n"
		"      --udp-port PORT  UDP source and destination port\n"
		"      --peer-mac MAC   Remote interface MAC address\n"
		"      --queue N        AF_XDP queue to bind (default 0)\n"
		"      --max-frags N    Fragment limit selected by the hardware runner\n"
		"  -h, --help           Display this help and exit\n";

	ksft_print_msg(str, basename(argv[0]));
	ksft_exit_xfail();
}

static void bind_iface(struct ifobject *ifobj, const char *ifname, char **argv)
{
	size_t len = strlen(ifname);

	if (!len || len >= sizeof(ifobj->ifname))
		print_usage(argv);

	memcpy(ifobj->ifname, ifname, len + 1);
	ifobj->ifindex = if_nametoindex(ifobj->ifname);
	if (!ifobj->ifindex) {
		ksft_print_msg("Error: cannot resolve interface %s\n", ifobj->ifname);
		ksft_exit_fail();
	}

	if (init_iface(ifobj)) {
		ksft_print_msg("Error: cannot initialize interface %s\n", ifobj->ifname);
		ksft_exit_fail();
	}
	if (xsk_get_mtu(ifobj->ifname, &ifobj->mtu)) {
		ksft_print_msg("Error: cannot read MTU of interface %s\n", ifobj->ifname);
		ksft_exit_fail();
	}
	ifobj->dev_mtu = ifobj->mtu;
	ifobj->orig_mtu = ifobj->mtu;
}

static void print_tests(void)
{
	u32 i;

	printf("Tests:\n");
	for (i = 0; i < ARRAY_SIZE(tests); i++)
		printf("%u: %s\n", i, tests[i].name);
}

static u32 parse_u32(const char *arg, u32 min, u32 max, char **argv)
{
	unsigned long val;
	char *end;

	errno = 0;
	val = strtoul(arg, &end, 10);
	if (arg[0] < '0' || arg[0] > '9' || errno || *end || val < min || val > max)
		print_usage(argv);
	return val;
}

static void parse_command_line(struct ifobject *ifobj_tx, struct ifobject *ifobj_rx, int argc,
			       char **argv)
{
	int option_index, c;

	opterr = 0;

	for (;;) {
		c = getopt_long(argc, argv, "i:vbm:lt:e:p:P:", long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case 'i':
			opt_ifname = optarg;
			break;
		case 'v':
			opt_verbose = true;
			break;
		case 'b':
			ifobj_tx->busy_poll = true;
			ifobj_rx->busy_poll = true;
			break;
		case 'm':
			if (!strncmp("skb", optarg, strlen(optarg)))
				opt_mode = TEST_MODE_SKB;
			else if (!strncmp("drv", optarg, strlen(optarg)))
				opt_mode = TEST_MODE_DRV;
			else if (!strncmp("zc", optarg, strlen(optarg)))
				opt_mode = TEST_MODE_ZC;
			else
				print_usage(argv);
			break;
		case 'l':
			print_tests();
			ksft_exit_pass();
			break;
		case 't':
			errno = 0;
			opt_run_test = strtol(optarg, NULL, 0);
			if (errno)
				print_usage(argv);
			break;
		case 'e':
			if (!strcmp(optarg, "rx"))
				opt_endpoint_role = XSK_ENDPOINT_RX;
			else if (!strcmp(optarg, "tx"))
				opt_endpoint_role = XSK_ENDPOINT_TX;
			else
				print_usage(argv);
			break;
		case 'p':
			opt_peer_host = optarg;
			break;
		case 'P':
			opt_peer_port = optarg;
			break;
		case OPT_HW:
			opt_hw = true;
			break;
		case OPT_LISTEN:
			opt_listen = true;
			break;
		case OPT_UDP_SRC:
			opt_udp_src = optarg;
			break;
		case OPT_UDP_DST:
			opt_udp_dst = optarg;
			break;
		case OPT_UDP_PORT:
			opt_udp_port = parse_u32(optarg, 1, UINT16_MAX, argv);
			break;
		case OPT_PEER_MAC:
			opt_peer_mac = ether_aton_r(optarg, &peer_mac);
			if (!opt_peer_mac)
				print_usage(argv);
			break;
		case OPT_QUEUE:
			opt_queue = parse_u32(optarg, 0, UINT32_MAX, argv);
			break;
		case OPT_MAX_FRAGS:
			opt_max_frags = parse_u32(optarg, 1, UINT16_MAX, argv);
			break;
		case 'h':
		default:
			print_usage(argv);
		}
	}

	if (!opt_ifname || opt_endpoint_role == XSK_ENDPOINT_NONE || !opt_peer_host ||
	    !*opt_peer_host || !opt_peer_port || opt_run_test == RUN_ALL_TESTS ||
	    opt_mode == TEST_MODE_ALL)
		print_usage(argv);
	if (opt_hw && (!opt_udp_src || !opt_udp_dst || !opt_udp_port || !opt_peer_mac))
		print_usage(argv);
	if ((opt_listen || opt_max_frags) && !opt_hw)
		print_usage(argv);
}

static void xsk_unload_xdp_programs(struct ifobject *ifobj)
{
	xsk_xdp_progs__destroy(ifobj->xdp_progs);
}

static void report_pkt_test(struct test_spec *test, int ret)
{
	switch (ret) {
	case TEST_PASS:
		ksft_test_result_pass("PASS: %s %s%s\n", mode_string(test), busy_poll_string(test),
				      test->name);
		break;
	case TEST_SKIP:
		ksft_test_result_skip("SKIP: %s %s%s\n", mode_string(test), busy_poll_string(test),
				      test->name);
		break;
	case TEST_FAILURE:
		ksft_test_result_fail("FAIL: %s %s%s\n", mode_string(test), busy_poll_string(test),
				      test->name);
		break;
	default:
		ksft_test_result_fail("FAIL: %s %s%s -- Unexpected returned value (%d)\n",
				      mode_string(test), busy_poll_string(test), test->name, ret);
	}
}

static int run_pkt_test(struct test_spec *test)
{
	int ret;

	ret = test->test_func(test);
	if (!opt_hw)
		report_pkt_test(test, ret);
	pkt_stream_restore_default(test);
	return ret;
}

static bool is_xdp_supported(int ifindex)
{
	int flags = XDP_FLAGS_DRV_MODE;

	LIBBPF_OPTS(bpf_link_create_opts, opts, .flags = flags);
	struct bpf_insn insns[2] = {
		BPF_MOV64_IMM(BPF_REG_0, XDP_PASS),
		BPF_EXIT_INSN()
	};
	int prog_fd, insn_cnt = ARRAY_SIZE(insns);
	int err;

	prog_fd = bpf_prog_load(BPF_PROG_TYPE_XDP, NULL, "GPL", insns, insn_cnt, NULL);
	if (prog_fd < 0)
		return false;

	err = bpf_xdp_attach(ifindex, prog_fd, flags, NULL);
	if (err) {
		close(prog_fd);
		return false;
	}

	bpf_xdp_detach(ifindex, flags, NULL);
	close(prog_fd);

	return true;
}

static u32 detect_mode_caps(struct ifobject *ifobj)
{
	if (opt_hw && opt_mode == TEST_MODE_ZC &&
	    ifobj_has_cap(ifobj, XSK_CAP_ZC_ADVERTISED)) {
		/* Trust the advertised flag; an XDP_ZEROCOPY bind never falls back to copy. */
		ifobj_set_cap(ifobj, XSK_CAP_DRV | XSK_CAP_ZC);
	} else if (!opt_hw && is_xdp_supported(ifobj->ifindex)) {
		ifobj_set_cap(ifobj, XSK_CAP_DRV);
		if (ifobj_zc_avail(ifobj))
			ifobj_set_cap(ifobj, XSK_CAP_ZC);
	}

	return ifobj->caps.flags;
}

static bool mode_supported(enum test_mode mode, u32 caps)
{
	if (mode == TEST_MODE_SKB)
		return true;
	if (mode == TEST_MODE_DRV)
		return caps & XSK_CAP_DRV;
	return caps & XSK_CAP_ZC;
}

static int cleanup_iface(struct ifobject *ifobj)
{
	int err = 0;

	/* A peer shadow has a skeleton but no bound interface. */
	if (!ifobj_is_local(ifobj))
		goto unload;

	if (ifobj_has_cap(ifobj, XSK_CAP_HW_RING))
		hw_ring_size_reset(ifobj);
	if (ifobj->xdp_prog)
		xsk_detach_xdp_program(ifobj->ifindex,
				       ifobj->mode == TEST_MODE_SKB ?
				       XDP_FLAGS_SKB_MODE : XDP_FLAGS_DRV_MODE);
	/* After the detach, as an attached program can cap the MTU. */
	if (ifobj->dev_mtu != ifobj->orig_mtu) {
		err = xsk_set_mtu(ifobj->ifindex, ifobj->orig_mtu);
		if (err)
			ksft_print_msg("Failed to restore MTU %d on %s\n",
				       ifobj->orig_mtu, ifobj->ifname);
	}
unload:
	xsk_unload_xdp_programs(ifobj);
	return err;
}

/* Connect the two endpoints without negotiating the remote NIC's capabilities. */
static int setup_peer(struct ifobject *local, struct ifobject *shadow,
		      struct xsk_peer **peer)
{
	/* Unattached skeleton copy; the engine only attaches the local side. */
	if (xsk_load_xdp_programs(shadow))
		return TEST_FAILURE;

	/* Generic TX listens; Python chooses the listener for hardware cases. */
	*peer = xsk_peer_open(opt_peer_host, opt_peer_port,
			      opt_hw ? opt_listen :
			      opt_endpoint_role == XSK_ENDPOINT_TX);
	if (!*peer) {
		ksft_print_msg("Failed to connect XSK peer: %s\n", strerror(errno));
		return TEST_FAILURE;
	}

	shadow->caps = local->caps;
	if (opt_hw)
		memcpy(shadow->caps.mac, opt_peer_mac->ether_addr_octet, ETH_ALEN);

	xsk_set_endpoint(*peer, opt_hw);
	return TEST_PASS;
}

int main(int argc, char **argv)
{
	u32 cache_line_size, max_frags, umem_tailroom;
	const size_t total_tests = ARRAY_SIZE(tests);
	struct ifobject *ifobj_tx, *ifobj_rx;
	struct ifobject *shadow_ifobj;
	struct ifobject *local_ifobj;
	struct xsk_peer *peer = NULL;
	struct test_spec test = {};
	int ret = TEST_FAILURE;
	u32 caps;
	int err;

	/* Use libbpf 1.0 API mode */
	libbpf_set_strict_mode(LIBBPF_STRICT_ALL);

	ifobj_tx = ifobject_create();
	if (!ifobj_tx)
		exit_with_error(ENOMEM);
	ifobj_rx = ifobject_create();
	if (!ifobj_rx)
		exit_with_error(ENOMEM);

	setlocale(LC_ALL, "");

	cache_line_size = read_procfs_val(SMP_CACHE_BYTES_PATH);
	if (!cache_line_size) {
		ksft_print_msg("Can't get SMP_CACHE_BYTES from system, using default (64)\n");
		cache_line_size = 64;
	}

	max_frags = read_procfs_val(MAX_SKB_FRAGS_PATH);
	if (!max_frags) {
		ksft_print_msg("Can't get MAX_SKB_FRAGS from system, using default (17)\n");
		max_frags = 17;
	}
	ifobj_tx->caps.max_skb_frags = max_frags;
	ifobj_rx->caps.max_skb_frags = max_frags;

	/* 48 bytes is a part of skb_shared_info w/o frags array;
	 * 16 bytes is sizeof(skb_frag_t)
	 */
	umem_tailroom = ALIGN(48 + (max_frags * 16), cache_line_size);
	ifobj_tx->caps.umem_tailroom = umem_tailroom;
	ifobj_rx->caps.umem_tailroom = umem_tailroom;

	parse_command_line(ifobj_tx, ifobj_rx, argc, argv);

	if (opt_run_test != RUN_ALL_TESTS && opt_run_test >= total_tests) {
		ksft_print_msg("Error: test %u does not exist.\n", opt_run_test);
		ksft_exit_xfail();
	}

	xsk_set_max_frags(opt_max_frags);
	if (opt_hw && xsk_set_udp_packet_format(opt_udp_src, opt_udp_dst,
						opt_udp_port))
		print_usage(argv);

	/* swap_directions() swaps the workers, so the shadow needs one too. */
	ifobj_tx->func_ptr = worker_testapp_validate_tx;
	ifobj_rx->func_ptr = worker_testapp_validate_rx;
	if (opt_endpoint_role == XSK_ENDPOINT_TX) {
		local_ifobj = ifobj_tx;
		shadow_ifobj = ifobj_rx;
	} else {
		local_ifobj = ifobj_rx;
		shadow_ifobj = ifobj_tx;
	}
	bind_iface(local_ifobj, opt_ifname, argv);
	local_ifobj->queue_id = opt_queue;
	/* The zero-copy probe binds to queue_id, so set it first. */
	caps = detect_mode_caps(local_ifobj);

	test.tx_pkt_stream_default = pkt_stream_generate(DEFAULT_PKT_CNT, MIN_PKT_SIZE);
	test.rx_pkt_stream_default = pkt_stream_generate(DEFAULT_PKT_CNT, MIN_PKT_SIZE);
	if (!test.tx_pkt_stream_default || !test.rx_pkt_stream_default)
		goto out;

	if (setup_peer(local_ifobj, shadow_ifobj, &peer))
		goto out;

	if (!mode_supported(opt_mode, caps)) {
		if (opt_mode == TEST_MODE_DRV)
			ksft_print_msg("XDP_DRV mode not supported.\n");
		else
			ksft_print_msg("zero-copy mode not supported.\n");
		ret = TEST_SKIP;
		goto out;
	}

	/* Line-buffer stdout so verdicts reach a capturing launcher live. */
	if (!opt_hw) {
		ksft_print_header();
		ksft_set_plan(1);
	}
	test_init(&test, ifobj_tx, ifobj_rx, opt_mode, &tests[opt_run_test]);
	ret = run_pkt_test(&test);

out:
	xsk_set_endpoint(NULL, false);
	err = cleanup_iface(ifobj_tx);
	err |= cleanup_iface(ifobj_rx);
	/* xsk.py trusts a passed or skipped endpoint to have undone the MTU. */
	if (err)
		ret = TEST_FAILURE;
	xsk_peer_close(peer);
	pkt_stream_delete(test.tx_pkt_stream_default);
	pkt_stream_delete(test.rx_pkt_stream_default);
	ifobject_delete(ifobj_tx);
	ifobject_delete(ifobj_rx);

	if (ret == TEST_SKIP && !opt_hw && ksft_test_num()) {
		ksft_print_cnts();
		return KSFT_SKIP;
	}
	if (ret == TEST_SKIP)
		ksft_exit_skip("mode or test not supported\n");
	if (ret)
		ksft_exit_fail();
	else
		ksft_exit_pass();
}
