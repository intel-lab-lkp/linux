/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  Shared Memory Communications over RDMA (SMC-R) and RoCE
 *
 *  Definitions for the SMC module (socket related)
 *
 *  Copyright IBM Corp. 2016
 *
 */

#ifndef __SMC_INET
#define __SMC_INET

#include <net/protocol.h>
#include <net/sock.h>
#include <net/tcp.h>
#include <net/ipv6.h>
/* MUST after net/tcp.h or warning */
#include <net/transp_v6.h>

extern struct proto smc_inet_prot;
extern struct proto smc_inet6_prot;

extern const struct proto_ops smc_inet_stream_ops;
extern const struct proto_ops smc_inet6_stream_ops;

extern struct inet_protosw smc_inet_protosw;
extern struct inet_protosw smc_inet6_protosw;

/* obtain TCP proto via sock family */
static __always_inline struct proto *smc_inet_get_tcp_prot(int family)
{
	switch (family) {
	case AF_INET:
		return &tcp_prot;
	case AF_INET6:
		return &tcpv6_prot;
	default:
		pr_warn_once("smc: %s(unknown family %d)\n", __func__, family);
		break;
	}
	return NULL;
}

/* This function initializes the inet related structures.
 * If initialization is successful, it returns 0;
 * otherwise, it returns a non-zero value.
 */
int smc_inet_sock_init(void);

int smc_inet_init_sock(struct sock *sk);
void smc_inet_sock_proto_release_cb(struct sock *sk);

int smc_inet_connect(struct socket *sock, struct sockaddr *addr,
		     int alen, int flags);

int smc_inet_setsockopt(struct socket *sock, int level, int optname,
			sockptr_t optval, unsigned int optlen);

int smc_inet_getsockopt(struct socket *sock, int level, int optname,
			char __user *optval, int __user *optlen);

int smc_inet_ioctl(struct socket *sock, unsigned int cmd,
		   unsigned long arg);

int smc_inet_sendmsg(struct socket *sock, struct msghdr *msg, size_t len);

int smc_inet_recvmsg(struct socket *sock, struct msghdr *msg, size_t len,
		     int flags);

ssize_t smc_inet_sendpage(struct socket *sock, struct page *page,
			  int offset, size_t size, int flags);

ssize_t smc_inet_splice_read(struct socket *sock, loff_t *ppos,
			     struct pipe_inode_info *pipe, size_t len,
			     unsigned int flags);

__poll_t smc_inet_poll(struct file *file, struct socket *sock, poll_table *wait);

struct sock *smc_inet_csk_accept(struct sock *sk, int flags, int *err, bool kern);
int smc_inet_listen(struct socket *sock, int backlog);

int smc_inet_shutdown(struct socket *sock, int how);
int smc_inet_release(struct socket *sock);

#endif // __SMC_INET
