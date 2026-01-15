/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#ifndef __CPC_H
#define __CPC_H

#include <linux/device.h>
#include <linux/greybus.h>
#include <linux/types.h>

/**
 * struct cpc_cport - CPC cport
 * @id: cport ID
 * @cpc_hd: pointer to the CPC host device this cport belongs to
 */
struct cpc_cport {
	u16 id;

	struct cpc_host_device *cpc_hd;
};

struct cpc_cport *cpc_cport_alloc(u16 cport_id, gfp_t gfp_mask);
void cpc_cport_release(struct cpc_cport *cport);

int cpc_cport_message_send(struct cpc_cport *cport, struct gb_message *message, gfp_t gfp_mask);

#endif
