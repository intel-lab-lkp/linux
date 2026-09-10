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

	/* TCP reply-ACK tracking (single-threaded send path) */
#define SVC_ACK_RING_BITS	6
#define SVC_ACK_RING_SIZE	(1 << SVC_ACK_RING_BITS)
	struct {
		u32		ae_pos;		/* write_seq after send */
		svc_ack_cookie_t ae_cookie;	/* opaque DRC cookie */
	}			sk_ack_ring[SVC_ACK_RING_SIZE];
	unsigned int		sk_ack_head;
	unsigned int		sk_ack_tail;

	struct completion	sk_handshake_done;

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
int		svc_recv(struct svc_rqst *rqstp, long timeo);
void		svc_send(struct svc_rqst *rqstp);
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
