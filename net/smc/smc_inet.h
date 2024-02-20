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

#include <net/smc.h>
#include "smc.h"

extern struct proto smc_inet_prot;
extern struct proto smc_inet6_prot;

extern const struct proto_ops smc_inet_stream_ops;
extern const struct proto_ops smc_inet6_stream_ops;

extern struct inet_protosw smc_inet_protosw;
extern struct inet_protosw smc_inet6_protosw;

extern const struct proto_ops smc_inet_clcsock_ops;

void smc_inet_sock_state_change(struct sock *sk);

enum smc_inet_sock_negotiation_state {
	/* When creating an AF_SMC sock, the state field will be initialized to 0 by default,
	 * which is only for logical compatibility with that situation
	 * and will never be used.
	 */
	SMC_NEGOTIATION_COMPATIBLE_WITH_AF_SMC = 0,

	/* This connection is still uncertain whether it is an SMC connection or not,
	 * It always appears when actively open SMC connection, because it's unclear
	 * whether the server supports the SMC protocol and has willing to use SMC.
	 */
	SMC_NEGOTIATION_TBD = 0x10,

	/* This state indicates that this connection is definitely not an SMC connection.
	 * and it is absolutely impossible to become an SMC connection again. A fina
	 * state.
	 */
	SMC_NEGOTIATION_NO_SMC = 0x20,

	/* This state indicates that this connection is an SMC connection. and it is
	 * absolutely impossible to become an not-SMC connection again. A final state.
	 */
	SMC_NEGOTIATION_SMC = 0x40,

	/* This state indicates that this connection is in the process of SMC handshake.
	 * It is mainly used to eliminate the ambiguity of syn_smc, because when syn_smc is 1,
	 * It may represent remote has support for SMC, or it may just indicate that itself has
	 * supports for SMC.
	 */
	SMC_NEGOTIATION_PREPARE_SMC = 0x80,

	/* flags */
	SMC_NEGOTIATION_LISTEN_FLAG = 0x01,
	SMC_NEGOTIATION_ABORT_FLAG = 0x02,
	SMC_NEGOTIATION_NOT_SUPPORT_FLAG = 0x04,
};

static __always_inline void isck_smc_negotiation_store(struct smc_sock *smc,
						       enum smc_inet_sock_negotiation_state state)
{
	WRITE_ONCE(smc->isck_smc_negotiation,
		   state | (READ_ONCE(smc->isck_smc_negotiation) & 0x0f));
}

static __always_inline int isck_smc_negotiation_load(struct smc_sock *smc)
{
	return READ_ONCE(smc->isck_smc_negotiation) & 0xf0;
}

static __always_inline void isck_smc_negotiation_set_flags(struct smc_sock *smc, int flags)
{
	smc->isck_smc_negotiation = (smc->isck_smc_negotiation | (flags & 0x0f));
}

static __always_inline int isck_smc_negotiation_get_flags(struct smc_sock *smc)
{
	return smc->isck_smc_negotiation & 0x0f;
}

static __always_inline bool smc_inet_sock_check_fallback_fast(struct sock *sk)
{
	return !tcp_sk(sk)->syn_smc;
}

static __always_inline bool smc_inet_sock_check_fallback(struct sock *sk)
{
	return isck_smc_negotiation_load(smc_sk(sk)) == SMC_NEGOTIATION_NO_SMC;
}

static __always_inline bool smc_inet_sock_check_smc(struct sock *sk)
{
	if (smc_inet_sock_check_fallback_fast(sk))
		return false;

	return isck_smc_negotiation_load(smc_sk(sk)) == SMC_NEGOTIATION_SMC;
}

static __always_inline bool smc_inet_sock_is_active_open(struct sock *sk)
{
	return !(isck_smc_negotiation_get_flags(smc_sk(sk)) & SMC_NEGOTIATION_LISTEN_FLAG);
}

static inline void smc_inet_sock_abort(struct sock *sk)
{
	write_lock_bh(&sk->sk_callback_lock);
	if (isck_smc_negotiation_get_flags(smc_sk(sk)) & SMC_NEGOTIATION_ABORT_FLAG) {
		write_unlock_bh(&sk->sk_callback_lock);
		return;
	}
	isck_smc_negotiation_set_flags(smc_sk(sk), SMC_NEGOTIATION_ABORT_FLAG);
	write_unlock_bh(&sk->sk_callback_lock);
	sk->sk_error_report(sk);
}

int smc_inet_sock_move_state(struct sock *sk, int except, int target);
int smc_inet_sock_move_state_locked(struct sock *sk, int except, int target);

static inline int smc_inet_sock_set_syn_smc_locked(struct sock *sk, int value)
{
	int flags;

	/* not set syn smc */
	if (value == 0) {
		if (smc_sk_state(sk) != SMC_LISTEN) {
			smc_inet_sock_move_state_locked(sk, SMC_NEGOTIATION_TBD,
							SMC_NEGOTIATION_NO_SMC);
			smc_sk_set_state(sk, SMC_ACTIVE);
		}
		return 0;
	}
	/* set syn smc */
	flags = isck_smc_negotiation_get_flags(smc_sk(sk));
	if (isck_smc_negotiation_load(smc_sk(sk)) != SMC_NEGOTIATION_TBD)
		return 0;
	if (flags & SMC_NEGOTIATION_ABORT_FLAG)
		return 0;
	if (flags & SMC_NEGOTIATION_NOT_SUPPORT_FLAG)
		return 0;
	tcp_sk(sk)->syn_smc = 1;
	return 1;
}

static inline int smc_inet_sock_try_disable_smc(struct sock *sk, int flag)
{
	struct smc_sock *smc = smc_sk(sk);
	int success = 0;

	write_lock_bh(&sk->sk_callback_lock);
	switch (isck_smc_negotiation_load(smc)) {
	case SMC_NEGOTIATION_TBD:
		/* can not disable now */
		if (flag != SMC_NEGOTIATION_ABORT_FLAG && tcp_sk(sk)->syn_smc)
			break;
		isck_smc_negotiation_set_flags(smc_sk(sk), flag);
		fallthrough;
	case SMC_NEGOTIATION_NO_SMC:
		success = 1;
	default:
		break;
	}
	write_unlock_bh(&sk->sk_callback_lock);
	return success;
}

static inline int smc_inet_sock_rectify_state(struct sock *sk)
{
	int cur = isck_smc_negotiation_load(smc_sk(sk));

	switch (cur) {
	case SMC_NEGOTIATION_TBD:
		if (!smc_inet_sock_try_disable_smc(sk, SMC_NEGOTIATION_NOT_SUPPORT_FLAG))
			break;
		fallthrough;
	case SMC_NEGOTIATION_NO_SMC:
		return SMC_NEGOTIATION_NO_SMC;
	default:
		break;
	}
	return cur;
}

static __always_inline void smc_inet_sock_init_accompany_socket(struct sock *sk)
{
	struct smc_sock *smc = smc_sk(sk);

	smc->accompany_socket.sk = sk;
	init_waitqueue_head(&smc->accompany_socket.wq.wait);
	smc->accompany_socket.ops = &smc_inet_clcsock_ops;
	smc->accompany_socket.state = SS_UNCONNECTED;

	smc->clcsock = &smc->accompany_socket;
}

#if IS_ENABLED(CONFIG_IPV6)
#define smc_call_inet_sock_ops(sk, inet, inet6, ...) ({		\
	(sk)->sk_family == PF_INET ? inet(__VA_ARGS__) :	\
		inet6(__VA_ARGS__);				\
})
#else
#define smc_call_inet_sock_ops(sk, inet, inet6, ...)	inet(__VA_ARGS__)
#endif
#define SMC_REQSK_SMC	0x01
#define SMC_REQSK_TCP	0x02

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

int smc_inet_sock_pre_connect(struct sock *sk, struct sockaddr *uaddr,
			    int addr_len);

#endif // __SMC_INET
