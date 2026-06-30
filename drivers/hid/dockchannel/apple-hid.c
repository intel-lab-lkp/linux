// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple DockChannel HID transport driver
 *
 * Copyright The Asahi Linux Contributors
 */

#include <linux/bitfield.h>
#include <linux/completion.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/hid.h>
#include <linux/mailbox/apple-dockchannel.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#define APPLE_ASC_CPU_CONTROL		0x44
#define APPLE_ASC_CPU_CONTROL_RUN	BIT(4)

#define COMMAND_TIMEOUT_MS	1000
#define START_TIMEOUT_MS	2000

#define MAX_INTERFACES		16

#define DCHID_MAX_PAYLOAD	0xffff
#define DCHID_CHECKSUM_LEN	4
#define DCHID_RX_BUF_SIZE	(sizeof(struct dchid_hdr) + DCHID_MAX_PAYLOAD + \
				 DCHID_CHECKSUM_LEN)

#define DCHID_CHANNEL_CMD	0x11
#define DCHID_CHANNEL_REPORT	0x12
#define DCHID_CHECKSUM_SEED	0xffffffff

struct dchid_hdr {
	u8 hdr_len;
	u8 channel;
	__le16 length;
	u8 seq;
	u8 iface;
	__le16 pad;
} __packed;

#define IFACE_COMM	0

#define FLAGS_GROUP	GENMASK(7, 6)
#define FLAGS_REQ	GENMASK(5, 0)

#define REQ_SET_REPORT	0
#define REQ_GET_REPORT	1

struct dchid_subhdr {
	u8 flags;
	u8 unk;
	__le16 length;
	__le32 retcode;
} __packed;

#define EVENT_INIT	0xf0
#define EVENT_READY	0xf1

struct dchid_init_hdr {
	u8 type;
	u8 unk1;
	u8 unk2;
	u8 iface;
	char name[16];
	u8 more_packets;
	u8 unkpad;
} __packed;

#define INIT_HID_DESCRIPTOR	0
#define INIT_TERMINATOR		2
#define INIT_PRODUCT_NAME	7

#define CMD_RESET_INTERFACE	0x40
#define CMD_RESET_INTERFACE_SUB	1
#define CMD_ENABLE_INTERFACE	0xb4

struct dchid_init_block_hdr {
	__le16 type;
	__le16 length;
} __packed;

#define STM_REPORT_ID		0x10
#define STM_REPORT_SERIAL	0x11

struct dchid_stm_id {
	u8 unk;
	__le16 vendor_id;
	__le16 product_id;
	__le16 version_number;
	u8 unk2;
	u8 unk3;
	u8 keyboard_type;
	u8 serial_length;
	/* Serial follows, but we grab it with a different report. */
} __packed;

struct dchid_work {
	struct work_struct work;
	struct dchid_iface *iface;

	struct dchid_hdr hdr;
	u8 data[];
};

struct dchid_iface {
	struct dchid_dev *dchid;
	struct hid_device *hid;
	struct workqueue_struct *wq;

	bool creating;
	struct work_struct create_work;

	int index;
	const char *name;
	struct fwnode_handle *fwnode;

	u8 tx_seq;
	bool deferred;
	bool starting;
	bool open;
	struct completion ready;

	void *hid_desc;
	size_t hid_desc_len;

	/* Lock for command submission state below */
	spinlock_t out_lock;
	u32 out_flags;
	int out_report;
	u32 retcode;
	void *resp_buf;
	size_t resp_size;
	struct completion out_complete;
};

struct dchid_dev {
	struct device *dev;
	struct mbox_client dc_mbox_client;
	struct mbox_chan *dc_mbox;

	struct apple_rtkit *rtk;
	void __iomem *asc_base;
	void __iomem *sram_base;
	struct resource sram_res;

	bool id_ready;
	struct dchid_stm_id device_id;
	char serial[64];

	u8 *rx_buf;
	size_t rx_len;

	struct dchid_iface *comm;
	struct mutex ifaces_lock; /* protects ifaces array */
	struct dchid_iface *ifaces[MAX_INTERFACES];

	/* Workqueue to asynchronously create HID devices */
	struct workqueue_struct *new_iface_wq;
};

static void dchid_destroy_wq(void *data)
{
	struct workqueue_struct *wq = data;

	destroy_workqueue(wq);
}

static void dchid_fwnode_release(void *data)
{
	fwnode_handle_put(data);
}

static void dchid_free_mbox(void *data)
{
	mbox_free_channel(data);
}

static u32 dchid_checksum(const void *data, size_t len)
{
	const u8 *p = data;
	u32 sum = 0;
	int i;

	while (len >= sizeof(u32)) {
		sum += get_unaligned_le32(p);
		p += sizeof(u32);
		len -= sizeof(u32);
	}

	if (len) {
		u32 tmp = 0;

		for (i = 0; i < len; i++)
			tmp |= p[i] << (i * 8);
		sum += tmp;
	}

	return sum;
}

static struct dchid_iface *
dchid_get_interface(struct dchid_dev *dchid, int index, const char *name)
{
	struct dchid_iface *iface;
	struct fwnode_handle *fwnode;
	int ret;

	if (index >= MAX_INTERFACES) {
		dev_err(dchid->dev, "interface index %d out of range\n", index);
		return NULL;
	}

	mutex_lock(&dchid->ifaces_lock);
	if (dchid->ifaces[index]) {
		iface = dchid->ifaces[index];
		mutex_unlock(&dchid->ifaces_lock);
		return iface;
	}

	iface = devm_kzalloc(dchid->dev, sizeof(*iface), GFP_KERNEL);
	if (!iface) {
		mutex_unlock(&dchid->ifaces_lock);
		return NULL;
	}

	iface->index = index;
	iface->name = devm_kstrdup(dchid->dev, name, GFP_KERNEL);
	if (!iface->name) {
		mutex_unlock(&dchid->ifaces_lock);
		return NULL;
	}

	iface->dchid = dchid;
	iface->out_report = -1;
	init_completion(&iface->out_complete);
	init_completion(&iface->ready);
	spin_lock_init(&iface->out_lock);

	iface->wq = alloc_ordered_workqueue("dchid-%s", 0, iface->name);
	if (!iface->wq) {
		mutex_unlock(&dchid->ifaces_lock);
		return NULL;
	}

	ret = devm_add_action_or_reset(dchid->dev, dchid_destroy_wq, iface->wq);
	if (ret) {
		mutex_unlock(&dchid->ifaces_lock);
		return NULL;
	}

	if (!strcmp(name, "comm")) {
		/* Comm is not a HID subdevice */
		dchid->ifaces[index] = iface;
		mutex_unlock(&dchid->ifaces_lock);
		return iface;
	}

	fwnode = device_get_named_child_node(dchid->dev, name);
	if (fwnode) {
		iface->fwnode = fwnode;
		ret = devm_add_action_or_reset(dchid->dev, dchid_fwnode_release,
					       iface->fwnode);
		if (ret) {
			mutex_unlock(&dchid->ifaces_lock);
			return NULL;
		}
	} else {
		iface->fwnode = dev_fwnode(dchid->dev);
	}

	dchid->ifaces[index] = iface;
	mutex_unlock(&dchid->ifaces_lock);
	return iface;
}

static int dchid_send(struct dchid_iface *iface, u32 flags, const void *msg,
		      size_t size)
{
	struct dchid_dev *dchid = iface->dchid;
	size_t payload_padded = round_up(size, sizeof(u32));
	size_t total_len = sizeof(struct dchid_hdr) + sizeof(struct dchid_subhdr) +
			   payload_padded + DCHID_CHECKSUM_LEN;
	struct apple_dockchannel_msg dc_msg;
	struct dchid_hdr *hdr;
	struct dchid_subhdr *sub;
	u32 *checksum_ptr;
	u8 *buf;
	int ret;

	if (total_len > DCHID_RX_BUF_SIZE)
		return -EINVAL;

	buf = kzalloc(total_len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	hdr = (struct dchid_hdr *)buf;
	sub = (struct dchid_subhdr *)(buf + sizeof(*hdr));
	checksum_ptr = (u32 *)(buf + total_len - DCHID_CHECKSUM_LEN);

	hdr->hdr_len = sizeof(*hdr);
	hdr->channel = DCHID_CHANNEL_CMD;
	hdr->length = cpu_to_le16(payload_padded + sizeof(*sub));
	hdr->seq = iface->tx_seq;
	hdr->iface = iface->index;

	sub->flags = (u8)flags;
	sub->length = cpu_to_le16(size);

	memcpy(buf + sizeof(*hdr) + sizeof(*sub), msg, size);

	*checksum_ptr = 0xffffffff - dchid_checksum(buf, total_len - DCHID_CHECKSUM_LEN);

	dc_msg.data = buf;
	dc_msg.len = total_len;
	ret = mbox_send_message(dchid->dc_mbox, &dc_msg);
	kfree(buf);

	return ret < 0 ? ret : 0;
}

static int dchid_cmd(struct dchid_iface *iface, u32 type, u32 req,
		     void *data, size_t size, void *resp_buf, size_t resp_size)
{
	unsigned long flags;
	int ret;
	int report_id;
	bool timed_out = false;
	u32 out_flags;

	if (size < 1)
		return -EINVAL;

	report_id = *(u8 *)data;
	out_flags = FIELD_PREP(FLAGS_GROUP, type) | FIELD_PREP(FLAGS_REQ, req);

	spin_lock_irqsave(&iface->out_lock, flags);

	/* Only one command can be in flight per interface */
	if (WARN_ON(iface->out_report != -1)) {
		spin_unlock_irqrestore(&iface->out_lock, flags);
		return -EBUSY;
	}

	iface->out_report = report_id;
	iface->out_flags = out_flags;
	iface->retcode = 0;
	iface->resp_buf = resp_buf;
	iface->resp_size = resp_size;
	reinit_completion(&iface->out_complete);

	spin_unlock_irqrestore(&iface->out_lock, flags);

	ret = dchid_send(iface, out_flags, data, size);
	if (ret < 0) {
		spin_lock_irqsave(&iface->out_lock, flags);
		iface->out_report = -1;
		iface->resp_buf = NULL;
		iface->resp_size = 0;
		spin_unlock_irqrestore(&iface->out_lock, flags);
		return ret;
	}

	if (!wait_for_completion_timeout(&iface->out_complete,
					 msecs_to_jiffies(COMMAND_TIMEOUT_MS))) {
		dev_err(iface->dchid->dev, "command 0x%x to iface %d (%s) timed out\n",
			report_id, iface->index, iface->name);
		timed_out = true;
	}

	spin_lock_irqsave(&iface->out_lock, flags);

	if (timed_out && iface->out_report == report_id) {
		ret = -ETIMEDOUT;
	} else if (iface->retcode) {
		dev_err(iface->dchid->dev,
			"command 0x%x to iface %d (%s) failed with err 0x%x\n",
			report_id, iface->index, iface->name, iface->retcode);
		ret = -EIO;
	} else {
		ret = iface->resp_size;
	}

	iface->tx_seq++;
	iface->out_report = -1;
	iface->resp_buf = NULL;
	iface->resp_size = 0;
	spin_unlock_irqrestore(&iface->out_lock, flags);

	return ret;
}

static int dchid_comm_cmd(struct dchid_dev *dchid, void *cmd, size_t size)
{
	return dchid_cmd(dchid->comm, HID_FEATURE_REPORT, REQ_SET_REPORT,
			 cmd, size, NULL, 0);
}

static int dchid_enable_interface(struct dchid_iface *iface)
{
	u8 cmd[] = { CMD_ENABLE_INTERFACE, iface->index };

	return dchid_comm_cmd(iface->dchid, cmd, sizeof(cmd));
}

static int dchid_reset_interface(struct dchid_iface *iface, int state)
{
	u8 cmd[] = { CMD_RESET_INTERFACE, CMD_RESET_INTERFACE_SUB, iface->index,
		     (u8)state };

	return dchid_comm_cmd(iface->dchid, cmd, sizeof(cmd));
}

static int dchid_start_interface(struct dchid_iface *iface)
{
	if (iface->starting)
		return -EINPROGRESS;

	dev_dbg(iface->dchid->dev, "starting interface %s\n", iface->name);

	iface->starting = true;
	dchid_reset_interface(iface, 0);
	dchid_reset_interface(iface, 2);

	return 0;
}

static int dchid_start(struct hid_device *hdev)
{
	return 0;
}

static int dchid_open(struct hid_device *hdev)
{
	struct dchid_iface *iface = hdev->driver_data;
	int ret;

	if (!completion_done(&iface->ready)) {
		ret = dchid_start_interface(iface);
		if (ret < 0)
			return ret;

		if (!wait_for_completion_timeout(&iface->ready,
						 msecs_to_jiffies(START_TIMEOUT_MS))) {
			dev_err(iface->dchid->dev, "iface %s start timed out\n",
				iface->name);
			return -ETIMEDOUT;
		}
	}

	iface->open = true;
	return 0;
}

static void dchid_close(struct hid_device *hdev)
{
	struct dchid_iface *iface = hdev->driver_data;

	iface->open = false;
}

static int dchid_parse(struct hid_device *hdev)
{
	struct dchid_iface *iface = hdev->driver_data;

	return hid_parse_report(hdev, iface->hid_desc, iface->hid_desc_len);
}

/* Note: buf excludes report number. */
static int dchid_get_report_cmd(struct dchid_iface *iface, u8 reportnum,
				void *buf, size_t len)
{
	int ret;

	ret = dchid_cmd(iface, HID_FEATURE_REPORT, REQ_GET_REPORT, &reportnum, 1,
			buf, len);

	return ret <= 0 ? ret : ret - 1;
}

/* Note: buf includes report number. */
static int dchid_set_report(struct dchid_iface *iface, void *buf, size_t len)
{
	return dchid_cmd(iface, HID_OUTPUT_REPORT, REQ_SET_REPORT, buf, len,
			 NULL, 0);
}

static int dchid_raw_request(struct hid_device *hdev, unsigned char reportnum,
			     __u8 *buf, size_t len, unsigned char rtype,
			     int reqtype)
{
	struct dchid_iface *iface = hdev->driver_data;

	switch (reqtype) {
	case HID_REQ_GET_REPORT:
		if (len < 1)
			return -EINVAL;

		buf[0] = reportnum;
		return dchid_cmd(iface, rtype, REQ_GET_REPORT, &reportnum, 1,
				 buf + 1, len - 1);
	case HID_REQ_SET_REPORT:
		return dchid_set_report(iface, buf, len);
	default:
		return -EIO;
	}
}

static const struct hid_ll_driver dchid_ll = {
	.start = dchid_start,
	.open = dchid_open,
	.close = dchid_close,
	.parse = dchid_parse,
	.raw_request = dchid_raw_request,
};

static void dchid_create_interface_work(struct work_struct *ws)
{
	struct dchid_iface *iface = container_of(ws, struct dchid_iface, create_work);
	struct dchid_dev *dchid = iface->dchid;
	struct hid_device *hid;
	char cap_name[16];
	int ret;

	if (iface->hid) {
		dev_warn(dchid->dev, "interface %s already created\n", iface->name);
		goto done;
	}

	ret = dchid_enable_interface(iface);
	if (ret < 0) {
		dev_warn(dchid->dev, "failed to enable %s: %d\n", iface->name, ret);
		goto done;
	}

	iface->deferred = false;

	hid = hid_allocate_device();
	if (IS_ERR(hid))
		goto done;

	strscpy(cap_name, iface->name, sizeof(cap_name));
	if (cap_name[0])
		cap_name[0] = toupper(cap_name[0]);
	snprintf(hid->name, sizeof(hid->name), "Apple DockChannel %s", cap_name);

	snprintf(hid->phys, sizeof(hid->phys), "%s.%d", dev_name(dchid->dev),
		 iface->index);
	strscpy(hid->uniq, dchid->serial, sizeof(hid->uniq));

	hid->ll_driver = &dchid_ll;
	hid->bus = BUS_HOST;
	hid->vendor = le16_to_cpu(dchid->device_id.vendor_id);
	hid->product = le16_to_cpu(dchid->device_id.product_id);
	hid->version = le16_to_cpu(dchid->device_id.version_number);
	hid->type = HID_TYPE_OTHER;
	if (!strcmp(iface->name, "keyboard")) {
		u32 country_code;

		hid->group = HID_GROUP_APPLE_DOCKCHANNEL;

		/*
		 * The device provides no reliable way to get the keyboard
		 * country code, so board devicetrees provide it instead,
		 * filled by the bootloader.
		 */
		if (!fwnode_property_read_u32(iface->fwnode, "hid-country-code",
					      &country_code))
			hid->country = country_code;
	}

	hid->dev.parent = iface->dchid->dev;
	hid->driver_data = iface;
	iface->hid = hid;

	ret = hid_add_device(hid);
	if (ret < 0) {
		iface->hid = NULL;
		hid_destroy_device(hid);
		dev_warn(iface->dchid->dev, "failed to register HID device %s\n",
			 iface->name);
	}

done:
	iface->creating = false;
}

static int dchid_create_interface(struct dchid_iface *iface)
{
	if (iface->creating)
		return -EBUSY;

	iface->creating = true;
	INIT_WORK(&iface->create_work, dchid_create_interface_work);
	return queue_work(iface->dchid->new_iface_wq, &iface->create_work);
}

static void dchid_handle_descriptor(struct dchid_iface *iface, void *hid_desc,
				    size_t desc_len)
{
	u8 *rdesc;
	int i;

	if (iface->hid)
		return;

	rdesc = devm_kmemdup(iface->dchid->dev, hid_desc, desc_len,
			     GFP_KERNEL);
	if (!rdesc)
		return;

	/* Fix up oversized report sizes in DockChannel report descriptors */
	if (desc_len >= 5) {
		for (i = 0; i <= (int)desc_len - 5; i++) {
			if (rdesc[i] == 0x76 && rdesc[i + 1] == 0x00 &&
			    rdesc[i + 2] == 0x40 && rdesc[i + 3] == 0x95) {
				u8 count = rdesc[i + 4];

				if (count > 0 && count < 32) {
					dev_info(iface->dchid->dev,
						 "fixing up interface %s (%d) report size\n",
						 iface->name, iface->index);
					rdesc[i] = 0x75;
					rdesc[i + 1] = 0x08;
					rdesc[i + 2] = 0x96;
					rdesc[i + 3] = 0x00;
					rdesc[i + 4] = count * 8;
				}
			}
		}
	}

	iface->hid_desc = rdesc;
	iface->hid_desc_len = desc_len;
}

static void dchid_handle_ready(struct dchid_dev *dchid, void *data, size_t length)
{
	struct dchid_iface *iface;
	u8 *pkt = data;
	u8 index;
	int i;
	int ret;

	if (length < 2)
		return;

	index = pkt[1];
	if (index >= MAX_INTERFACES)
		return;

	iface = dchid->ifaces[index];
	if (!iface)
		return;

	dev_dbg(dchid->dev, "interface %s is now ready\n", iface->name);
	complete_all(&iface->ready);

	/* When STM is ready, grab global device info */
	if (!strcmp(iface->name, "stm")) {
		ret = dchid_get_report_cmd(iface, STM_REPORT_ID, &dchid->device_id,
					   sizeof(dchid->device_id));
		if (ret < (int)sizeof(dchid->device_id)) {
			dev_warn(iface->dchid->dev, "failed to get device ID from STM\n");
			/* Fake it and keep going. Things might still work. */
			memset(&dchid->device_id, 0, sizeof(dchid->device_id));
		}

		ret = dchid_get_report_cmd(iface, STM_REPORT_SERIAL, dchid->serial,
					   sizeof(dchid->serial) - 1);
		if (ret < 0) {
			dev_warn(iface->dchid->dev, "failed to get serial from STM\n");
			dchid->serial[0] = 0;
		}

		dchid->id_ready = true;
		for (i = 0; i < MAX_INTERFACES; i++) {
			if (!dchid->ifaces[i] || !dchid->ifaces[i]->deferred)
				continue;
			dchid_create_interface(dchid->ifaces[i]);
		}
	}
}

static void dchid_handle_init(struct dchid_dev *dchid, void *data, size_t length)
{
	struct dchid_init_hdr *hdr = data;
	struct dchid_init_block_hdr *blk;
	struct dchid_iface *iface;
	u8 *p = data;

	if (length < sizeof(*hdr))
		return;

	iface = dchid_get_interface(dchid, hdr->iface, hdr->name);
	if (!iface)
		return;

	p += sizeof(*hdr);
	length -= sizeof(*hdr);

	while (length >= sizeof(*blk)) {
		u16 blk_len;

		blk = (struct dchid_init_block_hdr *)p;
		p += sizeof(*blk);
		length -= sizeof(*blk);

		blk_len = le16_to_cpu(blk->length);
		if (blk_len > length)
			break;

		switch (le16_to_cpu(blk->type)) {
		case INIT_HID_DESCRIPTOR:
			dchid_handle_descriptor(iface, p, blk_len);
			break;
		case INIT_PRODUCT_NAME:
			if (blk_len > 0 && p[blk_len - 1] != 0)
				dev_warn(dchid->dev, "unterminated product name for %s\n",
					 iface->name);
			break;
		}

		p += blk_len;
		length -= blk_len;

		if (le16_to_cpu(blk->type) == INIT_TERMINATOR)
			break;
	}

	if (hdr->more_packets)
		return;

	/*
	 * Prefer to enable STM first, since it provides device IDs. Some
	 * firmware versions do not expose STM, so let the keyboard start
	 * without it.
	 */
	if (iface->dchid->id_ready || !strcmp(iface->name, "stm") ||
	    !strcmp(iface->name, "keyboard"))
		dchid_create_interface(iface);
	else
		iface->deferred = true;
}

static void dchid_handle_event(struct dchid_dev *dchid, void *data, size_t length)
{
	u8 *p = data;

	if (!length)
		return;

	switch (*p) {
	case EVENT_INIT:
		dchid_handle_init(dchid, data, length);
		break;
	case EVENT_READY:
		dchid_handle_ready(dchid, data, length);
		break;
	}
}

static void dchid_handle_report(struct dchid_iface *iface, void *data, size_t length)
{
	if (!iface->hid || !iface->open)
		return;

	hid_input_report(iface->hid, HID_INPUT_REPORT, data, length, 1);
}

static void dchid_packet_work(struct work_struct *ws)
{
	struct dchid_work *work = container_of(ws, struct dchid_work, work);
	struct dchid_subhdr *shdr = (void *)work->data;
	struct dchid_dev *dchid = work->iface->dchid;
	u16 hdr_len = le16_to_cpu(work->hdr.length);
	u16 sub_len;
	int type;
	u8 *payload;

	if (hdr_len < sizeof(*shdr)) {
		dev_err(dchid->dev, "bad subheader length\n");
		goto done;
	}

	sub_len = le16_to_cpu(shdr->length);
	if (sub_len > hdr_len - sizeof(*shdr)) {
		dev_err(dchid->dev, "bad subheader length\n");
		goto done;
	}

	type = FIELD_GET(FLAGS_GROUP, shdr->flags);
	payload = work->data + sizeof(*shdr);

	switch (type) {
	case HID_INPUT_REPORT:
		if (work->hdr.iface == IFACE_COMM)
			dchid_handle_event(dchid, payload, sub_len);
		else
			dchid_handle_report(work->iface, payload, sub_len);
		break;
	}

done:
	kfree(work);
}

static void dchid_handle_ack(struct dchid_iface *iface, struct dchid_hdr *hdr,
			     void *data)
{
	struct dchid_subhdr *shdr = data;
	u8 *payload = data + sizeof(*shdr);
	u16 hdr_len = le16_to_cpu(hdr->length);
	u16 sub_len = le16_to_cpu(shdr->length);
	unsigned long flags;
	bool complete_cmd = false;

	if (hdr_len < sizeof(*shdr) || sub_len > hdr_len - sizeof(*shdr) ||
	    sub_len < 1)
		return;

	spin_lock_irqsave(&iface->out_lock, flags);

	if (shdr->flags == iface->out_flags && iface->tx_seq == hdr->seq &&
	    iface->out_report == payload[0]) {
		if (iface->resp_buf && iface->resp_size)
			memcpy(iface->resp_buf, payload + 1,
			       min_t(size_t, sub_len - 1, iface->resp_size));

		iface->resp_size = sub_len;
		iface->out_report = -1;
		iface->retcode = le32_to_cpu(shdr->retcode);
		complete_cmd = true;
	}

	spin_unlock_irqrestore(&iface->out_lock, flags);

	if (complete_cmd)
		complete(&iface->out_complete);
}

static void dchid_process_packet(struct dchid_dev *dchid, struct dchid_hdr *hdr,
				 u8 *payload, size_t payload_len, u8 *packet,
				 size_t packet_len)
{
	struct dchid_work *work;

	if (dchid_checksum(packet, packet_len) != DCHID_CHECKSUM_SEED) {
		dev_err_ratelimited(dchid->dev, "checksum error\n");
		return;
	}

	if (payload_len < sizeof(struct dchid_subhdr))
		return;

	if (hdr->iface >= MAX_INTERFACES || !dchid->ifaces[hdr->iface])
		return;

	if (hdr->channel == DCHID_CHANNEL_CMD) {
		dchid_handle_ack(dchid->ifaces[hdr->iface], hdr, payload);
		return;
	}

	if (hdr->channel != DCHID_CHANNEL_REPORT)
		return;

	work = kzalloc(sizeof(*work) + payload_len, GFP_ATOMIC);
	if (!work)
		return;

	work->hdr = *hdr;
	work->iface = dchid->ifaces[hdr->iface];
	memcpy(work->data, payload, payload_len);
	INIT_WORK(&work->work, dchid_packet_work);

	queue_work(work->iface->wq, &work->work);
}

static void dchid_consume_rx(struct dchid_dev *dchid)
{
	while (dchid->rx_len >= sizeof(struct dchid_hdr)) {
		struct dchid_hdr *hdr = (struct dchid_hdr *)dchid->rx_buf;
		size_t payload_len;
		size_t packet_len;

		if (hdr->hdr_len != sizeof(*hdr)) {
			dev_err_ratelimited(dchid->dev, "bad header length %u\n",
					    hdr->hdr_len);
			dchid->rx_len = 0;
			return;
		}

		payload_len = le16_to_cpu(hdr->length);
		packet_len = sizeof(*hdr) + payload_len + DCHID_CHECKSUM_LEN;
		if (packet_len > DCHID_RX_BUF_SIZE) {
			dev_err_ratelimited(dchid->dev, "oversized packet %zu\n",
					    packet_len);
			dchid->rx_len = 0;
			return;
		}

		if (dchid->rx_len < packet_len)
			return;

		dchid_process_packet(dchid, hdr, dchid->rx_buf + sizeof(*hdr),
				     payload_len, dchid->rx_buf, packet_len);

		dchid->rx_len -= packet_len;
		memmove(dchid->rx_buf, dchid->rx_buf + packet_len, dchid->rx_len);
	}
}

static void dchid_rx_callback(struct mbox_client *cl, void *mssg)
{
	struct dchid_dev *dchid = container_of(cl, struct dchid_dev, dc_mbox_client);
	struct apple_dockchannel_msg *msg = mssg;

	if (!msg || !msg->data || !msg->len)
		return;

	if (msg->len > DCHID_RX_BUF_SIZE - dchid->rx_len) {
		dev_err_ratelimited(dchid->dev, "RX buffer overflow\n");
		dchid->rx_len = 0;
		return;
	}

	memcpy(dchid->rx_buf + dchid->rx_len, msg->data, msg->len);
	dchid->rx_len += msg->len;

	dchid_consume_rx(dchid);
}

static int dchid_rtkit_shmem_setup(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct dchid_dev *dchid = cookie;
	struct resource res = {
		.start = bfr->iova,
		.end = bfr->iova + bfr->size - 1,
		.name = "rtkit_map",
	};

	if (!bfr->iova) {
		bfr->buffer = dma_alloc_coherent(dchid->dev, bfr->size,
						 &bfr->iova, GFP_KERNEL);
		if (!bfr->buffer)
			return -ENOMEM;
		return 0;
	}

	if (!dchid->sram_res.start)
		return -EFAULT;

	res.flags = dchid->sram_res.flags;
	if (res.end < res.start || !resource_contains(&dchid->sram_res, &res))
		return -EFAULT;

	bfr->iomem = dchid->sram_base + (res.start - dchid->sram_res.start);
	bfr->is_mapped = true;

	return 0;
}

static void dchid_rtkit_shmem_destroy(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct dchid_dev *dchid = cookie;

	if (bfr->buffer)
		dma_free_coherent(dchid->dev, bfr->size, bfr->buffer, bfr->iova);
}

static const struct apple_rtkit_ops dchid_rtkit_ops = {
	.shmem_setup = dchid_rtkit_shmem_setup,
	.shmem_destroy = dchid_rtkit_shmem_destroy,
};

static int dchid_map_helper_cpu(struct platform_device *pdev, struct dchid_dev *dchid)
{
	struct resource *res;

	dchid->asc_base = devm_platform_ioremap_resource_byname(pdev, "coproc-asc");
	if (IS_ERR(dchid->asc_base))
		return PTR_ERR(dchid->asc_base);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "coproc-sram");
	if (!res)
		return -EINVAL;

	dchid->sram_res = *res;

	dchid->sram_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(dchid->sram_base))
		return PTR_ERR(dchid->sram_base);

	return 0;
}

static int dchid_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dchid_dev *dchid;
	int ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(44));
	if (ret)
		return dev_err_probe(dev, ret, "failed to set DMA mask\n");

	dchid = devm_kzalloc(dev, sizeof(*dchid), GFP_KERNEL);
	if (!dchid)
		return -ENOMEM;

	dchid->rx_buf = devm_kmalloc(dev, DCHID_RX_BUF_SIZE, GFP_KERNEL);
	if (!dchid->rx_buf)
		return -ENOMEM;

	dchid->dev = dev;
	mutex_init(&dchid->ifaces_lock);
	platform_set_drvdata(pdev, dchid);

	ret = dchid_map_helper_cpu(pdev, dchid);
	if (ret)
		return dev_err_probe(dev, ret, "failed to map helper CPU\n");

	dchid->dc_mbox_client.dev = dev;
	dchid->dc_mbox_client.tx_block = true;
	dchid->dc_mbox_client.rx_callback = dchid_rx_callback;

	dchid->dc_mbox = mbox_request_channel_byname(&dchid->dc_mbox_client,
						     "dockchannel");
	if (IS_ERR(dchid->dc_mbox))
		return dev_err_probe(dev, PTR_ERR(dchid->dc_mbox),
				     "failed to request DockChannel mailbox\n");

	ret = devm_add_action_or_reset(dev, dchid_free_mbox, dchid->dc_mbox);
	if (ret)
		return ret;

	dchid->rtk = devm_apple_rtkit_init(dev, dchid, "asc", 0, &dchid_rtkit_ops);
	if (IS_ERR(dchid->rtk))
		return dev_err_probe(dev, PTR_ERR(dchid->rtk), "failed to init RTKit\n");

	writel_relaxed(APPLE_ASC_CPU_CONTROL_RUN,
		       dchid->asc_base + APPLE_ASC_CPU_CONTROL);

	ret = apple_rtkit_wake(dchid->rtk);
	if (ret)
		return dev_err_probe(dev, ret, "failed to wake coprocessor\n");

	dchid->new_iface_wq = alloc_ordered_workqueue("dchid-new", 0);
	if (!dchid->new_iface_wq)
		return dev_err_probe(dev, -ENOMEM, "failed to allocate workqueue\n");

	ret = devm_add_action_or_reset(dev, dchid_destroy_wq, dchid->new_iface_wq);
	if (ret)
		return ret;

	dchid->comm = dchid_get_interface(dchid, IFACE_COMM, "comm");
	if (!dchid->comm)
		return dev_err_probe(dev, -EIO, "failed to init comm interface\n");

	return 0;
}

static void dchid_remove(struct platform_device *pdev)
{
	struct dchid_dev *dchid = platform_get_drvdata(pdev);
	int i;

	if (dchid->dc_mbox) {
		devm_release_action(&pdev->dev, dchid_free_mbox, dchid->dc_mbox);
		dchid->dc_mbox = NULL;
	}

	if (dchid->rtk && apple_rtkit_is_running(dchid->rtk))
		apple_rtkit_quiesce(dchid->rtk);

	if (dchid->asc_base)
		writel_relaxed(0, dchid->asc_base + APPLE_ASC_CPU_CONTROL);

	for (i = 0; i < MAX_INTERFACES; i++) {
		struct dchid_iface *iface = dchid->ifaces[i];

		if (!iface)
			continue;

		cancel_work_sync(&iface->create_work);
		flush_workqueue(iface->wq);

		if (iface->hid)
			hid_destroy_device(iface->hid);
	}

	if (dchid->new_iface_wq)
		flush_workqueue(dchid->new_iface_wq);
}

static const struct of_device_id dchid_of_match[] = {
	{ .compatible = "apple,t8122-dockchannel-hid" },
	{ .compatible = "apple,t8112-dockchannel-hid" },
	{},
};
MODULE_DEVICE_TABLE(of, dchid_of_match);

static struct platform_driver dchid_platform_driver = {
	.driver = {
		.name = "dockchannel-hid",
		.of_match_table = dchid_of_match,
	},
	.probe = dchid_probe,
	.remove = dchid_remove,
};
module_platform_driver(dchid_platform_driver);

MODULE_DESCRIPTION("Apple DockChannel HID transport driver");
MODULE_AUTHOR("Hector Martin <marcan@marcan.st>");
MODULE_AUTHOR("Michael Reeves <michael.reeves077@gmail.com>");
MODULE_LICENSE("Dual MIT/GPL");
