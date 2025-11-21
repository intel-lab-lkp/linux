/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_RENESAS_RZG2L_POEG_H__
#define __LINUX_RENESAS_RZG2L_POEG_H__

#include <linux/types.h>

#define RZG2L_POEG_OUTPUT_DISABLE_USR_DISABLE_CMD	0
#define RZG2L_POEG_OUTPUT_DISABLE_USR_ENABLE_CMD	1
#define RZG2L_POEG_GPT_CFG_IRQ_CMD			2
#define RZG2L_POEG_GPT_FAULT_CLR_CMD			3

#define RZG2L_GPT_OABHF	1
#define RZG2L_GPT_OABLF	2

struct poeg_event {
	__u32 gpt_disable_irq_status;
	__u8 channel;
};

struct poeg_cmd {
	__u32 val;
	__u8 channel;
};

#endif /* __LINUX_RENESAS_RZG2L_POEG_H__ */
