/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/*
 * Copyright(c) 2015, 2016 Intel Corporation.
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */

struct hfi2_devdata;

int hfi2_eprom_init(struct hfi2_devdata *dd);
int hfi2_eprom_read_platform_config(struct hfi2_devdata *dd, void **buf_ret,
			       u32 *size_ret);
