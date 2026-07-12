// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		"Ping" sockets
 *
 *		IPv4 specific functions
 *
 *		code split from:
 *		net/ipv4/ping.c
 *
 *		See ping.c for author information
 */

#include <linux/bpf-cgroup.h>
#include <linux/in.h>
#include <linux/socket.h>
#include <linux/types.h>
#include <net/ping.h>
#include <net/udp.h>

static int ping_pre_connect(struct sock *sk, struct sockaddr_unsized *uaddr,
			    int addr_len)
{
	/* This check is replicated from __ip4_datagram_connect() and
	 * intended to prevent BPF program called below from accessing bytes
	 * that are out of the bound specified by user in addr_len.
	 */
	if (addr_len < sizeof(struct sockaddr_in))
		return -EINVAL;

	return BPF_CGROUP_RUN_PROG_INET4_CONNECT_LOCK(sk, uaddr, &addr_len);
}

static int ping_v4_push_pending_frames(struct sock *sk, struct pingfakehdr *pfh,
				       struct flowi4 *fl4)
{
	struct sk_buff *skb = skb_peek(&sk->sk_write_queue);

	if (!skb)
		return 0;
	pfh->wcheck = csum_partial((char *)&pfh->icmph,
		sizeof(struct icmphdr), pfh->wcheck);
	pfh->icmph.checksum = csum_fold(pfh->wcheck);
	memcpy(icmp_hdr(skb), &pfh->icmph, sizeof(struct icmphdr));
	skb->ip_summed = CHECKSUM_NONE;
	return ip_push_pending_frames(sk, fl4);
}

static int ping_v4_sendmsg(struct sock *sk, struct msghdr *msg, size_t len)
{
	DEFINE_RAW_FLEX(struct ip_options_rcu, opt_copy, opt.__data,
			IP_OPTIONS_DATA_FIXED_SIZE);
	struct net *net = sock_net(sk);
	struct flowi4 fl4;
	struct inet_sock *inet = inet_sk(sk);
	struct ipcm_cookie ipc;
	struct icmphdr user_icmph;
	struct pingfakehdr pfh;
	struct rtable *rt = NULL;
	int free = 0;
	__be32 saddr, daddr, faddr;
	u8 scope;
	int err;

	pr_debug("ping_v4_sendmsg(sk=%p,sk->num=%u)\n", inet, inet->inet_num);

	err = ping_common_sendmsg(AF_INET, msg, len, &user_icmph,
				  sizeof(user_icmph));
	if (err)
		return err;

	/*
	 *	Get and verify the address.
	 */

	if (msg->msg_name) {
		DECLARE_SOCKADDR(struct sockaddr_in *, usin, msg->msg_name);
		if (msg->msg_namelen < sizeof(*usin))
			return -EINVAL;
		if (usin->sin_family != AF_INET)
			return -EAFNOSUPPORT;
		daddr = usin->sin_addr.s_addr;
		/* no remote port */
	} else {
		if (sk->sk_state != TCP_ESTABLISHED)
			return -EDESTADDRREQ;
		daddr = inet->inet_daddr;
		/* no remote port */
	}

	ipcm_init_sk(&ipc, inet);

	if (msg->msg_controllen) {
		err = ip_cmsg_send(sk, msg, &ipc, false);
		if (unlikely(err)) {
			kfree(ipc.opt);
			return err;
		}
		if (ipc.opt)
			free = 1;
	}
	if (!ipc.opt) {
		struct ip_options_rcu *inet_opt;

		rcu_read_lock();
		inet_opt = rcu_dereference(inet->inet_opt);
		if (inet_opt) {
			memcpy(opt_copy, inet_opt,
			       sizeof(*inet_opt) + inet_opt->opt.optlen);
			ipc.opt = opt_copy;
		}
		rcu_read_unlock();
	}

	saddr = ipc.addr;
	ipc.addr = faddr = daddr;

	if (ipc.opt && ipc.opt->opt.srr) {
		if (!daddr) {
			err = -EINVAL;
			goto out_free;
		}
		faddr = ipc.opt->opt.faddr;
	}
	scope = ip_sendmsg_scope(inet, &ipc, msg);

	if (ipv4_is_multicast(daddr)) {
		if (!ipc.oif || netif_index_is_l3_master(sock_net(sk), ipc.oif))
			ipc.oif = READ_ONCE(inet->mc_index);
		if (!saddr)
			saddr = READ_ONCE(inet->mc_addr);
	} else if (!ipc.oif)
		ipc.oif = READ_ONCE(inet->uc_index);

	flowi4_init_output(&fl4, ipc.oif, ipc.sockc.mark,
			   ipc.tos & INET_DSCP_MASK, scope,
			   sk->sk_protocol, inet_sk_flowi_flags(sk), faddr,
			   saddr, 0, 0, sk_uid(sk));

	fl4.fl4_icmp_type = user_icmph.type;
	fl4.fl4_icmp_code = user_icmph.code;

	security_sk_classify_flow(sk, flowi4_to_flowi_common(&fl4));
	rt = ip_route_output_flow(net, &fl4, sk);
	if (IS_ERR(rt)) {
		err = PTR_ERR(rt);
		rt = NULL;
		if (err == -ENETUNREACH)
			IP_INC_STATS(net, IPSTATS_MIB_OUTNOROUTES);
		goto out;
	}

	err = -EACCES;
	if ((rt->rt_flags & RTCF_BROADCAST) &&
	    !sock_flag(sk, SOCK_BROADCAST))
		goto out;

	if (msg->msg_flags & MSG_CONFIRM)
		goto do_confirm;
back_from_confirm:

	if (!ipc.addr)
		ipc.addr = fl4.daddr;

	lock_sock(sk);

	pfh.icmph.type = user_icmph.type; /* already checked */
	pfh.icmph.code = user_icmph.code; /* ditto */
	pfh.icmph.checksum = 0;
	pfh.icmph.un.echo.id = inet->inet_sport;
	pfh.icmph.un.echo.sequence = user_icmph.un.echo.sequence;
	pfh.msg = msg;
	pfh.wcheck = 0;
	pfh.family = AF_INET;

	err = ip_append_data(sk, &fl4, ping_getfrag, &pfh, len,
			     sizeof(struct icmphdr), &ipc, &rt,
			     msg->msg_flags);
	if (err)
		ip_flush_pending_frames(sk);
	else
		err = ping_v4_push_pending_frames(sk, &pfh, &fl4);
	release_sock(sk);

out:
	ip_rt_put(rt);
out_free:
	if (free)
		kfree(ipc.opt);
	if (!err)
		return len;
	return err;

do_confirm:
	if (msg->msg_flags & MSG_PROBE)
		dst_confirm_neigh(&rt->dst, &fl4.daddr);
	if (!(msg->msg_flags & MSG_PROBE) || len)
		goto back_from_confirm;
	err = 0;
	goto out;
}

struct proto ping_prot = {
	.name =		"PING",
	.owner =	THIS_MODULE,
	.init =		ping_init_sock,
	.close =	ping_close,
	.pre_connect =	ping_pre_connect,
	.connect =	ip4_datagram_connect,
	.disconnect =	__udp_disconnect,
	.setsockopt =	ip_setsockopt,
	.getsockopt =	ip_getsockopt,
	.sendmsg =	ping_v4_sendmsg,
	.recvmsg =	ping_recvmsg,
	.bind =		ping_bind,
	.backlog_rcv =	ping_queue_rcv_skb,
	.release_cb =	ip4_datagram_release_cb,
	.unhash =	ping_unhash,
	.get_port =	ping_get_port,
	.put_port =	ping_unhash,
	.obj_size =	sizeof(struct inet_sock),
};
