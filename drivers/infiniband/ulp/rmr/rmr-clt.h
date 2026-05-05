/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Reliable multicast over RTRS (RMR)
 *
 * Copyright (c) 2026 IONOS SE
 */

#ifndef RMR_CLT_H
#define RMR_CLT_H

#include <rtrs-clt.h>
#include "rmr-pool.h"

#define RECONNECT_DELAY 30
#define MAX_RECONNECTS -1
#define RTRS_LINK_NAME "rtrs"

#define RMR_MAP_CLEAN_DELAY_MS	  5000
#define RMR_RECOVER_INTERVAL_MS	  3000

enum rmr_clt_sess_state {
	RMR_CLT_SESS_DISCONNECTED = 1,
	RMR_CLT_SESS_CONNECTED,
};

struct rmr_clt_sess {
	char		  	sessname[NAME_MAX];
	struct kobject    	kobj;
	struct mutex      	lock;
	struct rtrs_clt_sess	*rtrs;
	bool rtrs_ready;
	/* server this session is connected to */
	int		  	queue_depth;
	u32               	max_io_size;
	u32 max_segments;
	struct list_head pool_sess_list;
	struct list_head g_list;
	struct kref kref;
	enum rmr_clt_sess_state state;
};

/*
 * NB: If you change here, make sure the changes are in sync with
 *     pool_sess state machine routine i.e. pool_sess_change_state().
 */
enum rmr_clt_pool_sess_state {
	RMR_CLT_POOL_SESS_CREATED = 1, // No IO, No dirty map addition, Yes cmd msgs
	RMR_CLT_POOL_SESS_NORMAL, // Yes IO, No dirty map addition, Yes cmd msgs
	RMR_CLT_POOL_SESS_FAILED, // No IO, Yes dirty map addition, No cmd msgs
	RMR_CLT_POOL_SESS_RECONNECTING, // No IO, Yes, dirty map addition, Yes cmd msgs
					// But not with an updated map

	RMR_CLT_POOL_SESS_REMOVING // No IO, No dirty map addition, Yes cmd msgs
				   // Getting removed from pool
};

struct rmr_clt_pool_sess {
	char		sessname[NAME_MAX];
	struct		rmr_pool *pool;
	struct		kobject kobj;
	u8		member_id; /* refers to the pool id on the */
	struct		kobject sess_kobj;
	struct		list_head entry; /* for pool->sess_list */
	struct		list_head clt_sess_entry; /* for clt_sess->pool_sess_list */
	struct		rmr_clt_sess *clt_sess;
	atomic_t	state; /* rmr_clt_pool_sess_state */
	u8		ver; /* protocol version */
	u8		pool_id; /* refers to the pool id on the */
	bool		maintenance_mode; /* If the pool is in maintenance mode or not */
	bool		was_last_authoritative; /* last NORMAL sess before it went FAILED;
					       * carries complete dirty maps for all members */
};

struct rmr_clt_stats {
	struct kobject	kobj_stats;
	atomic_t read_retries;
};

/*
 * State descriptions:
 * RMR_CLT_POOL_STATE_JOINED: An rmr_clt_pool which has one or more legs (rmr_clt_pool_sess)
 *			      added to it. This means the pool has joined into pools from
 *			      storage nodes
 *
 * RMR_CLT_POOL_STATE_IN_USE: An rmr_clt_pool which is in use by an upper layer client. This
 *			      is usually done by calling rmr_clt_open
 *
 * Note: When adding a new state,
 * remember to add an entry in the function rmr_get_clt_pool_state_name()
 */
enum rmr_clt_pool_state {
	RMR_CLT_POOL_STATE_JOINED = 0,
	RMR_CLT_POOL_STATE_IN_USE,
	// RMR_CLT_POOL_STATE_DEGRADED,			uncomment and use
	// RMR_CLT_POOL_STATE_DIRTY,
	RMR_CLT_POOL_STATE_MAX,
};

struct rmr_clt_pool {
	struct rmr_pool		*pool;
	refcount_t		refcount;
	unsigned long		state;
	struct mutex		clt_pool_lock;

	size_t		     	queue_depth;

	struct rmr_clt_stats 	stats;
	struct kobject       	stats_kobj;

	void		     	*priv; /* provided by user */
	rmr_clt_ev_fn	     	*link_ev; /* deliver events to user */

	atomic_t                io_freeze;
	wait_queue_head_t       map_update_wq;
	struct mutex		io_freeze_lock;

	struct workqueue_struct	*recover_wq;
	struct delayed_work	recover_dwork;

	/* use sessions round robbin to read */
	struct rmr_clt_pool_sess __rcu *__percpu *pcpu_sess;
};

struct rmr_iu_comp {
        wait_queue_head_t wait;
        int errno;
};

/**
 * rmr_iu - reserves resources needed to do an I/O op on pool
 */
struct rmr_iu {
	struct rmr_pool		*pool;
	unsigned int		mem_id;
	struct list_head	sess_list;       /* list of per-session tags */
	u8			num_sessions;
	refcount_t		ref;             /* lifetime refcount */
	struct rmr_msg_io	msg;
	int			errno;
	atomic_t		succeeded;
	refcount_t		refcount;
	rmr_conf_fn		*conf;
	void			*priv;
	/* for retry of failed reads */
	struct work_struct	work;
	struct scatterlist	*sg;
	unsigned int		sg_cnt;
};

struct rmr_clt_sess_iu {
	void *buf; /* for session messages */
	struct rtrs_permit      *permit;
	struct rmr_clt_pool_sess *pool_sess;
	int			errno;
	union {
		/* for session messages only */
		struct scatterlist	sg;
		/* for tag->sess_list of io messages*/
		struct list_head	entry;
	};

	/* for session messages only */
	struct work_struct	work;

	/* for io requests */
	struct rmr_iu		*rmr_iu;
	unsigned int		mem_id;

	/* for command messages */
	struct rmr_clt_cmd_unit	*rmr_cmd_unit;

	/* for session messages only */
	struct rmr_iu_comp	comp;
	atomic_t		refcount;
};

struct rmr_clt_iu_comp {
	wait_queue_head_t wait;
	int errno;
};

struct rmr_clt_cmd_unit {
	struct rmr_pool		*pool;
	struct rmr_clt_pool	*clt_pool;

	struct list_head	sess_list;
	int			num_sessions;

	int			failed_state;
	int			errno;
	atomic_t		succeeded;
	refcount_t		refcount;

	rmr_conf_fn		*conf;
	void			*priv;
};

/* rmr-clt.c */
struct rmr_pool *rmr_clt_create_pool(const char *name);
void rmr_put_clt_pool(struct rmr_clt_pool *clt_pool);

void rmr_clt_change_pool_state(struct rmr_clt_pool *rmr_clt_pool,
			       enum rmr_clt_pool_state new_state, bool set);
int rmr_clt_remove_pool_from_sysfs(struct rmr_pool *pool,
				   const struct attribute *sysfs_self);
struct rmr_clt_sess *find_and_get_or_create_clt_sess(char *sessname,
						     struct rtrs_addr *paths,
						     size_t path_cnt);
struct rmr_clt_pool_sess *rmr_clt_add_pool_sess(struct rmr_pool *pool,
						struct rmr_clt_sess *clt_sess, bool create);
void rmr_clt_sess_put(struct rmr_clt_sess *sess);
void rmr_clt_del_pool_sess(struct rmr_clt_pool_sess *sess);
void rmr_clt_destroy_pool_sess(struct rmr_clt_pool_sess *sess, bool delete);

const char *rmr_clt_sess_state_str(enum rmr_clt_pool_sess_state state);
void resend_join_pool(struct rmr_clt_sess *sess);
int rmr_clt_reconnect_sess(struct rmr_clt_sess *sess,
			   const struct rtrs_addr *paths,
			   size_t path_cnt);
int rmr_clt_start_last_io_update(struct rmr_pool *pool);
int rmr_clt_set_pool_sess_mm(struct rmr_clt_pool_sess *pool_sess);
int rmr_clt_enable_sess(struct rmr_clt_pool_sess *sess);

int rmr_clt_send_map_update(struct rmr_pool *pool, struct rmr_iu *iu);

int rmr_clt_pool_send_all(struct rmr_pool *pool, struct rmr_msg_pool_cmd *msg);
int rmr_clt_send_cmd_with_data(struct rmr_pool *pool, struct rmr_clt_pool_sess *pool_sess,
			       struct rmr_msg_pool_cmd *msg,
			       void *buf, unsigned int buflen);
int rmr_clt_map_add_id(struct rmr_pool *pool, int stg_id, rmr_id_t id);
void rmr_clt_init_cmd(struct rmr_pool *pool, struct rmr_msg_pool_cmd *msg);
int rmr_clt_pool_send_cmd(struct rmr_clt_pool_sess *sess, struct rmr_msg_pool_cmd *msg, bool wait);
int rmr_clt_del_stor_from_pool(struct rmr_clt_pool_sess *pool_sess, bool delete);
bool rmr_clt_sess_is_sync(struct rmr_clt_pool_sess *sess);
int send_msg_leave_pool(struct rmr_clt_pool_sess *pool_sess, bool delete, bool wait);
void rmr_clt_free_pool_sess(struct rmr_clt_pool_sess *pool_sess);
int rmr_clt_send_map(struct rmr_pool *map_src_pool, struct rmr_pool *clt_pool,
		     const struct rmr_msg_map_send_cmd *map_send_cmd, rmr_map_filter filter);
int rmr_clt_test_map(struct rmr_pool *src_pool, struct rmr_pool *dst_pool);
int rmr_clt_send_cmd_with_data_all(struct rmr_pool *pool, struct rmr_msg_pool_cmd *msg,
				   void *buf, unsigned int buflen);
int rmr_clt_pool_send_md_all(struct rmr_pool *src_pool, struct rmr_pool *clt_pool);
int rmr_clt_pool_send_cmd_all(struct rmr_pool *pool, enum rmr_msg_cmd_type cmd_type);
void recover_work(struct work_struct *work);

int rmr_clt_pool_member_synced(struct rmr_pool *pool, u8 member_id);

bool pool_sess_change_state(struct rmr_clt_pool_sess *pool_sess,
			    enum rmr_clt_pool_sess_state newstate);

void rmr_clt_pool_io_freeze(struct rmr_clt_pool *clt_pool);
void rmr_clt_pool_io_unfreeze(struct rmr_clt_pool *clt_pool);
void rmr_clt_pool_io_wait_complete(struct rmr_clt_pool *clt_pool);
int rmr_clt_pool_try_enable(struct rmr_pool *pool);
int send_msg_enable_pool(struct rmr_clt_pool_sess *pool_sess, bool enable);

void rmr_get_iu(struct rmr_iu *iu);
void rmr_put_iu(struct rmr_iu *iu);
void rmr_msg_put_iu(struct rmr_clt_pool_sess *pool_sess,
		    struct rmr_clt_sess_iu *sess_iu);
void wake_up_iu_comp(struct rmr_clt_sess_iu *sess_iu);
void msg_conf(void *priv, int errno);

/* rmr-map-mgmt.c */
void send_map_check(struct rmr_clt_pool_sess *pool_sess);
void send_store_check(struct rmr_clt_pool_sess *pool_sess);
int send_map_get_version(struct rmr_clt_pool_sess *pool_sess, u64 *ver);
int send_discard(struct rmr_clt_pool_sess *pool_sess, u8 cmd_type, u8 member_id);
int rmr_clt_handle_map_check_rsp(struct rmr_clt_pool_sess *pool_sess,
				 struct rmr_msg_pool_cmd_rsp *rsp);
int rmr_clt_handle_store_check_rsp(struct rmr_clt_pool_sess *pool_sess,
				   struct rmr_msg_pool_cmd_rsp *rsp);
int rmr_clt_read_map(struct rmr_pool *pool);
int rmr_clt_spread_map(struct rmr_pool *pool, struct rmr_clt_pool_sess *pool_sess_chosen,
		       bool enable, bool skip_normal);
int rmr_clt_unset_pool_sess_mm(struct rmr_clt_pool_sess *pool_sess);
void sched_map_add(struct work_struct *work);
void msg_pool_cmd_map_content_conf(struct work_struct *work);

/* rmr-clt-sysfs.c */
int rmr_clt_create_sysfs_files(void);
void rmr_clt_destroy_sysfs_files(void);
void rmr_clt_destroy_pool_sysfs_files(struct rmr_pool *pool,
				      const struct attribute *sysfs_self);
int rmr_clt_create_clt_sess_sysfs_files(struct rmr_clt_sess *clt_sess);
void rmr_clt_destroy_clt_sess_sysfs_files(struct rmr_clt_sess *clt_sess);

int rmr_clt_reset_read_retries(struct rmr_clt_stats *stats, bool enable);
ssize_t rmr_clt_stats_read_retries_to_str(struct rmr_clt_stats *stats, char *page);

#endif /* RMR_CLT_H */
