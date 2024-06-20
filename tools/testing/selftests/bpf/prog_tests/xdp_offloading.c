// SPDX-License-Identifier: GPL-2.0
#include <test_progs.h>
#include <network_helpers.h>
#include "xdp_offloading.skel.h"

/* run tcp server in ns1, client in ns2, and transmit 10MB data */
static void run_tcp_test(const char *server_ip)
{
	struct nstoken *ns1 = NULL, *ns2 = NULL;
	struct sockaddr_storage server_addr;
	int total_bytes = 10 * 1024 * 1024;
	int server_fd = -1, client_fd = -1;
	int server_port = 5555;

	socklen_t addrlen = sizeof(server_addr);

	if (!ASSERT_OK(make_sockaddr(AF_INET, server_ip, server_port,
				     &server_addr, &addrlen), "make_addr"))
		goto err;

	ns1 = open_netns("ns1");
	if (!ASSERT_OK_PTR(ns1, "setns ns1"))
		goto err;

	server_fd = start_server_str(AF_INET, SOCK_STREAM, "0.0.0.0",
				     server_port, NULL);
	if (!ASSERT_NEQ(server_fd, -1, "start_server_str"))
		goto err;

	ns2 = open_netns("ns2");
	if (!ASSERT_OK_PTR(ns2, "setns ns2"))
		goto err;

	client_fd = connect_to_addr(SOCK_STREAM, &server_addr, addrlen, NULL);
	if (!ASSERT_NEQ(client_fd, -1, "connect_to_addr"))
		goto err;

	/* send 10MB data */
	if (!ASSERT_OK(send_recv_data(server_fd, client_fd, total_bytes),
		       "send_recv_data"))
		goto err;

err:
	if (server_fd != -1)
		close(server_fd);
	if (client_fd != -1)
		close(client_fd);
	if (ns1)
		close_netns(ns1);
	if (ns2)
		close_netns(ns2);
}

/* This test involves two netns:
 *     NS1              |           NS2
 *                      |
 *        ---->  veth1 --> veth_offloading(xdp)-->(tp:netif_receive_skb)
 *        |             |                              |
 *        |             |                              v
 *    tcp-server        |                         tcp-client
 *
 *   a TCP server in NS1 sends data through veth1, and the XDP program on
 *   "xdp_offloading" is what we test against. This XDP program will apply
 *   offloadings and we examine these at netif_receive_skb tracepoint if the
 *   offloadings are propagated to skbs.
 */
void test_xdp_offloading(void)
{
	const char *xdp_ifname = "veth_offloading";
	struct nstoken *nstoken = NULL;
	struct xdp_offloading *skel = NULL;
	struct bpf_link *link_xdp, *link_tp;
	const char *server_ip = "192.168.0.2";
	const char *client_ip = "192.168.0.3";
	int ifindex;

	SYS(out, "ip netns add ns1");
	SYS(out, "ip netns add ns2");
	SYS(out, "ip -n ns1 link add veth1 type veth peer name %s netns ns2",
	    xdp_ifname);
	SYS(out, "ip -n ns1 link set veth1 up");
	SYS(out, "ip -n ns2 link set veth_offloading up");
	SYS(out, "ip -n ns1 addr add dev veth1 %s/31", server_ip);
	SYS(out, "ip -n ns2 addr add dev %s %s/31", xdp_ifname, client_ip);

	SYS(out, "ip netns exec ns2 ethtool -K %s gro on", xdp_ifname);

	nstoken = open_netns("ns2");
	if (!ASSERT_OK_PTR(nstoken, "setns"))
		goto out;

	skel = xdp_offloading__open();
	if (!ASSERT_OK_PTR(skel, "skel"))
		return;

	ifindex = if_nametoindex(xdp_ifname);
	if (!ASSERT_NEQ(ifindex, 0, "ifindex"))
		goto out;

	memcpy(skel->rodata->target_ifname, xdp_ifname, IFNAMSIZ);

	if (!ASSERT_OK(xdp_offloading__load(skel), "load"))
		goto out;

	link_xdp = bpf_program__attach_xdp(skel->progs.xdp_disable_gro, ifindex);
	if (!ASSERT_OK_PTR(link_xdp, "xdp_attach"))
		goto out;

	link_tp = bpf_program__attach(skel->progs.observe_skb_gro_disabled);
	if (!ASSERT_OK_PTR(link_tp, "xdp_attach"))
		goto out;

	run_tcp_test(server_ip);

	ASSERT_NEQ(__sync_fetch_and_add(&skel->bss->invalid_skb, 0), 0,
		   "check invalid skbs");
out:
	if (nstoken)
		close_netns(nstoken);
	SYS_NOFAIL("ip netns del ns1");
	SYS_NOFAIL("ip netns del ns2");
}
