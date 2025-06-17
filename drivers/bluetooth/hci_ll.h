/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HCI_LL_H
#define HCI_LL_H

/*
 * BTS headers
 */
#define ACTION_SEND_COMMAND     1
#define ACTION_WAIT_EVENT       2
#define ACTION_SERIAL           3
#define ACTION_DELAY            4
#define ACTION_RUN_SCRIPT       5
#define ACTION_REMARKS          6

/**
 * struct bts_header - the fw file is NOT binary which can
 *	be sent onto TTY as is. The .bts is more a script
 *	file which has different types of actions.
 *	Each such action needs to be parsed by the KIM and
 *	relevant procedure to be called.
 */
struct bts_header {
	u32 magic;
	u32 version;
	u8 future[24];
	u8 actions[];
} __packed;

/**
 * struct bts_action - Each .bts action has its own type of
 *	data.
 */
struct bts_action {
	u16 type;
	u16 size;
	u8 data[];
} __packed;

struct bts_action_delay {
	u32 msec;
} __packed;

/**
 * struct hci_command - the HCI-VS for intrepreting
 *	the change baud rate of host-side UART, which
 *	needs to be ignored, since UIM would do that
 *	when it receives request from KIM for ldisc installation.
 */
struct hci_command {
	u8 prefix;
	u16 opcode;
	u8 plen;
	u32 speed;
} __packed;

#endif /* HCI_LL_H */
