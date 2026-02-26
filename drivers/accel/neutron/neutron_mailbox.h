/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright 2023, 2025-2026 NXP */

#ifndef __NEUTRON_MAILBOX_H__
#define __NEUTRON_MAILBOX_H__

#include <linux/types.h>

struct neutron_device;

/* Device (firmware) status magic values */
enum neutron_mbox_fwstat {
	NEUTRON_FW_STATUS_RESET		= 0,
	NEUTRON_FW_STATUS_ACK		= 0xA3,
	NEUTRON_FW_STATUS_DONE		= 0xAD0,
};

/* Firmware command opcodes */
enum neutron_mbox_cmdid {
	NEUTRON_CMD_INFERENCE		= 0x269,
	NEUTRON_CMD_RESET_STATE		= 0x23637,
};

#define NEUTRON_MBOX_MAX_CMD_ARGS	4

/* Firmware command */
struct neutron_mbox_cmd {
	enum neutron_mbox_cmdid id;
	u32 args[NEUTRON_MBOX_MAX_CMD_ARGS];
};

/* Device state */
struct neutron_mbox_state {
	enum neutron_mbox_fwstat status;
	u32 err_code;
};

int neutron_mbox_send_cmd(struct neutron_device *ndev, struct neutron_mbox_cmd *cmd);
void neutron_mbox_read_state(struct neutron_device *ndev, struct neutron_mbox_state *state);
int neutron_mbox_reset_state(struct neutron_device *ndev);

#endif /* __NEUTRON_MAILBOX_H__ */
