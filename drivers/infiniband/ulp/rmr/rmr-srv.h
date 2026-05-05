/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Reliable multicast over RTRS (RMR)
 *
 * Copyright (c) 2026 IONOS SE
 */

#ifndef RMR_SRV_H
#define RMR_SRV_H

/* rmr-srv-sysfs.c */

#include <linux/types.h>
#include <linux/idr.h>
#include <linux/kref.h>
#include <linux/limits.h>
#include <linux/kthread.h>

#include "rmr-pool.h"

/*
 * IO store interface implemented by an upper-layer consumer of rmr-server.
 * All consumer-specific types are passed as void * so RMR remains
 * independent of any particular client.
 */
struct rmr_srv_store_ops {
	int (*submit_req)(void *device, void *data, u32 offset, u32 length,
			  unsigned long flags, u16 prio, void *priv);
	int (*submit_md_req)(void *device, void *data, u32 offset, u32 length,
			     unsigned long flags, void *priv);
	int (*submit_cmd)(void *device, const void *usr_buf, int usr_len,
			  void *data, int datalen);
	bool (*io_allowed)(void *store_priv);
	int (*get_params)(void *device);
};

#define DEFAULT_SYNC_QUEUE_DEPTH 32
#define RMR_SRV_CHECK_MAPS_INTERVAL_MS 3000
#define RMR_SRV_MD_SYNC_INTERVAL_MS 500
#define RMR_SRV_DISCARD_TIMEOUT_MS 500

/* Bit indices for srv_pool->md_dirty — used with set_bit / test_and_clear_bit */
enum rmr_srv_md_dirty_bit {
	MD_DIRTY_POOL,		/* pool_md fields changed */
	MD_DIRTY_MAPS,		/* map bitmap changed */
	MD_DIRTY_LAST_IO,	/* last_io updated */
};

extern struct kmem_cache *rmr_req_cachep;
extern struct kmem_cache *rmr_map_entry_cachep;

enum rmr_srv_register_disk_mode {
	RMR_SRV_DISK_CREATE,	/* Fresh store, new pool */
	RMR_SRV_DISK_ADD,	/* Rejoin an existing pool */
	RMR_SRV_DISK_REPLACE,	/* Replace an existing store */
};

/*
 * When adding state, remember to add an entry in the function rmr_get_srv_pool_state_name()
 */
enum rmr_srv_pool_state {
	RMR_SRV_POOL_STATE_EMPTY,
	RMR_SRV_POOL_STATE_REGISTERED,
	RMR_SRV_POOL_STATE_CREATED,
	RMR_SRV_POOL_STATE_NORMAL,
	RMR_SRV_POOL_STATE_NO_IO,
};

struct rmr_srv_pool {
	u8			member_id;
	refcount_t		refcount;
	atomic_t		state;
	bool			maintenance_mode;

	struct rmr_pool		*pool;

	/* Sync thread */
	struct task_struct	*th_tsk;
	atomic_t		thread_state;
	atomic_t		in_flight_sync_reqs;

	struct rmr_srv_io_store	*io_store;
	struct mutex		srv_pool_lock;
	atomic_t		store_state;

	bool			marked_create;
	bool			marked_delete;

	unsigned long           md_dirty;  /* bitmask of dirty regions */
	unsigned long           map_update_state;
	/* The internal client pool assigned to this server pool. */
	struct rmr_pool         *clt;
	size_t			queue_depth;
	rmr_id_t		*last_io;
	/*
	 *  Each storage node keeps a command array with the length of queue depth to track the IOs
	 *  in the last round. Use an array of chunk indexes as a copy of srv_pool->last_io so that
	 *  it can be written back to/read from backing store as needed.
	 */
	rmr_id_t		*last_io_idx;

	u32			max_sync_io_size;
	struct workqueue_struct *clean_wq;
	struct delayed_work	clean_dwork;

	struct workqueue_struct *md_sync_wq;
	struct delayed_work	md_sync_dwork;
	struct delayed_work	last_io_sync_dwork;
};

/**
 * rmr_srv_mark_pool_md_dirty() - Set MD_DIRTY_POOL and schedule delayed sync
 * @srv_pool:	Server pool with changed pool_md fields
 */
static inline void rmr_srv_mark_pool_md_dirty(struct rmr_srv_pool *srv_pool)
{
	set_bit(MD_DIRTY_POOL, &srv_pool->md_dirty);
	mod_delayed_work(srv_pool->md_sync_wq, &srv_pool->md_sync_dwork,
			 msecs_to_jiffies(RMR_SRV_MD_SYNC_INTERVAL_MS));
}

struct rmr_srv_sess {
	struct list_head pool_sess_list;
	struct rtrs_srv_sess *rtrs;
	struct kobject		kobj;
	char			sessname[NAME_MAX];
	struct mutex		lock;
	u8			ver;
	struct xarray		pools;
	struct list_head g_list_entry;
};

struct rmr_srv_pool_sess {
	struct list_head pool_entry; /* for pool->sess_list */
	struct list_head srv_sess_entry;
	struct rmr_srv_pool *srv_pool;
	struct kobject kobj;
	char sessname[NAME_MAX];
	struct rmr_srv_sess *srv_sess;
	bool sync;
};

struct rmr_srv_io_store {
	struct rmr_srv_store_ops *ops;
	void *priv;
};

struct rmr_cmd_work_info {
	struct work_struct		cmd_work;
	struct rmr_pool			*pool;
	struct rmr_srv_sess *sess;
	struct rtrs_srv_sess		*rtrs;
	const struct rmr_msg_pool_cmd	*cmd_msg;
	struct rmr_msg_pool_cmd_rsp	*rsp;
	struct rtrs_srv_op		*rtrs_op;
	void				*data;
	size_t				datalen;
};

void rmr_put_srv_pool(struct rmr_srv_pool *srv_pool);
struct rmr_srv_pool *rmr_create_srv_pool(char *poolname, u32 member_id);
void rmr_srv_pool_update_params(struct rmr_pool *pool);
int rmr_srv_read_md(struct rmr_pool *pool, struct rtrs_srv_op *rtrs_op, u32 offset, u32 len,
		    struct rmr_pool_md *pool_md_page);
int rmr_srv_send_md_update(struct rmr_pool *pool);
int rmr_srv_check_params(struct rmr_srv_pool *srv_pool);
void rmr_srv_mark_maps_dirty(struct rmr_srv_pool *srv_pool);

/* rmr-srv-md.c */
struct rmr_srv_req;	/* forward decl for endreq prototype */

bool rmr_get_srv_pool(struct rmr_srv_pool *srv_pool);
void rmr_srv_endreq(struct rmr_srv_req *req, int err);

int process_md_io(struct rmr_pool *pool, struct rtrs_srv_op *rtrs_op,
		  u32 offset, u32 len, unsigned long flags, void *buf);
void rmr_srv_md_maps_sync(struct rmr_pool *pool);
void rmr_srv_flush_pool_md(struct rmr_srv_pool *srv_pool);
void rmr_srv_md_sync(struct work_struct *work);
int rmr_srv_md_process_buf(struct rmr_pool *pool, void *buf, bool sync);
int rmr_srv_refresh_md(struct rmr_srv_pool *srv_pool);

/* rmr-srv-sysfs.c */

int rmr_srv_create_sysfs_files(void);
void rmr_srv_destroy_sysfs_files(void);
void rmr_srv_destroy_pool_sysfs_files(struct rmr_pool *pool,
				      const struct attribute *sysfs_self);
int rmr_srv_sysfs_add_sess(struct rmr_pool *pool,
			   struct rmr_srv_pool_sess *pool_sess);
void rmr_srv_sysfs_del_sess(struct rmr_srv_pool_sess *pool_sess);

void rmr_srv_free_sync_permits(struct rmr_pool *pool);
void rmr_srv_destroy_pool(struct rmr_pool *pool);
int rmr_srv_remove_clt_pool(struct rmr_srv_pool *srv_pool);

void rmr_srv_stop_sync_and_go_offline(struct rmr_pool *pool);

int rmr_srv_get_sync_permit(struct rmr_srv_pool *srv_pool);
void rmr_srv_put_sync_permit(struct rmr_srv_pool *srv_pool);

int rmr_srv_sync_thread_start(struct rmr_srv_pool *srv_pool);
int rmr_srv_sync_thread_stop(struct rmr_srv_pool *srv_pool);

void rmr_srv_sync_req_failed(struct rmr_srv_pool *srv_pool);

int rmr_srv_query(struct rmr_pool *pool, u64 mapped_size, struct rmr_attrs *attr);
/* register/unregister rmr-srv */
struct rmr_pool *rmr_srv_register(char *poolname, struct rmr_srv_store_ops *ops, void *priv,
				  u64 mapped_size, enum rmr_srv_register_disk_mode mode);
void rmr_srv_unregister(char *poolname, bool delete);

int rmr_srv_pool_cmd_with_rsp(struct rmr_pool *pool, rmr_conf_fn *conf, void *priv,
			     const struct kvec *usr_vec, size_t nr, void *buf, int buf_len,
			     size_t size);
int rmr_srv_discard_id(struct rmr_pool *pool, u64 offset, u64 length, u8 member_id, bool sync);
void rmr_srv_replace_store(struct rmr_pool *pool);

#endif /* RMR_SRV_H */
