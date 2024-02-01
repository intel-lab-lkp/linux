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
	struct intel_vsec_device	*ivdev;
	struct list_head		node;
	void __iomem			*control_addr;
	void __iomem			*mbox_addr;
	void __iomem			*regs_addr;
	int				control_size;
	int				maibox_size;
	int				registers_size;
	int				id;
	u32				guid;
	u32				features;
};

extern struct list_head sdsi_list;
extern struct mutex sdsi_list_lock;

extern bool sdsi_supports_attestation(struct sdsi_priv *priv);
extern int
sdsi_spdm_exchange(void *private, const void *request, size_t request_sz,
		   void *response, size_t response_sz);
extern struct sdsi_priv *sdsi_dev_get_by_id(int id);
extern int sdsi_netlink_init(void);
extern int sdsi_netlink_exit(void);
#endif
