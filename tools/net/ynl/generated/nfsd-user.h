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
struct nfsd_nfs_version {
	struct {
		__u32 major:1;
		__u32 minor:1;
	} _present;

	__u32 major;
	__u32 minor;
};

struct nfsd_server_instance {
	struct {
		__u32 transport_name_len;
		__u32 port:1;
		__u32 inet_proto:1;
	} _present;

	char *transport_name;
	__u32 port;
	__u16 inet_proto;
};

/* ============== NFSD_CMD_RPC_STATUS_GET ============== */
/* NFSD_CMD_RPC_STATUS_GET - dump */
struct nfsd_rpc_status_get_rsp_dump {
	struct {
		__u32 xid:1;
		__u32 flags:1;
		__u32 prog:1;
		__u32 version:1;
		__u32 proc:1;
		__u32 service_time:1;
		__u32 saddr4:1;
		__u32 daddr4:1;
		__u32 saddr6_len;
		__u32 daddr6_len;
		__u32 sport:1;
		__u32 dport:1;
	} _present;

	__u32 xid /* big-endian */;
	__u32 flags;
	__u32 prog;
	__u8 version;
	__u32 proc;
	__s64 service_time;
	__u32 saddr4 /* big-endian */;
	__u32 daddr4 /* big-endian */;
	void *saddr6;
	void *daddr6;
	__u16 sport /* big-endian */;
	__u16 dport /* big-endian */;
	unsigned int n_compound_ops;
	__u32 *compound_ops;
};

struct nfsd_rpc_status_get_rsp_list {
	struct nfsd_rpc_status_get_rsp_list *next;
	struct nfsd_rpc_status_get_rsp_dump obj __attribute__((aligned(8)));
};

void
nfsd_rpc_status_get_rsp_list_free(struct nfsd_rpc_status_get_rsp_list *rsp);

struct nfsd_rpc_status_get_rsp_list *
nfsd_rpc_status_get_dump(struct ynl_sock *ys);

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
	unsigned int n_version;
	struct nfsd_nfs_version *version;
};

static inline struct nfsd_version_set_req *nfsd_version_set_req_alloc(void)
{
	return calloc(1, sizeof(struct nfsd_version_set_req));
}
void nfsd_version_set_req_free(struct nfsd_version_set_req *req);

static inline void
__nfsd_version_set_req_set_version(struct nfsd_version_set_req *req,
				   struct nfsd_nfs_version *version,
				   unsigned int n_version)
{
	free(req->version);
	req->version = version;
	req->n_version = n_version;
}

/*
 * set nfs enabled versions
 */
int nfsd_version_set(struct ynl_sock *ys, struct nfsd_version_set_req *req);

/* ============== NFSD_CMD_VERSION_GET ============== */
/* NFSD_CMD_VERSION_GET - do */

struct nfsd_version_get_rsp {
	unsigned int n_version;
	struct nfsd_nfs_version *version;
};

void nfsd_version_get_rsp_free(struct nfsd_version_get_rsp *rsp);

/*
 * get nfs enabled versions
 */
struct nfsd_version_get_rsp *nfsd_version_get(struct ynl_sock *ys);

/* ============== NFSD_CMD_LISTENER_SET ============== */
/* NFSD_CMD_LISTENER_SET - do */
struct nfsd_listener_set_req {
	unsigned int n_instance;
	struct nfsd_server_instance *instance;
};

static inline struct nfsd_listener_set_req *nfsd_listener_set_req_alloc(void)
{
	return calloc(1, sizeof(struct nfsd_listener_set_req));
}
void nfsd_listener_set_req_free(struct nfsd_listener_set_req *req);

static inline void
__nfsd_listener_set_req_set_instance(struct nfsd_listener_set_req *req,
				     struct nfsd_server_instance *instance,
				     unsigned int n_instance)
{
	free(req->instance);
	req->instance = instance;
	req->n_instance = n_instance;
}

/*
 * set nfs running listeners
 */
int nfsd_listener_set(struct ynl_sock *ys, struct nfsd_listener_set_req *req);

/* ============== NFSD_CMD_LISTENER_GET ============== */
/* NFSD_CMD_LISTENER_GET - do */

struct nfsd_listener_get_rsp {
	unsigned int n_instance;
	struct nfsd_server_instance *instance;
};

void nfsd_listener_get_rsp_free(struct nfsd_listener_get_rsp *rsp);

/*
 * get nfs running listeners
 */
struct nfsd_listener_get_rsp *nfsd_listener_get(struct ynl_sock *ys);

#endif /* _LINUX_NFSD_GEN_H */
