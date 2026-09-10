/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 NXP */

#ifndef _LINUX_RISCV_SBI_MPXY_MBOX_H_
#define _LINUX_RISCV_SBI_MPXY_MBOX_H_

#include <linux/errno.h>

struct mbox_chan;
struct rpmi_mbox_message;

#if IS_ENABLED(CONFIG_RISCV_SBI_MPXY_MBOX)
int riscv_sbi_mpxy_mbox_call(struct mbox_chan *chan,
			     struct rpmi_mbox_message *msg);
#else
static inline int riscv_sbi_mpxy_mbox_call(struct mbox_chan *chan,
					   struct rpmi_mbox_message *msg)
{
	return -ENODEV;
}
#endif

#endif /* _LINUX_RISCV_SBI_MPXY_MBOX_H_ */
