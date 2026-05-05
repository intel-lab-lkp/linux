/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Reliable multicast over RTRS (RMR)
 *
 * Copyright (c) 2026 IONOS SE
 */

#ifndef RMR_H
#define RMR_H

#include <linux/scatterlist.h>
#include <linux/kobject.h>
#include <rtrs.h>

#include "rmr-proto.h"
struct rmr_pool;

typedef void (rmr_conf_fn)(void *priv, int errno);
enum rmr_wait_type {
	NO_WAIT = RTRS_PERMIT_NOWAIT,
	WAIT = RTRS_PERMIT_WAIT
};

/*
 * Here goes RMR client API
 */

/**
 * inverse operation. decrements refcount
 * and free if it reaches 0.
 */
void rmr_clt_put_pool(struct rmr_pool *pool);

/**
 * enum rmr_clt_link_ev - Events about connectivity state of a client
 * @RMR_CLT_LINK_EV_RECONNECTED	Client was reconnected.
 * @RMR_CLT_LINK_EV_DISCONNECTED	Client was disconnected.
 */
enum rmr_clt_link_ev {
	RMR_CLT_LINK_EV_RECONNECTED,
	RMR_CLT_LINK_EV_DISCONNECTED,
};

typedef void (rmr_clt_ev_fn)(void *priv, enum rmr_clt_link_ev ev);
/**
 * rmr_clt_open() - Opens a pool from the RMR client
 * @priv:	User supplied private data.
 * @link_ev:	Event notification for connection state changes
 * @priv:	user supplied data that was passed to rmr_clt_open()
 * @ev:		Occurred event
 * @poolname:	name of the pool
 *
 * Only one user can open a pool at the same time.
 * However administrative operations are possible.
 *
 * Return a valid pointer on success otherwise PTR_ERR.
 */
struct rmr_pool *rmr_clt_open(void *priv, rmr_clt_ev_fn *link_ev, const char *poolname);

/**
   returns the priv data that had been provided with open()
*/
void *rmr_clt_get_priv(struct rmr_pool *pool);

/**
 * rmr_clt_close() - Closes a pool
 * @pool: Pool handler, is freed on return
 */
void rmr_clt_close(struct rmr_pool *pool);

#define RMR_OP_BITS 8
#define RMR_OP_MASK ((1 << RMR_OP_BITS) - 1)

/**
 * enum rmr_io_flags - RMR request types from rq_flag_bits
 * @RMR_OP_READ:		read object
 * @RMR_OP_WRITE:		write object
 * @RMR_OP_DISCARD:		remove object
 * @RMR_OP_SYNCREQ:		sync request
 * @RMR_OP_WRITE_ZEROES:	write zeroes
 * @RMR_OP_FLUSH:		flush object
 * @RMR_OP_MD_READ:		read metadata of rmr
 * @RMR_OP_MD_WRITE:		write metadata of rmr
 */
enum rmr_io_flags {
	/* Operations */
	RMR_OP_READ = 0,
	RMR_OP_WRITE = 1,
	RMR_OP_DISCARD = 2,
	RMR_OP_SYNCREQ = 3,
	RMR_OP_WRITE_ZEROES = 4,
	RMR_OP_FLUSH = 5,
	/* Add metadata related operations below this. */
	RMR_OP_MD_READ = 6,
	RMR_OP_MD_WRITE = 7,

	/* Flags */
	RMR_F_SYNC = 1 <<(RMR_OP_BITS + 0), // 0x100, 0b0100000000
	RMR_F_FUA = 1 <<(RMR_OP_BITS + 1),  // 0x200, 0b1000000000
};

static inline u32 rmr_op(u32 flag)
{
	return flag & RMR_OP_MASK;
}

static inline u32 rmr_flags(u32 flag)
{
	return flag & ~RMR_OP_MASK;
}

/**
 * Something to keep the 128 bit block_id (a.k.a object_id)
 */
typedef struct {
	u64 a;
	u64 b;
} rmr_id_t;

struct rmr_iu;

/**
 * rmr_clt_get_iu() - allocates iu for future RDMA operation
 * @pool:	Current pool
 * @id:		Id of the object/block
 * @flag:       READ/WRITE/REMOVE
 * @wait:       WAIT/NO_WAIT
 *
 * Description:
 *    Allocates iu for the following RDMA operation.  Iu is used
 *    to preallocate all resources and to propagate memory pressure
 *    up earlier.
 *
 */
struct rmr_iu *rmr_clt_get_iu(struct rmr_pool *pool,
			      enum rmr_io_flags flag,
			      enum rmr_wait_type wait);

/**
 * rmr_clt_put_iu() - puts allocated iu
 * @pool:	Current pool
 * @id:		Id of the object/block
 * @flag:       READ/WRITE/REMOVE
 * @iu:		Iu to be freed
 *
 * Context:
 *    Does not matter
 */
void rmr_clt_put_iu(struct rmr_pool *pool, struct rmr_iu *iu);

/**
 * rmr_clt_request() - Request data transfer to/from server via RDMA.
 *
 *
 * @pool:	The Pool
 * @iu:		Iu allocated by pevious rmr_clt_get_iu call.
 * @offset:	offset inside the object to read/write:
 * @length:	length of data starting from offset
 * @flag:	READ/WRITE/REMOVE
 * @prio:	priority of IO
 * @priv:	User provided data, passed back with corresponding
 *		@(conf) confirmation.
 * @conf:	callback function to be called as confirmation
 * @sg:		Pages to be sent/received to/from server.
 * @sg_cnt:	Number of elements in the @sg
 *
 * Return:
 * 0:		Success
 * -EAGAIN:	Currently there are no resources to execute the request.
 *              Retry again later.
 * <0:		Error
 *
 * On flag=READ rtrs client will request a data transfer from Server to client.
 * The data that the server will respond with will be stored in @sg when
 * the user confirmation function is called.
 * On flag=WRITE rtrs client will rdma write data in sg to server side.
 */
int rmr_clt_request(struct rmr_pool *pool, struct rmr_iu *iu,
		    size_t offset, size_t length, enum rmr_io_flags flag, unsigned short prio,
		    void *priv, rmr_conf_fn *conf, struct scatterlist *sg, unsigned int sg_cnt);

int rmr_clt_cmd_with_rsp(struct rmr_pool *pool, rmr_conf_fn *conf, void *priv,
			 const struct kvec *usr_vec, size_t nr, void *buf, int buf_len,
			 size_t size);


/**
 * rmr_attrs - RMR pool attributes
 */
struct rmr_attrs {
	u32	queue_depth;
	u32	max_io_size;
	u32	chunk_size;
	u32 	max_segments;
	u64	rmr_md_size; /* in sectors */
	u8	sync;
	struct kobject *pool_kobj;
};

/**
 * rmr_clt_query() - queries RMR pool attributes
 *
 * Returns:
 *    0 on success
 *    -EINVAL		no session in the pool
 */
int rmr_clt_query(struct rmr_pool *pool, struct rmr_attrs *attr);

typedef enum {
	RMR_MAP_ADD,
	RMR_MAP_REMOVE,
} rmr_map_cmd;

#define RMR_STORE_ID_BITS   32
#define RMR_STORE_ID_OFFSET (64 - RMR_STORE_ID_BITS)

#define RMR_CHUNK_BITS	 32
#define RMR_CHUNK_OFFSET 0

enum rmr_pool_state {
	RMR_POOL_STATE_CREATED = 0,
	RMR_POOL_STATE_JOINED,
	RMR_POOL_STATE_ONLINE,
	/* maybe we will use this later */
	RMR_POOL_STATE_DEGRADED,
	RMR_POOL_STATE_SYNCING,
};

#endif
