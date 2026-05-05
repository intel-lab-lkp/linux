/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Block device over RMR (BRMR)
 *
 * Copyright (c) 2026 IONOS SE
 */

#define BRMR_PROTO_VER_MAJOR 0
#define BRMR_PROTO_VER_MINOR 1

#define BRMR_CMD_RSP_MAGIC 0xDEADF00D

struct brmr_blk_dev_params {
	/*
	 * Params holding block device related info
	 */
	__le32 max_hw_sectors;
	__le32 max_write_zeroes_sectors;
	__le32 max_discard_sectors;
	__le32 discard_granularity;
	__le32 discard_alignment;
	__le16 physical_block_size;
	__le16 logical_block_size;
	__le16 max_segments;
	__le16 secure_discard;
	u8 cache_policy;
};

enum brmr_msg_type {
	BRMR_MSG_IO,
	BRMR_MSG_CMD,
};

struct brmr_msg_hdr {
	__le16	type;
	__le16	__padding;
};

enum brmr_msg_cmd_type {
	BRMR_CMD_MAP, // 0
	BRMR_CMD_REMAP,

	BRMR_CMD_UNMAP,
	BRMR_CMD_GET_PARAMS,

	/*
	 * Add new command types above this.
	 */
	BRMR_CMD_RSP,
};

struct brmr_msg_map_new_cmd {
	struct brmr_blk_dev_params dev_params;

	u32 version; /* version of the header itself */
	u64 mapped_size; /* size in 512 byte blocks of this device */
};

struct brmr_msg_cmd {
	struct brmr_msg_hdr	hdr;
	u8			ver;
	u8			cmd_type;
	u8			rsvd[2];
	union {
		struct brmr_msg_map_new_cmd map_new_cmd;
		/* May be other command(s) later */
	};
};

/**
 * struct brmr_cmd_get_params_rsp - response message to BRMR_CMD_GET_PARAMS
 * @hdr:			message header
 * @nsectors:			number of sectors in the usual 512b unit
 * @max_hw_sectors:		max hardware sectors in the usual 512b unit
 * @max_write_zeroes_sectors:	max sectors for WRITE ZEROES in the 512b unit
 * @max_discard_sectors:	max. sectors that can be discarded at once in 512b
 * unit.
 * @discard_granularity:	size of the internal discard allocation unit in bytes
 * @discard_alignment:		offset from internal allocation assignment in bytes
 * @physical_block_size:	physical block size device supports in bytes
 * @logical_block_size:		logical block size device supports in bytes
 * @max_segments:		max segments hardware support in one transfer
 * @secure_discard:		supports secure discard
 * @cache_policy:		support write-back caching or FUA?
 */
struct brmr_cmd_get_params_rsp {
	struct brmr_blk_dev_params dev_params;

	/*
	 * Params holding brmr device related info
	 */
	u8	mapped;
	__le64	mapped_size;
};

struct brmr_msg_cmd_rsp {
	struct brmr_msg_hdr	hdr;
	u64			magic;
	u8			ver;
	u8			cmd_type;
	u8			status;
	u8			rsvd[1];
	union {
		struct brmr_cmd_get_params_rsp get_params_rsp;
		//any other command responces.
	};
};

struct brmr_cmd_priv {
	void			*dev;
	u8			cmd_type;
	void			*rsp_buf;
	size_t			rsp_buf_len;
	int			errno;
	struct completion	complete_done;
};

enum brmr_cache_policy {
	BRMR_FUA = 1 << 0,
	BRMR_WRITEBACK = 1 << 1,
};
