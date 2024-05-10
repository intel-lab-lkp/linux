/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2024 NXP
 */

#ifndef SE_MU_H
#define SE_MU_H

#include <linux/miscdevice.h>
#include <linux/semaphore.h>
#include <linux/mailbox_client.h>

#define MAX_FW_LOAD_RETRIES		50

#define MSG_TAG(x)			(((x) & 0xff000000) >> 24)
#define MSG_COMMAND(x)			(((x) & 0x00ff0000) >> 16)
#define MSG_SIZE(x)			(((x) & 0x0000ff00) >> 8)
#define MSG_VER(x)			((x) & 0x000000ff)
#define RES_STATUS(x)			((x) & 0x000000ff)
#define MAX_DATA_SIZE_PER_USER		(65 * 1024)
#define S4_DEFAULT_MUAP_INDEX		(2)
#define S4_MUAP_DEFAULT_MAX_USERS	(4)
#define MESSAGING_VERSION_6		0x6
#define MESSAGING_VERSION_7		0x7

#define DEFAULT_MESSAGING_TAG_COMMAND           (0x17u)
#define DEFAULT_MESSAGING_TAG_RESPONSE          (0xe1u)

#define SE_MU_IO_FLAGS_USE_SEC_MEM	(0x02u)
#define SE_MU_IO_FLAGS_USE_SHORT_ADDR	(0x04u)

struct se_imem_buf {
	u8 *buf;
	phys_addr_t phyaddr;
	u32 size;
};

struct se_buf_desc {
	u8 *shared_buf_ptr;
	u8 *usr_buf_ptr;
	u32 size;
	struct list_head link;
};

/* Status of a char device */
enum se_if_dev_ctx_status_t {
	MU_FREE,
	MU_OPENED
};

struct se_shared_mem {
	dma_addr_t dma_addr;
	u32 size;
	u32 pos;
	u8 *ptr;
};

/* Private struct for each char device instance. */
struct se_if_device_ctx {
	struct device *dev;
	struct se_if_priv *priv;
	struct miscdevice miscdev;

	enum se_if_dev_ctx_status_t status;
	wait_queue_head_t wq;
	struct semaphore fops_lock;

	u32 pending_hdr;
	struct list_head pending_in;
	struct list_head pending_out;

	struct se_shared_mem secure_mem;
	struct se_shared_mem non_secure_mem;

	u32 *temp_resp;
	u32 temp_resp_size;
	struct notifier_block se_notify;
};

/* Header of the messages exchange with the EdgeLock Enclave */
struct se_msg_hdr {
	u8 ver;
	u8 size;
	u8 command;
	u8 tag;
}  __packed;

#define SE_MU_HDR_SZ	4
#define TAG_OFFSET	(SE_MU_HDR_SZ - 1)
#define CMD_OFFSET	(SE_MU_HDR_SZ - 2)
#define SZ_OFFSET	(SE_MU_HDR_SZ - 3)
#define VER_OFFSET	(SE_MU_HDR_SZ - 4)

struct se_api_msg {
	u32 header; /* u8 Tag; u8 Command; u8 Size; u8 Ver; */
	u32 *data;
};

struct se_if_priv {
	struct se_if_device_ctx *cmd_receiver_dev;
	struct se_if_device_ctx *waiting_rsp_dev;
	bool no_dev_ctx_used;
	/*
	 * prevent parallel access to the se interface registers
	 * e.g. a user trying to send a command while the other one is
	 * sending a response.
	 */
	struct mutex se_if_lock;
	/*
	 * prevent a command to be sent on the se interface while another one is
	 * still processing. (response to a command is allowed)
	 */
	struct mutex se_if_cmd_lock;
	struct device *dev;
	u8 *mem_pool_name;
	u8 cmd_tag;
	u8 rsp_tag;
	u8 success_tag;
	u8 base_api_ver;
	u8 fw_api_ver;
	u32 fw_fail;
	const void *info;

	struct mbox_client se_mb_cl;
	struct mbox_chan *tx_chan, *rx_chan;
	struct se_api_msg *rx_msg;
	struct completion done;
	spinlock_t lock;
	/*
	 * Flag to retain the state of initialization done at
	 * the time of se-mu probe.
	 */
	uint32_t flags;
	u8 max_dev_ctx;
	struct se_if_device_ctx **ctxs;
	struct se_imem_buf imem;
};

void *get_phy_buf_mem_pool(struct device *dev,
			   u8 *mem_pool_name,
			   dma_addr_t *buf,
			   u32 size);
phys_addr_t get_phy_buf_mem_pool1(struct device *dev,
				 u8 *mem_pool_name,
				 u32 **buf,
				 u32 size);
void free_phybuf_mem_pool(struct device *dev,
			  u8 *mem_pool_name,
			  u32 *buf,
			  u32 size);
#endif
