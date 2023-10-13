/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: util.h
 *
 * Abstract: This Include file used to define platform independent APIs
 *
 * Version: 1.00
 *
 * Author: Peter.Guo
 *
 * Environment:	OS Independent
 *
 * History:
 *
 * 8/25/2014		Creation	Peter.Guo
 */

#ifndef _UTIL_H
#define _UTIL_H

u32 swapu32(u32 value);

bool resize_dma_buf(dma_desc_buf_t *p, u32 sz);
void dump_dma_buf(char *str, const dma_desc_buf_t *pdma);
void pa_offset_pa(phy_addr_t *pa, u32 offset);

u32 fls32(u32 val);

bool random_percent_check(u32 percent);

void util_init_waitloop(void *pdx, u32 timeout_ms, u32 per_us,
			loop_wait_t *wait);
bool util_is_timeout(loop_wait_t *wait);

#endif
