/* SPDX-License-Identifier: GPL-2.0 */
/*
 * linux/include/linux/sunrpc/svcsock.h
 *
 * RPC server socket I/O.
 *
 * Copyright (C) 1995, 1996 Olaf Kirch <okir@monad.swb.de>
 */

#ifndef SUNRPC_SVCSOCK_H
#define SUNRPC_SVCSOCK_H

#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/svc_xprt.h>
#include <linux/cache.h>
#include <linux/llist.h>
#include <linux/wait.h>
#include <linux/completion.h>
#include <linux/ktime.h>

/* Maximum queued messages per TCP socket before backpressure */
#define SVC_TCP_MSG_QUEUE_MAX	64

/**
 * struct svc_tcp_msg - queued RPC message ready for processing
 * @tm_node: lock-free queue linkage
 * @tm_len: total message length
 * @tm_npages: number of pages holding message data
 * @tm_pages: flexible array of pages containing the message
 *
 * The receiver thread allocates these to queue complete RPC messages
 * for worker threads to process. Page ownership transfers from the
 * receiver's rqstp to this structure, then to the worker's rqstp.
 */
struct svc_tcp_msg {
	struct llist_node	tm_node;
	size_t			tm_len;
	unsigned int		tm_npages;
	struct page		*tm_pages[];
};

/*
 * RPC server socket.
 */
struct svc_sock {
	struct svc_xprt		sk_xprt;
	struct socket *		sk_sock;	/* berkeley socket layer */
	struct sock *		sk_sk;		/* INET layer */

	/* We keep the old state_change and data_ready CB's here */
	void			(*sk_ostate)(struct sock *);
	void			(*sk_odata)(struct sock *);
	void			(*sk_owspace)(struct sock *);

	/* For sends (protected by xpt_mutex) */
	struct bio_vec		*sk_bvec;
	struct llist_head	sk_send_queue;	/* pending sends for combining */
	u64			sk_drain_avg_ns; /* EMA of combiner drain time */

	/* private TCP part */
	/* On-the-wire fragment header: */
	__be32			sk_marker;
	/* As we receive a record, this includes the length received so
	 * far (including the fragment header): */
	u32			sk_tcplen;
	/* Total length of the data (not including fragment headers)
	 * received so far in the fragments making up this rpc: */
	u32			sk_datalen;

	struct page_frag_cache  sk_frag_cache;

	struct completion	sk_handshake_done;

	/* Dedicated receiver thread (TCP only) */
	struct task_struct	*sk_receiver;
	struct llist_head	sk_msg_queue;
	wait_queue_head_t	sk_receiver_wq;
	struct completion	sk_receiver_exit;
	struct svc_page_pool	*sk_page_pool;
	ktime_t			sk_partial_record_time;

	atomic_t		sk_msg_count ____cacheline_aligned_in_smp;

	/* received data */
	unsigned long		sk_maxpages;
	struct page *		sk_pages[] __counted_by(sk_maxpages);
};

static inline u32 svc_sock_reclen(struct svc_sock *svsk)
{
	return be32_to_cpu(svsk->sk_marker) & RPC_FRAGMENT_SIZE_MASK;
}

static inline u32 svc_sock_final_rec(struct svc_sock *svsk)
{
	return be32_to_cpu(svsk->sk_marker) & RPC_LAST_STREAM_FRAGMENT;
}

/*
 * Function prototypes.
 */
void		svc_recv(struct svc_rqst *rqstp);
void		svc_send(struct svc_rqst *rqstp);
int		svc_tcp_send(struct svc_xprt *xprt, struct xdr_buf *xdr,
			     rpc_fraghdr marker);
int		svc_addsock(struct svc_serv *serv, struct net *net,
			    const int fd, char *name_return, const size_t len,
			    const struct cred *cred);
void		svc_init_xprt_sock(void);
void		svc_cleanup_xprt_sock(void);

/*
 * svc_makesock socket characteristics
 */
#define SVC_SOCK_DEFAULTS	(0U)
#define SVC_SOCK_ANONYMOUS	(1U << 0)	/* don't register with pmap */
#define SVC_SOCK_TEMPORARY	(1U << 1)	/* flag socket as temporary */

#endif /* SUNRPC_SVCSOCK_H */
