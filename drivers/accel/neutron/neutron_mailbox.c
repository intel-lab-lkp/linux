// SPDX-License-Identifier: GPL-2.0+
/* Copyright 2023, 2025-2026 NXP */

#include <linux/iopoll.h>

#include "neutron_device.h"
#include "neutron_mailbox.h"

#define NEUTRON_MBOX_FW_STATUS(dev)	NEUTRON_REG(dev, MBOX0)
#define NEUTRON_MBOX_FW_ERRCODE(dev)	NEUTRON_REG(dev, MBOX1)
#define NEUTRON_MBOX_CMD_ID(dev)	NEUTRON_REG(dev, MBOX3)
#define NEUTRON_MBOX_CMD_ARG_BASE(dev)	NEUTRON_REG(dev, MBOX4)
#define NEUTRON_MBOX_CMD_ARG(dev, i)	(NEUTRON_MBOX_CMD_ARG_BASE(dev) + (i) * 4)

int neutron_mbox_send_cmd(struct neutron_device *ndev, struct neutron_mbox_cmd *cmd)
{
	u32 status;
	int i;

	/* Make sure Neutron is ready to receive commands */
	status = readl_relaxed(NEUTRON_MBOX_FW_STATUS(ndev));
	if (status != NEUTRON_FW_STATUS_RESET)
		return -EBUSY;

	for (i = 0; i < NEUTRON_MBOX_MAX_CMD_ARGS; i++)
		writel_relaxed(cmd->args[i], NEUTRON_MBOX_CMD_ARG(ndev, i));
	writel(cmd->id, NEUTRON_MBOX_CMD_ID(ndev));

	return 0;
}

int neutron_mbox_reset_state(struct neutron_device *ndev)
{
	u32 status;

	writel_relaxed(NEUTRON_CMD_RESET_STATE, NEUTRON_MBOX_CMD_ID(ndev));

	return readl_poll_timeout(NEUTRON_MBOX_FW_STATUS(ndev), status,
				  status == NEUTRON_FW_STATUS_RESET,
				  100, 100 * USEC_PER_MSEC);
}

void neutron_mbox_read_state(struct neutron_device *ndev, struct neutron_mbox_state *state)
{
	state->status = readl_relaxed(NEUTRON_MBOX_FW_STATUS(ndev));
	state->err_code = readl_relaxed(NEUTRON_MBOX_FW_ERRCODE(ndev));
}
