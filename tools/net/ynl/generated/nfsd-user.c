// SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause)
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/nfsd.yaml */
/* YNL-GEN user source */

#include <stdlib.h>
#include <string.h>
#include "nfsd-user.h"
#include "ynl.h"
#include <linux/nfsd_netlink.h>

#include <libmnl/libmnl.h>
#include <linux/genetlink.h>

/* Enums */
static const char * const nfsd_op_strmap[] = {
	[NFSD_CMD_RPC_STATUS_GET] = "rpc-status-get",
	[NFSD_CMD_THREADS_SET] = "threads-set",
	[NFSD_CMD_THREADS_GET] = "threads-get",
	[NFSD_CMD_VERSION_SET] = "version-set",
	[NFSD_CMD_VERSION_GET] = "version-get",
	[NFSD_CMD_LISTENER_START] = "listener-start",
	[NFSD_CMD_LISTENER_GET] = "listener-get",
};

const char *nfsd_op_str(int op)
{
	if (op < 0 || op >= (int)MNL_ARRAY_SIZE(nfsd_op_strmap))
		return NULL;
	return nfsd_op_strmap[op];
}

/* Policies */
struct ynl_policy_attr nfsd_rpc_status_policy[NFSD_A_RPC_STATUS_MAX + 1] = {
	[NFSD_A_RPC_STATUS_XID] = { .name = "xid", .type = YNL_PT_U32, },
	[NFSD_A_RPC_STATUS_FLAGS] = { .name = "flags", .type = YNL_PT_U32, },
	[NFSD_A_RPC_STATUS_PROG] = { .name = "prog", .type = YNL_PT_U32, },
	[NFSD_A_RPC_STATUS_VERSION] = { .name = "version", .type = YNL_PT_U8, },
	[NFSD_A_RPC_STATUS_PROC] = { .name = "proc", .type = YNL_PT_U32, },
	[NFSD_A_RPC_STATUS_SERVICE_TIME] = { .name = "service_time", .type = YNL_PT_U64, },
	[NFSD_A_RPC_STATUS_PAD] = { .name = "pad", .type = YNL_PT_IGNORE, },
	[NFSD_A_RPC_STATUS_SADDR4] = { .name = "saddr4", .type = YNL_PT_U32, },
	[NFSD_A_RPC_STATUS_DADDR4] = { .name = "daddr4", .type = YNL_PT_U32, },
	[NFSD_A_RPC_STATUS_SADDR6] = { .name = "saddr6", .type = YNL_PT_BINARY,},
	[NFSD_A_RPC_STATUS_DADDR6] = { .name = "daddr6", .type = YNL_PT_BINARY,},
	[NFSD_A_RPC_STATUS_SPORT] = { .name = "sport", .type = YNL_PT_U16, },
	[NFSD_A_RPC_STATUS_DPORT] = { .name = "dport", .type = YNL_PT_U16, },
	[NFSD_A_RPC_STATUS_COMPOUND_OPS] = { .name = "compound-ops", .type = YNL_PT_U32, },
};

struct ynl_policy_nest nfsd_rpc_status_nest = {
	.max_attr = NFSD_A_RPC_STATUS_MAX,
	.table = nfsd_rpc_status_policy,
};

struct ynl_policy_attr nfsd_server_worker_policy[NFSD_A_SERVER_WORKER_MAX + 1] = {
	[NFSD_A_SERVER_WORKER_THREADS] = { .name = "threads", .type = YNL_PT_U32, },
};

struct ynl_policy_nest nfsd_server_worker_nest = {
	.max_attr = NFSD_A_SERVER_WORKER_MAX,
	.table = nfsd_server_worker_policy,
};

struct ynl_policy_attr nfsd_server_version_policy[NFSD_A_SERVER_VERSION_MAX + 1] = {
	[NFSD_A_SERVER_VERSION_MAJOR] = { .name = "major", .type = YNL_PT_U32, },
	[NFSD_A_SERVER_VERSION_MINOR] = { .name = "minor", .type = YNL_PT_U32, },
	[NFSD_A_SERVER_VERSION_STATUS] = { .name = "status", .type = YNL_PT_U8, },
};

struct ynl_policy_nest nfsd_server_version_nest = {
	.max_attr = NFSD_A_SERVER_VERSION_MAX,
	.table = nfsd_server_version_policy,
};

struct ynl_policy_attr nfsd_server_listener_policy[NFSD_A_SERVER_LISTENER_MAX + 1] = {
	[NFSD_A_SERVER_LISTENER_TRANSPORT_NAME] = { .name = "transport-name", .type = YNL_PT_NUL_STR, },
	[NFSD_A_SERVER_LISTENER_PORT] = { .name = "port", .type = YNL_PT_U32, },
};

struct ynl_policy_nest nfsd_server_listener_nest = {
	.max_attr = NFSD_A_SERVER_LISTENER_MAX,
	.table = nfsd_server_listener_policy,
};

/* Common nested types */
/* ============== NFSD_CMD_RPC_STATUS_GET ============== */
/* NFSD_CMD_RPC_STATUS_GET - dump */
void nfsd_rpc_status_get_list_free(struct nfsd_rpc_status_get_list *rsp)
{
	struct nfsd_rpc_status_get_list *next = rsp;

	while ((void *)next != YNL_LIST_END) {
		rsp = next;
		next = rsp->next;

		free(rsp->obj.saddr6);
		free(rsp->obj.daddr6);
		free(rsp->obj.compound_ops);
		free(rsp);
	}
}

struct nfsd_rpc_status_get_list *nfsd_rpc_status_get_dump(struct ynl_sock *ys)
{
	struct ynl_dump_state yds = {};
	struct nlmsghdr *nlh;
	int err;

	yds.ys = ys;
	yds.alloc_sz = sizeof(struct nfsd_rpc_status_get_list);
	yds.cb = nfsd_rpc_status_get_rsp_parse;
	yds.rsp_cmd = NFSD_CMD_RPC_STATUS_GET;
	yds.rsp_policy = &nfsd_rpc_status_nest;

	nlh = ynl_gemsg_start_dump(ys, ys->family_id, NFSD_CMD_RPC_STATUS_GET, 1);

	err = ynl_exec_dump(ys, nlh, &yds);
	if (err < 0)
		goto free_list;

	return yds.first;

free_list:
	nfsd_rpc_status_get_list_free(yds.first);
	return NULL;
}

/* ============== NFSD_CMD_THREADS_SET ============== */
/* NFSD_CMD_THREADS_SET - do */
void nfsd_threads_set_req_free(struct nfsd_threads_set_req *req)
{
	free(req);
}

int nfsd_threads_set(struct ynl_sock *ys, struct nfsd_threads_set_req *req)
{
	struct nlmsghdr *nlh;
	int err;

	nlh = ynl_gemsg_start_req(ys, ys->family_id, NFSD_CMD_THREADS_SET, 1);
	ys->req_policy = &nfsd_server_worker_nest;

	if (req->_present.threads)
		mnl_attr_put_u32(nlh, NFSD_A_SERVER_WORKER_THREADS, req->threads);

	err = ynl_exec(ys, nlh, NULL);
	if (err < 0)
		return -1;

	return 0;
}

/* ============== NFSD_CMD_THREADS_GET ============== */
/* NFSD_CMD_THREADS_GET - do */
void nfsd_threads_get_rsp_free(struct nfsd_threads_get_rsp *rsp)
{
	free(rsp);
}

int nfsd_threads_get_rsp_parse(const struct nlmsghdr *nlh, void *data)
{
	struct ynl_parse_arg *yarg = data;
	struct nfsd_threads_get_rsp *dst;
	const struct nlattr *attr;

	dst = yarg->data;

	mnl_attr_for_each(attr, nlh, sizeof(struct genlmsghdr)) {
		unsigned int type = mnl_attr_get_type(attr);

		if (type == NFSD_A_SERVER_WORKER_THREADS) {
			if (ynl_attr_validate(yarg, attr))
				return MNL_CB_ERROR;
			dst->_present.threads = 1;
			dst->threads = mnl_attr_get_u32(attr);
		}
	}

	return MNL_CB_OK;
}

struct nfsd_threads_get_rsp *nfsd_threads_get(struct ynl_sock *ys)
{
	struct ynl_req_state yrs = { .yarg = { .ys = ys, }, };
	struct nfsd_threads_get_rsp *rsp;
	struct nlmsghdr *nlh;
	int err;

	nlh = ynl_gemsg_start_req(ys, ys->family_id, NFSD_CMD_THREADS_GET, 1);
	ys->req_policy = &nfsd_server_worker_nest;
	yrs.yarg.rsp_policy = &nfsd_server_worker_nest;

	rsp = calloc(1, sizeof(*rsp));
	yrs.yarg.data = rsp;
	yrs.cb = nfsd_threads_get_rsp_parse;
	yrs.rsp_cmd = NFSD_CMD_THREADS_GET;

	err = ynl_exec(ys, nlh, &yrs);
	if (err < 0)
		goto err_free;

	return rsp;

err_free:
	nfsd_threads_get_rsp_free(rsp);
	return NULL;
}

/* ============== NFSD_CMD_VERSION_SET ============== */
/* NFSD_CMD_VERSION_SET - do */
void nfsd_version_set_req_free(struct nfsd_version_set_req *req)
{
	free(req);
}

int nfsd_version_set(struct ynl_sock *ys, struct nfsd_version_set_req *req)
{
	struct nlmsghdr *nlh;
	int err;

	nlh = ynl_gemsg_start_req(ys, ys->family_id, NFSD_CMD_VERSION_SET, 1);
	ys->req_policy = &nfsd_server_version_nest;

	if (req->_present.major)
		mnl_attr_put_u32(nlh, NFSD_A_SERVER_VERSION_MAJOR, req->major);
	if (req->_present.minor)
		mnl_attr_put_u32(nlh, NFSD_A_SERVER_VERSION_MINOR, req->minor);
	if (req->_present.status)
		mnl_attr_put_u8(nlh, NFSD_A_SERVER_VERSION_STATUS, req->status);

	err = ynl_exec(ys, nlh, NULL);
	if (err < 0)
		return -1;

	return 0;
}

/* ============== NFSD_CMD_VERSION_GET ============== */
/* NFSD_CMD_VERSION_GET - dump */
void nfsd_version_get_list_free(struct nfsd_version_get_list *rsp)
{
	struct nfsd_version_get_list *next = rsp;

	while ((void *)next != YNL_LIST_END) {
		rsp = next;
		next = rsp->next;

		free(rsp);
	}
}

struct nfsd_version_get_list *nfsd_version_get_dump(struct ynl_sock *ys)
{
	struct ynl_dump_state yds = {};
	struct nlmsghdr *nlh;
	int err;

	yds.ys = ys;
	yds.alloc_sz = sizeof(struct nfsd_version_get_list);
	yds.cb = nfsd_version_get_rsp_parse;
	yds.rsp_cmd = NFSD_CMD_VERSION_GET;
	yds.rsp_policy = &nfsd_server_version_nest;

	nlh = ynl_gemsg_start_dump(ys, ys->family_id, NFSD_CMD_VERSION_GET, 1);

	err = ynl_exec_dump(ys, nlh, &yds);
	if (err < 0)
		goto free_list;

	return yds.first;

free_list:
	nfsd_version_get_list_free(yds.first);
	return NULL;
}

/* ============== NFSD_CMD_LISTENER_START ============== */
/* NFSD_CMD_LISTENER_START - do */
void nfsd_listener_start_req_free(struct nfsd_listener_start_req *req)
{
	free(req->transport_name);
	free(req);
}

int nfsd_listener_start(struct ynl_sock *ys,
			struct nfsd_listener_start_req *req)
{
	struct nlmsghdr *nlh;
	int err;

	nlh = ynl_gemsg_start_req(ys, ys->family_id, NFSD_CMD_LISTENER_START, 1);
	ys->req_policy = &nfsd_server_listener_nest;

	if (req->_present.transport_name_len)
		mnl_attr_put_strz(nlh, NFSD_A_SERVER_LISTENER_TRANSPORT_NAME, req->transport_name);
	if (req->_present.port)
		mnl_attr_put_u32(nlh, NFSD_A_SERVER_LISTENER_PORT, req->port);

	err = ynl_exec(ys, nlh, NULL);
	if (err < 0)
		return -1;

	return 0;
}

/* ============== NFSD_CMD_LISTENER_GET ============== */
/* NFSD_CMD_LISTENER_GET - dump */
void nfsd_listener_get_list_free(struct nfsd_listener_get_list *rsp)
{
	struct nfsd_listener_get_list *next = rsp;

	while ((void *)next != YNL_LIST_END) {
		rsp = next;
		next = rsp->next;

		free(rsp->obj.transport_name);
		free(rsp);
	}
}

struct nfsd_listener_get_list *nfsd_listener_get_dump(struct ynl_sock *ys)
{
	struct ynl_dump_state yds = {};
	struct nlmsghdr *nlh;
	int err;

	yds.ys = ys;
	yds.alloc_sz = sizeof(struct nfsd_listener_get_list);
	yds.cb = nfsd_listener_get_rsp_parse;
	yds.rsp_cmd = NFSD_CMD_LISTENER_GET;
	yds.rsp_policy = &nfsd_server_listener_nest;

	nlh = ynl_gemsg_start_dump(ys, ys->family_id, NFSD_CMD_LISTENER_GET, 1);

	err = ynl_exec_dump(ys, nlh, &yds);
	if (err < 0)
		goto free_list;

	return yds.first;

free_list:
	nfsd_listener_get_list_free(yds.first);
	return NULL;
}

const struct ynl_family ynl_nfsd_family =  {
	.name		= "nfsd",
};
