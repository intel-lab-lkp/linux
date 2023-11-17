/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/netdev.yaml */
/* YNL-GEN user header */

#ifndef _LINUX_NETDEV_GEN_H
#define _LINUX_NETDEV_GEN_H

#include <stdlib.h>
#include <string.h>
#include <linux/types.h>
#include <linux/netdev.h>

struct ynl_sock;

extern const struct ynl_family ynl_netdev_family;

/* Enums */
const char *netdev_op_str(int op);
const char *netdev_xdp_act_str(enum netdev_xdp_act value);
const char *netdev_xdp_rx_metadata_str(enum netdev_xdp_rx_metadata value);
const char *netdev_queue_type_str(enum netdev_queue_type value);

/* Common nested types */
/* ============== NETDEV_CMD_DEV_GET ============== */
/* NETDEV_CMD_DEV_GET - do */
struct netdev_dev_get_req {
	struct {
		__u32 ifindex:1;
	} _present;

	__u32 ifindex;
};

static inline struct netdev_dev_get_req *netdev_dev_get_req_alloc(void)
{
	return calloc(1, sizeof(struct netdev_dev_get_req));
}
void netdev_dev_get_req_free(struct netdev_dev_get_req *req);

static inline void
netdev_dev_get_req_set_ifindex(struct netdev_dev_get_req *req, __u32 ifindex)
{
	req->_present.ifindex = 1;
	req->ifindex = ifindex;
}

struct netdev_dev_get_rsp {
	struct {
		__u32 ifindex:1;
		__u32 xdp_features:1;
		__u32 xdp_zc_max_segs:1;
		__u32 xdp_rx_metadata_features:1;
	} _present;

	__u32 ifindex;
	__u64 xdp_features;
	__u32 xdp_zc_max_segs;
	__u64 xdp_rx_metadata_features;
};

void netdev_dev_get_rsp_free(struct netdev_dev_get_rsp *rsp);

/*
 * Get / dump information about a netdev.
 */
struct netdev_dev_get_rsp *
netdev_dev_get(struct ynl_sock *ys, struct netdev_dev_get_req *req);

/* NETDEV_CMD_DEV_GET - dump */
struct netdev_dev_get_list {
	struct netdev_dev_get_list *next;
	struct netdev_dev_get_rsp obj __attribute__((aligned(8)));
};

void netdev_dev_get_list_free(struct netdev_dev_get_list *rsp);

struct netdev_dev_get_list *netdev_dev_get_dump(struct ynl_sock *ys);

/* NETDEV_CMD_DEV_GET - notify */
struct netdev_dev_get_ntf {
	__u16 family;
	__u8 cmd;
	struct ynl_ntf_base_type *next;
	void (*free)(struct netdev_dev_get_ntf *ntf);
	struct netdev_dev_get_rsp obj __attribute__((aligned(8)));
};

void netdev_dev_get_ntf_free(struct netdev_dev_get_ntf *rsp);

/* ============== NETDEV_CMD_QUEUE_GET ============== */
/* NETDEV_CMD_QUEUE_GET - do */
struct netdev_queue_get_req {
	struct {
		__u32 ifindex:1;
		__u32 type:1;
		__u32 id:1;
	} _present;

	__u32 ifindex;
	enum netdev_queue_type type;
	__u32 id;
};

static inline struct netdev_queue_get_req *netdev_queue_get_req_alloc(void)
{
	return calloc(1, sizeof(struct netdev_queue_get_req));
}
void netdev_queue_get_req_free(struct netdev_queue_get_req *req);

static inline void
netdev_queue_get_req_set_ifindex(struct netdev_queue_get_req *req,
				 __u32 ifindex)
{
	req->_present.ifindex = 1;
	req->ifindex = ifindex;
}
static inline void
netdev_queue_get_req_set_type(struct netdev_queue_get_req *req,
			      enum netdev_queue_type type)
{
	req->_present.type = 1;
	req->type = type;
}
static inline void
netdev_queue_get_req_set_id(struct netdev_queue_get_req *req, __u32 id)
{
	req->_present.id = 1;
	req->id = id;
}

struct netdev_queue_get_rsp {
	struct {
		__u32 id:1;
		__u32 type:1;
		__u32 napi_id:1;
		__u32 ifindex:1;
	} _present;

	__u32 id;
	enum netdev_queue_type type;
	__u32 napi_id;
	__u32 ifindex;
};

void netdev_queue_get_rsp_free(struct netdev_queue_get_rsp *rsp);

/*
 * Get queue information from the kernel. Only configured queues will be reported (as opposed to all available hardware queues).
 */
struct netdev_queue_get_rsp *
netdev_queue_get(struct ynl_sock *ys, struct netdev_queue_get_req *req);

/* NETDEV_CMD_QUEUE_GET - dump */
struct netdev_queue_get_req_dump {
	struct {
		__u32 ifindex:1;
	} _present;

	__u32 ifindex;
};

static inline struct netdev_queue_get_req_dump *
netdev_queue_get_req_dump_alloc(void)
{
	return calloc(1, sizeof(struct netdev_queue_get_req_dump));
}
void netdev_queue_get_req_dump_free(struct netdev_queue_get_req_dump *req);

static inline void
netdev_queue_get_req_dump_set_ifindex(struct netdev_queue_get_req_dump *req,
				      __u32 ifindex)
{
	req->_present.ifindex = 1;
	req->ifindex = ifindex;
}

struct netdev_queue_get_list {
	struct netdev_queue_get_list *next;
	struct netdev_queue_get_rsp obj __attribute__((aligned(8)));
};

void netdev_queue_get_list_free(struct netdev_queue_get_list *rsp);

struct netdev_queue_get_list *
netdev_queue_get_dump(struct ynl_sock *ys,
		      struct netdev_queue_get_req_dump *req);

/* ============== NETDEV_CMD_NAPI_GET ============== */
/* NETDEV_CMD_NAPI_GET - do */
struct netdev_napi_get_req {
	struct {
		__u32 id:1;
	} _present;

	__u32 id;
};

static inline struct netdev_napi_get_req *netdev_napi_get_req_alloc(void)
{
	return calloc(1, sizeof(struct netdev_napi_get_req));
}
void netdev_napi_get_req_free(struct netdev_napi_get_req *req);

static inline void
netdev_napi_get_req_set_id(struct netdev_napi_get_req *req, __u32 id)
{
	req->_present.id = 1;
	req->id = id;
}

struct netdev_napi_get_rsp {
	struct {
		__u32 id:1;
		__u32 ifindex:1;
		__u32 irq:1;
	} _present;

	__u32 id;
	__u32 ifindex;
	__u32 irq;
};

void netdev_napi_get_rsp_free(struct netdev_napi_get_rsp *rsp);

/*
 * Get information about NAPI instances configured on the system.
 */
struct netdev_napi_get_rsp *
netdev_napi_get(struct ynl_sock *ys, struct netdev_napi_get_req *req);

/* NETDEV_CMD_NAPI_GET - dump */
struct netdev_napi_get_req_dump {
	struct {
		__u32 ifindex:1;
	} _present;

	__u32 ifindex;
};

static inline struct netdev_napi_get_req_dump *
netdev_napi_get_req_dump_alloc(void)
{
	return calloc(1, sizeof(struct netdev_napi_get_req_dump));
}
void netdev_napi_get_req_dump_free(struct netdev_napi_get_req_dump *req);

static inline void
netdev_napi_get_req_dump_set_ifindex(struct netdev_napi_get_req_dump *req,
				     __u32 ifindex)
{
	req->_present.ifindex = 1;
	req->ifindex = ifindex;
}

struct netdev_napi_get_list {
	struct netdev_napi_get_list *next;
	struct netdev_napi_get_rsp obj __attribute__((aligned(8)));
};

void netdev_napi_get_list_free(struct netdev_napi_get_list *rsp);

struct netdev_napi_get_list *
netdev_napi_get_dump(struct ynl_sock *ys, struct netdev_napi_get_req_dump *req);

#endif /* _LINUX_NETDEV_GEN_H */
