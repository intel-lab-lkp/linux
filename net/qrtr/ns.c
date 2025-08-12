// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Copyright (c) 2015, Sony Mobile Communications Inc.
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 * Copyright (c) 2020, Linaro Ltd.
 */

#define pr_fmt(fmt) "QRTR NS: "fmt
#include <linux/module.h>
#include <linux/qrtr.h>
#include <linux/workqueue.h>
#include <net/sock.h>

#include "qrtr.h"

#include <trace/events/sock.h>
#define CREATE_TRACE_POINTS
#include <trace/events/qrtr.h>

static DEFINE_XARRAY(endpoints);

static struct {
	struct socket *sock;
	struct sockaddr_qrtr bcast_sq;
	struct list_head lookups;
	struct workqueue_struct *workqueue;
	struct work_struct work;
	int local_node;
} qrtr_ns;

static const char * const qrtr_ctrl_pkt_strings[] = {
	[QRTR_TYPE_HELLO]	= "hello",
	[QRTR_TYPE_BYE]		= "bye",
	[QRTR_TYPE_NEW_SERVER]	= "new-server",
	[QRTR_TYPE_DEL_SERVER]	= "del-server",
	[QRTR_TYPE_DEL_CLIENT]	= "del-client",
	[QRTR_TYPE_RESUME_TX]	= "resume-tx",
	[QRTR_TYPE_EXIT]	= "exit",
	[QRTR_TYPE_PING]	= "ping",
	[QRTR_TYPE_NEW_LOOKUP]	= "new-lookup",
	[QRTR_TYPE_DEL_LOOKUP]	= "del-lookup",
};

struct qrtr_server_filter {
	unsigned int service;
	unsigned int instance;
	unsigned int ifilter;
};

struct qrtr_lookup {
	unsigned int service;
	unsigned int instance;

	struct sockaddr_qrtr sq;
	struct list_head li;
};

struct qrtr_server {
	unsigned int service;
	unsigned int instance;

	unsigned int node;
	unsigned int port;
	u32 endpoint_id;

	struct list_head qli;
};

struct qrtr_node {
	unsigned int id;
	struct xarray servers;
};

struct qrtr_ns_endpoint {
	unsigned int id;
	struct xarray nodes;
};

static struct qrtr_node *node_lookup(u32 endpoint_id, unsigned int node_id)
{
	struct qrtr_ns_endpoint *endpoint;

	endpoint = xa_load(&endpoints, endpoint_id);
	if (!endpoint)
		return NULL;

	return xa_load(&endpoint->nodes, node_id);
}

static struct qrtr_node *node_new(u32 endpoint_id, unsigned int node_id)
{
	struct qrtr_ns_endpoint *endpoint;
	struct qrtr_node *node;
	bool new_endpoint = false;

	endpoint = xa_load(&endpoints, endpoint_id);
	if (!endpoint) {
		endpoint = kzalloc(sizeof(*endpoint), GFP_KERNEL);
		if (!endpoint)
			return NULL;

		endpoint->id = endpoint_id;
		xa_init(&endpoint->nodes);

		if (xa_store(&endpoints, endpoint_id, endpoint, GFP_KERNEL)) {
			kfree(endpoint);
			return NULL;
		}

		new_endpoint = true;
	} else {
		node = xa_load(&endpoint->nodes, node_id);
		if (node)
			return node;
	}

	/* If node didn't exist, allocate and insert it to the tree */
	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		goto error;

	node->id = node_id;
	xa_init(&node->servers);

	if (xa_store(&endpoint->nodes, node_id, node, GFP_KERNEL)) {
		kfree(node);
		goto error;
	}

	return node;
error:
	if (new_endpoint) {
		xa_erase(&endpoints, endpoint_id);
		kfree(endpoint);
	}

	return NULL;
}

static int server_match(const struct qrtr_server *srv,
			const struct qrtr_server_filter *f)
{
	unsigned int ifilter = f->ifilter;

	if (f->service != 0 && srv->service != f->service)
		return 0;
	if (!ifilter && f->instance)
		ifilter = ~0;

	return (srv->instance & ifilter) == f->instance;
}

static int qrtr_ns_sendmsg(u32 endpoint_id, struct sockaddr_qrtr *dest,
			   struct qrtr_ctrl_pkt *pkt)
{
	struct msghdr msg = { };
	struct kvec iv;
	u8 control[CMSG_SPACE(sizeof(endpoint_id))];
	struct cmsghdr *cmsg;

	iv.iov_base = pkt;
	iv.iov_len = sizeof(*pkt);

	msg.msg_name = (struct sockaddr *)dest;
	msg.msg_namelen = sizeof(*dest);

	if (endpoint_id) {
		msg.msg_control = control;
		msg.msg_controllen = sizeof(control);

		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_QRTR;
		cmsg->cmsg_type = QRTR_ENDPOINT;
		cmsg->cmsg_len = CMSG_LEN(sizeof(endpoint_id));
		memcpy(CMSG_DATA(cmsg), &endpoint_id, sizeof(endpoint_id));
	}

	return kernel_sendmsg(qrtr_ns.sock, &msg, &iv, 1, sizeof(*pkt));
}

static int service_announce_new(u32 endpoint_id, struct sockaddr_qrtr *dest,
				struct qrtr_server *srv)
{
	struct qrtr_ctrl_pkt pkt;

	trace_qrtr_ns_service_announce_new(srv->service, srv->instance,
					   srv->node, srv->port);

	memset(&pkt, 0, sizeof(pkt));
	pkt.cmd = cpu_to_le32(QRTR_TYPE_NEW_SERVER);
	pkt.server.service = cpu_to_le32(srv->service);
	pkt.server.instance = cpu_to_le32(srv->instance);
	pkt.server.node = cpu_to_le32(srv->node);
	pkt.server.port = cpu_to_le32(srv->port);

	return qrtr_ns_sendmsg(endpoint_id, dest, &pkt);
}

static void service_announce_del(u32 endpoint_id, struct sockaddr_qrtr *dest,
				 struct qrtr_server *srv)
{
	struct qrtr_ctrl_pkt pkt;
	int ret;

	trace_qrtr_ns_service_announce_del(srv->service, srv->instance,
					   srv->node, srv->port);

	memset(&pkt, 0, sizeof(pkt));
	pkt.cmd = cpu_to_le32(QRTR_TYPE_DEL_SERVER);
	pkt.server.service = cpu_to_le32(srv->service);
	pkt.server.instance = cpu_to_le32(srv->instance);
	pkt.server.node = cpu_to_le32(srv->node);
	pkt.server.port = cpu_to_le32(srv->port);

	ret = qrtr_ns_sendmsg(endpoint_id, dest, &pkt);
	if (ret < 0 && ret != -ENODEV)
		pr_err("failed to announce del service\n");
}

static void lookup_notify(u32 endpoint_id, struct sockaddr_qrtr *to,
			  struct qrtr_server *srv, bool new)
{
	struct qrtr_ctrl_pkt pkt;
	int ret;

	/*
	 * Notify a local client at @to about a server change.  The aux data
	 * will look as if it came from the endpoint that reported the event
	 * (remote or local)
	 */
	memset(&pkt, 0, sizeof(pkt));
	pkt.cmd = new ? cpu_to_le32(QRTR_TYPE_NEW_SERVER) :
			cpu_to_le32(QRTR_TYPE_DEL_SERVER);
	if (srv) {
		pkt.server.service = cpu_to_le32(srv->service);
		pkt.server.instance = cpu_to_le32(srv->instance);
		pkt.server.node = cpu_to_le32(srv->node);
		pkt.server.port = cpu_to_le32(srv->port);
	}

	ret = qrtr_ns_sendmsg(endpoint_id, to, &pkt);
	if (ret < 0 && ret != -ENODEV)
		pr_err("failed to send lookup notification\n");
}

static int announce_servers(u32 endpoint_id, struct sockaddr_qrtr *sq)
{
	struct qrtr_server *srv;
	struct qrtr_node *node;
	unsigned long index;
	int ret;

	node = node_lookup(qrtr_ns.local_node, qrtr_ns.local_node);
	if (!node)
		return 0;

	/* Announce servers registered on local endpoint to remote endpoint */
	xa_for_each(&node->servers, index, srv) {
		ret = service_announce_new(endpoint_id, sq, srv);
		if (ret < 0) {
			if (ret == -ENODEV)
				continue;

			pr_err("failed to announce new service\n");
			return ret;
		}
	}
	return 0;
}

static struct qrtr_server *server_add(u32 endpoint_id,
				      unsigned int service,
				      unsigned int instance,
				      unsigned int node_id,
				      unsigned int port)
{
	struct qrtr_server *srv;
	struct qrtr_server *old;
	struct qrtr_node *node;

	if (!service || !port)
		return NULL;

	srv = kzalloc(sizeof(*srv), GFP_KERNEL);
	if (!srv)
		return NULL;

	srv->service = service;
	srv->instance = instance;
	srv->node = node_id;
	srv->port = port;

	node = node_new(endpoint_id, node_id);
	if (!node)
		goto err;

	/* Delete the old server on the same port */
	old = xa_store(&node->servers, port, srv, GFP_KERNEL);
	if (old) {
		if (xa_is_err(old)) {
			pr_err("failed to add server [0x%x:0x%x] ret:%d\n",
			       srv->service, srv->instance, xa_err(old));
			goto err;
		} else {
			kfree(old);
		}
	}

	trace_qrtr_ns_server_add(srv->service, srv->instance,
				 srv->node, srv->port);

	return srv;

err:
	kfree(srv);
	return NULL;
}

static int server_del(u32 endpoint_id, struct qrtr_node *node,
		      unsigned int port, bool bcast)
{
	struct qrtr_lookup *lookup;
	struct qrtr_server *srv;
	struct list_head *li;

	srv = xa_load(&node->servers, port);
	if (!srv)
		return -ENOENT;

	xa_erase(&node->servers, port);

	/* Broadcast the removal of local servers to remote endpoints */
	if (srv->node == qrtr_ns.local_node && bcast)
		service_announce_del(qrtr_ns.local_node,
				     &qrtr_ns.bcast_sq, srv);

	/* Announce the service's disappearance to observers */
	list_for_each(li, &qrtr_ns.lookups) {
		lookup = container_of(li, struct qrtr_lookup, li);
		if (lookup->service && lookup->service != srv->service)
			continue;
		if (lookup->instance && lookup->instance != srv->instance)
			continue;

		lookup_notify(endpoint_id, &lookup->sq, srv, false);
	}

	kfree(srv);

	return 0;
}

static int say_hello(u32 endpoint_id, struct sockaddr_qrtr *dest)
{
	struct qrtr_ctrl_pkt pkt;
	int ret;

	memset(&pkt, 0, sizeof(pkt));
	pkt.cmd = cpu_to_le32(QRTR_TYPE_HELLO);

	ret = qrtr_ns_sendmsg(endpoint_id, dest, &pkt);
	if (ret < 0)
		pr_err("failed to send hello msg\n");

	return ret;
}

/* Announce the list of servers registered on the local node */
static int ctrl_cmd_hello(u32 endpoint_id, struct sockaddr_qrtr *sq)
{
	int ret;

	/* Send Hello and New Server messages to remote endpoint */
	ret = say_hello(endpoint_id, sq);
	if (ret < 0)
		return ret;

	return announce_servers(endpoint_id, sq);
}

static int ctrl_cmd_bye(u32 endpoint_id, struct sockaddr_qrtr *from)
{
	struct qrtr_node *local_node;
	struct qrtr_ctrl_pkt pkt;
	struct qrtr_server *srv;
	struct sockaddr_qrtr sq;
	struct qrtr_node *node;
	unsigned long index;
	int ret;

	node = node_lookup(endpoint_id, from->sq_node);
	if (!node)
		return 0;

	/* Advertise removal of this client to all servers of remote node */
	xa_for_each(&node->servers, index, srv)
		server_del(endpoint_id, node, srv->port, true);

	/* Advertise the removal of this client to all local servers */
	local_node = node_lookup(qrtr_ns.local_node, qrtr_ns.local_node);
	if (!local_node)
		return 0;

	memset(&pkt, 0, sizeof(pkt));
	pkt.cmd = cpu_to_le32(QRTR_TYPE_BYE);
	pkt.client.node = cpu_to_le32(from->sq_node);

	xa_for_each(&local_node->servers, index, srv) {
		sq.sq_family = AF_QIPCRTR;
		sq.sq_node = srv->node;
		sq.sq_port = srv->port;

		/* Bye will look as if it came from endpoint_id */
		ret = qrtr_ns_sendmsg(endpoint_id, &sq, &pkt);
		if (ret < 0 && ret != -ENODEV) {
			pr_err("failed to send bye cmd\n");
			return ret;
		}
	}
	return 0;
}

static int ctrl_cmd_del_client(u32 endpoint_id, struct sockaddr_qrtr *from,
			       unsigned int node_id, unsigned int port)
{
	struct qrtr_node *local_node;
	struct qrtr_lookup *lookup;
	struct qrtr_ctrl_pkt pkt;
	struct qrtr_server *srv;
	struct sockaddr_qrtr sq;
	struct qrtr_node *node;
	struct list_head *tmp;
	struct list_head *li;
	unsigned long index;
	int ret;

	/* Don't accept spoofed messages */
	if (from->sq_node != node_id)
		return -EINVAL;

	/* Local DEL_CLIENT messages comes from the port being closed */
	if (from->sq_node == qrtr_ns.local_node && from->sq_port != port)
		return -EINVAL;

	/* Remove any lookups by this client */
	list_for_each_safe(li, tmp, &qrtr_ns.lookups) {
		lookup = container_of(li, struct qrtr_lookup, li);
		if (lookup->sq.sq_node != node_id)
			continue;
		if (lookup->sq.sq_port != port)
			continue;

		list_del(&lookup->li);
		kfree(lookup);
	}

	/* Remove the server belonging to this port but don't broadcast
	 * DEL_SERVER. Neighbours would've already removed the server belonging
	 * to this port due to the DEL_CLIENT broadcast from qrtr_port_remove().
	 */
	node = node_lookup(endpoint_id, node_id);
	if (node)
		server_del(endpoint_id, node, port, false);

	/* Advertise the removal of this client to all local servers */
	local_node = node_lookup(qrtr_ns.local_node, qrtr_ns.local_node);
	if (!local_node)
		return 0;

	memset(&pkt, 0, sizeof(pkt));
	pkt.cmd = cpu_to_le32(QRTR_TYPE_DEL_CLIENT);
	pkt.client.node = cpu_to_le32(node_id);
	pkt.client.port = cpu_to_le32(port);

	xa_for_each(&local_node->servers, index, srv) {
		sq.sq_family = AF_QIPCRTR;
		sq.sq_node = srv->node;
		sq.sq_port = srv->port;

		/* Del Client will look as if it came from endpoint_id */
		ret = qrtr_ns_sendmsg(endpoint_id, &sq, &pkt);
		if (ret < 0 && ret != -ENODEV) {
			pr_err("failed to send del client cmd\n");
			return ret;
		}
	}
	return 0;
}

static int ctrl_cmd_new_server(u32 endpoint_id, struct sockaddr_qrtr *from,
			       unsigned int service, unsigned int instance,
			       unsigned int node_id, unsigned int port)
{
	struct qrtr_lookup *lookup;
	struct qrtr_server *srv;
	struct list_head *li;
	int ret = 0;

	/* Ignore specified node and port for local servers */
	if (from->sq_node == qrtr_ns.local_node) {
		node_id = from->sq_node;
		port = from->sq_port;
	}

	srv = server_add(endpoint_id, service, instance, node_id, port);
	if (!srv) {
		pr_err("Failed to add server\n");
		return -EINVAL;
	}

	if (srv->node == qrtr_ns.local_node) {
		/* Broadcast local server info to all peer endpoints */
		ret = service_announce_new(qrtr_ns.local_node,
					   &qrtr_ns.bcast_sq, srv);
		if (ret < 0) {
			pr_err("failed to announce new service\n");
			return ret;
		}
	}

	/* Notify any potential lookups about the new server */
	list_for_each(li, &qrtr_ns.lookups) {
		lookup = container_of(li, struct qrtr_lookup, li);
		if (lookup->service && lookup->service != service)
			continue;
		if (lookup->instance && lookup->instance != instance)
			continue;

		lookup_notify(endpoint_id, &lookup->sq, srv, true);
	}

	return ret;
}

static int ctrl_cmd_del_server(u32 endpoint_id, struct sockaddr_qrtr *from,
			       unsigned int service, unsigned int instance,
			       unsigned int node_id, unsigned int port)
{
	struct qrtr_node *node;

	/* Ignore specified node and port for local servers*/
	if (from->sq_node == qrtr_ns.local_node) {
		node_id = from->sq_node;
		port = from->sq_port;
	}

	/* Local servers may only unregister themselves */
	if (from->sq_node == qrtr_ns.local_node && from->sq_port != port)
		return -EINVAL;

	node = node_lookup(endpoint_id, node_id);
	if (!node)
		return -ENOENT;

	server_del(endpoint_id, node, port, true);

	return 0;
}

static int ctrl_cmd_new_lookup(u32 endpoint_id, struct sockaddr_qrtr *from,
			       unsigned int service, unsigned int instance)
{
	struct qrtr_server_filter filter;
	struct qrtr_lookup *lookup;
	unsigned long id;
	struct qrtr_ns_endpoint *endpoint;

	/* Accept only local observers */
	if (from->sq_node != qrtr_ns.local_node)
		return -EINVAL;

	lookup = kzalloc(sizeof(*lookup), GFP_KERNEL);
	if (!lookup)
		return -ENOMEM;

	lookup->sq = *from;
	lookup->service = service;
	lookup->instance = instance;
	list_add_tail(&lookup->li, &qrtr_ns.lookups);

	memset(&filter, 0, sizeof(filter));
	filter.service = service;
	filter.instance = instance;

	xa_for_each(&endpoints, id, endpoint) {
		struct qrtr_node *node;
		unsigned long node_idx;

		xa_for_each(&endpoint->nodes, node_idx, node) {
			unsigned long srv_idx;
			struct qrtr_server *srv;

			xa_for_each(&node->servers, srv_idx, srv) {
				if (!server_match(srv, &filter))
					continue;

				lookup_notify(id, from, srv, true);
			}
		}
	}

	/* Empty notification, to indicate end of listing */
	lookup_notify(0, from, NULL, true);

	return 0;
}

static void ctrl_cmd_del_lookup(u32 endpoint_id, struct sockaddr_qrtr *from,
				unsigned int service, unsigned int instance)
{
	struct qrtr_lookup *lookup;
	struct list_head *tmp;
	struct list_head *li;

	list_for_each_safe(li, tmp, &qrtr_ns.lookups) {
		lookup = container_of(li, struct qrtr_lookup, li);
		if (lookup->sq.sq_node != from->sq_node)
			continue;
		if (lookup->sq.sq_port != from->sq_port)
			continue;
		if (lookup->service != service)
			continue;
		if (lookup->instance && lookup->instance != instance)
			continue;

		list_del(&lookup->li);
		kfree(lookup);
	}
}

static void qrtr_ns_worker(struct work_struct *work)
{
	const struct qrtr_ctrl_pkt *pkt;
	size_t recv_buf_size = 4096;
	struct sockaddr_qrtr sq;
	struct msghdr msg = { };
	unsigned int cmd;
	ssize_t msglen;
	void *recv_buf;
	struct kvec iv;
	u8 control[32];
	int ret;

	msg.msg_name = (struct sockaddr *)&sq;
	msg.msg_namelen = sizeof(sq);

	recv_buf = kzalloc(recv_buf_size, GFP_KERNEL);
	if (!recv_buf)
		return;

	for (;;) {
		u32 endpoint_id;

		iv.iov_base = recv_buf;
		iv.iov_len = recv_buf_size;
		msg.msg_control = control;
		msg.msg_controllen = sizeof(control);

		msglen = kernel_recvmsg(qrtr_ns.sock, &msg, &iv, 1,
					iv.iov_len, MSG_DONTWAIT);

		if (msglen == -EAGAIN)
			break;

		if (msglen < 0) {
			pr_err("error receiving packet: %zd\n", msglen);
			break;
		}

		/* AUX data is written direct into the control buffer */
		msg.msg_control = control;
		msg.msg_controllen = sizeof(control) - msg.msg_controllen;

		ret = qrtr_msg_get_endpoint(&msg, &endpoint_id);
		if (ret < 0) {
			pr_err("error receiving endpoint id: %d\n", ret);
			break;
		}

		if ((size_t)msglen < sizeof(*pkt))
			break;

		pkt = recv_buf;
		cmd = le32_to_cpu(pkt->cmd);
		if (cmd < ARRAY_SIZE(qrtr_ctrl_pkt_strings) &&
		    qrtr_ctrl_pkt_strings[cmd])
			trace_qrtr_ns_message(qrtr_ctrl_pkt_strings[cmd],
					      sq.sq_node, sq.sq_port);

		ret = 0;
		switch (cmd) {
		case QRTR_TYPE_HELLO:
			ret = ctrl_cmd_hello(endpoint_id, &sq);
			break;
		case QRTR_TYPE_BYE:
			ret = ctrl_cmd_bye(endpoint_id, &sq);
			break;
		case QRTR_TYPE_DEL_CLIENT:
			ret = ctrl_cmd_del_client(endpoint_id, &sq,
					le32_to_cpu(pkt->client.node),
					le32_to_cpu(pkt->client.port));
			break;
		case QRTR_TYPE_NEW_SERVER:
			ret = ctrl_cmd_new_server(endpoint_id, &sq,
					le32_to_cpu(pkt->server.service),
					le32_to_cpu(pkt->server.instance),
					le32_to_cpu(pkt->server.node),
					le32_to_cpu(pkt->server.port));
			break;
		case QRTR_TYPE_DEL_SERVER:
			ret = ctrl_cmd_del_server(endpoint_id, &sq,
					 le32_to_cpu(pkt->server.service),
					 le32_to_cpu(pkt->server.instance),
					 le32_to_cpu(pkt->server.node),
					 le32_to_cpu(pkt->server.port));
			break;
		case QRTR_TYPE_EXIT:
		case QRTR_TYPE_PING:
		case QRTR_TYPE_RESUME_TX:
			break;
		case QRTR_TYPE_NEW_LOOKUP:
			ret = ctrl_cmd_new_lookup(endpoint_id, &sq,
					 le32_to_cpu(pkt->server.service),
					 le32_to_cpu(pkt->server.instance));
			break;
		case QRTR_TYPE_DEL_LOOKUP:
			ctrl_cmd_del_lookup(endpoint_id, &sq,
				    le32_to_cpu(pkt->server.service),
				    le32_to_cpu(pkt->server.instance));
			break;
		}

		if (ret < 0)
			pr_err("failed while handling packet from %d:%d",
			       sq.sq_node, sq.sq_port);
	}

	kfree(recv_buf);
}

static void qrtr_ns_data_ready(struct sock *sk)
{
	trace_sk_data_ready(sk);

	queue_work(qrtr_ns.workqueue, &qrtr_ns.work);
}

int qrtr_ns_init(void)
{
	struct sockaddr_qrtr sq;
	int ret;

	INIT_LIST_HEAD(&qrtr_ns.lookups);
	INIT_WORK(&qrtr_ns.work, qrtr_ns_worker);

	ret = sock_create_kern(&init_net, AF_QIPCRTR, SOCK_DGRAM,
			       PF_QIPCRTR, &qrtr_ns.sock);
	if (ret < 0)
		return ret;

	qrtr_sock_set_report_endpoint(qrtr_ns.sock->sk);

	ret = kernel_getsockname(qrtr_ns.sock, (struct sockaddr *)&sq);
	if (ret < 0) {
		pr_err("failed to get socket name\n");
		goto err_sock;
	}

	qrtr_ns.workqueue = alloc_ordered_workqueue("qrtr_ns_handler", 0);
	if (!qrtr_ns.workqueue) {
		ret = -ENOMEM;
		goto err_sock;
	}

	qrtr_ns.sock->sk->sk_data_ready = qrtr_ns_data_ready;

	sq.sq_port = QRTR_PORT_CTRL;
	qrtr_ns.local_node = sq.sq_node;

	ret = kernel_bind(qrtr_ns.sock, (struct sockaddr *)&sq, sizeof(sq));
	if (ret < 0) {
		pr_err("failed to bind to socket\n");
		goto err_wq;
	}

	qrtr_ns.bcast_sq.sq_family = AF_QIPCRTR;
	qrtr_ns.bcast_sq.sq_node = QRTR_NODE_BCAST;
	qrtr_ns.bcast_sq.sq_port = QRTR_PORT_CTRL;

	ret = say_hello(qrtr_ns.local_node, &qrtr_ns.bcast_sq);
	if (ret < 0)
		goto err_wq;

	/* As the qrtr ns socket owner and creator is the same module, we have
	 * to decrease the qrtr module reference count to guarantee that it
	 * remains zero after the ns socket is created, otherwise, executing
	 * "rmmod" command is unable to make the qrtr module deleted after the
	 *  qrtr module is inserted successfully.
	 *
	 * However, the reference count is increased twice in
	 * sock_create_kern(): one is to increase the reference count of owner
	 * of qrtr socket's proto_ops struct; another is to increment the
	 * reference count of owner of qrtr proto struct. Therefore, we must
	 * decrement the module reference count twice to ensure that it keeps
	 * zero after server's listening socket is created. Of course, we
	 * must bump the module reference count twice as well before the socket
	 * is closed.
	 */
	module_put(qrtr_ns.sock->ops->owner);
	module_put(qrtr_ns.sock->sk->sk_prot_creator->owner);

	return 0;

err_wq:
	destroy_workqueue(qrtr_ns.workqueue);
err_sock:
	sock_release(qrtr_ns.sock);
	return ret;
}
EXPORT_SYMBOL_GPL(qrtr_ns_init);

void qrtr_ns_remove(void)
{
	cancel_work_sync(&qrtr_ns.work);
	destroy_workqueue(qrtr_ns.workqueue);

	/* sock_release() expects the two references that were put during
	 * qrtr_ns_init(). This function is only called during module remove,
	 * so try_stop_module() has already set the refcnt to 0. Use
	 * __module_get() instead of try_module_get() to successfully take two
	 * references.
	 */
	__module_get(qrtr_ns.sock->ops->owner);
	__module_get(qrtr_ns.sock->sk->sk_prot_creator->owner);
	sock_release(qrtr_ns.sock);
}
EXPORT_SYMBOL_GPL(qrtr_ns_remove);

MODULE_AUTHOR("Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>");
MODULE_DESCRIPTION("Qualcomm IPC Router Nameservice");
MODULE_LICENSE("Dual BSD/GPL");
