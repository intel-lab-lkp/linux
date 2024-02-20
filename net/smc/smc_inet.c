// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Shared Memory Communications over RDMA (SMC-R) and RoCE
 *
 *  AF_SMC protocol family socket handler keeping the AF_INET sock address type
 *  applies to SOCK_STREAM sockets only
 *  offers an alternative communication option for TCP-protocol sockets
 *  applicable with RoCE-cards only
 *
 *  Initial restrictions:
 *    - support for alternate links postponed
 *
 *  Copyright IBM Corp. 2016, 2018
 *
 */

#include <net/sock.h>
#include <net/inet_common.h>

#include "smc_inet.h"
#include "smc.h"

static struct timewait_sock_ops smc_timewait_sock_ops = {
	.twsk_obj_size		= sizeof(struct tcp_timewait_sock),
	.twsk_unique		= tcp_twsk_unique,
	.twsk_destructor	= tcp_twsk_destructor,
};

static struct timewait_sock_ops smc6_timewait_sock_ops = {
	.twsk_obj_size		= sizeof(struct tcp6_timewait_sock),
	.twsk_unique		= tcp_twsk_unique,
	.twsk_destructor	= tcp_twsk_destructor,
};

struct proto smc_inet_prot = {
	.name			= "SMC",
	.owner			= THIS_MODULE,
	.close			= tcp_close,
	.pre_connect		= NULL,
	.connect		= tcp_v4_connect,
	.disconnect		= tcp_disconnect,
	.accept			= smc_inet_csk_accept,
	.ioctl			= tcp_ioctl,
	.init			= smc_inet_init_sock,
	.destroy		= tcp_v4_destroy_sock,
	.shutdown		= tcp_shutdown,
	.setsockopt		= tcp_setsockopt,
	.getsockopt		= tcp_getsockopt,
	.keepalive		= tcp_set_keepalive,
	.recvmsg		= tcp_recvmsg,
	.sendmsg		= tcp_sendmsg,
	.backlog_rcv		= tcp_v4_do_rcv,
	.release_cb		= smc_inet_sock_proto_release_cb,
	.hash			= inet_hash,
	.unhash			= inet_unhash,
	.get_port		= inet_csk_get_port,
	.enter_memory_pressure	= tcp_enter_memory_pressure,
	.per_cpu_fw_alloc       = &tcp_memory_per_cpu_fw_alloc,
	.leave_memory_pressure	= tcp_leave_memory_pressure,
	.stream_memory_free	= tcp_stream_memory_free,
	.sockets_allocated	= &tcp_sockets_allocated,
	.orphan_count		= &tcp_orphan_count,
	.memory_allocated	= &tcp_memory_allocated,
	.memory_pressure	= &tcp_memory_pressure,
	.sysctl_mem		= sysctl_tcp_mem,
	.sysctl_wmem_offset	= offsetof(struct net, ipv4.sysctl_tcp_wmem),
	.sysctl_rmem_offset	= offsetof(struct net, ipv4.sysctl_tcp_rmem),
	.max_header		= MAX_TCP_HEADER,
	.obj_size		= sizeof(struct smc_sock),
	.slab_flags		= SLAB_TYPESAFE_BY_RCU,
	.twsk_prot		= &smc_timewait_sock_ops,
	/* tcp_conn_request will use tcp_request_sock_ops */
	.rsk_prot		= NULL,
	.h.hashinfo		= &tcp_hashinfo,
	.no_autobind		= true,
	.diag_destroy		= tcp_abort,
};
EXPORT_SYMBOL_GPL(smc_inet_prot);

const struct proto_ops smc_inet_stream_ops = {
	.family		   = PF_INET,
	.owner		   = THIS_MODULE,
	.release	   = smc_inet_release,
	.bind		   = inet_bind,
	.connect	   = smc_inet_connect,
	.socketpair	   = sock_no_socketpair,
	.accept		   = inet_accept,
	.getname	   = inet_getname,
	.poll		   = smc_inet_poll,
	.ioctl		   = smc_inet_ioctl,
	.gettstamp	   = sock_gettstamp,
	.listen		   = smc_inet_listen,
	.shutdown	   = smc_inet_shutdown,
	.setsockopt	   = smc_inet_setsockopt,
	.getsockopt	   = smc_inet_getsockopt,
	.sendmsg	   = smc_inet_sendmsg,
	.recvmsg	   = smc_inet_recvmsg,
#ifdef CONFIG_MMU
	.mmap		   = tcp_mmap,
#endif
	.splice_read	   = smc_inet_splice_read,
	.read_sock	   = tcp_read_sock,
	.sendmsg_locked    = tcp_sendmsg_locked,
	.peek_len	   = tcp_peek_len,
#ifdef CONFIG_COMPAT
	.compat_ioctl	   = inet_compat_ioctl,
#endif
	.set_rcvlowat	   = tcp_set_rcvlowat,
};

struct inet_protosw smc_inet_protosw = {
	.type       = SOCK_STREAM,
	.protocol   = IPPROTO_SMC,
	.prot   = &smc_inet_prot,
	.ops    = &smc_inet_stream_ops,
	.flags  = INET_PROTOSW_ICSK,
};

#if IS_ENABLED(CONFIG_IPV6)
struct proto smc_inet6_prot = {
	.name			= "SMCv6",
	.owner			= THIS_MODULE,
	.close			= tcp_close,
	.pre_connect		= NULL,
	.connect		= NULL,
	.disconnect		= tcp_disconnect,
	.accept			= smc_inet_csk_accept,
	.ioctl			= tcp_ioctl,
	.init			= smc_inet_init_sock,
	.destroy		= NULL,
	.shutdown		= tcp_shutdown,
	.setsockopt		= tcp_setsockopt,
	.getsockopt		= tcp_getsockopt,
	.keepalive		= tcp_set_keepalive,
	.recvmsg		= tcp_recvmsg,
	.sendmsg		= tcp_sendmsg,
	.backlog_rcv		= NULL,
	.release_cb		= smc_inet_sock_proto_release_cb,
	.hash			= NULL,
	.unhash			= inet_unhash,
	.get_port		= inet_csk_get_port,
	.enter_memory_pressure	= tcp_enter_memory_pressure,
	.per_cpu_fw_alloc       = &tcp_memory_per_cpu_fw_alloc,
	.leave_memory_pressure	= tcp_leave_memory_pressure,
	.stream_memory_free	= tcp_stream_memory_free,
	.sockets_allocated	= &tcp_sockets_allocated,
	.memory_allocated	= &tcp_memory_allocated,
	.memory_pressure	= &tcp_memory_pressure,
	.orphan_count		= &tcp_orphan_count,
	.sysctl_mem		= sysctl_tcp_mem,
	.sysctl_wmem_offset	= offsetof(struct net, ipv4.sysctl_tcp_wmem),
	.sysctl_rmem_offset	= offsetof(struct net, ipv4.sysctl_tcp_rmem),
	.max_header		= MAX_TCP_HEADER,
	.obj_size		= sizeof(struct smc_sock),
	.ipv6_pinfo_offset	= offsetof(struct tcp6_sock, inet6),
	.slab_flags		= SLAB_TYPESAFE_BY_RCU,
	.twsk_prot		= &smc6_timewait_sock_ops,
	/* tcp_conn_request will use tcp_request_sock_ops */
	.rsk_prot		= NULL,
	.h.hashinfo		= &tcp_hashinfo,
	.no_autobind		= true,
	.diag_destroy		= tcp_abort,
};
EXPORT_SYMBOL_GPL(smc_inet6_prot);

const struct proto_ops smc_inet6_stream_ops = {
	.family		   = PF_INET6,
	.owner		   = THIS_MODULE,
	.release	   = smc_inet_release,
	.bind		   = inet6_bind,
	.connect	   = smc_inet_connect,	/* ok		*/
	.socketpair	   = sock_no_socketpair,	/* a do nothing	*/
	.accept		   = inet_accept,		/* ok		*/
	.getname	   = inet6_getname,
	.poll		   = smc_inet_poll,			/* ok		*/
	.ioctl		   = smc_inet_ioctl,		/* must change  */
	.gettstamp	   = sock_gettstamp,
	.listen		   = smc_inet_listen,		/* ok		*/
	.shutdown	   = smc_inet_shutdown,		/* ok		*/
	.setsockopt	   = smc_inet_setsockopt,	/* ok		*/
	.getsockopt	   = smc_inet_getsockopt,	/* ok		*/
	.sendmsg	   = smc_inet_sendmsg,		/* retpoline's sake */
	.recvmsg	   = smc_inet_recvmsg,		/* retpoline's sake */
#ifdef CONFIG_MMU
	.mmap		   = tcp_mmap,
#endif
	.sendmsg_locked    = tcp_sendmsg_locked,
	.splice_read	   = smc_inet_splice_read,
	.read_sock	   = tcp_read_sock,
	.peek_len	   = tcp_peek_len,
#ifdef CONFIG_COMPAT
	.compat_ioctl	   = inet6_compat_ioctl,
#endif
	.set_rcvlowat	   = tcp_set_rcvlowat,
};

struct inet_protosw smc_inet6_protosw = {
	.type       = SOCK_STREAM,
	.protocol   = IPPROTO_SMC,
	.prot   = &smc_inet6_prot,
	.ops    = &smc_inet6_stream_ops,
	.flags  = INET_PROTOSW_ICSK,
};
#endif

int smc_inet_sock_init(void)
{
	struct proto *tcp_v4prot;
#if IS_ENABLED(CONFIG_IPV6)
	struct proto *tcp_v6prot;
#endif

	tcp_v4prot = smc_inet_get_tcp_prot(PF_INET);
	if (unlikely(!tcp_v4prot))
		return -EINVAL;

#if IS_ENABLED(CONFIG_IPV6)
	tcp_v6prot = smc_inet_get_tcp_prot(PF_INET6);
	if (unlikely(!tcp_v6prot))
		return -EINVAL;
#endif

	/* INET sock has a issues here. twsk will hold the reference of the this module,
	 * so it may be found that the SMC module cannot be uninstalled after the test program ends,
	 * But eventually, twsk will release the reference of the module.
	 * This may affect some old test cases if they try to remove the module immediately after
	 * completing their test.
	 */

	/* Complete the full prot and proto_ops to
	 * ensure consistency with TCP. Some symbols here have not been exported,
	 * so that we have to assign it here.
	 */
	smc_inet_prot.pre_connect = tcp_v4prot->pre_connect;

#if IS_ENABLED(CONFIG_IPV6)
	smc_inet6_prot.pre_connect = tcp_v6prot->pre_connect;
	smc_inet6_prot.connect = tcp_v6prot->connect;
	smc_inet6_prot.destroy = tcp_v6prot->destroy;
	smc_inet6_prot.backlog_rcv = tcp_v6prot->backlog_rcv;
	smc_inet6_prot.hash = tcp_v6prot->hash;
#endif
	return 0;
}

int smc_inet_init_sock(struct sock *sk) { return  0; }

void smc_inet_sock_proto_release_cb(struct sock *sk) {}

int smc_inet_connect(struct socket *sock, struct sockaddr *addr,
		     int alen, int flags)
{
	return -EOPNOTSUPP;
}

int smc_inet_setsockopt(struct socket *sock, int level, int optname,
			sockptr_t optval, unsigned int optlen)
{
	return -EOPNOTSUPP;
}

int smc_inet_getsockopt(struct socket *sock, int level, int optname,
			char __user *optval, int __user *optlen)
{
	return -EOPNOTSUPP;
}

int smc_inet_ioctl(struct socket *sock, unsigned int cmd,
		   unsigned long arg)
{
	return -EOPNOTSUPP;
}

int smc_inet_sendmsg(struct socket *sock, struct msghdr *msg, size_t len)
{
	return -EOPNOTSUPP;
}

int smc_inet_recvmsg(struct socket *sock, struct msghdr *msg, size_t len,
		     int flags)
{
	return -EOPNOTSUPP;
}

ssize_t smc_inet_splice_read(struct socket *sock, loff_t *ppos,
			     struct pipe_inode_info *pipe, size_t len,
			     unsigned int flags)
{
	return -EOPNOTSUPP;
}

__poll_t smc_inet_poll(struct file *file, struct socket *sock, poll_table *wait)
{
	return 0;
}

struct sock *smc_inet_csk_accept(struct sock *sk, int flags, int *err, bool kern)
{
	return NULL;
}

int smc_inet_listen(struct socket *sock, int backlog)
{
	return -EOPNOTSUPP;
}

int smc_inet_shutdown(struct socket *sock, int how)
{
	return -EOPNOTSUPP;
}

int smc_inet_release(struct socket *sock)
{
	return -EOPNOTSUPP;
}
