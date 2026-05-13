// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * KUnit harness for siw MPA receive-state length validation.
 *
 * Internal to the SIW_MPA_LEN_UNDERFLOW_RX_COPY disclosure validation tree.
 * Not part of the upstream patch.
 *
 *   case 1: short mpa_len triggers the new siw_get_hdr() check via direct
 *           siw_tcp_rx_data() call with a constructed sk_buff
 *           - expects: TERM(LLP/MPA/FPDU_START), rx_suspend=1
 *           - under stock siw: KASAN slab-out-of-bounds in skb_copy_bits()
 *           - under patched siw: no splat, terminate state set
 *
 *   case 2: minimum-valid mpa_len control (constructed sk_buff)
 *           - mpa_len = hdr_len - MPA_HDR_SIZE  ->  fpdu_part_rem = 0
 *             so siw_proc_write() takes the zero-length WRITE short path
 *             and returns 0 without calling skb_copy_bits().
 *           - expects: no TERM, state machine progressed normally
 *
 *   case 3: real loopback TCP socketpair (the "live two-node" analog)
 *           - opens AF_INET TCP sockets in-kernel via sock_create_kern()
 *           - binds/listens on 127.0.0.1:0, connects, accepts
 *           - installs siw_qp_llp_data_ready on the victim socket and
 *             attaches a struct siw_cep so sk_to_qp() resolves to our qp
 *           - writes the malformed FPDU bytes via kernel_sendmsg on the
 *             attacker socket
 *           - the kernel TCP stack delivers, sk_data_ready fires, and
 *             siw_qp_llp_data_ready -> tcp_read_sock -> siw_tcp_rx_data
 *             runs in the normal kernel receive path
 *           - expects: TERM(LLP/MPA/FPDU_START) on the qp
 */

#include <kunit/test.h>
#include <linux/inet.h>
#include <linux/in.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/skbuff.h>
#include <linux/tcp.h>
#include <linux/wait.h>
#include <linux/xarray.h>
#include <net/sock.h>
#include <net/tcp.h>
#include <rdma/ib_verbs.h>

#include "siw.h"
#include "siw_cm.h"
#include "siw_mem.h"

static void siw_kunit_kfree_skb(void *skb)
{
	kfree_skb(skb);
}

struct siw_mpa_rx_ctx {
	struct siw_device *sdev;
	struct siw_qp *qp;
	struct siw_mem *mem;
	void *target;
	u32 stag;
};

static void siw_mpa_rx_setup(struct kunit *test, struct siw_mpa_rx_ctx *c)
{
	void *xa_ret;

	c->stag = 0x00000100;

	c->sdev = kunit_kzalloc(test, sizeof(*c->sdev), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, c->sdev);
	xa_init_flags(&c->sdev->mem_xa, XA_FLAGS_ALLOC1);

	c->qp = kunit_kzalloc(test, sizeof(*c->qp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, c->qp);
	c->qp->sdev = c->sdev;
	c->qp->pd = kunit_kzalloc(test, sizeof(*c->qp->pd), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, c->qp->pd);
	c->qp->rx_stream.state = SIW_GET_HDR;

	c->target = kunit_kzalloc(test, 64, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, c->target);

	c->mem = kunit_kzalloc(test, sizeof(*c->mem), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, c->mem);
	kref_init(&c->mem->ref);
	c->mem->sdev = c->sdev;
	c->mem->stag = c->stag;
	c->mem->stag_valid = 1;
	c->mem->va = (u64)(uintptr_t)c->target;
	c->mem->len = 64;
	c->mem->pd = c->qp->pd;
	c->mem->perms = IB_ACCESS_REMOTE_WRITE;

	xa_ret = xa_store(&c->sdev->mem_xa, c->stag >> 8, c->mem, GFP_KERNEL);
	KUNIT_ASSERT_FALSE(test, xa_is_err(xa_ret));
}

static void siw_mpa_rx_run(struct kunit *test, struct siw_mpa_rx_ctx *c,
			   u16 mpa_len_val)
{
	struct iwarp_rdma_write write = { };
	struct sk_buff *skb;
	read_descriptor_t rd_desc = { };
	u8 payload[sizeof(write) + 1];

	write.ctrl.mpa_len = cpu_to_be16(mpa_len_val);
	write.ctrl.ddp_rdmap_ctrl = DDP_FLAG_TAGGED | DDP_FLAG_LAST |
		cpu_to_be16(DDP_VERSION << 8) |
		cpu_to_be16(RDMAP_VERSION << 6) |
		cpu_to_be16(RDMAP_RDMA_WRITE);
	write.sink_stag = cpu_to_be32(c->stag);
	write.sink_to = cpu_to_be64((u64)(uintptr_t)c->target);

	memcpy(payload, &write, sizeof(write));
	payload[sizeof(write)] = 0x41;

	skb = alloc_skb(sizeof(payload), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, skb);
	skb_put_data(skb, payload, sizeof(payload));
	kunit_add_action_or_reset(test, siw_kunit_kfree_skb, skb);

	rd_desc.arg.data = c->qp;
	rd_desc.count = sizeof(payload);

	siw_tcp_rx_data(&rd_desc, skb, 0, sizeof(payload));
}

static void siw_mpa_write_underflow_rejected(struct kunit *test)
{
	struct siw_mpa_rx_ctx c;

	siw_mpa_rx_setup(test, &c);

	/* mpa_len one byte short of the WRITE hdr_len - MPA_HDR_SIZE floor. */
	siw_mpa_rx_run(test, &c,
		       sizeof(struct iwarp_rdma_write) - MPA_HDR_SIZE - 1);

	KUNIT_EXPECT_EQ(test, (int)c.qp->term_info.valid, 1);
	KUNIT_EXPECT_EQ(test, (int)c.qp->term_info.layer,
			(int)TERM_ERROR_LAYER_LLP);
	KUNIT_EXPECT_EQ(test, (int)c.qp->term_info.etype,
			(int)LLP_ETYPE_MPA);
	KUNIT_EXPECT_EQ(test, (int)c.qp->term_info.ecode,
			(int)LLP_ECODE_FPDU_START);
	KUNIT_EXPECT_EQ(test, (int)c.qp->rx_stream.rx_suspend, 1);
}

static void siw_mpa_write_minimum_valid_accepted(struct kunit *test)
{
	struct siw_mpa_rx_ctx c;

	siw_mpa_rx_setup(test, &c);

	/*
	 * mpa_len == hdr_len - MPA_HDR_SIZE is the smallest legal value;
	 * it yields fpdu_part_rem = 0, i.e. a zero-length WRITE. The new
	 * length check in siw_get_hdr() must accept this.
	 */
	siw_mpa_rx_run(test, &c,
		       sizeof(struct iwarp_rdma_write) - MPA_HDR_SIZE);

	KUNIT_EXPECT_EQ(test, (int)c.qp->term_info.valid, 0);
	KUNIT_EXPECT_EQ(test, (int)c.qp->rx_stream.rx_suspend, 0);
}

static int siw_mpa_rx_bring_up_lo(struct kunit *test)
{
	struct net_device *lo;
	int rv;

	rtnl_lock();
	lo = __dev_get_by_name(&init_net, "lo");
	KUNIT_ASSERT_NOT_NULL(test, lo);
	if (!(lo->flags & IFF_UP))
		rv = dev_change_flags(lo, lo->flags | IFF_UP, NULL);
	else
		rv = 0;
	rtnl_unlock();
	KUNIT_ASSERT_EQ(test, rv, 0);
	return 0;
}

static int siw_mpa_rx_make_pair(struct kunit *test, struct socket **listen,
				struct socket **server, struct socket **client)
{
	struct sockaddr_in addr = { .sin_family = AF_INET, };
	struct sockaddr_in bound = { };
	struct socket *l = NULL, *s = NULL, *c = NULL;
	int rv;

	siw_mpa_rx_bring_up_lo(test);

	rv = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &l);
	KUNIT_ASSERT_EQ(test, rv, 0);

	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	rv = kernel_bind(l, (struct sockaddr_unsized *)&addr, sizeof(addr));
	KUNIT_ASSERT_EQ(test, rv, 0);

	rv = l->ops->getname(l, (struct sockaddr *)&bound, 0);
	KUNIT_ASSERT_GT(test, rv, 0);

	rv = kernel_listen(l, 1);
	KUNIT_ASSERT_EQ(test, rv, 0);

	rv = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &c);
	KUNIT_ASSERT_EQ(test, rv, 0);

	rv = kernel_connect(c, (struct sockaddr_unsized *)&bound,
			    sizeof(bound), 0);
	KUNIT_ASSERT_EQ(test, rv, 0);

	rv = kernel_accept(l, &s, 0);
	KUNIT_ASSERT_EQ(test, rv, 0);

	*listen = l;
	*server = s;
	*client = c;
	return 0;
}

static void siw_mpa_write_underflow_rejected_live_socket(struct kunit *test)
{
	struct siw_device *sdev;
	struct siw_qp *qp;
	struct siw_cep *cep;
	struct siw_mem *mem;
	struct socket *listen_sock = NULL, *server_sock = NULL, *client_sock = NULL;
	struct iwarp_rdma_write write = { };
	struct kvec iov;
	struct msghdr msg = { };
	void *xa_ret, *target;
	u8 payload[sizeof(write) + 1];
	u32 stag = 0x00000100;
	int rv, i;

	sdev = kunit_kzalloc(test, sizeof(*sdev), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, sdev);
	xa_init_flags(&sdev->mem_xa, XA_FLAGS_ALLOC1);

	qp = kunit_kzalloc(test, sizeof(*qp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, qp);
	qp->sdev = sdev;
	qp->pd = kunit_kzalloc(test, sizeof(*qp->pd), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, qp->pd);
	qp->rx_stream.state = SIW_GET_HDR;
	init_rwsem(&qp->state_lock);
	qp->attrs.state = SIW_QP_STATE_RTS;
	qp->cep = NULL;

	/* Register a valid REMOTE_WRITE memory object. On unpatched siw
	 * this is what lets the negative-length copy reach skb_copy_bits;
	 * without an MR the STag lookup in siw_proc_write() returns NULL
	 * and the WRITE is terminated before the underflow primitive fires.
	 * With this patch in place, the new siw_get_hdr() check rejects
	 * the FPDU BEFORE STag lookup, so the MR is unused. We keep it in
	 * the test so unpatched-kernel reruns also exercise the full path.
	 */
	target = kunit_kzalloc(test, 64, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, target);
	mem = kunit_kzalloc(test, sizeof(*mem), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, mem);
	kref_init(&mem->ref);
	mem->sdev = sdev;
	mem->stag = stag;
	mem->stag_valid = 1;
	mem->va = (u64)(uintptr_t)target;
	mem->len = 64;
	mem->pd = qp->pd;
	mem->perms = IB_ACCESS_REMOTE_WRITE;
	xa_ret = xa_store(&sdev->mem_xa, stag >> 8, mem, GFP_KERNEL);
	KUNIT_ASSERT_FALSE(test, xa_is_err(xa_ret));

	/* siw_qp_llp_data_ready dereferences sk_user_data as siw_cep. */
	cep = kunit_kzalloc(test, sizeof(*cep), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, cep);
	cep->qp = qp;
	spin_lock_init(&cep->lock);
	kref_init(&cep->ref);

	rv = siw_mpa_rx_make_pair(test, &listen_sock, &server_sock, &client_sock);
	KUNIT_ASSERT_EQ(test, rv, 0);

	write_lock_bh(&server_sock->sk->sk_callback_lock);
	server_sock->sk->sk_user_data = cep;
	server_sock->sk->sk_data_ready = siw_qp_llp_data_ready;
	qp->attrs.sk = server_sock;
	write_unlock_bh(&server_sock->sk->sk_callback_lock);

	write.ctrl.mpa_len =
		cpu_to_be16(sizeof(write) - MPA_HDR_SIZE - 1);
	write.ctrl.ddp_rdmap_ctrl = DDP_FLAG_TAGGED | DDP_FLAG_LAST |
		cpu_to_be16(DDP_VERSION << 8) |
		cpu_to_be16(RDMAP_VERSION << 6) |
		cpu_to_be16(RDMAP_RDMA_WRITE);
	write.sink_stag = cpu_to_be32(stag);
	write.sink_to = cpu_to_be64((u64)(uintptr_t)target);

	memcpy(payload, &write, sizeof(write));
	payload[sizeof(write)] = 0x41;

	iov.iov_base = payload;
	iov.iov_len = sizeof(payload);
	rv = kernel_sendmsg(client_sock, &msg, &iov, 1, sizeof(payload));
	KUNIT_ASSERT_EQ(test, rv, (int)sizeof(payload));

	/* Wait for TCP to deliver bytes and sk_data_ready to fire. */
	for (i = 0; i < 200; i++) {
		if (qp->term_info.valid)
			break;
		msleep(20);
	}

	KUNIT_EXPECT_EQ(test, (int)qp->term_info.valid, 1);
	KUNIT_EXPECT_EQ(test, (int)qp->term_info.layer,
			(int)TERM_ERROR_LAYER_LLP);
	KUNIT_EXPECT_EQ(test, (int)qp->term_info.etype,
			(int)LLP_ETYPE_MPA);
	KUNIT_EXPECT_EQ(test, (int)qp->term_info.ecode,
			(int)LLP_ECODE_FPDU_START);
	KUNIT_EXPECT_EQ(test, (int)qp->rx_stream.rx_suspend, 1);

	/* Detach our handler before tearing down sockets so the TCP stack
	 * cannot call into freed kunit memory after the test.
	 */
	write_lock_bh(&server_sock->sk->sk_callback_lock);
	server_sock->sk->sk_user_data = NULL;
	server_sock->sk->sk_data_ready = sock_def_readable;
	write_unlock_bh(&server_sock->sk->sk_callback_lock);

	sock_release(client_sock);
	sock_release(server_sock);
	sock_release(listen_sock);
}

static struct kunit_case siw_mpa_rx_cases[] = {
	KUNIT_CASE(siw_mpa_write_underflow_rejected),
	KUNIT_CASE(siw_mpa_write_minimum_valid_accepted),
	KUNIT_CASE(siw_mpa_write_underflow_rejected_live_socket),
	{ }
};

static struct kunit_suite siw_mpa_rx_suite = {
	.name = "siw_mpa_rx",
	.test_cases = siw_mpa_rx_cases,
};

kunit_test_suite(siw_mpa_rx_suite);
