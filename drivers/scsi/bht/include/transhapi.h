/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: transhapi.h
 *
 * Abstract: declare related API about transmission
 *
 * Version: 1.00
 *
 * Environment:	OS Independent
 *
 */

#ifndef _TRANS_HD_API_H_
#define _TRANS_HD_API_H_

bool dma_api_io_init(bht_dev_ext_t *pdx, dma_desc_buf_t *desc_buf);

bool build_dma_ctx(void *pdx, sd_data_t *sd_data,
		   u32 cmdflag,
		   e_data_dir dir,
		   byte *data, u32 datalen, sg_list_t *sglist, u32 sg_len);

#endif
