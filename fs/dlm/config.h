/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __DLM_CONFIG_H__
#define __DLM_CONFIG_H__

#include <uapi/linux/dlmconstants.h>
#include <linux/socket.h>

#define CONN_HASH_SIZE 32

#define DLM_MAX_SOCKET_BUFSIZE	4096

#define DLM_MAX_ADDR_COUNT 3

#define DLM_PROTO_TCP	0
#define DLM_PROTO_SCTP	1

#define DLM_DEFAULT_WEIGHT 1
#define DLM_DEFAULT_MARK 0

extern const struct rhashtable_params dlm_rhash_rsb_params;

struct dlm_net;

struct dlm_config_node {
	int nodeid;
	int weight;
	int new;
	uint32_t comm_seq;
};

struct dlm_proto_ops {
	bool try_new_addr;
	const char *name;
	int proto;

	void (*sockopts)(struct socket *sock);
	int (*bind)(struct dlm_net *dn, struct socket *sock);
	int (*listen_validate)(const struct dlm_net *dn);
	void (*listen_sockopts)(struct socket *sock);
	int (*listen_bind)(struct dlm_net *dn, struct socket *sock);
};

struct listen_connection {
	struct socket *sock;
	struct work_struct rwork;
};

struct dlm_config_info {
	__be16 ci_tcp_port;
	unsigned int ci_buffer_size;
	unsigned int ci_recover_timer;
	unsigned int ci_toss_secs;
	unsigned int ci_log_debug;
	unsigned int ci_log_info;
	unsigned int ci_protocol;
	unsigned int ci_mark;
	unsigned int ci_recover_callbacks;
	char ci_cluster_name[DLM_LOCKSPACE_LEN];

	/* unused, still here for backwards compatibility */
	unsigned int ci_rsbtbl_size;
	unsigned int ci_new_rsb_count;
	unsigned int ci_scan_secs;
};

struct listen_sock_callbacks {
	void (*sk_error_report)(struct sock *sk);
	void (*sk_data_ready)(struct sock *sk);
	void (*sk_state_change)(struct sock *sk);
	void (*sk_write_space)(struct sock *sk);
};

struct dlm_cfg_ls {
	char name[DLM_LOCKSPACE_LEN];
	struct list_head members;
	unsigned int members_count;
	unsigned int member_idx;
	unsigned int idx;

	struct list_head list;
};

struct dlm_cfg_node {
	unsigned int id;
	uint32_t mark;
	struct sockaddr_storage addrs[DLM_MAX_ADDR_COUNT];
	unsigned int addrs_count;
	unsigned int idx;
	unsigned int seq;
	unsigned int used;

	struct list_head list;
};

struct dlm_cfg_member {
	struct dlm_cfg_node *nd;
	struct dlm_cfg_ls *ls;
	unsigned int weight;
	unsigned int idx;
	bool new;

	struct list_head list;
};

struct dlm_net {
	possible_net_t net;
	struct dlm_config_info config;

	atomic_t dlm_monitor_opened;
	int dlm_monitor_unused;

	struct listen_sock_callbacks listen_sock;
	struct listen_connection listen_con;
	struct sockaddr_storage dlm_local_addr[DLM_MAX_ADDR_COUNT];
	int dlm_local_count;

	/* Work queues */
	struct workqueue_struct *io_workqueue;
	struct workqueue_struct *process_workqueue;

	struct hlist_head connection_hash[CONN_HASH_SIZE];
	spinlock_t connections_lock;
	struct srcu_struct connections_srcu;

	const struct dlm_proto_ops *dlm_proto_ops;

	struct work_struct process_work;
	spinlock_t processqueue_lock;
	bool process_dlm_messages_pending;
	wait_queue_head_t processqueue_wq;
	atomic_t processqueue_count;
	struct list_head processqueue;

	struct hlist_head node_hash[CONN_HASH_SIZE];
	spinlock_t nodes_lock;
	struct srcu_struct nodes_srcu;

	/* This mutex prevents that midcomms_close() is running while
	 * stop() or remove(). As I experienced invalid memory access
	 * behaviours when DLM_DEBUG_FENCE_TERMINATION is enabled and
	 * resetting machines. I will end in some double deletion in nodes
	 * datastructure.
	 */
	struct mutex close_lock;

	int ls_count;
	struct mutex ls_lock;
	struct list_head lslist;
	spinlock_t lslist_lock;

	struct mutex cfg_lock;
	uint32_t dlm_cfg_node_count;
	struct dlm_cfg_node *our_node;
	unsigned int node_idx;
	struct list_head nodes;
	unsigned int ls_idx;
	struct list_head lockspaces;
};

struct dlm_net *dlm_pernet(struct net *net);
int dlm_config_init(void);
void dlm_config_exit(void);

unsigned int dlm_our_nodeid(struct dlm_net *dn);

struct dlm_cfg_node *dlm_cfg_get_node(struct dlm_net *dn, unsigned int id);
struct dlm_cfg_ls *dlm_cfg_get_ls(struct dlm_net *dn, const char *lsname);
struct dlm_cfg_member *
dlm_cfg_get_ls_member(struct dlm_net *dn, const char *lsname,
		      unsigned int nodeid);

int dlm_cfg_set_cluster_name(struct dlm_net *dn, const char *name);
int dlm_cfg_set_port(struct dlm_net *dn, __be16 port);
int dlm_cfg_set_buffer_size(struct dlm_net *dn, unsigned int size);
int dlm_cfg_set_protocol(struct dlm_net *dn, unsigned int protocol);
int dlm_cfg_set_toss_secs(struct dlm_net *dn, unsigned int secs);
int dlm_cfg_set_recover_timer(struct dlm_net *dn, unsigned int secs);
int dlm_cfg_set_mark(struct dlm_net *dn, unsigned int mark);
int dlm_cfg_set_features(struct dlm_net *dn, unsigned int features);
int dlm_cfg_set_log_debug(struct dlm_net *dn, unsigned int on);
int dlm_cfg_set_log_info(struct dlm_net *dn, unsigned int on);
int dlm_cfg_set_recover_callbacks(struct dlm_net *dn, unsigned int on);

int dlm_cfg_new_node(struct dlm_net *dn, unsigned int id, unsigned int mark,
		     struct sockaddr_storage *addrs, size_t addrs_count);
int dlm_cfg_del_node(struct dlm_net *dn, unsigned int id);
int dlm_cfg_set_our_node(struct dlm_net *dn, unsigned int id);
int dlm_cfg_set_node_mark(struct dlm_net *dn, unsigned int nodeid, unsigned int mark);
int dlm_cfg_add_member(struct dlm_net *dn, const char *lsname,
		       unsigned int id, unsigned int weight);
int dlm_cfg_del_member(struct dlm_net *dn, const char *lsname, unsigned int id);
int dlm_cfg_add_addr(struct dlm_net *dn, unsigned int id,
		     struct sockaddr_storage *addr);
int dlm_cfg_set_weight(struct dlm_net *dn, const char *lsname,
		       unsigned int nodeid, unsigned int weight);

int dlm_config_nodes(struct dlm_net *dn, char *lsname,
		     struct dlm_config_node **nodes_out,
		     unsigned int *count_out);
int dlm_comm_seq(struct dlm_net *dn, unsigned int id, uint32_t *seq);
int dlm_our_addr(struct dlm_net *dn, struct sockaddr_storage *addr, int num);

#endif /* __DLM_CONFIG_H__ */
