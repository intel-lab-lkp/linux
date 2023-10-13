/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: tq_util.h
 *
 * Abstract: This file is used to define util interface for tag queue
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

#ifndef _TQ_UTIL_CBS_
#define _TQ_UTIL_CBS_

u32 pp_ofs(byte *ph, byte *pl);

srb_ext_t *node_2_srb_ext(node_t *node);
dma_desc_buf_t *get_one_desc_res(dma_desc_buf_t *cur, u32 max_use_size);
bool put_one_desc_res(dma_desc_buf_t *cur, u32 size);

#endif
