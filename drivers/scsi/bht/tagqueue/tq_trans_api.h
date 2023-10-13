/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: tq_trans_api.h
 *
 * Abstract: This file is used to declare interface for TagQueue DMA mode callbacks
 *
 * Version: 1.00
 *
 * Author: Chuanjin
 *
 * Environment:	OS Independent
 *
 * History:
 *
 * 9/12/2014		Creation	Chuanjin
 */

#ifndef _TQ_TRANS_CBS_
#define _TQ_TRANS_CBS_

u32 node_list_get_cnt(list_t *p);
node_t *node_list_get_one(list_t *p);
void node_list_put_one(list_t *p, node_t *pnode);
u32 tag_queue_isr(void *pdx, cmd_err_t *err);
u32 tq_issue_post_cb(void *p);
bool req_queue_loop_ctx_ops(req_queue_t *p, req_queue_node_ops_ctx_cb cb,
			    void *ctx);

/* transfer handle */
bool adma3_end_integrated_tb(u8 *desc, bool dma_64bit);
bool link_adma2_desc(u8 *pdesc, phy_addr_t *pa, bool dma_64bit);
bool gen_sdma_like_sgl(request_t *req, dma_desc_buf_t *pdma);
dma_desc_buf_t *node_get_desc_res(node_t *node, u32 max_use_size);
u32 build_card_cmd_desc(sd_card_t *card, u8 *desc, sd_command_t *cmd);

u32 get_sdma_boudary_size(cfg_item_t *cfg);
bool dma_align(dma_desc_buf_t *pdma, u32 align_size);
/* sdma */
bool tq_sdma_mode_init(transfer_cb_t *ops);

/* adma2 */
void dump_adma2_desc(u8 *desc, u8 *desc_end);
bool dump_node_adma2_desc(node_t *node, void *ctx);

void dbg_dump_general_desc_tb(u8 *desc, u32 size);

bool tq_adma2_mode_init(transfer_cb_t *ops);
bool tq_adma2_inf_mode_init(transfer_cb_t *ops);

bool tq_adma2_inf_build_io(void *p, node_t *node);
bool tq_adma2_build_io(void *p, node_t *node);
bool tq_adma2_inf_send_command(void *p);
bool req_build_cmd(sd_card_t *card, sd_command_t *cmd, request_t *req);
bool tq_adma2_prebuild_io(void *p, node_t *node);
bool update_adma2_inf_tb(u8 *pdesc, u8 **link_addr, phy_addr_t *pa,
			 bool dma_64bit);
bool tq_adma2_inf_unload(void *p);
bool tq_adma2_sdmalike_copy(void *p, node_t *node);

/* adma3 */
dma_desc_buf_t *get_one_integrate_desc_res(req_queue_t *rq);
bool put_one_integrate_desc(req_queue_t *rq, u32 size);
bool tq_adma3_reset_integrate(req_queue_t *rq);
bool tq_adma3_mode_init(transfer_cb_t *ops);

bool tq_adma3_build_io(void *p, node_t *node);
bool tq_adma3_prebuild_io(void *p, node_t *node);
/* adma3 merge */

bool adma3_merge_io_descriptor(req_queue_t *rq, sd_card_t *card,
			       bool dma_64bit);
bool tq_judge_request_continuous(bool low_capacity_card, e_data_dir req_dir,
				 u32 req_sec_addr, u32 req_sec_cnt,
				 e_data_dir next_req_dir,
				 u32 next_req_sec_addr);

/* sdma-like */
bool tq_adma2_inf_sdmalike_mode_init(transfer_cb_t *ops);
bool tq_adma2_sdmalike_mode_init(transfer_cb_t *ops);
bool tq_adma3_sdmalike_mode_init(transfer_cb_t *ops);
/* others */

bool tag_queue_policy_break(bht_dev_ext_t *pdx);
void tq_dma_decision_policy(tag_queue_t *tq, sd_card_t *card,
			    request_t *request);
void tq_dma_decision_init(decision_mgr *mgr, e_chip_type chip_id,
			  int scope_size, int up_threshold, int low_threshold);

#endif
