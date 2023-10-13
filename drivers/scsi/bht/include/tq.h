/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: tq.h
 *
 * Abstract: define tag queue related struct
 *
 * Version: 1.00
 *
 * Environment:	OS Independent
 *
 */

#ifndef _TQ_HEADER_
#define _TQ_HEADER_

#include "transh.h"

/* reserved one node  for internal use(currently for adma2 infinite link issue need one). */
#define TQ_RESERVED_NODE_SIZE  1

/* TagQueueStateType; */
typedef enum {
	QUEUE_STATE_IDLE = 0,
	QUEUE_STATE_BUILD,
	QUEUE_STATE_READY,
	QUEUE_STATE_WAIT_CPL,
} e_tq_state;

typedef enum {
	TQ_BUILD_IO_OK = 0,
	TQ_BUILD_IO_ERROR,
	TQ_BUILD_IO_EMPTY,
} e_build_ctx_result;

typedef enum {
	QUEUE_EVT_GET = 0,
	QUEUE_EVT_BUILD_OK,
	QUEUE_EVT_BUILD_FAILED,
	QUEUE_EVT_ISSUE,
	QUEUE_EVT_DONE,
	QUEUE_EVT_FORCE_FAILED,
	QUEUE_EVT_ABORT,
	QUEUE_EVT_INIT,
} e_tq_state_evt;

typedef struct {
	e_data_dir data_dir;
	u32 sec_addr;
	u32 sec_cnt;
} req_card_access_info_t;

typedef struct {
	/* request list */
	list_t list;
	/* id name */
	u32 id;
	/* below is work queue member */
	e_tq_state state;
	/* adma2 */
	req_card_access_info_t adma2_last_req;
	dma_desc_buf_t adma3_integrate_tbl;
	dma_desc_buf_t adma3_integrate_tbl_cur;
	/* data mgr */
	sd_data_t sd_data;
	/* private ctx */
	void *priv;
} req_queue_t;

typedef bool (*req_queue_node_ops_ctx_cb)(node_t *, void *ctx);
typedef bool (*storage_cmd_cb)(void *pdx, node_t *node);
typedef bool (*storage_cb)(void *pdx);

typedef struct {
	/* Init transfer info and policy */
	storage_cb init_io;
	/* build adma2 desc table only */
	storage_cmd_cb prebuild_io;
	/* build adma2 desc table only */
	storage_cmd_cb build_io;
	/* merage adma 2 table */
	storage_cb merge_io;
	/* build cmd reg, amda3 cmd table and adma3 integate table */
	storage_cb issue_transfer;
	storage_cb unload;
	storage_cb poweroff_need_rebuild;
} transfer_cb_t;

#define TAG_QUEUE_INIT_MAGIC_NUMBER 0x55AA3344

#define  MAX_DECISION_SCOPE_SIZE 8

typedef void (*host_dma_mode_selector_cb)(void *tq, bool flg);

typedef struct {
	bool slot[MAX_DECISION_SCOPE_SIZE];
	int scope;
	int idx;
	/* output */
	bool out;
	bool up_flg;
	int up_thd;
	int low_thd;
	req_card_access_info_t last_req;

	host_dma_mode_selector_cb dma_selector_cb;
} decision_mgr;

typedef struct {
	/* node memory pool */
	/* node memory resources management (for below node array) */
	list_t node_pool_list;
	/* internal use */
	node_t node[MAX_WORK_QUEUE_SIZE];

	/* IO transfer strategy controller */
	u32 max_wq_req_size;
	u32 max_build_limit;
	/* pointer to cmd_req.done */
	completion_t *tran_cpl;
	/* IO transfer strategy controller end */

	atomic_t req_cnt;
	/* SRBs Request Queue */
	req_queue_t req_queue;

	/* work queue start */
	/* pointer to current queue */
	req_queue_t *wq_cur;
	/* build queue */
	req_queue_t *wq_build;
	req_queue_t work_queues[TQ_WORK_QUEUE_SIZE];
	/* work queue end */

	u8 *adma2_inf_link_addr;
	host_cmd_req_t cmd_req;
	/* tq fake cmd, only use for fake cmd */
	sd_command_t priv_fake_cmd;

	/* for anti-uninit use */
	u32 init_magic_number;
	bool hw_idle;
	/* init 0 */
	u32 queue_id_seed;

	/* transfer ops */
	transfer_cb_t ops;
	/* dma mode */
	u32 cur_dma_mode;
	atomic_t target_dma_mode;
	u32 cfg_dma_mode;
	decision_mgr decision;

} tag_queue_t;

#define TQ_MAX_RECOVERY_ERROR_RETRY_TIMES 2

#endif
