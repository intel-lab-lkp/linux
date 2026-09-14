// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2026 NXP
 *
 * OP-TEE ABI over the RISC-V Platform Management Interface (RPMI),
 * carried on an SBI Message Proxy (MPXY) channel.
 *
 * This is a placeholder: the transport is not implemented yet, so the
 * ABI never registers and the driver only probes through its other ABIs.
 */

#include <linux/errno.h>

#include "optee_private.h"

int optee_rpmi_abi_register(void)
{
	return -EOPNOTSUPP;
}

void optee_rpmi_abi_unregister(void)
{
}
