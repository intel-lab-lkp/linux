// SPDX-License-Identifier: BSD-2-Clause or GPL-2.0+

/* This file consists mostly of "glue" that hooks Homa into the rest of
 * the Linux kernel. The guts of the protocol are in other files.
 */

#include "homa_impl.h"
#include "homa_peer.h"
#include "homa_pool.h"

/* Identifier for retrieving Homa-specific data for a struct net. */
unsigned int homa_net_id;

/* This structure defines functions that allow Homa to be used as a
 * pernet subsystem.
 */
static struct pernet_operations homa_net_ops = {
	.init = homa_net_start,
	.exit = homa_net_exit,
	.id = &homa_net_id,
	.size = sizeof(struct homa_net)
};

/* Global data for Homa. Avoid referencing directly except when there is
 * no alternative (instead, use a homa pointer stored in a struct or
 * passed via a parameter). This allows overriding during unit tests.
 */
static struct homa homa_data;

/* This structure defines functions that handle various operations on
 * Homa sockets. These functions are relatively generic: they are called
 * to implement top-level system calls. Many of these operations can
 * be implemented by PF_INET6 functions that are independent of the
 * Homa protocol.
 */
static const struct proto_ops homa_proto_ops = {
	.family		   = PF_INET,
	.owner		   = THIS_MODULE,
	.release	   = inet_release,
	.bind		   = homa_bind,
	.connect	   = inet_dgram_connect,
	.socketpair	   = sock_no_socketpair,
	.accept		   = sock_no_accept,
	.getname	   = inet_getname,
	.poll		   = homa_poll,
	.ioctl		   = homa_ioctl,
	.listen		   = sock_no_listen,
	.shutdown	   = homa_shutdown,
	.setsockopt	   = sock_common_setsockopt,
	.getsockopt	   = sock_common_getsockopt,
	.sendmsg	   = inet_sendmsg,
	.recvmsg	   = inet_recvmsg,
	.mmap		   = sock_no_mmap,
	.set_peek_off	   = sk_set_peek_off,
};

static const struct proto_ops homav6_proto_ops = {
	.family		   = PF_INET6,
	.owner		   = THIS_MODULE,
	.release	   = inet6_release,
	.bind		   = homa_bind,
	.connect	   = inet_dgram_connect,
	.socketpair	   = sock_no_socketpair,
	.accept		   = sock_no_accept,
	.getname	   = inet6_getname,
	.poll		   = homa_poll,
	.ioctl		   = homa_ioctl,
	.listen		   = sock_no_listen,
	.shutdown	   = homa_shutdown,
	.setsockopt	   = sock_common_setsockopt,
	.getsockopt	   = sock_common_getsockopt,
	.sendmsg	   = inet_sendmsg,
	.recvmsg	   = inet_recvmsg,
	.mmap		   = sock_no_mmap,
	.set_peek_off	   = sk_set_peek_off,
};

/* This structure also defines functions that handle various operations
 * on Homa sockets. However, these functions are lower-level than those
 * in homa_proto_ops: they are specific to the PF_INET or PF_INET6
 * protocol family, and in many cases they are invoked by functions in
 * homa_proto_ops. Most of these functions have Homa-specific implementations.
 */
static struct proto homa_prot = {
	.name		   = "HOMA",
	.owner		   = THIS_MODULE,
	.close		   = homa_close,
	.connect	   = ip4_datagram_connect,
	.init		   = homa_socket,
	.destroy	   = homa_sock_destroy,
	.setsockopt	   = homa_setsockopt,
	.getsockopt	   = homa_getsockopt,
	.sendmsg	   = homa_sendmsg,
	.recvmsg	   = homa_recvmsg,
	.hash		   = homa_hash,
	.unhash		   = homa_unhash,
	.obj_size	   = sizeof(struct homa_sock),
	.no_autobind       = 1,
};

static struct proto homav6_prot = {
	.name		   = "HOMAv6",
	.owner		   = THIS_MODULE,
	.close		   = homa_close,
	.connect	   = ip6_datagram_connect,
	.init		   = homa_socket,
	.destroy	   = homa_sock_destroy,
	.setsockopt	   = homa_setsockopt,
	.getsockopt	   = homa_getsockopt,
	.sendmsg	   = homa_sendmsg,
	.recvmsg	   = homa_recvmsg,
	.hash		   = homa_hash,
	.unhash		   = homa_unhash,
	.obj_size	   = sizeof(struct homa_v6_sock),
	.ipv6_pinfo_offset = offsetof(struct homa_v6_sock, inet6),
	.no_autobind       = 1,
};

/* Top-level structure describing the Homa protocol. */
static struct inet_protosw homa_protosw = {
	.type              = SOCK_DGRAM,
	.protocol          = IPPROTO_HOMA,
	.prot              = &homa_prot,
	.ops               = &homa_proto_ops,
	.flags             = INET_PROTOSW_REUSE,
};

static struct inet_protosw homav6_protosw = {
	.type              = SOCK_DGRAM,
	.protocol          = IPPROTO_HOMA,
	.prot              = &homav6_prot,
	.ops               = &homav6_proto_ops,
	.flags             = INET_PROTOSW_REUSE,
};

/* This structure is used by IP to deliver incoming Homa packets to us. */
static struct net_protocol homa_protocol = {
	.handler =	homa_softirq,
	.err_handler =	homa_err_handler_v4,
	.no_policy =     1,
};

static struct inet6_protocol homav6_protocol = {
	.handler =	homa_softirq,
	.err_handler =	homa_err_handler_v6,
	.flags =        INET6_PROTO_NOPOLICY | INET6_PROTO_FINAL,
};

/* Sizes of the headers for each Homa packet type, in bytes. */
static u16 header_lengths[] = {
	sizeof(struct homa_data_hdr),
	0,
	sizeof(struct homa_resend_hdr),
	sizeof(struct homa_rpc_unknown_hdr),
	sizeof(struct homa_busy_hdr),
	0,
	0,
	sizeof(struct homa_need_ack_hdr),
	sizeof(struct homa_ack_hdr)
};

/* Thread that runs timer code to detect lost packets and crashed peers. */
static struct task_struct *timer_kthread;
static DECLARE_COMPLETION(timer_thread_done);

/* Used to wakeup timer_kthread at regular intervals. */
static struct hrtimer hrtimer;

/* Nonzero is an indication to the timer thread that it should exit. */
static int timer_thread_exit;

/**
 * homa_load() - invoked when this module is loaded into the Linux kernel
 * Return: 0 on success, otherwise a negative errno.
 */
int __init homa_load(void)
{
	struct homa *homa = &homa_data;
	bool init_protocol6 = false;
	bool init_protosw6 = false;
	bool init_protocol = false;
	bool init_protosw = false;
	bool init_net_ops = false;
	bool init_proto6 = false;
	bool init_proto = false;
	bool init_homa = false;
	int status;

	/* Compile-time validations that no packet header is longer
	 * than HOMA_MAX_HEADER.
	 */
	BUILD_BUG_ON(sizeof(struct homa_data_hdr) > HOMA_MAX_HEADER);
	BUILD_BUG_ON(sizeof(struct homa_resend_hdr) > HOMA_MAX_HEADER);
	BUILD_BUG_ON(sizeof(struct homa_rpc_unknown_hdr) > HOMA_MAX_HEADER);
	BUILD_BUG_ON(sizeof(struct homa_busy_hdr) > HOMA_MAX_HEADER);
	BUILD_BUG_ON(sizeof(struct homa_need_ack_hdr) > HOMA_MAX_HEADER);
	BUILD_BUG_ON(sizeof(struct homa_ack_hdr) > HOMA_MAX_HEADER);

	/* Extra constraints on data packets:
	 * - Ensure minimum header length so Homa doesn't have to worry about
	 *   padding data packets.
	 * - Make sure data packet headers are a multiple of 4 bytes (needed
	 *   for TCP/TSO compatibility).
	 */
	BUILD_BUG_ON(sizeof(struct homa_data_hdr) < HOMA_MIN_PKT_LENGTH);
	BUILD_BUG_ON((sizeof(struct homa_data_hdr) -
		      sizeof(struct homa_seg_hdr)) & 0x3);

	/* Detect size changes in uAPI structs. */
	BUILD_BUG_ON(sizeof(struct homa_sendmsg_args) != 24);
	BUILD_BUG_ON(sizeof(struct homa_recvmsg_args) != 88);

	status = homa_init(homa);
	if (status)
		goto error;
	init_homa = true;

	status = proto_register(&homa_prot, 1);
	if (status != 0) {
		pr_err("proto_register failed for homa_prot: %d\n", status);
		goto error;
	}
	init_proto = true;

	status = proto_register(&homav6_prot, 1);
	if (status != 0) {
		pr_err("proto_register failed for homav6_prot: %d\n", status);
		goto error;
	}
	init_proto6 = true;

	inet_register_protosw(&homa_protosw);
	init_protosw = true;

	status = inet6_register_protosw(&homav6_protosw);
	if (status != 0) {
		pr_err("inet6_register_protosw failed in %s: %d\n", __func__,
		       status);
		goto error;
	}
	init_protosw6 = true;

	status = inet_add_protocol(&homa_protocol, IPPROTO_HOMA);
	if (status != 0) {
		pr_err("inet_add_protocol failed in %s: %d\n", __func__,
		       status);
		goto error;
	}
	init_protocol = true;

	status = inet6_add_protocol(&homav6_protocol, IPPROTO_HOMA);
	if (status != 0) {
		pr_err("inet6_add_protocol failed in %s: %d\n",  __func__,
		       status);
		goto error;
	}
	init_protocol6 = true;

	status = register_pernet_subsys(&homa_net_ops);
	if (status != 0) {
		pr_err("Homa got error from register_pernet_subsys: %d\n",
		       status);
		goto error;
	}
	init_net_ops = true;

	timer_kthread = kthread_run(homa_timer_main, homa, "homa_timer");
	if (IS_ERR(timer_kthread)) {
		status = PTR_ERR(timer_kthread);
		pr_err("couldn't create Homa timer thread: error %d\n",
		       status);
		timer_kthread = NULL;
		goto error;
	}

	return 0;

error:
	if (timer_kthread) {
		timer_thread_exit = 1;
		wake_up_process(timer_kthread);
		wait_for_completion(&timer_thread_done);
	}
	if (init_net_ops)
		unregister_pernet_subsys(&homa_net_ops);
	if (init_homa)
		homa_destroy(homa);
	if (init_protocol)
		inet_del_protocol(&homa_protocol, IPPROTO_HOMA);
	if (init_protocol6)
		inet6_del_protocol(&homav6_protocol, IPPROTO_HOMA);
	if (init_protosw)
		inet_unregister_protosw(&homa_protosw);
	if (init_protosw6)
		inet6_unregister_protosw(&homav6_protosw);
	if (init_proto)
		proto_unregister(&homa_prot);
	if (init_proto6)
		proto_unregister(&homav6_prot);
	return status;
}

/**
 * homa_unload() - invoked when this module is unloaded from the Linux kernel.
 */
void __exit homa_unload(void)
{
	struct homa *homa = &homa_data;

	pr_notice("Homa module unloading\n");

	if (timer_kthread) {
		timer_thread_exit = 1;
		wake_up_process(timer_kthread);
		wait_for_completion(&timer_thread_done);
	}
	unregister_pernet_subsys(&homa_net_ops);
	inet_del_protocol(&homa_protocol, IPPROTO_HOMA);
	inet_unregister_protosw(&homa_protosw);
	inet6_del_protocol(&homav6_protocol, IPPROTO_HOMA);
	inet6_unregister_protosw(&homav6_protosw);
	proto_unregister(&homa_prot);
	proto_unregister(&homav6_prot);
	homa_destroy(homa);
}

module_init(homa_load);
module_exit(homa_unload);

/**
 * homa_net_start() - Initialize Homa for a new network namespace.
 * @net:    The net that Homa will be associated with.
 * Return:  0 on success, otherwise a negative errno.
 */
int homa_net_start(struct net *net)
{
	pr_notice("Homa attaching to net namespace\n");
	return homa_net_init(homa_net(net), net, &homa_data);
}

/**
 * homa_net_exit() - Perform Homa cleanup needed when a network namespace
 * is destroyed.
 * @net:    The net from which Homa should be removed.
 */
void homa_net_exit(struct net *net)
{
	pr_notice("Homa detaching from net namespace\n");
	homa_net_destroy(homa_net(net));
}

/**
 * homa_bind() - Implements the bind system call for Homa sockets: associates
 * a well-known service port with a socket. Unlike other AF_INET6 protocols,
 * there is no need to invoke this system call for sockets that are only
 * used as clients.
 * @sock:     Socket on which the system call was invoked.
 * @addr:     Contains the desired port number.
 * @addr_len: Number of bytes in uaddr.
 * Return:    0 on success, otherwise a negative errno. Sets hsk->error_msg
 *            on errors.
 */
int homa_bind(struct socket *sock, struct sockaddr_unsized *addr, int addr_len)
{
	union sockaddr_in_union *addr_in = (union sockaddr_in_union *)addr;
	struct homa_sock *hsk = homa_sk(sock->sk);
	int port = 0;

	if (unlikely(addr->sa_family != sock->sk->sk_family)) {
		hsk->error_msg = "address family in bind address didn't match socket";
		return -EAFNOSUPPORT;
	}
	if (addr_in->in6.sin6_family == AF_INET6) {
		if (addr_len < sizeof(struct sockaddr_in6)) {
			hsk->error_msg = "ipv6 address too short";
			return -EINVAL;
		}
		port = ntohs(addr_in->in4.sin_port);
	} else if (addr_in->in4.sin_family == AF_INET) {
		if (addr_len < sizeof(struct sockaddr_in)) {
			hsk->error_msg = "ipv4 address too short";
			return -EINVAL;
		}
		port = ntohs(addr_in->in6.sin6_port);
	}
	return homa_sock_bind(hsk->hnet, hsk, port);
}

/**
 * homa_close() - Invoked when close system call is invoked on a Homa socket.
 * @sk:      Socket being closed
 * @timeout: ??
 */
void homa_close(struct sock *sk, long timeout)
{
	struct homa_sock *hsk = homa_sk(sk);

	homa_sock_shutdown(hsk);
	sk_common_release(sk);
}

/**
 * homa_shutdown() - Implements the shutdown system call for Homa sockets.
 * @sock:    Socket to shut down.
 * @how:     Ignored: for other sockets, can independently shut down
 *           sending and receiving, but for Homa any shutdown will
 *           shut down everything.
 *
 * Return: 0 on success, otherwise a negative errno.
 */
int homa_shutdown(struct socket *sock, int how)
{
	homa_sock_shutdown(homa_sk(sock->sk));
	return 0;
}

/**
 * homa_ioc_info() - The top-level function that implements the
 * HOMAIOCINFO ioctl for Homa sockets.
 * @sock:  Socket for this request
 * @arg:   The address in user space of the argument to ioctl, which
 *          is a homa_info struct.
 *
 * Return:  0 on success, otherwise a negative errno. Sets hsk->error_msg
 *          on errors.
 */
int homa_ioc_info(struct socket *sock, unsigned long arg)
{
	struct homa_sock *hsk = homa_sk(sock->sk);
	struct homa_rpc **rpcs = NULL;
	struct homa_rpc_info rinfo;
	struct homa_info hinfo;
	struct homa_rpc *rpc;
	int result = 0;
	int bytes_avl;
	int num_rpcs;
	char *dst;
	int i;

	if (unlikely(copy_from_user(&hinfo, (void __user *)arg,
				    sizeof(hinfo)))) {
		hsk->error_msg = "invalid address for homa_info";
		return -EFAULT;
	}

	if (!homa_protect_rpcs(hsk)) {
		hsk->error_msg = "socket has been shut down";
		return -ESHUTDOWN;
	}
	hinfo.bpool_avail_bytes = homa_pool_avail_bytes(hsk->buffer_pool);
	hinfo.port = hsk->port;
	dst = (char *)hinfo.rpc_info;
	bytes_avl = hinfo.rpc_info_length;
	hinfo.num_rpcs = 0;

	/* This function is tricky because we need to hold an RCU lock
	 * while scanning the RPCs of the socket, but we can't hold the
	 * RCU lock while copying status information out to user space.
	 * Thus, we first collect pointers to all the RPCs in a separate
	 * array (while holding an RCU lock), acquire a reference on each
	 * RPC, then release the RCU lock and copy out to user space.
	 */
	num_rpcs = 0;
	rcu_read_lock();
	list_for_each_entry_rcu(rpc, &hsk->active_rpcs, active_links)
		num_rpcs++;

	if (num_rpcs > 0) {
		/* Allocate an array and populate it with homa_rpc pointers.
		 * Must release the RCU lock temporarily while allocating.
		 */
		rcu_read_unlock();
		rpcs = kmalloc_objs(*rpcs, num_rpcs);
		if (!rpcs) {
			homa_unprotect_rpcs(hsk);
			return -ENOMEM;
		}
		rcu_read_lock();
		i = 0;
		list_for_each_entry_rcu(rpc, &hsk->active_rpcs, active_links) {
			homa_rpc_lock(rpc);
			if (rpc->state != RPC_DEAD) {
				rpcs[i] = rpc;
				homa_rpc_hold(rpc);
				i++;
			}
			homa_rpc_unlock(rpc);
			if (i == num_rpcs)
				break;
		}
		/* The number of RPCs could have changed between the first
		 * pass and the second pass.
		 */
		num_rpcs = i;
	}
	rcu_read_unlock();
	homa_unprotect_rpcs(hsk);

	/* Now scan the array and copy information out to user space.  Note
	 * that RPCs could have ended since we added them to the array.
	 */
	for (i = 0; i < num_rpcs; i++) {
		rpc = rpcs[i];
		homa_rpc_lock(rpc);
		homa_rpc_put(rpc);
		if (rpc->state == RPC_DEAD) {
			homa_rpc_unlock(rpc);
			continue;
		}
		homa_rpc_get_info(rpc, &rinfo);
		if (dst && bytes_avl >= sizeof(rinfo) && result == 0) {
			if (copy_to_user((void __user *)dst, &rinfo,
					 sizeof(rinfo))) {
				hsk->error_msg = "couldn't copy homa_rpc_info to user space: invalid or read-only address?";
				result = -EFAULT;
			}
			dst += sizeof(rinfo);
			bytes_avl -= sizeof(rinfo);
		}
		homa_rpc_unlock(rpc);
		hinfo.num_rpcs++;
	}
	kfree(rpcs);

	if (hsk->error_msg)
		snprintf(hinfo.error_msg, HOMA_ERROR_MSG_SIZE, "%s",
			 hsk->error_msg);
	else
		hinfo.error_msg[0] = 0;

	if (copy_to_user((void __user *)arg, &hinfo, sizeof(hinfo))) {
		hsk->error_msg = "couldn't copy homa_info to user space: read-only address?";
		return -EFAULT;
	}
	return result;
}

/**
 * homa_ioctl() - Implements the ioctl system call for Homa sockets.
 * @sock:  Socket on which the system call was invoked.
 * @cmd:   Identifier for a particular ioctl operation.
 * @arg:   Operation-specific argument; typically the address of a block
 *         of data in user address space.
 *
 * Return: 0 on success, otherwise a negative errno. Sets hsk->error_msg
 *         on errors.
 */
int homa_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
	if (cmd == HOMAIOCINFO)
		return homa_ioc_info(sock, arg);
	homa_sk(sock->sk)->error_msg = "ioctl opcode isn't supported by Homa";
	return -EINVAL;
}

/**
 * homa_socket() - Implements the socket(2) system call for sockets.
 * @sk:    Socket on which the system call was invoked. The non-Homa
 *         parts have already been initialized.
 *
 * Return: always 0 (success).
 */
int homa_socket(struct sock *sk)
{
	return homa_sock_init(homa_sk(sk));
}

/**
 * homa_setsockopt() - Implements the setsockopt system call for Homa sockets.
 * @sk:      Socket on which the system call was invoked.
 * @level:   Level at which the operation should be handled; will always
 *           be IPPROTO_HOMA.
 * @optname: Identifies a particular setsockopt operation.
 * @optval:  Address in user space of information about the option.
 * @optlen:  Number of bytes of data at @optval.
 * Return:   0 on success, otherwise a negative errno. Sets hsk->error_msg
 *           on errors.
 */
int homa_setsockopt(struct sock *sk, int level, int optname,
		    sockptr_t optval, unsigned int optlen)
{
	struct homa_sock *hsk = homa_sk(sk);
	int ret;

	if (level != IPPROTO_HOMA) {
		hsk->error_msg = "homa_setsockopt invoked with level not IPPROTO_HOMA";
		return -ENOPROTOOPT;
	}

	if (optname == SO_HOMA_RCVBUF) {
		struct homa_rcvbuf_args args;

		if (optlen != sizeof(struct homa_rcvbuf_args)) {
			hsk->error_msg = "invalid optlen argument: must be sizeof(struct homa_rcvbuf_args)";
			return -EINVAL;
		}

		if (copy_from_sockptr(&args, optval, optlen)) {
			hsk->error_msg = "invalid address for homa_rcvbuf_args";
			return -EFAULT;
		}

		/* Do a trivial test to make sure we can at least write the
		 * first page of the region.
		 */
		if (copy_to_user(u64_to_user_ptr(args.start), &args,
				 sizeof(args))) {
			hsk->error_msg = "receive buffer region is not writable";
			return -EFAULT;
		}

		ret = homa_pool_set_region(hsk, u64_to_user_ptr(args.start),
					   args.length);
	} else if (optname == SO_HOMA_SERVER) {
		int arg;

		if (optlen != sizeof(arg)) {
			hsk->error_msg = "invalid optlen argument: must be sizeof(int)";
			return -EINVAL;
		}

		if (copy_from_sockptr(&arg, optval, optlen)) {
			hsk->error_msg = "invalid address for SO_HOMA_SERVER value";
			return -EFAULT;
		}

		if (arg)
			hsk->is_server = true;
		else
			hsk->is_server = false;
		ret = 0;
	} else {
		hsk->error_msg = "setsockopt option not supported by Homa";
		ret = -ENOPROTOOPT;
	}
	return ret;
}

/**
 * homa_getsockopt() - Implements the getsockopt system call for Homa sockets.
 * @sk:      Socket on which the system call was invoked.
 * @level:   Selects level in the network stack to handle the request;
 *           must be IPPROTO_HOMA.
 * @optname: Identifies a particular setsockopt operation.
 * @optval:  Address in user space where the option's value should be stored.
 * @optlen:  Number of bytes available at optval; will be overwritten with
 *           actual number of bytes stored.
 * Return:   0 on success, otherwise a negative errno. Sets hsk->error_msg
 *           on errors.
 */
int homa_getsockopt(struct sock *sk, int level, int optname,
		    char __user *optval, int __user *optlen)
{
	struct homa_sock *hsk = homa_sk(sk);
	struct homa_rcvbuf_args rcvbuf_args;
	int is_server;
	void *result;
	int len;

	if (copy_from_sockptr(&len, USER_SOCKPTR(optlen), sizeof(int))) {
		hsk->error_msg = "invalid address for optlen argument to getsockopt";
		return -EFAULT;
	}

	if (level != IPPROTO_HOMA) {
		hsk->error_msg = "homa_getsockopt invoked with level not IPPROTO_HOMA";
		return -ENOPROTOOPT;
	}
	if (optname == SO_HOMA_RCVBUF) {
		if (len < sizeof(rcvbuf_args)) {
			hsk->error_msg = "invalid optlen argument: must be sizeof(struct homa_rcvbuf_args)";
			return -EINVAL;
		}

		homa_sock_lock(hsk);
		homa_pool_get_rcvbuf(hsk->buffer_pool, &rcvbuf_args);
		homa_sock_unlock(hsk);
		len = sizeof(rcvbuf_args);
		result = &rcvbuf_args;
	} else if (optname == SO_HOMA_SERVER) {
		if (len < sizeof(is_server)) {
			hsk->error_msg = "invalid optlen argument: must be sizeof(int)";
			return -EINVAL;
		}

		is_server = hsk->is_server;
		len = sizeof(is_server);
		result = &is_server;
	} else {
		hsk->error_msg = "getsockopt option not supported by Homa";
		return -ENOPROTOOPT;
	}

	if (copy_to_sockptr(USER_SOCKPTR(optlen), &len, sizeof(int))) {
		hsk->error_msg = "couldn't update optlen argument to getsockopt: read-only?";
		return -EFAULT;
	}

	if (copy_to_sockptr(USER_SOCKPTR(optval), result, len)) {
		hsk->error_msg = "couldn't update optval argument to getsockopt: read-only?";
		return -EFAULT;
	}

	return 0;
}

/**
 * homa_sendmsg() - Send a request or response message on a Homa socket.
 * @sk:     Socket on which the system call was invoked.
 * @msg:    Structure describing the message to send; the msg_control
 *          field points to additional information.
 * @length: Number of bytes of the message.
 * Return:  0 on success, otherwise a negative errno. Sets hsk->error_msg
 *          on errors.
 */
int homa_sendmsg(struct sock *sk, struct msghdr *msg, size_t length)
{
	DECLARE_SOCKADDR(union sockaddr_in_union *, addr, msg->msg_name);
	struct homa_sock *hsk = homa_sk(sk);
	struct homa_sendmsg_args args;
	struct homa_rpc *rpc = NULL;
	int result = 0;

	if (!addr) {
		hsk->error_msg = "no msg_name passed to sendmsg";
		result = -EINVAL;
		goto error;
	}

	if (unlikely(!msg->msg_control_is_user)) {
		hsk->error_msg = "msg_control argument for sendmsg isn't in user space";
		result = -EINVAL;
		goto error;
	}
	if (unlikely(copy_from_user(&args, (void __user *)msg->msg_control,
				    sizeof(args)))) {
		hsk->error_msg = "invalid address for msg_control argument to sendmsg";
		result = -EFAULT;
		goto error;
	}
	if (args.flags & ~HOMA_SENDMSG_VALID_FLAGS ||
	    args.reserved != 0) {
		hsk->error_msg = "reserved fields in homa_sendmsg_args must be zero";
		result = -EINVAL;
		goto error;
	}

	if (!homa_sock_wmem_avl(hsk)) {
		result = homa_sock_wait_wmem(hsk,
					     msg->msg_flags & MSG_DONTWAIT);
		if (result != 0)
			goto error;
	}

	if (addr->sa.sa_family != sk->sk_family) {
		hsk->error_msg = "address family in sendmsg address must match the socket";
		result = -EAFNOSUPPORT;
		goto error;
	}
	if (msg->msg_namelen < sizeof(struct sockaddr_in) ||
	    (msg->msg_namelen < sizeof(struct sockaddr_in6) &&
	     addr->in6.sin6_family == AF_INET6)) {
		hsk->error_msg = "msg_namelen too short";
		result = -EINVAL;
		goto error;
	}

	if (!args.id) {
		/* This is a request message. */
		rpc = homa_rpc_alloc_client(hsk, addr);
		if (IS_ERR(rpc)) {
			result = PTR_ERR(rpc);
			rpc = NULL;
			goto error;
		}
		homa_rpc_hold(rpc);
		if (args.flags & HOMA_SENDMSG_PRIVATE)
			set_bit(RPC_PRIVATE, &rpc->flags);
		rpc->completion_cookie = args.completion_cookie;
		result = homa_message_out_fill(rpc, &msg->msg_iter, 1);
		if (result)
			goto error;
		args.id = rpc->id;
		homa_rpc_unlock(rpc); /* Locked by homa_rpc_alloc_client. */

		if (unlikely(copy_to_user((void __user *)msg->msg_control,
					  &args, sizeof(args)))) {
			homa_rpc_lock(rpc);
			hsk->error_msg = "couldn't update homa_sendmsg_args argument to sendmsg: read-only?";
			result = -EFAULT;
			goto error;
		}
		homa_rpc_put(rpc);
	} else {
		/* This is a response message. */
		struct in6_addr canonical_dest;

		if (args.completion_cookie != 0) {
			hsk->error_msg = "completion_cookie must be zero when sending responses";
			result = -EINVAL;
			goto error;
		}
		canonical_dest = canonical_ipv6_addr(addr);

		rpc = homa_rpc_find_server(hsk, &canonical_dest, args.id);
		if (!rpc) {
			/* Return without an error if the RPC doesn't exist;
			 * this could be totally valid (e.g. client is
			 * no longer interested in it).
			 */
			return 0;
		}
		homa_rpc_hold(rpc);
		if (rpc->error) {
			hsk->error_msg = "RPC has failed, so can't send response";
			result = rpc->error;
			goto error;
		}
		if (rpc->state != RPC_IN_SERVICE) {
			hsk->error_msg = "RPC is not in a state where a response can be sent";
			result = -EINVAL;
			goto error_dont_end_rpc;
		}
		rpc->state = RPC_OUTGOING;

		result = homa_message_out_fill(rpc, &msg->msg_iter, 1);
		if (result && rpc->state != RPC_DEAD)
			goto error;
		homa_rpc_put(rpc);
		homa_rpc_unlock(rpc); /* Locked by homa_rpc_find_server. */
	}
	return 0;

error:
	if (rpc)
		homa_rpc_end(rpc);

error_dont_end_rpc:
	if (rpc) {
		homa_rpc_put(rpc);

		/* Locked by homa_rpc_find_server or homa_rpc_alloc_client. */
		homa_rpc_unlock(rpc);
	}
	return result;
}

/**
 * homa_recvmsg() - Receive a message from a Homa socket.
 * @sk:          Socket on which the system call was invoked.
 * @msg:         Controlling information for the receive.
 * @len:         Total bytes of space available in msg->msg_iov; not used.
 * @flags:       Flags from system call; only MSG_DONTWAIT is used.
 * Return:       The length of the message on success, otherwise a negative
 *               errno. Sets hsk->error_msg on errors.
 */
int homa_recvmsg(struct sock *sk, struct msghdr *msg, size_t len, int flags)
{
	struct homa_sock *hsk = homa_sk(sk);
	struct homa_recvmsg_args control;
	struct homa_rpc *rpc = NULL;
	int nonblocking;
	int result;

	if (unlikely(!msg->msg_control)) {
		/* This test isn't strictly necessary, but it provides a
		 * hook for testing kernel call times.
		 */
		hsk->error_msg = "no msg_control passed to recvmsg";
		return -EINVAL;
	}
	if (msg->msg_controllen != sizeof(control)) {
		hsk->error_msg = "invalid msg_controllen in recvmsg";
		return -EINVAL;
	}
	if (unlikely(copy_from_user(&control, (void __user *)msg->msg_control,
				    sizeof(control)))) {
		hsk->error_msg = "invalid address for msg_control argument to recvmsg";
		return -EFAULT;
	}
	control.completion_cookie = 0;

	if (control.num_bpages > HOMA_MAX_BPAGES) {
		hsk->error_msg = "num_pages exceeds HOMA_MAX_BPAGES";
		result = -EINVAL;
		goto done;
	}
	if (control.reserved != 0) {
		hsk->error_msg = "reserved fields in homa_recvmsg_args must be zero";
		result = -EINVAL;
		goto done;
	}
	if (!hsk->buffer_pool) {
		hsk->error_msg = "SO_HOMA_RECVBUF socket option has not been set";
		result = -EINVAL;
		goto done;
	}
	result = homa_pool_free_bufs(hsk->buffer_pool, control.num_bpages,
				     control.bpage_offsets);
	control.num_bpages = 0;
	if (result != 0) {
		hsk->error_msg = "error while releasing buffer pages";
		goto done;
	}

	nonblocking = flags & MSG_DONTWAIT;
	if (control.id != 0) {
		rpc = homa_rpc_find_client(hsk, control.id); /* Locks RPC. */
		if (!rpc) {
			hsk->error_msg = "invalid RPC id passed to recvmsg";
			result = -EINVAL;
			goto done;
		}
		homa_rpc_hold(rpc);
		result = homa_wait_private(rpc, nonblocking);
		if (result != 0) {
			hsk->error_msg = "error while waiting for private RPC to complete";
			control.id = 0;
			goto done;
		}
	} else {
		rpc = homa_wait_shared(hsk, nonblocking);
		if (IS_ERR(rpc)) {
			/* If we get here, it means there was an error that
			 * prevented us from finding an RPC to return. Errors
			 * in the RPC itself are handled below.
			 */
			hsk->error_msg = "error while waiting for shared RPC to complete";
			result = PTR_ERR(rpc);
			rpc = NULL;
			goto done;
		}
	}
	if (rpc->error) {
		hsk->error_msg = "RPC failed";
		result = rpc->error;
	} else {
		result = rpc->msgin.length;
	}

	/* Collect result information. */
	control.id = rpc->id;
	control.completion_cookie = rpc->completion_cookie;
	if (likely(result > 0)) {
		control.num_bpages = rpc->msgin.num_bpages;
		memcpy(control.bpage_offsets, rpc->msgin.bpage_offsets,
		       sizeof(rpc->msgin.bpage_offsets));
	}
	if (msg->msg_name) {
		if (sk->sk_family == AF_INET6) {
			DECLARE_SOCKADDR(struct sockaddr_in6 *, in6,
					 msg->msg_name);

			in6->sin6_family = AF_INET6;
			in6->sin6_port = htons(rpc->dport);
			in6->sin6_addr = rpc->peer->addr;
			msg->msg_namelen = sizeof(*in6);
		} else {
			DECLARE_SOCKADDR(struct sockaddr_in *, in4,
					 msg->msg_name);

			in4->sin_family = AF_INET;
			in4->sin_port = htons(rpc->dport);
			in4->sin_addr.s_addr = ipv6_to_ipv4(rpc->peer->addr);
			msg->msg_namelen = sizeof(*in4);
		}
	}

	if (result < 0) {
		homa_rpc_end(rpc);
	} else {
		/* This indicates that the application now owns the buffers, so
		 * they won't be freed in homa_rpc_end.
		 */
		rpc->msgin.num_bpages = 0;
		if (homa_is_client(rpc->id)) {
			homa_peer_add_ack(rpc);
			homa_rpc_end(rpc);
		} else {
			rpc->state = RPC_IN_SERVICE;
		}
	}

done:
	/* Note: must release the RPC lock before calling homa_rpc_reap
	 * or copying results to user space.
	 */
	if (rpc) {
		homa_rpc_put(rpc);

		/* Locked by homa_rpc_find_client or homa_wait_shared. */
		homa_rpc_unlock(rpc);
	}

	if (test_bit(HOMA_SOCK_NOSPACE, &hsk->flags)) {
		/* There are tasks waiting for tx memory, so reap
		 * immediately.
		 */
		homa_rpc_reap(hsk, true);
	}

	if (unlikely(copy_to_user((__force void __user *)msg->msg_control,
				  &control, sizeof(control)))) {
		hsk->error_msg = "couldn't update homa_recvmsg_args argument to recvmsg: read-only?";
		result = -EFAULT;
	}

	return result;
}

/**
 * homa_hash() - Not needed for Homa.
 * @sk:    Socket for the operation
 * Return: ??
 */
int homa_hash(struct sock *sk)
{
	return 0;
}

/**
 * homa_unhash() - Not needed for Homa.
 * @sk:    Socket for the operation
 */
void homa_unhash(struct sock *sk)
{
}

/**
 * homa_softirq() - This function is invoked at SoftIRQ level to handle
 * incoming packets.
 * @skb:   The incoming packet.
 * Return: Always 0
 */
int homa_softirq(struct sk_buff *skb)
{
	struct sk_buff *packets, *other_pkts, *next;
	struct sk_buff **prev_link, **other_link;
	enum skb_drop_reason reason;
	struct homa_common_hdr *h;
	int header_offset;

	/* skb may actually contain many distinct packets, linked through
	 * skb_shinfo(skb)->frag_list by the Homa GRO mechanism. Make a
	 * pass through the list to process all of the short packets,
	 * leaving the longer packets in the list. Also, perform various
	 * prep/cleanup/error checking functions.
	 */
	skb->next = skb_shinfo(skb)->frag_list;
	skb_shinfo(skb)->frag_list = NULL;
	packets = skb;
	prev_link = &packets;
	for (skb = packets; skb; skb = next) {
		next = skb->next;

		/* Make the header available at skb->data, even if the packet
		 * is fragmented. One complication: it's possible that the IP
		 * header hasn't yet been removed (this happens for GRO packets
		 * on the frag_list, since they aren't handled explicitly by IP.
		 */
		if (!homa_make_header_avl(skb)) {
			reason = SKB_DROP_REASON_HDR_TRUNC;
			goto discard;
		}
		header_offset = skb_transport_header(skb) - skb->data;
		if (header_offset)
			__skb_pull(skb, header_offset);

		/* Reject packets that are too short or have bogus types. */
		h = (struct homa_common_hdr *)skb->data;
		if (unlikely(skb->len < sizeof(struct homa_common_hdr) ||
			     h->type < DATA || h->type > MAX_OP ||
			     skb->len < header_lengths[h->type - DATA])) {
			reason = SKB_DROP_REASON_PKT_TOO_SMALL;
			goto discard;
		}

		/* Process the packet now if it is a control packet or
		 * if it contains an entire short message.
		 */
		if (h->type != DATA || ntohl(((struct homa_data_hdr *)h)
				->message_length) < 1400) {
			*prev_link = skb->next;
			skb->next = NULL;
			homa_dispatch_pkts(skb);
		} else {
			prev_link = &skb->next;
		}
		continue;

discard:
		*prev_link = skb->next;
		kfree_skb_reason(skb, reason);
	}

	/* Now process the longer packets. Each iteration of this loop
	 * collects all of the packets for a particular RPC and dispatches
	 * them (batching the packets for an RPC allows more efficient
	 * generation of grants).
	 */
	while (packets) {
		struct in6_addr saddr, saddr2;
		struct homa_common_hdr *h2;
		struct sk_buff *skb2;

		skb = packets;
		prev_link = &skb->next;
		saddr = skb_canonical_ipv6_saddr(skb);
		other_pkts = NULL;
		other_link = &other_pkts;
		h = (struct homa_common_hdr *)skb->data;
		for (skb2 = skb->next; skb2; skb2 = next) {
			next = skb2->next;
			h2 = (struct homa_common_hdr *)skb2->data;
			if (h2->sender_id == h->sender_id) {
				saddr2 = skb_canonical_ipv6_saddr(skb2);
				if (ipv6_addr_equal(&saddr, &saddr2)) {
					*prev_link = skb2;
					prev_link = &skb2->next;
					continue;
				}
			}
			*other_link = skb2;
			other_link = &skb2->next;
		}
		*prev_link = NULL;
		*other_link = NULL;
		homa_dispatch_pkts(packets);
		packets = other_pkts;
	}

	return 0;
}

/**
 * homa_err_handler_v4() - Invoked by IP to handle an incoming error
 * packet, such as ICMP UNREACHABLE.
 * @skb:    The incoming packet; skb->data points to the byte just after
 *          the ICMP header (the first byte of the embedded packet IP header).
 * @skb:   The incoming packet.
 * @info:  Information about the error that occurred?
 *
 * Return: zero, or a negative errno if the error couldn't be handled here.
 */
int homa_err_handler_v4(struct sk_buff *skb, u32 info)
{
	struct homa *homa = homa_net(dev_net(skb->dev))->homa;
	const struct icmphdr *icmp = icmp_hdr(skb);
	struct in6_addr daddr;
	int type = icmp->type;
	int code = icmp->code;
	struct iphdr *iph;
	int error = 0;
	int port = 0;

	iph = (struct iphdr *)(skb->data);
	ipv6_addr_set_v4mapped(iph->daddr, &daddr);
	if (type == ICMP_DEST_UNREACH && code == ICMP_PORT_UNREACH) {
		struct homa_common_hdr *h = (struct homa_common_hdr *)(skb->data
				+ iph->ihl * 4);

		port = ntohs(h->dport);
		error = -ENOTCONN;
	} else if (type == ICMP_DEST_UNREACH) {
		if (code == ICMP_PROT_UNREACH)
			error = -EPROTONOSUPPORT;
		else
			error = -EHOSTUNREACH;
	} else {
		pr_notice("%s invoked with info %x, ICMP type %d, ICMP code %d\n",
			  __func__, info, type, code);
	}
	if (error != 0)
		homa_abort_rpcs(homa, &daddr, port, error);
	return 0;
}

/**
 * homa_err_handler_v6() - Invoked by IP to handle an incoming error
 * packet, such as ICMP UNREACHABLE.
 * @skb:    The incoming packet; skb->data points to the byte just after
 *          the ICMP header (the first byte of the embedded packet IP header).
 * @opt:    Not used.
 * @type:   Type of ICMP packet.
 * @code:   Additional information about the error.
 * @offset: Total length of IPv6 header, including any extension headers.
 * @info:   Information about the error that occurred?
 *
 * Return: zero, or a negative errno if the error couldn't be handled here.
 */
int homa_err_handler_v6(struct sk_buff *skb, struct inet6_skb_parm *opt,
			u8 type,  u8 code,  int offset,  __be32 info)
{
	const struct ipv6hdr *iph = (const struct ipv6hdr *)skb->data;
	struct homa *homa = homa_net(dev_net(skb->dev))->homa;
	int error = 0;
	int port = 0;

	if (type == ICMPV6_DEST_UNREACH && code == ICMPV6_PORT_UNREACH) {
		const struct homa_common_hdr *h;

		h = (struct homa_common_hdr *)(skb->data + offset);
		port = ntohs(h->dport);
		error = -ENOTCONN;
	} else if (type == ICMPV6_DEST_UNREACH && code == ICMPV6_ADDR_UNREACH) {
		error = -EHOSTUNREACH;
	} else if (type == ICMPV6_PARAMPROB && code == ICMPV6_UNK_NEXTHDR) {
		error = -EPROTONOSUPPORT;
	}
	if (error != 0)
		homa_abort_rpcs(homa, &iph->daddr, port, error);
	return 0;
}

/**
 * homa_poll() - Invoked by Linux as part of implementing select, poll,
 * epoll, etc.
 * @file:  Open file that is participating in a poll, select, etc.
 * @sock:  A Homa socket, associated with @file.
 * @wait:  This table will be registered with the socket, so that it
 *         is notified when the socket's ready state changes.
 *
 * Return: A mask of bits such as EPOLLIN, which indicate the current
 *         state of the socket.
 */
__poll_t homa_poll(struct file *file, struct socket *sock,
		   struct poll_table_struct *wait)
{
	struct homa_sock *hsk = homa_sk(sock->sk);
	__poll_t mask;

	mask = 0;
	sock_poll_wait(file, sock, wait);
	if (homa_sock_wmem_avl(hsk))
		mask |= EPOLLOUT | EPOLLWRNORM;
	else
		set_bit(HOMA_SOCK_NOSPACE, &hsk->flags);

	if (hsk->shutdown)
		mask |= EPOLLIN;

	if (!list_empty(&hsk->ready_rpcs))
		mask |= EPOLLIN | EPOLLRDNORM;
	return mask;
}

/**
 * homa_hrtimer() - This function is invoked by the hrtimer mechanism to
 * wake up the timer thread. Runs at IRQ level.
 * @timer:   The timer that triggered; not used.
 *
 * Return:   Always HRTIMER_NORESTART.
 */
enum hrtimer_restart homa_hrtimer(struct hrtimer *timer)
{
	wake_up_process(timer_kthread);
	return HRTIMER_NORESTART;
}

/**
 * homa_timer_main() - Top-level function for the timer thread.
 * @transport:  Pointer to struct homa.
 *
 * Return:         Always 0.
 */
int homa_timer_main(void *transport)
{
	struct homa *homa = (struct homa *)transport;
	ktime_t tick_interval;
	u64 nsec;

	hrtimer_setup(&hrtimer, homa_hrtimer, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);
	nsec = 1000000;                   /* 1 ms */
	tick_interval = ns_to_ktime(nsec);
	while (1) {
		set_current_state(TASK_UNINTERRUPTIBLE);
		if (!timer_thread_exit) {
			hrtimer_start(&hrtimer, tick_interval,
				      HRTIMER_MODE_REL);
			schedule();
		}
		__set_current_state(TASK_RUNNING);
		if (timer_thread_exit)
			break;
		homa_timer(homa);
	}
	hrtimer_cancel(&hrtimer);
	kthread_complete_and_exit(&timer_thread_done, 0);
	return 0;
}

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("John Ousterhout <ouster@cs.stanford.edu>");
MODULE_DESCRIPTION("Homa transport protocol");
MODULE_VERSION("1.0");

/* Arrange for this module to be loaded automatically when a Homa socket is
 * opened. Apparently symbols don't work in the macros below, so must use
 * numeric values for IPPROTO_HOMA (146) and SOCK_DGRAM(2).
 */
MODULE_ALIAS_NET_PF_PROTO_TYPE(PF_INET, 146, 2);
MODULE_ALIAS_NET_PF_PROTO_TYPE(PF_INET6, 146, 2);
