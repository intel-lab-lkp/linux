/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PDx86_SDSI_H_
#define __PDx86_SDSI_H_
#include <linux/mutex.h>
#include <linux/types.h>

#define SDSI_SIZE_MAILBOX		1024

/*
 * Write messages are currently up to the size of the mailbox
 * while read messages are up to 4 times the size of the
 * mailbox, sent in packets
 */
#define SDSI_SIZE_WRITE_MSG		SDSI_SIZE_MAILBOX
#define SDSI_SIZE_READ_MSG		(SDSI_SIZE_MAILBOX * 4)

struct device;

struct sdsi_priv {
	struct mutex			mb_lock;	/* Mailbox access lock */
	struct device			*dev;
	void __iomem			*control_addr;
	void __iomem			*mbox_addr;
	void __iomem			*regs_addr;
	int				control_size;
	int				maibox_size;
	int				registers_size;
	u32				guid;
	u32				features;
};
#endif
