/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: tqapi.h
 *
 * Abstract: This File is used to declare interface for TagQueue
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

#ifndef _TQ_API_H_
#define _TQ_API_H_

/* Start IO */
e_req_result tq_add_request(bht_dev_ext_t *pdx, srb_ext_t *srb_ext,
			    sd_card_t *card);
bool tq_is_empty(bht_dev_ext_t *pdx);

/*
 * This function is called req_global_init
 */
bool tag_queue_init(bht_dev_ext_t *pdx);

e_req_result tag_queue_rw_data(bht_dev_ext_t *pdx);
void tag_queue_abort(bht_dev_ext_t *pdx, e_req_result result);

#endif
