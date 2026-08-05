/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2026 NXP
 */

#ifndef SE_CTRL_H
#define SE_CTRL_H

#include <linux/bitfield.h>
#include <linux/miscdevice.h>
#include <linux/mailbox_client.h>
#include <linux/semaphore.h>
#include <linux/workqueue.h>

#define MAX_FW_LOAD_RETRIES		50
#define SE_MSG_WORD_SZ			0x4

#define RES_STATUS(x)			FIELD_GET(0x000000ff, x)
#define MAX_DATA_SIZE_PER_USER		(128 * 1024)
#define MAX_NVM_MSG_LEN			(256)
#define MESSAGING_VERSION_6		0x6
#define MESSAGING_VERSION_7		0x7

struct se_if_open_gate {
	struct miscdevice miscdev;
	struct se_if_priv *priv;
	/* to lock to update the structure */
	struct mutex lock;
	struct kref refcount;
	bool dying;
	/* set once misc_register() has succeeded (deferred to probe end) */
	bool registered;
};

struct se_clbk_handle {
	struct se_if_device_ctx *dev_ctx;
	struct completion done;
	bool signal_rcvd;
	u32 rx_msg_sz;
	/*
	 * Assignment of the rx_msg buffer to held till the
	 * received content as part callback function, is copied.
	 */
	struct se_api_msg *rx_msg;
	/*
	 * Serialise the timeout path in ele_msg_rcv() against
	 * se_if_rx_callback() so that the callback can never
	 * memcpy into a buffer that the timeout path has already
	 * freed.
	 */
	spinlock_t clbk_rx_lock;
};

struct se_imem_buf {
	u8 *buf;
	dma_addr_t daddr;
	u32 size;
	u32 state;
};

struct se_buf_desc {
	u8 *shared_buf_ptr;
	void __user *usr_buf_ptr;
	u32 size;
	struct list_head link;
};

struct se_shared_mem {
	dma_addr_t dma_addr;
	u32 size;
	u32 pos;
	u8 *ptr;
};

struct se_shared_mem_mgmt_info {
	struct list_head mem_pool_buf_list;
	struct list_head pending_in;
	struct list_head pending_out;

	struct se_shared_mem non_secure_mem;
};

/* Private struct for each char device instance. */
struct se_if_device_ctx {
	struct se_if_priv *priv;
	struct miscdevice *miscdev;
	const char *devname;
	u32 sess_hdl;
	u32 strg_hdl;
	bool cleanup_done;
	unsigned long rcv_msg_timeout_jiffies;

	/* process one file operation at a time. */
	struct mutex fops_lock;

	struct se_shared_mem_mgmt_info se_shared_mem_mgmt;
	struct list_head link;

	/* Add reference counting */
	struct kref refcount;
};

/* Header of the messages exchange with the EdgeLock Enclave */
struct se_msg_hdr {
	u8 ver;
	u8 size;
	u8 command;
	u8 tag;
}  __packed;

#define SE_MU_HDR_SZ		4
#define SE_MU_HDR_WORD_SZ	1

struct se_api_msg {
	struct se_msg_hdr header;
	u32 data[];
};

struct se_if_defines {
	const u8 se_if_type;
	u8 cmd_tag;
	u8 rsp_tag;
	u8 success_tag;
	u8 base_api_ver;
	u8 fw_api_ver;
};

struct se_fw_img_name {
	const char *prim_fw_nm_in_rfs;
	const char *seco_fw_nm_in_rfs;
};

struct se_fw_load_info {
	const struct se_fw_img_name *se_fw_img_nm;
	bool is_fw_tobe_loaded;
	bool imem_mgmt;
	struct se_imem_buf imem;
	/* to serialize the fw load state */
	struct mutex load_fw_lock;
};

struct se_if_priv {
	struct device *dev;

	struct se_clbk_handle cmd_receiver_clbk_hdl;
	/*
	 * Update to the waiting_rsp_dev, to be protected
	 * under se_if_cmd_lock.
	 */
	struct se_clbk_handle waiting_rsp_clbk_hdl;
	/*
	 * prevent new command to be sent on the se interface while previous
	 * command is still processing. (response is awaited)
	 */
	struct mutex se_if_cmd_lock;

	struct mbox_client se_mb_cl;
	struct mbox_chan *tx_chan, *rx_chan;

	struct gen_pool *mem_pool;
	const struct se_if_defines *if_defs;
	struct se_fw_load_info load_fw;

	atomic_t fw_busy;
	/*
	 * Set once teardown begins. New synchronous transactions are rejected
	 * and a teardown-forced completion is not mistaken for a real firmware
	 * response.
	 */
	atomic_t going_away;
	/*
	 * Serialise the fw_busy_dev_ctx and fw_busy state updates between the
	 * timeout path, late-response callback/work, and teardown.
	 */
	spinlock_t fw_busy_lock;
	struct se_if_device_ctx *fw_busy_dev_ctx;
	struct work_struct fw_busy_work;

	struct se_if_device_ctx *priv_dev_ctx;
	struct list_head dev_ctx_list;

	/* prevent modifying priv member variable in parallel. */
	struct mutex modify_lock;
	u32 active_devctx_count;
	u32 dev_ctx_mono_count;

	/* Add reference counting */
	struct kref refcount;

	/* stable gate used by .open() */
	struct se_if_open_gate *open_gate;
};

char *get_se_if_name(u8 se_if_id);
void unset_dev_ctx_as_command_receiver(struct se_if_device_ctx *dev_ctx);
int set_dev_ctx_as_command_receiver(struct se_if_device_ctx *dev_ctx);
bool se_is_fw_busy_ctx(struct se_if_device_ctx *dev_ctx);
void se_dev_ctx_shared_mem_cleanup(struct se_if_device_ctx *dev_ctx);
int get_shared_mem_slot(struct se_if_device_ctx *dev_ctx,
			u32 *length, dma_addr_t *ele_dma_addr, void **ptr);
int se_get_mem_pool_buf(struct se_if_device_ctx *dev_ctx, void **buf,
			dma_addr_t *daddr, u32 len);
void se_cleanup_mem_pool_buf(struct se_if_device_ctx *dev_ctx, bool reclaim);
#endif
