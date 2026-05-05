/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Reliable multicast over RTRS (RMR)
 *
 * Copyright (c) 2026 IONOS SE
 */

#ifndef RMR_PROTO_H
#define RMR_PROTO_H

#define RMR_PROTO_VER_MAJOR 0
#define RMR_PROTO_VER_MINOR 1

#define RMR_PROTO_VER_STRING __stringify(RMR_PROTO_VER_MAJOR) "." \
			       __stringify(RMR_PROTO_VER_MINOR)

#ifndef RMR_VER_STRING
#define RMR_VER_STRING __stringify(RMR_PROTO_VER_MAJOR) "." \
			 __stringify(RMR_PROTO_VER_MINOR)
#endif

/* TODO: should be configurable */
#define RTRS_PORT 1234

#define RMR_POOL_MAX_SESS 4

/**
 * enum rmr_msg_types - RMR message types
 * @RMR_MSG_JOIN_POOL:      Join pool message from client to server
 * @RMR_MSG_JOIN_POOL_RSP:  Join pool messge response from server to client
 * @RMR_MSG_LEAVE_POOL:     Leave pool message from client to server
 * @RMR_MSG_IO:             IO(read/write) request on an object
 */
enum rmr_msg_type {
	RMR_MSG_CMD,
	RMR_MSG_CMD_RSP,
	RMR_MSG_IO,
	RMR_MSG_MD,
	RMR_MSG_MAP_CLEAR,
	RMR_MSG_MAP_ADD,
};

/**
 * struct rmr_msg_hdr - header of RMR messages
 * @type:	Message type, valid values see: enum rmr_msg_types
 */
struct rmr_msg_hdr {
	__le32		group_id; /* poolname jhash() */
	__le16		type;
	__le16		__padding;
};

/**
 * struct rmr_msg_io - message for object I/O read/write
 * @hdr:	message header
 * @id_a:	first 64bit of the object id
 * @id_b:	second 64bit of the object id
 * @offset:	offset from where to read/write
 * @flags:	bitmask, valid values are defined in enum rmr_io_flags
 * @length:	number of bytes for I/O read/write
 * @pool_id:	pool id to which the object belongs
 */
struct rmr_msg_io {
	struct rmr_msg_hdr hdr;
	__le64		id_a;
	__le64		id_b;

	__le32		offset;
	__le32		length;
	__le32		flags;
	__le16          prio;

	__le32		mem_id;
	__le64		map_ver;
	u8		failed_id[RMR_POOL_MAX_SESS];
	u8		failed_cnt;

	u8		member_id;
	u8		sync;
	u8		__padding[19]; //padding is not correct now i think
};

struct rmr_pool_member_info {
	u8	no_of_stor;

	struct per_mem_info {
		u8	member_id;
		u8	c_dirty;
	} p_mem_info[RMR_POOL_MAX_SESS];
};

/**
 * enum rmr_msg_cmd_types - RMR command types
 * @RMR_CMD_MAP_READY: Get ready to receive map
 * @RMR_CMD_MAP_SEND:  Send map to certain node
 * @RMR_CMD_MAP_DONE:  Confirm map receipt
 *
 * When adding a command,
 * make sure to add it to the function rmr_get_cmd_name.
 */
enum rmr_msg_cmd_type {
	RMR_CMD_MAP_READY,	// 0
	RMR_CMD_MAP_SEND,
	RMR_CMD_SEND_MAP_BUF,
	RMR_CMD_MAP_BUF_DONE,
	RMR_CMD_MAP_DONE,
	RMR_CMD_MAP_DISABLE,
	RMR_CMD_READ_MAP_BUF,
	RMR_CMD_MAP_CHECK,
	RMR_CMD_LAST_IO_TO_MAP,
	RMR_CMD_STORE_CHECK,
	RMR_CMD_MAP_TEST,
	/* sends the metadata of non-sync rmr-client to server */
	RMR_CMD_SEND_MD_BUF,
	/*sends the message of discards to the node */
	RMR_CMD_SEND_DISCARD,
	/* sends the message of md_update to the node; the node sends its srv_md back. */
	RMR_CMD_MD_SEND,

	RMR_CMD_MAP_GET_VER,	// 14
	RMR_CMD_MAP_SET_VER,
	RMR_CMD_DISCARD_CLEAR_FLAG,

	/*
	 * Add map related commands above this
	 */
	RMR_MAP_CMD_MAX,

	RMR_CMD_POOL_INFO,	// 18
	RMR_CMD_JOIN_POOL,

	RMR_CMD_REJOIN_POOL,

	RMR_CMD_LEAVE_POOL,
	RMR_CMD_ENABLE_POOL,	// 22

	RMR_CMD_USER,

	/*
	 * Add pool related commands above this
	 */
	RMR_POOL_CMD_MAX,
};

struct rmr_msg_map_send_cmd {
	u8	receiver_member_id;
};

struct rmr_msg_map_buf_cmd {
	u64	version;
	u8	map_idx;
	u64	slp_idx;
};

struct rmr_msg_map_buf_done_cmd {
	u64	map_version;
};

struct rmr_msg_map_done_cmd {
	u8	enable;
};

struct rmr_msg_send_md_buf_cmd {
	u8	sync;	/* if the pool is sync or not */
	u8	sender_id;
	u8	receiver_id;
	u64	flags;
};

struct rmr_msg_send_discard_cmd {
	u8	member_id;	/* the storage node that discards all data */
};

struct rmr_msg_md_send_cmd {
	u64	src_mapped_size; /* the pool mapped size on the sending side */
	u8	sender_id;
	u8	leader_id;
	u8	read_full_md;	/* 1 = return full pool_md; 0 = own entry only */
};

struct rmr_msg_pool_info_cmd {
	u8	member_id;
	u8	operation;	/* add/remove */
	u8	mode;		/* For add -> create/assemble. For remove -> delete/disassemble */
	u8	dirty;		/* Valid only when operation=ADD and mode=CREATE */
};

enum rmr_pool_info_op {
	RMR_POOL_INFO_OP_ADD = 0,
	RMR_POOL_INFO_OP_REMOVE,
};

enum rmr_pool_info_mode {
	RMR_POOL_INFO_MODE_CREATE = 0,
	RMR_POOL_INFO_MODE_ASSEMBLE,
	RMR_POOL_INFO_MODE_DELETE,
	RMR_POOL_INFO_MODE_DISASSEMBLE,
};

struct rmr_msg_set_map_ver_cmd {
	u8	map_ver; /* the map version to set */
};

struct rmr_msg_join_pool_cmd {
	u64	queue_depth;
	u32	chunk_size;
	struct rmr_pool_member_info	mem_info;
	u8	dirty;
	u8	create;
	u8	rejoin;
};

struct rmr_msg_leave_pool_cmd {
	u8	member_id;
	u8	delete;
};

struct rmr_msg_enable_pool_cmd {
	u32	enable;
};

struct rmr_msg_user_cmd {
	size_t usr_len;
};

struct rmr_msg_join_pool_cmd_rsp {
	u64	mapped_size;
	u32	chunk_size;
};

struct rmr_msg_pool_cmd {
	struct rmr_msg_hdr	hdr;
	u8			ver;
	u8			cmd_type;
	u8			sync;
	u8			rsvd[1];
	s8			pool_name[NAME_MAX];
	union {
		struct rmr_msg_map_send_cmd	map_send_cmd;
		struct rmr_msg_map_buf_cmd	map_buf_cmd;
		struct rmr_msg_map_buf_done_cmd	map_buf_done_cmd;
		struct rmr_msg_map_done_cmd	map_done_cmd;

		struct rmr_msg_send_md_buf_cmd	send_md_buf_cmd;
		struct rmr_msg_send_discard_cmd send_discard_cmd;
		struct rmr_msg_md_send_cmd	md_send_cmd;

		struct rmr_msg_pool_info_cmd	pool_info_cmd;

		struct rmr_msg_set_map_ver_cmd  set_map_ver_cmd;

		struct rmr_msg_join_pool_cmd	join_pool_cmd;

		struct rmr_msg_leave_pool_cmd	leave_pool_cmd;
		struct rmr_msg_enable_pool_cmd	enable_pool_cmd;

		struct rmr_msg_user_cmd		user_cmd;
	};
};

struct rmr_msg_pool_cmd_rsp {
	struct rmr_msg_hdr	hdr;
	enum rmr_msg_cmd_type	cmd_type;
	u8			err;
	u8			ver;
	u8			member_id;
	union {
		struct rmr_msg_join_pool_cmd_rsp	join_pool_cmd_rsp;
		u64					value;
	};
};

#endif /* RMR_PROTO_H */
