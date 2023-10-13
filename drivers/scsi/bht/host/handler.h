/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: handler.h
 *
 * Abstract: Handler for interrupt and dma buffer manager apis
 *
 * Version: 1.00
 *
 * Author: Peter.Guo
 *
 * Environment:	OS Independent
 *
 * History:
 *
 * 9/5/2014		Creation	Peter.guo
 */

u32 cmd_legacy_response(void *card, void *host_request);

u32 cmd_uhs2_response(void *card, void *host_request);

u32 cmd_piobuff_ready(void *card, void *host_request);

u32 cmd_sdma_boundary(void *card, void *host_request);

u32 cmd_adma2_inf_boundary(void *card, void *host_request);

u32 cmd_adma2_sdma_like_trans_done(void *card, void *host_request);

u32 cmd_adma3_trans_done(void *card, void *host_request);

u32 cmd_sdma_trans_done(void *card, void *host_request);

bool irq_poll_cmd_done(bht_dev_ext_t *pdx, completion_t *p, s32 timeout_ms);
