// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generic netlink handshake service
 *
 * Author: Chuck Lever <chuck.lever@oracle.com>
 *
 * Copyright (c) 2023, Oracle and/or its affiliates.
 */

#include <linux/types.h>
#include <linux/socket.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/mm.h>

#include <net/sock.h>
#include <net/genetlink.h>
#include <net/handshake.h>
#include <net/netns/generic.h>

#include <kunit/visibility.h>

#include <uapi/linux/handshake.h>
#include "handshake.h"
#include "genl.h"

#include <trace/events/handshake.h>

/**
 * handshake_genl_notify - Notify handlers that a request is waiting
 * @net: target network namespace
 * @proto: handshake protocol
 * @flags: memory allocation control flags
 *
 * Returns zero on success or a negative errno if notification failed.
 */
int handshake_genl_notify(struct net *net, const struct handshake_proto *proto,
			  gfp_t flags)
{
	struct sk_buff *msg;
	void *hdr;

	/* Disable notifications during unit testing */
	if (!test_bit(HANDSHAKE_F_PROTO_NOTIFY, &proto->hp_flags))
		return 0;

	if (!genl_has_listeners(&handshake_nl_family, net,
				proto->hp_handler_class))
		return -ESRCH;

	msg = genlmsg_new(GENLMSG_DEFAULT_SIZE, flags);
	if (!msg)
		return -ENOMEM;

	hdr = genlmsg_put(msg, 0, 0, &handshake_nl_family, 0,
			  HANDSHAKE_CMD_READY);
	if (!hdr)
		goto out_free;

	if (nla_put_u32(msg, HANDSHAKE_A_ACCEPT_HANDLER_CLASS,
			proto->hp_handler_class) < 0) {
		genlmsg_cancel(msg, hdr);
		goto out_free;
	}

	genlmsg_end(msg, hdr);
	return genlmsg_multicast_netns(&handshake_nl_family, net, msg,
				       0, proto->hp_handler_class, flags);

out_free:
	nlmsg_free(msg);
	return -EMSGSIZE;
}

/**
 * handshake_genl_put - Create a generic netlink message header
 * @msg: buffer in which to create the header
 * @info: generic netlink message context
 *
 * Returns a ready-to-use header, or NULL.
 */
struct nlmsghdr *handshake_genl_put(struct sk_buff *msg,
				    struct genl_info *info)
{
	return genlmsg_put(msg, info->snd_portid, info->snd_seq,
			   &handshake_nl_family, 0, info->genlhdr->cmd);
}
EXPORT_SYMBOL(handshake_genl_put);

int handshake_nl_accept_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct net *net = sock_net(skb->sk);
	struct handshake_net *hn = handshake_pernet(net);
	struct handshake_req *req = NULL;
	struct socket *sock;
	int class, err;

	err = -EOPNOTSUPP;
	if (!hn)
		goto out_status;

	err = -EINVAL;
	if (GENL_REQ_ATTR_CHECK(info, HANDSHAKE_A_ACCEPT_HANDLER_CLASS))
		goto out_status;
	class = nla_get_u32(info->attrs[HANDSHAKE_A_ACCEPT_HANDLER_CLASS]);

	err = -EAGAIN;
	req = handshake_req_next(hn, class);
	if (req) {
		sock = req->hr_sk->sk_socket;

		FD_PREPARE(fdf, O_CLOEXEC, sock->file);
		if (fdf.err) {
			err = fdf.err;
			goto out_complete;
		}

		get_file(sock->file); /* FD_PREPARE() consumes a reference. */
		err = req->hr_proto->hp_accept(req, info, fd_prepare_fd(fdf));
		if (err)
			goto out_complete; /* Automatic cleanup handles fput */

		trace_handshake_cmd_accept(net, req, req->hr_sk, fd_prepare_fd(fdf));
		fd_publish(fdf);
		return 0;
	}

out_complete:
	if (req)
		handshake_complete(req, -EIO, NULL);
out_status:
	trace_handshake_cmd_accept_err(net, req, NULL, err);
	return err;
}

/*
 * Pick up session tags from the DONE downcall payload into a
 * caller-owned tagset. No handshake_req fields are mutated here:
 * concurrent DONE handlers each populate a private tagset, and
 * the winner of the completion gate publishes its set into
 * req->hr_tags by struct assignment.
 *
 * Return: 0 if tags were processed (some may have been dropped on
 * per-tag or bulk allocation pressure, or truncated at
 * HANDSHAKE_MAX_SESSIONTAGS); a negative errno if the payload was
 * rejected and no tags collected.
 */
static int handshake_get_sessiontags(struct tagset *tags,
				     struct genl_info *info)
{
	unsigned int count = 0;
	struct nlattr *nla;
	int rem;

	/*
	 * Reject embedded NUL bytes only. NLA_STRING payloads may
	 * arrive with or without a trailing NUL, and nla_strdup()
	 * appends the terminator when copying into the tagset.
	 * NLA_NUL_STRING would accept a NUL at any offset, and the
	 * YAML schema cannot express "no NUL except as terminator,"
	 * so the check belongs here.
	 */
	nlmsg_for_each_attr_type(nla, HANDSHAKE_A_DONE_TAG, info->nlhdr,
				 GENL_HDRLEN, rem) {
		const char *src = nla_data(nla);
		size_t srclen = nla_len(nla);

		if (srclen > 0 && src[srclen - 1] == '\0')
			srclen--;
		if (srclen == 0 || memchr(src, '\0', srclen))
			return -EINVAL;
		count++;
	}
	if (count == 0)
		return 0;
	if (count > HANDSHAKE_MAX_SESSIONTAGS) {
		pr_warn_once("handshake: too many session tags (%u > %u)\n",
			     count, HANDSHAKE_MAX_SESSIONTAGS);
		count = HANDSHAKE_MAX_SESSIONTAGS;
	}
	if (!tagset_alloc(tags, count, GFP_KERNEL)) {
		pr_warn_once("handshake: dropping session tags under memory pressure\n");
		return 0;
	}

	nlmsg_for_each_attr_type(nla, HANDSHAKE_A_DONE_TAG, info->nlhdr,
				 GENL_HDRLEN, rem) {
		char *tag;

		/*
		 * The first pass may have clamped count to
		 * HANDSHAKE_MAX_SESSIONTAGS. Stop here to avoid
		 * alloc/free churn on excess attributes.
		 */
		if (tagset_count(tags) >= count)
			break;

		tag = nla_strdup(nla, GFP_KERNEL);
		if (!tag)
			continue;
		if (!tagset_add(tags, tag))
			kfree(tag);
	}
	return 0;
}

int handshake_nl_done_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct net *net = sock_net(skb->sk);
	struct handshake_req *req;
	struct socket *sock;
	DEFINE_TAGSET(tags);
	int fd, status, err;

	if (GENL_REQ_ATTR_CHECK(info, HANDSHAKE_A_DONE_SOCKFD))
		return -EINVAL;
	fd = nla_get_s32(info->attrs[HANDSHAKE_A_DONE_SOCKFD]);

	sock = sockfd_lookup(fd, &err);
	if (!sock)
		return err;

	req = handshake_req_hash_lookup(sock->sk);
	if (!req) {
		err = -EBUSY;
		trace_handshake_cmd_done_err(net, req, sock->sk, err);
		sockfd_put(sock);
		return err;
	}

	trace_handshake_cmd_done(net, req, sock->sk, fd);

	status = -EIO;
	if (info->attrs[HANDSHAKE_A_DONE_STATUS])
		status = nla_get_u32(info->attrs[HANDSHAKE_A_DONE_STATUS]);
	err = 0;
	if (!status) {
		int ret = handshake_get_sessiontags(&tags, info);

		if (ret < 0) {
			err = ret;
			trace_handshake_cmd_done_err(net, req, sock->sk, err);
			status = -EIO;
		}
	}

	/*
	 * Take the unique-completer gate after collection so the gate
	 * region contains no GFP_KERNEL allocations. handshake_req_cancel()
	 * observers must not see the gate as taken while sleeping work
	 * remains here, or they will free callback data while the consumer
	 * callback is still pending.
	 */
	if (!handshake_try_complete(req)) {
		trace_handshake_cmd_done_err(net, req, sock->sk, -EBUSY);
		tagset_destroy(&tags);
		sockfd_put(sock);
		return -EBUSY;
	}

	/*
	 * Publish the locally-collected tagset. req->hr_tags was
	 * initialized empty by handshake_req_alloc() and no other writer
	 * can reach it past the gate, so a struct assignment cleanly
	 * transfers ownership of the heap-allocated tag array.
	 */
	req->hr_tags = tags;

	handshake_finish_complete(req, status, info);
	sockfd_put(sock);
	return err;
}

static unsigned int handshake_net_id;

static int __net_init handshake_net_init(struct net *net)
{
	struct handshake_net *hn = net_generic(net, handshake_net_id);
	unsigned long tmp;
	struct sysinfo si;

	/*
	 * Arbitrary limit to prevent handshakes that do not make
	 * progress from clogging up the system. The cap scales up
	 * with the amount of physical memory on the system.
	 */
	si_meminfo(&si);
	tmp = si.totalram / (25 * si.mem_unit);
	hn->hn_pending_max = clamp(tmp, 3UL, 50UL);

	spin_lock_init(&hn->hn_lock);
	hn->hn_pending = 0;
	hn->hn_flags = 0;
	INIT_LIST_HEAD(&hn->hn_requests);
	return 0;
}

static void __net_exit handshake_net_exit(struct net *net)
{
	struct handshake_net *hn = net_generic(net, handshake_net_id);
	struct handshake_req *req;
	LIST_HEAD(requests);

	/*
	 * Drain the net's pending list. Requests that have been
	 * accepted and are in progress will be destroyed when
	 * the socket is closed.
	 */
	spin_lock(&hn->hn_lock);
	set_bit(HANDSHAKE_F_NET_DRAINING, &hn->hn_flags);
	list_splice_init(&requests, &hn->hn_requests);
	spin_unlock(&hn->hn_lock);

	while (!list_empty(&requests)) {
		req = list_first_entry(&requests, struct handshake_req, hr_list);
		list_del(&req->hr_list);

		/*
		 * Requests on this list have not yet been
		 * accepted, so they do not have an fd to put.
		 */

		handshake_complete(req, -ETIMEDOUT, NULL);
	}
}

static struct pernet_operations handshake_genl_net_ops = {
	.init		= handshake_net_init,
	.exit		= handshake_net_exit,
	.id		= &handshake_net_id,
	.size		= sizeof(struct handshake_net),
};

/**
 * handshake_pernet - Get the handshake private per-net structure
 * @net: network namespace
 *
 * Returns a pointer to the net's private per-net structure for the
 * handshake module, or NULL if handshake_init() failed.
 */
struct handshake_net *handshake_pernet(struct net *net)
{
	return handshake_net_id ?
		net_generic(net, handshake_net_id) : NULL;
}
EXPORT_SYMBOL_IF_KUNIT(handshake_pernet);

static int __init handshake_init(void)
{
	int ret;

	ret = handshake_req_hash_init();
	if (ret) {
		pr_warn("handshake: hash initialization failed (%d)\n", ret);
		return ret;
	}

	ret = genl_register_family(&handshake_nl_family);
	if (ret) {
		pr_warn("handshake: netlink registration failed (%d)\n", ret);
		handshake_req_hash_destroy();
		return ret;
	}

	/*
	 * ORDER: register_pernet_subsys must be done last.
	 *
	 *	If initialization does not make it past pernet_subsys
	 *	registration, then handshake_net_id will remain 0. That
	 *	shunts the handshake consumer API to return ENOTSUPP
	 *	to prevent it from dereferencing something that hasn't
	 *	been allocated.
	 */
	ret = register_pernet_subsys(&handshake_genl_net_ops);
	if (ret) {
		pr_warn("handshake: pernet registration failed (%d)\n", ret);
		genl_unregister_family(&handshake_nl_family);
		handshake_req_hash_destroy();
	}

	return ret;
}

static void __exit handshake_exit(void)
{
	unregister_pernet_subsys(&handshake_genl_net_ops);
	handshake_net_id = 0;

	handshake_req_hash_destroy();
	genl_unregister_family(&handshake_nl_family);
}

module_init(handshake_init);
module_exit(handshake_exit);
