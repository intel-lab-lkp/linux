/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SUN55I_MSGBOX_H_
#define _SUN55I_MSGBOX_H_

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/interrupt.h>
#include <linux/mailbox_controller.h>
#include <linux/spinlock.h>

#define SUN55I_PROC_ARM			0
#define SUN55I_PROC_DSP			1
#define SUN55I_PROC_CPUS		2
#define SUN55I_PROC_RV			3

#define SUN55I_MAX_PROCESSORS		4
#define SUN55I_CHANS_PER_PROC		4
#define SUN55I_NUM_ROUTES		(SUN55I_MAX_PROCESSORS - 1)
#define SUN55I_NUM_CHANS		(SUN55I_NUM_ROUTES * SUN55I_CHANS_PER_PROC)
#define SUN55I_FIFO_MAX			8

#define SUNXI_MSGBOX_OFFSET(n)			(0x100 * (n))
#define SUNXI_MSGBOX_READ_IRQ_ENABLE(n)		(0x020 + SUNXI_MSGBOX_OFFSET(n))
#define SUNXI_MSGBOX_READ_IRQ_STATUS(n)		(0x024 + SUNXI_MSGBOX_OFFSET(n))
#define SUNXI_MSGBOX_WRITE_IRQ_ENABLE(n)	(0x030 + SUNXI_MSGBOX_OFFSET(n))
#define SUNXI_MSGBOX_WRITE_IRQ_STATUS(n)	(0x034 + SUNXI_MSGBOX_OFFSET(n))
#define SUNXI_MSGBOX_FIFO_STATUS(n, p)		(0x050 + SUNXI_MSGBOX_OFFSET(n) + 0x4 * (p))
#define SUNXI_MSGBOX_MSG_STATUS(n, p)		(0x060 + SUNXI_MSGBOX_OFFSET(n) + 0x4 * (p))
#define SUNXI_MSGBOX_MSG_FIFO(n, p)		(0x070 + SUNXI_MSGBOX_OFFSET(n) + 0x4 * (p))

#define RD_IRQ_EN_BIT(p)			BIT((p) * 2)
#define RD_IRQ_PEND_BIT(p)			BIT((p) * 2)
#define MSG_NUM_MASK				GENMASK(3, 0)

struct sun55i_route {
	u8 remote_id;
	u8 remote_n;
};

struct sun55i_msgbox {
	struct mbox_controller controller;
	void __iomem *regs[SUN55I_MAX_PROCESSORS];
	struct clk *clk;
	struct reset_control *reset;
	int irqs[SUN55I_MAX_PROCESSORS];
	int num_irqs;
	/* Protects concurrent MMIO register access */
	spinlock_t lock;
};

extern const struct sun55i_route sun55i_msgbox_arm_routes[SUN55I_NUM_ROUTES];
extern const struct mbox_chan_ops sun55i_msgbox_chan_ops;

void sun55i_chan_to_route(int chan_idx, int *local_n, int *p,
			  int *remote_id, int *remote_n);
irqreturn_t sun55i_msgbox_irq(int irq, void *dev_id);

#endif /* _SUN55I_MSGBOX_H_ */
