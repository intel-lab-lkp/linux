/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/nfsd.yaml */
/* YNL-GEN user header */

#ifndef _LINUX_NFSD_GEN_H
#define _LINUX_NFSD_GEN_H

#include <stdlib.h>
#include <string.h>
#include <linux/types.h>
#include <linux/nfsd_netlink.h>

struct ynl_sock;

extern const struct ynl_family ynl_nfsd_family;

/* Enums */
const char *nfsd_op_str(int op);

/* Common nested types */
/* ============== NFSD_CMD_RPC_STATUS_GET ============== */
/* NFSD_CMD_RPC_STATUS_GET - dump */
struct nfsd_rpc_status_get_list {
	struct nfsd_rpc_status_get_list *next;
	struct nfsd_rpc_status_get_rsp obj __attribute__ ((aligned (8)));
};

void nfsd_rpc_status_get_list_free(struct nfsd_rpc_status_get_list *rsp);

struct nfsd_rpc_status_get_list *nfsd_rpc_status_get_dump(struct ynl_sock *ys);

/* ============== NFSD_CMD_THREADS_SET ============== */
/* NFSD_CMD_THREADS_SET - do */
struct nfsd_threads_set_req {
	struct {
		__u32 threads:1;
	} _present;

	__u32 threads;
};

static inline struct nfsd_threads_set_req *nfsd_threads_set_req_alloc(void)
{
	return calloc(1, sizeof(struct nfsd_threads_set_req));
}
void nfsd_threads_set_req_free(struct nfsd_threads_set_req *req);

static inline void
nfsd_threads_set_req_set_threads(struct nfsd_threads_set_req *req,
				 __u32 threads)
{
	req->_present.threads = 1;
	req->threads = threads;
}

/*
 * set the number of running threads
 */
int nfsd_threads_set(struct ynl_sock *ys, struct nfsd_threads_set_req *req);

/* ============== NFSD_CMD_THREADS_GET ============== */
/* NFSD_CMD_THREADS_GET - do */

struct nfsd_threads_get_rsp {
	struct {
		__u32 threads:1;
	} _present;

	__u32 threads;
};

void nfsd_threads_get_rsp_free(struct nfsd_threads_get_rsp *rsp);

/*
 * get the number of running threads
 */
struct nfsd_threads_get_rsp *nfsd_threads_get(struct ynl_sock *ys);

/* ============== NFSD_CMD_VERSION_SET ============== */
/* NFSD_CMD_VERSION_SET - do */
struct nfsd_version_set_req {
	struct {
		__u32 major:1;
		__u32 minor:1;
		__u32 status:1;
	} _present;

	__u32 major;
	__u32 minor;
	__u8 status;
};

static inline struct nfsd_version_set_req *nfsd_version_set_req_alloc(void)
{
	return calloc(1, sizeof(struct nfsd_version_set_req));
}
void nfsd_version_set_req_free(struct nfsd_version_set_req *req);

static inline void
nfsd_version_set_req_set_major(struct nfsd_version_set_req *req, __u32 major)
{
	req->_present.major = 1;
	req->major = major;
}
static inline void
nfsd_version_set_req_set_minor(struct nfsd_version_set_req *req, __u32 minor)
{
	req->_present.minor = 1;
	req->minor = minor;
}
static inline void
nfsd_version_set_req_set_status(struct nfsd_version_set_req *req, __u8 status)
{
	req->_present.status = 1;
	req->status = status;
}

/*
 * enable/disable server version
 */
int nfsd_version_set(struct ynl_sock *ys, struct nfsd_version_set_req *req);

/* ============== NFSD_CMD_VERSION_GET ============== */
/* NFSD_CMD_VERSION_GET - dump */
struct nfsd_version_get_list {
	struct nfsd_version_get_list *next;
	struct nfsd_version_get_rsp obj __attribute__ ((aligned (8)));
};

void nfsd_version_get_list_free(struct nfsd_version_get_list *rsp);

struct nfsd_version_get_list *nfsd_version_get_dump(struct ynl_sock *ys);

/* ============== NFSD_CMD_LISTENER_START ============== */
/* NFSD_CMD_LISTENER_START - do */
struct nfsd_listener_start_req {
	struct {
		__u32 transport_name_len;
		__u32 port:1;
	} _present;

	char *transport_name;
	__u32 port;
};

static inline struct nfsd_listener_start_req *
nfsd_listener_start_req_alloc(void)
{
	return calloc(1, sizeof(struct nfsd_listener_start_req));
}
void nfsd_listener_start_req_free(struct nfsd_listener_start_req *req);

static inline void
nfsd_listener_start_req_set_transport_name(struct nfsd_listener_start_req *req,
					   const char *transport_name)
{
	free(req->transport_name);
	req->_present.transport_name_len = strlen(transport_name);
	req->transport_name = malloc(req->_present.transport_name_len + 1);
	memcpy(req->transport_name, transport_name, req->_present.transport_name_len);
	req->transport_name[req->_present.transport_name_len] = 0;
}
static inline void
nfsd_listener_start_req_set_port(struct nfsd_listener_start_req *req,
				 __u32 port)
{
	req->_present.port = 1;
	req->port = port;
}

/*
 * start server listener
 */
int nfsd_listener_start(struct ynl_sock *ys,
			struct nfsd_listener_start_req *req);

/* ============== NFSD_CMD_LISTENER_GET ============== */
/* NFSD_CMD_LISTENER_GET - dump */
struct nfsd_listener_get_list {
	struct nfsd_listener_get_list *next;
	struct nfsd_listener_get_rsp obj __attribute__ ((aligned (8)));
};

void nfsd_listener_get_list_free(struct nfsd_listener_get_list *rsp);

struct nfsd_listener_get_list *nfsd_listener_get_dump(struct ynl_sock *ys);

#endif /* _LINUX_NFSD_GEN_H */
