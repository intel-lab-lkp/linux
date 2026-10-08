/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef _RPMI_TEE_PRIVATE_H
#define _RPMI_TEE_PRIVATE_H

#include <linux/mailbox_client.h>
#include <linux/types.h>

/* TEE service group and the services used by this module. */
#define RPMI_SRVGRP_TEE		0x10

struct rpmi_tee_mbox {
	struct mbox_client client;
	struct mbox_chan *chan;
	u32 max_msg_data_size;
};

struct rpmi_tee_transport {
	struct device *dev;
	struct rpmi_tee_mbox mbox;
};

#endif /* _RPMI_TEE_PRIVATE_H */
