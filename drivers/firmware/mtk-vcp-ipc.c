// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/*
 * Copyright (c) 2024 MediaTek Inc.
 */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/firmware/mediatek/mtk-vcp-ipc.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/sched/clock.h>
#include <linux/time64.h>
#include <linux/vmalloc.h>

/**
 * struct mtk_ipi_chan_table - channel table that belong to mtk_ipi_device
 * @mbox: the mbox channel number
 * @mbox_pin_cb: callback function
 * @holder: keep 1 if there are ipi waiters (to wait the reply)
 * @ipi_record: timestamp of each ipi transmission stage
 * @pin_buf: buffer point
 * @prdata: private data
 * @recv_opt: recv option,  0:receive ,1: response
 * @notify: completion notify process
 * @send_ofs: message offset in the slots of a mbox
 * @send_index: bit offset in the mbox
 * @msg_zie: slot size of the ipi message
 *
 * All of these data should be initialized by mtk_ipi_device_register()
 */
struct mtk_ipi_chan_table {
	u32 mbox;
	mbox_pin_cb_t mbox_pin_cb;
	atomic_t holder;
	void *pin_buf;
	void *prdata;
	u32 recv_opt;
	struct completion notify;
	/* define a mutex for remote response */
	struct mutex mutex_send;
	u32 send_ofs;
	u32 send_index;
	u32 msg_size;
};

/**
 * mbox information
 *
 * @mbdev: mbox device
 * @mbox_id: mbox id
 * @slot: how many slots that mbox used
 * @opt: option for tx mode, 0:mbox, 1:share memory 2:queue
 * @base: mbox base address
 * @mbox_client: mbox client
 * @mbox_chan: mbox channel
 */
struct mtk_mbox_info {
	struct mtk_vcp_ipc *vcp_ipc;
	u32 mbox_id;
	u32 slot;
	u32 opt;
	/* lock of mbox */
	spinlock_t mbox_lock;
	struct mbox_client cl;
	struct mbox_chan *ch;
	struct mtk_ipi_info ipi_info;
};

static const char * const mbox_names[VCP_MBOX_NUM] = {
	"mbox0", "mbox1", "mbox2", "mbox3", "mbox4"
};

/**
 * mtk_vcp_ipc_recv - recv callback used by MTK VCP mailbox
 *
 * @c: mbox client
 * @msg: message received
 *
 * Users of VCP IPC will need to provide handle_reply and handle_request
 * callbacks.
 */
static void mtk_vcp_ipc_recv(struct mbox_client *c, void *msg)
{
	struct mtk_mbox_info *minfo = container_of(c, struct mtk_mbox_info, cl);
	struct mtk_vcp_ipc *vcp_ipc = minfo->vcp_ipc;
	struct mtk_ipi_info *ipi_info = msg;
	struct mtk_ipi_device *ipidev = vcp_ipc->ipi_priv;
	struct mtk_ipi_chan_table *table;
	struct mtk_mbox_recv_table *mbox_recv;
	u32 id;

	/* execute all receive pin handler */
	for (id = 0; id < vcp_ipc->mbdev->recv_count; id++) {
		mbox_recv = &vcp_ipc->mbdev->recv_table[id];
		if (mbox_recv->mbox_id != minfo->mbox_id)
			continue;

		if (!(BIT(mbox_recv->pin_index) & ipi_info->irq_status))
			continue;

		table = &ipidev->table[mbox_recv->ipi_id];
		if (!table->pin_buf) {
			dev_err(vcp_ipc->dev, "IPI%d buf is null.\n",
				mbox_recv->ipi_id);
			continue;
		}

		memcpy(table->pin_buf,
		       ipi_info->msg + mbox_recv->offset * MBOX_SLOT_SIZE,
		       mbox_recv->msg_size * MBOX_SLOT_SIZE);

		if (!mbox_recv->recv_opt && table->mbox_pin_cb)
			table->mbox_pin_cb(mbox_recv->ipi_id,
					   table->prdata,
					   table->pin_buf,
					   mbox_recv->msg_size * MBOX_SLOT_SIZE);

		/* notify task */
		if (table->recv_opt == MBOX_RECV_MESSAGE ||
		    atomic_read(&table->holder))
			complete(&table->notify);
	}
}

/*
 * mtk_vcp_ipc_send - send ipc command to MTK VCP
 *
 * @ipidev: VCP struct mtk_ipi_device handle
 * @id: id of the feature IPI
 * @data: message address
 * @len: message length
 *
 * Return: Zero for success from mbox_send_message
 *         negative value for error
 */
int mtk_vcp_ipc_send(struct mtk_ipi_device *ipidev, u32 id, void *data, u32 len)
{
	struct device *dev;
	struct mtk_mbox_info *minfo;
	struct mtk_ipi_chan_table *table;
	struct mtk_vcp_ipc *vcp_ipc;
	int ret;

	if (!ipidev || !ipidev->ipi_inited || !data)
		return IPI_UNAVAILABLE;
	vcp_ipc = ipidev->vcp_ipc;
	if (!vcp_ipc)
		return IPI_UNAVAILABLE;

	table = ipidev->table;
	dev = ipidev->vcp_ipc->dev;
	minfo = &ipidev->vcp_ipc->info_table[table[id].mbox];
	if (!minfo) {
		dev_err(dev, "%s IPI%d minfo is invalid.\n", ipidev->name, id);
		return IPI_UNAVAILABLE;
	}

	if (len > table[id].msg_size)
		return IPI_MSG_TOO_BIG;
	else if (!len)
		len = table[id].msg_size;

	mutex_lock(&table[id].mutex_send);

	minfo->ipi_info.msg = data;
	minfo->ipi_info.len = len;
	minfo->ipi_info.id = id;
	minfo->ipi_info.index = table[id].send_index;
	minfo->ipi_info.slot_ofs = table[id].send_ofs * MBOX_SLOT_SIZE;

	ret = mbox_send_message(minfo->ch, &minfo->ipi_info);
	mutex_unlock(&table[id].mutex_send);
	if (ret < 0) {
		dev_err(dev, "%s IPI%d send failed.\n", ipidev->name, id);
		return IPI_MBOX_ERR;
	}

	return IPI_ACTION_DONE;
}
EXPORT_SYMBOL(mtk_vcp_ipc_send);

/*
 * mtk_vcp_ipc_send_compl - send ipc command to MTK VCP
 *
 * @ipidev: VCP struct mtk_ipi_device handle
 * @id: id of the feature IPI
 * @data: message address
 * @len: message length
 * @timeout_ms:
 *
 * Return: Zero for success from mbox_send_message
 *         negative value for error
 */
int mtk_vcp_ipc_send_compl(struct mtk_ipi_device *ipidev, u32 id,
			   void *data, u32 len, u32 timeout_ms)
{
	struct device *dev;
	struct mtk_mbox_info *minfo;
	struct mtk_ipi_chan_table *table;
	struct mtk_vcp_ipc *vcp_ipc;
	int ret;

	if (!ipidev || !ipidev->ipi_inited || !data)
		return IPI_UNAVAILABLE;
	vcp_ipc = ipidev->vcp_ipc;
	if (!vcp_ipc)
		return IPI_UNAVAILABLE;

	table = ipidev->table;
	dev = ipidev->vcp_ipc->dev;
	minfo = &ipidev->vcp_ipc->info_table[table[id].mbox];
	if (!minfo) {
		dev_err(dev, "%s IPI%d minfo is invalid.\n", ipidev->name, id);
		return IPI_UNAVAILABLE;
	}

	if (len > table[id].msg_size)
		return IPI_MSG_TOO_BIG;
	else if (!len)
		len = table[id].msg_size;

	mutex_lock(&table[id].mutex_send);

	minfo->ipi_info.msg = data;
	minfo->ipi_info.len = len;
	minfo->ipi_info.id = id;
	minfo->ipi_info.index = table[id].send_index;
	minfo->ipi_info.slot_ofs = table[id].send_ofs * MBOX_SLOT_SIZE;

	atomic_inc(&table[id].holder);

	ret = mbox_send_message(minfo->ch, &minfo->ipi_info);
	if (ret < 0) {
		atomic_set(&table[id].holder, 0);
		mutex_unlock(&table[id].mutex_send);
		dev_err(dev, "%s IPI%d send failed.\n", ipidev->name, id);
		return IPI_MBOX_ERR;
	}

	/* wait for completion */
	ret = wait_for_completion_timeout(&table[id].notify,
					  msecs_to_jiffies(timeout_ms));
	atomic_set(&table[id].holder, 0);
	if (ret > 0)
		ret = IPI_ACTION_DONE;

	mutex_unlock(&table[id].mutex_send);

	return ret;
}
EXPORT_SYMBOL(mtk_vcp_ipc_send_compl);

int mtk_vcp_mbox_ipc_register(struct mtk_ipi_device *ipidev, int id,
			      mbox_pin_cb_t cb, void *prdata, void *msg)
{
	if (!ipidev || !ipidev->ipi_inited)
		return IPI_DEV_ILLEGAL;
	if (!msg)
		return IPI_NO_MSGBUF;

	if (ipidev->table[id].pin_buf)
		return IPI_ALREADY_USED;
	ipidev->table[id].mbox_pin_cb = cb;
	ipidev->table[id].pin_buf = msg;
	ipidev->table[id].prdata = prdata;

	return IPI_ACTION_DONE;
}
EXPORT_SYMBOL(mtk_vcp_mbox_ipc_register);

int mtk_vcp_mbox_ipc_unregister(struct mtk_ipi_device *ipidev, int id)
{
	if (!ipidev || !ipidev->ipi_inited)
		return IPI_DEV_ILLEGAL;

	/* Drop the ipi and reset the record */
	complete(&ipidev->table[id].notify);

	ipidev->table[id].mbox_pin_cb = NULL;
	ipidev->table[id].pin_buf = NULL;
	ipidev->table[id].prdata = NULL;

	return IPI_ACTION_DONE;
}
EXPORT_SYMBOL(mtk_vcp_mbox_ipc_unregister);

static void mtk_fill_in_entry(struct mtk_ipi_chan_table *entry, const u32 ipi_id,
			      const struct mtk_mbox_table *mbdev)
{
	const struct mtk_mbox_send_table *mbox_send = mbdev->send_table;
	u32 index;

	for (index = 0; index < mbdev->send_count; index++) {
		if (ipi_id != mbox_send[index].ipi_id)
			continue;

		entry->send_ofs = mbox_send[index].offset;
		entry->send_index = mbox_send[index].pin_index;
		entry->msg_size = mbox_send[index].msg_size;
		entry->mbox = mbox_send[index].mbox_id;
		return;
	}

	entry->mbox = -ENOENT;
}

int mtk_vcp_ipc_device_register(struct mtk_ipi_device *ipidev,
				u32 ipi_chan_count, struct mtk_vcp_ipc *vcp_ipc)
{
	struct mtk_ipi_chan_table *ipi_chan_table;
	struct mtk_mbox_table *mbdev;
	u32 index;

	if (!vcp_ipc || !ipidev)
		return -EINVAL;

	ipi_chan_table = kcalloc(ipi_chan_count,
				 sizeof(struct mtk_ipi_chan_table), GFP_KERNEL);
	if (!ipi_chan_table)
		return -ENOMEM;

	mbdev = vcp_ipc->mbdev;
	vcp_ipc->ipi_priv = (void *)ipidev;
	ipidev->table = ipi_chan_table;
	ipidev->vcp_ipc = vcp_ipc;

	for (index = 0; index < ipi_chan_count; index++) {
		atomic_set(&ipi_chan_table[index].holder, 0);
		mutex_init(&ipi_chan_table[index].mutex_send);
		init_completion(&ipi_chan_table[index].notify);
		mtk_fill_in_entry(&ipi_chan_table[index], index, mbdev);
	}

	ipidev->ipi_inited = 1;

	dev_dbg(vcp_ipc->dev, "%s (with %d IPI) has registered.\n",
		ipidev->name, ipi_chan_count);

	return IPI_ACTION_DONE;
}
EXPORT_SYMBOL(mtk_vcp_ipc_device_register);

static int setup_mbox_table(struct mtk_mbox_table *mbdev, u32 mbox)
{
	struct mtk_mbox_send_table *mbox_send = &mbdev->send_table[0];
	struct mtk_mbox_recv_table *mbox_recv = &mbdev->recv_table[0];
	u32 i, last_ofs = 0, last_idx = 0, last_slot = 0, last_sz = 0;

	for (i = 0; i < mbdev->send_count; i++) {
		if (mbox == mbox_send[i].mbox_id) {
			mbox_send[i].offset = last_ofs + last_slot;
			mbox_send[i].pin_index = last_idx + last_sz;
			last_idx = mbox_send[i].pin_index;
			last_sz = DIV_ROUND_UP(mbox_send[i].msg_size, MBOX_SLOT_ALIGN);
			last_ofs = last_sz * MBOX_SLOT_ALIGN;
			last_slot = last_idx * MBOX_SLOT_ALIGN;
		} else if (mbox < mbox_send[i].mbox_id) {
			/* no need to search the rest id */
			break;
		}
	}

	for (i = 0; i < mbdev->recv_count; i++) {
		if (mbox == mbox_recv[i].mbox_id) {
			mbox_recv[i].offset = last_ofs + last_slot;
			mbox_recv[i].pin_index = last_idx + last_sz;
			last_idx = mbox_recv[i].pin_index;
			last_sz = DIV_ROUND_UP(mbox_recv[i].msg_size, MBOX_SLOT_ALIGN);
			last_ofs = last_sz * MBOX_SLOT_ALIGN;
			last_slot = last_idx * MBOX_SLOT_ALIGN;
		} else if (mbox < mbox_recv[i].mbox_id) {
			/* no need to search the rest id */
			break;
		}
	}

	if (last_idx > MBOX_MAX_PIN || (last_ofs + last_slot) > MAX_SLOT_NUM)
		return -EINVAL;

	return 0;
}

static int mtk_vcp_ipc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_vcp_ipc *vcp_ipc;
	struct mbox_client *cl;
	struct mtk_mbox_info *minfo;
	int ret;
	u32 mbox, i;
	struct mtk_mbox_table *mbox_data = dev_get_platdata(dev);

	device_set_of_node_from_dev(&pdev->dev, pdev->dev.parent);

	vcp_ipc = devm_kzalloc(dev, sizeof(*vcp_ipc), GFP_KERNEL);
	if (!vcp_ipc)
		return -ENOMEM;

	if (!mbox_data) {
		dev_err(dev, "No platform data available\n");
		return -EINVAL;
	}
	vcp_ipc->mbdev = mbox_data;

	/* alloc and init mmup_mbox_info */
	vcp_ipc->info_table = vzalloc(sizeof(*vcp_ipc->info_table) * VCP_MBOX_NUM);
	if (!vcp_ipc->info_table)
		return -ENOMEM;

	/* create mbox dev */
	for (mbox = 0; mbox < VCP_MBOX_NUM; mbox++) {
		minfo = &vcp_ipc->info_table[mbox];
		minfo->mbox_id = mbox;
		minfo->vcp_ipc = vcp_ipc;
		spin_lock_init(&minfo->mbox_lock);

		ret = setup_mbox_table(vcp_ipc->mbdev, mbox);
		if (ret)
			return ret;

		cl = &minfo->cl;
		cl->dev = &pdev->dev;
		cl->tx_block = false;
		cl->knows_txdone = false;
		cl->tx_prepare = NULL;
		cl->rx_callback = mtk_vcp_ipc_recv;
		minfo->ch = mbox_request_channel_byname(cl, mbox_names[mbox]);
		if (IS_ERR(minfo->ch)) {
			ret = PTR_ERR(minfo->ch);
			if (ret != -EPROBE_DEFER)
				dev_err(dev, "Failed to request mbox channel %s ret %d\n",
					mbox_names[mbox], ret);

			for (i = 0; i < mbox; i++) {
				minfo = &vcp_ipc->info_table[i];
				mbox_free_channel(minfo->ch);
			}

			vfree(vcp_ipc->info_table);
			return ret;
		}
	}

	vcp_ipc->dev = dev;
	dev_set_drvdata(dev, vcp_ipc);
	dev_dbg(dev, "MTK VCP IPC initialized\n");

	return 0;
}

static void mtk_vcp_ipc_remove(struct platform_device *pdev)
{
	struct mtk_vcp_ipc *vcp_ipc = dev_get_drvdata(&pdev->dev);
	struct mtk_mbox_info *minfo;
	int i;

	for (i = 0; i < VCP_MBOX_NUM; i++) {
		minfo = &vcp_ipc->info_table[i];
		mbox_free_channel(minfo->ch);
	}

	vfree(vcp_ipc->info_table);
}

static struct platform_driver mtk_vcp_ipc_driver = {
	.probe = mtk_vcp_ipc_probe,
	.remove = mtk_vcp_ipc_remove,
	.driver = {
		.name = "mtk-vcp-ipc",
	},
};
builtin_platform_driver(mtk_vcp_ipc_driver);

MODULE_AUTHOR("Jjian Zhou <jjian.zhou@mediatek.com>");
MODULE_DESCRIPTION("MediaTek VCP IPC Controller");
MODULE_LICENSE("GPL");
