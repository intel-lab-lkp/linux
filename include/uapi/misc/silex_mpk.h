// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 */

#ifndef __SILEX_MPK_H__
#define __SILEX_MPK_H__

#include <linux/types.h>

#define MULTIPK_IOC_MAGIC 0xBA

/** Set the number of multipk configuration **/
#define MULTIPK_CONF _IOW(MULTIPK_IOC_MAGIC, 1, int)

#define MULTIPK_IOC_MAXNR 1

#define CQ_STATUS_INVALID 0x0
#define CQ_STATUS_VALID 0x80000000
#define CQ_COMPLETION_ERROR 0x40000000

#define MAX_PK_REQS 128

struct multipk_conf {
	int eventfd[MAX_PK_REQS];
	int max_queue_depth;
};

#endif
